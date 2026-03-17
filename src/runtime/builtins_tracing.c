/* tracing - structured logging and span-based tracing for XS.
 *
 * Surface:
 *   tracing.set_level(level)         "trace"|"debug"|"info"|"warn"|"error"
 *   tracing.add_sink(fn)             fn(record:Map) called per record
 *   tracing.remove_sinks()           clear all registered sinks
 *   tracing.console_sink()           pretty colourised text to stderr
 *   tracing.json_sink(path?)         one json line per record; path or stderr
 *   tracing.event(level, msg, attrs?)  emit a single event
 *   tracing.trace(msg, attrs?)       \
 *   tracing.debug(msg, attrs?)        |  shortcuts
 *   tracing.info(msg, attrs?)         |
 *   tracing.warn(msg, attrs?)         |
 *   tracing.error(msg, attrs?)       /
 *   tracing.start_span(name, attrs?) returns a span-handle (Map)
 *   tracing.end_span(handle, attrs?) closes the span, fires sinks
 *   tracing.with_span(name, fn)      runs fn inside a span; returns fn's
 *                                    result, end_span runs even if fn throws
 *
 * Records always carry: name, level, ts (epoch ns), kind ("event"|"span"),
 * and on span records also: span_id, parent_id, duration_ns. */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* ---- level filter ---- */

#define LVL_TRACE 0
#define LVL_DEBUG 1
#define LVL_INFO  2
#define LVL_WARN  3
#define LVL_ERROR 4

static int g_tracing_level = LVL_INFO;

static int level_for(const char *s) {
    if (!s) return LVL_INFO;
    if (!strcmp(s, "trace")) return LVL_TRACE;
    if (!strcmp(s, "debug")) return LVL_DEBUG;
    if (!strcmp(s, "info"))  return LVL_INFO;
    if (!strcmp(s, "warn"))  return LVL_WARN;
    if (!strcmp(s, "error")) return LVL_ERROR;
    return LVL_INFO;
}

static const char *name_for(int lvl) {
    switch (lvl) {
        case LVL_TRACE: return "trace";
        case LVL_DEBUG: return "debug";
        case LVL_INFO:  return "info";
        case LVL_WARN:  return "warn";
        case LVL_ERROR: return "error";
        default:        return "info";
    }
}

/* ---- sink registry ---- */

#define MAX_SINKS 16
static Value *g_sinks[MAX_SINKS];
static int    g_nsinks = 0;

/* ---- span stack (single-threaded for now; per-thread later) ---- */

#define MAX_SPAN_DEPTH 256
static int64_t g_span_ids[MAX_SPAN_DEPTH];
static int     g_span_depth = 0;
static int64_t g_next_span_id = 1;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* dispatch a record map to every sink */
static void dispatch(Interp *ig, Value *record) {
    for (int i = 0; i < g_nsinks; i++) {
        Value *sink = g_sinks[i];
        if (!sink) continue;
        Value *args[1] = { record };
        Value *r = call_value(ig, sink, args, 1, "tracing.sink");
        if (r) value_decref(r);
    }
}

