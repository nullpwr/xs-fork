/* Real concurrency: pthread-backed spawn + GIL + blocking channels.
   Replaces the previous "spawn runs synchronously, channel.recv throws
   on empty" placeholder. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200112L
#include "runtime/concurrent.h"
#include "runtime/interp.h"
#include "runtime/error.h"
#include "core/value.h"
#include "core/env.h"
#include <stdlib.h>
#include <string.h>

/* The GIL itself plus a recursion counter. Recursion lets the same
   thread reacquire the lock without deadlocking on nested interp
   entries. */
static xs_mutex_t  g_gil;
static int         g_gil_init_done = 0;

void xs_gil_init(void) {
    if (g_gil_init_done) return;
#if defined(_WIN32) || defined(__MINGW32__)
    xs_mutex_init(&g_gil);
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_gil, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
    g_gil_init_done = 1;
    /* main thread enters holding the lock */
    xs_mutex_lock(&g_gil);
}

void xs_gil_acquire(void) {
    if (!g_gil_init_done) xs_gil_init();
    xs_mutex_lock(&g_gil);
}

void xs_gil_release(void) {
    if (!g_gil_init_done) return;
    xs_mutex_unlock(&g_gil);
}

/* Per-task record. Stays alive after the thread exits so callers can
   await the result. The owning future map holds a refcount via _task_id. */
typedef struct ThreadTask {
    int                 id;
    int                 done;
    int                 errored;
    Value              *closure;       /* incref'd while running */
    Value              *result;        /* set under task_mu when done */
    xs_mutex_t          mu;
    xs_cond_t           cv;
    struct ThreadTask  *next;
} ThreadTask;

static xs_mutex_t   g_tasks_mu;
static int          g_tasks_mu_init = 0;
static ThreadTask  *g_tasks_head    = NULL;
static int          g_next_task_id  = 1;

static void tasks_mu_init_once(void) {
    if (g_tasks_mu_init) return;
    xs_mutex_init(&g_tasks_mu);
    g_tasks_mu_init = 1;
}

static ThreadTask *tasks_find(int id) {
    for (ThreadTask *t = g_tasks_head; t; t = t->next)
        if (t->id == id) return t;
    return NULL;
}

typedef struct {
    ThreadTask *task;
    Interp     *interp;   /* shared with main; protected by GIL */
} ThreadArg;

static void *thread_entry(void *arg_) {
    ThreadArg *arg = (ThreadArg *)arg_;
    ThreadTask *t  = arg->task;
    Interp     *ig = arg->interp;
    free(arg);

    xs_gil_acquire();
    /* Save and clear control-flow signals so the spawned task starts
       clean and any throw it does is captured here, not propagated to
       the parent. */
    int  saved_signal = ig->cf.signal;
    Value *saved_val  = ig->cf.value;
    ig->cf.signal = 0;
    ig->cf.value  = NULL;

    Value *r = call_value(ig, t->closure, NULL, 0, "spawn");
    int errored = (ig->cf.signal == CF_THROW || ig->cf.signal == CF_ERROR
                   || ig->cf.signal == CF_PANIC);

    /* Restore parent signals; the spawned task must not poison them. */
    if (ig->cf.value) value_decref(ig->cf.value);
    ig->cf.signal = saved_signal;
    ig->cf.value  = saved_val;

    xs_mutex_lock(&t->mu);
    t->result  = r ? r : value_incref(XS_NULL_VAL);
    t->errored = errored;
    t->done    = 1;
    xs_cond_broadcast(&t->cv);
    xs_mutex_unlock(&t->mu);

    xs_gil_release();
    return NULL;
}

Value *xs_spawn_thread(Interp *parent, Value *closure) {
    if (!closure || (VAL_TAG(closure) != XS_FUNC && VAL_TAG(closure) != XS_NATIVE)) {
        return value_incref(XS_NULL_VAL);
    }
    tasks_mu_init_once();

    ThreadTask *t = xs_calloc(1, sizeof(ThreadTask));
    xs_mutex_lock(&g_tasks_mu);
    t->id = g_next_task_id++;
    t->next = g_tasks_head;
    g_tasks_head = t;
    xs_mutex_unlock(&g_tasks_mu);

    t->closure = value_incref(closure);
    xs_mutex_init(&t->mu);
    xs_cond_init(&t->cv);

    ThreadArg *arg = xs_malloc(sizeof(ThreadArg));
    arg->task   = t;
    arg->interp = parent;

    xs_thread_t th;
    if (xs_thread_create(&th, thread_entry, arg) != 0) {
        free(arg);
        return value_incref(XS_NULL_VAL);
    }
    xs_thread_detach(th);

    /* Future-shaped result map for the caller to await on. */
    Value *fut = xs_map_new();
    Value *id  = xs_int(t->id);
    map_set(fut->map, "_task_id", id);
    value_decref(id);
    Value *kind = xs_str("task");
    map_set(fut->map, "_kind", kind);
    value_decref(kind);
    return fut;
}

