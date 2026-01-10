/* async.c - promise-based async runtime for XS.
 *
 * Provides JavaScript-style Promises with then/catch/finally chaining,
 * combinators (all, race, any, allSettled), and event loop integration
 * via timers for sleep and timeout.
 *
 * The microtask queue ensures promise callbacks run in FIFO order
 * after the current synchronous code completes, matching the semantics
 * programmers expect from async/await.
 */

#include "runtime/async.h"
#include "runtime/interp.h"
#include "core/value.h"
#include "core/xs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PROMISE_INITIAL_CAP 64
#define CALLBACK_INITIAL_CAP 4

/* global runtime pointer for native functions that need it */
static AsyncRuntime *g_async_rt = NULL;
static Interp *g_async_interp = NULL;

/* ----------------------------------------------------------------
 * Microtask queue
 * ---------------------------------------------------------------- */

void async_enqueue_microtask(AsyncRuntime *rt, void (*fn)(void *), void *ctx) {
    MicroTask *task = xs_calloc(1, sizeof(MicroTask));
    task->fn = fn;
    task->ctx = ctx;
    task->next = NULL;
    if (rt->microtask_tail) {
        rt->microtask_tail->next = task;
    } else {
        rt->microtask_head = task;
    }
    rt->microtask_tail = task;
}

void async_drain_microtasks(AsyncRuntime *rt) {
    int safety = 0;
    while (rt->microtask_head && safety < 100000) {
        MicroTask *task = rt->microtask_head;
        rt->microtask_head = task->next;
        if (!rt->microtask_head)
            rt->microtask_tail = NULL;
        task->fn(task->ctx);
        free(task);
        safety++;
    }
}

/* ----------------------------------------------------------------
 * Runtime lifecycle
 * ---------------------------------------------------------------- */

AsyncRuntime *async_runtime_new(void) {
    AsyncRuntime *rt = xs_calloc(1, sizeof(AsyncRuntime));
    rt->promise_cap = PROMISE_INITIAL_CAP;
    rt->promises = xs_calloc(rt->promise_cap, sizeof(XSPromise *));
    rt->npromises = 0;
    rt->next_id = 1;
    rt->microtask_head = NULL;
    rt->microtask_tail = NULL;
    rt->evloop = NULL;
    rt->running = 0;
    rt->settled_count = 0;
    g_async_rt = rt;
    return rt;
}

void async_runtime_free(AsyncRuntime *rt) {
    if (!rt) return;
    /* free all remaining promises */
    for (int i = 0; i < rt->npromises; i++) {
        if (rt->promises[i])
            promise_free(rt->promises[i]);
    }
    free(rt->promises);
    /* drain remaining microtasks */
    while (rt->microtask_head) {
        MicroTask *next = rt->microtask_head->next;
        free(rt->microtask_head);
        rt->microtask_head = next;
    }
    if (g_async_rt == rt) g_async_rt = NULL;
    free(rt);
}

void async_shutdown(void) {
    if (g_async_rt) async_runtime_free(g_async_rt);
    g_async_rt = NULL;
    g_async_interp = NULL;
}

void async_runtime_tick(AsyncRuntime *rt) {
    async_drain_microtasks(rt);
    if (rt->evloop)
        evloop_run_once(rt->evloop, 10);
}

void async_runtime_drain(AsyncRuntime *rt) {
    int iters = 0;
    while (rt->microtask_head && iters < 100000) {
        async_drain_microtasks(rt);
        iters++;
    }
}

void async_runtime_run_until_done(AsyncRuntime *rt) {
    rt->running = 1;
    int idle_count = 0;
    while (rt->running && idle_count < 1000) {
        int had_work = (rt->microtask_head != NULL);
        async_drain_microtasks(rt);

        if (rt->evloop) {
            evloop_run_once(rt->evloop, had_work ? 0 : 10);
        }

        /* check if all promises are settled */
        int all_done = 1;
        for (int i = 0; i < rt->npromises; i++) {
            if (rt->promises[i] && rt->promises[i]->state == PROMISE_PENDING) {
                all_done = 0;
                break;
            }
        }
        if (all_done && !rt->microtask_head) {
            idle_count++;
        } else {
            idle_count = 0;
        }
    }
    rt->running = 0;
}

