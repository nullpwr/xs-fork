#define _POSIX_C_SOURCE 200809L
#include "core/gc_concurrent.h"
#include "core/value.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ----------------------------------------------------------------
 *  Free queue: lock-protected linked list of GCNodes pending free.
 *  Each entry retains the GCNode pointer; the worker walks the
 *  underlying Value's children and frees both. The mutator and the
 *  worker share the queue under gc_q_mu; the queue's growth is
 *  unbounded by design (the mutator can outrun the worker briefly,
 *  the worker catches up between collections).
 * ---------------------------------------------------------------- */

typedef struct FreeJob {
    GCNode *node;
    struct FreeJob *next;
} FreeJob;

static FreeJob *q_head = NULL;
static FreeJob *q_tail = NULL;
static int      q_depth = 0;

static pthread_mutex_t q_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  q_drain_cv = PTHREAD_COND_INITIALIZER;

static pthread_t        worker_thread;
static atomic_int       worker_running = 0;
static atomic_int       worker_should_stop = 0;
static atomic_int       enabled = 0;

static atomic_long      stat_queued = 0;
static atomic_long      stat_freed  = 0;

/* Wall-time accounting for the worker's idle ratio. */
static double worker_active_ms = 0;
static double worker_idle_ms   = 0;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static double now_ms_local(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* free a single garbage GCNode + its Value payload. Mirrors the
 * inline free in gc_collect_gen phase 7 but operates on a fully
 * detached node (already off gen list + node map). */
static void free_one(GCNode *node) {
    if (!node) return;
    Value *v = node->value;
    if (v) {
        switch (VAL_TAG(v)) {
            case XS_ARRAY:
            case XS_TUPLE:
                if (v->arr) {
                    /* children already had refs nulled out by phase 6 of
                     * gc_collect_gen, so this is just the container free */
                    free(v->arr->items);
                    free(v->arr);
                }
                break;
            case XS_MAP:
            case XS_MODULE:
                if (v->map) {
                    free(v->map->keys);
                    free(v->map->vals);
                    free(v->map);
                }
                break;
            case XS_OVERLOAD:
                if (v->overload) {
                    free(v->overload->items);
                    free(v->overload);
                }
                break;
            case XS_FUNC:
                if (v->fn) {
                    v->fn->refcount--;
                    if (v->fn->refcount <= 0) {
                        free(v->fn->name);
                        free(v->fn->deprecated_msg);
                        free(v->fn->params);
                        free(v->fn->default_vals);
                        free(v->fn->variadic_flags);
                        if (v->fn->param_type_names) {
                            for (int j = 0; j < v->fn->nparams; j++)
                                free(v->fn->param_type_names[j]);
                            free(v->fn->param_type_names);
                        }
                        free(v->fn->ret_type_name);
                        free(v->fn->param_contracts);
                        free(v->fn);
                    }
                }
                break;
            case XS_STRUCT_VAL:
                if (v->st) {
                    v->st->refcount--;
                    if (v->st->refcount <= 0) {
                        free(v->st->type_name);
                        if (v->st->fields) {
                            free(v->st->fields->keys);
                            free(v->st->fields->vals);
                            free(v->st->fields);
                        }
                        free(v->st);
                    }
                }
                break;
            default:
                break;
        }
        free(v);
    }
    free(node);
    atomic_fetch_add(&stat_freed, 1);
}

static void *worker_main(void *unused) {
    (void)unused;
    while (!atomic_load(&worker_should_stop)) {
        pthread_mutex_lock(&q_mu);
        double t_idle_start = now_ms_local();
        while (!q_head && !atomic_load(&worker_should_stop)) {
            pthread_cond_wait(&q_cv, &q_mu);
        }
        double t_active_start = now_ms_local();
        double idle = t_active_start - t_idle_start;

        FreeJob *batch = q_head;
        q_head = NULL;
        q_tail = NULL;
        int batch_depth = q_depth;
        q_depth = 0;
        pthread_mutex_unlock(&q_mu);

        if (atomic_load(&worker_should_stop) && !batch) break;

        while (batch) {
            FreeJob *next = batch->next;
            free_one(batch->node);
            free(batch);
            batch = next;
        }

        double active = now_ms_local() - t_active_start;
        pthread_mutex_lock(&stat_mu);
        worker_idle_ms   += idle;
        worker_active_ms += active;
        pthread_mutex_unlock(&stat_mu);

        /* notify any drain waiters that the queue is empty */
        pthread_mutex_lock(&q_mu);
        if (q_depth == 0) pthread_cond_broadcast(&q_drain_cv);
        pthread_mutex_unlock(&q_mu);

        (void)batch_depth;
    }
    atomic_store(&worker_running, 0);
    return NULL;
}

void gc_concurrent_enable(void) {
    if (atomic_load(&enabled)) return;
    atomic_store(&enabled, 1);
    atomic_store(&worker_should_stop, 0);
    if (!atomic_load(&worker_running)) {
        atomic_store(&worker_running, 1);
        if (pthread_create(&worker_thread, NULL, worker_main, NULL) != 0) {
            atomic_store(&worker_running, 0);
            atomic_store(&enabled, 0);
            fprintf(stderr, "gc_concurrent: pthread_create failed; staying synchronous\n");
        }
    }
}

void gc_concurrent_disable(void) {
    atomic_store(&enabled, 0);
    /* worker keeps running until gc_concurrent_stop() so a queued
     * batch isn't orphaned. New collections fall back to inline free. */
}

int gc_concurrent_is_enabled(void) {
    return atomic_load(&enabled);
}

void gc_concurrent_queue(GCNode *node) {
    if (!node) return;
    if (!atomic_load(&enabled)) {
        /* inline free on the calling thread - matches old behaviour */
        free_one(node);
        return;
    }
    FreeJob *job = (FreeJob *)malloc(sizeof(FreeJob));
    if (!job) { free_one(node); return; }
    job->node = node;
    job->next = NULL;

    pthread_mutex_lock(&q_mu);
    if (q_tail) q_tail->next = job;
    else        q_head = job;
    q_tail = job;
    q_depth++;
    atomic_fetch_add(&stat_queued, 1);
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mu);
}

void gc_concurrent_drain(void) {
    pthread_mutex_lock(&q_mu);
    while (q_head || q_depth > 0) {
        pthread_cond_wait(&q_drain_cv, &q_mu);
    }
    pthread_mutex_unlock(&q_mu);
}

void gc_concurrent_stop(void) {
    if (!atomic_load(&worker_running)) return;
    gc_concurrent_drain();
    pthread_mutex_lock(&q_mu);
    atomic_store(&worker_should_stop, 1);
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mu);
    pthread_join(worker_thread, NULL);
    atomic_store(&worker_running, 0);
}

GCConcurrentStats gc_concurrent_stats(void) {
    GCConcurrentStats s;
    s.queued_total = atomic_load(&stat_queued);
    s.freed_total  = atomic_load(&stat_freed);
    pthread_mutex_lock(&q_mu);
    s.queue_depth = q_depth;
    pthread_mutex_unlock(&q_mu);
    pthread_mutex_lock(&stat_mu);
    double total = worker_active_ms + worker_idle_ms;
    s.worker_idle_ratio = total > 0 ? worker_idle_ms / total : 1.0;
    pthread_mutex_unlock(&stat_mu);
    return s;
}
