#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/concurrent.h"
#include "runtime/error.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static Value *native_time_now(Interp *i, Value **args, int argc) {
    (void)i; (void)args; (void)argc;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return xs_float((double)ts.tv_sec + (double)ts.tv_nsec/1e9);
}

static Value *native_time_sleep(Interp *i, Value **args, int argc) {
    if (argc<1) return value_incref(XS_NULL_VAL);
    double secs;
    if (VAL_TAG(args[0]) == XS_DURATION) secs = (double)args[0]->i / 1e9;
    else if (VAL_TAG(args[0]) == XS_FLOAT) secs = args[0]->f;
    else secs = (double)VAL_INT(args[0]);
    xs_sleep_seconds(secs);
    if (xs_task_is_cancelled()) {
        Value *err = xs_error_new("Cancelled",
            "task cancelled by sibling failure", NULL);
        if (i) {
            if (i->cf.value) value_decref(i->cf.value);
            i->cf.signal = CF_THROW;
            i->cf.value  = err;
        } else if (!g_xs_pending_throw) {
            g_xs_pending_throw = err;
        } else {
            value_decref(err);
        }
    }
    return value_incref(XS_NULL_VAL);
}

static Value *native_time_stopwatch(Interp *i, Value **args, int argc) {
    (void)i; (void)args; (void)argc;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double start = (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
    Value *sw = xs_map_new();
    Value *sv = xs_float(start);
    map_set(sw->map, "_start", sv);
    value_decref(sv);
    return sw;
}

static Value *native_time_millis(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    return xs_int((int64_t)(ts.tv_sec*1000 + ts.tv_nsec/1000000));
}
#define TIME_COMPONENT(name, field) \
    static Value *native_time_##name(Interp *i, Value **a, int n) { \
        (void)i;(void)a;(void)n; \
        time_t t=time(NULL); struct tm *tm=localtime(&t); \
        return xs_int(tm->field); }
TIME_COMPONENT(year,   tm_year+1900)
TIME_COMPONENT(month,  tm_mon+1)
TIME_COMPONENT(day,    tm_mday)
TIME_COMPONENT(hour,   tm_hour)
TIME_COMPONENT(minute, tm_min)
TIME_COMPONENT(second, tm_sec)

