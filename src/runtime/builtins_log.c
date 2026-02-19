#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* log module */
static int xs_log_level = 1; /* default: info */
#define LOG_MSG(level_val, prefix) \
static Value *native_log_##prefix(Interp *ig, Value **a, int n) { \
    (void)ig; \
    if (xs_log_level > level_val) return value_incref(XS_NULL_VAL); \
    char *s=(n>0)?value_str(a[0]):xs_strdup(""); \
    fprintf(stderr,"[" #prefix "] %s\n",s); free(s); \
    return value_incref(XS_NULL_VAL); \
}
LOG_MSG(0, debug)
LOG_MSG(1, info)
LOG_MSG(2, warn)
LOG_MSG(3, error)
#undef LOG_MSG
static Value *native_log_fatal(Interp *ig, Value **a, int n) {
    (void)ig;
    char *s=(n>0)?value_str(a[0]):xs_strdup("fatal");
    fprintf(stderr,"[FATAL] %s\n",s); free(s);
    exit(1);
}
static Value *native_log_set_level(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>0&&VAL_TAG(a[0])==XS_INT) xs_log_level=(int)VAL_INT(a[0]);
    return value_incref(XS_NULL_VAL);
}
Value *make_log_module(void) {
    XSMap *m=map_new();
    map_take(m,"debug",     xs_native(native_log_debug));
    map_take(m,"info",      xs_native(native_log_info));
    map_take(m,"warn",      xs_native(native_log_warn));
    map_take(m,"error",     xs_native(native_log_error));
    map_take(m,"fatal",     xs_native(native_log_fatal));
    map_take(m,"set_level", xs_native(native_log_set_level));
    return xs_module(m);
}