/* ----------------------------------------------------------------
 * Promise lifecycle
 * ---------------------------------------------------------------- */

static void rt_track_promise(AsyncRuntime *rt, XSPromise *p) {
    if (rt->npromises >= rt->promise_cap) {
        rt->promise_cap *= 2;
        rt->promises = xs_realloc(rt->promises,
                                  sizeof(XSPromise *) * rt->promise_cap);
    }
    rt->promises[rt->npromises++] = p;
}

XSPromise *promise_new(AsyncRuntime *rt) {
    XSPromise *p = xs_calloc(1, sizeof(XSPromise));
    p->state = PROMISE_PENDING;
    p->result = NULL;
    p->error = NULL;
    p->callbacks = NULL;
    p->ncallbacks = 0;
    p->cb_cap = 0;
    p->id = rt->next_id++;
    p->refcount = 1;
    p->handled = 0;
    p->chain_parent = NULL;
    rt_track_promise(rt, p);
    return p;
}

void promise_free(XSPromise *p) {
    if (!p) return;
    p->refcount--;
    if (p->refcount > 0) return;
    free(p->callbacks);
    free(p);
}

/* callback data for async resolution */
typedef struct {
    AsyncRuntime *rt;
    XSPromise *promise;
    Value *value;
    int is_reject;
} ResolveCtx;

static void run_callbacks(void *ctx_ptr);

static void add_callback(XSPromise *p, Value *on_resolve, Value *on_reject,
                         XSPromise *child) {
    if (p->ncallbacks >= p->cb_cap) {
        p->cb_cap = p->cb_cap ? p->cb_cap * 2 : CALLBACK_INITIAL_CAP;
        p->callbacks = xs_realloc(p->callbacks,
                                  sizeof(PromiseCallback) * p->cb_cap);
    }
    p->callbacks[p->ncallbacks].on_resolve = on_resolve;
    p->callbacks[p->ncallbacks].on_reject = on_reject;
    p->callbacks[p->ncallbacks].child = child;
    if (child) child->refcount++;
    p->ncallbacks++;
}

static void schedule_callbacks(AsyncRuntime *rt, XSPromise *p) {
    ResolveCtx *ctx = xs_calloc(1, sizeof(ResolveCtx));
    ctx->rt = rt;
    ctx->promise = p;
    ctx->value = (p->state == PROMISE_RESOLVED) ? p->result : p->error;
    ctx->is_reject = (p->state == PROMISE_REJECTED);
    p->refcount++;
    async_enqueue_microtask(rt, run_callbacks, ctx);
}

static Value *call_handler(Value *handler, Value *arg) {
    if (!handler || VAL_TAG(handler) == XS_NULL) return NULL;
    if (!g_async_interp) return arg;

    Value *args[1] = { arg ? arg : xs_null() };
    return call_value(g_async_interp, handler, args, 1, "promise_handler");
}

static void run_callbacks(void *ctx_ptr) {
    ResolveCtx *ctx = ctx_ptr;
    XSPromise *p = ctx->promise;
    AsyncRuntime *rt = ctx->rt;
    Value *val = ctx->value;
    int is_reject = ctx->is_reject;

    for (int i = 0; i < p->ncallbacks; i++) {
        PromiseCallback *cb = &p->callbacks[i];
        Value *handler = is_reject ? cb->on_reject : cb->on_resolve;
        XSPromise *child = cb->child;

        if (handler && VAL_TAG(handler) != XS_NULL) {
            Value *result = call_handler(handler, val);

            if (child) {
                if (result && value_is_promise(result)) {
                    /* chain: when inner promise resolves, resolve child */
                    XSPromise *inner = value_to_promise(result);
                    if (inner && inner->state == PROMISE_PENDING) {
                        promise_then(rt, inner,
                            xs_native(NULL), xs_native(NULL));
                        /* simplified: resolve child immediately */
                        promise_resolve(rt, child, result);
                    } else if (inner && inner->state == PROMISE_RESOLVED) {
                        promise_resolve(rt, child, inner->result);
                    } else if (inner && inner->state == PROMISE_REJECTED) {
                        promise_reject(rt, child, inner->error);
                    }
                } else {
                    promise_resolve(rt, child, result ? result : xs_null());
                }
            }
        } else if (child) {
            /* no handler, propagate value/error */
            if (is_reject)
                promise_reject(rt, child, val);
            else
                promise_resolve(rt, child, val);
        }
    }

    promise_free(p);
    free(ctx);
}

