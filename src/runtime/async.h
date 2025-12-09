/* async.h - promise-based async runtime for XS */
#ifndef XS_ASYNC_H
#define XS_ASYNC_H

#include "core/xs.h"
#include "core/value.h"
#include "runtime/event_loop.h"

/* promise states */
#define PROMISE_PENDING   0
#define PROMISE_RESOLVED  1
#define PROMISE_REJECTED  2

/* callback entry for then/catch/finally */
typedef struct PromiseCallback {
    Value *on_resolve;
    Value *on_reject;
    struct XSPromise *child;
} PromiseCallback;

/* the promise object */
typedef struct XSPromise {
    int              state;
    Value           *result;
    Value           *error;
    PromiseCallback *callbacks;
    int              ncallbacks;
    int              cb_cap;
    int              id;
    int              refcount;
    int              handled;
    struct XSPromise *chain_parent;
} XSPromise;

/* async task for the microtask queue */
typedef struct MicroTask {
    void (*fn)(void *ctx);
    void  *ctx;
    struct MicroTask *next;
} MicroTask;

/* async runtime context */
typedef struct AsyncRuntime {
    XSPromise **promises;
    int         npromises;
    int         promise_cap;
    int         next_id;

    MicroTask  *microtask_head;
    MicroTask  *microtask_tail;

    EventLoop  *evloop;
    int         running;
    int         settled_count;
} AsyncRuntime;

/* runtime lifecycle */
AsyncRuntime *async_runtime_new(void);
void          async_runtime_free(AsyncRuntime *rt);
/* free the process-wide async runtime if one was lazily allocated. Call
   when an interpreter instance is being torn down so pending promises
   and their callback closures (which may reference the interpreter's
   env) do not outlive it. */
void          async_shutdown(void);
void          async_runtime_drain(AsyncRuntime *rt);
void          async_runtime_tick(AsyncRuntime *rt);
void          async_runtime_run_until_done(AsyncRuntime *rt);

/* microtask queue */
void  async_enqueue_microtask(AsyncRuntime *rt, void (*fn)(void *), void *ctx);
void  async_drain_microtasks(AsyncRuntime *rt);

/* promise lifecycle */
XSPromise *promise_new(AsyncRuntime *rt);
void       promise_free(XSPromise *p);
void       promise_resolve(AsyncRuntime *rt, XSPromise *p, Value *result);
void       promise_reject(AsyncRuntime *rt, XSPromise *p, Value *error);

/* promise chaining */
XSPromise *promise_then(AsyncRuntime *rt, XSPromise *p,
                        Value *on_resolve, Value *on_reject);
XSPromise *promise_catch(AsyncRuntime *rt, XSPromise *p,
                         Value *on_reject);
XSPromise *promise_finally(AsyncRuntime *rt, XSPromise *p,
                           Value *callback);

/* combinators */
XSPromise *promise_all(AsyncRuntime *rt, XSPromise **promises, int count);
XSPromise *promise_race(AsyncRuntime *rt, XSPromise **promises, int count);
XSPromise *promise_any(AsyncRuntime *rt, XSPromise **promises, int count);
XSPromise *promise_all_settled(AsyncRuntime *rt, XSPromise **promises, int count);

/* utility promises */
XSPromise *promise_resolve_value(AsyncRuntime *rt, Value *v);
XSPromise *promise_reject_value(AsyncRuntime *rt, Value *err);
XSPromise *async_sleep(AsyncRuntime *rt, int ms);
XSPromise *async_timeout(AsyncRuntime *rt, XSPromise *p, int ms);

/* wrap a promise as an XS value (stored as a struct) */
Value     *promise_to_value(XSPromise *p);
XSPromise *value_to_promise(Value *v);
int        value_is_promise(Value *v);

/* XS module factory */
Value     *make_promise_module(void);

#endif /* XS_ASYNC_H */