static Value *native_time_sleep_ms(Interp *ig, Value **a, int n) {
    if (n<1) return value_incref(XS_NULL_VAL);
    int64_t ms;
    if (VAL_TAG(a[0]) == XS_DURATION) ms = a[0]->i / 1000000;
    else if (VAL_TAG(a[0]) == XS_INT) ms = VAL_INT(a[0]);
    else ms = (int64_t)a[0]->f;
    if (ms <= 0) return value_incref(XS_NULL_VAL);
    /* Goes through xs_sleep_seconds so the GIL drops while we wait,
       letting parallel spawn workers actually progress in parallel. */
    xs_sleep_seconds((double)ms / 1000.0);
    if (xs_task_is_cancelled()) {
        Value *err = xs_error_new("Cancelled",
            "task cancelled by sibling failure", NULL);
        if (ig) {
            if (ig->cf.value) value_decref(ig->cf.value);
            ig->cf.signal = CF_THROW;
            ig->cf.value  = err;
        } else if (!g_xs_pending_throw) {
            g_xs_pending_throw = err;
        } else {
            value_decref(err);
        }
    }
    return value_incref(XS_NULL_VAL);
}
static Value *native_time_format(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1) return xs_str("");
    time_t t=(time_t)((VAL_TAG(a[0])==XS_FLOAT)?a[0]->f:(double)VAL_INT(a[0]));
    const char *fmt=(n>=2&&VAL_TAG(a[1])==XS_STR)?a[1]->s:"%Y-%m-%d %H:%M:%S";
    struct tm *tm2=localtime(&t);
    char buf[256]; buf[0]='\0';
    strftime(buf,sizeof(buf),fmt,tm2);
    return xs_str(buf);
}
static Value *native_time_monotonic(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return xs_float((double)ts.tv_sec+(double)ts.tv_nsec/1e9);
}
static Value *native_time_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *input=a[0]->s;
    const char *fmt=(n>=2&&VAL_TAG(a[1])==XS_STR)?a[1]->s:NULL;
    struct tm tm2; memset(&tm2,0,sizeof(tm2));
    int parsed=0;
    if (fmt) {
        if (strptime(input,fmt,&tm2)) parsed=1;
    } else {
        if (strptime(input,"%Y-%m-%dT%H:%M:%S",&tm2)) parsed=1;
        else if (strptime(input,"%Y-%m-%d",&tm2)) parsed=1;
    }
    if (!parsed) return value_incref(XS_NULL_VAL);
    tm2.tm_isdst = -1;
    time_t t = mktime(&tm2);
    if (t == (time_t)-1) return value_incref(XS_NULL_VAL);
    return xs_int((int64_t)t);
}
static Value *native_time_now_ms(Interp *ig, Value **a, int n) {
    return native_time_millis(ig,a,n);
}
static Value *native_time_now_ns(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    return xs_int((int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec);
}
static Value *native_time_date(Interp *ig, Value **a, int n) {
    (void)ig;
    time_t t;
    if (n >= 1) {
        if (VAL_TAG(a[0]) == XS_INT) t = (time_t)VAL_INT(a[0]);
        else if (VAL_TAG(a[0]) == XS_FLOAT) t = (time_t)a[0]->f;
        else t = time(NULL);
    } else t = time(NULL);
    struct tm *tm2 = localtime(&t);
    Value *m = xs_map_new();
    Value *v;
    v = xs_int(tm2->tm_year+1900); map_set(m->map,"year",v);    value_decref(v);
    v = xs_int(tm2->tm_mon+1);     map_set(m->map,"month",v);   value_decref(v);
    v = xs_int(tm2->tm_mday);      map_set(m->map,"day",v);     value_decref(v);
    v = xs_int(tm2->tm_hour);      map_set(m->map,"hour",v);    value_decref(v);
    v = xs_int(tm2->tm_min);       map_set(m->map,"minute",v);  value_decref(v);
    v = xs_int(tm2->tm_sec);       map_set(m->map,"second",v);  value_decref(v);
    v = xs_int(tm2->tm_wday);      map_set(m->map,"weekday",v); value_decref(v);
    return m;
}
static Value *native_time_to_iso(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_str("");
    time_t t;
    if (VAL_TAG(a[0]) == XS_INT) t = (time_t)VAL_INT(a[0]);
    else if (VAL_TAG(a[0]) == XS_FLOAT) t = (time_t)a[0]->f;
    else return xs_str("");
    struct tm *tm2 = gmtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm2);
    return xs_str(buf);
}
static Value *native_time_from_iso(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    struct tm tm2; memset(&tm2, 0, sizeof(tm2));
    if (strptime(a[0]->s, "%Y-%m-%dT%H:%M:%S", &tm2)
        || strptime(a[0]->s, "%Y-%m-%d", &tm2)) {
        time_t t = mktime(&tm2);
        return xs_int((int64_t)t);
    }
    return value_incref(XS_NULL_VAL);
}

Value *make_time_module(void) {
    XSMap *m = map_new();
    map_take(m, "now",       xs_native(native_time_now));
    map_take(m, "now_ms",    xs_native(native_time_now_ms));
    map_take(m, "now_ns",    xs_native(native_time_now_ns));
    map_take(m, "sleep",     xs_native(native_time_sleep));
    map_take(m, "sleep_ms",  xs_native(native_time_sleep_ms));
    map_take(m, "stopwatch", xs_native(native_time_stopwatch));
    map_take(m, "millis",    xs_native(native_time_millis));
    map_take(m, "format",    xs_native(native_time_format));
    map_take(m, "monotonic", xs_native(native_time_monotonic));
    map_take(m, "clock",     xs_native(native_time_monotonic));
    map_take(m, "parse",     xs_native(native_time_parse));
    map_take(m, "date",      xs_native(native_time_date));
    map_take(m, "to_iso",    xs_native(native_time_to_iso));
    map_take(m, "from_iso",  xs_native(native_time_from_iso));
    map_take(m, "year",      xs_native(native_time_year));
    map_take(m, "month",     xs_native(native_time_month));
    map_take(m, "day",       xs_native(native_time_day));
    map_take(m, "hour",      xs_native(native_time_hour));
    map_take(m, "minute",    xs_native(native_time_minute));
    map_take(m, "second",    xs_native(native_time_second));
    return xs_module(m);
}
