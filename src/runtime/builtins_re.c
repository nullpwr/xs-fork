#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "core/value.h"
#include "core/regex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The regex builtins now run on the in-tree Thompson NFA in
   src/core/regex.c (full PCRE shorthand: \d, \w, \s, \b plus
   non-greedy quantifiers, non-capturing groups, lookaheads). The
   previous shape was a `regex.h` POSIX wrapper with a hand-rolled
   `\d -> [0-9]` translator; that lost any feature POSIX itself
   doesn't ship (notably `\b` and the lookarounds), so we just
   call the real engine directly. */

static int compile_or_null(const char *pat, XSRegex *re) {
    return xs_regex_compile(re, pat, 0) == 0;
}

static Value *native_re_test(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_FALSE_VAL);
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return value_incref(XS_FALSE_VAL);
    XSMatch m;
    int ok = xs_regex_search(&re, a[1]->s, (int)strlen(a[1]->s), 0, &m) && m.matched;
    xs_regex_free(&re);
    return ok ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_re_match(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return value_incref(XS_NULL_VAL);
    XSMatch m;
    Value *res = value_incref(XS_NULL_VAL);
    int slen = (int)strlen(a[1]->s);
    if (xs_regex_search(&re, a[1]->s, slen, 0, &m) && m.matched) {
        value_decref(res);
        res = xs_str_n(a[1]->s + m.start, m.end - m.start);
    }
    xs_regex_free(&re);
    return res;
}

static Value *native_re_find_all(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return xs_array_new();
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return xs_array_new();
    Value *arr = xs_array_new();
    const char *s = a[1]->s;
    int slen = (int)strlen(s);
    int pos = 0;
    XSMatch m;
    while (pos <= slen && xs_regex_search(&re, s, slen, pos, &m) && m.matched) {
        array_push(arr->arr, xs_str_n(s + m.start, m.end - m.start));
        if (m.end == m.start) pos = m.end + 1;
        else                  pos = m.end;
    }
    xs_regex_free(&re);
    return arr;
}

static Value *native_re_replace(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 3 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR ||
        VAL_TAG(a[2]) != XS_STR)
        return n > 1 ? value_incref(a[1]) : xs_str("");
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return value_incref(a[1]);
    int slen = (int)strlen(a[1]->s);
    char *out = xs_regex_replace(&re, a[1]->s, slen, a[2]->s);
    xs_regex_free(&re);
    if (!out) return value_incref(a[1]);
    Value *v = xs_str(out);
    free(out);
    return v;
}

static Value *native_re_replace_all(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 3 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR ||
        VAL_TAG(a[2]) != XS_STR)
        return n > 1 ? value_incref(a[1]) : xs_str("");
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return value_incref(a[1]);
    int slen = (int)strlen(a[1]->s);
    char *out = xs_regex_replace_all(&re, a[1]->s, slen, a[2]->s);
    xs_regex_free(&re);
    if (!out) return value_incref(a[1]);
    Value *v = xs_str(out);
    free(out);
    return v;
}

static Value *native_re_split(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return xs_array_new();
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return xs_array_new();
    int slen = (int)strlen(a[1]->s);
    char **parts = NULL;
    int nparts = 0;
    Value *arr = xs_array_new();
    /* xs_regex_split returns the number of parts produced, not 0/-1. */
    xs_regex_split(&re, a[1]->s, slen, &parts, &nparts);
    if (parts) {
        for (int j = 0; j < nparts; j++) {
            array_push(arr->arr, xs_str(parts[j]));
            free(parts[j]);
        }
        free(parts);
    }
    xs_regex_free(&re);
    return arr;
}

static Value *native_re_groups(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return xs_array_new();
    XSRegex re;
    if (!compile_or_null(a[0]->s, &re)) return xs_array_new();
    int slen = (int)strlen(a[1]->s);
    XSMatch m;
    Value *arr = xs_array_new();
    if (xs_regex_search(&re, a[1]->s, slen, 0, &m) && m.matched) {
        for (int j = 1; j < m.ngroups; j++) {
            int gs = m.group_starts[j];
            int ge = m.group_ends[j];
            if (gs < 0 || ge < 0)
                array_push(arr->arr, value_incref(XS_NULL_VAL));
            else
                array_push(arr->arr, xs_str_n(a[1]->s + gs, ge - gs));
        }
    }
    xs_regex_free(&re);
    return arr;
}

Value *make_re_module(void) {
    XSMap *m = map_new();
    map_take(m, "test",        xs_native(native_re_test));
    map_take(m, "is_match",    xs_native(native_re_test));
    map_take(m, "match",       xs_native(native_re_match));
    map_take(m, "find_all",    xs_native(native_re_find_all));
    map_take(m, "replace",     xs_native(native_re_replace));
    map_take(m, "replace_all", xs_native(native_re_replace_all));
    map_take(m, "split",       xs_native(native_re_split));
    map_take(m, "groups",      xs_native(native_re_groups));
    return xs_module(m);
}