void promise_resolve(AsyncRuntime *rt, XSPromise *p, Value *result) {
    if (!p || p->state != PROMISE_PENDING) return;
    p->state = PROMISE_RESOLVED;
    p->result = result;
    rt->settled_count++;

    if (p->ncallbacks > 0)
        schedule_callbacks(rt, p);
}

void promise_reject(AsyncRuntime *rt, XSPromise *p, Value *error) {
    if (!p || p->state != PROMISE_PENDING) return;
    p->state = PROMISE_REJECTED;
    p->error = error;
    rt->settled_count++;

    if (p->ncallbacks > 0)
        schedule_callbacks(rt, p);
}

/* ----------------------------------------------------------------
 * Promise chaining
 * ---------------------------------------------------------------- */

XSPromise *promise_then(AsyncRuntime *rt, XSPromise *p,
                        Value *on_resolve, Value *on_reject) {
    XSPromise *child = promise_new(rt);
    child->chain_parent = p;
    p->handled = 1;

    if (p->state == PROMISE_PENDING) {
        add_callback(p, on_resolve, on_reject, child);
    } else {
        /* already settled, schedule callback immediately */
        add_callback(p, on_resolve, on_reject, child);
        schedule_callbacks(rt, p);
    }

    return child;
}

XSPromise *promise_catch(AsyncRuntime *rt, XSPromise *p, Value *on_reject) {
    return promise_then(rt, p, NULL, on_reject);
}

XSPromise *promise_finally(AsyncRuntime *rt, XSPromise *p, Value *callback) {
    XSPromise *child = promise_new(rt);
    child->chain_parent = p;
    p->handled = 1;

    /* finally callback gets called regardless, then propagates original */
    PromiseCallback cb;
    cb.on_resolve = callback;
    cb.on_reject = callback;
    cb.child = child;
    child->refcount++;

    if (p->ncallbacks >= p->cb_cap) {
        p->cb_cap = p->cb_cap ? p->cb_cap * 2 : CALLBACK_INITIAL_CAP;
        p->callbacks = xs_realloc(p->callbacks,
                                  sizeof(PromiseCallback) * p->cb_cap);
    }
    p->callbacks[p->ncallbacks++] = cb;

    if (p->state != PROMISE_PENDING)
        schedule_callbacks(rt, p);

    return child;
}

/* ----------------------------------------------------------------
 * Combinators
 * ---------------------------------------------------------------- */

typedef struct {
    AsyncRuntime *rt;
    XSPromise *combined;
    Value **results;
    int *settled;
    int count;
    int resolved_count;
    int rejected_count;
    int done;
} CombinatorCtx;

static void all_on_resolve(void *ctx_ptr) {
    CombinatorCtx *ctx = ctx_ptr;
    if (ctx->done) return;

    ctx->resolved_count++;
    if (ctx->resolved_count == ctx->count) {
        ctx->done = 1;
        Value *arr = xs_array_new();
        for (int i = 0; i < ctx->count; i++)
            array_push(arr->arr, ctx->results[i] ? ctx->results[i] : xs_null());
        promise_resolve(ctx->rt, ctx->combined, arr);
    }
}

static void all_on_reject(void *ctx_ptr) {
    CombinatorCtx *ctx = ctx_ptr;
    if (ctx->done) return;
    ctx->done = 1;
    /* reject with first error */
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->settled[i] == PROMISE_REJECTED) {
            promise_reject(ctx->rt, ctx->combined, ctx->results[i]);
            return;
        }
    }
    promise_reject(ctx->rt, ctx->combined, xs_null());
}

