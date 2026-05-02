/* Real-thread spawn for the bytecode VM.
 *
 * Each `spawn { ... }` allocates a private worker VM that shares the
 * parent's globals and stdlib registrations and runs the closure on a
 * fresh OS thread. The GIL serializes interpreter work so XS values
 * (refcounts, maps, arrays) stay coherent without atomics; sleep / IO /
 * channel.recv release the GIL while they block so spawn-and-sleep
 * workloads actually run in parallel.
 *
 * Live tasks register on a process-global table; the main thread drains
 * any unjoined tasks at exit so a background spawn that finishes after
 * the main script returns is still observed instead of being silently
 * dropped under detach. Errors that escape a spawn body are captured on
 * the task struct and surfaced when the parent awaits (or, for unjoined
 * tasks, printed once during drain). */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200112L
#include "vm/vm.h"
#include "vm/bytecode.h"
#include "core/value.h"
#include "core/xs_compat.h"
#include "core/xs_thread.h"
#include "runtime/concurrent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations from vm.c -- these are file-static there but the
   spawn worker needs to drive the same machinery. We re-declare with the
   matching signatures and vm.c exposes them via thin extern wrappers. */
VM   *vm_new_child(VM *parent);
void  vm_free_child(VM *vm);
Value *vm_invoke_public(VM *vm, Value *fn, Value **args, int argc);

typedef struct VMThreadTask {
    int                  id;
    int                  done;
    int                  errored;
    Value               *closure;
    Value               *result;          /* on success */
    Value               *error;           /* on error (incref'd snapshot) */
    xs_mutex_t           mu;
    xs_cond_t            cv;
    struct VMThreadTask *next;
} VMThreadTask;

static xs_mutex_t    g_vm_tasks_mu;
static int           g_vm_tasks_mu_init = 0;
static VMThreadTask *g_vm_tasks_head    = NULL;
static int           g_vm_next_task_id  = 1;

static void vm_tasks_mu_init_once(void) {
    if (g_vm_tasks_mu_init) return;
    xs_mutex_init(&g_vm_tasks_mu);
    g_vm_tasks_mu_init = 1;
}

static VMThreadTask *vm_tasks_find(int id) {
    for (VMThreadTask *t = g_vm_tasks_head; t; t = t->next)
        if (t->id == id) return t;
    return NULL;
}

typedef struct {
    VMThreadTask *task;
    /* Snapshot the globals map up-front instead of stashing the parent
       VM pointer: a nested spawn's parent is the OUTER worker VM, which
       gets freed when the outer body completes. The globals XSMap
       itself is owned by the root VM and survives. */
    XSMap        *globals;
} VMThreadArg;

static void *vm_thread_entry(void *arg_) {
    VMThreadArg *arg = (VMThreadArg *)arg_;
    VMThreadTask *t  = arg->task;
    XSMap        *globals = arg->globals;
    free(arg);

    xs_gil_acquire();

    /* Each spawn gets a private VM so its stack/frames/eff_stack don't
       race against whoever else is running under the GIL between yields.
       Globals stay shared via the cached XSMap pointer (root VM owns it). */
    VM *worker = vm_new_child(NULL);
    worker->globals = globals;
    worker->is_thread_worker = 1;

    Value *r = vm_invoke_public(worker, t->closure, NULL, 0);

    /* If the body raised an uncaught throw, vm_dispatch parked it on
       worker->uncaught_thread_exc instead of stderr. Surface it via the
       future so an `await` can re-raise it on the awaiter. */
    Value *captured_err = NULL;
    int errored = 0;
    if (worker->uncaught_thread_exc) {
        errored = 1;
        captured_err = worker->uncaught_thread_exc;
        worker->uncaught_thread_exc = NULL;
    }

    vm_free_child(worker);

    xs_mutex_lock(&t->mu);
    t->result  = r ? r : value_incref(XS_NULL_VAL);
    t->error   = captured_err;
    t->errored = errored;
    t->done    = 1;
    xs_cond_broadcast(&t->cv);
    xs_mutex_unlock(&t->mu);

    xs_gil_release();
    return NULL;
}

