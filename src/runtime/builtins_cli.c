#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* cli arg parsing */

static Value *native_cli_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    /* parse(args_array) -> map of parsed flags/options */
    XSMap *result = map_new();
    Value *positionals = xs_array_new();
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY) {
        map_set(result, "args", positionals);
        value_decref(positionals);
        return xs_module(result);
    }
    XSArray *args = a[0]->arr;
    for (int i = 0; i < args->len; i++) {
        Value *arg = args->items[i];
        if (VAL_TAG(arg) != XS_STR) {
            array_push(positionals->arr, arg);
            continue;
        }
        const char *s = arg->s;
        if (s[0] == '-' && s[1] == '-') {
            /* --key=value or --flag */
            const char *eq = strchr(s + 2, '=');
            if (eq) {
                char *key = xs_strndup(s + 2, (size_t)(eq - s - 2));
                Value *val = xs_str(eq + 1);
                map_set(result, key, val);
                value_decref(val);
                free(key);
            } else {
                map_set(result, s + 2, value_incref(XS_TRUE_VAL));
            }
        } else if (s[0] == '-' && s[1] != '\0') {
            /* -f (short flag) */
            char key[2] = { s[1], '\0' };
            if (s[2] == '\0' && i + 1 < args->len && VAL_TAG(args->items[i+1]) == XS_STR
                && args->items[i+1]->s[0] != '-') {
                /* -k value */
                i++;
                Value *val = value_incref(args->items[i]);
                map_set(result, key, val);
                value_decref(val);
            } else {
                map_set(result, key, value_incref(XS_TRUE_VAL));
            }
        } else {
            array_push(positionals->arr, arg);
        }
    }
    map_set(result, "args", positionals);
    value_decref(positionals);
    return xs_module(result);
}

static Value *native_cli_flag(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *spec = map_new();
    map_set(spec, "name", value_incref(a[0]));
    map_set(spec, "type", xs_str("flag"));
    if (n > 1 && VAL_TAG(a[1]) == XS_MAP) {
        /* merge opts */
        int klen; char **keys = map_keys(a[1]->map, &klen);
        for (int i = 0; i < klen; i++) {
            Value *v = map_get(a[1]->map, keys[i]);
            if (v) map_set(spec, keys[i], value_incref(v));
            free(keys[i]);
        }
        free(keys);
    }
    return xs_module(spec);
}

static Value *native_cli_option(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *spec = map_new();
    map_set(spec, "name", value_incref(a[0]));
    map_set(spec, "type", xs_str("option"));
    return xs_module(spec);
}

static Value *native_cli_positional(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *spec = map_new();
    map_set(spec, "name", value_incref(a[0]));
    map_set(spec, "type", xs_str("positional"));
    return xs_module(spec);
}

Value *make_cli_module(void) {
    XSMap *m = map_new();
    map_take(m, "parse",      xs_native(native_cli_parse));
    map_take(m, "flag",       xs_native(native_cli_flag));
    map_take(m, "option",     xs_native(native_cli_option));
    map_take(m, "positional", xs_native(native_cli_positional));
    return xs_module(m);
}