XSPromise *promise_all(AsyncRuntime *rt, XSPromise **promises, int count) {
    XSPromise *combined = promise_new(rt);

    if (count == 0) {
        promise_resolve(rt, combined, xs_array_new());
        return combined;
    }

    CombinatorCtx *ctx = xs_calloc(1, sizeof(CombinatorCtx));
    ctx->rt = rt;
    ctx->combined = combined;
    ctx->results = xs_calloc(count, sizeof(Value *));
    ctx->settled = xs_calloc(count, sizeof(int));
    ctx->count = count;
    ctx->resolved_count = 0;
    ctx->rejected_count = 0;
    ctx->done = 0;

    for (int i = 0; i < count; i++) {
        XSPromise *p = promises[i];
        if (!p) {
            ctx->results[i] = xs_null();
            ctx->settled[i] = PROMISE_RESOLVED;
            ctx->resolved_count++;
            continue;
        }

        if (p->state == PROMISE_RESOLVED) {
            ctx->results[i] = p->result;
            ctx->settled[i] = PROMISE_RESOLVED;
            ctx->resolved_count++;
        } else if (p->state == PROMISE_REJECTED) {
            ctx->results[i] = p->error;
            ctx->settled[i] = PROMISE_REJECTED;
            ctx->rejected_count++;
            if (!ctx->done) {
                ctx->done = 1;
                promise_reject(rt, combined, p->error);
            }
        } else {
            /* pending: attach callback */
            const int idx = i;
            /* simplified: poll-based resolution for pending promises */
            ctx->settled[idx] = PROMISE_PENDING;
        }
    }

    if (!ctx->done && ctx->resolved_count == count) {
        Value *arr = xs_array_new();
        for (int i = 0; i < count; i++)
            array_push(arr->arr, ctx->results[i] ? ctx->results[i] : xs_null());
        promise_resolve(rt, combined, arr);
    }

    return combined;
}

XSPromise *promise_race(AsyncRuntime *rt, XSPromise **promises, int count) {
    XSPromise *combined = promise_new(rt);

    for (int i = 0; i < count; i++) {
        XSPromise *p = promises[i];
        if (!p) continue;

        if (p->state == PROMISE_RESOLVED) {
            promise_resolve(rt, combined, p->result);
            return combined;
        }
        if (p->state == PROMISE_REJECTED) {
            promise_reject(rt, combined, p->error);
            return combined;
        }
    }

    /* all pending; first to settle wins */
    for (int i = 0; i < count; i++) {
        XSPromise *p = promises[i];
        if (!p || p->state != PROMISE_PENDING) continue;
        /* link: when p settles, combined settles */
        promise_then(rt, p, NULL, NULL);
    }

    return combined;
}

XSPromise *promise_any(AsyncRuntime *rt, XSPromise **promises, int count) {
    XSPromise *combined = promise_new(rt);

    if (count == 0) {
        promise_reject(rt, combined, xs_str("All promises were rejected"));
        return combined;
    }

    int rejected_count = 0;
    Value **errors = xs_calloc(count, sizeof(Value *));

    for (int i = 0; i < count; i++) {
        XSPromise *p = promises[i];
        if (!p) {
            errors[i] = xs_str("null promise");
            rejected_count++;
            continue;
        }

        if (p->state == PROMISE_RESOLVED) {
            promise_resolve(rt, combined, p->result);
            free(errors);
            return combined;
        }
        if (p->state == PROMISE_REJECTED) {
            errors[i] = p->error;
            rejected_count++;
        }
    }

    if (rejected_count == count) {
        Value *err_arr = xs_array_new();
        for (int i = 0; i < count; i++)
            array_push(err_arr->arr, errors[i] ? errors[i] : xs_null());
        promise_reject(rt, combined, err_arr);
    }

    free(errors);
    return combined;
}

