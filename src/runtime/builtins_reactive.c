#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include "core/gc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* reactive primitives */
Value *builtin_signal(Interp *i, Value **args, int argc) {
    (void)i;
    XSSignal *sig = xs_calloc(1, sizeof(XSSignal));
    sig->value = (argc > 0) ? value_incref(args[0]) : value_incref(XS_NULL_VAL);
    sig->subscribers = NULL;
    sig->nsubs = 0;
    sig->subcap = 0;
    sig->compute = NULL;
    sig->notifying = 0;
    sig->refcount = 1;
    Value *v = xs_calloc(1, sizeof(Value));
    v->tag = XS_SIGNAL;
    v->refcount = 1;
    v->signal = sig;
    return v;
}
Value *builtin_derived(Interp *i, Value **args, int argc) {
    (void)i;
    XSSignal *sig = xs_calloc(1, sizeof(XSSignal));
    sig->value = value_incref(XS_NULL_VAL);
    sig->subscribers = NULL;
    sig->nsubs = 0;
    sig->subcap = 0;
    sig->compute = (argc > 0 && (VAL_TAG(args[0]) == XS_FUNC || VAL_TAG(args[0]) == XS_NATIVE))
                   ? value_incref(args[0]) : NULL;
    sig->notifying = 0;
    sig->refcount = 1;
    Value *v = xs_calloc(1, sizeof(Value));
    v->tag = XS_SIGNAL;
    v->refcount = 1;
    v->signal = sig;
    return v;
}

/* reactive signals */

static Value *native_reactive_signal(Interp *ig, Value **a, int n) {
    return builtin_signal(ig, a, n);
}

static Value *native_reactive_derived(Interp *ig, Value **a, int n) {
    return builtin_derived(ig, a, n);
}

static Value *native_reactive_effect(Interp *ig, Value **a, int n) {
    /* effect(fn, ...signals) -> calls fn immediately, then subscribes fn
       to each signal argument so it re-runs when signal values change */
    if (n < 1 || (VAL_TAG(a[0]) != XS_FUNC && VAL_TAG(a[0]) != XS_NATIVE))
        return value_incref(XS_NULL_VAL);
    Value *fn = a[0];
    /* Call the effect function immediately */
    Value *result = call_value(ig, fn, NULL, 0, "effect");
    value_decref(result);
    /* Subscribe fn to any signal arguments passed after the function */
    for (int j = 1; j < n; j++) {
        if (VAL_TAG(a[j]) == XS_SIGNAL && a[j]->signal) {
            XSSignal *sig = a[j]->signal;
            if (sig->nsubs >= sig->subcap) {
                sig->subcap = sig->subcap ? sig->subcap * 2 : 4;
                sig->subscribers = xs_realloc(sig->subscribers, sig->subcap * sizeof(Value*));
            }
            sig->subscribers[sig->nsubs++] = value_incref(fn);
        }
    }
    return value_incref(XS_NULL_VAL);
}

Value *make_reactive_module(void) {
    XSMap *m = map_new();
    map_take(m, "signal",  xs_native(native_reactive_signal));
    map_take(m, "derived", xs_native(native_reactive_derived));
    map_take(m, "effect",  xs_native(native_reactive_effect));
    return xs_module(m);
}