Value *xs_await_task(int task_id) {
    if (task_id <= 0) return value_incref(XS_NULL_VAL);
    tasks_mu_init_once();

    xs_mutex_lock(&g_tasks_mu);
    ThreadTask *t = tasks_find(task_id);
    xs_mutex_unlock(&g_tasks_mu);
    if (!t) return value_incref(XS_NULL_VAL);

    /* Release the GIL so the spawned thread can actually run. */
    xs_gil_release();
    xs_mutex_lock(&t->mu);
    while (!t->done) xs_cond_wait(&t->cv, &t->mu);
    Value *r = t->result ? value_incref(t->result) : value_incref(XS_NULL_VAL);
    xs_mutex_unlock(&t->mu);
    xs_gil_acquire();
    return r;
}

/* --- channels backed by mutex+condvar -------------------------------- */
/* The channel is a regular XS_MAP that stores an integer "_chan_id" key.
   The actual mutex+condvar+buffer live in a process-global table indexed
   by that id. This avoids extending the Value type tag with a raw-ptr
   variant. */

typedef struct ChanState {
    xs_mutex_t mu;
    xs_cond_t  cv;
    /* The buffer is owned by the XS_MAP slot "_buf"; the ChanState only
       provides synchronization. We still cache an XSArray* here for
       speed but it shares ownership with the map slot. */
} ChanState;

#define MAX_CHANS 4096
static ChanState g_chans[MAX_CHANS];
static int       g_n_chans = 0;
static xs_mutex_t g_chans_mu;
static int       g_chans_mu_init = 0;

static void chans_mu_init_once(void) {
    if (g_chans_mu_init) return;
    xs_mutex_init(&g_chans_mu);
    g_chans_mu_init = 1;
}

int xs_chan_alloc(void) {
    chans_mu_init_once();
    xs_mutex_lock(&g_chans_mu);
    if (g_n_chans >= MAX_CHANS) {
        xs_mutex_unlock(&g_chans_mu);
        return -1;
    }
    int id = g_n_chans++;
    xs_mutex_init(&g_chans[id].mu);
    xs_cond_init(&g_chans[id].cv);
    xs_mutex_unlock(&g_chans_mu);
    return id;
}

static ChanState *chan_state(Value *ch) {
    if (!ch || (VAL_TAG(ch) != XS_MAP && VAL_TAG(ch) != XS_MODULE) || !ch->map) return NULL;
    Value *p = map_get(ch->map, "_chan_id");
    if (!p || VAL_TAG(p) != XS_INT) return NULL;
    int id = (int)VAL_INT(p);
    if (id < 0 || id >= g_n_chans) return NULL;
    return &g_chans[id];
}

static XSArray *chan_buf(Value *ch) {
    if (!ch || (VAL_TAG(ch) != XS_MAP && VAL_TAG(ch) != XS_MODULE) || !ch->map) return NULL;
    Value *b = map_get(ch->map, "_buf");
    if (!b || VAL_TAG(b) != XS_ARRAY) return NULL;
    return b->arr;
}

Value *xs_chan_send(Value *ch, Value *v) {
    ChanState *cs = chan_state(ch);
    XSArray   *bf = chan_buf(ch);
    if (!cs || !bf) return value_incref(XS_NULL_VAL);

    xs_mutex_lock(&cs->mu);
    array_push(bf, v);
    xs_cond_broadcast(&cs->cv);
    xs_mutex_unlock(&cs->mu);
    return value_incref(XS_NULL_VAL);
}

Value *xs_chan_recv(Value *ch, Interp *interp) {
    (void)interp;
    ChanState *cs = chan_state(ch);
    XSArray   *bf = chan_buf(ch);
    if (!cs || !bf) return value_incref(XS_NULL_VAL);

    /* Drop the GIL while we sit on the channel condvar so other threads
       (and the sender we are waiting on) can actually run. */
    xs_gil_release();
    xs_mutex_lock(&cs->mu);
    while (bf->len == 0) {
        xs_cond_wait(&cs->cv, &cs->mu);
    }
    Value *val = value_incref(bf->items[0]);
    for (int i = 0; i < bf->len - 1; i++) bf->items[i] = bf->items[i + 1];
    bf->len--;
    xs_mutex_unlock(&cs->mu);
    xs_gil_acquire();
    return val;
}