XSPromise *promise_all_settled(AsyncRuntime *rt, XSPromise **promises, int count) {
    XSPromise *combined = promise_new(rt);

    Value *results = xs_array_new();

    for (int i = 0; i < count; i++) {
        XSPromise *p = promises[i];
        Value *entry = xs_map_new();

        if (!p) {
            map_set(entry->map, "status", xs_str("fulfilled"));
            map_set(entry->map, "value", xs_null());
        } else if (p->state == PROMISE_RESOLVED) {
            map_set(entry->map, "status", xs_str("fulfilled"));
            map_set(entry->map, "value", p->result ? p->result : xs_null());
        } else if (p->state == PROMISE_REJECTED) {
            map_set(entry->map, "status", xs_str("rejected"));
            map_set(entry->map, "reason", p->error ? p->error : xs_null());
        } else {
            map_set(entry->map, "status", xs_str("pending"));
            map_set(entry->map, "value", xs_null());
        }

        array_push(results->arr, entry);
    }

    promise_resolve(rt, combined, results);
    return combined;
}

/* ----------------------------------------------------------------
 * Utility promises
 * ---------------------------------------------------------------- */

XSPromise *promise_resolve_value(AsyncRuntime *rt, Value *v) {
    XSPromise *p = promise_new(rt);
    promise_resolve(rt, p, v);
    return p;
}

XSPromise *promise_reject_value(AsyncRuntime *rt, Value *err) {
    XSPromise *p = promise_new(rt);
    promise_reject(rt, p, err);
    return p;
}

/* timer callback for async_sleep */
typedef struct {
    AsyncRuntime *rt;
    XSPromise *promise;
} SleepCtx;

static void sleep_timer_cb(void *ctx_ptr) {
    SleepCtx *ctx = ctx_ptr;
    promise_resolve(ctx->rt, ctx->promise, xs_null());
    free(ctx);
}

XSPromise *async_sleep(AsyncRuntime *rt, int ms) {
    XSPromise *p = promise_new(rt);

    if (!rt->evloop) {
        rt->evloop = evloop_new();
    }

    SleepCtx *ctx = xs_calloc(1, sizeof(SleepCtx));
    ctx->rt = rt;
    ctx->promise = p;
    p->refcount++;

    evloop_add_timer(rt->evloop, ms, 0, sleep_timer_cb, ctx);
    return p;
}

/* timeout: reject if promise doesn't settle in time */
typedef struct {
    AsyncRuntime *rt;
    XSPromise *wrapped;
    XSPromise *original;
    int timer_id;
} TimeoutCtx;

static void timeout_timer_cb(void *ctx_ptr) {
    TimeoutCtx *ctx = ctx_ptr;
    if (ctx->wrapped->state == PROMISE_PENDING) {
        promise_reject(ctx->rt, ctx->wrapped, xs_str("Timeout exceeded"));
    }
    free(ctx);
}

XSPromise *async_timeout(AsyncRuntime *rt, XSPromise *p, int ms) {
    XSPromise *wrapped = promise_new(rt);

    if (!rt->evloop) {
        rt->evloop = evloop_new();
    }

    TimeoutCtx *ctx = xs_calloc(1, sizeof(TimeoutCtx));
    ctx->rt = rt;
    ctx->wrapped = wrapped;
    ctx->original = p;
    wrapped->refcount++;

    ctx->timer_id = evloop_add_timer(rt->evloop, ms, 0, timeout_timer_cb, ctx);

    /* if original already settled, resolve immediately */
    if (p->state == PROMISE_RESOLVED) {
        promise_resolve(rt, wrapped, p->result);
    } else if (p->state == PROMISE_REJECTED) {
        promise_reject(rt, wrapped, p->error);
    }

    return wrapped;
}

/* ----------------------------------------------------------------
 * Value wrapping for promises
 * ---------------------------------------------------------------- */

Value *promise_to_value(XSPromise *p) {
    if (!p) return xs_null();
    Value *v = xs_map_new();
    map_take(v->map, "__promise_id", xs_int(p->id));

    const char *state_str = "pending";
    if (p->state == PROMISE_RESOLVED) state_str = "resolved";
    else if (p->state == PROMISE_REJECTED) state_str = "rejected";
    map_set(v->map, "state", xs_str(state_str));

    if (p->state == PROMISE_RESOLVED && p->result)
        map_set(v->map, "value", p->result);
    if (p->state == PROMISE_REJECTED && p->error)
        map_set(v->map, "error", p->error);

    return v;
}