static Value *native_tracing_set_level(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    g_tracing_level = level_for(a[0]->s);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_tracing_get_level(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    return xs_str(name_for(g_tracing_level));
}

static Value *native_tracing_add_sink(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_FALSE_VAL);
    int t = VAL_TAG(a[0]);
    if (t != XS_FUNC && t != XS_NATIVE && t != XS_CLOSURE) return value_incref(XS_FALSE_VAL);
    if (g_nsinks >= MAX_SINKS) return value_incref(XS_FALSE_VAL);
    g_sinks[g_nsinks++] = value_incref(a[0]);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_tracing_remove_sinks(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    for (int i = 0; i < g_nsinks; i++) {
        if (g_sinks[i]) value_decref(g_sinks[i]);
        g_sinks[i] = NULL;
    }
    g_nsinks = 0;
    return value_incref(XS_NULL_VAL);
}

/* attrs may be null/missing. If a Map, copy its entries onto the record.
   XSMap is an open-addressed hash table -- iterate up to cap, skip empty
   slots. */
static void copy_attrs(Value *attrs, Value *record) {
    if (!attrs || VAL_TAG(attrs) != XS_MAP || !attrs->map) return;
    XSMap *src = attrs->map;
    for (int i = 0; i < src->cap; i++) {
        if (src->keys[i]) {
            map_set(record->map, src->keys[i], src->vals[i]);
        }
    }
}

static Value *build_record(const char *kind, int level,
                           const char *msg, Value *attrs) {
    Value *rec = xs_map_new();
    Value *v;
    v = xs_str(kind);              map_set(rec->map, "kind", v);  value_decref(v);
    v = xs_str(name_for(level));   map_set(rec->map, "level", v); value_decref(v);
    v = xs_int(now_ns());          map_set(rec->map, "ts", v);    value_decref(v);
    if (msg) { v = xs_str(msg);    map_set(rec->map, "msg", v);   value_decref(v); }
    if (g_span_depth > 0) {
        v = xs_int(g_span_ids[g_span_depth - 1]);
        map_set(rec->map, "parent_id", v);
        value_decref(v);
    }
    copy_attrs(attrs, rec);
    return rec;
}

static Value *native_tracing_event(Interp *ig, Value **a, int n) {
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    int lvl = level_for(a[0]->s);
    if (lvl < g_tracing_level) return value_incref(XS_NULL_VAL);
    Value *attrs = (n >= 3) ? a[2] : NULL;
    Value *rec = build_record("event", lvl, a[1]->s, attrs);
    dispatch(ig, rec);
    value_decref(rec);
    return value_incref(XS_NULL_VAL);
}

#define LEVEL_HELPER(lname, lconst) \
    static Value *native_tracing_##lname(Interp *ig, Value **a, int n) { \
        if (lconst < g_tracing_level) return value_incref(XS_NULL_VAL); \
        if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL); \
        Value *attrs = (n >= 2) ? a[1] : NULL; \
        Value *rec = build_record("event", lconst, a[0]->s, attrs); \
        dispatch(ig, rec); \
        value_decref(rec); \
        return value_incref(XS_NULL_VAL); \
    }
LEVEL_HELPER(trace, LVL_TRACE)
LEVEL_HELPER(debug, LVL_DEBUG)
LEVEL_HELPER(info,  LVL_INFO)
LEVEL_HELPER(warn,  LVL_WARN)
LEVEL_HELPER(error, LVL_ERROR)
#undef LEVEL_HELPER

static Value *native_tracing_start_span(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    if (g_span_depth >= MAX_SPAN_DEPTH) return value_incref(XS_NULL_VAL);

    int64_t id = g_next_span_id++;
    int64_t parent = g_span_depth > 0 ? g_span_ids[g_span_depth - 1] : 0;
    g_span_ids[g_span_depth++] = id;

    Value *handle = xs_map_new();
    Value *v;
    v = xs_int(id);       map_set(handle->map, "span_id", v);   value_decref(v);
    v = xs_int(parent);   map_set(handle->map, "parent_id", v); value_decref(v);
    v = xs_str(a[0]->s);  map_set(handle->map, "name", v);      value_decref(v);
    v = xs_int(now_ns()); map_set(handle->map, "start_ns", v);  value_decref(v);
    if (n >= 2) copy_attrs(a[1], handle);
    return handle;
}

static Value *native_tracing_end_span(Interp *ig, Value **a, int n) {
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    Value *handle = a[0];
    Value *start_v = map_get(handle->map, "start_ns");
    if (!start_v || VAL_TAG(start_v) != XS_INT) return value_incref(XS_NULL_VAL);
    int64_t start = VAL_INT(start_v);
    int64_t dur = now_ns() - start;

    /* pop the span stack if this id is on top */
    Value *id_v = map_get(handle->map, "span_id");
    if (id_v && VAL_TAG(id_v) == XS_INT && g_span_depth > 0 &&
        g_span_ids[g_span_depth - 1] == VAL_INT(id_v)) {
        g_span_depth--;
    }

    Value *rec = xs_map_new();
    /* copy the handle's keys onto the record */
    XSMap *hm = handle->map;
    for (int i = 0; i < hm->cap; i++) {
        if (hm->keys[i]) map_set(rec->map, hm->keys[i], hm->vals[i]);
    }
    Value *v;
    v = xs_str("span");          map_set(rec->map, "kind", v);  value_decref(v);
    v = xs_str(name_for(LVL_INFO)); map_set(rec->map, "level", v); value_decref(v);
    v = xs_int(now_ns());        map_set(rec->map, "ts", v);    value_decref(v);
    v = xs_int(dur);             map_set(rec->map, "duration_ns", v); value_decref(v);
    if (n >= 2) copy_attrs(a[1], rec);

    dispatch(ig, rec);
    value_decref(rec);
    return value_incref(XS_NULL_VAL);
}

