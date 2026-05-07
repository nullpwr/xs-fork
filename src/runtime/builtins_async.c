#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/async.h"
#include "runtime/concurrent.h"
#include "runtime/error.h"
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
    if (n < 2) return value_incref(XS_NULL_VAL);
    if (!xs_chan_send(a[0], a[1])) {
        Value *err = xs_error_new("ChannelClosed",
            "send on closed channel", NULL);
        if (ig) {
            if (ig->cf.value) value_decref(ig->cf.value);
            ig->cf.signal = CF_THROW;
            ig->cf.value  = err;
        } else {
            value_decref(err);
        }
        return value_incref(XS_NULL_VAL);
    }
    return value_incref(XS_NULL_VAL);
}

Value *native_channel_recv(Interp *ig, Value **a, int n) {
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_recv(a[0], ig);
}

/* recv_pair: Go-style (value, ok) tuple. ok=false signals the channel
 * was closed and drained, removing the ambiguity between "received
 * null" and "channel done". A null value is considered a real send
 * if the channel still has buffered items or is still open. */
Value *native_channel_recv_pair(Interp *ig, Value **a, int n) {
    if (n < 1) return value_incref(XS_NULL_VAL);
    Value *v = xs_chan_recv(a[0], ig);
    int ok = 1;
    if (VAL_TAG(v) == XS_NULL
        && xs_chan_is_closed(a[0])
        && xs_chan_len(a[0]) == 0) {
        ok = 0;
    }
    Value *t = xs_tuple_new();
    array_push(t->arr, v);
    array_push(t->arr, ok ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
    return t;
}

Value *native_channel_try_recv(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_try_recv(a[0]);
}

Value *native_channel_close(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    xs_chan_close(a[0]);
    return value_incref(XS_NULL_VAL);
}

Value *native_channel_is_closed(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_FALSE_VAL);
    return xs_chan_is_closed(a[0]) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *native_channel_len(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_int(0);
    return xs_int(xs_chan_len(a[0]));
}

Value *native_channel_cap(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_int(0);
    return xs_int(xs_chan_cap(a[0]));
}

Value *native_channel_is_empty(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_TRUE_VAL);
    return xs_chan_len(a[0]) == 0 ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *native_channel_is_full(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_FALSE_VAL);
    return xs_chan_is_full(a[0]) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_async_channel(Interp *ig, Value **a, int n) {
    (void)ig;
    /* Concurrent channel: FIFO buffer + mutex/condvar slot allocated
       in a global table so the sync state survives across send/recv
       calls. recv blocks until data is available; sends wake all
       waiters. With a positive cap arg, send blocks while the buffer
       is full. */
    int cap = 0;
    if (n > 0 && a[0]) {
        if (VAL_TAG(a[0]) == XS_INT) cap = (int)VAL_INT(a[0]);
        else if (VAL_TAG(a[0]) == XS_FLOAT) cap = (int)a[0]->f;
    }
    int chid = xs_chan_alloc(cap);
    XSMap *ch = map_new();
    Value *buf = xs_array_new();
    map_set(ch, "_buf", buf);
    value_decref(buf);
    Value *idv = xs_int(chid);
    map_set(ch, "_chan_id", idv);
    value_decref(idv);
    Value *capv = xs_int(cap);
    map_set(ch, "_cap", capv);
    value_decref(capv);
    map_take(ch, "send", xs_native(native_channel_send));
    map_take(ch, "recv", xs_native(native_channel_recv));
    map_take(ch, "recv_pair", xs_native(native_channel_recv_pair));
    map_take(ch, "try_recv", xs_native(native_channel_try_recv));
    map_take(ch, "close", xs_native(native_channel_close));
    map_take(ch, "is_closed", xs_native(native_channel_is_closed));
    map_take(ch, "len", xs_native(native_channel_len));
    map_take(ch, "cap", xs_native(native_channel_cap));
    map_take(ch, "is_empty", xs_native(native_channel_is_empty));
    map_take(ch, "is_full", xs_native(native_channel_is_full));
    return xs_module(ch);
}

Value *native_async_select(Interp *ig, Value **a, int n) {
    (void)ig;
    /* select(channels_or_tasks): poll an array of channel/task-like
       values and return a map { index: <idx>, value: <result> } for the
       first one that has a ready result. Channel readiness goes through
       the channel mutex (so no lost wake-ups racing with a sender);
       task readiness still keys on the "_result" map field used by
       cooperative spawn/await. Returns null if nothing is ready. */
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY) return value_incref(XS_NULL_VAL);
    XSArray *arr = a[0]->arr;
    for (int i = 0; i < arr->len; i++) {
        Value *item = arr->items[i];
        if (!item || (VAL_TAG(item) != XS_MAP && VAL_TAG(item) != XS_MODULE) || !item->map)
            continue;
        Value *cid = map_get(item->map, "_chan_id");
        if (cid && VAL_TAG(cid) == XS_INT) {
            Value *got = NULL;
            int idx = xs_chan_select(&item, 1, &got);
            if (idx >= 0 && got) {
                Value *result = xs_map_new();
                Value *iv = xs_int(i);
                map_set(result->map, "index", iv); value_decref(iv);
                map_set(result->map, "value", got); value_decref(got);
                return result;
            }
            continue;
        }
        Value *res = map_get(item->map, "_result");
        if (res) {
            Value *result = xs_map_new();
            Value *iv = xs_int(i);
            map_set(result->map, "index", iv); value_decref(iv);
            map_set(result->map, "value", value_incref(res));
            return result;
        }
    }
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