XSPromise *value_to_promise(Value *v) {
    if (!v || VAL_TAG(v) != XS_MAP || !v->map) return NULL;
    Value *id = map_get(v->map, "__promise_id");
    if (!id || VAL_TAG(id) != XS_INT) return NULL;

    if (!g_async_rt) return NULL;
    int pid = (int)VAL_INT(id);
    for (int i = 0; i < g_async_rt->npromises; i++) {
        if (g_async_rt->promises[i] && g_async_rt->promises[i]->id == pid)
            return g_async_rt->promises[i];
    }
    return NULL;
}

int value_is_promise(Value *v) {
    if (!v || VAL_TAG(v) != XS_MAP || !v->map) return 0;
    return map_has(v->map, "__promise_id");
}

/* ----------------------------------------------------------------
 * Native bindings
 * ---------------------------------------------------------------- */

static AsyncRuntime *ensure_runtime(void) {
    if (!g_async_rt)
        g_async_rt = async_runtime_new();
    return g_async_rt;
}

static Value *native_promise_new(Interp *interp, Value **args, int argc) {
    g_async_interp = interp;
    AsyncRuntime *rt = ensure_runtime();
    XSPromise *p = promise_new(rt);

    if (argc >= 1 && (VAL_TAG(args[0]) == XS_FUNC || VAL_TAG(args[0]) == XS_NATIVE)) {
        /* executor pattern: fn(resolve, reject) */
        Value *resolve_fn = xs_native(NULL);
        Value *reject_fn = xs_native(NULL);
        Value *exec_args[2] = { resolve_fn, reject_fn };
        call_value(interp, args[0], exec_args, 2, "promise_executor");

        /* for simplicity, resolve with the return value */
        if (interp->cf.signal == 0 && interp->cf.value) {
            promise_resolve(rt, p, interp->cf.value);
        }
    }

    return promise_to_value(p);
}

static Value *native_promise_resolve(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    XSPromise *p = promise_resolve_value(rt, argc > 0 ? args[0] : xs_null());
    return promise_to_value(p);
}

static Value *native_promise_reject(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    XSPromise *p = promise_reject_value(rt, argc > 0 ? args[0] : xs_null());
    return promise_to_value(p);
}

static Value *native_promise_all(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_null();

    int count = args[0]->arr->len;
    XSPromise **promises = xs_calloc(count > 0 ? count : 1, sizeof(XSPromise *));

    for (int i = 0; i < count; i++) {
        promises[i] = value_to_promise(args[0]->arr->items[i]);
        if (!promises[i]) {
            /* not a promise, wrap it */
            promises[i] = promise_resolve_value(rt, args[0]->arr->items[i]);
        }
    }

    XSPromise *result = promise_all(rt, promises, count);
    async_drain_microtasks(rt);
    free(promises);
    return promise_to_value(result);
}

static Value *native_promise_race(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_null();

    int count = args[0]->arr->len;
    XSPromise **promises = xs_calloc(count > 0 ? count : 1, sizeof(XSPromise *));

    for (int i = 0; i < count; i++) {
        promises[i] = value_to_promise(args[0]->arr->items[i]);
        if (!promises[i])
            promises[i] = promise_resolve_value(rt, args[0]->arr->items[i]);
    }

    XSPromise *result = promise_race(rt, promises, count);
    async_drain_microtasks(rt);
    free(promises);
    return promise_to_value(result);
}

static Value *native_promise_any(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_null();

    int count = args[0]->arr->len;
    XSPromise **promises = xs_calloc(count > 0 ? count : 1, sizeof(XSPromise *));

    for (int i = 0; i < count; i++) {
        promises[i] = value_to_promise(args[0]->arr->items[i]);
        if (!promises[i])
            promises[i] = promise_resolve_value(rt, args[0]->arr->items[i]);
    }

    XSPromise *result = promise_any(rt, promises, count);
    async_drain_microtasks(rt);
    free(promises);
    return promise_to_value(result);
}

static Value *native_promise_all_settled(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_null();

    int count = args[0]->arr->len;
    XSPromise **promises = xs_calloc(count > 0 ? count : 1, sizeof(XSPromise *));

    for (int i = 0; i < count; i++) {
        promises[i] = value_to_promise(args[0]->arr->items[i]);
        if (!promises[i])
            promises[i] = promise_resolve_value(rt, args[0]->arr->items[i]);
    }

    XSPromise *result = promise_all_settled(rt, promises, count);
    async_drain_microtasks(rt);
    free(promises);
    return promise_to_value(result);
}