/* with_span(name, fn) -- runs fn() inside a span, always closes. */
static Value *native_tracing_with_span(Interp *ig, Value **a, int n) {
    if (n < 2 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    int t = VAL_TAG(a[1]);
    if (t != XS_FUNC && t != XS_NATIVE && t != XS_CLOSURE) return value_incref(XS_NULL_VAL);

    Value *handle = native_tracing_start_span(ig, a, 1);
    Value *r = call_value(ig, a[1], NULL, 0, "tracing.with_span");
    Value *eargs[1] = { handle };
    Value *e = native_tracing_end_span(ig, eargs, 1);
    if (e) value_decref(e);
    value_decref(handle);
    return r ? r : value_incref(XS_NULL_VAL);
}

/* ---- built-in console sink: pretty colourised text to stderr ---- */

static int color_supported(void) {
    const char *t = getenv("TERM");
    return t && strcmp(t, "dumb") != 0;
}

static const char *level_color(const char *lvl) {
    if (!strcmp(lvl, "error")) return "\033[31m";
    if (!strcmp(lvl, "warn"))  return "\033[33m";
    if (!strcmp(lvl, "info"))  return "\033[36m";
    if (!strcmp(lvl, "debug")) return "\033[90m";
    return "\033[0m";
}

static Value *native_tracing_console(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    Value *rec = a[0];
    Value *lvl = map_get(rec->map, "level");
    Value *msg = map_get(rec->map, "msg");
    Value *kind = map_get(rec->map, "kind");
    Value *name = map_get(rec->map, "name");
    Value *dur = map_get(rec->map, "duration_ns");

    int color = color_supported();
    const char *lvl_s = (lvl && VAL_TAG(lvl) == XS_STR) ? lvl->s : "info";
    const char *col = color ? level_color(lvl_s) : "";
    const char *rst = color ? "\033[0m" : "";

    if (kind && VAL_TAG(kind) == XS_STR && !strcmp(kind->s, "span")) {
        const char *nm = (name && VAL_TAG(name) == XS_STR) ? name->s : "?";
        int64_t d = (dur && VAL_TAG(dur) == XS_INT) ? VAL_INT(dur) : 0;
        fprintf(stderr, "%s[span]%s %s %.3fms\n", col, rst, nm, d / 1e6);
    } else {
        const char *m = (msg && VAL_TAG(msg) == XS_STR) ? msg->s : "";
        fprintf(stderr, "%s[%-5s]%s %s\n", col, lvl_s, rst, m);
    }
    return value_incref(XS_NULL_VAL);
}

/* json_sink: one json record per line. With `path` arg: append to that file. */

static FILE *g_json_fp = NULL;

static Value *native_tracing_json(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    FILE *fp = g_json_fp ? g_json_fp : stderr;
    /* serialize via value_repr for a quick-and-dirty json-ish line */
    char *s = value_repr(a[0]);
    fputs(s, fp);
    fputc('\n', fp);
    fflush(fp);
    free(s);
    return value_incref(XS_NULL_VAL);
}

static Value *native_tracing_json_sink_set_path(Interp *ig, Value **a, int n) {
    (void)ig;
    if (g_json_fp && g_json_fp != stderr) fclose(g_json_fp);
    g_json_fp = NULL;
    if (n >= 1 && VAL_TAG(a[0]) == XS_STR) {
        g_json_fp = fopen(a[0]->s, "a");
    }
    return value_incref(XS_NULL_VAL);
}

Value *make_tracing_module(void) {
    XSMap *m = map_new();
    map_take(m, "set_level",      xs_native(native_tracing_set_level));
    map_take(m, "get_level",      xs_native(native_tracing_get_level));
    map_take(m, "add_sink",       xs_native(native_tracing_add_sink));
    map_take(m, "remove_sinks",   xs_native(native_tracing_remove_sinks));
    map_take(m, "console_sink",   xs_native(native_tracing_console));
    map_take(m, "json_sink",      xs_native(native_tracing_json));
    map_take(m, "json_sink_path", xs_native(native_tracing_json_sink_set_path));
    map_take(m, "event",          xs_native(native_tracing_event));
    map_take(m, "trace",          xs_native(native_tracing_trace));
    map_take(m, "debug",          xs_native(native_tracing_debug));
    map_take(m, "info",           xs_native(native_tracing_info));
    map_take(m, "warn",           xs_native(native_tracing_warn));
    map_take(m, "error",          xs_native(native_tracing_error));
    map_take(m, "start_span",     xs_native(native_tracing_start_span));
    map_take(m, "end_span",       xs_native(native_tracing_end_span));
    map_take(m, "with_span",      xs_native(native_tracing_with_span));
    return xs_module(m);
}