Value *xs_chan_try_recv(Value *ch) {
    ChanState *cs = chan_state(ch);
    XSArray   *bf = chan_buf(ch);
    if (!cs || !bf) return value_incref(XS_NULL_VAL);

    xs_mutex_lock(&cs->mu);
    Value *val = NULL;
    if (bf->len > 0) {
        val = value_incref(bf->items[0]);
        for (int i = 0; i < bf->len - 1; i++) bf->items[i] = bf->items[i + 1];
        bf->len--;
    }
    xs_mutex_unlock(&cs->mu);
    return val ? val : value_incref(XS_NULL_VAL);
}

int xs_chan_len(Value *ch) {
    ChanState *cs = chan_state(ch);
    XSArray   *bf = chan_buf(ch);
    if (!cs || !bf) return 0;
    xs_mutex_lock(&cs->mu);
    int n = bf->len;
    xs_mutex_unlock(&cs->mu);
    return n;
}

void xs_sleep_seconds(double secs) {
    if (secs <= 0) return;
    xs_gil_release();
    xs_thread_sleep_ns(secs);
    xs_gil_acquire();
}

/* --- lazy generator worker thread --------------------------------- */
typedef struct GenArg {
    Interp *parent;
    Value  *closure;
    Value  *yield_chan;
    Value  *resume_chan;
} GenArg;

static void *gen_worker_entry(void *arg_) {
    GenArg *ga = (GenArg *)arg_;
    Interp *ip = ga->parent;
    Value  *closure  = ga->closure;
    Value  *ych      = ga->yield_chan;
    Value  *rch      = ga->resume_chan;
    free(ga);

    xs_gil_acquire();

    /* Wait for the first .next() to issue a permit before any code in
       the body runs. This makes generator bodies behave like a paused
       coroutine that resumes on demand. */
    Value *first = xs_chan_recv(rch, ip);
    if (first) value_decref(first);

    /* Install the lazy-handoff channels so NODE_YIELD routes through
       them instead of accumulating into i->yield_collect. We restore
       on exit; we are running on a separate thread but the GIL ensures
       we're alone in the interpreter. */
    Value *saved_yield_chan  = ip->lazy_yield_chan;
    Value *saved_resume_chan = ip->lazy_resume_chan;
    Value *saved_collect     = ip->yield_collect;
    int    saved_limit       = ip->yield_limit;
    ip->lazy_yield_chan  = ych;
    ip->lazy_resume_chan = rch;
    ip->yield_collect    = NULL;
    ip->yield_limit      = 0;

    Value *body = call_value(ip, closure, NULL, 0, "gen");
    if (body) value_decref(body);
    if (ip->cf.signal == CF_RETURN || ip->cf.signal == CF_YIELD) {
        if (ip->cf.value) value_decref(ip->cf.value);
        ip->cf.signal = 0; ip->cf.value = NULL;
    }

    ip->lazy_yield_chan  = saved_yield_chan;
    ip->lazy_resume_chan = saved_resume_chan;
    ip->yield_collect    = saved_collect;
    ip->yield_limit      = saved_limit;

    /* End-of-stream sentinel. The for-loop / .next() consumer sees
       _gen_eos=true and stops. */
    Value *eos = xs_map_new();
    Value *t   = value_incref(XS_TRUE_VAL);
    map_set(eos->map, "_gen_eos", t);
    value_decref(t);
    xs_chan_send(ych, eos);
    value_decref(eos);

    value_decref(closure);
    value_decref(ych);
    value_decref(rch);
    xs_gil_release();
    return NULL;
}

void xs_spawn_generator(Interp *parent, Value *closure,
                        Value *yield_chan, Value *resume_chan) {
    GenArg *ga = xs_malloc(sizeof(GenArg));
    ga->parent      = parent;
    ga->closure     = value_incref(closure);
    ga->yield_chan  = value_incref(yield_chan);
    ga->resume_chan = value_incref(resume_chan);
    xs_thread_t th;
    if (xs_thread_create(&th, gen_worker_entry, ga) != 0) {
        value_decref(ga->closure);
        value_decref(ga->yield_chan);
        value_decref(ga->resume_chan);
        free(ga);
        return;
    }
    xs_thread_detach(th);
}
