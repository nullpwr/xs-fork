#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/gc.h"
#include "core/value.h"
#include <stdlib.h>
#include <stdio.h>

/* gc */

static Value *native_gc_collect(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    int freed = gc_collect();
    return xs_int(freed);
}

static Value *native_gc_disable(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    gc_disable();
    return value_incref(XS_NULL_VAL);
}

static Value *native_gc_enable(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    gc_enable();
    return value_incref(XS_NULL_VAL);
}

static Value *native_gc_stats(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    GCStats st = gc_get_stats();
    XSMap *s = map_new();
    map_take(s, "total_collected", xs_int(st.total_collected));
    map_take(s, "total_allocations", xs_int(st.total_allocations));
    map_take(s, "gen0_collections", xs_int(st.gen0_collections));
    map_take(s, "gen1_collections", xs_int(st.gen1_collections));
    map_take(s, "gen2_collections", xs_int(st.gen2_collections));
    map_take(s, "tracked", xs_int(gc_tracked_count()));
    map_take(s, "peak_tracked", xs_int(st.peak_tracked));
    map_set(s, "gc_time_ms",        xs_float(st.total_gc_time_ms));
    map_set(s, "strategy",          xs_str("generational-refcount"));
    return xs_module(s);
}

static Value *native_gc_set_threshold(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || !a[0] || !a[1]) return value_incref(XS_NULL_VAL);
    if (VAL_TAG(a[0]) != XS_INT || VAL_TAG(a[1]) != XS_INT) return value_incref(XS_NULL_VAL);
    gc_set_threshold((int)VAL_INT(a[0]), (int)VAL_INT(a[1]));
    return value_incref(XS_NULL_VAL);
}

static Value *native_gc_freeze(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || !a[0]) return value_incref(XS_NULL_VAL);
    gc_freeze(a[0]);
    return value_incref(XS_NULL_VAL);
}

static Value *native_gc_tracked(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    return xs_int(gc_tracked_count());
}
Value *make_gc_module(void) {
    gc_init();
    XSMap *m = map_new();
    map_take(m, "collect",       xs_native(native_gc_collect));
    map_take(m, "disable",       xs_native(native_gc_disable));
    map_take(m, "enable",        xs_native(native_gc_enable));
    map_take(m, "stats",         xs_native(native_gc_stats));
    map_take(m, "set_threshold", xs_native(native_gc_set_threshold));
    map_take(m, "freeze",        xs_native(native_gc_freeze));
    map_take(m, "tracked",       xs_native(native_gc_tracked));
    return xs_module(m);
}