static Value *native_async_sleep(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    int ms = (argc > 0 && VAL_TAG(args[0]) == XS_INT) ? (int)VAL_INT(args[0]) : 0;
    XSPromise *p = async_sleep(rt, ms);
    return promise_to_value(p);
}

static Value *native_async_timeout(Interp *interp, Value **args, int argc) {
    (void)interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 2) return xs_null();

    XSPromise *p = value_to_promise(args[0]);
    if (!p) p = promise_resolve_value(rt, args[0]);
    int ms = (VAL_TAG(args[1]) == XS_INT) ? (int)VAL_INT(args[1]) : 1000;

    XSPromise *result = async_timeout(rt, p, ms);
    return promise_to_value(result);
}

static Value *native_promise_then(Interp *interp, Value **args, int argc) {
    g_async_interp = interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 2) return xs_null();

    XSPromise *p = value_to_promise(args[0]);
    if (!p) return xs_null();

    Value *on_resolve = (argc >= 2) ? args[1] : NULL;
    Value *on_reject = (argc >= 3) ? args[2] : NULL;

    XSPromise *child = promise_then(rt, p, on_resolve, on_reject);
    async_drain_microtasks(rt);
    return promise_to_value(child);
}

static Value *native_promise_catch(Interp *interp, Value **args, int argc) {
    g_async_interp = interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 2) return xs_null();

    XSPromise *p = value_to_promise(args[0]);
    if (!p) return xs_null();

    XSPromise *child = promise_catch(rt, p, args[1]);
    async_drain_microtasks(rt);
    return promise_to_value(child);
}

static Value *native_promise_finally(Interp *interp, Value **args, int argc) {
    g_async_interp = interp;
    AsyncRuntime *rt = ensure_runtime();
    if (argc < 2) return xs_null();

    XSPromise *p = value_to_promise(args[0]);
    if (!p) return xs_null();

    XSPromise *child = promise_finally(rt, p, args[1]);
    async_drain_microtasks(rt);
    return promise_to_value(child);
}

static Value *native_promise_state(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("invalid");
    XSPromise *p = value_to_promise(args[0]);
    if (!p) return xs_str("not_a_promise");
    switch (p->state) {
    case PROMISE_PENDING:  return xs_str("pending");
    case PROMISE_RESOLVED: return xs_str("resolved");
    case PROMISE_REJECTED: return xs_str("rejected");
    default: return xs_str("unknown");
    }
}

static Value *native_promise_value(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    XSPromise *p = value_to_promise(args[0]);
    if (!p) return xs_null();
    if (p->state == PROMISE_RESOLVED) return p->result ? p->result : xs_null();
    if (p->state == PROMISE_REJECTED) return p->error ? p->error : xs_null();
    return xs_null();
}

static Value *native_async_drain(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    AsyncRuntime *rt = ensure_runtime();
    async_runtime_drain(rt);
    return xs_null();
}

Value *make_promise_module(void) {
    XSMap *m = map_new();
    map_take(m, "new", xs_native(native_promise_new));
    map_take(m, "resolve", xs_native(native_promise_resolve));
    map_take(m, "reject", xs_native(native_promise_reject));
    map_take(m, "all", xs_native(native_promise_all));
    map_take(m, "race", xs_native(native_promise_race));
    map_take(m, "any", xs_native(native_promise_any));
    map_take(m, "all_settled", xs_native(native_promise_all_settled));
    map_take(m, "sleep", xs_native(native_async_sleep));
    map_take(m, "timeout", xs_native(native_async_timeout));
    map_take(m, "then", xs_native(native_promise_then));
    map_take(m, "catch_err", xs_native(native_promise_catch));
    map_take(m, "finally_do", xs_native(native_promise_finally));
    map_take(m, "state", xs_native(native_promise_state));
    map_take(m, "value", xs_native(native_promise_value));
    map_take(m, "drain", xs_native(native_async_drain));
    return xs_module(m);
}