Value *vm_spawn_real(VM *parent, Value *closure) {
    if (!closure || (VAL_TAG(closure) != XS_CLOSURE && VAL_TAG(closure) != XS_NATIVE)) {
        return value_incref(XS_NULL_VAL);
    }
    vm_tasks_mu_init_once();

    VMThreadTask *t = xs_calloc(1, sizeof(VMThreadTask));
    xs_mutex_lock(&g_vm_tasks_mu);
    t->id   = g_vm_next_task_id++;
    t->next = g_vm_tasks_head;
    g_vm_tasks_head = t;
    xs_mutex_unlock(&g_vm_tasks_mu);

    t->closure = value_incref(closure);
    xs_mutex_init(&t->mu);
    xs_cond_init(&t->cv);

    VMThreadArg *arg = xs_malloc(sizeof(*arg));
    arg->task    = t;
    arg->globals = parent ? parent->globals : NULL;

    xs_thread_t th;
    if (xs_thread_create(&th, vm_thread_entry, arg) != 0) {
        free(arg);
        /* Fall back to synchronous execution so the program still makes
           progress on platforms without thread support. */
        Value *r = vm_invoke_public(parent, t->closure, NULL, 0);
        xs_mutex_lock(&t->mu);
        t->result = r ? r : value_incref(XS_NULL_VAL);
        t->done   = 1;
        xs_mutex_unlock(&t->mu);
    } else {
        xs_thread_detach(th);
    }

    Value *fut = xs_map_new();
    Value *id  = xs_int(t->id);
    map_set(fut->map, "_task_id", id);
    value_decref(id);
    Value *kind = xs_str("task");
    map_set(fut->map, "_kind", kind);
    value_decref(kind);
    Value *marker = value_incref(XS_TRUE_VAL);
    map_set(fut->map, "__is_future", marker);
    value_decref(marker);
    return fut;
}

/* Block on the task's condvar (releasing GIL) until it finishes. */
Value *vm_await_task(int task_id, int *errored_out, Value **err_out) {
    if (task_id <= 0) return value_incref(XS_NULL_VAL);
    vm_tasks_mu_init_once();

    xs_mutex_lock(&g_vm_tasks_mu);
    VMThreadTask *t = vm_tasks_find(task_id);
    xs_mutex_unlock(&g_vm_tasks_mu);
    if (!t) return value_incref(XS_NULL_VAL);

    xs_gil_release();
    xs_mutex_lock(&t->mu);
    while (!t->done) xs_cond_wait(&t->cv, &t->mu);
    Value *r = t->result ? value_incref(t->result) : value_incref(XS_NULL_VAL);
    if (errored_out) *errored_out = t->errored;
    if (err_out)     *err_out     = t->error ? value_incref(t->error) : NULL;
    /* Clear the error so the at-exit drain doesn't double-report it.
       The awaiter has either captured it (err_out != NULL) or chosen
       not to look (err_out == NULL); either way, ownership transferred
       and drain should treat the task as handled. */
    if (t->error) {
        value_decref(t->error);
        t->error = NULL;
        t->errored = 0;
    }
    xs_mutex_unlock(&t->mu);
    xs_gil_acquire();
    return r;
}

/* Drain any not-yet-awaited tasks. Called at program exit so a fire-and
   -forget spawn { sleep_ms(N) } actually completes before we return.
   Errors raised inside an unjoined task are reported once on stderr.
   This walks both pending and already-completed tasks: vm_await_task
   nulls out t->error after handing it off, so anything still carrying
   an error here is by definition unjoined and should be surfaced. */
void vm_drain_tasks(void) {
    if (!g_vm_tasks_mu_init) return;
    /* First, wait for any tasks that haven't completed yet. */
    for (;;) {
        xs_mutex_lock(&g_vm_tasks_mu);
        VMThreadTask *pending = NULL;
        for (VMThreadTask *t = g_vm_tasks_head; t; t = t->next) {
            xs_mutex_lock(&t->mu);
            int done = t->done;
            xs_mutex_unlock(&t->mu);
            if (!done) { pending = t; break; }
        }
        xs_mutex_unlock(&g_vm_tasks_mu);
        if (!pending) break;

        xs_gil_release();
        xs_mutex_lock(&pending->mu);
        while (!pending->done) xs_cond_wait(&pending->cv, &pending->mu);
        xs_mutex_unlock(&pending->mu);
        xs_gil_acquire();
    }
    /* Now scan everything for unconsumed errors. Clear t->error after
       reporting so a second drain call (e.g. atexit after the VM-side
       call) doesn't double-print. */
    xs_mutex_lock(&g_vm_tasks_mu);
    for (VMThreadTask *t = g_vm_tasks_head; t; t = t->next) {
        xs_mutex_lock(&t->mu);
        int errored = t->errored;
        Value *err  = t->error;
        if (errored && err) {
            const char *msg = NULL;
            if (VAL_TAG(err) == XS_STR) msg = err->s;
            else if (VAL_TAG(err) == XS_MAP) {
                Value *m = map_get(err->map, "message");
                if (m && VAL_TAG(m) == XS_STR) msg = m->s;
            }
            fprintf(stderr, "uncaught in spawn: %s\n", msg ? msg : "<error>");
            value_decref(t->error);
            t->error = NULL;
            t->errored = 0;
        }
        xs_mutex_unlock(&t->mu);
    }
    xs_mutex_unlock(&g_vm_tasks_mu);
}
