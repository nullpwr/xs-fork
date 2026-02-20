#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/async.h"
#include "runtime/concurrent.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* async module */

static Value *native_async_spawn(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Cooperative/synchronous semantics: call the function eagerly and
       wrap the result in a task map with _result / _status fields. */
    XSMap *task = map_new();
    if (n < 1 || (VAL_TAG(a[0]) != XS_NATIVE && VAL_TAG(a[0]) != XS_FUNC)) {
        map_set(task, "_status", xs_str("rejected"));
        map_set(task, "_error",  xs_str("spawn requires a callable"));
        return xs_module(task);
    }
    Value *result = call_value(ig, a[0], (n > 1 ? a + 1 : NULL), (n > 1 ? n - 1 : 0), "async.spawn");
    map_set(task, "_status", xs_str("resolved"));
    map_set(task, "_result", result);
    value_decref(result);
    return xs_module(task);
}

static Value *native_async_sleep(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    double secs = 0.0;
    if (VAL_TAG(a[0]) == XS_FLOAT) secs = a[0]->f;
    else if (VAL_TAG(a[0]) == XS_INT) secs = (double)VAL_INT(a[0]);
#if defined(__wasi__)
    (void)secs; /* no sleep in WASI */
#elif !defined(__MINGW32__)
    struct timespec ts;
    ts.tv_sec  = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#else
    /* Windows: Sleep() takes milliseconds */
    DWORD ms = (DWORD)(secs * 1000.0);
    if (ms > 0) Sleep(ms);
#endif
    return value_incref(XS_NULL_VAL);
}

Value *native_channel_send(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return value_incref(XS_NULL_VAL);
    return xs_chan_send(a[0], a[1]);
}

Value *native_channel_recv(Interp *ig, Value **a, int n) {
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_recv(a[0], ig);
}

Value *native_channel_try_recv(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_try_recv(a[0]);
}

Value *native_channel_len(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_int(0);
    return xs_int(xs_chan_len(a[0]));
}

Value *native_channel_is_empty(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_TRUE_VAL);
    return xs_chan_len(a[0]) == 0 ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *native_channel_is_full(Interp *ig, Value **a, int n) {
    /* The current channel implementation is unbounded, so a channel is
       never full. Kept for API compatibility with the previous version. */
    (void)ig; (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
}

static Value *native_async_channel(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    /* Concurrent channel: FIFO buffer + mutex/condvar slot allocated
       in a global table so the sync state survives across send/recv
       calls. recv blocks until data is available; sends wake all
       waiters. */
    int chid = xs_chan_alloc();
    XSMap *ch = map_new();
    Value *buf = xs_array_new();
    map_set(ch, "_buf", buf);
    value_decref(buf);
    Value *idv = xs_int(chid);
    map_set(ch, "_chan_id", idv);
    value_decref(idv);
    map_take(ch, "send", xs_native(native_channel_send));
    map_take(ch, "recv", xs_native(native_channel_recv));
    map_take(ch, "try_recv", xs_native(native_channel_try_recv));
    map_take(ch, "len", xs_native(native_channel_len));
    map_take(ch, "is_empty", xs_native(native_channel_is_empty));
    map_take(ch, "is_full", xs_native(native_channel_is_full));
    return xs_module(ch);
}

static Value *native_async_select(Interp *ig, Value **a, int n) {
    (void)ig;
    /* select(channels_or_tasks): poll an array of channel/task-like
       values and return a map { index: <idx>, value: <result> } for the
       first one that has a ready result.
       A channel is ready when its "_buf" array is non-empty.
       A task/promise is ready when it has a "_result" key.
       If nothing is ready, return null. */
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY) return value_incref(XS_NULL_VAL);
    XSArray *arr = a[0]->arr;
    for (int i = 0; i < arr->len; i++) {
        Value *item = arr->items[i];
        if ((VAL_TAG(item) == XS_MAP || VAL_TAG(item) == XS_MODULE) && item->map) {
            /* Check for channel readiness: "_buf" array with len > 0 */
            Value *buf = map_get(item->map, "_buf");
            if (buf && VAL_TAG(buf) == XS_ARRAY && buf->arr->len > 0) {
                /* Consume the first buffered value */
                Value *val = value_incref(buf->arr->items[0]);
                /* Shift the buffer: remove first element */
                for (int j = 0; j < buf->arr->len - 1; j++)
                    buf->arr->items[j] = buf->arr->items[j + 1];
                buf->arr->len--;
                XSMap *result = map_new();
                map_take(result, "index", xs_int(i));
                map_set(result, "value", val);
                value_decref(val);
                return xs_module(result);
            }
            /* Check for task/promise readiness: "_result" key present */
            Value *res = map_get(item->map, "_result");
            if (res) {
                XSMap *result = map_new();
                map_take(result, "index", xs_int(i));
                map_set(result, "value", value_incref(res));
                value_decref(res);
                return xs_module(result);
            }
        }
    }
    /* Nothing ready */
    return value_incref(XS_NULL_VAL);
}

static Value *native_async_all(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Collect _result from each task map into a results array.
       If the argument is not an array of task maps, return it as-is. */
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY) return xs_array_new();
    XSArray *tasks = a[0]->arr;
    Value *results = xs_array_new();
    for (int i = 0; i < tasks->len; i++) {
        Value *t = tasks->items[i];
        if ((VAL_TAG(t) == XS_MAP || VAL_TAG(t) == XS_MODULE) && t->map) {
            Value *r = map_get(t->map, "_result");
            if (r) {
                array_push(results->arr, r);
            } else {
                array_push(results->arr, XS_NULL_VAL);
            }
        } else {
            /* Not a task map: include the value itself */
            array_push(results->arr, t);
        }
    }
    return results;
}

static Value *native_async_race(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Return the _result of the first task in the array.
       Since we use cooperative semantics, all tasks are already resolved,
       so "first" is simply the first element. */
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY || a[0]->arr->len == 0)
        return value_incref(XS_NULL_VAL);
    Value *first = a[0]->arr->items[0];
    if ((VAL_TAG(first) == XS_MAP || VAL_TAG(first) == XS_MODULE) && first->map) {
        Value *r = map_get(first->map, "_result");
        if (r) return value_incref(r);
    }
    return value_incref(first);
}

static Value *native_async_resolve(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Create a resolved task/future with the given value */
    XSMap *task = map_new();
    map_set(task, "_status", xs_str("resolved"));
    if (n > 0) {
        map_set(task, "_result", value_incref(a[0]));
    } else {
        map_set(task, "_result", value_incref(XS_NULL_VAL));
    }
    return xs_module(task);
}

static Value *native_async_reject(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Create a rejected task/future with an error value */
    XSMap *task = map_new();
    map_set(task, "_status", xs_str("rejected"));
    if (n > 0) {
        map_set(task, "_error", value_incref(a[0]));
    } else {
        map_set(task, "_error", xs_str("rejected"));
    }
    return xs_module(task);
}

Value *make_async_module(void) {
    XSMap *m = map_new();
    map_take(m, "spawn",   xs_native(native_async_spawn));
    map_take(m, "sleep",   xs_native(native_async_sleep));
    map_take(m, "channel", xs_native(native_async_channel));
    map_take(m, "select",  xs_native(native_async_select));
    map_take(m, "all",     xs_native(native_async_all));
    map_take(m, "race",    xs_native(native_async_race));
    map_take(m, "resolve", xs_native(native_async_resolve));
    map_take(m, "reject",  xs_native(native_async_reject));
    return xs_module(m);
}
