#define _POSIX_C_SOURCE 200112L
#include "vm/vm.h"
#include "core/value.h"
#include "core/xs_bigint.h"
#include "core/utf8.h"
#include "core/limits.h"
#include "runtime/builtins.h"
#include "runtime/error.h"
#include "optimizer/inline_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "core/xs_regex.h"

#define PUSH(v)  do { \
    if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm); \
    *vm->sp++ = (v); \
} while(0)
#define POP()    (*--vm->sp)
#define PEEK(n)  (vm->sp[-(n)-1])
#define FRAME    (&vm->frames[vm->frame_count - 1])
#define CL       (FRAME->closure_val->cl)
#define PROTO    (CL->proto)

static int vm_dispatch(VM *vm, int stop_frame);
static Value *vm_invoke(VM *vm, Value *fn, Value **args, int argc);
/* Thread-local so per-thread VMs can resolve callback fn-targets without
   trampling the parent's pointer; native callbacks (array.map, etc.)
   thread their work through whichever VM the current thread is running. */
_Thread_local VM *g_vm_for_invoke;

static Upvalue *upvalue_new_open(Value **slot) {
    Upvalue *u = xs_malloc(sizeof *u);
    u->ptr = slot; u->closed_val = NULL; u->is_open = 1; u->refcount = 0; u->next = NULL;
    return u;
}

void upvalue_close_all(Upvalue **list, Value **cutoff) {
    while (*list && (*list)->ptr >= cutoff) {
        Upvalue *u = *list;
        u->closed_val = value_incref(*u->ptr);
        u->ptr        = &u->closed_val;
        u->is_open    = 0;
        *list = u->next;
        /* The open list held one reference; decrement now that we're
           dropping it. If the last closure has already been freed (e.g.
           a struct methods map getting torn down before the frame
           returns), this is what actually frees the upvalue. */
        u->refcount--;
        if (u->refcount <= 0) {
            if (u->closed_val) value_decref(u->closed_val);
            free(u);
        }
    }
}

static Upvalue *capture_upvalue(VM *vm, Value **slot) {
    Upvalue **p = &vm->open_upvalues;
    while (*p && (*p)->ptr > slot) p = &(*p)->next;
    if (*p && (*p)->ptr == slot) return *p;
    Upvalue *u = upvalue_new_open(slot);
    /* Being in the open list counts as a reference. Closures bump this
       further in OP_MAKE_CLOSURE. Paired with the decrement in
       upvalue_close_all, so fresh upvalues never get freed while open. */
    u->refcount = 1;
    u->next = *p; *p = u;
    return u;
}

/* stdlib functions */

static Value *vm_println(Interp *interp, Value **args, int argc) {
    (void)interp;
    for (int i = 0; i < argc; i++) {
        char *s = value_str(args[i]);
        if (i) printf(" ");
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return xs_null();
}

static Value *vm_print(Interp *interp, Value **args, int argc) {
    (void)interp;
    for (int i = 0; i < argc; i++) {
        char *s = value_str(args[i]);
        if (i) printf(" ");
        printf("%s", s);
        free(s);
    }
    return xs_null();
}

static Value *vm_len(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    Value *v = args[0];
    if (VAL_TAG(v) == XS_ARRAY || VAL_TAG(v) == XS_TUPLE) return xs_int(v->arr->len);
    if (VAL_TAG(v) == XS_MAP)   return xs_int(v->map->len);
    if (VAL_TAG(v) == XS_STR)   return xs_int(utf8_strlen(v->s, (int)strlen(v->s)));
    if (VAL_TAG(v) == XS_RANGE && v->range) {
        int64_t n = v->range->end - v->range->start + (v->range->inclusive ? 1 : 0);
        return xs_int(n < 0 ? 0 : n);
    }
    return xs_int(0);
}

static Value *vm_str(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("");
    char *s = value_str(args[0]);
    Value *r = xs_str(s);
    free(s);
    return r;
}

static Value *vm_int_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    Value *v = args[0];
    if (VAL_TAG(v) == XS_INT)   return xs_int(VAL_INT(v));
    if (VAL_TAG(v) == XS_BOOL)  return xs_int(VAL_INT(v) ? 1 : 0);
    if (VAL_TAG(v) == XS_FLOAT) {
        /* NaN -> exposed LLONG_MIN under C cast. Reject explicitly. */
        if (isnan(v->f) || isinf(v->f)) {
            xs_runtime_error(span_zero(), "TypeError", NULL,
                             "int(): can't convert non-finite float");
            return value_incref(XS_NULL_VAL);
        }
        return xs_int((int64_t)v->f);
    }
    if (VAL_TAG(v) == XS_STR) {
        const char *s = v->s;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): empty string");
            return value_incref(XS_NULL_VAL);
        }
        char *end = NULL;
        errno = 0;
        long long iv = strtoll(s, &end, 0);
        if (end == s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): invalid literal: '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end && *end) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): trailing characters in '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        if (errno == ERANGE) {
            xs_runtime_error(span_zero(), "OverflowError", NULL,
                             "int(): value out of range: '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        return xs_int((int64_t)iv);
    }
    xs_runtime_error(span_zero(), "TypeError", NULL,
                     "int(): cannot convert from this type");
    return value_incref(XS_NULL_VAL);
}

static Value *vm_float_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_float(0.0);
    Value *v = args[0];
    if (VAL_TAG(v) == XS_INT)   return xs_float((double)VAL_INT(v));
    if (VAL_TAG(v) == XS_BOOL)  return xs_float(VAL_INT(v) ? 1.0 : 0.0);
    if (VAL_TAG(v) == XS_FLOAT) return xs_float(v->f);
    if (VAL_TAG(v) == XS_STR) {
        const char *s = v->s;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "float(): empty string");
            return value_incref(XS_NULL_VAL);
        }
        char *end = NULL;
        double d = strtod(s, &end);
        if (end == s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "float(): invalid literal: '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end && *end) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "float(): trailing characters in '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        return xs_float(d);
    }
    xs_runtime_error(span_zero(), "TypeError", NULL,
                     "float(): cannot convert from this type");
    return value_incref(XS_NULL_VAL);
}

static Value *vm_type(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("null");
    /* Index by ValueTag. Keep this aligned with src/core/xs.h so type(...)
       returns the same name as the interpreter's builtin_type. A closure
       is still a function from the user's perspective, so map it to "fn". */
    static const char *names[] = {
        "null","bool","int","int","float","str","char",
        "array","map","tuple","fn","native",
        "struct","enum","class","inst","range","signal","actor","re","module",
        "fn", /* overload */
        "fn", /* closure */
    };
    int tag = (int)VAL_TAG(args[0]);
    return xs_str(tag >= 0 && tag < (int)(sizeof names/sizeof *names) ? names[tag] : "?");
}

static Value *vm_range(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_array_new();
    int64_t start = 0, end = 0, step = 1;
    if (argc == 1) { end = VAL_TAG(args[0])==XS_INT ? VAL_INT(args[0]) : 0; }
    else { start = VAL_TAG(args[0])==XS_INT ? VAL_INT(args[0]) : 0;
           end = VAL_TAG(args[1])==XS_INT ? VAL_INT(args[1]) : 0; }
    if (argc >= 3 && VAL_TAG(args[2])==XS_INT) step = VAL_INT(args[2]);
    if (step == 0) {
        fprintf(stderr, "range: step cannot be zero\n");
        return xs_range(0, 0, 0);
    }
    return xs_range_step(start, end, 0, step);
}

static Value *vm_abs(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    if (VAL_TAG(args[0])==XS_INT) {
        int64_t v = VAL_INT(args[0]);
        if (v == INT64_MIN) {
            /* -INT64_MIN doesn't fit in int64; promote to bigint
               so abs() never sneakily returns a negative number. */
            XSBigInt *bi = bigint_from_i64(v);
            XSBigInt *neg = bigint_neg(bi);
            bigint_free(bi);
            return xs_bigint_val(neg);
        }
        return xs_int(v < 0 ? -v : v);
    }
    if (VAL_TAG(args[0])==XS_FLOAT) return xs_float(fabs(args[0]->f));
    if (VAL_TAG(args[0])==XS_BIGINT) {
        XSBigInt *bi = args[0]->bigint;
        if (bi->sign) return xs_bigint_val(bigint_neg(bi));
        return value_incref(args[0]);
    }
    return xs_int(0);
}

static Value *vm_min(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return argc > 0 ? value_incref(args[0]) : xs_null();
    return value_cmp(args[0],args[1]) <= 0 ? value_incref(args[0]) : value_incref(args[1]);
}

static Value *vm_max(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return argc > 0 ? value_incref(args[0]) : xs_null();
    return value_cmp(args[0],args[1]) >= 0 ? value_incref(args[0]) : value_incref(args[1]);
}

static Value *vm_sqrt(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_float(0.0);
    double v = VAL_TAG(args[0])==XS_INT ? (double)VAL_INT(args[0]) : args[0]->f;
    return xs_float(sqrt(v));
}

static Value *vm_pow(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_float(0.0);
    double a = VAL_TAG(args[0])==XS_INT ? (double)VAL_INT(args[0]) : args[0]->f;
    double b = VAL_TAG(args[1])==XS_INT ? (double)VAL_INT(args[1]) : args[1]->f;
    return xs_float(pow(a, b));
}

static Value *vm_floor(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    double v = VAL_TAG(args[0])==XS_INT ? (double)VAL_INT(args[0]) : args[0]->f;
    return xs_int((int64_t)floor(v));
}

static Value *vm_ceil(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    double v = VAL_TAG(args[0])==XS_INT ? (double)VAL_INT(args[0]) : args[0]->f;
    return xs_int((int64_t)ceil(v));
}

static Value *vm_round(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    double v = VAL_TAG(args[0])==XS_INT ? (double)VAL_INT(args[0]) : args[0]->f;
    return xs_int((int64_t)round(v));
}

static Value *vm_assert(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || !value_truthy(args[0])) {
        const char *msg = (argc >= 2 && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : "assertion failed";
        fprintf(stderr, "xs: %s\n", msg);
        exit(1);
    }
    return xs_null();
}

static Value *vm_assert_eq(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) {
        fprintf(stderr, "xs: assert_eq requires 2 arguments\n");
        exit(1);
    }
    int eq = value_equal(args[0], args[1]);
    if (!eq && argc == 2) {
        double a_d, b_d; int have_a = 1, have_b = 1;
        if (VAL_TAG(args[0]) == XS_FLOAT) a_d = args[0]->f;
        else if (VAL_TAG(args[0]) == XS_INT) a_d = (double)VAL_INT(args[0]);
        else have_a = 0;
        if (VAL_TAG(args[1]) == XS_FLOAT) b_d = args[1]->f;
        else if (VAL_TAG(args[1]) == XS_INT) b_d = (double)VAL_INT(args[1]);
        else have_b = 0;
        if (have_a && have_b &&
            (VAL_TAG(args[0]) == XS_FLOAT || VAL_TAG(args[1]) == XS_FLOAT) &&
            a_d == a_d && b_d == b_d) {
            double diff = a_d - b_d; if (diff < 0) diff = -diff;
            double scale = (a_d < 0 ? -a_d : a_d);
            double b_abs = (b_d < 0 ? -b_d : b_d);
            if (b_abs > scale) scale = b_abs;
            if (diff <= 1e-9 + 1e-9 * scale) eq = 1;
        }
    }
    if (!eq) {
        char *a = value_repr(args[0]);
        char *b = value_repr(args[1]);
        const char *msg = (argc >= 3 && VAL_TAG(args[2]) == XS_STR) ? args[2]->s : "";
        fprintf(stderr, "xs: assertion failed: assert_eq(%s, %s)%s%s\n",
                a, b, msg[0] ? ": " : "", msg);
        free(a); free(b);
        exit(1);
    }
    return xs_null();
}

static Value *vm_panic(Interp *interp, Value **args, int argc) {
    (void)interp;
    const char *msg = (argc >= 1 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : "panic";
    fprintf(stderr, "xs: panic: %s\n", msg);
    exit(1);
}

static Value *vm_exit_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    int code = (argc >= 1 && VAL_TAG(args[0]) == XS_INT) ? (int)VAL_INT(args[0]) : 0;
    exit(code);
}

static Value *vm_input(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc >= 1) { char *s = value_str(args[0]); printf("%s", s); free(s); fflush(stdout); }
    char buf[4096];
    if (fgets(buf, sizeof buf, stdin)) {
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        return xs_str(buf);
    }
    return xs_str("");
}

static Value *vm_bool_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_bool(0);
    return xs_bool(value_truthy(args[0]));
}

static Value *vm_repr(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("null");
    char *s = value_repr(args[0]);
    Value *r = xs_str(s); free(s); return r;
}

static Value *vm_typeof(Interp *interp, Value **args, int argc) {
    return vm_type(interp, args, argc);
}

static Value *vm_contains(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_bool(0);
    Value *col = args[0], *item = args[1];
    if (VAL_TAG(col) == XS_ARRAY || VAL_TAG(col) == XS_TUPLE) {
        for (int j = 0; j < col->arr->len; j++)
            if (value_equal(col->arr->items[j], item)) return xs_bool(1);
    } else if (VAL_TAG(col) == XS_STR && VAL_TAG(item) == XS_STR) {
        return xs_bool(strstr(col->s, item->s) != NULL);
    } else if (VAL_TAG(col) == XS_MAP && VAL_TAG(item) == XS_STR) {
        return xs_bool(map_get(col->map, item->s) != NULL);
    }
    return xs_bool(0);
}

static Value *vm_sorted(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_ARRAY && VAL_TAG(args[0]) != XS_TUPLE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->arr->len; j++)
        array_push(arr->arr, value_incref(args[0]->arr->items[j]));
    for (int j = 0; j < arr->arr->len-1; j++)
        for (int k = 0; k < arr->arr->len-1-j; k++)
            if (value_cmp(arr->arr->items[k], arr->arr->items[k+1]) > 0) {
                Value *tmp2 = arr->arr->items[k];
                arr->arr->items[k] = arr->arr->items[k+1];
                arr->arr->items[k+1] = tmp2;
            }
    return arr;
}

static Value *vm_reversed(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_ARRAY && VAL_TAG(args[0]) != XS_TUPLE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = args[0]->arr->len - 1; j >= 0; j--)
        array_push(arr->arr, value_incref(args[0]->arr->items[j]));
    return arr;
}

static Value *vm_enumerate(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_ARRAY && VAL_TAG(args[0]) != XS_TUPLE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->arr->len; j++) {
        Value *pair = xs_tuple_new();
        array_push(pair->arr, xs_int(j));
        array_push(pair->arr, value_incref(args[0]->arr->items[j]));
        array_push(arr->arr, pair);
    }
    return arr;
}

static Value *vm_zip(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_array_new();
    Value *a = args[0], *b = args[1];
    if ((VAL_TAG(a) != XS_ARRAY && VAL_TAG(a) != XS_TUPLE) ||
        (VAL_TAG(b) != XS_ARRAY && VAL_TAG(b) != XS_TUPLE)) return xs_array_new();
    Value *arr = xs_array_new();
    int n = a->arr->len < b->arr->len ? a->arr->len : b->arr->len;
    for (int j = 0; j < n; j++) {
        Value *pair = xs_tuple_new();
        array_push(pair->arr, value_incref(a->arr->items[j]));
        array_push(pair->arr, value_incref(b->arr->items[j]));
        array_push(arr->arr, pair);
    }
    return arr;
}

static Value *vm_sum(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_ARRAY && VAL_TAG(args[0]) != XS_TUPLE))
        return xs_int(0);
    int64_t si = 0; double sf = 0; int is_float = 0;
    for (int j = 0; j < args[0]->arr->len; j++) {
        Value *v = args[0]->arr->items[j];
        if (VAL_TAG(v) == XS_INT) si += VAL_INT(v);
        else if (VAL_TAG(v) == XS_FLOAT) { sf += v->f; is_float = 1; }
    }
    return is_float ? xs_float(sf + (double)si) : xs_int(si);
}

static Value *vm_map_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_array_new();
    Value *fn = args[0], *col = args[1];
    if ((VAL_TAG(col) != XS_ARRAY && VAL_TAG(col) != XS_TUPLE)) return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < col->arr->len; j++) {
        Value *elem = col->arr->items[j];
        Value *r = (VAL_TAG(fn) == XS_NATIVE)
            ? fn->native(NULL, &elem, 1)
            : (VAL_TAG(fn) == XS_CLOSURE ? vm_invoke(g_vm_for_invoke, fn, &elem, 1) : value_incref(elem));
        array_push(arr->arr, r ? r : value_incref(XS_NULL_VAL));
    }
    return arr;
}

static Value *vm_filter_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_array_new();
    Value *fn = args[0], *col = args[1];
    if ((VAL_TAG(col) != XS_ARRAY && VAL_TAG(col) != XS_TUPLE)) return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < col->arr->len; j++) {
        Value *elem = col->arr->items[j];
        Value *r = (VAL_TAG(fn) == XS_NATIVE)
            ? fn->native(NULL, &elem, 1)
            : (VAL_TAG(fn) == XS_CLOSURE ? vm_invoke(g_vm_for_invoke, fn, &elem, 1) : value_incref(XS_FALSE_VAL));
        if (value_truthy(r)) array_push(arr->arr, value_incref(elem));
        value_decref(r);
    }
    return arr;
}

static Value *vm_reduce_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2) return xs_null();
    Value *fn = args[0], *col = args[1];
    if ((VAL_TAG(col) != XS_ARRAY && VAL_TAG(col) != XS_TUPLE) || col->arr->len == 0)
        return argc >= 3 ? value_incref(args[2]) : xs_null();
    Value *acc = argc >= 3 ? value_incref(args[2]) : value_incref(col->arr->items[0]);
    int start = argc >= 3 ? 0 : 1;
    if (VAL_TAG(fn) == XS_NATIVE || VAL_TAG(fn) == XS_CLOSURE) {
        for (int j = start; j < col->arr->len; j++) {
            Value *pair[2] = {acc, col->arr->items[j]};
            Value *r = (VAL_TAG(fn) == XS_NATIVE)
                ? fn->native(NULL, pair, 2)
                : vm_invoke(g_vm_for_invoke, fn, pair, 2);
            value_decref(acc); acc = r ? r : xs_null();
        }
    }
    return acc;
}

static Value *vm_keys(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_MAP && VAL_TAG(args[0]) != XS_MODULE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->map->cap; j++)
        if (args[0]->map->keys[j])
            array_push(arr->arr, xs_str(args[0]->map->keys[j]));
    return arr;
}

static Value *vm_values(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_MAP && VAL_TAG(args[0]) != XS_MODULE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->map->cap; j++)
        if (args[0]->map->keys[j])
            array_push(arr->arr, value_incref(args[0]->map->vals[j]));
    return arr;
}

static Value *vm_entries(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || (VAL_TAG(args[0]) != XS_MAP && VAL_TAG(args[0]) != XS_MODULE))
        return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->map->cap; j++)
        if (args[0]->map->keys[j]) {
            Value *pair = xs_tuple_new();
            array_push(pair->arr, xs_str(args[0]->map->keys[j]));
            array_push(pair->arr, value_incref(args[0]->map->vals[j]));
            array_push(arr->arr, pair);
        }
    return arr;
}

static Value *vm_chars(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || VAL_TAG(args[0]) != XS_STR) return xs_array_new();
    Value *arr = xs_array_new();
    const char *s = args[0]->s;
    for (int j = 0; s[j]; j++) {
        char buf[2] = {s[j], 0};
        array_push(arr->arr, xs_str(buf));
    }
    return arr;
}

static Value *vm_flatten(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return xs_array_new();
    Value *arr = xs_array_new();
    for (int j = 0; j < args[0]->arr->len; j++) {
        Value *el = args[0]->arr->items[j];
        if (VAL_TAG(el) == XS_ARRAY)
            for (int k = 0; k < el->arr->len; k++)
                array_push(arr->arr, value_incref(el->arr->items[k]));
        else
            array_push(arr->arr, value_incref(el));
    }
    return arr;
}

static Value *vm_is_null(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && VAL_TAG(args[0]) == XS_NULL);
}

static Value *vm_copy(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    return value_copy(args[0]);
}

static Value *vm_Ok(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *r = xs_map_new();
    Value *tag = xs_str("Ok"); map_set(r->map, "_tag", tag); value_decref(tag);
    Value *val = argc >= 1 ? value_incref(args[0]) : xs_null();
    map_set(r->map, "_val", val); value_decref(val);
    return r;
}

static Value *vm_Err(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *r = xs_map_new();
    Value *tag = xs_str("Err"); map_set(r->map, "_tag", tag); value_decref(tag);
    Value *val = argc >= 1 ? value_incref(args[0]) : xs_null();
    map_set(r->map, "_val", val); value_decref(val);
    return r;
}

static Value *vm_Some(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *r = xs_map_new();
    Value *tag = xs_str("Some"); map_set(r->map, "_tag", tag); value_decref(tag);
    Value *val = argc >= 1 ? value_incref(args[0]) : xs_null();
    map_set(r->map, "_val", val); value_decref(val);
    return r;
}

static Value *vm_None_fn(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    Value *r = xs_map_new();
    Value *tag = xs_str("None"); map_set(r->map, "_tag", tag); value_decref(tag);
    return r;
}

/* Defined in builtins.c; reused so format() works the same in both
   backends without duplicating the spec parser. */
extern Value *builtin_format(Interp *i, Value **args, int argc);

static Value *vm_format(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("");
    if (VAL_TAG(args[0]) != XS_STR) { char *s = value_str(args[0]); Value *r = xs_str(s); free(s); return r; }
    return builtin_format(NULL, args, argc);
}

static Value *vm_todo(Interp *interp, Value **args, int argc) {
    (void)interp;
    const char *msg = (argc >= 1 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : "not yet implemented";
    fprintf(stderr, "xs: TODO: %s\n", msg);
    exit(1);
}

static Value *vm_unreachable(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    fprintf(stderr, "xs: Reached unreachable code\n");
    exit(1);
}

static Value *vm_vec(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *arr = xs_array_new();
    for (int j = 0; j < argc; j++) {
        array_push(arr->arr, value_incref(args[j]));
    }
    return arr;
}

static Value *vm_eprint(Interp *interp, Value **args, int argc) {
    (void)interp;
    for (int j = 0; j < argc; j++) {
        if (j) fprintf(stderr, " ");
        char *s = value_str(args[j]);
        fprintf(stderr, "%s", s); free(s);
    }
    return xs_null();
}

static Value *vm_eprintln(Interp *interp, Value **args, int argc) {
    (void)interp;
    for (int j = 0; j < argc; j++) {
        if (j) fprintf(stderr, " ");
        char *s = value_str(args[j]);
        fprintf(stderr, "%s", s); free(s);
    }
    fprintf(stderr, "\n");
    return xs_null();
}

static Value *vm_sin(Interp *i, Value **a, int n) { (void)i; return n>=1 ? xs_float(sin(VAL_TAG(a[0])==XS_INT?(double)VAL_INT(a[0]):a[0]->f)) : xs_float(0); }
static Value *vm_cos(Interp *i, Value **a, int n) { (void)i; return n>=1 ? xs_float(cos(VAL_TAG(a[0])==XS_INT?(double)VAL_INT(a[0]):a[0]->f)) : xs_float(0); }
static Value *vm_tan(Interp *i, Value **a, int n) { (void)i; return n>=1 ? xs_float(tan(VAL_TAG(a[0])==XS_INT?(double)VAL_INT(a[0]):a[0]->f)) : xs_float(0); }
static Value *vm_log_fn(Interp *i, Value **a, int n) { (void)i; return n>=1 ? xs_float(log(VAL_TAG(a[0])==XS_INT?(double)VAL_INT(a[0]):a[0]->f)) : xs_float(0); }

/* plugin loading */
static _Thread_local VM *g_plugin_vm;

static Value *vm_plugin_global_set(Interp *interp, Value **args, int argc) {
    (void)interp;
    /* called as method: global.set(name, fn) → args = [self, name, fn] */
    if (argc >= 3 && VAL_TAG(args[1]) == XS_STR && g_plugin_vm) {
        map_set(g_plugin_vm->globals, args[1]->s, args[2]);
    } else if (argc >= 2 && VAL_TAG(args[0]) == XS_STR && g_plugin_vm) {
        map_set(g_plugin_vm->globals, args[0]->s, args[1]);
    }
    return xs_null();
}

static Value *vm_plugin_add_method(Interp *interp, Value **args, int argc) {
    (void)interp;
    /* called as method: runtime.add_method(type, name, fn) → args = [self, type, name, fn] */
    int off = (argc >= 4 && VAL_TAG(args[0]) == XS_MAP) ? 1 : 0;
    if (argc >= 3 + off && VAL_TAG(args[off]) == XS_STR && VAL_TAG(args[off+1]) == XS_STR && g_plugin_vm) {
        Value *pmethods = map_get(g_plugin_vm->globals, "__plugin_methods");
        if (!pmethods) {
            pmethods = xs_map_new();
            map_set(g_plugin_vm->globals, "__plugin_methods", pmethods);
            value_decref(pmethods);
            pmethods = map_get(g_plugin_vm->globals, "__plugin_methods");
        }
        Value *type_methods = map_get(pmethods->map, args[off]->s);
        if (!type_methods) {
            type_methods = xs_map_new();
            map_set(pmethods->map, args[off]->s, type_methods);
            value_decref(type_methods);
            type_methods = map_get(pmethods->map, args[off]->s);
        }
        map_set(type_methods->map, args[off+1]->s, args[off+2]);
    }
    return xs_null();
}

extern XSProto *compile_program(Node *program);

#include "core/lexer.h"
#include "core/parser.h"

static Value *vm_load_plugin(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || VAL_TAG(args[0]) != XS_STR || !g_plugin_vm) return xs_null();

    const char *path = args[0]->s;
    FILE *f = fopen(path, "r");
    /* try relative to source file directory */
    char resolved[1024];
    if (!f) {
        Value *src_file = map_get(g_plugin_vm->globals, "__source_file");
        if (src_file && VAL_TAG(src_file) == XS_STR) {
            const char *dir_end = strrchr(src_file->s, '/');
            if (!dir_end) dir_end = strrchr(src_file->s, '\\');
            if (dir_end) {
                int dir_len = (int)(dir_end - src_file->s + 1);
                snprintf(resolved, sizeof resolved, "%.*s%s", dir_len, src_file->s, path);
                f = fopen(resolved, "r");
                if (f) path = resolved;
            }
        }
    }
    /* try tests/ prefix */
    if (!f) {
        snprintf(resolved, sizeof resolved, "tests/%.*s",
                 (int)(sizeof resolved - 8), path);
        f = fopen(resolved, "r");
        if (f) path = resolved;
    }
    if (!f) { fprintf(stderr, "plugin not found: %s\n", args[0]->s); return xs_null(); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = xs_malloc((size_t)len + 1);
    size_t got = fread(src, 1, (size_t)len, f);
    src[got] = '\0';
    fclose(f);

    Lexer lex; lexer_init(&lex, src, path);
    TokenArray ta = lexer_tokenize(&lex);
    Parser psr; parser_init(&psr, &ta, path);
    Node *prog = parser_parse(&psr);
    token_array_free(&ta);
    comment_list_free(&lex.comments);
    free(src);
    if (!prog) return xs_null();

    /* set up plugin object in globals */
    Value *plugin = xs_map_new();
    Value *runtime = xs_map_new();
    Value *global_obj = xs_map_new();
    { Value *v = xs_native(vm_plugin_global_set); map_set(global_obj->map, "set", v); value_decref(v); }
    { Value *v = xs_str("plugin_global"); map_set(global_obj->map, "__type", v); value_decref(v); }
    map_set(runtime->map, "global", global_obj); value_decref(global_obj);
    { Value *v = xs_native(vm_plugin_add_method); map_set(runtime->map, "add_method", v); value_decref(v); }
    map_set(plugin->map, "runtime", runtime); value_decref(runtime);
    Value *meta = xs_map_new();
    map_set(plugin->map, "meta", meta); value_decref(meta);
    map_set(g_plugin_vm->globals, "plugin", plugin); value_decref(plugin);

    XSProto *proto = compile_program(prog);
    node_free(prog);

    /* create a temporary VM for plugin execution, sharing globals */
    VM *plugin_vm = xs_malloc(sizeof(VM));
    memset(plugin_vm, 0, sizeof(VM));
    plugin_vm->sp = plugin_vm->stack;
    plugin_vm->globals = g_plugin_vm->globals;  /* shared globals */
    VM *saved_main_vm = g_plugin_vm;
    VM *saved_invoke = g_vm_for_invoke;
    /* g_plugin_vm stays pointing to main VM for global.set */
    g_vm_for_invoke = plugin_vm;
    vm_run(plugin_vm, proto);
    g_vm_for_invoke = saved_invoke;
    g_plugin_vm = saved_main_vm;
    /* don't free globals: they're shared */
    plugin_vm->globals = NULL;
    /* cleanup plugin_vm manually */
    while (plugin_vm->sp > plugin_vm->stack) value_decref(*--plugin_vm->sp);
    free(plugin_vm);
    proto_free(proto);

    return xs_null();
}

static Value *vm_channel(Interp *interp, Value **args, int argc) {
    (void)interp;
    extern int xs_chan_alloc(int cap);
    Value *ch = xs_map_new();
    Value *type = xs_str("channel");
    map_set(ch->map, "__type", type); value_decref(type);
    Value *buf = xs_array_new();
    map_set(ch->map, "_buf", buf); value_decref(buf);
    int cap = 0;
    if (argc >= 1 && args[0]) {
        if (VAL_TAG(args[0]) == XS_INT) cap = (int)VAL_INT(args[0]);
        else if (VAL_TAG(args[0]) == XS_FLOAT) cap = (int)args[0]->f;
    }
    int id = xs_chan_alloc(cap);
    Value *id_v = xs_int(id);
    map_set(ch->map, "_chan_id", id_v); value_decref(id_v);
    Value *cap_v = xs_int(cap);
    map_set(ch->map, "_cap", cap_v); value_decref(cap_v);
    return ch;
}

static Value *vm_is_int(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && (VAL_TAG(args[0]) == XS_INT || VAL_TAG(args[0]) == XS_BIGINT));
}
static Value *vm_is_float(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && VAL_TAG(args[0]) == XS_FLOAT);
}
static Value *vm_is_str(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && VAL_TAG(args[0]) == XS_STR);
}
static Value *vm_is_bool(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && VAL_TAG(args[0]) == XS_BOOL);
}
static Value *vm_is_array(Interp *interp, Value **args, int argc) {
    (void)interp;
    return xs_bool(argc >= 1 && VAL_TAG(args[0]) == XS_ARRAY);
}
static Value *vm_is_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_bool(0);
    ValueTag t = VAL_TAG(args[0]);
    return xs_bool(t == XS_FUNC || t == XS_NATIVE || t == XS_CLOSURE);
}
static Value *vm_char_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_str("");
    Value *v = args[0];
    if (VAL_TAG(v) == XS_INT) {
        char buf[2] = { (char)VAL_INT(v), '\0' };
        return xs_str(buf);
    }
    if (VAL_TAG(v) == XS_STR && v->s[0]) {
        char buf[2] = { v->s[0], '\0' };
        return xs_str(buf);
    }
    return xs_str("");
}
static Value *vm_ord(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    Value *v = args[0];
    if (VAL_TAG(v) == XS_STR) return xs_int((int64_t)(unsigned char)v->s[0]);
    if (VAL_TAG(v) == XS_INT) return xs_int(VAL_INT(v));
    return xs_int(0);
}
static Value *vm_bytes(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *arr = xs_array_new();
    if (argc < 1) return arr;
    Value *v = args[0];
    if (VAL_TAG(v) == XS_STR) {
        for (const unsigned char *p = (const unsigned char *)v->s; *p; p++) {
            Value *b = xs_int((int64_t)*p);
            array_push(arr->arr, b);
        }
    }
    return arr;
}
static Value *vm_array_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    Value *arr = xs_array_new();
    for (int j = 0; j < argc; j++)
        array_push(arr->arr, value_incref(args[j]));
    return arr;
}
static Value *vm_print_no_nl(Interp *interp, Value **args, int argc) {
    (void)interp;
    for (int j = 0; j < argc; j++) {
        if (j) printf(" ");
        char *s = value_str(args[j]);
        printf("%s", s); free(s);
    }
    fflush(stdout);
    return xs_null();
}
static Value *vm_pprint(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) { printf("null\n"); return xs_null(); }
    char *s = value_repr(args[0]);
    printf("%s\n", s); free(s);
    return xs_null();
}
static Value *vm_clear(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    printf("\033[2J\033[H");
    fflush(stdout);
    return xs_null();
}
static Value *vm_signal_fn(Interp *interp, Value **args, int argc) {
    (void)interp;
    XSSignal *sig = xs_calloc(1, sizeof(XSSignal));
    sig->value = (argc >= 1) ? value_incref(args[0]) : xs_null();
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
static Value *vm_derived(Interp *interp, Value **args, int argc) {
    (void)interp;
    XSSignal *sig = xs_calloc(1, sizeof(XSSignal));
    sig->value = xs_null();
    sig->subscribers = NULL;
    sig->nsubs = 0;
    sig->subcap = 0;
    sig->compute = (argc >= 1) ? value_incref(args[0]) : NULL;
    sig->notifying = 0;
    sig->refcount = 1;
    Value *v = xs_calloc(1, sizeof(Value));
    v->tag = XS_SIGNAL;
    v->refcount = 1;
    v->signal = sig;
    return v;
}

static void vm_register_stdlib(VM *vm) {
    Value *v;
#define REG(name, fn) v = xs_native(fn); map_set(vm->globals, name, v); value_decref(v)
    REG("println", vm_println);
    REG("print",   vm_print);
    REG("len",     vm_len);
    REG("str",     vm_str);
    REG("int",     vm_int_fn);
    REG("float",   vm_float_fn);
    REG("type",    vm_type);
    REG("type_of", vm_typeof);
    REG("typeof",  vm_typeof);
    REG("bool",    vm_bool_fn);
    REG("repr",    vm_repr);
    REG("dbg",     vm_repr);
    REG("range",   vm_range);
    REG("abs",     vm_abs);
    REG("min",     vm_min);
    REG("max",     vm_max);
    REG("sqrt",    vm_sqrt);
    REG("pow",     vm_pow);
    REG("floor",   vm_floor);
    REG("ceil",    vm_ceil);
    REG("round",   vm_round);
    REG("sin",     vm_sin);
    REG("cos",     vm_cos);
    REG("tan",     vm_tan);
    REG("log",     vm_log_fn);
    REG("assert",     vm_assert);
    REG("assert_eq",  vm_assert_eq);
    REG("panic",      vm_panic);
    REG("exit",    vm_exit_fn);
    REG("input",   vm_input);
    REG("contains", vm_contains);
    REG("sorted",  vm_sorted);
    REG("reversed", vm_reversed);
    REG("enumerate", vm_enumerate);
    REG("zip",     vm_zip);
    REG("sum",     vm_sum);
    REG("map",     vm_map_fn);
    REG("filter",  vm_filter_fn);
    REG("reduce",  vm_reduce_fn);
    REG("keys",    vm_keys);
    REG("values",  vm_values);
    REG("entries", vm_entries);
    REG("chars",   vm_chars);
    REG("flatten", vm_flatten);
    REG("is_null", vm_is_null);
    REG("copy",    vm_copy);
    REG("clone",   vm_copy);
    REG("Ok",      vm_Ok);
    REG("Err",     vm_Err);
    REG("Some",    vm_Some);
    REG("None",    vm_None_fn);
    REG("format",  vm_format);
    REG("sprintf", vm_format);
    REG("todo",    vm_todo);
    REG("unreachable", vm_unreachable);
    REG("vec",     vm_vec);
    REG("eprint",  vm_eprint);
    REG("eprintln", vm_eprintln);
    REG("channel", vm_channel);
    {
        extern Value *native_async_select(Interp *, Value **, int);
        REG("select",  native_async_select);
    }
    REG("__load_plugin", vm_load_plugin);
    REG("is_int",      vm_is_int);
    REG("is_float",    vm_is_float);
    REG("is_str",      vm_is_str);
    REG("is_bool",     vm_is_bool);
    REG("is_array",    vm_is_array);
    REG("is_fn",       vm_is_fn);
    REG("i64",         vm_int_fn);
    REG("f64",         vm_float_fn);
    REG("char",        vm_char_fn);
    REG("chr",         vm_char_fn);
    REG("ord",         vm_ord);
    REG("bytes",       vm_bytes);
    REG("array",       vm_array_fn);
    REG("print_no_nl", vm_print_no_nl);
    REG("pprint",      vm_pprint);
    REG("clear",       vm_clear);
    REG("signal",      vm_signal_fn);
    REG("derived",     vm_derived);
#undef REG
    {
        extern Value *make_math_module(void);
        extern Value *make_time_module(void);
        extern Value *make_string_module(void);
        extern Value *make_path_module(void);
        extern Value *make_base64_module(void);
        extern Value *make_hash_module(void);
        extern Value *make_uuid_module(void);
        extern Value *make_collections_module(void);
        extern Value *make_random_module(void);
        extern Value *make_json_module(void);
        extern Value *make_log_module(void);
        extern Value *make_fmt_module(void);
        extern Value *make_csv_module(void);
        extern Value *make_url_module(void);
        extern Value *make_re_module(void);
        extern Value *make_process_module(void);
        extern Value *make_io_module(void);
        extern Value *make_async_module(void);
        extern Value *make_net_module(void);
        extern Value *make_crypto_module(void);
        extern Value *make_thread_module(void);
        extern Value *make_buf_module(void);
        extern Value *make_encode_module(void);
        extern Value *make_db_module(void);
        extern Value *make_cli_module(void);
        extern Value *make_ffi_module(void);
        extern Value *make_reflect_module(void);
        extern Value *make_gc_module(void);
        extern Value *make_reactive_module(void);
        extern Value *make_os_module(Interp *ig);
        extern Value *make_test_module(void);
        extern Value *make_http_module(void);
        extern Value *make_fs_module(void);
        extern Value *make_toml_module(void);
        extern Value *make_msgpack_module(void);
        extern Value *make_promise_module(void);
#define REG_MOD(name, fn) do { Value *_m = fn(); map_set(vm->globals, name, _m); value_decref(_m); } while(0)
        REG_MOD("math",        make_math_module);
        REG_MOD("time",        make_time_module);
        REG_MOD("string",      make_string_module);
        REG_MOD("path",        make_path_module);
        REG_MOD("base64",      make_base64_module);
        REG_MOD("hash",        make_hash_module);
        REG_MOD("uuid",        make_uuid_module);
        REG_MOD("collections", make_collections_module);
        REG_MOD("random",      make_random_module);
        REG_MOD("json",        make_json_module);
        REG_MOD("log",         make_log_module);
        REG_MOD("fmt",         make_fmt_module);
        REG_MOD("csv",         make_csv_module);
        REG_MOD("url",         make_url_module);
        REG_MOD("re",          make_re_module);
        REG_MOD("process",     make_process_module);
        { Value *_m = make_io_module();
          extern Value *native_io_read_json(Interp*,Value**,int);
          extern Value *native_io_write_json(Interp*,Value**,int);
          map_take(_m->map, "read_json", xs_native(native_io_read_json));
          map_take(_m->map, "write_json", xs_native(native_io_write_json));
          map_set(vm->globals, "io", _m); value_decref(_m); }
        REG_MOD("async",       make_async_module);
        REG_MOD("net",         make_net_module);
        REG_MOD("crypto",      make_crypto_module);
        REG_MOD("thread",      make_thread_module);
        REG_MOD("buf",         make_buf_module);
        REG_MOD("encode",      make_encode_module);
        REG_MOD("db",          make_db_module);
        REG_MOD("cli",         make_cli_module);
        REG_MOD("ffi",         make_ffi_module);
        REG_MOD("reflect",     make_reflect_module);
        REG_MOD("gc",          make_gc_module);
        REG_MOD("reactive",    make_reactive_module);
        REG_MOD("test",        make_test_module);
        REG_MOD("http",        make_http_module);
        REG_MOD("fs",          make_fs_module);
        REG_MOD("toml",        make_toml_module);
        REG_MOD("msgpack",     make_msgpack_module);
        REG_MOD("Promise",     make_promise_module);
#undef REG_MOD
        { Value *_m = make_os_module(NULL); map_set(vm->globals, "os", _m); value_decref(_m); }
        { Value *cv = xs_float(3.14159265358979323846); map_set(vm->globals, "PI", cv); value_decref(cv); }
        { Value *cv = xs_float(2.71828182845904523536); map_set(vm->globals, "E",  cv); value_decref(cv); }
    }
}

VM *vm_new(void) {
    value_init_singletons();
    VM *vm = xs_malloc(sizeof *vm);
    memset(vm, 0, sizeof *vm);
    vm->stack_cap  = VM_STACK_INIT;
    vm->stack      = xs_malloc(vm->stack_cap * sizeof(Value *));
    memset(vm->stack, 0, vm->stack_cap * sizeof(Value *));
    vm->sp         = vm->stack;
    vm->stack_soft_limit = vm->stack + (vm->stack_cap - 16);
    vm->frames_cap = VM_FRAMES_INIT;
    vm->frames     = xs_malloc(vm->frames_cap * sizeof(CallFrame));
    memset(vm->frames, 0, vm->frames_cap * sizeof(CallFrame));
    vm->globals    = map_new();
    vm->n_tasks    = 0;
    vm->pending_throw_frame = -1;
    vm_register_stdlib(vm);
    return vm;
}

static void eff_cont_release_snapshot(EffectCont *ec) {
    if (!ec) return;
    if (ec->stack_snapshot) {
        for (int i = 0; i < ec->snapshot_len; i++) {
            if (ec->stack_snapshot[i]) value_decref(ec->stack_snapshot[i]);
        }
        free(ec->stack_snapshot);
        ec->stack_snapshot = NULL;
        ec->snapshot_len = 0;
        ec->snapshot_cap = 0;
    }
    if (ec->arm_stack_snapshot) {
        for (int i = 0; i < ec->arm_snapshot_len; i++) {
            if (ec->arm_stack_snapshot[i]) value_decref(ec->arm_stack_snapshot[i]);
        }
        free(ec->arm_stack_snapshot);
        ec->arm_stack_snapshot = NULL;
        ec->arm_snapshot_len = 0;
        ec->arm_snapshot_cap = 0;
    }
    if (ec->arm_frames) {
        free(ec->arm_frames);
        ec->arm_frames = NULL;
        ec->arm_frames_cap = 0;
        ec->arm_frame_count = 0;
    }
    ec->in_resume = 0;
}

void vm_free(VM *vm) {
    if (!vm) return;
    vm->eff_cont.valid = 0;
    eff_cont_release_snapshot(&vm->eff_cont);
    free(vm->eff_cont.frames);
    for (int i = 0; i < vm->eff_stack_count; i++) {
        eff_cont_release_snapshot(&vm->eff_stack[i]);
        free(vm->eff_stack[i].frames);
    }
    free(vm->eff_stack);
    for (int i = 0; i < vm->nursery_stack_cap; i++) free(vm->nursery_stack[i]);
    free(vm->nursery_stack);
    free(vm->nursery_lens);
    free(vm->nursery_caps);
    free(vm->nursery_ids);
    free(vm->nursery_prev_ids);
    if (vm->globals) { map_free(vm->globals); vm->globals = NULL; }
    free(vm->stack);
    free(vm->frames);
    free(vm);
}

/* Worker VM for spawn: shares the parent's globals map (so stdlib /
   user-defined globals stay live without redoing register_stdlib) but
   keeps its own stack, frames, and effect continuations. The shared
   pointer is borrowed -- vm_free_child clears it before vm_free runs so
   the parent retains ownership. */
VM *vm_new_child(VM *parent) {
    /* singletons are already initialised by the main thread; calling
       value_init_singletons here would race the parent and leak. */
    VM *vm = xs_malloc(sizeof *vm);
    memset(vm, 0, sizeof *vm);
    vm->stack_cap  = VM_STACK_INIT;
    vm->stack      = xs_malloc(vm->stack_cap * sizeof(Value *));
    memset(vm->stack, 0, vm->stack_cap * sizeof(Value *));
    vm->sp         = vm->stack;
    vm->stack_soft_limit = vm->stack + (vm->stack_cap - 16);
    vm->frames_cap = VM_FRAMES_INIT;
    vm->frames     = xs_malloc(vm->frames_cap * sizeof(CallFrame));
    memset(vm->frames, 0, vm->frames_cap * sizeof(CallFrame));
    /* parent==NULL means the caller will set globals manually (used by
       the spawn worker so it can borrow the root VM's globals even when
       the immediate spawner is itself a worker that's about to exit). */
    vm->globals    = parent ? parent->globals : NULL;
    vm->n_tasks    = 0;
    vm->pending_throw_frame = -1;
    return vm;
}

void vm_free_child(VM *vm) {
    if (!vm) return;
    /* Don't free shared globals; the parent owns them. */
    vm->globals = NULL;
    vm->eff_cont.valid = 0;
    eff_cont_release_snapshot(&vm->eff_cont);
    free(vm->eff_cont.frames);
    for (int i = 0; i < vm->eff_stack_count; i++) {
        eff_cont_release_snapshot(&vm->eff_stack[i]);
        free(vm->eff_stack[i].frames);
    }
    free(vm->eff_stack);
    for (int i = 0; i < vm->nursery_stack_cap; i++) free(vm->nursery_stack[i]);
    free(vm->nursery_stack);
    free(vm->nursery_lens);
    free(vm->nursery_caps);
    free(vm->nursery_ids);
    free(vm->nursery_prev_ids);
    while (vm->sp > vm->stack) value_decref(*--vm->sp);
    free(vm->stack);
    free(vm->frames);
    free(vm);
}

/* Public wrapper around the file-static vm_invoke so vm_thread.c can
   drive the same dispatch path the in-process callbacks use. The fresh
   worker VM starts with main_called=0, which would make OP_RETURN at
   the top frame try to dispatch a "main" entry point on the way out;
   pre-mark it as already called so the spawn body's RETURN unwinds
   cleanly back to us. We also push a sentinel frame so vm_dispatch's
   "frame_count <= stop_frame" check at the spawn body's RETURN treats
   the spawn frame as a nested call (PUSH result + return) instead of
   the top-level "decref the trailing main return" path. */
Value *vm_invoke_public(VM *vm, Value *fn, Value **args, int argc) {
    VM *saved_invoke = g_vm_for_invoke;
    VM *saved_plugin = g_plugin_vm;
    g_vm_for_invoke = vm;
    g_plugin_vm = vm;
    int saved_main_called = vm->main_called;
    vm->main_called = 1;

    /* Sentinel frame so the spawn body's OP_RETURN sees a non-empty
       call stack underneath and pushes its result back instead of
       falling into the "main entry" cleanup. */
    if (vm->frame_count >= vm->frames_cap) {
        int new_cap = vm->frames_cap > 0 ? vm->frames_cap * 2 : 64;
        vm->frames = realloc(vm->frames, new_cap * sizeof(CallFrame));
        memset(vm->frames + vm->frames_cap, 0,
               (size_t)(new_cap - vm->frames_cap) * sizeof(CallFrame));
        vm->frames_cap = new_cap;
    }
    CallFrame *sentinel = &vm->frames[vm->frame_count++];
    memset(sentinel, 0, sizeof(*sentinel));
    sentinel->base = vm->sp;

    Value *r = vm_invoke(vm, fn, args, argc);

    vm->frame_count--;     /* drop the sentinel */
    vm->main_called = saved_main_called;
    g_vm_for_invoke = saved_invoke;
    g_plugin_vm = saved_plugin;
    return r;
}

void vm_grow_stack(VM *vm) {
    int sp_off = (int)(vm->sp - vm->stack);
    /* save frame base offsets before realloc */
    int base_offs[vm->frame_count];
    for (int i = 0; i < vm->frame_count; i++)
        base_offs[i] = (int)(vm->frames[i].base - vm->stack);
    /* Fresh-VM case: stack_cap == 0 doubles to 0. Pick a real minimum. */
    int new_cap = vm->stack_cap > 0 ? vm->stack_cap * 2 : 1024;
    ptrdiff_t old_base = (ptrdiff_t)vm->stack;
    vm->stack = realloc(vm->stack, new_cap * sizeof(Value *));
    memset(vm->stack + vm->stack_cap, 0, (new_cap - vm->stack_cap) * sizeof(Value *));
    vm->sp = vm->stack + sp_off;
    vm->stack_soft_limit = vm->stack + (new_cap - 16);
    /* fix frame base pointers and try_stack stack_top pointers */
    for (int i = 0; i < vm->frame_count; i++) {
        vm->frames[i].base = vm->stack + base_offs[i];
        for (int t = 0; t < vm->frames[i].try_depth; t++) {
            TryEntry *te = &vm->frames[i].try_stack[t];
            if (te->stack_top) {
                ptrdiff_t off = (ptrdiff_t)te->stack_top - old_base;
                te->stack_top = (Value **)((char *)vm->stack + off);
            }
        }
    }
    /* fix open upvalue pointers if realloc moved the buffer */
    if ((ptrdiff_t)vm->stack != old_base) {
        for (Upvalue *u = vm->open_upvalues; u; u = u->next) {
            if (u->is_open) {
                ptrdiff_t off = (ptrdiff_t)u->ptr - old_base;
                u->ptr = (Value **)((char *)vm->stack + off);
            }
        }
    }
    vm->stack_cap = new_cap;
}

static void vm_grow_frames(VM *vm) {
    /* Handle the fresh-VM case: frames_cap == 0 means doubling stays at 0,
       which realloc treats as "free and allocate 1 byte" and the caller
       then writes 8 bytes into it. Start with a reasonable minimum. */
    int new_cap = vm->frames_cap > 0 ? vm->frames_cap * 2 : 64;
    vm->frames = realloc(vm->frames, new_cap * sizeof(CallFrame));
    memset(vm->frames + vm->frames_cap, 0, (new_cap - vm->frames_cap) * sizeof(CallFrame));
    vm->frames_cap = new_cap;
}

static int call_frame_push(VM *vm, Value *closure_val, int argc) {
    if (vm->frame_count >= 2048) {
        /* Raise a catchable StackOverflow by unwinding to the nearest
           try. If there is no try on the call stack, fall through to the
           normal error-return path. */
        XSClosure *cl = closure_val ? closure_val->cl : NULL;
        const char *fname = (cl && cl->proto && cl->proto->name) ? cl->proto->name : "<anon>";
        Value *err = xs_map_new();
        Value *kind = xs_str("StackOverflow");
        map_set(err->map, "kind", kind); value_decref(kind);
        char msg[128];
        snprintf(msg, sizeof msg, "stack overflow in '%s' (depth %d)",
                 fname, vm->frame_count);
        Value *mv = xs_str(msg);
        map_set(err->map, "message", mv); value_decref(mv);

        /* By the time we reach here OP_CALL has already shifted the
           callee out of the stack and decremented sp, so only the args
           remain above the caller's base. Drop them. */
        for (int i = 0; i < argc; i++) value_decref(POP());

        while (vm->frame_count > 0) {
            CallFrame *cf = &vm->frames[vm->frame_count - 1];
            if (cf->try_depth > 0) {
                TryEntry *te = &cf->try_stack[--cf->try_depth];
                while (vm->sp > te->stack_top) value_decref(POP());
                PUSH(err);
                cf->ip = te->catch_ip;
                return 0;  /* resume at catch */
            }
            upvalue_close_all(&vm->open_upvalues, cf->base);
            while (vm->sp > cf->base) value_decref(POP());
            value_decref(cf->closure_val);
            vm->frame_count--;
        }
        value_decref(err);
        fprintf(stderr, "uncaught: stack overflow (depth %d)\n", 8192);
        return 1;
    }
    if (vm->frame_count >= vm->frames_cap) {
        vm_grow_frames(vm);
    }
    XSClosure *cl = closure_val->cl;
    int raw_arity = cl->proto->arity;
    int is_gen = cl->proto->is_generator;
    int is_variadic = cl->proto->is_variadic;
    int arity = raw_arity;
    if (arity < 0) arity = -(arity + 1);
    if (is_variadic) {
        /* arity = min required. Collect extra args into array for variadic param */
        if (argc < arity) {
            fprintf(stderr, "arity: expected at least %d got %d\n", arity, argc);
            return 1;
        }
        /* collect variadic args into an array */
        int n_extra = argc - arity;
        Value *varargs = xs_array_new();
        Value *te_s[256], **tmp_extra = n_extra <= 256 ? te_s : malloc(n_extra * sizeof(Value*));
        for (int i = n_extra - 1; i >= 0; i--) tmp_extra[i] = POP();
        for (int i = 0; i < n_extra; i++) {
            array_push(varargs->arr, tmp_extra[i]);
            value_decref(tmp_extra[i]);
        }
        if (tmp_extra != te_s) free(tmp_extra);
        PUSH(varargs);
        argc = arity + 1; /* required + 1 varargs array */
    } else {
        if (argc < arity) {
            /* fill missing args with null for default params */
            for (int i = argc; i < arity; i++) PUSH(value_incref(XS_NULL_VAL));
            argc = arity;
        } else if (argc > arity && !is_gen) {
            /* too many args: pop extras */
            for (int i = argc; i > arity; i--) value_decref(POP());
            argc = arity;
        }
    }
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure_val = value_incref(closure_val);
    frame->ip          = cl->proto->chunk.code;
    frame->base        = vm->sp - argc;
    frame->try_depth   = 0;
    frame->defer_depth = 0;
    frame->defer_return_ip = NULL;
    frame->is_generator = is_gen;
    frame->yield_arr    = is_gen ? xs_array_new() : NULL;
    frame->yield_index  = 0;
    frame->owns_init_inst = 0;
    for (int i = 0; i < cl->proto->nlocals - arity; i++) PUSH(xs_null());
    return 0;
}

static Value *vm_invoke(VM *vm, Value *fn, Value **args, int argc) {
    if (VAL_TAG(fn) == XS_NATIVE) {
        return fn->native(NULL, args, argc);
    }
    if (VAL_TAG(fn) != XS_CLOSURE) return value_incref(XS_NULL_VAL);

    for (int i = 0; i < argc; i++) PUSH(value_incref(args[i]));

    int saved_fc = vm->frame_count;
    value_incref(fn);
    if (call_frame_push(vm, fn, argc)) {
        value_decref(fn);
        for (int i = 0; i < argc; i++) value_decref(POP());
        return value_incref(XS_NULL_VAL);
    }
    value_decref(fn);

    /* Run the callee to completion regardless of the JIT's single-step
       mode. Otherwise a JIT-driven step that triggers a callback (e.g.
       array.map) would only run one opcode of the callback before
       unwinding back to native code. */
    int saved_step = vm->single_step;
    vm->single_step = 0;
    int rc = vm_dispatch(vm, saved_fc);
    vm->single_step = saved_step;
    if (rc != 0) return value_incref(XS_NULL_VAL);
    return POP();
}

static Value *vm_try_struct_op(VM *vm, Value *a, const char *op, Value *b) {
    if (VAL_TAG(a) != XS_STRUCT_VAL || !a->st || !a->st->type_name) return NULL;
    /* look up the operator method from globals: stored by impl under the op name */
    Value *fn = map_get(vm->globals, op);
    if (!fn || (VAL_TAG(fn) != XS_CLOSURE && VAL_TAG(fn) != XS_FUNC && VAL_TAG(fn) != XS_NATIVE))
        return NULL;
    Value *args[2] = { a, b };
    return vm_invoke(vm, fn, args, 2);
}

/* True only if `a` and `b` have a sensible ordering: numeric/numeric,
   string/string, char/char, or array/array, tuple/tuple of identical tags.
   Without this guard, value_cmp's tag-comparison fallback would silently
   accept things like `bool < int`, which the interpreter rejects. */
static int vm_orderable_pair(Value *a, Value *b) {
    if (!a || !b) return 0;
    int ta = VAL_TAG(a), tb = VAL_TAG(b);
    int a_num = (ta == XS_INT || ta == XS_FLOAT || ta == XS_BIGINT);
    int b_num = (tb == XS_INT || tb == XS_FLOAT || tb == XS_BIGINT);
    if (a_num && b_num) return 1;
    if (ta == XS_STR   && tb == XS_STR)   return 1;
    if (ta == XS_CHAR  && tb == XS_CHAR)  return 1;
    if (ta == XS_ARRAY && tb == XS_ARRAY) return 1;
    if (ta == XS_TUPLE && tb == XS_TUPLE) return 1;
    return 0;
}

static Value *vm_try_dunder(VM *vm, Value *obj, const char *dunder, Value *other) {
    if (VAL_TAG(obj) != XS_MAP) return NULL;
    Value *fn = map_get(obj->map, dunder);
    if (!fn) {
        Value *methods = map_get(obj->map, "__methods");
        if (methods && VAL_TAG(methods) == XS_MAP)
            fn = map_get(methods->map, dunder);
    }
    if (!fn || (VAL_TAG(fn) != XS_CLOSURE && VAL_TAG(fn) != XS_NATIVE)) return NULL;
    Value *args[2] = { obj, other };
    return vm_invoke(vm, fn, args, 2);
}

/* Try an operator method (e.g. "+", "-") on an instance-shaped map.
   Structs compiled with OP_MAKE_INST live as maps tagged with __type; the
   operator impl is registered on the class global's __methods. */
static Value *vm_try_map_op(VM *vm, Value *a, const char *op, Value *b) {
    if (VAL_TAG(a) != XS_MAP || !a->map) return NULL;
    Value *type = map_get(a->map, "__type");
    if (!type || VAL_TAG(type) != XS_STR || !type->s) return NULL;
    Value *cls = map_get(vm->globals, type->s);
    if (!cls || (VAL_TAG(cls) != XS_MAP && VAL_TAG(cls) != XS_MODULE) || !cls->map) return NULL;
    Value *methods = map_get(cls->map, "__methods");
    if (!methods || VAL_TAG(methods) != XS_MAP) return NULL;
    Value *fn = map_get(methods->map, op);
    if (!fn || (VAL_TAG(fn) != XS_CLOSURE && VAL_TAG(fn) != XS_FUNC && VAL_TAG(fn) != XS_NATIVE))
        return NULL;
    Value *args[2] = { a, b };
    return vm_invoke(vm, fn, args, 2);
}

/* Per-opcode execution histogram, populated only when XS_VM_OPCOUNTS=1.
 * Atexit handler dumps a sorted top-N so you can see exactly where each
 * benchmark spends its dispatch cycles. Cost when disabled is one
 * never-taken branch per dispatched opcode. */
static int g_opcounts_enabled = -1;
static uint64_t g_opcounts[OP__MAX];
static uint64_t g_opcounts_jit_step[OP__MAX];   /* subset dispatched via single_step */

static void vm_opcounts_dump(void) {
    uint64_t total = 0, total_jit = 0;
    for (int i = 0; i < OP__MAX; i++) {
        total     += g_opcounts[i];
        total_jit += g_opcounts_jit_step[i];
    }
    if (total == 0) return;
    fprintf(stderr, "xs vm opcounts: %llu ops dispatched"
            " (%llu via JIT single-step, %llu in pure VM)\n",
            (unsigned long long)total, (unsigned long long)total_jit,
            (unsigned long long)(total - total_jit));
    typedef struct { int op; uint64_t n; } Row;
    Row rows[OP__MAX];
    for (int i = 0; i < OP__MAX; i++) { rows[i].op = i; rows[i].n = g_opcounts[i]; }
    /* selection sort top-20; we only need the top of the list */
    for (int i = 0; i < 20 && i < OP__MAX; i++) {
        int best = i;
        for (int j = i + 1; j < OP__MAX; j++)
            if (rows[j].n > rows[best].n) best = j;
        Row tmp = rows[i]; rows[i] = rows[best]; rows[best] = tmp;
        if (rows[i].n == 0) break;
    }
    for (int i = 0; i < 20; i++) {
        if (rows[i].n == 0) break;
        const char *nm = bytecode_op_name((Opcode)rows[i].op);
        fprintf(stderr, "  %-18s %12llu  (%5.2f%%)\n",
                nm ? nm : "?", (unsigned long long)rows[i].n,
                100.0 * rows[i].n / total);
    }
    if (total_jit > 0) {
        fprintf(stderr, "xs vm opcounts (JIT single-step residue):\n");
        Row jrows[OP__MAX];
        for (int i = 0; i < OP__MAX; i++) {
            jrows[i].op = i; jrows[i].n = g_opcounts_jit_step[i];
        }
        for (int i = 0; i < 20 && i < OP__MAX; i++) {
            int best = i;
            for (int j = i + 1; j < OP__MAX; j++)
                if (jrows[j].n > jrows[best].n) best = j;
            Row tmp = jrows[i]; jrows[i] = jrows[best]; jrows[best] = tmp;
            if (jrows[i].n == 0) break;
        }
        for (int i = 0; i < 20; i++) {
            if (jrows[i].n == 0) break;
            const char *nm = bytecode_op_name((Opcode)jrows[i].op);
            fprintf(stderr, "  %-18s %12llu  (%5.2f%%)\n",
                    nm ? nm : "?", (unsigned long long)jrows[i].n,
                    100.0 * jrows[i].n / total_jit);
        }
    }
}

static void vm_opcounts_init(void) {
    if (g_opcounts_enabled >= 0) return;
    const char *env = getenv("XS_VM_OPCOUNTS");
    g_opcounts_enabled = (env && *env && env[0] != '0') ? 1 : 0;
    if (g_opcounts_enabled) atexit(vm_opcounts_dump);
}

int vm_run(VM *vm, XSProto *proto) {
    xs_limits_reset();
    vm_opcounts_init();
    g_vm_for_invoke = vm;
    g_plugin_vm = vm;
    XSClosure *top_cl    = xs_malloc(sizeof *top_cl);
    top_cl->proto        = proto; proto->refcount++;
    top_cl->upvalues     = NULL;
    top_cl->refcount     = 1;
    Value *top_val       = xs_malloc(sizeof *top_val);
    top_val->tag = XS_CLOSURE;
    top_val->refcount    = 1;
    top_val->cl          = top_cl;

    /* Ensure frames is allocated. Plugin loads create a freshly-zeroed VM
       (via memset), so frames is NULL until grown; the first frame push
       would otherwise deref a null pointer. */
    if (vm->frame_count >= vm->frames_cap) {
        vm_grow_frames(vm);
    }
    CallFrame *frame     = &vm->frames[vm->frame_count++];
    frame->closure_val   = top_val;
    frame->ip            = proto->chunk.code;
    frame->base          = vm->sp;
    frame->try_depth     = 0;
    for (int i = 0; i < proto->nlocals; i++) PUSH(xs_null());

    return vm_dispatch(vm, 0);
}

static int vm_dispatch(VM *vm, int stop_frame) {
    CallFrame *frame = FRAME;
    for (;;) {
        /* Per-opcode resource-limit tick. Bails out through the same
           pending-throw path so try/catch can catch it. */
        if (xs_limits_tick()) {
            xs_runtime_error(span_zero(), "ResourceLimit", NULL,
                             "%s exceeded", xs_limits_exceeded_name());
        }
        /* If xs_runtime_error queued a throw (e.g. from an arithmetic
           op or builtin), drain it before fetching the next opcode and
           run the same unwind logic OP_THROW would. */
        if (g_xs_pending_throw) {
            Value *exc = g_xs_pending_throw;
            g_xs_pending_throw = NULL;
            int handled = 0;
            while (vm->frame_count > 0) {
                CallFrame *cf = &vm->frames[vm->frame_count - 1];
                if (cf->try_depth > 0) {
                    TryEntry *te = &cf->try_stack[--cf->try_depth];
                    if (g_xs_in_try > 0) g_xs_in_try--;
                    while (vm->sp > te->stack_top) value_decref(POP());
                    PUSH(exc);
                    frame = cf;
                    frame->ip = te->catch_ip;
                    handled = 1;
                    break;
                }
                upvalue_close_all(&vm->open_upvalues, cf->base);
                while (vm->sp > cf->base) value_decref(POP());
                value_decref(cf->closure_val);
                vm->frame_count--;
            }
            if (!handled) {
                if (vm->is_thread_worker) {
                    /* Capture for the spawn-thread's future to surface
                       on await; otherwise stderr races between threads
                       and an unjoined spawn would print every time. */
                    vm->uncaught_thread_exc = exc;
                    return 1;
                }
                char *s = value_str(exc);
                fprintf(stderr, "uncaught: %s\n", s);
                free(s);
                value_decref(exc);
                return 1;
            }
            continue; /* re-enter loop at catch handler */
        }
        Instruction instr = *frame->ip++;
        Opcode op = INSTR_OPCODE(instr);

        if (g_opcounts_enabled > 0) {
            g_opcounts[op]++;
            if (vm->single_step) g_opcounts_jit_step[op]++;
        }

        switch (op) {

        case OP_NOP: break;

        case OP_PUSH_CONST:
            PUSH(value_incref(PROTO->chunk.consts[INSTR_Bx(instr)]));
            break;
        case OP_PUSH_NULL:  PUSH(value_incref(XS_NULL_VAL));  break;
        case OP_PUSH_TRUE:  PUSH(value_incref(XS_TRUE_VAL));  break;
        case OP_PUSH_FALSE: PUSH(value_incref(XS_FALSE_VAL)); break;
        case OP_POP:  value_decref(POP()); break;
        case OP_DUP:  { Value *_top = PEEK(0); PUSH(value_incref(_top)); break; }

        case OP_LOAD_LOCAL: {
            Value *v = frame->base[INSTR_Bx(instr)];
            PUSH(value_incref(v ? v : XS_NULL_VAL));
            break;
        }
        case OP_STORE_LOCAL: {
            int slot   = (int)INSTR_Bx(instr);
            Value *old = frame->base[slot];
            frame->base[slot] = POP();
            if (old) value_decref(old);
            break;
        }
        case OP_LOAD_UPVALUE:
            PUSH(value_incref(*CL->upvalues[INSTR_Bx(instr)]->ptr));
            break;
        case OP_STORE_UPVALUE: {
            Upvalue *uv = CL->upvalues[INSTR_Bx(instr)];
            Value *old  = *uv->ptr;
            *uv->ptr    = POP();
            if (old) value_decref(old);
            break;
        }
        case OP_LOAD_GLOBAL: {
            XSChunk *chk = &PROTO->chunk;
            int ip_idx = (int)(frame->ip - chk->code) - 1;
            if (ip_idx < 0 || ip_idx >= chk->len) {
                /* ip out of range (e.g., proto executing via cached
                   closure after reload) -- take the non-cached path. */
                const char *name = chk->consts[INSTR_Bx(instr)]->s;
                Value *v = map_get(vm->globals, name);
                PUSH(v ? value_incref(v) : value_incref(XS_NULL_VAL));
                break;
            }
            if (!chk->ic) {
                chk->ic = xs_calloc((size_t)chk->len, sizeof(Value *));
                if (!chk->ic_version)
                    chk->ic_version = xs_calloc((size_t)chk->len,
                                                sizeof(uint64_t));
            }
            /* The field IC also writes to ic_version, but only when the
             * receiver is XS_INST -- different opcode at the same IP can
             * never happen, so the two never alias. The version field
             * for LOAD_GLOBAL means "global_version snapshot"; for
             * LOAD_FIELD it means "(cap << 32) | bucket". */
            Value *cached = chk->ic[ip_idx];
            if (cached && chk->ic_version[ip_idx] == vm->global_version) {
                PUSH(value_incref(cached));
                break;
            }
            const char *name = chk->consts[INSTR_Bx(instr)]->s;
            Value *v = map_get(vm->globals, name);
            if (!v) v = XS_NULL_VAL;
            if (chk->ic[ip_idx]) value_decref(chk->ic[ip_idx]);
            chk->ic[ip_idx] = value_incref(v);
            chk->ic_version[ip_idx] = vm->global_version;
            PUSH(value_incref(v));
            break;
        }
        case OP_STORE_GLOBAL: {
            vm->global_version++;
            const char *name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *v = POP();
            /* Overload accumulation: if a user function with the same
               name is already bound and the new value is also a user
               function, merge them into an XS_OVERLOAD wrapper. We
               deliberately do NOT pull XS_NATIVE built-ins into the
               overload set: a user `fn sum(...)` should shadow the
               built-in `sum`, not co-exist with it (otherwise the
               arity-pick would route some calls back to the builtin). */
            if (VAL_TAG(v) == XS_CLOSURE || VAL_TAG(v) == XS_FUNC) {
                Value *existing = map_get(vm->globals, name);
                if (existing && VAL_TAG(existing) == XS_OVERLOAD) {
                    array_push(existing->overload, value_incref(v));
                    value_decref(v);
                    break;
                }
                if (existing && (VAL_TAG(existing) == XS_FUNC ||
                                 VAL_TAG(existing) == XS_CLOSURE)) {
                    Value *oset = xs_overload_new();
                    array_push(oset->overload, value_incref(existing));
                    array_push(oset->overload, value_incref(v));
                    map_set(vm->globals, name, oset);
                    value_decref(oset);
                    value_decref(v);
                    break;
                }
            }
            map_set(vm->globals, name, v);
            value_decref(v);
            break;
        }

        case OP_ADD: {
            Value *b = POP(), *a = POP(); Value *r;
            if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) {
                r = xs_safe_add(VAL_INT(a), VAL_INT(b));
            } else if ((VAL_TAG(a) == XS_INT || VAL_TAG(a) == XS_BIGINT) &&
                       (VAL_TAG(b) == XS_INT || VAL_TAG(b) == XS_BIGINT)) {
                r = xs_numeric_add(a, b);
            } else if (VAL_TAG(a) == XS_STR || VAL_TAG(b) == XS_STR) {
                char *as = value_str(a), *bs = value_str(b);
                size_t n = strlen(as) + strlen(bs) + 1;
                char *buf = xs_malloc(n);
                strcpy(buf, as); strcat(buf, bs);
                free(as); free(bs);
                r = xs_str(buf); free(buf);
            } else if (VAL_TAG(a) == XS_ARRAY && VAL_TAG(b) == XS_ARRAY) {
                r = xs_array_new();
                for (int ai = 0; ai < a->arr->len; ai++) array_push(r->arr, a->arr->items[ai]);
                for (int bi = 0; bi < b->arr->len; bi++) array_push(r->arr, b->arr->items[bi]);
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_dunder(vm, a, "__add__", b)) != NULL) {
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_map_op(vm, a, "+", b)) != NULL) {
            } else if (VAL_TAG(a) == XS_STRUCT_VAL && (r = vm_try_struct_op(vm, a, "+", b)) != NULL) {
            /* mixed-type fallback when LHS is not an instance: let the
             * RHS's class handle the op. The receiver becomes b so the
             * user's method runs as `self.+(other_lhs)`; commutative
             * intent is on the user, but the call gets dispatched. */
            } else if (VAL_TAG(b) == XS_MAP && (r = vm_try_map_op(vm, b, "+", a)) != NULL) {
            } else if ((VAL_TAG(a) == XS_INT || VAL_TAG(a) == XS_FLOAT || VAL_TAG(a) == XS_BIGINT) &&
                       (VAL_TAG(b) == XS_INT || VAL_TAG(b) == XS_FLOAT || VAL_TAG(b) == XS_BIGINT)) {
                double av = VAL_TAG(a)==XS_INT?(double)VAL_INT(a):(VAL_TAG(a)==XS_BIGINT?bigint_to_double(a->bigint):a->f);
                double bv = VAL_TAG(b)==XS_INT?(double)VAL_INT(b):(VAL_TAG(b)==XS_BIGINT?bigint_to_double(b->bigint):b->f);
                r = xs_float(av + bv);
            } else {
                Span s = {0};
                xs_runtime_error(s, "type mismatch", NULL,
                    "cannot add values of tag %d and %d",
                    (int)VAL_TAG(a), (int)VAL_TAG(b));
                r = value_incref(XS_NULL_VAL);
            }
            value_decref(a); value_decref(b); PUSH(r); break;
        }
        case OP_SUB: {
            Value *b=POP(), *a=POP(); Value *r;
            if (VAL_TAG(a)==XS_INT && VAL_TAG(b)==XS_INT) {
                r = xs_safe_sub(VAL_INT(a), VAL_INT(b));
            } else if ((VAL_TAG(a)==XS_INT||VAL_TAG(a)==XS_BIGINT) && (VAL_TAG(b)==XS_INT||VAL_TAG(b)==XS_BIGINT)) {
                r = xs_numeric_sub(a, b);
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_dunder(vm, a, "__sub__", b)) != NULL) {
                /* dunder */
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_map_op(vm, a, "-", b)) != NULL) {
            } else if (VAL_TAG(a) == XS_STRUCT_VAL && (r = vm_try_struct_op(vm, a, "-", b)) != NULL) {
            } else if (VAL_TAG(b) == XS_MAP && (r = vm_try_map_op(vm, b, "-", a)) != NULL) {
            } else if ((VAL_TAG(a) == XS_INT || VAL_TAG(a) == XS_FLOAT || VAL_TAG(a) == XS_BIGINT) &&
                       (VAL_TAG(b) == XS_INT || VAL_TAG(b) == XS_FLOAT || VAL_TAG(b) == XS_BIGINT)) {
                double av = VAL_TAG(a)==XS_INT?(double)VAL_INT(a):(VAL_TAG(a)==XS_BIGINT?bigint_to_double(a->bigint):a->f);
                double bv = VAL_TAG(b)==XS_INT?(double)VAL_INT(b):(VAL_TAG(b)==XS_BIGINT?bigint_to_double(b->bigint):b->f);
                r = xs_float(av - bv);
            } else {
                Span s = {0};
                xs_runtime_error(s, "type mismatch", NULL,
                    "cannot subtract values of tag %d and %d",
                    (int)VAL_TAG(a), (int)VAL_TAG(b));
                r = value_incref(XS_NULL_VAL);
            }
            value_decref(a); value_decref(b); PUSH(r); break;
        }
        case OP_MUL: {
            Value *b=POP(), *a=POP(); Value *r;
            if (VAL_TAG(a)==XS_INT && VAL_TAG(b)==XS_INT) {
                r = xs_safe_mul(VAL_INT(a), VAL_INT(b));
            } else if ((VAL_TAG(a)==XS_INT||VAL_TAG(a)==XS_BIGINT) && (VAL_TAG(b)==XS_INT||VAL_TAG(b)==XS_BIGINT)) {
                r = xs_numeric_mul(a, b);
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_dunder(vm, a, "__mul__", b)) != NULL) {
                /* dunder */
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_map_op(vm, a, "*", b)) != NULL) {
            } else if (VAL_TAG(a) == XS_STRUCT_VAL && (r = vm_try_struct_op(vm, a, "*", b)) != NULL) {
            } else if (VAL_TAG(b) == XS_MAP && (r = vm_try_map_op(vm, b, "*", a)) != NULL) {
            } else if ((VAL_TAG(a)==XS_STR && VAL_TAG(b)==XS_INT)
                    || (VAL_TAG(b)==XS_STR && VAL_TAG(a)==XS_INT)) {
                /* Make repetition commutative: 3 * "ab" should match
                   "ab" * 3 since users expect it to mirror Python. */
                Value *sv = VAL_TAG(a)==XS_STR ? a : b;
                Value *iv = VAL_TAG(a)==XS_STR ? b : a;
                int64_t count = VAL_INT(iv);
                if (count <= 0) { r = xs_str(""); }
                else {
                    size_t slen = strlen(sv->s);
                    char *buf = xs_malloc(slen * (size_t)count + 1);
                    buf[0] = '\0';
                    for (int64_t ci = 0; ci < count; ci++) memcpy(buf + slen * (size_t)ci, sv->s, slen);
                    buf[slen * (size_t)count] = '\0';
                    r = xs_str(buf); free(buf);
                }
            } else if ((VAL_TAG(a) == XS_INT || VAL_TAG(a) == XS_FLOAT || VAL_TAG(a) == XS_BIGINT) &&
                       (VAL_TAG(b) == XS_INT || VAL_TAG(b) == XS_FLOAT || VAL_TAG(b) == XS_BIGINT)) {
                double av = VAL_TAG(a)==XS_INT?(double)VAL_INT(a):(VAL_TAG(a)==XS_BIGINT?bigint_to_double(a->bigint):a->f);
                double bv = VAL_TAG(b)==XS_INT?(double)VAL_INT(b):(VAL_TAG(b)==XS_BIGINT?bigint_to_double(b->bigint):b->f);
                r = xs_float(av * bv);
            } else {
                Span s = {0};
                xs_runtime_error(s, "type mismatch", NULL,
                    "cannot multiply values of tag %d and %d",
                    (int)VAL_TAG(a), (int)VAL_TAG(b));
                r = value_incref(XS_NULL_VAL);
            }
            value_decref(a); value_decref(b); PUSH(r); break;
        }
        case OP_DIV: {
            Value *b=POP(), *a=POP(); Value *r;
            if (VAL_TAG(a)==XS_INT && VAL_TAG(b)==XS_INT) {
                if (VAL_INT(b) == 0) {
                    Span s = {0};
                    xs_runtime_error(s, "division by zero", NULL, "cannot divide by zero");
                    r = value_incref(XS_NULL_VAL);
                } else r = xs_int(VAL_INT(a) / VAL_INT(b));
            } else if ((VAL_TAG(a)==XS_INT||VAL_TAG(a)==XS_BIGINT) && (VAL_TAG(b)==XS_INT||VAL_TAG(b)==XS_BIGINT)) {
                r = xs_numeric_div(a, b);
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_dunder(vm, a, "__div__", b)) != NULL) {
                /* dunder */
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_map_op(vm, a, "/", b)) != NULL) {
            } else if (VAL_TAG(b) == XS_MAP && (r = vm_try_map_op(vm, b, "/", a)) != NULL) {
            } else {
                double bv = VAL_TAG(b)==XS_INT?(double)VAL_INT(b):(VAL_TAG(b)==XS_BIGINT?bigint_to_double(b->bigint):b->f);
                double av = VAL_TAG(a)==XS_INT?(double)VAL_INT(a):(VAL_TAG(a)==XS_BIGINT?bigint_to_double(a->bigint):a->f);
                /* Float division follows IEEE 754: 1.0/0 -> Infinity,
                   0.0/0.0 -> NaN. Only int/int by zero throws. */
                r = xs_float(av / bv);
            }
            value_decref(a); value_decref(b); PUSH(r); break;
        }
        case OP_MOD: {
            Value *b=POP(), *a=POP();
            Value *r;
            if (VAL_TAG(a)==XS_INT && VAL_TAG(b)==XS_INT) {
                if (VAL_INT(b) == 0) {
                    Span s = {0};
                    xs_runtime_error(s, "modulo by zero", NULL, "cannot take modulo with zero divisor");
                    r = value_incref(XS_NULL_VAL);
                } else {
                    /* Truncated modulo (sign of dividend), matches C. */
                    r = xs_int(VAL_INT(a) % VAL_INT(b));
                }
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_dunder(vm, a, "__mod__", b)) != NULL) {
                /* dunder */
            } else if (VAL_TAG(a) == XS_MAP && (r = vm_try_map_op(vm, a, "%", b)) != NULL) {
            } else if (VAL_TAG(b) == XS_MAP && (r = vm_try_map_op(vm, b, "%", a)) != NULL) {
            } else {
                r = xs_numeric_mod(a, b);
            }
            value_decref(a); value_decref(b); PUSH(r); break;
        }
        case OP_POW: {
            Value *b=POP(), *a=POP();
            if (VAL_TAG(a)==XS_INT && VAL_TAG(b)==XS_INT && VAL_INT(b) >= 0) {
                Value *r = xs_safe_pow(VAL_INT(a), VAL_INT(b));
                value_decref(a); value_decref(b); PUSH(r);
            } else if ((VAL_TAG(a)==XS_INT||VAL_TAG(a)==XS_BIGINT) && (VAL_TAG(b)==XS_INT||VAL_TAG(b)==XS_BIGINT)) {
                Value *r = xs_numeric_pow(a, b);
                value_decref(a); value_decref(b); PUSH(r);
            } else {
                double av = VAL_TAG(a)==XS_INT?(double)VAL_INT(a):(VAL_TAG(a)==XS_BIGINT?bigint_to_double(a->bigint):a->f);
                double bv = VAL_TAG(b)==XS_INT?(double)VAL_INT(b):(VAL_TAG(b)==XS_BIGINT?bigint_to_double(b->bigint):b->f);
                value_decref(a); value_decref(b); PUSH(xs_float(pow(av,bv)));
            }
            break;
        }
        case OP_NEG: {
            Value *a = POP();
            if (VAL_TAG(a) == XS_INT) {
                PUSH(xs_safe_neg(VAL_INT(a)));
            } else if (VAL_TAG(a) == XS_BIGINT) {
                PUSH(xs_numeric_neg(a));
            } else {
                PUSH(xs_float(-a->f));
            }
            value_decref(a); break;
        }
        case OP_NOT: {
            Value *a = POP();
            PUSH(xs_bool(!value_truthy(a)));
            value_decref(a); break;
        }
        case OP_CONCAT: {
            Value *b=POP(), *a=POP();
            char *as=value_str(a), *bs=value_str(b);
            size_t n=strlen(as)+strlen(bs)+1;
            char *buf=xs_malloc(n); strcpy(buf,as); strcat(buf,bs);
            free(as); free(bs); value_decref(a); value_decref(b);
            Value *r=xs_str(buf); free(buf); PUSH(r); break;
        }

        // --- comparisons
        /* For instance-shaped maps we try the dunder first (`__eq__`,
         * `__lt__`, ...), then the literal operator method (`==`, `<`)
         * declared in the user's `impl` block. Without the second lookup,
         * arithmetic ops dispatched correctly but comparisons silently
         * fell back to value_equal / value_cmp on the struct fields. */
        case OP_EQ:  { Value *b=POP(),*a=POP(); Value *r;
                       if (VAL_TAG(a)==XS_MAP && (r=vm_try_dunder(vm,a,"__eq__",b))!=NULL) { value_decref(a);value_decref(b);PUSH(r); }
                       else if (VAL_TAG(a)==XS_MAP && (r=vm_try_map_op(vm,a,"==",b))!=NULL) { value_decref(a);value_decref(b);PUSH(r); }
                       else { PUSH(xs_bool(value_equal(a,b))); value_decref(a);value_decref(b); } break; }
        case OP_NEQ: { Value *b=POP(),*a=POP(); Value *r;
                       if (VAL_TAG(a)==XS_MAP && (r=vm_try_dunder(vm,a,"__ne__",b))!=NULL) { value_decref(a);value_decref(b);PUSH(r); }
                       else if (VAL_TAG(a)==XS_MAP && (r=vm_try_map_op(vm,a,"!=",b))!=NULL) { value_decref(a);value_decref(b);PUSH(r); }
                       else { PUSH(xs_bool(!value_equal(a,b))); value_decref(a);value_decref(b); } break; }
#define VM_CMP_OP(opname, sym, predicate)                                       \
        { Value *b=POP(),*a=POP(); Value *r;                                    \
          if (VAL_TAG(a)==XS_MAP && (r=vm_try_dunder(vm,a,opname,b))!=NULL) {   \
              value_decref(a);value_decref(b);PUSH(r);                          \
          } else if (VAL_TAG(a)==XS_MAP && (r=vm_try_map_op(vm,a,sym,b))!=NULL){ \
              value_decref(a);value_decref(b);PUSH(r);                          \
          } else if (!vm_orderable_pair(a, b)) {                                \
              Span s = {0};                                                     \
              xs_runtime_error(s, "type mismatch", NULL,                        \
                  "operator '%s' is not defined for these operands", sym);      \
              value_decref(a); value_decref(b);                                 \
              PUSH(value_incref(XS_NULL_VAL));                                  \
          } else { PUSH(xs_bool(predicate)); value_decref(a);value_decref(b); } \
          break; }
        case OP_LT:  VM_CMP_OP("__lt__", "<",  value_cmp(a,b)<0)
        case OP_GT:  VM_CMP_OP("__gt__", ">",  value_cmp(a,b)>0)
        case OP_LTE: VM_CMP_OP("__le__", "<=", value_cmp(a,b)<=0)
        case OP_GTE: VM_CMP_OP("__ge__", ">=", value_cmp(a,b)>=0)
#undef VM_CMP_OP

        /* collections */
        case OP_MAKE_ARRAY: {
            int n = (int)INSTR_C(instr);
            Value *arr = xs_array_new();
            Value *tmp_s[256], **tmp = n <= 256 ? tmp_s : malloc(n * sizeof(Value*));
            for (int i = n-1; i >= 0; i--) tmp[i] = POP();
            for (int i = 0; i < n; i++) {
                array_push(arr->arr, tmp[i]);
                value_decref(tmp[i]);
            }
            if (tmp != tmp_s) free(tmp);
            PUSH(arr); break;
        }
        case OP_MAKE_TUPLE: {
            int n = (int)INSTR_C(instr);
            Value *tup = xs_tuple_new();
            Value *tmp_s[256], **tmp = n <= 256 ? tmp_s : malloc(n * sizeof(Value*));
            for (int i = n-1; i >= 0; i--) tmp[i] = POP();
            for (int i = 0; i < n; i++) {
                array_push(tup->arr, tmp[i]);
                value_decref(tmp[i]);
            }
            if (tmp != tmp_s) free(tmp);
            PUSH(tup); break;
        }
        case OP_INDEX_GET: {
            Value *idx = POP(), *col = POP(); Value *r;
            if ((VAL_TAG(col)==XS_ARRAY||VAL_TAG(col)==XS_TUPLE) && VAL_TAG(idx)==XS_INT) {
                int64_t i = VAL_INT(idx);
                if (i < 0) i += col->arr->len;
                r = (i>=0 && i<col->arr->len) ? value_incref(col->arr->items[i]) : value_incref(XS_NULL_VAL);
            } else if ((VAL_TAG(col)==XS_ARRAY||VAL_TAG(col)==XS_TUPLE) && VAL_TAG(idx)==XS_RANGE && idx->range) {
                int64_t start = idx->range->start;
                int64_t end = idx->range->end;
                if (idx->range->inclusive) end++;
                if (start < 0) start += col->arr->len;
                if (end < 0) end += col->arr->len;
                if (start < 0) start = 0;
                if (end > col->arr->len) end = col->arr->len;
                Value *arr = xs_array_new();
                for (int64_t j = start; j < end; j++)
                    array_push(arr->arr, value_incref(col->arr->items[j]));
                r = arr;
            } else if (VAL_TAG(col)==XS_MAP && VAL_TAG(idx)==XS_STR) {
                Value *cid = map_get(col->map, "_chan_id");
                int is_chan = (cid && VAL_TAG(cid) == XS_INT);
                if (is_chan && (strcmp(idx->s, "_buf") == 0 ||
                                strcmp(idx->s, "_cap") == 0 ||
                                strcmp(idx->s, "_chan_id") == 0 ||
                                strcmp(idx->s, "_type") == 0 ||
                                strcmp(idx->s, "__type") == 0)) {
                    r = value_incref(XS_NULL_VAL);
                } else {
                    Value *v = map_get(col->map, idx->s);
                    r = v ? value_incref(v) : value_incref(XS_NULL_VAL);
                }
            } else if (VAL_TAG(col)==XS_RANGE && VAL_TAG(idx)==XS_INT && col->range) {
                r = xs_int(col->range->start + VAL_INT(idx));
            } else if (VAL_TAG(col)==XS_STR && VAL_TAG(idx)==XS_INT) {
                const char *s = col->s;
                int64_t slen = (int64_t)strlen(s);
                int64_t i = VAL_INT(idx);
                if (i < 0) i += slen;
                if (i >= 0 && i < slen) { char buf[2] = {s[i], 0}; r = xs_str(buf); }
                else r = value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(col)==XS_STR && VAL_TAG(idx)==XS_RANGE && idx->range) {
                const char *s = col->s;
                int64_t slen = (int64_t)strlen(s);
                int64_t start = idx->range->start;
                int64_t end = idx->range->end;
                if (idx->range->inclusive) end++;
                if (start < 0) start += slen;
                if (end < 0) end += slen;
                if (start < 0) start = 0;
                if (end > slen) end = slen;
                if (start >= end) { r = xs_str(""); }
                else {
                    int64_t len = end - start;
                    char *buf = malloc(len + 1);
                    memcpy(buf, s + start, len);
                    buf[len] = '\0';
                    r = xs_str(buf);
                    free(buf);
                }
            } else if (VAL_TAG(col)==XS_NULL) {
                Span s = {0};
                xs_runtime_error(s, "null index", NULL,
                    "cannot index a null value");
                r = value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(col)==XS_INT || VAL_TAG(col)==XS_FLOAT ||
                       VAL_TAG(col)==XS_BOOL || VAL_TAG(col)==XS_BIGINT) {
                Span s = {0};
                xs_runtime_error(s, "not indexable", NULL,
                    "cannot index a value of tag %d (only arrays, tuples, maps, strings, ranges)",
                    (int)VAL_TAG(col));
                r = value_incref(XS_NULL_VAL);
            } else {
                /* Unknown collection/key combo: keep null fallback for
                   user-defined types so dunder lookups stay open-ended. */
                r = value_incref(XS_NULL_VAL);
            }
            value_decref(idx); value_decref(col); PUSH(r); break;
        }
        case OP_INDEX_SET: {
            Value *val = POP(), *idx = POP(), *col = POP();
            if (VAL_TAG(col) == XS_STR) {
                /* Strings are immutable; silent no-op was masking the
                   error for users coming from languages with mutable
                   string buffers. */
                value_decref(val); value_decref(idx); value_decref(col);
                xs_runtime_error(span_zero(), "TypeError", NULL,
                                 "strings are immutable; cannot assign by index");
                break;
            }
            if (VAL_TAG(col) == XS_TUPLE) {
                value_decref(val); value_decref(idx); value_decref(col);
                xs_runtime_error(span_zero(), "TypeError", NULL,
                                 "tuples are immutable; cannot assign by index");
                break;
            }
            if ((VAL_TAG(col)==XS_ARRAY) && VAL_TAG(idx)==XS_INT) {
                int64_t i = VAL_INT(idx);
                int64_t n = col->arr->len;
                /* allow Python-style negative indexing on assign too */
                if (i < 0) i += n;
                if (i>=0 && i<n) {
                    value_decref(col->arr->items[i]);
                    col->arr->items[i] = value_incref(val);
                } else {
                    /* Out-of-bounds set used to silently no-op, masking
                       bugs (e.g. growing the array via assign at idx==N
                       was expected but not supported). Raise instead. */
                    int64_t orig_idx = VAL_INT(idx);
                    value_decref(val); value_decref(idx); value_decref(col);
                    xs_runtime_error(span_zero(), "IndexError", NULL,
                                     "index %lld out of bounds (len %lld)",
                                     (long long)orig_idx, (long long)n);
                    break;
                }
            } else if (VAL_TAG(col)==XS_MAP && (VAL_TAG(idx)==XS_STR || VAL_TAG(idx)==XS_INT
                                                || VAL_TAG(idx)==XS_BOOL || VAL_TAG(idx)==XS_NULL
                                                || VAL_TAG(idx)==XS_FLOAT)) {
                if (VAL_TAG(idx)==XS_STR) {
                    map_set(col->map, idx->s, val);
                } else {
                    /* Stringify non-string keys so the literal-vs-runtime
                       asymmetry (`m[1]=...` vs `#{1: "one"}`) matches up:
                       both produce the stringified key. */
                    char kbuf[64];
                    if (VAL_TAG(idx)==XS_INT) snprintf(kbuf, sizeof kbuf, "%lld", (long long)VAL_INT(idx));
                    else if (VAL_TAG(idx)==XS_BOOL) snprintf(kbuf, sizeof kbuf, "%s", VAL_INT(idx)?"true":"false");
                    else if (VAL_TAG(idx)==XS_NULL) snprintf(kbuf, sizeof kbuf, "null");
                    else snprintf(kbuf, sizeof kbuf, "%g", idx->f);
                    map_set(col->map, kbuf, val);
                }
            }
            value_decref(val); value_decref(idx); value_decref(col); break;
        }
        case OP_LOAD_FIELD: {
            const char *name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *obj = POP(); Value *r = NULL;
            /* Inline cache for the dominant case: instance-of-class
             * field reads. Cache shape = (XSClass*, fields->cap) and
             * the bucket index where `name` lives. Hot benchmarks
             * (n-body, anything with a per-iteration `obj.field`) call
             * this thousands to millions of times on the same shape;
             * skipping hash+probe + collision walking is a big win for
             * both --vm and --jit (the JIT routes IR_LOAD_FIELD through
             * vm_step_jit, so this case is shared). */
            XSChunk *chk = &PROTO->chunk;
            int ip_idx = (int)(frame->ip - chk->code) - 1;
            if (VAL_TAG(obj) == XS_INST && obj->inst && obj->inst->fields
                && ip_idx >= 0 && ip_idx < chk->len) {
                if (!chk->ic_class) {
                    chk->ic_class = xs_calloc((size_t)chk->len,
                                              sizeof(struct XSClass *));
                    if (!chk->ic_version)
                        chk->ic_version = xs_calloc((size_t)chk->len,
                                                    sizeof(uint64_t));
                }
                XSClass *expected = chk->ic_class[ip_idx];
                uint64_t packed = chk->ic_version[ip_idx];
                int cached_cap    = (int)(packed >> 32);
                int cached_bucket = (int)(packed & 0xFFFFFFFFu);
                XSMap *fields = obj->inst->fields;
                if (expected && expected == obj->inst->class_
                    && cached_cap == fields->cap
                    && cached_bucket >= 0 && cached_bucket < fields->cap
                    && fields->keys[cached_bucket]
                    && strcmp(fields->keys[cached_bucket], name) == 0) {
                    Value *v = fields->vals[cached_bucket];
                    if (v) {
                        r = value_incref(v);
                        value_decref(obj); PUSH(r); break;
                    }
                }
                /* Cold or stale -- do the lookup, record (class, cap, bucket). */
                int bucket = -1;
                Value *v = map_get_at(fields, name, &bucket);
                if (v && bucket >= 0) {
                    chk->ic_class[ip_idx]   = obj->inst->class_;
                    chk->ic_version[ip_idx] =
                        ((uint64_t)(uint32_t)fields->cap << 32) |
                        (uint64_t)(uint32_t)bucket;
                    r = value_incref(v);
                    value_decref(obj); PUSH(r); break;
                }
                /* Field not on the instance directly -- fall through to
                 * the slow path so methods / inherited fallbacks still
                 * resolve correctly. */
            }
            if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
                Value *cid = map_get(obj->map, "_chan_id");
                int is_chan = (cid && VAL_TAG(cid) == XS_INT);
                if (is_chan && (strcmp(name, "_buf") == 0 ||
                                strcmp(name, "_cap") == 0 ||
                                strcmp(name, "_chan_id") == 0 ||
                                strcmp(name, "_type") == 0 ||
                                strcmp(name, "__type") == 0)) {
                    r = value_incref(XS_NULL_VAL);
                    value_decref(obj); PUSH(r); break;
                }
                Value *v = map_get(obj->map, name);
                if (v) {
                    r = value_incref(v);
                } else {
                    Value *methods = map_get(obj->map, "__methods");
                    if (methods && VAL_TAG(methods) == XS_MAP) {
                        Value *mv = map_get(methods->map, name);
                        if (mv) r = value_incref(mv);
                    }
                    if (!r) {
                        Value *impl = map_get(obj->map, "__impl__");
                        if (impl && VAL_TAG(impl) == XS_MAP) {
                            Value *mv = map_get(impl->map, name);
                            if (mv) r = value_incref(mv);
                        }
                    }
                }
            } else if (VAL_TAG(obj) == XS_ENUM_VAL && obj->en) {
                if (strcmp(name, "variant") == 0 || strcmp(name, "_tag") == 0)
                    r = xs_str(obj->en->variant);
                else if (strcmp(name, "type") == 0 || strcmp(name, "__type") == 0)
                    r = xs_str(obj->en->type_name);
            } else if ((VAL_TAG(obj) == XS_TUPLE || VAL_TAG(obj) == XS_ARRAY) && name[0] >= '0' && name[0] <= '9') {
                int64_t idx2 = atoll(name);
                if (idx2 >= 0 && idx2 < obj->arr->len)
                    r = value_incref(obj->arr->items[idx2]);
            } else if (VAL_TAG(obj) == XS_STRUCT_VAL && obj->st && obj->st->fields) {
                Value *v = map_get(obj->st->fields, name);
                if (v) r = value_incref(v);
            } else if (VAL_TAG(obj) == XS_INST && obj->inst && obj->inst->fields) {
                Value *v = map_get(obj->inst->fields, name);
                if (v) r = value_incref(v);
            }
            if (!r) r = value_incref(XS_NULL_VAL);
            value_decref(obj); PUSH(r); break;
        }
        case OP_STORE_FIELD: {
            const char *name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *val = POP(), *obj = POP();
            if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
                map_set(obj->map, name, val);
            } else if (VAL_TAG(obj) == XS_TUPLE) {
                value_decref(val); value_decref(obj);
                xs_runtime_error(span_zero(), "TypeError", NULL,
                                 "tuples are immutable; cannot assign field '%s'", name);
                break;
            }
            value_decref(val); value_decref(obj); break;
        }
        case OP_MAKE_MAP: {
            int n = (int)INSTR_C(instr); /* n key-value pairs */
            Value *m = xs_map_new();
            Value *tmp_s[512], **tmp = n*2 <= 512 ? tmp_s : malloc(n * 2 * sizeof(Value*));
            for (int i = n*2-1; i >= 0; i--) tmp[i] = POP();
            for (int i = 0; i < n; i++) {
                Value *k = tmp[i*2], *v = tmp[i*2+1];
                if (VAL_TAG(k) == XS_STR) map_set(m->map, k->s, v);
                value_decref(k); value_decref(v);
            }
            if (tmp != tmp_s) free(tmp);
            PUSH(m); break;
        }

        case OP_JUMP:
            frame->ip += INSTR_sBx(instr); break;
        case OP_JUMP_IF_FALSE: {
            Value *cond = POP();
            if (!value_truthy(cond)) frame->ip += INSTR_sBx(instr);
            value_decref(cond); break;
        }
        case OP_JUMP_IF_TRUE: {
            Value *cond = POP();
            if (value_truthy(cond)) frame->ip += INSTR_sBx(instr);
            value_decref(cond); break;
        }

        case OP_MAKE_CLOSURE: {
            int inner_idx = (int)VAL_INT(PROTO->chunk.consts[INSTR_Bx(instr)]);
            XSProto *inner = PROTO->inner[inner_idx];
            int nuv = inner->n_upvalues;
            Upvalue **uvs = nuv ? xs_malloc((size_t)nuv * sizeof(Upvalue*)) : NULL;
            for (int i = 0; i < nuv; i++) {
                UVDesc *d = &inner->uv_descs[i];
                uvs[i] = d->is_local
                    ? capture_upvalue(vm, &frame->base[d->index])
                    : CL->upvalues[d->index];
                uvs[i]->refcount++;
            }
            XSClosure *cl = xs_malloc(sizeof *cl);
            cl->proto    = inner; inner->refcount++;
            cl->upvalues = uvs;  cl->refcount = 1;
            Value *v     = xs_malloc(sizeof *v);
            v->tag = XS_CLOSURE; v->refcount = 1; v->cl = cl;
            PUSH(v); break;
        }

        case OP_CALL: {
            int argc   = (int)INSTR_C(instr);
            Value *callee = vm->sp[-argc - 1];
            /* Overload set: pick a candidate whose arity matches argc.
               If multiple match, the one whose declared arity equals
               argc wins; otherwise fall back to the last candidate
               whose min_arity <= argc <= max_arity. Mirrors interp. */
            if (VAL_TAG(callee) == XS_OVERLOAD && callee->overload) {
                XSArray *cs = callee->overload;
                Value *best = NULL;
                for (int oi = 0; oi < cs->len; oi++) {
                    Value *cand = cs->items[oi];
                    int min_a = 0, max_a = 0;
                    if (VAL_TAG(cand) == XS_FUNC) {
                        max_a = cand->fn->nparams;
                        min_a = max_a;
                        if (cand->fn->default_vals) {
                            for (int pi = 0; pi < max_a; pi++)
                                if (cand->fn->default_vals[pi]) min_a--;
                        }
                    } else if (VAL_TAG(cand) == XS_CLOSURE) {
                        max_a = cand->cl->proto->arity;
                        min_a = max_a;
                    } else {
                        min_a = 0; max_a = argc;
                    }
                    if (argc == max_a) { best = cand; break; }
                    if (argc >= min_a && argc <= max_a) best = cand;
                }
                if (best) {
                    /* Replace the overload value on the stack with the
                       chosen candidate so the existing dispatch arms
                       handle it directly. */
                    value_incref(best);
                    value_decref(callee);
                    vm->sp[-argc - 1] = best;
                    callee = best;
                }
            }
            if (VAL_TAG(callee) == XS_NATIVE) {
                Value **args = vm->sp - argc;
                Value *result = callee->native(NULL, args, argc);
                for (int i = 0; i < argc; i++) value_decref(POP());
                value_decref(POP()); /* callee */
                PUSH(result ? result : value_incref(XS_NULL_VAL));
            } else if (VAL_TAG(callee) == XS_CLOSURE) {
                Value *saved = callee;
                value_incref(saved);
                for (int i = -argc-1; i < -1; i++) vm->sp[i] = vm->sp[i+1];
                vm->sp--;
                value_decref(saved);
                if (call_frame_push(vm, saved, argc)) {
                    value_decref(saved);
                    return 1;
                }
                value_decref(saved);
                frame = FRAME;
            } else if (VAL_TAG(callee) == XS_MAP || VAL_TAG(callee) == XS_MODULE) {
                Value *fields = map_get(callee->map, "__fields");
                if (fields && VAL_TAG(fields) == XS_MAP) {
                    Value *inst = xs_map_new();
                    for (int j = 0; j < fields->map->cap; j++)
                        if (fields->map->keys[j])
                            map_set(inst->map, fields->map->keys[j],
                                    value_incref(fields->map->vals[j]));
                    Value *methods = map_get(callee->map, "__methods");
                    if (methods && VAL_TAG(methods) == XS_MAP)
                        for (int j = 0; j < methods->map->cap; j++)
                            if (methods->map->keys[j])
                                map_set(inst->map, methods->map->keys[j],
                                        value_incref(methods->map->vals[j]));
                    Value *cls_name = map_get(callee->map, "__name");
                    if (cls_name) map_set(inst->map, "__type", value_incref(cls_name));
                    Value *bases = map_get(callee->map, "__bases");
                    if (bases) {
                        map_set(inst->map, "__bases", value_incref(bases));
                        if (VAL_TAG(bases) == XS_ARRAY && bases->arr->len > 0) {
                            Value *super_inst = xs_map_new();
                            Value *base_cls = bases->arr->items[0];
                            if (VAL_TAG(base_cls) == XS_MAP) {
                                Value *bm = map_get(base_cls->map, "__methods");
                                if (bm && VAL_TAG(bm) == XS_MAP)
                                    for (int bj = 0; bj < bm->map->cap; bj++)
                                        if (bm->map->keys[bj])
                                            map_set(super_inst->map, bm->map->keys[bj],
                                                    value_incref(bm->map->vals[bj]));
                            }
                            map_set(super_inst->map, "__self", value_incref(inst));
                            map_set(inst->map, "super", super_inst);
                            value_decref(super_inst);
                        }
                    }
                    if (argc > 0 && fields->map->len > 0) {
                        int fi = 0;
                        for (int j = 0; j < fields->map->cap && fi < argc; j++)
                            if (fields->map->keys[j])
                                map_set(inst->map, fields->map->keys[j],
                                        value_incref(vm->sp[-argc + fi++]));
                    }
                    Value *init_fn = map_get(inst->map, "init");
                    Value *ca_s[256], **ctor_args = argc <= 256 ? ca_s : malloc(argc * sizeof(Value*));
                    for (int j = 0; j < argc; j++)
                        ctor_args[j] = value_incref(vm->sp[-argc + j]);
                    for (int j = 0; j < argc; j++) value_decref(POP());
                    value_decref(POP()); /* callee */
                    /* Verify the call's arg count against init's declared
                       arity. `expected` is the count without self. Skip
                       this check when init is missing (default-only ctor)
                       or has 0 params (legacy zero-arg init). */
                    int init_arity = -1;
                    if (init_fn && VAL_TAG(init_fn) == XS_CLOSURE && init_fn->cl->proto)
                        init_arity = init_fn->cl->proto->arity;
                    if (init_arity > 0 && argc != init_arity - 1) {
                        Span s = {0};
                        const char *cname = "<class>";
                        Value *cname_v = map_get(inst->map, "__type");
                        if (cname_v && VAL_TAG(cname_v) == XS_STR && cname_v->s)
                            cname = cname_v->s;
                        xs_runtime_error(s, "type mismatch", NULL,
                            "init for '%s' expected %d arg%s, got %d",
                            cname, init_arity - 1,
                            init_arity - 1 == 1 ? "" : "s", argc);
                        for (int j = 0; j < argc; j++) value_decref(ctor_args[j]);
                        if (ctor_args != ca_s) free(ctor_args);
                        value_decref(inst);
                        PUSH(value_incref(XS_NULL_VAL));
                        break;
                    }
                    if (init_fn && VAL_TAG(init_fn) == XS_NATIVE) {
                        Value *init_call_args[257];
                        init_call_args[0] = inst;
                        for (int j = 0; j < argc; j++)
                            init_call_args[j + 1] = ctor_args[j];
                        Value *ir = init_fn->native(NULL, init_call_args, argc + 1);
                        if (ir) value_decref(ir);
                        for (int j = 0; j < argc; j++) value_decref(ctor_args[j]);
                        if (ctor_args != ca_s) free(ctor_args);
                        PUSH(inst);
                    } else if (init_fn && VAL_TAG(init_fn) == XS_CLOSURE) {
                        PUSH(inst);
                        PUSH(value_incref(inst));
                        for (int j = 0; j < argc; j++) PUSH(ctor_args[j]);
                        if (ctor_args != ca_s) free(ctor_args);
                        value_incref(init_fn);
                        if (call_frame_push(vm, init_fn, argc + 1) == 0) {
                            value_decref(init_fn);
                            frame = FRAME;
                            vm->init_inst = value_incref(inst);
                            frame->owns_init_inst = 1;
                            break;
                        } else {
                            value_decref(init_fn);
                        }
                    } else {
                        for (int j = 0; j < argc; j++) value_decref(ctor_args[j]);
                        if (ctor_args != ca_s) free(ctor_args);
                        PUSH(inst);
                    }
                } else {
                    for (int j = 0; j < argc; j++) value_decref(POP());
                    value_decref(POP());
                    PUSH(value_incref(XS_NULL_VAL));
                }
            } else {
                fprintf(stderr, "call on non-callable (tag=%d)\n", VAL_TAG(callee));
                return 1;
            }
            break;
        }

        case OP_CALL_KW: {
            int nargs = (int)INSTR_A(instr);
            int nkw   = (int)INSTR_C(instr);
            /* stack: callee, pos_1..pos_n, key_1, val_1, ..., key_k, val_k */
            Value **kwslot = vm->sp - 2 * nkw;
            Value **posslot = kwslot - nargs;
            Value *callee = posslot[-1];
            XSProto *target = NULL;
            if (VAL_TAG(callee) == XS_CLOSURE) target = callee->cl->proto;
            if (target && target->param_names && target->n_params > 0) {
                int total = target->n_params;
                Value **merged = xs_malloc((size_t)total * sizeof(Value *));
                for (int i = 0; i < total; i++) merged[i] = NULL;
                int copy = nargs < total ? nargs : total;
                for (int i = 0; i < copy; i++)
                    merged[i] = value_incref(posslot[i]);
                for (int i = 0; i < nkw; i++) {
                    Value *k = kwslot[i * 2];
                    Value *v = kwslot[i * 2 + 1];
                    if (!k || VAL_TAG(k) != XS_STR) continue;
                    for (int p = 0; p < total; p++) {
                        if (target->param_names[p] &&
                            strcmp(target->param_names[p], k->s) == 0) {
                            if (merged[p]) value_decref(merged[p]);
                            merged[p] = value_incref(v);
                            break;
                        }
                    }
                }
                for (int i = 0; i < total; i++)
                    if (!merged[i]) merged[i] = value_incref(XS_NULL_VAL);
                /* tear down the old stack items (positional + kwargs) */
                for (int i = 0; i < nkw * 2; i++) value_decref(POP());
                for (int i = 0; i < nargs; i++) value_decref(POP());
                /* push merged positional args */
                for (int i = 0; i < total; i++) PUSH(merged[i]);
                free(merged);
                /* do the call */
                Value *saved = callee;
                value_incref(saved);
                for (int i = -total - 1; i < -1; i++) vm->sp[i] = vm->sp[i + 1];
                vm->sp--;
                value_decref(saved);
                if (call_frame_push(vm, saved, total)) {
                    value_decref(saved);
                    return 1;
                }
                value_decref(saved);
                frame = FRAME;
            } else {
                /* callee has no known param names (native, non-closure, or
                   unknown): drop kwargs and call with positional only. */
                for (int i = 0; i < nkw * 2; i++) value_decref(POP());
                /* positional args are now at top; reuse OP_CALL logic */
                if (VAL_TAG(callee) == XS_NATIVE) {
                    Value **args = vm->sp - nargs;
                    Value *result = callee->native(NULL, args, nargs);
                    for (int i = 0; i < nargs; i++) value_decref(POP());
                    value_decref(POP());
                    PUSH(result ? result : value_incref(XS_NULL_VAL));
                } else if (VAL_TAG(callee) == XS_CLOSURE) {
                    Value *saved = callee;
                    value_incref(saved);
                    for (int i = -nargs - 1; i < -1; i++) vm->sp[i] = vm->sp[i + 1];
                    vm->sp--;
                    value_decref(saved);
                    if (call_frame_push(vm, saved, nargs)) {
                        value_decref(saved);
                        return 1;
                    }
                    value_decref(saved);
                    frame = FRAME;
                } else {
                    fprintf(stderr, "call_kw on non-callable (tag=%d)\n", VAL_TAG(callee));
                    return 1;
                }
            }
            break;
        }

        case OP_TAIL_CALL: {
            int argc   = (int)INSTR_C(instr);
            Value *callee = vm->sp[-argc - 1];
            if (VAL_TAG(callee) == XS_NATIVE) {
                Value **args = vm->sp - argc;
                Value *result = callee->native(NULL, args, argc);
                for (int i = 0; i < argc; i++) value_decref(POP());
                value_decref(POP());
                upvalue_close_all(&vm->open_upvalues, frame->base);
                while (vm->sp > frame->base) value_decref(POP());
                value_decref(frame->closure_val);
                vm->frame_count--;
                if (vm->frame_count == 0) { value_decref(result); return 0; }
                frame = FRAME;
                PUSH(result ? result : value_incref(XS_NULL_VAL));
                break;
            }
            if (VAL_TAG(callee) != XS_CLOSURE) {
                fprintf(stderr, "tail call on non-callable\n");
                return 1;
            }
            Value *new_cv = callee;
            value_incref(new_cv);
            XSClosure *new_cl = new_cv->cl;
            if (argc != new_cl->proto->arity) {
                value_decref(new_cv);
                fprintf(stderr, "tail call arity mismatch\n");
                return 1;
            }
            Value *na_s[256], **new_args = argc <= 256 ? na_s : malloc(argc * sizeof(Value*));
            for (int i = argc-1; i >= 0; i--) new_args[i] = POP();
            value_decref(POP());
            upvalue_close_all(&vm->open_upvalues, frame->base);
            while (vm->sp > frame->base) value_decref(POP());
            Value *old_cv      = frame->closure_val;
            frame->closure_val = new_cv;
            frame->ip          = new_cl->proto->chunk.code;
            frame->base        = vm->sp;
            value_decref(old_cv);
            for (int i = 0; i < argc; i++) PUSH(new_args[i]);
            if (new_args != na_s) free(new_args);
            for (int i = argc; i < new_cl->proto->nlocals; i++) PUSH(xs_null());
            break;
        }

        case OP_RETURN: {
            if (frame->defer_return_ip) {
                Value *dret = POP();
                value_decref(dret);
                if (frame->defer_depth > 0) {
                    frame->defer_depth--;
                    frame->ip = frame->defer_stack[frame->defer_depth].defer_ip;
                } else if (vm->pending_throw_exc &&
                           vm->pending_throw_frame == vm->frame_count - 1) {
                    /* This frame was mid-throw; all defers have run.
                       Drop the frame and resume the unwind with the
                       parked exception value. */
                    Value *exc = vm->pending_throw_exc;
                    vm->pending_throw_exc = NULL;
                    vm->pending_throw_frame = -1;
                    frame->defer_return_ip = NULL;
                    upvalue_close_all(&vm->open_upvalues, frame->base);
                    while (vm->sp > frame->base) value_decref(POP());
                    value_decref(frame->closure_val);
                    vm->frame_count--;
                    int handled = 0;
                    while (vm->frame_count > 0) {
                        CallFrame *cf = &vm->frames[vm->frame_count - 1];
                        if (cf->try_depth > 0) {
                            TryEntry *te = &cf->try_stack[--cf->try_depth];
                            if (g_xs_in_try > 0) g_xs_in_try--;
                            while (vm->sp > te->stack_top) value_decref(POP());
                            PUSH(exc);
                            frame = cf;
                            frame->ip = te->catch_ip;
                            handled = 1;
                            break;
                        }
                        if (cf->defer_depth > 0) {
                            vm->pending_throw_frame = vm->frame_count - 1;
                            vm->pending_throw_exc = exc;
                            cf->defer_return_ip = cf->ip;
                            cf->defer_depth--;
                            cf->ip = cf->defer_stack[cf->defer_depth].defer_ip;
                            frame = cf;
                            handled = 1;
                            break;
                        }
                        upvalue_close_all(&vm->open_upvalues, cf->base);
                        while (vm->sp > cf->base) value_decref(POP());
                        value_decref(cf->closure_val);
                        vm->frame_count--;
                    }
                    if (!handled) {
                        char *s = value_str(exc);
                        fprintf(stderr, "uncaught: %s\n", s);
                        free(s);
                        value_decref(exc);
                        return 1;
                    }
                } else {
                    frame->ip = frame->defer_return_ip;
                    frame->defer_return_ip = NULL;
                }
                break;
            }
            Value *result = POP();
            if (frame->is_generator && frame->yield_arr) {
                value_decref(result);
                Value *gen = xs_map_new();
                Value *type_v = xs_str("generator");
                map_set(gen->map, "__type", type_v); value_decref(type_v);
                map_set(gen->map, "_yields", frame->yield_arr);
                value_decref(frame->yield_arr);
                frame->yield_arr = NULL;
                Value *idx_v = xs_int(0);
                map_set(gen->map, "_index", idx_v); value_decref(idx_v);
                Value *done_v = value_incref(XS_FALSE_VAL);
                map_set(gen->map, "_done", done_v); value_decref(done_v);
                result = gen;
            }
            /* run deferred blocks before returning */
            if (frame->defer_depth > 0) {
                PUSH(result);
                frame->defer_return_ip = frame->ip - 1;
                frame->defer_depth--;
                frame->ip = frame->defer_stack[frame->defer_depth].defer_ip;
                break;
            }
            int returning_frame_owned_init_inst = frame->owns_init_inst;
            upvalue_close_all(&vm->open_upvalues, frame->base);
            while (vm->sp > frame->base) value_decref(POP());
            value_decref(frame->closure_val);
            vm->frame_count--;
            if (vm->frame_count <= stop_frame) {
                if (stop_frame > 0) {
                    PUSH(result);
                    return 0;
                }
                value_decref(result);
                if (!vm->main_called) {
                    Value *main_fn = map_get(vm->globals, "main");
                    if (main_fn && VAL_TAG(main_fn) == XS_CLOSURE) {
                        vm->main_called = 1;
                        value_incref(main_fn);
                        if (call_frame_push(vm, main_fn, 0) == 0) {
                            value_decref(main_fn);
                            frame = FRAME;
                            break;
                        }
                        value_decref(main_fn);
                    }
                }
                return 0;
            }
            frame = FRAME;
            PUSH(result);
            /* Only the constructor's own init frame consumes init_inst.
               Without this guard, a nested super.init(...) returning
               while init_inst is still live strips operand-stack values
               from the outer init's frame and the original instance
               surfaces with its fields wiped. */
            if (vm->init_inst && returning_frame_owned_init_inst) {
                Value *init_retval = POP();
                value_decref(init_retval);
                Value *saved_inst = POP();
                value_decref(saved_inst);
                PUSH(vm->init_inst);
                vm->init_inst = NULL;
            }
            if (vm->spawn_task) {
                Value *spawn_retval = POP();
                Value *saved_task = POP();
                { Value *sv = xs_str("done"); map_set(saved_task->map, "_status", sv); value_decref(sv); }
                map_set(saved_task->map, "_result", spawn_retval);
                value_decref(spawn_retval);
                value_decref(vm->spawn_task);
                vm->spawn_task = NULL;
                PUSH(saved_task);
            }
            break;
        }

        case OP_MAKE_RANGE: {
            int inclusive = (int)INSTR_A(instr);
            Value *end_v = POP(), *start_v = POP();
            int64_t s = VAL_TAG(start_v)==XS_INT ? VAL_INT(start_v) : (int64_t)start_v->f;
            int64_t e = VAL_TAG(end_v)==XS_INT   ? VAL_INT(end_v)   : (int64_t)end_v->f;
            Value *r = xs_range(s, e, inclusive);
            value_decref(start_v); value_decref(end_v); PUSH(r); break;
        }

        case OP_ITER_LEN: {
            Value *iter = POP(); Value *r;
            if (VAL_TAG(iter)==XS_ARRAY||VAL_TAG(iter)==XS_TUPLE) r = xs_int(iter->arr->len);
            else if (VAL_TAG(iter)==XS_STR) r = xs_int((int64_t)strlen(iter->s));
            else if (VAL_TAG(iter)==XS_MAP && map_get(iter->map, "__type") &&
                     map_get(iter->map, "__type")->tag == XS_STR &&
                     strcmp(map_get(iter->map, "__type")->s, "generator") == 0) {
                Value *yields = map_get(iter->map, "_yields");
                r = xs_int(yields && VAL_TAG(yields) == XS_ARRAY ? yields->arr->len : 0);
            }
            else if (VAL_TAG(iter)==XS_MAP && map_get(iter->map, "_chan_id") &&
                     VAL_TAG(map_get(iter->map, "_chan_id")) == XS_INT) {
                /* Channel: snapshot the current buffered length; ITER_GET
                   drains that many via try_recv. */
                extern int xs_chan_len(Value *);
                r = xs_int(xs_chan_len(iter));
            }
            else if (VAL_TAG(iter)==XS_MAP||VAL_TAG(iter)==XS_MODULE) r = xs_int(iter->map->len);
            else if (VAL_TAG(iter)==XS_RANGE && iter->range) {
                int64_t span = iter->range->end - iter->range->start;
                if (iter->range->inclusive) span += (span >= 0) ? 1 : -1;
                int64_t step = iter->range->step ? iter->range->step : 1;
                int64_t len;
                if (step > 0) len = (span > 0) ? (span + step - 1) / step : 0;
                else           len = (span < 0) ? (-span + (-step) - 1) / (-step) : 0;
                r = xs_int(len);
            } else r = xs_int(0);
            value_decref(iter); PUSH(r); break;
        }
        case OP_ITER_GET: {
            int want_pairs = INSTR_A(instr);
            Value *idx = POP(), *iter = POP(); Value *r;
            int64_t i = VAL_TAG(idx)==XS_INT ? VAL_INT(idx) : (int64_t)idx->f;
            if (VAL_TAG(iter)==XS_ARRAY||VAL_TAG(iter)==XS_TUPLE) {
                r = (i>=0&&i<iter->arr->len) ? value_incref(iter->arr->items[i]) : value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(iter)==XS_STR) {
                const char *s = iter->s;
                int64_t slen = (int64_t)strlen(s);
                if (i>=0&&i<slen) { char buf[2]={s[i],0}; r=xs_str(buf); }
                else r = value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(iter)==XS_MAP && iter->map &&
                       map_get(iter->map, "__type") &&
                       map_get(iter->map, "__type")->tag == XS_STR &&
                       strcmp(map_get(iter->map, "__type")->s, "generator") == 0) {
                Value *yields = map_get(iter->map, "_yields");
                if (yields && VAL_TAG(yields) == XS_ARRAY && i >= 0 && i < yields->arr->len)
                    r = value_incref(yields->arr->items[i]);
                else
                    r = value_incref(XS_NULL_VAL);
            } else if (VAL_TAG(iter)==XS_MAP && iter->map &&
                       map_get(iter->map, "_chan_id") &&
                       VAL_TAG(map_get(iter->map, "_chan_id")) == XS_INT) {
                /* Channel iteration: ignore idx, pop the next buffered
                   value via try_recv. ITER_LEN snapshotted the count so
                   the loop bound matches the drain. */
                extern Value *xs_chan_try_recv(Value *);
                r = xs_chan_try_recv(iter);
            } else if ((VAL_TAG(iter)==XS_MAP||VAL_TAG(iter)==XS_MODULE) && iter->map) {
                int64_t ki = 0;
                r = value_incref(XS_NULL_VAL);
                for (int j = 0; j < iter->map->cap; j++) {
                    if (iter->map->keys[j]) {
                        if (ki == i) {
                            if (want_pairs) {
                                r = xs_tuple_new();
                                Value *ks = xs_str(iter->map->keys[j]);
                                Value *val = iter->map->vals[j];
                                array_push(r->arr, ks);
                                array_push(r->arr, val ? val : XS_NULL_VAL);
                                value_decref(ks);
                            } else {
                                r = xs_str(iter->map->keys[j]);
                            }
                            break;
                        }
                        ki++;
                    }
                }
            } else if (VAL_TAG(iter)==XS_RANGE && iter->range) {
                int64_t step = iter->range->step ? iter->range->step : 1;
                r = xs_int(iter->range->start + i * step);
            } else r = value_incref(XS_NULL_VAL);
            value_decref(idx); value_decref(iter); PUSH(r); break;
        }

        /* method call */
        case OP_METHOD_CALL: {
            int mc_argc   = (int)INSTR_A(instr);
            const char *mc_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *mc_obj = vm->sp[-mc_argc - 1];
            Value **mc_args = vm->sp - mc_argc;
            Value *mc_result = NULL;
            int mc_called = 0; /* set to 1 if we pushed a call frame */

            /* inline cache: key the site by the caller proto + IP offset so
               it survives across vm_invoke reentry without aliasing between
               protos. */
            int mc_site_id = ic_site_id(
                (int)((uintptr_t)PROTO ^
                      (uintptr_t)(frame->ip - PROTO->chunk.code)));
            Value *mc_cached_fn = NULL;
            uint8_t mc_cached_is_module = 0;
            uint8_t mc_cached_needs_self = 0;
            int mc_cache_hit = 0;

            if (VAL_TAG(mc_obj) == XS_MAP || VAL_TAG(mc_obj) == XS_MODULE) {
                /* hot path: the cache already has the resolved callable
                   and the dispatch flags (module-vs-self, self arity)
                   from the first miss, so we can skip every map_get
                   probe below and drop straight into invocation. The
                   method-name key is the const-pool pointer so hit
                   checks are a pointer compare, not a strcmp. */
                {
                    int64_t type_tag = (int64_t)(intptr_t)mc_obj->map;
                    Value *cached = ic_lookup_ex(mc_site_id, type_tag, mc_name,
                                                 &mc_cached_is_module,
                                                 &mc_cached_needs_self);
                    if (cached && (VAL_TAG(cached) == XS_CLOSURE ||
                                   VAL_TAG(cached) == XS_NATIVE ||
                                   VAL_TAG(cached) == XS_FUNC)) {
                        mc_cached_fn = cached;
                        mc_cache_hit = 1;
                        goto map_generic_method;
                    }
                }
                /* Prefer a user-defined method stored on the map over
                   generic fallbacks. Modules like `fs` define their own
                   size/len that must not be shadowed by the map method. */
                {
                    Value *user_fn = map_get(mc_obj->map, mc_name);
                    if (user_fn && (VAL_TAG(user_fn) == XS_NATIVE ||
                                    VAL_TAG(user_fn) == XS_CLOSURE ||
                                    VAL_TAG(user_fn) == XS_FUNC)) {
                        goto map_generic_method;
                    }
                }
                Value *_gen_type = map_get(mc_obj->map, "__type");
                if (_gen_type && VAL_TAG(_gen_type) == XS_STR &&
                    strcmp(_gen_type->s, "generator") == 0 &&
                    strcmp(mc_name, "next") == 0) {
                    Value *yields = map_get(mc_obj->map, "_yields");
                    Value *idx_v  = map_get(mc_obj->map, "_index");
                    int idx = idx_v && VAL_TAG(idx_v) == XS_INT ? (int)VAL_INT(idx_v) : 0;
                    mc_result = xs_map_new();
                    if (yields && VAL_TAG(yields) == XS_ARRAY && idx < yields->arr->len) {
                        map_set(mc_result->map, "value", value_incref(yields->arr->items[idx]));
                        Value *dv = value_incref(XS_FALSE_VAL);
                        map_set(mc_result->map, "done", dv); value_decref(dv);
                        Value *new_idx = xs_int(idx + 1);
                        map_set(mc_obj->map, "_index", new_idx); value_decref(new_idx);
                    } else {
                        map_set(mc_result->map, "value", value_incref(XS_NULL_VAL));
                        Value *dv = value_incref(XS_TRUE_VAL);
                        map_set(mc_result->map, "done", dv); value_decref(dv);
                        Value *done_v = value_incref(XS_TRUE_VAL);
                        map_set(mc_obj->map, "_done", done_v); value_decref(done_v);
                    }
                } else if (strcmp(mc_name,"keys")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]){
                            Value *k=xs_str(mc_obj->map->keys[j]);
                            array_push(arr->arr,k); value_decref(k);
                        }
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"values")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]) array_push(arr->arr,value_incref(mc_obj->map->vals[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"entries")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]){
                            Value *pair=xs_tuple_new();
                            array_push(pair->arr,xs_str(mc_obj->map->keys[j]));
                            array_push(pair->arr,value_incref(mc_obj->map->vals[j]));
                            array_push(arr->arr,pair);
                        }
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"map")==0 && mc_argc>=1) {
                    Value *out = xs_map_new();
                    for (int j = 0; j < mc_obj->map->cap; j++) {
                        if (!mc_obj->map->keys[j]) continue;
                        Value *k = xs_str(mc_obj->map->keys[j]);
                        Value *v = mc_obj->map->vals[j];
                        Value *a[2] = { k, v ? v : XS_NULL_VAL };
                        Value *r = vm_invoke(vm, mc_args[0], a, 2);
                        frame = FRAME;
                        if (r) { map_set(out->map, mc_obj->map->keys[j], r); value_decref(r); }
                        value_decref(k);
                    }
                    mc_result = out;
                } else if (strcmp(mc_name,"filter")==0 && mc_argc>=1) {
                    Value *out = xs_map_new();
                    for (int j = 0; j < mc_obj->map->cap; j++) {
                        if (!mc_obj->map->keys[j]) continue;
                        Value *k = xs_str(mc_obj->map->keys[j]);
                        Value *v = mc_obj->map->vals[j];
                        Value *a[2] = { k, v ? v : XS_NULL_VAL };
                        Value *r = vm_invoke(vm, mc_args[0], a, 2);
                        frame = FRAME;
                        int keep = r && value_truthy(r);
                        if (r) value_decref(r);
                        if (keep && v) map_set(out->map, mc_obj->map->keys[j], value_incref(v));
                        value_decref(k);
                    }
                    mc_result = out;
                } else if (strcmp(mc_name,"merge")==0 && mc_argc>=1 &&
                           (VAL_TAG(mc_args[0])==XS_MAP || VAL_TAG(mc_args[0])==XS_MODULE)) {
                    Value *out = xs_map_new();
                    for (int j = 0; j < mc_obj->map->cap; j++)
                        if (mc_obj->map->keys[j])
                            map_set(out->map, mc_obj->map->keys[j],
                                    value_incref(mc_obj->map->vals[j]));
                    for (int j = 0; j < mc_args[0]->map->cap; j++)
                        if (mc_args[0]->map->keys[j])
                            map_set(out->map, mc_args[0]->map->keys[j],
                                    value_incref(mc_args[0]->map->vals[j]));
                    mc_result = out;
                } else if (strcmp(mc_name,"len")==0||strcmp(mc_name,"size")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        Value *buf = map_get(mc_obj->map, "_buf");
                        mc_result = xs_int(buf && VAL_TAG(buf) == XS_ARRAY ? buf->arr->len : 0);
                    } else
                    mc_result=xs_int(mc_obj->map->len);
                } else if (strcmp(mc_name,"has")==0||strcmp(mc_name,"contains_key")==0) {
                    mc_result=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR&&map_get(mc_obj->map,mc_args[0]->s))
                        ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"get")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    Value *v=map_get(mc_obj->map,mc_args[0]->s);
                    mc_result=v?value_incref(v):(mc_argc>=2?value_incref(mc_args[1]):value_incref(XS_NULL_VAL));
                } else if (strcmp(mc_name,"set")==0&&mc_argc>=2&&VAL_TAG(mc_args[0])==XS_STR) {
                    /* check if this is a plugin global.set: delegate to native */
                    Value *set_fn = map_get(mc_obj->map, "set");
                    if (set_fn && VAL_TAG(set_fn) == XS_NATIVE) {
                        goto map_generic_method;
                    }
                    map_set(mc_obj->map,mc_args[0]->s,mc_args[1]);
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"delete")==0||strcmp(mc_name,"remove")==0) {
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                        Value *nv = value_incref(XS_NULL_VAL);
                        map_set(mc_obj->map,mc_args[0]->s,nv);
                        value_decref(nv);
                    }
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"send")==0 && mc_argc>=1) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern int xs_chan_send(Value *, Value *);
                        if (!xs_chan_send(mc_obj, mc_args[0])) {
                            Value *err = xs_error_new("ChannelClosed",
                                "send on closed channel", NULL);
                            g_xs_pending_throw = err;
                            mc_result = value_incref(XS_NULL_VAL);
                        } else {
                            mc_result = value_incref(XS_NULL_VAL);
                        }
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"recv")==0 || strcmp(mc_name,"try_recv")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        int nonblocking = (mc_name[0] == 't');
                        if (nonblocking) {
                            extern Value *xs_chan_try_recv(Value *);
                            mc_result = xs_chan_try_recv(mc_obj);
                        } else {
                            extern Value *xs_chan_recv(Value *, struct Interp *);
                            mc_result = xs_chan_recv(mc_obj, NULL);
                        }
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"close")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern void xs_chan_close(Value *);
                        xs_chan_close(mc_obj);
                        mc_result = value_incref(XS_NULL_VAL);
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"is_closed")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern int xs_chan_is_closed(Value *);
                        mc_result = xs_bool(xs_chan_is_closed(mc_obj));
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"cap")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern int xs_chan_cap(Value *);
                        mc_result = xs_int(xs_chan_cap(mc_obj));
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"is_empty")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern int xs_chan_len(Value *);
                        mc_result = xs_bool(xs_chan_len(mc_obj) == 0);
                    } else {
                        mc_result = xs_bool(mc_obj->map->len == 0);
                    }
                } else if (strcmp(mc_name,"is_full")==0) {
                    Value *ch_type = map_get(mc_obj->map, "__type");
                    if (ch_type && VAL_TAG(ch_type) == XS_STR && strcmp(ch_type->s, "channel") == 0) {
                        extern int xs_chan_is_full(Value *);
                        mc_result = xs_bool(xs_chan_is_full(mc_obj));
                    } else goto map_generic_method;
                } else if (strcmp(mc_name,"merge")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_MAP) {
                    for(int j=0;j<mc_args[0]->map->cap;j++){
                        if(mc_args[0]->map->keys[j])
                            map_set(mc_obj->map,mc_args[0]->map->keys[j],value_incref(mc_args[0]->map->vals[j]));
                    }
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"clone")==0||strcmp(mc_name,"copy")==0) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j])
                            map_set(m->map,mc_obj->map->keys[j],value_incref(mc_obj->map->vals[j]));
                    }
                    mc_result=m;
                } else if (strcmp(mc_name,"items")==0) {
                    /* alias for entries */
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]){
                            Value *pair=xs_tuple_new();
                            array_push(pair->arr,xs_str(mc_obj->map->keys[j]));
                            array_push(pair->arr,value_incref(mc_obj->map->vals[j]));
                            array_push(arr->arr,pair);
                        }
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"has_key")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    mc_result=map_get(mc_obj->map,mc_args[0]->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"intersection")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_MAP) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]&&map_get(mc_args[0]->map,mc_obj->map->keys[j]))
                            map_set(m->map,mc_obj->map->keys[j],value_incref(mc_obj->map->vals[j]));
                    }
                    mc_result=m;
                } else if (strcmp(mc_name,"union")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_MAP) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j])
                            map_set(m->map,mc_obj->map->keys[j],value_incref(mc_obj->map->vals[j]));
                    }
                    for(int j=0;j<mc_args[0]->map->cap;j++){
                        if(mc_args[0]->map->keys[j]&&!map_get(m->map,mc_args[0]->map->keys[j]))
                            map_set(m->map,mc_args[0]->map->keys[j],value_incref(mc_args[0]->map->vals[j]));
                    }
                    mc_result=m;
                } else if (strcmp(mc_name,"difference")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_MAP) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]&&!map_get(mc_args[0]->map,mc_obj->map->keys[j]))
                            map_set(m->map,mc_obj->map->keys[j],value_incref(mc_obj->map->vals[j]));
                    }
                    mc_result=m;
                } else if (strcmp(mc_name,"most_common")==0) {
                    int64_t n2=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?VAL_INT(mc_args[0]):mc_obj->map->len;
                    /* collect entries, sort by value descending, return top n */
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->map->cap;j++){
                        if(mc_obj->map->keys[j]){
                            Value *pair=xs_tuple_new();
                            array_push(pair->arr,xs_str(mc_obj->map->keys[j]));
                            array_push(pair->arr,value_incref(mc_obj->map->vals[j]));
                            array_push(arr->arr,pair);
                        }
                    }
                    /* bubble sort descending by second element */
                    int alen=arr->arr->len;
                    for(int j=0;j<alen-1;j++) for(int k=0;k<alen-1-j;k++){
                        Value *va=arr->arr->items[k]->arr->items[1];
                        Value *vb=arr->arr->items[k+1]->arr->items[1];
                        if(value_cmp(va,vb)<0){
                            Value *tmp=arr->arr->items[k]; arr->arr->items[k]=arr->arr->items[k+1]; arr->arr->items[k+1]=tmp;
                        }
                    }
                    if(n2<alen) arr->arr->len=(int)n2;
                    mc_result=arr;
                } else if (strcmp(mc_name,"elapsed")==0) {
                    Value *start_v = map_get(mc_obj->map, "_start");
                    if (start_v && (VAL_TAG(start_v) == XS_FLOAT || VAL_TAG(start_v) == XS_INT)) {
                        struct timespec _ts; clock_gettime(CLOCK_REALTIME, &_ts);
                        double now = (double)_ts.tv_sec + (double)_ts.tv_nsec/1e9;
                        double start = VAL_TAG(start_v) == XS_FLOAT ? start_v->f : (double)VAL_INT(start_v);
                        mc_result = xs_float(now - start);
                    } else mc_result = value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"to_map")==0) {
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"unwrap")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    Value *val_v = map_get(mc_obj->map, "_val");
                    if (tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s, "Err")==0) {
                        char *es = val_v ? value_str(val_v) : xs_strdup("Err");
                        fprintf(stderr, "unwrap called on Err: %s\n", es); free(es);
                        mc_result = value_incref(XS_NULL_VAL);
                    } else if (tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s, "None")==0) {
                        fprintf(stderr, "unwrap called on None\n");
                        mc_result = value_incref(XS_NULL_VAL);
                    } else {
                        mc_result = val_v ? value_incref(val_v) : value_incref(XS_NULL_VAL);
                    }
                } else if (strcmp(mc_name,"unwrap_or")==0&&mc_argc>=1) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    Value *val_v = map_get(mc_obj->map, "_val");
                    int is_err = tag_v && VAL_TAG(tag_v) == XS_STR &&
                        (strcmp(tag_v->s,"Err")==0 || strcmp(tag_v->s,"None")==0);
                    mc_result = is_err ? value_incref(mc_args[0]) : (val_v ? value_incref(val_v) : value_incref(XS_NULL_VAL));
                } else if (strcmp(mc_name,"is_ok")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    mc_result = xs_bool(tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Ok")==0);
                } else if (strcmp(mc_name,"is_err")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    mc_result = xs_bool(tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Err")==0);
                } else if (strcmp(mc_name,"is_some")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    mc_result = xs_bool(tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Some")==0);
                } else if (strcmp(mc_name,"is_none")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    mc_result = xs_bool(tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"None")==0);
                } else if (strcmp(mc_name,"ok")==0) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    Value *val_v = map_get(mc_obj->map, "_val");
                    if (tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Ok")==0)
                        mc_result = val_v ? value_incref(val_v) : value_incref(XS_NULL_VAL);
                    else mc_result = value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"or_else")==0&&mc_argc>=1) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    if (tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Err")==0) {
                        Value *val_v = map_get(mc_obj->map, "_val");
                        Value *arg = val_v ? val_v : XS_NULL_VAL;
                        mc_result = vm_invoke(vm, mc_args[0], &arg, 1);
                        frame = FRAME;
                        if (!mc_result) mc_result = value_incref(XS_NULL_VAL);
                    } else mc_result = value_incref(mc_obj);
                } else if (strcmp(mc_name,"map_err")==0&&mc_argc>=1) {
                    Value *tag_v = map_get(mc_obj->map, "_tag");
                    if (tag_v && VAL_TAG(tag_v) == XS_STR && strcmp(tag_v->s,"Err")==0) {
                        Value *val_v = map_get(mc_obj->map, "_val");
                        Value *arg = val_v ? val_v : XS_NULL_VAL;
                        Value *new_err = vm_invoke(vm, mc_args[0], &arg, 1);
                        frame = FRAME;
                        Value *m = xs_map_new();
                        Value *etag = xs_str("Err");
                        map_set(m->map, "_tag", etag); value_decref(etag);
                        if (new_err) { map_set(m->map, "_val", new_err); value_decref(new_err); }
                        mc_result = m;
                    } else mc_result = value_incref(mc_obj);
                } else if (strcmp(mc_name,"is_a")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    Value *type_name = map_get(mc_obj->map, "__type");
                    int match = type_name && VAL_TAG(type_name) == XS_STR && strcmp(type_name->s, mc_args[0]->s)==0;
                    mc_result = xs_bool(match);
                } else if (strcmp(mc_name,"subscribe")==0||strcmp(mc_name,"reset")==0||
                           strcmp(mc_name,"peek")==0||strcmp(mc_name,"step")==0||
                           strcmp(mc_name,"elapsed_ms")==0) {
                    mc_result = value_incref(XS_NULL_VAL);
                } else { map_generic_method: {
                    Value *fn = mc_cached_fn;
                    if (!fn) fn = map_get(mc_obj->map, mc_name);
                    if (!fn) {
                        Value *methods = map_get(mc_obj->map, "__methods");
                        if (methods && VAL_TAG(methods) == XS_MAP)
                            fn = map_get(methods->map, mc_name);
                    }
                    if (!fn) {
                        Value *impl = map_get(mc_obj->map, "__impl__");
                        if (impl && VAL_TAG(impl) == XS_MAP)
                            fn = map_get(impl->map, mc_name);
                    }
                    /* look up methods on the type (for struct impl) */
                    if (!fn) {
                        Value *type_name = map_get(mc_obj->map, "__type");
                        if (type_name && VAL_TAG(type_name) == XS_STR) {
                            Value *type_val = map_get(vm->globals, type_name->s);
                            if (type_val && VAL_TAG(type_val) == XS_MAP) {
                                Value *tm = map_get(type_val->map, "__methods");
                                if (tm && VAL_TAG(tm) == XS_MAP)
                                    fn = map_get(tm->map, mc_name);
                                if (!fn) {
                                    Value *ti = map_get(type_val->map, "__impl__");
                                    if (ti && VAL_TAG(ti) == XS_MAP)
                                        fn = map_get(ti->map, mc_name);
                                }
                            }
                        }
                    }
                    if (fn && (VAL_TAG(fn) == XS_CLOSURE || VAL_TAG(fn) == XS_NATIVE)) {
                        int is_module_call;
                        int needs_self;
                        if (mc_cache_hit) {
                            /* flags came from the cache: no map_get probes */
                            is_module_call = mc_cached_is_module;
                            needs_self     = mc_cached_needs_self;
                        } else {
                            is_module_call = (VAL_TAG(mc_obj) == XS_MODULE) ||
                                (VAL_TAG(mc_obj) == XS_MAP && !map_get(mc_obj->map, "__type") &&
                                 !map_get(mc_obj->map, "__methods") && !map_get(mc_obj->map, "__fields") &&
                                 !map_get(mc_obj->map, "__self"));
                            /* `super` proxy: has __self pointing at the
                               real instance. The dispatch path below
                               will swap it in for self before calling. */
                            /* Check if first param is 'self' for struct/class methods */
                            needs_self = 0;
                            if (VAL_TAG(fn) == XS_CLOSURE) {
                                int fn_arity = fn->cl->proto->arity;
                                if (fn_arity < 0) fn_arity = -(fn_arity + 1);
                                needs_self = (fn_arity == mc_argc + 1);
                            }
                            /* Populate the inline cache so the next call with
                               the same receiver + method name hits fast and
                               can skip the probes above. */
                            ic_update_ex(mc_site_id,
                                         (int64_t)(intptr_t)mc_obj->map,
                                         mc_name, fn,
                                         (uint8_t)is_module_call,
                                         (uint8_t)needs_self);
                        }
                        if (VAL_TAG(fn) == XS_CLOSURE && needs_self && !is_module_call) {
                            /* super proxy: replace self with __self */
                            Value *self_ref = map_get(mc_obj->map, "__self");
                            if (self_ref) {
                                value_incref(self_ref);
                                value_decref(vm->sp[-mc_argc - 1]);
                                vm->sp[-mc_argc - 1] = self_ref;
                            }
                            /* self is on stack below args */
                            Value *fn_val = value_incref(fn);
                            if (call_frame_push(vm, fn_val, mc_argc + 1)) {
                                value_decref(fn_val); return 1;
                            }
                            value_decref(fn_val); frame = FRAME;
                            mc_called = 1;
                        } else if (VAL_TAG(fn) == XS_NATIVE) {
                            if (is_module_call) {
                                /* module call: don't pass module as self */
                                Value *r2 = fn->native(NULL, mc_args, mc_argc);
                                for (int j = 0; j < mc_argc; j++) value_decref(POP());
                                value_decref(POP());
                                PUSH(r2 ? r2 : value_incref(XS_NULL_VAL));
                            } else {
                                Value *nargs[17];
                                nargs[0] = mc_obj;
                                int total = 1 + mc_argc;
                                if (total > 17) total = 17;
                                for (int j = 0; j < mc_argc && j < 16; j++) nargs[j+1] = mc_args[j];
                                Value *r2 = fn->native(NULL, nargs, total);
                                for (int j = 0; j < mc_argc; j++) value_decref(POP());
                                value_decref(POP());
                                PUSH(r2 ? r2 : value_incref(XS_NULL_VAL));
                            }
                        } else {
                            /* closure without self: treat as plain call */
                            value_decref(mc_obj);
                            vm->sp[-mc_argc - 1] = value_incref(fn);
                            Value *sv = vm->sp[-mc_argc - 1]; value_incref(sv);
                            for (int j = -mc_argc-1; j < -1; j++) vm->sp[j] = vm->sp[j+1];
                            vm->sp--; value_decref(sv);
                            if (call_frame_push(vm, sv, mc_argc)) { value_decref(sv); return 1; }
                            value_decref(sv); frame = FRAME;
                            mc_called = 1;
                        }
                        break;
                    }
                    /* No method found on this map / module / instance.
                       Raise rather than silently returning null - matches
                       interp behaviour and surfaces typos like fs.lst()
                       instead of leaking nulls down the call chain.
                       The dispatch loop's top-of-iteration check on
                       g_xs_pending_throw will unwind on the next pass. */
                    {
                        char *repr = value_repr(mc_obj);
                        char label[160];
                        snprintf(label, sizeof label,
                                 "no method '%s' on type '%s'",
                                 mc_name, repr ? repr : "?");
                        xs_runtime_error(span_zero(), label, NULL,
                                         "value of type '%s' has no method '%s'",
                                         repr ? repr : "?", mc_name);
                        free(repr);
                        for (int j = 0; j < mc_argc; j++) value_decref(POP());
                        value_decref(POP());
                        PUSH(value_incref(XS_NULL_VAL));
                        break;
                    }
                }}
            }

            else if (VAL_TAG(mc_obj) == XS_SIGNAL && mc_obj->signal) {
                XSSignal *sig = mc_obj->signal;
                if (strcmp(mc_name,"get")==0 || strcmp(mc_name,"value")==0) {
                    if (sig->compute) {
                        mc_result = vm_invoke(vm, sig->compute, NULL, 0);
                        frame = FRAME;
                        if (!mc_result) mc_result = value_incref(XS_NULL_VAL);
                    } else {
                        mc_result = sig->value ? value_incref(sig->value) : value_incref(XS_NULL_VAL);
                    }
                } else if (strcmp(mc_name,"set")==0 && mc_argc>=1) {
                    if (sig->value) value_decref(sig->value);
                    sig->value = value_incref(mc_args[0]);
                    if (!sig->notifying) {
                        sig->notifying = 1;
                        for (int j = 0; j < sig->nsubs; j++) {
                            Value *r = vm_invoke(vm, sig->subscribers[j], mc_args, 1);
                            frame = FRAME;
                            if (r) value_decref(r);
                        }
                        sig->notifying = 0;
                    }
                    mc_result = value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"subscribe")==0 && mc_argc>=1) {
                    if (sig->nsubs >= sig->subcap) {
                        sig->subcap = sig->subcap ? sig->subcap * 2 : 4;
                        sig->subscribers = xs_realloc(sig->subscribers,
                            (size_t)sig->subcap * sizeof(Value*));
                    }
                    sig->subscribers[sig->nsubs++] = value_incref(mc_args[0]);
                    mc_result = value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"update")==0 && mc_argc>=1) {
                    Value *cur = sig->value ? sig->value : XS_NULL_VAL;
                    Value *nv = vm_invoke(vm, mc_args[0], &cur, 1);
                    frame = FRAME;
                    if (nv) {
                        if (sig->value) value_decref(sig->value);
                        sig->value = nv;
                    }
                    mc_result = value_incref(XS_NULL_VAL);
                } else {
                    mc_result = value_incref(XS_NULL_VAL);
                }
            }

            else if (VAL_TAG(mc_obj) == XS_STR) {
                const char *s = mc_obj->s;
                int slen = (int)strlen(s);
                if (strcmp(mc_name,"len")==0||strcmp(mc_name,"size")==0||strcmp(mc_name,"length")==0)
                    mc_result = xs_int(utf8_strlen(s, slen));
                else if (strcmp(mc_name,"byte_len")==0)
                    mc_result = xs_int(slen);
                else if (strcmp(mc_name,"char_len")==0)
                    mc_result = xs_int(utf8_strlen(s, slen));
                else if (strcmp(mc_name,"upper")==0||strcmp(mc_name,"to_upper")==0) {
                    int out_len = 0;
                    char *r = utf8_str_upper(s, slen, &out_len);
                    mc_result = xs_str(r); free(r);
                } else if (strcmp(mc_name,"lower")==0||strcmp(mc_name,"to_lower")==0) {
                    int out_len = 0;
                    char *r = utf8_str_lower(s, slen, &out_len);
                    mc_result = xs_str(r); free(r);
                } else if (strcmp(mc_name,"trim")==0) {
                    if (slen == 0) { mc_result = xs_str(""); }
                    else {
                    const char *p2=s; while(*p2==' '||*p2=='\t'||*p2=='\n'||*p2=='\r') p2++;
                    const char *e2=s+slen-1;
                    while(e2>p2&&(*e2==' '||*e2=='\t'||*e2=='\n'||*e2=='\r')) e2--;
                    size_t n2=(size_t)(e2-p2+1); char *r=xs_malloc(n2+1);
                    memcpy(r,p2,n2); r[n2]='\0'; mc_result=xs_str(r); free(r);
                    }
                } else if (strcmp(mc_name,"contains")==0||strcmp(mc_name,"includes")==0) {
                    mc_result = (mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR&&strstr(s,mc_args[0]->s))
                        ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"starts_with")==0) {
                    if (mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                        size_t pl=strlen(mc_args[0]->s);
                        mc_result=strncmp(s,mc_args[0]->s,pl)==0
                            ? value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    } else mc_result=value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"ends_with")==0) {
                    if (mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                        size_t pl=strlen(mc_args[0]->s);
                        mc_result=(size_t)slen>=pl&&strcmp(s+slen-(int)pl,mc_args[0]->s)==0
                            ? value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    } else mc_result=value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"split")==0) {
                    const char *sep=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR)?mc_args[0]->s:" ";
                    Value *arr=xs_array_new(); size_t seplen=strlen(sep); const char *p3=s;
                    if (seplen==0) {
                        for(int j=0;j<slen;j++){char b[2]={s[j],0};Value*cv=xs_str(b);array_push(arr->arr,cv);value_decref(cv);}
                    } else {
                        const char *fnd;
                        while((fnd=strstr(p3,sep))!=NULL){
                            size_t chunk=(size_t)(fnd-p3); char *b=xs_malloc(chunk+1);
                            memcpy(b,p3,chunk); b[chunk]='\0';
                            Value *cv=xs_str(b); free(b); array_push(arr->arr,cv); value_decref(cv);
                            p3=fnd+seplen;
                        }
                        Value *cv=xs_str(p3); array_push(arr->arr,cv); value_decref(cv);
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"chars")==0 || strcmp(mc_name,"graphemes")==0) {
                    Value *arr=xs_array_new();
                    int j=0;
                    while (j < slen) {
                        int cp;
                        int n = utf8_decode(s+j, slen-j, &cp);
                        if (n <= 0) n = 1;
                        Value *cv = xs_str_n(s+j, (size_t)n);
                        array_push(arr->arr, cv); value_decref(cv);
                        j += n;
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"replace")==0&&mc_argc>=2&&VAL_TAG(mc_args[0])==XS_STR&&VAL_TAG(mc_args[1])==XS_STR) {
                    const char *from=mc_args[0]->s,*to=mc_args[1]->s;
                    size_t fl=strlen(from),tl=strlen(to);
                    size_t cap=strlen(s)*2+64; char *buf=xs_malloc(cap); size_t wpos=0;
                    const char *p3=s; const char *fnd2;
                    while(fl&&(fnd2=strstr(p3,from))!=NULL){
                        size_t prefix=(size_t)(fnd2-p3);
                        while(wpos+prefix+tl+1>cap){cap*=2;buf=xs_realloc(buf,cap);}
                        memcpy(buf+wpos,p3,prefix); wpos+=prefix;
                        memcpy(buf+wpos,to,tl); wpos+=tl;
                        p3=fnd2+fl;
                    }
                    size_t rest=strlen(p3);
                    while(wpos+rest+1>cap){cap*=2;buf=xs_realloc(buf,cap);}
                    memcpy(buf+wpos,p3,rest); buf[wpos+rest]='\0';
                    mc_result=xs_str(buf); free(buf);
                } else if (strcmp(mc_name,"parse_int")==0||strcmp(mc_name,"to_int")==0)
                    mc_result=xs_int(atoll(s));
                else if (strcmp(mc_name,"parse_float")==0||strcmp(mc_name,"to_float")==0)
                    mc_result=xs_float(atof(s));
                else if (strcmp(mc_name,"index_of")==0) {
                    mc_result=xs_int(-1);
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        const char *fnd=strstr(s,mc_args[0]->s);
                        if(fnd){value_decref(mc_result);mc_result=xs_int((int64_t)(fnd-s));}
                    }
                } else if (strcmp(mc_name,"last_index_of")==0) {
                    mc_result=xs_int(-1);
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        size_t sl2=strlen(mc_args[0]->s);
                        if(sl2>0){
                            for(int j=slen-(int)sl2;j>=0;j--){
                                if(strncmp(s+j,mc_args[0]->s,sl2)==0){value_decref(mc_result);mc_result=xs_int(j);break;}
                            }
                        }
                    }
                } else if (strcmp(mc_name,"repeat")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<0) n2=0;
                    size_t rlen=(size_t)slen*(size_t)n2;
                    char *buf=xs_malloc(rlen+1); size_t wpos=0;
                    for(int64_t j=0;j<n2;j++){memcpy(buf+wpos,s,(size_t)slen);wpos+=(size_t)slen;}
                    buf[wpos]='\0'; mc_result=xs_str(buf); free(buf);
                } else if (strcmp(mc_name,"slice")==0) {
                    int64_t st2=0, en2=(int64_t)slen;
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) st2=VAL_INT(mc_args[0]);
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_INT) en2=VAL_INT(mc_args[1]);
                    if(st2<0) st2+=(int64_t)slen;
                    if(en2<0) en2+=(int64_t)slen;
                    if(st2<0) st2=0;
                    if(en2>(int64_t)slen) en2=(int64_t)slen;
                    if(st2>=en2){mc_result=xs_str("");}
                    else{
                        size_t n2=(size_t)(en2-st2); char *buf=xs_malloc(n2+1);
                        memcpy(buf,s+st2,n2); buf[n2]='\0'; mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"bytes")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<slen;j++) array_push(arr->arr,xs_int((unsigned char)s[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"capitalize")==0) {
                    char *r=xs_strdup(s);
                    if(r[0]) r[0]=(char)toupper((unsigned char)r[0]);
                    for(int j=1;r[j];j++) r[j]=(char)tolower((unsigned char)r[j]);
                    mc_result=xs_str(r); free(r);
                } else if (strcmp(mc_name,"count")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    int cnt=0; size_t sl2=strlen(mc_args[0]->s);
                    if(sl2>0){const char *p3=s;while((p3=strstr(p3,mc_args[0]->s))!=NULL){cnt++;p3+=sl2;}}
                    mc_result=xs_int(cnt);
                } else if (strcmp(mc_name,"pad_left")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); char ch=' ';
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_STR&&mc_args[1]->s[0]) ch=mc_args[1]->s[0];
                    if(n2<=(int64_t)slen) mc_result=xs_str(s);
                    else{
                        char *buf=xs_malloc((size_t)n2+1);
                        int64_t pad=n2-(int64_t)slen;
                        for(int64_t j=0;j<pad;j++) buf[j]=ch;
                        memcpy(buf+pad,s,(size_t)slen); buf[n2]='\0';
                        mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"pad_right")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); char ch=' ';
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_STR&&mc_args[1]->s[0]) ch=mc_args[1]->s[0];
                    if(n2<=(int64_t)slen) mc_result=xs_str(s);
                    else{
                        char *buf=xs_malloc((size_t)n2+1);
                        memcpy(buf,s,(size_t)slen);
                        for(int64_t j=(int64_t)slen;j<n2;j++) buf[j]=ch;
                        buf[n2]='\0'; mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"reverse")==0) {
                    /* Walk codepoints in reverse so multi-byte UTF-8
                       sequences stay intact (byte-level reverse breaks them). */
                    char *r=xs_malloc((size_t)slen+1);
                    int wpos = 0;
                    int i2 = slen;
                    while (i2 > 0) {
                        int start = i2 - 1;
                        while (start > 0 && (((unsigned char)s[start] & 0xC0) == 0x80)) start--;
                        int n2 = i2 - start;
                        memcpy(r + wpos, s + start, (size_t)n2);
                        wpos += n2;
                        i2 = start;
                    }
                    r[wpos]='\0'; mc_result=xs_str(r); free(r);
                } else if (strcmp(mc_name,"join")==0&&mc_argc>=1) {
                    /* "sep".join(arr) */
                    if(VAL_TAG(mc_args[0])==XS_ARRAY||VAL_TAG(mc_args[0])==XS_TUPLE){
                        size_t cap=256; char *buf=xs_malloc(cap); size_t wpos=0;
                        for(int j=0;j<mc_args[0]->arr->len;j++){
                            char *sv=value_str(mc_args[0]->arr->items[j]); size_t svl=strlen(sv);
                            if(j>0){while(wpos+(size_t)slen+svl+2>cap){cap*=2;buf=xs_realloc(buf,cap);}memcpy(buf+wpos,s,(size_t)slen);wpos+=(size_t)slen;}
                            while(wpos+svl+2>cap){cap*=2;buf=xs_realloc(buf,cap);}
                            memcpy(buf+wpos,sv,svl); wpos+=svl; free(sv);
                        }
                        buf[wpos]='\0'; mc_result=xs_str(buf); free(buf);
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"trim_start")==0||strcmp(mc_name,"ltrim")==0) {
                    const char *p2=s; while(*p2==' '||*p2=='\t'||*p2=='\n'||*p2=='\r') p2++;
                    mc_result=xs_str(p2);
                } else if (strcmp(mc_name,"trim_end")==0||strcmp(mc_name,"rtrim")==0) {
                    char *r=xs_strdup(s); int rlen=(int)strlen(r);
                    while(rlen>0&&(r[rlen-1]==' '||r[rlen-1]=='\t'||r[rlen-1]=='\n'||r[rlen-1]=='\r')) rlen--;
                    r[rlen]='\0'; mc_result=xs_str(r); free(r);
                } else if (strcmp(mc_name,"title")==0) {
                    char *r=xs_strdup(s); int prev_space=1;
                    for(int j=0;r[j];j++){
                        if(r[j]==' '||r[j]=='\t'||r[j]=='\n'||r[j]=='\r'){prev_space=1;}
                        else if(prev_space){r[j]=(char)toupper((unsigned char)r[j]);prev_space=0;}
                        else{r[j]=(char)tolower((unsigned char)r[j]);}
                    }
                    mc_result=xs_str(r); free(r);
                } else if (strcmp(mc_name,"center")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); char ch=' ';
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_STR&&mc_args[1]->s[0]) ch=mc_args[1]->s[0];
                    if(n2<=(int64_t)slen) mc_result=xs_str(s);
                    else{
                        int64_t total_pad=n2-(int64_t)slen;
                        int64_t lpad=total_pad/2, rpad=total_pad-lpad;
                        char *buf=xs_malloc((size_t)n2+1);
                        for(int64_t j=0;j<lpad;j++) buf[j]=ch;
                        memcpy(buf+lpad,s,(size_t)slen);
                        for(int64_t j=0;j<rpad;j++) buf[lpad+(int64_t)slen+j]=ch;
                        buf[n2]='\0'; mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"char_at")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t idx=VAL_INT(mc_args[0]);
                    if(idx<0) idx+=slen;
                    if(idx>=0&&idx<(int64_t)slen){char b[2]={s[idx],0};mc_result=xs_str(b);}
                    else mc_result=xs_str("");
                } else if (strcmp(mc_name,"lines")==0) {
                    Value *arr=xs_array_new(); const char *p2=s;
                    while(1){
                        const char *nl=strchr(p2,'\n');
                        if(!nl){Value *cv=xs_str(p2);array_push(arr->arr,cv);value_decref(cv);break;}
                        size_t chunk=(size_t)(nl-p2);
                        /* strip trailing \r */
                        size_t clen=chunk; if(clen>0&&p2[clen-1]=='\r') clen--;
                        char *b=xs_malloc(clen+1); memcpy(b,p2,clen); b[clen]='\0';
                        Value *cv=xs_str(b); free(b); array_push(arr->arr,cv); value_decref(cv);
                        p2=nl+1;
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"is_empty")==0) {
                    mc_result=slen==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_ascii")==0) {
                    int ok=1; for(int j=0;j<slen;j++) if((unsigned char)s[j]>=128){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_digit")==0||strcmp(mc_name,"is_numeric")==0) {
                    int ok=(slen>0); for(int j=0;j<slen;j++) if(!isdigit((unsigned char)s[j])){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_alpha")==0) {
                    int ok=(slen>0); for(int j=0;j<slen;j++) if(!isalpha((unsigned char)s[j])){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_alnum")==0) {
                    int ok=(slen>0); for(int j=0;j<slen;j++) if(!isalnum((unsigned char)s[j])){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_upper")==0) {
                    int ok=(slen>0); for(int j=0;j<slen;j++) if(isalpha((unsigned char)s[j])&&!isupper((unsigned char)s[j])){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"is_lower")==0) {
                    int ok=(slen>0); for(int j=0;j<slen;j++) if(isalpha((unsigned char)s[j])&&!islower((unsigned char)s[j])){ok=0;break;}
                    mc_result=ok?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"remove_prefix")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    size_t pl=strlen(mc_args[0]->s);
                    if(pl>0&&strncmp(s,mc_args[0]->s,pl)==0) mc_result=xs_str(s+pl);
                    else mc_result=xs_str(s);
                } else if (strcmp(mc_name,"remove_suffix")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    size_t pl=strlen(mc_args[0]->s);
                    if(pl>0&&(size_t)slen>=pl&&strcmp(s+slen-pl,mc_args[0]->s)==0){
                        char *r=xs_malloc(slen-pl+1); memcpy(r,s,slen-pl); r[slen-pl]='\0';
                        mc_result=xs_str(r); free(r);
                    } else mc_result=xs_str(s);
                } else if (strcmp(mc_name,"truncate")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<0) n2=0;
                    if((int64_t)slen<=n2) mc_result=xs_str(s);
                    else{
                        size_t tlen=(size_t)(n2>3?n2-3:n2);
                        char *buf=xs_malloc(tlen+4);
                        memcpy(buf,s,tlen);
                        if(n2>3){memcpy(buf+tlen,"...",3);buf[tlen+3]='\0';}
                        else{buf[tlen]='\0';}
                        mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"substr")==0||strcmp(mc_name,"substring")==0) {
                    int64_t st2=0, en2=(int64_t)slen;
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) st2=VAL_INT(mc_args[0]);
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_INT) en2=VAL_INT(mc_args[1]);
                    if(st2<0) st2+=(int64_t)slen;
                    if(en2<0) en2+=(int64_t)slen;
                    if(st2<0) st2=0;
                    if(en2>(int64_t)slen) en2=(int64_t)slen;
                    if(st2>=en2){mc_result=xs_str("");}
                    else{
                        size_t n2=(size_t)(en2-st2); char *buf=xs_malloc(n2+1);
                        memcpy(buf,s+st2,n2); buf[n2]='\0'; mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"to_str")==0||strcmp(mc_name,"to_string")==0) {
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"startswith")==0) {
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        size_t pl=strlen(mc_args[0]->s);
                        mc_result=strncmp(s,mc_args[0]->s,pl)==0
                            ?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    } else mc_result=value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"endswith")==0) {
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        size_t pl=strlen(mc_args[0]->s);
                        mc_result=(size_t)slen>=pl&&strcmp(s+slen-pl,mc_args[0]->s)==0
                            ?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
                    } else mc_result=value_incref(XS_FALSE_VAL);
                } else if (strcmp(mc_name,"rfind")==0) {
                    mc_result=xs_int(-1);
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        size_t sl2=strlen(mc_args[0]->s);
                        if(sl2>0){
                            for(int j=slen-(int)sl2;j>=0;j--){
                                if(strncmp(s+j,mc_args[0]->s,sl2)==0){value_decref(mc_result);mc_result=xs_int(j);break;}
                            }
                        }
                    }
                } else if (strcmp(mc_name,"split_at")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t idx=VAL_INT(mc_args[0]);
                    if(idx<0) idx+=slen;
                    if(idx<0) idx=0;
                    if(idx>(int64_t)slen) idx=(int64_t)slen;
                    Value *arr=xs_array_new();
                    char *a=xs_malloc((size_t)idx+1); memcpy(a,s,(size_t)idx); a[idx]='\0';
                    Value *va=xs_str(a); free(a); array_push(arr->arr,va); value_decref(va);
                    Value *vb=xs_str(s+idx); array_push(arr->arr,vb); value_decref(vb);
                    mc_result=arr;
                } else if (strcmp(mc_name,"to_chars")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<slen;j++){char b[2]={s[j],0};Value*cv=xs_str(b);array_push(arr->arr,cv);value_decref(cv);}
                    mc_result=arr;
                } else if (strcmp(mc_name,"to_bytes")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<slen;j++) array_push(arr->arr,xs_int((unsigned char)s[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"length")==0) {
                    mc_result=xs_int(slen);
                } else if (strcmp(mc_name,"lpad")==0||strcmp(mc_name,"pad_start")==0) {
                    int64_t n2=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?VAL_INT(mc_args[0]):(int64_t)slen;
                    char ch=' ';
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_STR&&mc_args[1]->s[0]) ch=mc_args[1]->s[0];
                    if(n2<=(int64_t)slen) mc_result=xs_str(s);
                    else{
                        char *buf=xs_malloc((size_t)n2+1);
                        int64_t pad=n2-(int64_t)slen;
                        for(int64_t j=0;j<pad;j++) buf[j]=ch;
                        memcpy(buf+pad,s,(size_t)slen); buf[n2]='\0';
                        mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"rpad")==0||strcmp(mc_name,"pad_end")==0) {
                    int64_t n2=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?VAL_INT(mc_args[0]):(int64_t)slen;
                    char ch=' ';
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_STR&&mc_args[1]->s[0]) ch=mc_args[1]->s[0];
                    if(n2<=(int64_t)slen) mc_result=xs_str(s);
                    else{
                        char *buf=xs_malloc((size_t)n2+1);
                        memcpy(buf,s,(size_t)slen);
                        for(int64_t j=(int64_t)slen;j<n2;j++) buf[j]=ch;
                        buf[n2]='\0'; mc_result=xs_str(buf); free(buf);
                    }
                } else if (strcmp(mc_name,"reversed")==0) {
                    char *r=xs_malloc((size_t)slen+1);
                    for(int j=0;j<slen;j++) r[j]=s[slen-1-j];
                    r[slen]='\0'; mc_result=xs_str(r); free(r);
                } else if (strcmp(mc_name,"find")==0) {
                    mc_result=xs_int(-1);
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR){
                        const char *fnd=strstr(s,mc_args[0]->s);
                        if(fnd){value_decref(mc_result);mc_result=xs_int((int64_t)(fnd-s));}
                    }
                } else if (strcmp(mc_name,"format")==0) {
                    /* simple sprintf-style: replace %s/%d/%f with args in order */
                    size_t cap=strlen(s)*2+64; char *buf=xs_malloc(cap); size_t wpos=0;
                    const char *p2=s; int ai=0;
                    while(*p2){
                        if(*p2=='%'&&*(p2+1)){
                            char spec=*(p2+1);
                            if(spec=='s'||spec=='d'||spec=='f'||spec=='i'){
                                char tmp[64]; tmp[0]='\0';
                                if(ai<mc_argc){
                                    if(spec=='s'){
                                        char *sv=value_str(mc_args[ai]);
                                        size_t svl=strlen(sv);
                                        while(wpos+svl+1>cap){cap*=2;buf=xs_realloc(buf,cap);}
                                        memcpy(buf+wpos,sv,svl); wpos+=svl; free(sv); ai++;
                                        p2+=2; continue;
                                    } else if((spec=='d'||spec=='i')&&VAL_TAG(mc_args[ai])==XS_INT){
                                        snprintf(tmp,sizeof(tmp),"%lld",(long long)VAL_INT(mc_args[ai])); ai++;
                                    } else if(spec=='f'&&VAL_TAG(mc_args[ai])==XS_FLOAT){
                                        snprintf(tmp,sizeof(tmp),"%g",mc_args[ai]->f); ai++;
                                    } else if((spec=='d'||spec=='i')&&VAL_TAG(mc_args[ai])==XS_FLOAT){
                                        snprintf(tmp,sizeof(tmp),"%lld",(long long)mc_args[ai]->f); ai++;
                                    } else if(spec=='f'&&VAL_TAG(mc_args[ai])==XS_INT){
                                        snprintf(tmp,sizeof(tmp),"%g",(double)VAL_INT(mc_args[ai])); ai++;
                                    }
                                }
                                size_t tl=strlen(tmp);
                                while(wpos+tl+1>cap){cap*=2;buf=xs_realloc(buf,cap);}
                                memcpy(buf+wpos,tmp,tl); wpos+=tl; p2+=2; continue;
                            }
                        }
                        while(wpos+2>cap){cap*=2;buf=xs_realloc(buf,cap);}
                        buf[wpos++]=*p2++;
                    }
                    buf[wpos]='\0'; mc_result=xs_str(buf); free(buf);
                } else if (strcmp(mc_name,"as_int")==0)
                    mc_result=xs_int(atoll(s));
                else if (strcmp(mc_name,"as_float")==0)
                    mc_result=xs_float(atof(s));
                else if (strcmp(mc_name,"as_str")==0)
                    mc_result=value_incref(mc_obj);
                else if (strcmp(mc_name,"parse")==0) {
                    int base=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?(int)VAL_INT(mc_args[0]):10;
                    mc_result=xs_int((int64_t)strtoll(s,NULL,base));
                } else if (strcmp(mc_name,"from_chars")==0) {
                    /* join chars into string: just return self if called on a string */
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"is_a")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    mc_result=xs_bool(strcmp(mc_args[0]->s,"str")==0||strcmp(mc_args[0]->s,"String")==0);
                } else mc_result=value_incref(XS_NULL_VAL);
            }
            else if (VAL_TAG(mc_obj)==XS_ARRAY||VAL_TAG(mc_obj)==XS_TUPLE) {
                if (strcmp(mc_name,"len")==0||strcmp(mc_name,"size")==0)
                    mc_result=xs_int(mc_obj->arr->len);
                else if (strcmp(mc_name,"push")==0||strcmp(mc_name,"append")==0) {
                    for(int j=0;j<mc_argc;j++) array_push(mc_obj->arr,value_incref(mc_args[j]));
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"pop")==0) {
                    if(mc_obj->arr->len>0){
                        mc_result=value_incref(mc_obj->arr->items[mc_obj->arr->len-1]);
                        value_decref(mc_obj->arr->items[--mc_obj->arr->len]);
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"first")==0||strcmp(mc_name,"head")==0)
                    mc_result=mc_obj->arr->len>0?value_incref(mc_obj->arr->items[0]):value_incref(XS_NULL_VAL);
                else if (strcmp(mc_name,"last")==0)
                    mc_result=mc_obj->arr->len>0?value_incref(mc_obj->arr->items[mc_obj->arr->len-1]):value_incref(XS_NULL_VAL);
                else if (strcmp(mc_name,"contains")==0||strcmp(mc_name,"includes")==0) {
                    mc_result=value_incref(XS_FALSE_VAL);
                    if(mc_argc>=1){for(int j=0;j<mc_obj->arr->len;j++){if(value_equal(mc_obj->arr->items[j],mc_args[0])){value_decref(mc_result);mc_result=value_incref(XS_TRUE_VAL);break;}}}
                } else if (strcmp(mc_name,"join")==0) {
                    const char *sep=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR)?mc_args[0]->s:"";
                    size_t cap=256; char *buf=xs_malloc(cap); size_t wpos=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        char *sv=value_str(mc_obj->arr->items[j]); size_t svl=strlen(sv);
                        if(j>0){size_t sl=strlen(sep);while(wpos+sl+svl+2>cap){cap*=2;buf=xs_realloc(buf,cap);}memcpy(buf+wpos,sep,sl);wpos+=sl;}
                        while(wpos+svl+2>cap){cap*=2;buf=xs_realloc(buf,cap);}
                        memcpy(buf+wpos,sv,svl); wpos+=svl; free(sv);
                    }
                    buf[wpos]='\0'; mc_result=xs_str(buf); free(buf);
                } else if (strcmp(mc_name,"reverse")==0) {
                    int n2=mc_obj->arr->len;
                    for(int j=0;j<n2/2;j++){
                        Value *tmp=mc_obj->arr->items[j];
                        mc_obj->arr->items[j]=mc_obj->arr->items[n2-1-j];
                        mc_obj->arr->items[n2-1-j]=tmp;
                    }
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"shift")==0) {
                    if(mc_obj->arr->len>0){
                        mc_result=value_incref(mc_obj->arr->items[0]);
                        value_decref(mc_obj->arr->items[0]);
                        for(int j=1;j<mc_obj->arr->len;j++) mc_obj->arr->items[j-1]=mc_obj->arr->items[j];
                        mc_obj->arr->len--;
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"unshift")==0) {
                    for(int j=0;j<mc_argc;j++){
                        array_push(mc_obj->arr, value_incref(XS_NULL_VAL));
                        for(int k=mc_obj->arr->len-1;k>0;k--) mc_obj->arr->items[k]=mc_obj->arr->items[k-1];
                        mc_obj->arr->items[0]=value_incref(mc_args[j]);
                    }
                    mc_result=xs_int(mc_obj->arr->len);
                } else if (strcmp(mc_name,"insert")==0&&mc_argc>=2&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t pos=VAL_INT(mc_args[0]);
                    if(pos<0) pos=0;
                    if(pos>mc_obj->arr->len) pos=mc_obj->arr->len;
                    array_push(mc_obj->arr, value_incref(XS_NULL_VAL));
                    for(int k=mc_obj->arr->len-1;k>(int)pos;k--) mc_obj->arr->items[k]=mc_obj->arr->items[k-1];
                    mc_obj->arr->items[(int)pos]=value_incref(mc_args[1]);
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"remove")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t pos=VAL_INT(mc_args[0]);
                    if(pos>=0&&pos<mc_obj->arr->len){
                        mc_result=value_incref(mc_obj->arr->items[pos]);
                        value_decref(mc_obj->arr->items[pos]);
                        for(int k=(int)pos;k<mc_obj->arr->len-1;k++) mc_obj->arr->items[k]=mc_obj->arr->items[k+1];
                        mc_obj->arr->len--;
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"slice")==0) {
                    int64_t start=0, end=mc_obj->arr->len;
                    if(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) start=VAL_INT(mc_args[0]);
                    if(mc_argc>=2&&VAL_TAG(mc_args[1])==XS_INT) end=VAL_INT(mc_args[1]);
                    if(start<0) start+=mc_obj->arr->len;
                    if(end<0) end+=mc_obj->arr->len;
                    if(start<0) start=0;
                    if(end>mc_obj->arr->len) end=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    for(int64_t j=start;j<end;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"concat")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    for(int a2=0;a2<mc_argc;a2++){
                        if(VAL_TAG(mc_args[a2])==XS_ARRAY) for(int j=0;j<mc_args[a2]->arr->len;j++) array_push(arr->arr,value_incref(mc_args[a2]->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"sort")==0) {
                    /* Sort in place; matches interp behavior. */
                    Value *cmp_fn = (mc_argc >= 1 && mc_args[0] &&
                                     (VAL_TAG(mc_args[0])==XS_NATIVE ||
                                      VAL_TAG(mc_args[0])==XS_CLOSURE))
                                    ? mc_args[0] : NULL;
                    XSArray *sa = mc_obj->arr;
                    for(int j=0;j<sa->len-1;j++) for(int k=0;k<sa->len-1-j;k++){
                        int worse;
                        if (cmp_fn) {
                            Value *pair[2] = { sa->items[k], sa->items[k+1] };
                            Value *r = vm_invoke(vm, cmp_fn, pair, 2);
                            frame = FRAME;
                            worse = (r && VAL_IS_INT(r)) ? (VAL_INT(r) > 0) : 0;
                            if (r) value_decref(r);
                        } else {
                            worse = (value_cmp(sa->items[k],sa->items[k+1]) > 0);
                        }
                        if (worse) {
                            Value *tmp=sa->items[k]; sa->items[k]=sa->items[k+1]; sa->items[k+1]=tmp;
                        }
                    }
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"filter")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    Value *pred=mc_args[0];
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *elem=mc_obj->arr->items[j];
                        Value *r=vm_invoke(vm,pred,&elem,1);
                        if(value_truthy(r)) array_push(arr->arr,value_incref(elem));
                        value_decref(r);
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"map")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    Value *fn=mc_args[0];
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *elem=mc_obj->arr->items[j];
                        Value *r=vm_invoke(vm,fn,&elem,1);
                        array_push(arr->arr,r?r:value_incref(XS_NULL_VAL));
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"find")==0&&mc_argc>=1) {
                    mc_result=value_incref(XS_NULL_VAL);
                    Value *pred=mc_args[0];
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *elem=mc_obj->arr->items[j];
                        Value *r=vm_invoke(vm,pred,&elem,1);
                        if(value_truthy(r)){value_decref(mc_result);mc_result=value_incref(elem);value_decref(r);break;}
                        value_decref(r);
                    }
                    frame=FRAME;
                } else if (strcmp(mc_name,"reduce")==0||strcmp(mc_name,"fold")==0) {
                    /* Accept either (fn, init) or (init, fn); fold is
                       always (init, fn). Pick whichever arg is callable. */
                    int is_fn_0 = mc_argc>=1 && mc_args[0] &&
                                  (VAL_TAG(mc_args[0])==XS_NATIVE ||
                                   VAL_TAG(mc_args[0])==XS_CLOSURE);
                    int is_fn_1 = mc_argc>=2 && mc_args[1] &&
                                  (VAL_TAG(mc_args[1])==XS_NATIVE ||
                                   VAL_TAG(mc_args[1])==XS_CLOSURE);
                    Value *fn; Value *acc;
                    int start_j = 0;
                    if (strcmp(mc_name,"fold")==0 && mc_argc>=2) {
                        acc = value_incref(mc_args[0]); fn = mc_args[1];
                    } else if (mc_argc>=2 && is_fn_1 && !is_fn_0) {
                        acc = value_incref(mc_args[0]); fn = mc_args[1];
                    } else if (mc_argc>=2) {
                        fn = mc_args[0]; acc = value_incref(mc_args[1]);
                    } else {
                        fn = mc_argc>=1 ? mc_args[0] : NULL;
                        if (mc_obj->arr->len>0) { acc=value_incref(mc_obj->arr->items[0]); start_j=1; }
                        else acc = value_incref(XS_NULL_VAL);
                    }
                    if(fn&&(VAL_TAG(fn)==XS_NATIVE||VAL_TAG(fn)==XS_CLOSURE)){
                        for(int j=start_j;j<mc_obj->arr->len;j++){
                            Value *pair[2]={acc,mc_obj->arr->items[j]};
                            Value *r=vm_invoke(vm,fn,pair,2);
                            value_decref(acc); acc=r?r:value_incref(XS_NULL_VAL);
                        }
                        frame=FRAME;
                    }
                    mc_result=acc;
                } else if (strcmp(mc_name,"index_of")==0||strcmp(mc_name,"find_index")==0) {
                    mc_result=xs_int(-1);
                    if(mc_argc>=1){
                        for(int j=0;j<mc_obj->arr->len;j++){
                            if(value_equal(mc_obj->arr->items[j],mc_args[0])){value_decref(mc_result);mc_result=xs_int(j);break;}
                        }
                    }
                } else if (strcmp(mc_name,"any")==0&&mc_argc>=1) {
                    mc_result=value_incref(XS_FALSE_VAL);
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(value_truthy(r)){value_decref(mc_result);mc_result=value_incref(XS_TRUE_VAL);value_decref(r);break;}
                        value_decref(r);
                    }
                    frame=FRAME;
                } else if (strcmp(mc_name,"all")==0&&mc_argc>=1) {
                    mc_result=value_incref(XS_TRUE_VAL);
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(!value_truthy(r)){value_decref(mc_result);mc_result=value_incref(XS_FALSE_VAL);value_decref(r);break;}
                        value_decref(r);
                    }
                    frame=FRAME;
                } else if (strcmp(mc_name,"count")==0) mc_result=xs_int(mc_obj->arr->len);
                else if (strcmp(mc_name,"sum")==0) {
                    int64_t si=0; double sf=0; int is_float=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(VAL_TAG(mc_obj->arr->items[j])==XS_INT) si+=VAL_INT(mc_obj->arr->items[j]);
                        else if(VAL_TAG(mc_obj->arr->items[j])==XS_FLOAT){sf+=mc_obj->arr->items[j]->f;is_float=1;}
                    }
                    mc_result=is_float?xs_float(sf+(double)si):xs_int(si);
                } else if (strcmp(mc_name,"min")==0) {
                    mc_result=mc_obj->arr->len>0?value_incref(mc_obj->arr->items[0]):value_incref(XS_NULL_VAL);
                    for(int j=1;j<mc_obj->arr->len;j++) if(value_cmp(mc_obj->arr->items[j],mc_result)<0){value_decref(mc_result);mc_result=value_incref(mc_obj->arr->items[j]);}
                } else if (strcmp(mc_name,"max")==0) {
                    mc_result=mc_obj->arr->len>0?value_incref(mc_obj->arr->items[0]):value_incref(XS_NULL_VAL);
                    for(int j=1;j<mc_obj->arr->len;j++) if(value_cmp(mc_obj->arr->items[j],mc_result)>0){value_decref(mc_result);mc_result=value_incref(mc_obj->arr->items[j]);}
                } else if (strcmp(mc_name,"enumerate")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *pair=xs_tuple_new();
                        array_push(pair->arr,xs_int(j));
                        array_push(pair->arr,value_incref(mc_obj->arr->items[j]));
                        array_push(arr->arr,pair);
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"flat")==0||strcmp(mc_name,"flatten")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(VAL_TAG(mc_obj->arr->items[j])==XS_ARRAY){
                            for(int k=0;k<mc_obj->arr->items[j]->arr->len;k++)
                                array_push(arr->arr,value_incref(mc_obj->arr->items[j]->arr->items[k]));
                        } else array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"chunks")==0) {
                    int64_t sz = (mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?VAL_INT(mc_args[0])
                               : (mc_argc>=1&&VAL_TAG(mc_args[0])==XS_FLOAT)?(int64_t)mc_args[0]->f : 0;
                    if (sz<=0) {
                        g_xs_pending_throw = xs_error_new("ValueError",
                            "chunks() requires a positive integer size", NULL);
                        mc_result=value_incref(XS_NULL_VAL);
                    } else {
                        Value *arr=xs_array_new();
                        for(int j=0;j<mc_obj->arr->len;j+=(int)sz){
                            Value *chunk=xs_array_new();
                            int end=j+(int)sz; if(end>mc_obj->arr->len) end=mc_obj->arr->len;
                            for(int k=j;k<end;k++) array_push(chunk->arr,value_incref(mc_obj->arr->items[k]));
                            array_push(arr->arr,chunk);
                        }
                        mc_result=arr;
                    }
                } else if (strcmp(mc_name,"unique")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        int dup=0;
                        for(int k=0;k<arr->arr->len;k++) if(value_equal(mc_obj->arr->items[j],arr->arr->items[k])){dup=1;break;}
                        if(!dup) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"is_empty")==0) {
                    mc_result=xs_bool(mc_obj->arr->len==0);
                } else if (strcmp(mc_name,"each")==0||strcmp(mc_name,"for_each")==0) {
                    if(mc_argc>=1){
                        for(int j=0;j<mc_obj->arr->len;j++){
                            Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                            value_decref(r);
                        }
                        frame=FRAME;
                    }
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"take")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<0) n2=0;
                    if(n2>mc_obj->arr->len) n2=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    for(int64_t j=0;j<n2;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"drop")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<0) n2=0;
                    if(n2>mc_obj->arr->len) n2=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    for(int64_t j=n2;j<mc_obj->arr->len;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"take_while")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        int ok=value_truthy(r); value_decref(r);
                        if(!ok) break;
                        array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"drop_while")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    int dropping=1;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(dropping){
                            Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                            dropping=value_truthy(r); value_decref(r);
                        }
                        if(!dropping) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"flat_map")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(r&&(VAL_TAG(r)==XS_ARRAY||VAL_TAG(r)==XS_TUPLE)){
                            for(int k=0;k<r->arr->len;k++) array_push(arr->arr,value_incref(r->arr->items[k]));
                            value_decref(r);
                        } else if(r) { array_push(arr->arr,r); }
                        else { array_push(arr->arr,value_incref(XS_NULL_VAL)); }
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"zip")==0&&mc_argc>=1&&(VAL_TAG(mc_args[0])==XS_ARRAY||VAL_TAG(mc_args[0])==XS_TUPLE)) {
                    Value *arr=xs_array_new();
                    int n2=mc_obj->arr->len<mc_args[0]->arr->len?mc_obj->arr->len:mc_args[0]->arr->len;
                    for(int j=0;j<n2;j++){
                        Value *pair=xs_tuple_new();
                        array_push(pair->arr,value_incref(mc_obj->arr->items[j]));
                        array_push(pair->arr,value_incref(mc_args[0]->arr->items[j]));
                        array_push(arr->arr,pair);
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"zip_with")==0&&mc_argc>=2&&(VAL_TAG(mc_args[0])==XS_ARRAY||VAL_TAG(mc_args[0])==XS_TUPLE)) {
                    Value *arr=xs_array_new();
                    int n2=mc_obj->arr->len<mc_args[0]->arr->len?mc_obj->arr->len:mc_args[0]->arr->len;
                    for(int j=0;j<n2;j++){
                        Value *pair[2]={mc_obj->arr->items[j],mc_args[0]->arr->items[j]};
                        Value *r=vm_invoke(vm,mc_args[1],pair,2);
                        array_push(arr->arr,r?r:value_incref(XS_NULL_VAL));
                    }
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"partition")==0&&mc_argc>=1) {
                    Value *yes=xs_array_new(), *no=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(value_truthy(r)) array_push(yes->arr,value_incref(mc_obj->arr->items[j]));
                        else array_push(no->arr,value_incref(mc_obj->arr->items[j]));
                        value_decref(r);
                    }
                    frame=FRAME;
                    Value *parr=xs_array_new();
                    array_push(parr->arr,yes); array_push(parr->arr,no);
                    mc_result=parr;
                } else if (strcmp(mc_name,"group_by")==0&&mc_argc>=1) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *k=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(!k) k=value_incref(XS_NULL_VAL);
                        char *ks=value_str(k); value_decref(k);
                        Value *bucket=map_get(m->map,ks);
                        if(!bucket){bucket=xs_array_new();map_set(m->map,ks,bucket);value_decref(bucket);bucket=map_get(m->map,ks);}
                        array_push(bucket->arr,value_incref(mc_obj->arr->items[j]));
                        free(ks);
                    }
                    frame=FRAME;
                    mc_result=m;
                } else if (strcmp(mc_name,"sort_by")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    {
                        int slen2=arr->arr->len;
                        Value **skeys=(Value**)xs_malloc(sizeof(Value*)*(size_t)(slen2>0?slen2:1));
                        for(int j=0;j<slen2;j++){skeys[j]=vm_invoke(vm,mc_args[0],&arr->arr->items[j],1);if(!skeys[j])skeys[j]=value_incref(XS_NULL_VAL);}
                        frame=FRAME;
                        for(int j=0;j<slen2-1;j++) for(int k=0;k<slen2-1-j;k++){
                            if(value_cmp(skeys[k],skeys[k+1])>0){
                                Value *tv=arr->arr->items[k]; arr->arr->items[k]=arr->arr->items[k+1]; arr->arr->items[k+1]=tv;
                                Value *tk=skeys[k]; skeys[k]=skeys[k+1]; skeys[k+1]=tk;
                            }
                        }
                        for(int j=0;j<slen2;j++) value_decref(skeys[j]);
                        free(skeys);
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"min_by")==0&&mc_argc>=1) {
                    if(mc_obj->arr->len==0){mc_result=value_incref(XS_NULL_VAL);}
                    else{
                        Value *best=mc_obj->arr->items[0];
                        Value *bkey=vm_invoke(vm,mc_args[0],&best,1); if(!bkey) bkey=value_incref(XS_NULL_VAL);
                        for(int j=1;j<mc_obj->arr->len;j++){
                            Value *k=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1); if(!k) k=value_incref(XS_NULL_VAL);
                            if(value_cmp(k,bkey)<0){value_decref(bkey);bkey=k;best=mc_obj->arr->items[j];}
                            else value_decref(k);
                        }
                        value_decref(bkey);
                        frame=FRAME;
                        mc_result=value_incref(best);
                    }
                } else if (strcmp(mc_name,"max_by")==0&&mc_argc>=1) {
                    if(mc_obj->arr->len==0){mc_result=value_incref(XS_NULL_VAL);}
                    else{
                        Value *best=mc_obj->arr->items[0];
                        Value *bkey=vm_invoke(vm,mc_args[0],&best,1); if(!bkey) bkey=value_incref(XS_NULL_VAL);
                        for(int j=1;j<mc_obj->arr->len;j++){
                            Value *k=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1); if(!k) k=value_incref(XS_NULL_VAL);
                            if(value_cmp(k,bkey)>0){value_decref(bkey);bkey=k;best=mc_obj->arr->items[j];}
                            else value_decref(k);
                        }
                        value_decref(bkey);
                        frame=FRAME;
                        mc_result=value_incref(best);
                    }
                } else if (strcmp(mc_name,"dedup")==0) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(j==0||!value_equal(mc_obj->arr->items[j],mc_obj->arr->items[j-1]))
                            array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"intersperse")==0&&mc_argc>=1) {
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(j>0) array_push(arr->arr,value_incref(mc_args[0]));
                        array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"window")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); Value *arr=xs_array_new();
                    if(n2>0){
                        for(int j=0;j<=mc_obj->arr->len-(int)n2;j++){
                            Value *win=xs_array_new();
                            for(int64_t k=0;k<n2;k++) array_push(win->arr,value_incref(mc_obj->arr->items[j+k]));
                            array_push(arr->arr,win);
                        }
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"chunk")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<1) n2=1;
                    Value *arr=xs_array_new();
                    for(int j=0;j<mc_obj->arr->len;){
                        Value *ch=xs_array_new();
                        for(int64_t k=0;k<n2&&j<mc_obj->arr->len;k++,j++) array_push(ch->arr,value_incref(mc_obj->arr->items[j]));
                        array_push(arr->arr,ch);
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"rotate")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int alen=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    if(alen>0){
                        int64_t n2=VAL_INT(mc_args[0])%alen;
                        if(n2<0) n2+=alen;
                        for(int j=(int)n2;j<alen;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                        for(int j=0;j<(int)n2;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"sample")==0) {
                    int64_t n2=(mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT)?VAL_INT(mc_args[0]):1;
                    if(n2<0) n2=0;
                    if(n2>mc_obj->arr->len) n2=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    for(int64_t j=0;j<n2;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"product")==0) {
                    int64_t pi=1; double pf=1.0; int is_float=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(VAL_TAG(mc_obj->arr->items[j])==XS_INT) pi*=VAL_INT(mc_obj->arr->items[j]);
                        else if(VAL_TAG(mc_obj->arr->items[j])==XS_FLOAT){pf*=mc_obj->arr->items[j]->f;is_float=1;}
                    }
                    mc_result=is_float?xs_float(pf*(double)pi):xs_int(pi);
                } else if (strcmp(mc_name,"frequencies")==0) {
                    Value *m=xs_map_new();
                    for(int j=0;j<mc_obj->arr->len;j++){
                        char *ks=value_str(mc_obj->arr->items[j]);
                        Value *cur=map_get(m->map,ks);
                        int64_t cnt=cur&&VAL_TAG(cur)==XS_INT?VAL_INT(cur):0;
                        Value *nv=xs_int(cnt+1); map_set(m->map,ks,nv); value_decref(nv);
                        free(ks);
                    }
                    mc_result=m;
                } else if (strcmp(mc_name,"reversed")==0) {
                    Value *arr=xs_array_new();
                    for(int j=mc_obj->arr->len-1;j>=0;j--) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"scan")==0&&mc_argc>=2) {
                    /* scan(init, fn): init first, fn second, matching interpreter */
                    Value *acc=value_incref(mc_args[0]);
                    Value *arr=xs_array_new();
                    array_push(arr->arr,value_incref(acc));
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *pair[2]={acc,mc_obj->arr->items[j]};
                        Value *r=vm_invoke(vm,mc_args[1],pair,2);
                        value_decref(acc); acc=r?r:value_incref(XS_NULL_VAL);
                        array_push(arr->arr,value_incref(acc));
                    }
                    value_decref(acc);
                    frame=FRAME;
                    mc_result=arr;
                } else if (strcmp(mc_name,"prepend")==0&&mc_argc>=1) {
                    array_push(mc_obj->arr,value_incref(XS_NULL_VAL));
                    for(int k=mc_obj->arr->len-1;k>0;k--) mc_obj->arr->items[k]=mc_obj->arr->items[k-1];
                    mc_obj->arr->items[0]=value_incref(mc_args[0]);
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"clear")==0) {
                    for(int j=0;j<mc_obj->arr->len;j++) value_decref(mc_obj->arr->items[j]);
                    mc_obj->arr->len=0;
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"push_back")==0||strcmp(mc_name,"add")==0) {
                    for(int j=0;j<mc_argc;j++) array_push(mc_obj->arr,value_incref(mc_args[j]));
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"push_front")==0) {
                    for(int j=0;j<mc_argc;j++){
                        array_push(mc_obj->arr,value_incref(XS_NULL_VAL));
                        for(int k=mc_obj->arr->len-1;k>0;k--) mc_obj->arr->items[k]=mc_obj->arr->items[k-1];
                        mc_obj->arr->items[0]=value_incref(mc_args[j]);
                    }
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"pop_back")==0) {
                    if(mc_obj->arr->len>0){
                        mc_result=value_incref(mc_obj->arr->items[mc_obj->arr->len-1]);
                        value_decref(mc_obj->arr->items[--mc_obj->arr->len]);
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"pop_front")==0) {
                    if(mc_obj->arr->len>0){
                        mc_result=value_incref(mc_obj->arr->items[0]);
                        value_decref(mc_obj->arr->items[0]);
                        for(int j=1;j<mc_obj->arr->len;j++) mc_obj->arr->items[j-1]=mc_obj->arr->items[j];
                        mc_obj->arr->len--;
                    } else mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"extend")==0&&mc_argc>=1&&(VAL_TAG(mc_args[0])==XS_ARRAY||VAL_TAG(mc_args[0])==XS_TUPLE)) {
                    for(int j=0;j<mc_args[0]->arr->len;j++) array_push(mc_obj->arr,value_incref(mc_args[0]->arr->items[j]));
                    mc_result=value_incref(XS_NULL_VAL);
                } else if (strcmp(mc_name,"shuffle")==0) {
                    int n2=mc_obj->arr->len;
                    for(int j=n2-1;j>0;j--){
                        int k=rand()%(j+1);
                        Value *tmp=mc_obj->arr->items[j]; mc_obj->arr->items[j]=mc_obj->arr->items[k]; mc_obj->arr->items[k]=tmp;
                    }
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"skip")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t n2=VAL_INT(mc_args[0]); if(n2<0) n2=0;
                    if(n2>mc_obj->arr->len) n2=mc_obj->arr->len;
                    Value *arr=xs_array_new();
                    for(int64_t j=n2;j<mc_obj->arr->len;j++) array_push(arr->arr,value_incref(mc_obj->arr->items[j]));
                    mc_result=arr;
                } else if (strcmp(mc_name,"to_array")==0) {
                    mc_result=value_incref(mc_obj);
                } else if (strcmp(mc_name,"total")==0) {
                    int64_t si=0; double sf=0; int is_float=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(VAL_TAG(mc_obj->arr->items[j])==XS_INT) si+=VAL_INT(mc_obj->arr->items[j]);
                        else if(VAL_TAG(mc_obj->arr->items[j])==XS_FLOAT){sf+=mc_obj->arr->items[j]->f;is_float=1;}
                    }
                    mc_result=is_float?xs_float(sf+(double)si):xs_int(si);
                } else if (strcmp(mc_name,"sum_by")==0&&mc_argc>=1) {
                    int64_t si=0; double sf=0; int is_float=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        Value *r=vm_invoke(vm,mc_args[0],&mc_obj->arr->items[j],1);
                        if(r&&VAL_TAG(r)==XS_INT) si+=VAL_INT(r);
                        else if(r&&VAL_TAG(r)==XS_FLOAT){sf+=r->f;is_float=1;}
                        value_decref(r);
                    }
                    frame=FRAME;
                    mc_result=is_float?xs_float(sf+(double)si):xs_int(si);
                } else if (strcmp(mc_name,"from_chars")==0) {
                    /* join array of chars into a string */
                    size_t cap=256; char *buf=xs_malloc(cap); size_t wpos=0;
                    for(int j=0;j<mc_obj->arr->len;j++){
                        if(VAL_TAG(mc_obj->arr->items[j])==XS_STR){
                            size_t cl=strlen(mc_obj->arr->items[j]->s);
                            while(wpos+cl+1>cap){cap*=2;buf=xs_realloc(buf,cap);}
                            memcpy(buf+wpos,mc_obj->arr->items[j]->s,cl); wpos+=cl;
                        }
                    }
                    buf[wpos]='\0'; mc_result=xs_str(buf); free(buf);
                } else if (strcmp(mc_name,"is_a")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    mc_result=xs_bool(strcmp(mc_args[0]->s,"array")==0||strcmp(mc_args[0]->s,"Array")==0||strcmp(mc_args[0]->s,"List")==0);
                } else mc_result=value_incref(XS_NULL_VAL);
            }
            else if (VAL_TAG(mc_obj)==XS_INT||VAL_TAG(mc_obj)==XS_FLOAT||VAL_TAG(mc_obj)==XS_BIGINT) {
                double num_f=(VAL_TAG(mc_obj)==XS_FLOAT)?mc_obj->f:(VAL_TAG(mc_obj)==XS_BIGINT?bigint_to_double(mc_obj->bigint):(double)VAL_INT(mc_obj));
                int64_t num_i=(VAL_TAG(mc_obj)==XS_INT)?VAL_INT(mc_obj):(VAL_TAG(mc_obj)==XS_BIGINT?(int64_t)bigint_to_double(mc_obj->bigint):(int64_t)mc_obj->f);
                if (strcmp(mc_name,"is_even")==0) {
                    if (VAL_TAG(mc_obj)==XS_INT) mc_result=xs_bool(VAL_INT(mc_obj)%2==0);
                    else if (VAL_TAG(mc_obj)==XS_BIGINT)
                        mc_result=xs_bool((mc_obj->bigint->len==0) || (mc_obj->bigint->limbs[0]%2==0));
                    else mc_result=value_incref(XS_FALSE_VAL);
                }
                else if (strcmp(mc_name,"is_odd")==0) {
                    if (VAL_TAG(mc_obj)==XS_INT) mc_result=xs_bool(VAL_INT(mc_obj)%2!=0);
                    else if (VAL_TAG(mc_obj)==XS_BIGINT)
                        mc_result=xs_bool((mc_obj->bigint->len>0) && (mc_obj->bigint->limbs[0]%2!=0));
                    else mc_result=value_incref(XS_FALSE_VAL);
                }
                else if (strcmp(mc_name,"is_nan")==0)
                    mc_result=VAL_TAG(mc_obj)==XS_FLOAT?xs_bool(isnan(mc_obj->f)):value_incref(XS_FALSE_VAL);
                else if (strcmp(mc_name,"is_inf")==0)
                    mc_result=VAL_TAG(mc_obj)==XS_FLOAT?xs_bool(isinf(mc_obj->f)):value_incref(XS_FALSE_VAL);
                else if (strcmp(mc_name,"abs")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj)<0?-VAL_INT(mc_obj):VAL_INT(mc_obj));
                    else mc_result=xs_float(fabs(mc_obj->f));
                } else if (strcmp(mc_name,"floor")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj));
                    else mc_result=xs_int((int64_t)floor(mc_obj->f));
                } else if (strcmp(mc_name,"ceil")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj));
                    else mc_result=xs_int((int64_t)ceil(mc_obj->f));
                } else if (strcmp(mc_name,"round")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj));
                    else mc_result=xs_int((int64_t)round(mc_obj->f));
                } else if (strcmp(mc_name,"trunc")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj));
                    else mc_result=xs_int((int64_t)trunc(mc_obj->f));
                } else if (strcmp(mc_name,"sign")==0) {
                    if(VAL_TAG(mc_obj)==XS_INT) mc_result=xs_int(VAL_INT(mc_obj)>0?1:(VAL_INT(mc_obj)<0?-1:0));
                    else mc_result=xs_int(mc_obj->f>0.0?1:(mc_obj->f<0.0?-1:0));
                } else if (strcmp(mc_name,"clamp")==0&&mc_argc>=2) {
                    double lo=(VAL_TAG(mc_args[0])==XS_FLOAT)?mc_args[0]->f:(double)VAL_INT(mc_args[0]);
                    double hi=(VAL_TAG(mc_args[1])==XS_FLOAT)?mc_args[1]->f:(double)VAL_INT(mc_args[1]);
                    if(VAL_TAG(mc_obj)==XS_INT){
                        int64_t loi=(int64_t)lo,hii=(int64_t)hi;
                        int64_t v=VAL_INT(mc_obj)<loi?loi:(VAL_INT(mc_obj)>hii?hii:VAL_INT(mc_obj));
                        mc_result=xs_int(v);
                    } else {
                        double v=num_f<lo?lo:(num_f>hi?hi:num_f);
                        mc_result=xs_float(v);
                    }
                } else if (strcmp(mc_name,"to_str")==0||strcmp(mc_name,"to_string")==0) {
                    if(VAL_TAG(mc_obj)==XS_BIGINT) {
                        char *s = value_str(mc_obj);
                        mc_result = xs_str(s);
                        free(s);
                    } else {
                        char buf[64];
                        if(VAL_TAG(mc_obj)==XS_INT) snprintf(buf,sizeof(buf),"%lld",(long long)VAL_INT(mc_obj));
                        else snprintf(buf,sizeof(buf),"%g",mc_obj->f);
                        mc_result=xs_str(buf);
                    }
                } else if (strcmp(mc_name,"to_char")==0) {
                    char buf[2]={(char)(num_i&0xFF),0};
                    mc_result=xs_str(buf);
                } else if (strcmp(mc_name,"digits")==0) {
                    int64_t n2=VAL_TAG(mc_obj)==XS_INT?VAL_INT(mc_obj):(int64_t)mc_obj->f;
                    if(n2<0) n2=-n2;
                    Value *arr=xs_array_new();
                    if(n2==0){ array_push(arr->arr,xs_int(0)); }
                    else {
                        char tmp[32]; int tlen=snprintf(tmp,sizeof(tmp),"%lld",(long long)n2);
                        for(int j=0;j<tlen;j++) array_push(arr->arr,xs_int(tmp[j]-'0'));
                    }
                    mc_result=arr;
                } else if (strcmp(mc_name,"to_hex")==0) {
                    char buf[32]; snprintf(buf,sizeof(buf),"%llx",(long long)num_i);
                    mc_result=xs_str(buf);
                } else if (strcmp(mc_name,"to_oct")==0) {
                    char buf[32]; snprintf(buf,sizeof(buf),"%llo",(long long)num_i);
                    mc_result=xs_str(buf);
                } else if (strcmp(mc_name,"to_bin")==0) {
                    uint64_t v2=(uint64_t)num_i;
                    if(v2==0){mc_result=xs_str("0");}
                    else{
                        char buf[65]; int pos=64; buf[pos]='\0';
                        while(v2>0){buf[--pos]=(char)('0'+(v2&1));v2>>=1;}
                        mc_result=xs_str(buf+pos);
                    }
                } else if (strcmp(mc_name,"is_a")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    int match=(VAL_TAG(mc_obj)==XS_INT&&(strcmp(mc_args[0]->s,"int")==0||strcmp(mc_args[0]->s,"Int")==0))||
                              (VAL_TAG(mc_obj)==XS_FLOAT&&(strcmp(mc_args[0]->s,"float")==0||strcmp(mc_args[0]->s,"Float")==0));
                    mc_result=xs_bool(match);
                } else mc_result=value_incref(XS_NULL_VAL);
                (void)num_f; (void)num_i;
            }
            else if (VAL_TAG(mc_obj) == XS_RANGE && mc_obj->range) {
                XSRange *rr = mc_obj->range;
                int64_t len = rr->inclusive ? (rr->end - rr->start + 1)
                                            : (rr->end - rr->start);
                if (len < 0) len = 0;
                if (strcmp(mc_name,"len")==0 || strcmp(mc_name,"size")==0)
                    mc_result = xs_int(len);
                else if (strcmp(mc_name,"start")==0)
                    mc_result = xs_int(rr->start);
                else if (strcmp(mc_name,"end")==0)
                    mc_result = xs_int(rr->end);
                else if (strcmp(mc_name,"is_empty")==0)
                    mc_result = xs_bool(len == 0);
                else if (strcmp(mc_name,"contains")==0 && mc_argc>=1 && VAL_TAG(mc_args[0])==XS_INT) {
                    int64_t v = VAL_INT(mc_args[0]);
                    int ok = rr->inclusive
                        ? (v >= rr->start && v <= rr->end)
                        : (v >= rr->start && v <  rr->end);
                    mc_result = xs_bool(ok);
                }
                else if (strcmp(mc_name,"to_array")==0 || strcmp(mc_name,"collect")==0) {
                    Value *arr = xs_array_new();
                    int64_t stop = rr->inclusive ? rr->end + 1 : rr->end;
                    for (int64_t v = rr->start; v < stop; v++) {
                        Value *iv = xs_int(v);
                        array_push(arr->arr, iv);
                        value_decref(iv);
                    }
                    mc_result = arr;
                }
                else mc_result = value_incref(XS_NULL_VAL);
            }
            else if (VAL_TAG(mc_obj) == XS_REGEX && mc_obj->s) {
                /* Regex methods: .test/.is_match, .match/.find, .replace,
                   .source/.pattern. Mirrors interp.c. */
                const char *pat = mc_obj->s;
                if ((strcmp(mc_name,"test")==0 || strcmp(mc_name,"is_match")==0)
                        && mc_argc>=1 && VAL_TAG(mc_args[0])==XS_STR) {
                    regex_t re;
                    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) == 0) {
                        int ok = (regexec(&re, mc_args[0]->s, 0, NULL, 0) == 0);
                        regfree(&re);
                        mc_result = xs_bool(ok);
                    } else {
                        mc_result = value_incref(XS_FALSE_VAL);
                    }
                } else if ((strcmp(mc_name,"match")==0 || strcmp(mc_name,"find")==0)
                        && mc_argc>=1 && VAL_TAG(mc_args[0])==XS_STR) {
                    regex_t re;
                    if (regcomp(&re, pat, REG_EXTENDED) == 0) {
                        regmatch_t m[1];
                        if (regexec(&re, mc_args[0]->s, 1, m, 0) == 0) {
                            int len = m[0].rm_eo - m[0].rm_so;
                            mc_result = xs_str_n(mc_args[0]->s + m[0].rm_so, (size_t)len);
                        } else {
                            mc_result = value_incref(XS_NULL_VAL);
                        }
                        regfree(&re);
                    } else {
                        mc_result = value_incref(XS_NULL_VAL);
                    }
                } else if (strcmp(mc_name,"replace")==0 && mc_argc>=2
                        && VAL_TAG(mc_args[0])==XS_STR && VAL_TAG(mc_args[1])==XS_STR) {
                    regex_t re;
                    if (regcomp(&re, pat, REG_EXTENDED) == 0) {
                        regmatch_t m[1];
                        if (regexec(&re, mc_args[0]->s, 1, m, 0) == 0) {
                            int pre_len = m[0].rm_so;
                            int post_start = m[0].rm_eo;
                            int rlen = (int)strlen(mc_args[1]->s);
                            int slen = (int)strlen(mc_args[0]->s);
                            char *buf = xs_malloc((size_t)(pre_len + rlen + slen - post_start + 1));
                            memcpy(buf, mc_args[0]->s, (size_t)pre_len);
                            memcpy(buf + pre_len, mc_args[1]->s, (size_t)rlen);
                            memcpy(buf + pre_len + rlen, mc_args[0]->s + post_start, (size_t)(slen - post_start));
                            buf[pre_len + rlen + slen - post_start] = '\0';
                            mc_result = xs_str(buf); free(buf);
                        } else {
                            mc_result = value_incref(mc_args[0]);
                        }
                        regfree(&re);
                    } else {
                        mc_result = value_incref(mc_args[0]);
                    }
                } else if (strcmp(mc_name,"source")==0 || strcmp(mc_name,"pattern")==0) {
                    mc_result = xs_str(pat);
                } else {
                    mc_result = value_incref(XS_NULL_VAL);
                }
            }
            else {
                /* generic methods for any remaining types */
                if (strcmp(mc_name,"is_a")==0&&mc_argc>=1&&VAL_TAG(mc_args[0])==XS_STR) {
                    const char *tn = mc_args[0]->s;
                    int match = 0;
                    if (VAL_TAG(mc_obj)==XS_STR && (strcmp(tn,"str")==0||strcmp(tn,"String")==0)) match=1;
                    else if (VAL_TAG(mc_obj)==XS_INT && (strcmp(tn,"int")==0||strcmp(tn,"Int")==0)) match=1;
                    else if (VAL_TAG(mc_obj)==XS_FLOAT && (strcmp(tn,"float")==0||strcmp(tn,"Float")==0)) match=1;
                    else if ((VAL_TAG(mc_obj)==XS_ARRAY||VAL_TAG(mc_obj)==XS_TUPLE) && (strcmp(tn,"array")==0||strcmp(tn,"Array")==0||strcmp(tn,"List")==0)) match=1;
                    else if (VAL_TAG(mc_obj)==XS_BOOL && (strcmp(tn,"bool")==0||strcmp(tn,"Bool")==0)) match=1;
                    mc_result = xs_bool(match);
                } else {
                /* check plugin methods */
                Value *pmethods = map_get(vm->globals, "__plugin_methods");
                if (pmethods && VAL_TAG(pmethods) == XS_MAP) {
                    const char *type_name = NULL;
                    if (VAL_TAG(mc_obj) == XS_STR) type_name = "str";
                    else if (VAL_TAG(mc_obj) == XS_INT) type_name = "int";
                    else if (VAL_TAG(mc_obj) == XS_FLOAT) type_name = "float";
                    else if (VAL_TAG(mc_obj) == XS_ARRAY) type_name = "array";
                    else if (VAL_TAG(mc_obj) == XS_BOOL) type_name = "bool";
                    if (type_name) {
                        Value *tm = map_get(pmethods->map, type_name);
                        if (tm && VAL_TAG(tm) == XS_MAP) {
                            Value *pfn = map_get(tm->map, mc_name);
                            if (pfn && (VAL_TAG(pfn) == XS_CLOSURE || VAL_TAG(pfn) == XS_NATIVE)) {
                                /* call plugin method with self as first arg */
                                Value *pargs[17];
                                pargs[0] = mc_obj;
                                int total = 1 + mc_argc;
                                if (total > 17) total = 17;
                                for (int pj = 0; pj < mc_argc && pj < 16; pj++) pargs[pj+1] = mc_args[pj];
                                mc_result = vm_invoke(vm, pfn, pargs, total);
                                frame = FRAME;
                                if (!mc_result) mc_result = value_incref(XS_NULL_VAL);
                            } else mc_result = value_incref(XS_NULL_VAL);
                        } else mc_result = value_incref(XS_NULL_VAL);
                    } else mc_result = value_incref(XS_NULL_VAL);
                } else mc_result = value_incref(XS_NULL_VAL);
                } /* end is_a else */
            }

            /* check plugin methods if result is null */
            if (!mc_called && mc_result && VAL_TAG(mc_result) == XS_NULL) {
                Value *pmethods = map_get(vm->globals, "__plugin_methods");
                if (pmethods && VAL_TAG(pmethods) == XS_MAP) {
                    const char *ptype = NULL;
                    if (VAL_TAG(mc_obj) == XS_STR) ptype = "str";
                    else if (VAL_TAG(mc_obj) == XS_INT) ptype = "int";
                    else if (VAL_TAG(mc_obj) == XS_FLOAT) ptype = "float";
                    else if (VAL_TAG(mc_obj) == XS_ARRAY) ptype = "array";
                    else if (VAL_TAG(mc_obj) == XS_BOOL) ptype = "bool";
                    if (ptype) {
                        Value *tm = map_get(pmethods->map, ptype);
                        if (tm && VAL_TAG(tm) == XS_MAP) {
                            Value *pfn = map_get(tm->map, mc_name);
                            if (pfn && (VAL_TAG(pfn) == XS_CLOSURE || VAL_TAG(pfn) == XS_NATIVE)) {
                                Value *pargs[17];
                                pargs[0] = mc_obj;
                                int ptotal = 1 + mc_argc;
                                if (ptotal > 17) ptotal = 17;
                                for (int pj = 0; pj < mc_argc && pj < 16; pj++) pargs[pj+1] = mc_args[pj];
                                value_decref(mc_result);
                                mc_result = vm_invoke(vm, pfn, pargs, ptotal);
                                frame = FRAME;
                                if (!mc_result) mc_result = value_incref(XS_NULL_VAL);
                            }
                        }
                    }
                }
            }
            if (!mc_called) {
                for(int j=0;j<mc_argc;j++) value_decref(POP());
                value_decref(POP()); /* obj */
                PUSH(mc_result ? mc_result : value_incref(XS_NULL_VAL));
            }
            break;
        }

        case OP_SWAP: {
            Value *b = POP(), *a = POP();
            PUSH(b); PUSH(a);
            break;
        }

        /* bitwise */
        case OP_BAND: { Value *b=POP(),*a=POP(); PUSH(xs_int(VAL_INT(a) & VAL_INT(b))); value_decref(a);value_decref(b); break; }
        case OP_BOR:  { Value *b=POP(),*a=POP(); PUSH(xs_int(VAL_INT(a) | VAL_INT(b))); value_decref(a);value_decref(b); break; }
        case OP_BXOR: { Value *b=POP(),*a=POP(); PUSH(xs_int(VAL_INT(a) ^ VAL_INT(b))); value_decref(a);value_decref(b); break; }
        case OP_BNOT: { Value *a=POP(); PUSH(xs_int(~VAL_INT(a))); value_decref(a); break; }
        case OP_SHL:  {
            Value *b=POP(),*a=POP();
            int64_t n2 = VAL_INT(b);
            if (n2 < 0) {
                value_decref(a); value_decref(b);
                xs_runtime_error(span_zero(), "ValueError", NULL,
                                 "shift count cannot be negative");
                PUSH(value_incref(XS_NULL_VAL));
                break;
            }
            if (n2 >= 64) {
                /* Result is logically 0 (or overflowed bigint); avoid
                   the C-undefined `<< 64+` and just emit 0. */
                value_decref(a); value_decref(b);
                PUSH(xs_int(0));
                break;
            }
            int64_t av = VAL_INT(a);
            /* Detect signed overflow before doing the shift; promote to
               bigint when the result wouldn't fit in i64. */
            if (n2 > 0 && (av >> (63 - n2)) != 0 && (av >> (63 - n2)) != -1) {
                XSBigInt *bi = bigint_from_i64(av);
                XSBigInt *out = bigint_shl(bi, (int)n2);
                bigint_free(bi);
                PUSH(xs_bigint_val(out));
            } else {
                PUSH(xs_int(av << n2));
            }
            value_decref(a); value_decref(b); break;
        }
        case OP_SHR:  {
            Value *b=POP(),*a=POP();
            int64_t n2 = VAL_INT(b);
            if (n2 < 0) {
                value_decref(a); value_decref(b);
                xs_runtime_error(span_zero(), "ValueError", NULL,
                                 "shift count cannot be negative");
                PUSH(value_incref(XS_NULL_VAL));
                break;
            }
            if (n2 >= 64) {
                /* Arithmetic shift: -1 stays -1, otherwise 0. */
                int64_t av = VAL_INT(a);
                value_decref(a); value_decref(b);
                PUSH(xs_int(av < 0 ? -1 : 0));
                break;
            }
            PUSH(xs_int(VAL_INT(a) >> n2));
            value_decref(a); value_decref(b); break;
        }

        case OP_TRY_BEGIN: {
            if (frame->try_depth < VM_TRY_STACK_MAX) {
                TryEntry *te = &frame->try_stack[frame->try_depth++];
                te->catch_ip  = frame->ip + INSTR_sBx(instr);
                te->stack_top = vm->sp;
                g_xs_in_try++;
            }
            break;
        }
        case OP_TRY_END: {
            if (frame->try_depth > 0) {
                frame->try_depth--;
                if (g_xs_in_try > 0) g_xs_in_try--;
            }
            break;
        }
        case OP_THROW: {
            Value *exc = POP();
            int handled = 0;
            while (vm->frame_count > 0) {
                CallFrame *cf = &vm->frames[vm->frame_count - 1];
                if (cf->try_depth > 0) {
                    TryEntry *te = &cf->try_stack[--cf->try_depth];
                    if (g_xs_in_try > 0) g_xs_in_try--;
                    while (vm->sp > te->stack_top) value_decref(POP());
                    PUSH(exc);
                    frame = cf;
                    frame->ip = te->catch_ip;
                    handled = 1;
                    break;
                }
                /* No catch on this frame: drain pending defers first,
                   parking the exception on the VM so OP_RETURN's defer
                   completion path can pick it up and keep unwinding. */
                if (cf->defer_depth > 0) {
                    vm->pending_throw_frame = vm->frame_count - 1;
                    if (vm->pending_throw_exc) value_decref(vm->pending_throw_exc);
                    vm->pending_throw_exc = exc;
                    cf->defer_return_ip = cf->ip;
                    cf->defer_depth--;
                    cf->ip = cf->defer_stack[cf->defer_depth].defer_ip;
                    frame = cf;
                    handled = 1;
                    break;
                }
                upvalue_close_all(&vm->open_upvalues, cf->base);
                while (vm->sp > cf->base) value_decref(POP());
                value_decref(cf->closure_val);
                vm->frame_count--;
            }
            if (!handled) {
                if (vm->is_thread_worker) {
                    vm->uncaught_thread_exc = exc;
                    return 1;
                }
                char *s = value_str(exc);
                fprintf(stderr, "uncaught: %s\n", s);
                free(s);
                value_decref(exc);
                return 1;
            }
            break;
        }
        case OP_CATCH:
            break;

        // --- trace
        case OP_TRACE_CALL: {
#ifdef XSC_ENABLE_TRACER
            if (vm->tracer) {
                const char *fn_name = PROTO->name ? PROTO->name : "<anon>";
                tracer_record_call(vm->tracer, fn_name, 0);
            }
#endif
            break;
        }
        case OP_TRACE_RETURN: {
#ifdef XSC_ENABLE_TRACER
            if (vm->tracer) {
                const char *fn_name = PROTO->name ? PROTO->name : "<anon>";
                Value *retval = (vm->sp > vm->stack) ? PEEK(0) : NULL;
                tracer_record_return(vm->tracer, fn_name, retval);
            }
#endif
            break;
        }
        case OP_TRACE_STORE: {
#ifdef XSC_ENABLE_TRACER
            if (vm->tracer) {
                uint16_t name_idx = INSTR_Bx(instr);
                Value *name_val = PROTO->chunk.consts[name_idx];
                const char *var_name = (name_val && VAL_TAG(name_val) == XS_STR) ? name_val->s : "?";
                Value *val = (vm->sp > vm->stack) ? PEEK(0) : NULL;
                tracer_record_store(vm->tracer, var_name, val);
            }
#endif
            break;
        }
        case OP_TRACE_IO: {
#ifdef XSC_ENABLE_TRACER
            if (vm->tracer) {
                Value *top = (vm->sp > vm->stack) ? PEEK(0) : NULL;
                const char *buf = NULL;
                int buf_len = 0;
                if (top && VAL_TAG(top) == XS_STR && top->s) {
                    buf = top->s;
                    buf_len = (int)strlen(top->s);
                }
                tracer_record_io(vm->tracer, "io", (void *)buf, buf_len);
            }
#endif
            break;
        }

        // --- logical
        case OP_AND: {
            Value *b = POP(), *a = POP();
            int at = value_truthy(a);
            if (!at) { PUSH(a); value_decref(b); }
            else     { PUSH(b); value_decref(a); }
            break;
        }
        case OP_OR: {
            Value *b = POP(), *a = POP();
            int at = value_truthy(a);
            if (at) { PUSH(a); value_decref(b); }
            else    { PUSH(b); value_decref(a); }
            break;
        }

        /* spread */
        case OP_SPREAD: {
            Value *arr = POP();
            if (VAL_TAG(arr) == XS_ARRAY) {
                for (int si = 0; si < arr->arr->len; si++) {
                    value_incref(arr->arr->items[si]);
                    PUSH(arr->arr->items[si]);
                }
            }
            value_decref(arr);
            break;
        }

        case OP_LOOP:
            frame->ip += INSTR_sBx(instr);
            break;

        // --- effects
        case OP_EFFECT_HANDLE: {
            if (frame->try_depth < VM_TRY_STACK_MAX) {
                TryEntry *te = &frame->try_stack[frame->try_depth++];
                te->catch_ip  = frame->ip + INSTR_sBx(instr);
                te->stack_top = vm->sp;
            }
            break;
        }
        case OP_EFFECT_CALL: {
            int argc_eff = (int)INSTR_A(instr);
            const char *eff_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            (void)eff_name;

            Value *eff_args[16];
            for (int i = argc_eff - 1; i >= 0; i--) eff_args[i] = POP();

            /* Push a fresh continuation onto the LIFO stack so nested
             * perform/handle pairs each get their own snapshot. The
             * outer perform's snapshot stays untouched while the inner
             * handler runs; resume in the outer handler still rewinds
             * to its own saved body state. */
            if (vm->eff_stack_count >= vm->eff_stack_cap) {
                int new_cap = vm->eff_stack_cap ? vm->eff_stack_cap * 2 : 8;
                vm->eff_stack = realloc(vm->eff_stack,
                    (size_t)new_cap * sizeof(EffectCont));
                memset(vm->eff_stack + vm->eff_stack_cap, 0,
                       (size_t)(new_cap - vm->eff_stack_cap) * sizeof(EffectCont));
                vm->eff_stack_cap = new_cap;
            }
            Value *eff_val = (argc_eff > 0) ? eff_args[0] : value_incref(XS_NULL_VAL);
            for (int i = 1; i < argc_eff; i++) value_decref(eff_args[i]);

            /* Reserve a slot on the LIFO continuation stack BEFORE we
             * scan for a handler, so the matched-handler block can
             * just bump the count and write into the reserved entry. */
            if (vm->eff_stack_count >= vm->eff_stack_cap) {
                int new_cap = vm->eff_stack_cap ? vm->eff_stack_cap * 2 : 8;
                vm->eff_stack = realloc(vm->eff_stack,
                    (size_t)new_cap * sizeof(EffectCont));
                memset(vm->eff_stack + vm->eff_stack_cap, 0,
                       (size_t)(new_cap - vm->eff_stack_cap) * sizeof(EffectCont));
                vm->eff_stack_cap = new_cap;
            }

            /* find handler (scan try stack) */
            int eff_handled = 0;
            for (int fi = vm->frame_count - 1; fi >= 0; fi--) {
                CallFrame *cf = &vm->frames[fi];
                if (cf->try_depth > 0) {
                    TryEntry *te = &cf->try_stack[cf->try_depth - 1];
                    /* Capture frames + suspended-body stack slice now
                     * that we know which handler matched. We snapshot
                     * BEFORE the try entry is consumed, so resume can
                     * re-enter the handler context (a second perform
                     * inside the same handle still finds the same
                     * try entry to dispatch on). The slice is
                     * [te->stack_top, vm->sp): everything above the
                     * handler frame's TRY_BEGIN sp belongs to the
                     * suspended body and inner calls. The handler's
                     * own frame stays untouched so closure mutations
                     * the arm body performs persist past resume. */
                    EffectCont *cur = &vm->eff_stack[vm->eff_stack_count++];
                    cur->frame_count = vm->frame_count;
                    if (cur->frames_cap < vm->frame_count) {
                        cur->frames_cap = vm->frame_count;
                        cur->frames = realloc(cur->frames,
                            (size_t)cur->frames_cap * sizeof(CallFrame));
                    }
                    memcpy(cur->frames, vm->frames,
                           sizeof(CallFrame) * (size_t)vm->frame_count);
                    /* Now consume the try entry from the live frames. */
                    cf->try_depth--;
                    cur->sp_off = (int)(vm->sp - vm->stack);
                    cur->stack_top_off = (int)(te->stack_top - vm->stack);
                    cur->valid = 1;
                    {
                        int lo = cur->stack_top_off;
                        int hi = cur->sp_off;
                        if (lo < 0) lo = 0;
                        if (hi < lo) hi = lo;
                        int n = hi - lo;
                        if (cur->snapshot_cap < n) {
                            cur->stack_snapshot = realloc(cur->stack_snapshot,
                                (size_t)n * sizeof(Value *));
                            cur->snapshot_cap = n;
                        }
                        for (int si = 0; si < n; si++) {
                            Value *sv = vm->stack[lo + si];
                            cur->stack_snapshot[si] =
                                sv ? value_incref(sv) : NULL;
                        }
                        cur->snapshot_len = n;
                    }
                    /* mirror into legacy single slot. */
                    vm->eff_cont = *cur;
                    vm->eff_cont.frames = NULL;
                    vm->eff_cont.frames_cap = 0;
                    vm->eff_cont.stack_snapshot = NULL;
                    vm->eff_cont.snapshot_cap = 0;
                    vm->eff_cont.snapshot_len = 0;
                    vm->eff_cont.valid = 0;

                    /* DON'T truncate sp to te->stack_top: the memory
                     * between there and sp_off is the suspended nested
                     * frames' live local slots. Resume will rewind
                     * cleanly via sp_off + the snapshot. */
                    PUSH(eff_val);
                    Value *eff_name_v = PROTO->chunk.consts[INSTR_Bx(instr)];
                    PUSH(value_incref(eff_name_v));
                    vm->frame_count = fi + 1;
                    frame = cf;
                    frame->ip = te->catch_ip;
                    eff_handled = 1;
                    break;
                }
            }
            if (!eff_handled) {
                if (vm->is_thread_worker) {
                    /* Spawn worker has no access to the parent's
                       handler stack. Surface the unhandled effect as a
                       throw so `await` can rethrow it on the awaiter
                       (where a handler may exist). */
                    Value *err = xs_map_new();
                    Value *kind = xs_str("UnhandledEffect");
                    map_set(err->map, "kind", kind); value_decref(kind);
                    Value *msg = xs_str("effect performed in spawn worker without an in-thread handler");
                    map_set(err->map, "message", msg); value_decref(msg);
                    map_set(err->map, "effect", eff_val);
                    vm->uncaught_thread_exc = err;
                    return 1;
                }
                fprintf(stderr, "unhandled effect\n");
                value_decref(eff_val);
                return 1;
            }
            break;
        }
        case OP_EFFECT_RESUME: {
            Value *resume_val = POP();
            EffectCont *cur = NULL;
            if (vm->eff_stack_count > 0) {
                cur = &vm->eff_stack[vm->eff_stack_count - 1];
                if (!cur->valid) cur = NULL;
            }
            if (cur) {
                /* First resume in this arm body: snapshot the arm's
                 * current frames + stack so a later HANDLE_BODY_END
                 * can return body's value here. The body snapshot
                 * (cur->frames / stack_snapshot) is left intact so a
                 * second resume after the body returns can replay it
                 * fresh -- multi-shot resume is just multiple replays
                 * of the same captured continuation. */
                if (!cur->in_resume) {
                    cur->arm_frame_count = vm->frame_count;
                    if (cur->arm_frames_cap < vm->frame_count) {
                        cur->arm_frames_cap = vm->frame_count;
                        cur->arm_frames = realloc(cur->arm_frames,
                            (size_t)cur->arm_frames_cap * sizeof(CallFrame));
                    }
                    memcpy(cur->arm_frames, vm->frames,
                           sizeof(CallFrame) * (size_t)vm->frame_count);
                    cur->arm_sp_off = (int)(vm->sp - vm->stack);
                    int n = cur->arm_sp_off;
                    if (cur->arm_snapshot_cap < n) {
                        cur->arm_snapshot_cap = n;
                        cur->arm_stack_snapshot = realloc(cur->arm_stack_snapshot,
                            (size_t)n * sizeof(Value *));
                    }
                    for (int si = 0; si < n; si++) {
                        Value *v = vm->stack[si];
                        cur->arm_stack_snapshot[si] = v ? value_incref(v) : NULL;
                    }
                    cur->arm_snapshot_len = n;
                    cur->in_resume = 1;
                }

                /* Restore body frames + stack slice. */
                if (vm->frames_cap < cur->frame_count) {
                    vm->frames_cap = cur->frame_count;
                    vm->frames = realloc(vm->frames,
                        (size_t)vm->frames_cap * sizeof(CallFrame));
                }
                memcpy(vm->frames, cur->frames,
                       sizeof(CallFrame) * (size_t)cur->frame_count);
                vm->frame_count = cur->frame_count;

                int lo = cur->stack_top_off;
                int hi = cur->sp_off;
                if (lo < 0) lo = 0;
                if (hi < lo) hi = lo;
                if (cur->stack_snapshot) {
                    for (int si = 0; si < cur->snapshot_len; si++) {
                        Value *prev = vm->stack[lo + si];
                        Value *snap = cur->stack_snapshot[si];
                        Value *next = snap ? value_incref(snap) : NULL;
                        if (prev && prev != next) value_decref(prev);
                        vm->stack[lo + si] = next;
                    }
                }
                vm->sp = vm->stack + hi;
                frame = FRAME;

                PUSH(resume_val);
            } else {
                PUSH(resume_val);
            }
            break;
        }

        case OP_HANDLE_BODY_END: {
            /* Body just finished. If a resume from the arm body got us
             * here, swap the body's return value into the arm body's
             * resume call site and continue the arm. Otherwise no-op
             * (body completed naturally without ever performing). */
            EffectCont *cur = NULL;
            if (vm->eff_stack_count > 0) {
                cur = &vm->eff_stack[vm->eff_stack_count - 1];
                if (!cur->valid || !cur->in_resume) cur = NULL;
            }
            if (cur) {
                /* Steal the body's return value off the stack. Clear
                 * the popped slot so the arm-state restore below
                 * doesn't decref body_val while we still hold the
                 * single live reference. */
                Value *body_val;
                if (vm->sp > vm->stack) {
                    body_val = POP();
                    *vm->sp = NULL;
                } else {
                    body_val = value_incref(XS_NULL_VAL);
                }
                if (vm->frames_cap < cur->arm_frame_count) {
                    vm->frames_cap = cur->arm_frame_count;
                    vm->frames = realloc(vm->frames,
                        (size_t)vm->frames_cap * sizeof(CallFrame));
                }
                memcpy(vm->frames, cur->arm_frames,
                       sizeof(CallFrame) * (size_t)cur->arm_frame_count);
                vm->frame_count = cur->arm_frame_count;
                int n = cur->arm_snapshot_len;
                if (cur->arm_stack_snapshot) {
                    for (int si = 0; si < n; si++) {
                        Value *prev = vm->stack[si];
                        Value *snap = cur->arm_stack_snapshot[si];
                        Value *next = snap ? value_incref(snap) : NULL;
                        if (prev && prev != next) value_decref(prev);
                        vm->stack[si] = next;
                    }
                }
                vm->sp = vm->stack + cur->arm_sp_off;
                frame = FRAME;
                cur->in_resume = 0;
                PUSH(body_val);
            }
            break;
        }

        case OP_EFFECT_DONE: {
            /* Arm body has finished -- release the saved continuation
             * (and its arm snapshot) so a future perform doesn't see
             * stale state. */
            if (vm->eff_stack_count > 0) {
                EffectCont *cur = &vm->eff_stack[vm->eff_stack_count - 1];
                eff_cont_release_snapshot(cur);
                cur->valid = 0;
                vm->eff_stack_count--;
            }
            break;
        }

        // --- async/generators
        case OP_AWAIT: {
            Value *task = POP();
            if (VAL_TAG(task) == XS_MAP) {
                Value *kind = map_get(task->map, "_kind");
                Value *tid_val = map_get(task->map, "_task_id");
                /* Threaded spawn future: kind=="task" + _task_id refers to
                   the OS-thread task table. Block on its condvar until
                   the worker finishes, surfacing any captured throw. */
                if (kind && VAL_TAG(kind) == XS_STR && strcmp(kind->s, "task") == 0
                    && tid_val && VAL_TAG(tid_val) == XS_INT) {
                    extern Value *vm_await_task(int task_id, int *errored_out, Value **err_out);
                    int tid = (int)VAL_INT(tid_val);
                    int errored = 0;
                    Value *err = NULL;
                    Value *r = vm_await_task(tid, &errored, &err);
                    /* Mirror the result into the future map so callers
                       can read _status / _result after await. */
                    {
                        Value *sv = xs_str(errored ? "error" : "done");
                        map_set(task->map, "_status", sv); value_decref(sv);
                    }
                    if (errored && err) {
                        map_set(task->map, "_error", err);
                        value_decref(task);
                        if (r) value_decref(r);
                        /* Re-raise the captured throw on the awaiter's
                           VM so try/catch around the await sees it. */
                        g_xs_pending_throw = err;
                        break;
                    }
                    if (err) value_decref(err);
                    if (r) map_set(task->map, "_result", r);
                    else { Value *nv = value_incref(XS_NULL_VAL); map_set(task->map, "_result", nv); value_decref(nv); }
                    value_decref(task);
                    PUSH(r ? r : value_incref(XS_NULL_VAL));
                    break;
                }
                if (tid_val && VAL_TAG(tid_val) == XS_INT) {
                    int tid = (int)VAL_INT(tid_val);
                    for (int t = 0; t <= tid && t < vm->n_tasks; t++) {
                        if (vm->tasks[t].done) continue;
                        Value *tfn = vm->tasks[t].fn;
                        Value *tres = NULL;
                        if (VAL_TAG(tfn) == XS_NATIVE) {
                            tres = tfn->native(NULL, NULL, 0);
                        } else if (VAL_TAG(tfn) == XS_CLOSURE) {
                            tres = vm_invoke(vm, tfn, NULL, 0);
                        }
                        vm->tasks[t].result = tres ? tres : value_incref(XS_NULL_VAL);
                        vm->tasks[t].done = 1;
                    }
                    if (tid >= 0 && tid < vm->n_tasks && vm->tasks[tid].done) {
                        Value *await_result = vm->tasks[tid].result;
                        PUSH(await_result ? value_incref(await_result) : value_incref(XS_NULL_VAL));
                        { Value *sv = xs_str("done"); map_set(task->map, "_status", sv); value_decref(sv); }
                        if (await_result) map_set(task->map, "_result", await_result);
                    } else {
                        PUSH(value_incref(XS_NULL_VAL));
                    }
                    value_decref(task);
                } else {
                    Value *status = map_get(task->map, "_status");
                    if (status && VAL_TAG(status) == XS_STR && strcmp(status->s, "done") == 0) {
                        Value *await_result = map_get(task->map, "_result");
                        PUSH(await_result ? value_incref(await_result) : value_incref(XS_NULL_VAL));
                        value_decref(task);
                    } else if (status && VAL_TAG(status) == XS_STR && strcmp(status->s, "pending") == 0) {
                        Value *task_fn = map_get(task->map, "_fn");
                        if (task_fn && VAL_TAG(task_fn) == XS_NATIVE) {
                            Value *await_result = task_fn->native(NULL, NULL, 0);
                            { Value *sv = xs_str("done"); map_set(task->map, "_status", sv); value_decref(sv); }
                            map_set(task->map, "_result", await_result ? await_result : value_incref(XS_NULL_VAL));
                            PUSH(await_result ? await_result : value_incref(XS_NULL_VAL));
                            if (await_result) value_decref(await_result);
                            value_decref(task);
                        } else if (task_fn && VAL_TAG(task_fn) == XS_CLOSURE) {
                            int cl_arity = task_fn->cl->proto->arity;
                            if (cl_arity < 0) cl_arity = -(cl_arity + 1);
                            if (cl_arity == 0) {
                                PUSH(task);
                                value_incref(task_fn);
                                if (call_frame_push(vm, task_fn, 0) == 0) {
                                    value_decref(task_fn);
                                    frame = FRAME;
                                    vm->spawn_task = value_incref(task);
                                    value_decref(task);
                                    break;
                                }
                                value_decref(task_fn);
                                Value *st = POP();
                                PUSH(st);
                            } else {
                                PUSH(task);
                            }
                        } else {
                            Value *await_result = map_get(task->map, "_result");
                            PUSH(await_result ? value_incref(await_result) : value_incref(task));
                            if (!await_result) value_decref(task);
                            else value_decref(task);
                        }
                    } else {
                        Value *await_result = map_get(task->map, "_result");
                        if (await_result) {
                            PUSH(value_incref(await_result));
                            value_decref(task);
                        } else {
                            PUSH(task);
                        }
                    }
                }
            } else if (VAL_TAG(task) == XS_NATIVE) {
                Value *await_result = task->native(NULL, NULL, 0);
                value_decref(task);
                PUSH(await_result ? await_result : value_incref(XS_NULL_VAL));
            } else {
                PUSH(task);
            }
            break;
        }
        case OP_YIELD: {
            Value *val = POP();
            if (frame->is_generator && frame->yield_arr) {
                array_push(frame->yield_arr->arr, val);
                frame->yield_index++;
            } else {
                Value *result = xs_map_new();
                { Value *dv = value_incref(XS_FALSE_VAL); map_set(result->map, "done", dv); value_decref(dv); }
                map_set(result->map, "value", val);
                value_decref(val);
                upvalue_close_all(&vm->open_upvalues, frame->base);
                while (vm->sp > frame->base) value_decref(POP());
                value_decref(frame->closure_val);
                vm->frame_count--;
                if (vm->frame_count == 0) { value_decref(result); return 0; }
                frame = FRAME;
                PUSH(result);
            }
            break;
        }
        case OP_SPAWN: {
            Value *fn = POP();
            /* The closure path runs the body on a real OS thread so two
               sleep / IO bound spawns actually overlap. Synchronous
               execution is wrong even ignoring parallelism: it lets a
               throw inside the body unwind all the way out and crash the
               parent. The thread captures any uncaught error on the
               returned future instead. */
            if (VAL_TAG(fn) == XS_CLOSURE) {
                int cl_arity = fn->cl->proto->arity;
                if (cl_arity < 0) cl_arity = -(cl_arity + 1);
                if (cl_arity == 0) {
                    extern Value *vm_spawn_real(VM *vm, Value *closure);
                    Value *fut = vm_spawn_real(vm, fn);
                    value_decref(fn);
                    /* Register the new task with the enclosing nursery (if
                       any) so its OP_NURSERY_END will join on it. */
                    if (vm->nursery_depth > 0 && fut && VAL_TAG(fut) == XS_MAP) {
                        Value *tid_v = map_get(fut->map, "_task_id");
                        if (tid_v && VAL_TAG(tid_v) == XS_INT) {
                            int d = vm->nursery_depth - 1;
                            if (vm->nursery_lens[d] >= vm->nursery_caps[d]) {
                                int nc = vm->nursery_caps[d] > 0 ? vm->nursery_caps[d] * 2 : 16;
                                vm->nursery_stack[d] = realloc(vm->nursery_stack[d], nc * sizeof(int));
                                vm->nursery_caps[d] = nc;
                            }
                            vm->nursery_stack[d][vm->nursery_lens[d]++] = (int)VAL_INT(tid_v);
                        }
                    }
                    PUSH(fut);
                    break;
                }
                /* Closures with required arguments can't be spawned with
                   no caller-supplied args; preserve the historical
                   "future with no body" placeholder so the remaining
                   tests that exercise this path don't lose semantics. */
                Value *task = xs_map_new();
                { Value *sv = xs_str("done"); map_set(task->map, "_status", sv); value_decref(sv); }
                map_set(task->map, "_result", value_incref(XS_NULL_VAL));
                value_decref(fn);
                PUSH(task);
                break;
            }
            if (VAL_TAG(fn) == XS_NATIVE) {
                /* Native callables bypass the threaded path: they're
                   typically used as actor message handlers and rely on
                   running synchronously under the parent's GIL. */
                Value *task = xs_map_new();
                Value *result = fn->native(NULL, NULL, 0);
                { Value *sv = xs_str("done"); map_set(task->map, "_status", sv); value_decref(sv); }
                map_set(task->map, "_result", result ? result : value_incref(XS_NULL_VAL));
                if (result) value_decref(result);
                value_decref(fn);
                PUSH(task);
                break;
            }
            Value *task = xs_map_new();
            if (0) {
            } else if (VAL_TAG(fn) == XS_MAP && map_get(fn->map, "__actor_name")) {
                /* spawn an actor: create instance with state + methods merged */
                value_decref(task);
                Value *actor_inst = xs_map_new();
                Value *state = map_get(fn->map, "__state");
                if (state && VAL_TAG(state) == XS_MAP) {
                    for (int j = 0; j < state->map->cap; j++)
                        if (state->map->keys[j])
                            map_set(actor_inst->map, state->map->keys[j],
                                    value_incref(state->map->vals[j]));
                }
                Value *methods = map_get(fn->map, "__methods");
                if (methods && VAL_TAG(methods) == XS_MAP) {
                    map_set(actor_inst->map, "__methods", value_incref(methods));
                }
                Value *aname = map_get(fn->map, "__actor_name");
                if (aname) map_set(actor_inst->map, "__type", value_incref(aname));
                value_decref(fn);
                PUSH(actor_inst);
                break;
            } else {
                { Value *sv = xs_str("done"); map_set(task->map, "_status", sv); value_decref(sv); }
                map_set(task->map, "_result", fn);
            }
            value_decref(fn);
            PUSH(task);
            break;
        }

        case OP_NURSERY_BEGIN: {
            if (vm->nursery_depth >= vm->nursery_stack_cap) {
                int newcap = vm->nursery_stack_cap > 0 ? vm->nursery_stack_cap * 2 : 4;
                vm->nursery_stack = realloc(vm->nursery_stack, newcap * sizeof(int *));
                vm->nursery_lens  = realloc(vm->nursery_lens, newcap * sizeof(int));
                vm->nursery_caps  = realloc(vm->nursery_caps, newcap * sizeof(int));
                vm->nursery_ids   = realloc(vm->nursery_ids,  newcap * sizeof(int));
                vm->nursery_prev_ids = realloc(vm->nursery_prev_ids,
                                               newcap * sizeof(int));
                for (int i = vm->nursery_stack_cap; i < newcap; i++) {
                    vm->nursery_stack[i] = NULL;
                    vm->nursery_lens[i] = 0;
                    vm->nursery_caps[i] = 0;
                    vm->nursery_ids[i]  = 0;
                    vm->nursery_prev_ids[i] = 0;
                }
                vm->nursery_stack_cap = newcap;
            }
            vm->nursery_lens[vm->nursery_depth] = 0;
            {
                extern int xs_nursery_alloc_id(void);
                extern int xs_nursery_current_id(void);
                extern void xs_nursery_set_current_id(int);
                int new_id = xs_nursery_alloc_id();
                vm->nursery_prev_ids[vm->nursery_depth] = xs_nursery_current_id();
                vm->nursery_ids[vm->nursery_depth] = new_id;
                xs_nursery_set_current_id(new_id);
            }
            vm->nursery_depth++;
            break;
        }

        case OP_NURSERY_END: {
            if (vm->nursery_depth <= 0) break;
            int d = vm->nursery_depth - 1;
            int *ids = vm->nursery_stack[d];
            int n = vm->nursery_lens[d];
            extern Value *vm_await_task(int task_id, int *errored_out, Value **err_out);
            for (int i = 0; i < n; i++) {
                int err = 0;
                Value *e = NULL;
                Value *r = vm_await_task(ids[i], &err, &e);
                if (r) value_decref(r);
                if (err && e) {
                    /* Cancelled errors are sibling cleanup noise from
                       the cancellation propagation -- the *original*
                       throw is the one we want to surface. Suppress
                       Cancelled if a real error already won, and don't
                       let Cancelled mask a later real error either. */
                    int is_cancel = 0;
                    if (VAL_TAG(e) == XS_MAP) {
                        Value *kind = map_get(e->map, "kind");
                        if (kind && VAL_TAG(kind) == XS_STR &&
                            strcmp(kind->s, "Cancelled") == 0) is_cancel = 1;
                    }
                    if (is_cancel) {
                        value_decref(e);
                    } else if (!g_xs_pending_throw) {
                        g_xs_pending_throw = e;
                    } else {
                        /* If pending is a Cancelled, replace with the
                           real error; else keep first-real-error. */
                        Value *cur = g_xs_pending_throw;
                        int cur_is_cancel = 0;
                        if (VAL_TAG(cur) == XS_MAP) {
                            Value *kind = map_get(cur->map, "kind");
                            if (kind && VAL_TAG(kind) == XS_STR &&
                                strcmp(kind->s, "Cancelled") == 0) cur_is_cancel = 1;
                        }
                        if (cur_is_cancel) {
                            value_decref(cur);
                            g_xs_pending_throw = e;
                        } else {
                            value_decref(e);
                        }
                    }
                } else if (e) value_decref(e);
            }
            vm->nursery_lens[d] = 0;
            {
                extern void xs_nursery_set_current_id(int);
                xs_nursery_set_current_id(vm->nursery_prev_ids[d]);
            }
            vm->nursery_depth--;
            break;
        }

        /* MAKE_CLASS */
        case OP_MAKE_CLASS: {
            int nfields = (int)INSTR_A(instr);
            const char *cls_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *cls = xs_map_new();
            { Value *nv = xs_str(cls_name); map_set(cls->map, "__name", nv); value_decref(nv); }
            { Value *nv = xs_str(cls_name); map_set(cls->map, "__type", nv); value_decref(nv); }
            Value *fields = xs_map_new();
            int ntmp = nfields*2;
            Value *tmp_s[512], **tmp = ntmp <= 512 ? tmp_s : malloc(ntmp * sizeof(Value*));
            for (int i = ntmp-1; i >= 0; i--) tmp[i] = POP();
            for (int i = 0; i < nfields; i++) {
                Value *k = tmp[i*2], *v = tmp[i*2+1];
                if (VAL_TAG(k) == XS_STR) map_set(fields->map, k->s, v);
                value_decref(k); value_decref(v);
            }
            if (tmp != tmp_s) free(tmp);
            map_set(cls->map, "__fields", fields);
            value_decref(fields);
            Value *methods = xs_map_new();
            map_set(cls->map, "__methods", methods);
            value_decref(methods);
            PUSH(cls);
            break;
        }

        case OP_TRAIT_APPLY: {
            /* stack: class, trait  ->  copy trait.__defaults methods onto
               class.__methods for any method the class does not already
               define. */
            Value *trait_val = POP();
            Value *class_val = POP();
            if ((VAL_TAG(class_val) == XS_MAP || VAL_TAG(class_val) == XS_MODULE) &&
                (VAL_TAG(trait_val) == XS_MAP || VAL_TAG(trait_val) == XS_MODULE)) {
                Value *methods = map_get(class_val->map, "__methods");
                Value *defaults = map_get(trait_val->map, "__defaults");
                if (methods && VAL_TAG(methods) == XS_MAP &&
                    defaults && VAL_TAG(defaults) == XS_MAP) {
                    XSMap *dm = defaults->map;
                    for (int j = 0; j < dm->cap; j++) {
                        if (!dm->keys[j]) continue;
                        if (map_get(methods->map, dm->keys[j])) continue;
                        map_set(methods->map, dm->keys[j], dm->vals[j]);
                    }
                }
            }
            value_decref(trait_val);
            value_decref(class_val);
            break;
        }

        case OP_IMPL_METHOD: {
            Value *closure = POP();
            Value *name_val = POP();
            Value *type_val = POP();
            if (VAL_TAG(type_val) == XS_MAP || VAL_TAG(type_val) == XS_MODULE) {
                Value *methods = map_get(type_val->map, "__methods");
                if (methods && VAL_TAG(methods) == XS_MAP && VAL_TAG(name_val) == XS_STR) {
                    map_set(methods->map, name_val->s, closure);
                } else if (VAL_TAG(name_val) == XS_STR) {
                    Value *impl = map_get(type_val->map, "__impl__");
                    if (!impl) {
                        impl = xs_map_new();
                        map_set(type_val->map, "__impl__", impl);
                        value_decref(impl);
                        impl = map_get(type_val->map, "__impl__");
                    }
                    if (impl && VAL_TAG(impl) == XS_MAP)
                        map_set(impl->map, name_val->s, closure);
                }
            }
            value_decref(closure);
            value_decref(name_val);
            value_decref(type_val);
            break;
        }

        case OP_DEFER_PUSH: {
            if (frame->defer_depth < VM_DEFER_MAX) {
                frame->defer_stack[frame->defer_depth++].defer_ip =
                    frame->ip; /* ip already advanced past DEFER_PUSH */
            }
            frame->ip += INSTR_sBx(instr);
            break;
        }
        case OP_DEFER_RUN: {
            if (frame->defer_depth > 0) {
                frame->defer_return_ip = frame->ip;
                frame->defer_depth--;
                frame->ip = frame->defer_stack[frame->defer_depth].defer_ip;
            }
            break;
        }

        /* actor send */
        case OP_SEND: {
            Value *msg = POP();
            Value *actor_val = POP();
            Value *result = value_incref(XS_NULL_VAL);
            if (VAL_TAG(actor_val) == XS_MAP) {
                Value *methods = map_get(actor_val->map, "__methods");
                if (methods && VAL_TAG(methods) == XS_MAP) {
                    Value *handle_fn = map_get(methods->map, "handle");
                    if (handle_fn && VAL_TAG(handle_fn) == XS_CLOSURE) {
                        /* call handle(self, msg) via vm_invoke */
                        Value *args2[2] = { actor_val, msg };
                        value_decref(result);
                        result = vm_invoke(vm, handle_fn, args2, 2);
                        frame = FRAME;
                        if (!result) result = value_incref(XS_NULL_VAL);
                    }
                }
            } else if (VAL_TAG(actor_val) == XS_ACTOR && actor_val->actor) {
                XSActor *act = actor_val->actor;
                if (act->handle_fn) {
                    if (act->methods) {
                        Value *hfn = map_get(act->methods, "handle");
                        if (hfn && VAL_TAG(hfn) == XS_NATIVE) {
                            Value *hargs[1] = { msg };
                            Value *hr = hfn->native(NULL, hargs, 1);
                            value_decref(result);
                            result = hr ? hr : value_incref(XS_NULL_VAL);
                        } else if (hfn && VAL_TAG(hfn) == XS_CLOSURE) {
                            value_decref(result);
                            value_incref(hfn);
                            PUSH(value_incref(msg));
                            if (call_frame_push(vm, hfn, 1) == 0) {
                                value_decref(hfn);
                                frame = FRAME;
                                value_decref(msg);
                                value_decref(actor_val);
                                break;
                            }
                            value_decref(hfn);
                            result = value_incref(XS_NULL_VAL);
                        }
                    }
                }
            }
            value_decref(msg);
            value_decref(actor_val);
            PUSH(result);
            break;
        }

        case OP_FLOOR_DIV: {
            Value *b = POP(), *a = POP();
            double av = VAL_TAG(a)==XS_INT ? (double)VAL_INT(a) : a->f;
            double bv = VAL_TAG(b)==XS_INT ? (double)VAL_INT(b) : b->f;
            Value *r;
            if (bv == 0.0) { r = value_incref(XS_NULL_VAL); }
            else r = xs_int((int64_t)floor(av / bv));
            value_decref(a); value_decref(b); PUSH(r);
            break;
        }
        case OP_SPACESHIP: {
            Value *b = POP(), *a = POP();
            int cmp = value_cmp(a, b);
            Value *r = xs_int(cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
            value_decref(a); value_decref(b); PUSH(r);
            break;
        }

        case OP_MAKE_ENUM: {
            int nvariants = (int)INSTR_A(instr);
            const char *enum_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *enum_map = xs_map_new();
            int ne = nvariants * 2;
            Value *te_s[512], **tmp_e = ne <= 512 ? te_s : malloc(ne * sizeof(Value*));
            for (int i = ne - 1; i >= 0; i--) tmp_e[i] = POP();
            for (int i = 0; i < nvariants; i++) {
                Value *k = tmp_e[i * 2], *v = tmp_e[i * 2 + 1];
                if (VAL_TAG(k) == XS_STR) map_set(enum_map->map, k->s, v);
                value_decref(k); value_decref(v);
            }
            if (tmp_e != te_s) free(tmp_e);
            { Value *nv = xs_str(enum_name); map_set(enum_map->map, "__type", nv); value_decref(nv); }
            { Value *nv = xs_str(enum_name); map_set(enum_map->map, "__name", nv); value_decref(nv); }
            PUSH(enum_map);
            break;
        }

        case OP_MAKE_INST: {
            int nargs = (int)INSTR_A(instr);
            const char *cls_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *ia_s[256], **inst_args = nargs <= 256 ? ia_s : malloc(nargs * sizeof(Value*));
            for (int i = nargs - 1; i >= 0; i--) inst_args[i] = POP();
            Value *cls_val = POP();
            Value *inst = xs_map_new();
            if (VAL_TAG(cls_val) == XS_MAP || VAL_TAG(cls_val) == XS_MODULE) {
                Value *fields = map_get(cls_val->map, "__fields");
                if (fields && VAL_TAG(fields) == XS_MAP) {
                    for (int j = 0; j < fields->map->cap; j++) {
                        if (fields->map->keys[j])
                            map_set(inst->map, fields->map->keys[j],
                                    value_incref(fields->map->vals[j]));
                    }
                }
                Value *methods = map_get(cls_val->map, "__methods");
                if (methods && VAL_TAG(methods) == XS_MAP) {
                    for (int j = 0; j < methods->map->cap; j++) {
                        if (methods->map->keys[j])
                            map_set(inst->map, methods->map->keys[j],
                                    value_incref(methods->map->vals[j]));
                    }
                }
            }
            { Value *nv = xs_str(cls_name); map_set(inst->map, "__type", nv); value_decref(nv); }
            {
                Value *mi_init = map_get(inst->map, "init");
                if (mi_init && VAL_TAG(mi_init) == XS_NATIVE && nargs > 0) {
                    Value *mi_call_args[257];
                    mi_call_args[0] = inst;
                    for (int i = 0; i < nargs; i++)
                        mi_call_args[i + 1] = inst_args[i];
                    Value *ir = mi_init->native(NULL, mi_call_args, nargs + 1);
                    if (ir) value_decref(ir);
                    for (int i = 0; i < nargs; i++) value_decref(inst_args[i]);
                    if (inst_args != ia_s) free(inst_args);
                    value_decref(cls_val);
                    PUSH(inst);
                } else if (mi_init && VAL_TAG(mi_init) == XS_CLOSURE && nargs > 0) {
                    value_decref(cls_val);
                    PUSH(inst);
                    PUSH(value_incref(inst));
                    for (int i = 0; i < nargs; i++) PUSH(inst_args[i]);
                    if (inst_args != ia_s) free(inst_args);
                    value_incref(mi_init);
                    if (call_frame_push(vm, mi_init, nargs + 1) == 0) {
                        value_decref(mi_init);
                        frame = FRAME;
                        vm->init_inst = value_incref(inst);
                        frame->owns_init_inst = 1;
                        break;
                    } else {
                        value_decref(mi_init);
                    }
                } else {
                    for (int i = 0; i < nargs; i++) value_decref(inst_args[i]);
                    if (inst_args != ia_s) free(inst_args);
                    value_decref(cls_val);
                    PUSH(inst);
                }
            }
            break;
        }

        case OP_INHERIT: {
            Value *base = POP();
            Value *child = POP();
            if ((VAL_TAG(base) == XS_MAP || VAL_TAG(base) == XS_MODULE) &&
                (VAL_TAG(child) == XS_MAP || VAL_TAG(child) == XS_MODULE)) {
                Value *base_fields = map_get(base->map, "__fields");
                Value *child_fields = map_get(child->map, "__fields");
                if (base_fields && VAL_TAG(base_fields) == XS_MAP && child_fields && VAL_TAG(child_fields) == XS_MAP) {
                    for (int j = 0; j < base_fields->map->cap; j++) {
                        if (base_fields->map->keys[j] &&
                            !map_get(child_fields->map, base_fields->map->keys[j]))
                            map_set(child_fields->map, base_fields->map->keys[j],
                                    value_incref(base_fields->map->vals[j]));
                    }
                }
                Value *base_methods = map_get(base->map, "__methods");
                Value *child_methods = map_get(child->map, "__methods");
                if (base_methods && VAL_TAG(base_methods) == XS_MAP && child_methods && VAL_TAG(child_methods) == XS_MAP) {
                    for (int j = 0; j < base_methods->map->cap; j++) {
                        if (base_methods->map->keys[j] &&
                            !map_get(child_methods->map, base_methods->map->keys[j]))
                            map_set(child_methods->map, base_methods->map->keys[j],
                                    value_incref(base_methods->map->vals[j]));
                    }
                }
            }
            value_decref(base);
            PUSH(child);
            break;
        }

        case OP_MAKE_MODULE: {
            Value *mod = xs_map_new();
            mod->tag = XS_MODULE;
            const char *mod_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            { Value *nv = xs_str(mod_name); map_set(mod->map, "__name", nv); value_decref(nv); }
            PUSH(mod);
            break;
        }

        case OP_END_MODULE: {
            break;
        }

        case OP_IMPORT: {
            const char *mod_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *mod = map_get(vm->globals, mod_name);
            if (mod) {
                PUSH(value_incref(mod));
            } else {
                PUSH(value_incref(XS_NULL_VAL));
            }
            break;
        }

        case OP_IMPORT_ITEM: {
            const char *item_name = PROTO->chunk.consts[INSTR_A(instr)]->s;
            const char *mod_name  = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *mod = map_get(vm->globals, mod_name);
            if (mod && (VAL_TAG(mod) == XS_MAP || VAL_TAG(mod) == XS_MODULE)) {
                Value *item = map_get(mod->map, item_name);
                PUSH(item ? value_incref(item) : value_incref(XS_NULL_VAL));
            } else {
                PUSH(value_incref(XS_NULL_VAL));
            }
            break;
        }

        case OP_MAKE_ACTOR: {
            int nstate = (int)INSTR_A(instr);
            const char *actor_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *methods_map = POP();
            int ns2 = nstate * 2;
            Value *st_s[512], **state_tmp = ns2 <= 512 ? st_s : malloc(ns2 * sizeof(Value*));
            for (int i = ns2 - 1; i >= 0; i--) state_tmp[i] = POP();
            Value *actor = xs_map_new();
            { Value *nv = xs_str(actor_name); map_set(actor->map, "__actor_name", nv); value_decref(nv); }
            Value *state = xs_map_new();
            for (int i = 0; i < nstate; i++) {
                Value *k = state_tmp[i * 2], *v = state_tmp[i * 2 + 1];
                if (VAL_TAG(k) == XS_STR) map_set(state->map, k->s, v);
                value_decref(k); value_decref(v);
            }
            if (state_tmp != st_s) free(state_tmp);
            map_set(actor->map, "__state", state);
            value_decref(state);
            map_set(actor->map, "__methods", methods_map);
            value_decref(methods_map);
            PUSH(actor);
            break;
        }

        case OP_OPT_CHAIN: {
            const char *field_name = PROTO->chunk.consts[INSTR_Bx(instr)]->s;
            Value *obj = POP();
            if (VAL_TAG(obj) == XS_NULL) {
                value_decref(obj);
                PUSH(value_incref(XS_NULL_VAL));
            } else if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
                Value *val = map_get(obj->map, field_name);
                PUSH(val ? value_incref(val) : value_incref(XS_NULL_VAL));
                value_decref(obj);
            } else {
                value_decref(obj);
                PUSH(value_incref(XS_NULL_VAL));
            }
            break;
        }

        case OP_NULL_COALESCE: {
            Value *b = POP(), *a = POP();
            if (VAL_TAG(a) == XS_NULL) {
                value_decref(a);
                PUSH(b);
            } else {
                value_decref(b);
                PUSH(a);
            }
            break;
        }

        case OP_TRY_OP: {
            Value *val = POP();
            if (VAL_TAG(val) == XS_MAP) {
                Value *tag_val = map_get(val->map, "_tag");
                if (tag_val && VAL_TAG(tag_val) == XS_STR && strcmp(tag_val->s, "Err") == 0) {
                    upvalue_close_all(&vm->open_upvalues, frame->base);
                    while (vm->sp > frame->base) value_decref(POP());
                    value_decref(frame->closure_val);
                    vm->frame_count--;
                    if (vm->frame_count == 0) { value_decref(val); return 0; }
                    frame = FRAME;
                    PUSH(val);
                    break;
                }
                Value *inner = map_get(val->map, "_val");
                if (inner) {
                    PUSH(value_incref(inner));
                    value_decref(val);
                } else {
                    PUSH(val);
                }
            } else {
                PUSH(val);
            }
            break;
        }

        case OP_PIPE: {
            Value *fn = POP();
            Value *arg = POP();
            if (VAL_TAG(fn) == XS_NATIVE) {
                Value *args[1] = { arg };
                Value *result = fn->native(NULL, args, 1);
                value_decref(arg);
                value_decref(fn);
                PUSH(result ? result : value_incref(XS_NULL_VAL));
            } else if (VAL_TAG(fn) == XS_CLOSURE) {
                PUSH(arg);
                value_incref(fn);
                if (call_frame_push(vm, fn, 1)) {
                    value_decref(fn);
                    return 1;
                }
                value_decref(fn);
                frame = FRAME;
            } else {
                fprintf(stderr, "pipe to non-callable\n");
                value_decref(fn);
                value_decref(arg);
                return 1;
            }
            break;
        }

        // --- membership/type
        case OP_IN: {
            Value *right = POP(), *left = POP();
            int found = 0;
            if (VAL_TAG(right) == XS_ARRAY) {
                for (int j = 0; j < right->arr->len; j++)
                    if (value_equal(left, right->arr->items[j])) { found = 1; break; }
            } else if (VAL_TAG(right) == XS_MAP || VAL_TAG(right) == XS_MODULE) {
                if (VAL_TAG(left) == XS_STR) found = map_has(right->map, left->s);
            } else if (VAL_TAG(right) == XS_STR && VAL_TAG(left) == XS_STR) {
                found = strstr(right->s, left->s) != NULL;
            } else if (VAL_TAG(right) == XS_RANGE) {
                if (VAL_TAG(left) == XS_INT) {
                    int64_t v = VAL_INT(left);
                    found = v >= right->range->start &&
                            (right->range->inclusive ? v <= right->range->end : v < right->range->end);
                }
            }
            value_decref(left); value_decref(right);
            PUSH(xs_bool(found));
            break;
        }
        case OP_IS: {
            Value *right = POP(), *left = POP();
            int match = 0;
            if (VAL_TAG(right) == XS_STR) {
                const char *t = right->s;
                if      (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0) match = (VAL_TAG(left) == XS_INT);
                else if (strcmp(t, "float") == 0 || strcmp(t, "f64") == 0) match = (VAL_TAG(left) == XS_FLOAT);
                else if (strcmp(t, "str") == 0 || strcmp(t, "string") == 0) match = (VAL_TAG(left) == XS_STR);
                else if (strcmp(t, "bool") == 0) match = (VAL_TAG(left) == XS_BOOL);
                else if (strcmp(t, "array") == 0) match = (VAL_TAG(left) == XS_ARRAY);
                else if (strcmp(t, "map") == 0) match = (VAL_TAG(left) == XS_MAP);
                else if (strcmp(t, "null") == 0) match = (VAL_TAG(left) == XS_NULL);
                else if (strcmp(t, "fn") == 0 || strcmp(t, "function") == 0) match = (VAL_TAG(left) == XS_FUNC || VAL_TAG(left) == XS_NATIVE || VAL_TAG(left) == XS_CLOSURE);
                else if (strcmp(t, "tuple") == 0) match = (VAL_TAG(left) == XS_TUPLE);
                /* shape predicates used by the match-pattern compiler.
                   Slice patterns ([a, b]) only match arrays; tuple
                   patterns ((a, b)) only match tuples. Without this
                   strictness `match (1,2) { [a,b] => ... }` would
                   spuriously fire on a tuple subject. */
                else if (strcmp(t, "<array-like>") == 0)
                    match = (VAL_TAG(left) == XS_ARRAY);
                else if (strcmp(t, "<tuple-like>") == 0)
                    match = (VAL_TAG(left) == XS_TUPLE);
                else if (strcmp(t, "<map-like>") == 0)
                    match = (VAL_TAG(left) == XS_MAP || VAL_TAG(left) == XS_MODULE ||
                             VAL_TAG(left) == XS_STRUCT_VAL || VAL_TAG(left) == XS_INST);
                else if (VAL_TAG(left) == XS_STRUCT_VAL && left->st) match = (strcmp(left->st->type_name, t) == 0);
                else if (VAL_TAG(left) == XS_ENUM_VAL && left->en) match = (strcmp(left->en->type_name, t) == 0);
                else if (VAL_TAG(left) == XS_INST && left->inst && left->inst->class_) match = (strcmp(left->inst->class_->name, t) == 0);
            }
            value_decref(left); value_decref(right);
            PUSH(xs_bool(match));
            break;
        }

        case OP_MAP_MERGE: {
            Value *src = POP(), *dst = POP();
            if (VAL_TAG(dst) == XS_MAP && VAL_TAG(src) == XS_MAP) {
                int nk = 0;
                char **keys = map_keys(src->map, &nk);
                for (int i = 0; i < nk; i++) {
                    Value *v = map_get(src->map, keys[i]);
                    if (v) { value_incref(v); map_set(dst->map, keys[i], v); value_decref(v); }
                    free(keys[i]);
                }
                free(keys);
            }
            value_decref(src);
            PUSH(dst);
            break;
        }
        case OP_CLOSE_UPVALUES: {
            int slot = (int)INSTR_Bx(instr);
            upvalue_close_all(&vm->open_upvalues, frame->base + slot);
            break;
        }

        default:
            fprintf(stderr, "bad opcode %d\n", (int)op);
            return 1;
        }
        /* Cooperative single-step exit for the JIT: when the JIT-emitted
           machine code wants to drive one opcode per call, it sets
           vm->single_step before invoking and we bail out right here.
           step_yielded distinguishes "paused after one op" from
           "program completed". */
        if (vm->single_step) {
            vm->step_yielded = 1;
            return 0;
        }
    }
}

int vm_step(VM *vm) {
    if (!vm || vm->frame_count == 0) return 1;
    int saved = vm->single_step;
    vm->single_step = 1;
    vm->step_yielded = 0;
    int rc = vm_dispatch(vm, 0);
    vm->single_step = saved;
    if (rc != 0) return -1; /* error / uncaught throw */
    if (vm->step_yielded) return 0; /* one op done, more to do */
    return 1; /* program finished */
}

/* JIT-only fast variant: assumes single_step is already 1 (set once
   by the JIT prologue) so we skip the save/restore. Avoids three
   memory writes per step in the hottest path. */
int vm_step_jit(VM *vm) {
    if (vm->frame_count == 0) return 1;
    vm->step_yielded = 0;
    int rc = vm_dispatch(vm, 0);
    if (rc != 0) return -1;
    if (vm->step_yielded) return 0;
    return 1;
}

/* JIT helper for OP_NEG on a float: takes +1 ownership of v, returns
   a fresh +1 negated float on success, or NULL when v isn't a float so
   the caller knows to fall through to the slow path. The SMI path is
   still inlined in machine code (1 cycle), so this only runs when the
   tag check there missed. */
Value *vm_float_neg(Value *v) {
    if (VAL_TAG(v) != XS_FLOAT) return NULL;
    Value *r = xs_float(-v->f);
    value_decref(v);
    return r;
}

/* JIT helper for OP_INDEX_GET. Returns a fresh +1 result on hit and
   consumes the +1 references on col / idx; returns NULL on miss with
   refs untouched so the caller can run the full slow path. Covers the
   three high-frequency shapes the bytecode handler does inline:
     - array / tuple [int]   (with negative-index wrap)
     - map [str]             (skipping channel-private keys)
     - range [int]
   String indexing, range slices, and map-with-non-str-key all go to
   the slow path; their codepaths allocate or surface errors and the
   wins of inlining them are smaller. */
Value *vm_index_get_fast(Value *col, Value *idx) {
    if ((VAL_TAG(col) == XS_ARRAY || VAL_TAG(col) == XS_TUPLE) &&
        VAL_TAG(idx) == XS_INT) {
        int64_t i = VAL_INT(idx);
        int64_t n = col->arr->len;
        if (i < 0) i += n;
        Value *r = (i >= 0 && i < n)
                 ? value_incref(col->arr->items[i])
                 : value_incref(XS_NULL_VAL);
        value_decref(idx); value_decref(col);
        return r;
    }
    if (VAL_TAG(col) == XS_MAP && VAL_TAG(idx) == XS_STR) {
        /* Channels store a few private keys we hide from index_get;
           punt those to the slow path so the existing logic runs. */
        Value *cid = map_get(col->map, "_chan_id");
        if (cid && VAL_TAG(cid) == XS_INT) return NULL;
        Value *v = map_get(col->map, idx->s);
        Value *r = v ? value_incref(v) : value_incref(XS_NULL_VAL);
        value_decref(idx); value_decref(col);
        return r;
    }
    if (VAL_TAG(col) == XS_RANGE && VAL_TAG(idx) == XS_INT && col->range) {
        int64_t step = col->range->step ? col->range->step : 1;
        Value *r = xs_int(col->range->start + VAL_INT(idx) * step);
        value_decref(idx); value_decref(col);
        return r;
    }
    return NULL;
}

/* JIT helper for OP_MAKE_ARRAY / OP_MAKE_TUPLE. The JIT pushes `n`
   items onto vm->sp before the call; the helper consumes them in
   insertion order, packages them into a fresh array (or tuple),
   adjusts vm->sp by -n, and returns the +1 result for the JIT to
   store into dst. `is_tuple` selects xs_tuple_new vs xs_array_new
   so one helper covers both ops. */
Value *vm_make_array_fast(VM *vm, int n, int is_tuple) {
    Value *r = is_tuple ? xs_tuple_new() : xs_array_new();
    Value **base = vm->sp - n;
    for (int i = 0; i < n; i++) {
        array_push(r->arr, base[i]);
        value_decref(base[i]);
    }
    vm->sp -= n;
    return r;
}

/* JIT helper for OP_MAKE_MAP. JIT pushed `2*npairs` items (k0, v0,
   k1, v1, ...) onto vm->sp; helper consumes them, builds the map,
   pops them off, returns +1 result. Non-string keys are ignored
   here, mirroring the interpreter's check. */
Value *vm_make_map_fast(VM *vm, int npairs) {
    Value *m = xs_map_new();
    int n = npairs * 2;
    Value **base = vm->sp - n;
    for (int i = 0; i < npairs; i++) {
        Value *k = base[i*2], *v = base[i*2 + 1];
        if (VAL_TAG(k) == XS_STR) map_set(m->map, k->s, v);
        value_decref(k); value_decref(v);
    }
    vm->sp -= n;
    return m;
}

/* JIT helper for `s == s` / `s != s`: returns the TRUE / FALSE
   singleton (incref'd) and consumes both string operands. The JIT
   tag-checks both for XS_STR before the call, so we trust both
   `s` pointers are non-NULL. `invert` is 1 for IR_NE, 0 for IR_EQ. */
Value *vm_str_eq_fast(Value *a, Value *b, int invert) {
    int eq = strcmp(a->s, b->s) == 0;
    Value *r = value_incref((eq ^ invert) ? XS_TRUE_VAL : XS_FALSE_VAL);
    value_decref(a); value_decref(b);
    return r;
}

/* JIT helper for the IR_CMP_BR fused string path: skip materialising
   a TRUE/FALSE singleton and report directly whether the branch
   should be taken. `take_when_equal` is 1 if the caller wants to
   branch on equality (kind=EQ + fall-through, or kind=NE + branch-
   if-false), 0 otherwise. The codegen pre-XORs kind/branch_if_false
   into one bit at compile time so this helper sees a single flag. */
int vm_str_eq_branch(Value *a, Value *b, int take_when_equal) {
    int eq = strcmp(a->s, b->s) == 0;
    value_decref(a); value_decref(b);
    return eq == take_when_equal;
}

/* JIT helper for OP_CONCAT: stringifies both operands, concatenates,
   returns a fresh +1 string. Both inputs are decref'd. Mirror of
   the OP_CONCAT case body in vm_dispatch with the operand pop and
   stack push factored out -- the JIT calls this directly so the
   per-instr vm_step_jit setup (limits tick, instr fetch, switch
   dispatch) is skipped on every concat. Hot on json/string-heavy
   workloads where it eats 30-40% of vm_step dispatches. */
Value *vm_concat_fast(Value *a, Value *b) {
    char *as = value_str(a), *bs = value_str(b);
    size_t n = strlen(as) + strlen(bs) + 1;
    char *buf = xs_malloc(n); strcpy(buf, as); strcat(buf, bs);
    free(as); free(bs);
    value_decref(a); value_decref(b);
    Value *r = xs_str(buf);
    free(buf);
    return r;
}

/* JIT helper for OP_ITER_GET: returns the i-th element of an
   iterable. Mirror of the case body in vm_dispatch but with the
   operand pop and stack push factored out. Same shape as
   vm_concat_fast: both operands are owned (caller transfers +1),
   return is a fresh +1.

   Covers all the iter shapes the interpreter handles (array,
   tuple, str, range, map, generator, channel) so the JIT never
   has to fall back to vm_step_jit for ITER_GET. The map-pairs
   case reads INSTR_A so we take it as an explicit arg. */
Value *vm_iter_get_fast(Value *iter, Value *idx, int want_pairs) {
    int64_t i = VAL_TAG(idx) == XS_INT ? VAL_INT(idx)
              : (VAL_TAG(idx) == XS_FLOAT ? (int64_t)idx->f : 0);
    Value *r = NULL;
    if (VAL_TAG(iter) == XS_ARRAY || VAL_TAG(iter) == XS_TUPLE) {
        r = (i >= 0 && i < iter->arr->len)
            ? value_incref(iter->arr->items[i])
            : value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(iter) == XS_STR) {
        const char *s = iter->s;
        int64_t slen = (int64_t)strlen(s);
        if (i >= 0 && i < slen) {
            char buf[2] = { s[i], 0 };
            r = xs_str(buf);
        } else {
            r = value_incref(XS_NULL_VAL);
        }
    } else if (VAL_TAG(iter) == XS_RANGE && iter->range) {
        int64_t start = iter->range->start;
        int64_t step  = iter->range->step ? iter->range->step : 1;
        r = xs_int(start + i * step);
    } else if (VAL_TAG(iter) == XS_MAP && iter->map &&
               map_get(iter->map, "__type") &&
               VAL_TAG(map_get(iter->map, "__type")) == XS_STR &&
               strcmp(map_get(iter->map, "__type")->s, "generator") == 0) {
        /* Generator: index into the materialised yields array. */
        Value *yields = map_get(iter->map, "_yields");
        if (yields && VAL_TAG(yields) == XS_ARRAY &&
            i >= 0 && i < yields->arr->len)
            r = value_incref(yields->arr->items[i]);
        else
            r = value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(iter) == XS_MAP && iter->map &&
               map_get(iter->map, "_chan_id") &&
               VAL_TAG(map_get(iter->map, "_chan_id")) == XS_INT) {
        /* Channel: drain the next buffered value -- ignore idx. */
        extern Value *xs_chan_try_recv(Value *);
        r = xs_chan_try_recv(iter);
    } else if ((VAL_TAG(iter) == XS_MAP || VAL_TAG(iter) == XS_MODULE)
               && iter->map) {
        /* Generic map iteration: i-th inserted key, optionally as
           (key, value) tuple. */
        int64_t ki = 0;
        r = value_incref(XS_NULL_VAL);
        for (int j = 0; j < iter->map->cap; j++) {
            if (!iter->map->keys[j]) continue;
            if (ki == i) {
                if (want_pairs) {
                    value_decref(r);
                    r = xs_tuple_new();
                    Value *ks = xs_str(iter->map->keys[j]);
                    Value *vv = iter->map->vals[j];
                    array_push(r->arr, ks);
                    array_push(r->arr, vv ? value_incref(vv)
                                          : value_incref(XS_NULL_VAL));
                    value_decref(ks);
                } else {
                    value_decref(r);
                    r = xs_str(iter->map->keys[j]);
                }
                break;
            }
            ki++;
        }
    } else {
        r = value_incref(XS_NULL_VAL);
    }
    value_decref(idx);
    value_decref(iter);
    return r;
}

/* JIT helper for OP_LOAD_GLOBAL: resolves a global name via the
   current proto's inline cache. Populates the cache on miss. Returns
   an incref'd value ready to push on the stack. */
Value *vm_load_global_ic(VM *vm, int ip_idx, uint16_t const_idx) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    XSProto *proto = frame->closure_val->cl->proto;
    XSChunk *chk = &proto->chunk;
    if (ip_idx < 0 || ip_idx >= chk->len) {
        /* ip out of range -- shouldn't happen but be safe */
        const char *name = chk->consts[const_idx]->s;
        Value *v = map_get(vm->globals, name);
        return value_incref(v ? v : XS_NULL_VAL);
    }
    if (!chk->ic) {
        chk->ic = xs_calloc((size_t)chk->len, sizeof(Value *));
        if (!chk->ic_version)
            chk->ic_version = xs_calloc((size_t)chk->len, sizeof(uint64_t));
    }
    Value *cached = chk->ic[ip_idx];
    if (cached && chk->ic_version[ip_idx] == vm->global_version) {
        return value_incref(cached);
    }
    const char *name = chk->consts[const_idx]->s;
    Value *v = map_get(vm->globals, name);
    if (!v) v = XS_NULL_VAL;
    if (chk->ic[ip_idx]) value_decref(chk->ic[ip_idx]);
    chk->ic[ip_idx] = value_incref(v);
    chk->ic_version[ip_idx] = vm->global_version;
    return value_incref(v);
}

/* JIT fast path for OP_RETURN on a plain frame (no defer, no
   generator, no init_inst/spawn_task baggage, no top-level main
   transition). Pops result, tears down locals + closure, pushes
   result on caller's stack. Returns 0 on success, 1 to fall back. */
int vm_return_fast(VM *vm) {
    if (vm->frame_count == 0) return 1;
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    if (frame->defer_return_ip) return 1;
    if (frame->is_generator) return 1;
    if (frame->defer_depth > 0) return 1;
    if (vm->frame_count <= 1) return 1;       /* top frame: let slow path */
    if (vm->init_inst) return 1;
    if (vm->spawn_task) return 1;
    if (vm->open_upvalues) return 1;          /* let slow path close them */
    Value *result = *--vm->sp;
    while (vm->sp > frame->base) value_decref(*--vm->sp);
    value_decref(frame->closure_val);
    vm->frame_count--;
    if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
    *vm->sp++ = result;
    return 0;
}

/* JIT fast path for OP_CALL on a plain closure (no overload, no
   native, no map/struct constructor, no variadic, no generator,
   exact-arity). Returns 0 on success (frame pushed, ip advanced past
   the CALL instruction), 1 to tell the JIT "not eligible, run the
   slow path". On error returns 1 too -- vm_dispatch will be the one
   to surface it on the next step. */
int vm_call_closure_fast(VM *vm, int argc) {
    Value *callee = vm->sp[-argc - 1];
    /* Native fast path: most builtins are just a C function. The slow
       OP_CALL arm does exactly this; doing it here saves the
       vm_step_jit dispatch tax (~50ns) on every native call from JIT'd
       code. If the native queues a throw, we return 1 so vm_step_jit's
       vm_dispatch top-of-loop unwinds; the args+result we just touched
       are still on the stack but unwind clears them. */
    if (VAL_TAG(callee) == XS_NATIVE) {
        Value **args = vm->sp - argc;
        Value *result = callee->native(NULL, args, argc);
        for (int i = 0; i < argc; i++) value_decref(*--vm->sp);
        value_decref(*--vm->sp); /* callee */
        if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
        *vm->sp++ = result ? result : value_incref(XS_NULL_VAL);
        if (g_xs_pending_throw) return 1;
        vm->frames[vm->frame_count - 1].ip += 1; /* past OP_CALL */
        return 0;
    }
    if (VAL_TAG(callee) != XS_CLOSURE) return 1;
    XSClosure *cl = callee->cl;
    XSProto *proto = cl->proto;
    if (proto->arity < 0) return 1;          /* signed-encoded variadic */
    if (proto->is_variadic) return 1;
    if (proto->is_generator) return 1;
    if (proto->arity != argc) return 1;       /* requires exact arity */
    if (vm->frame_count >= 2048) return 1;    /* let slow path do StackOverflow */
    if (vm->frame_count >= vm->frames_cap) vm_grow_frames(vm);
    /* Shift the callee out from under the args (matches OP_CALL). */
    Value *saved = callee;
    value_incref(saved);
    for (int i = -argc - 1; i < -1; i++) vm->sp[i] = vm->sp[i + 1];
    vm->sp--;
    value_decref(saved);
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure_val      = value_incref(saved);
    frame->ip               = proto->chunk.code;
    frame->base             = vm->sp - argc;
    frame->try_depth        = 0;
    frame->defer_depth      = 0;
    frame->defer_return_ip  = NULL;
    frame->is_generator     = 0;
    frame->yield_arr        = NULL;
    frame->yield_index      = 0;
    value_decref(saved);
    /* Fill rest of locals with null. */
    for (int i = 0; i < proto->nlocals - argc; i++) {
        if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
        *vm->sp++ = value_incref(XS_NULL_VAL);
    }
    /* Advance the CALLER's ip past the OP_CALL: when this fast path
       returns to the JIT, the JIT's loop_top sees the new top frame
       and runs from its first instruction. The caller frame's ip is
       restored when OP_RETURN pops back. We do NOT touch caller ip
       here -- vm_dispatch's OP_CALL doesn't either; the *frame->ip++
       at the top of the dispatch loop already moved past it before
       the case ran. We mirror that: bump the caller's ip by 4. */
    if (vm->frame_count >= 2) {
        vm->frames[vm->frame_count - 2].ip += 1;
    }
    /* Intentionally NOT dispatching proto->jit_entry here. Doing so
     * caused tier-1 to route into partially-tested tier-2 code paths
     * for inner protos (the match-compiler / deep-pattern tests pull
     * in IR shapes the tier-2 codegen doesn't yet handle cleanly).
     * The tier-2 inner-frame dispatcher (tier2_run_until) still picks
     * up proto->jit_entry when the enclosing caller is itself a
     * tier-2 frame, so recursive self-calls inside a compiled proto
     * still land on native code. */
    return 0;
}

/* JIT fast path for OP_METHOD_CALL. Two routes:

   1. Array / string workhorse methods (.push, .len, .size, .pop) get
      a tag-checked inline implementation. These dominate in real code
      (every push or len in a tight loop) and aren't covered by the IC
      because the interpreter dispatches them through hardcoded type
      switches rather than caching a resolved Value*.

   2. Map / module receivers with a previously-resolved XS_NATIVE go
      through the inline cache, mirroring the interp's hot path.

   Everything else returns 1 so vm_step_jit's full OP_METHOD_CALL
   handles it (closures, signals, channels, generic map methods,
   tuples writing to themselves, etc.). */
int vm_method_call_fast(VM *vm, int argc) {
    if (vm->frame_count == 0) return 1;
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    Instruction instr = *frame->ip;
    if (INSTR_OPCODE(instr) != OP_METHOD_CALL) return 1;
    XSProto *proto = frame->closure_val->cl->proto;
    Value *name_const = proto->chunk.consts[INSTR_Bx(instr)];
    if (!name_const || VAL_TAG(name_const) != XS_STR) return 1;
    const char *name = name_const->s;
    Value *obj = vm->sp[-argc - 1];
    Value **args = vm->sp - argc;

    /* ---- Array .push / .len / .size / .pop / .concat ---- */
    if (VAL_TAG(obj) == XS_ARRAY) {
        Value *r = NULL;
        if ((name[0] == 'p' && name[1] == 'u' &&
             strcmp(name, "push") == 0) ||
            (name[0] == 'a' && strcmp(name, "append") == 0)) {
            for (int j = 0; j < argc; j++)
                array_push(obj->arr, value_incref(args[j]));
            r = value_incref(XS_NULL_VAL);
        } else if (argc == 0 && name[0] == 'l' && strcmp(name, "len") == 0) {
            r = xs_int(obj->arr->len);
        } else if (argc == 0 && name[0] == 's' && strcmp(name, "size") == 0) {
            r = xs_int(obj->arr->len);
        } else if (argc == 0 && name[0] == 'p' && name[1] == 'o' &&
                   strcmp(name, "pop") == 0) {
            if (obj->arr->len > 0) {
                r = value_incref(obj->arr->items[obj->arr->len - 1]);
                value_decref(obj->arr->items[--obj->arr->len]);
            } else {
                r = value_incref(XS_NULL_VAL);
            }
        } else if (name[0] == 'c' && strcmp(name, "concat") == 0 &&
                   argc >= 1) {
            /* Build a new array combining receiver + each array arg.
               Non-array args get dropped, mirroring the slow-path. */
            Value *out = xs_array_new();
            for (int j = 0; j < obj->arr->len; j++)
                array_push(out->arr, value_incref(obj->arr->items[j]));
            for (int a2 = 0; a2 < argc; a2++) {
                /* match the slow path: only XS_ARRAY contributes;
                   tuple args go through the slow path's null-fallback. */
                if (VAL_TAG(args[a2]) == XS_ARRAY) {
                    for (int j = 0; j < args[a2]->arr->len; j++)
                        array_push(out->arr,
                                   value_incref(args[a2]->arr->items[j]));
                }
            }
            r = out;
        }
        if (!r) return 1;
        for (int j = 0; j < argc; j++) value_decref(*--vm->sp);
        value_decref(*--vm->sp); /* receiver */
        if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
        *vm->sp++ = r;
        if (g_xs_pending_throw) return 1;
        frame->ip += 1;
        return 0;
    }

    /* ---- String workhorse methods (no-arg transforms) ---- */
    if (VAL_TAG(obj) == XS_STR && argc == 0) {
        const char *s = obj->s;
        int slen = (int)strlen(s);
        Value *r = NULL;
        if (strcmp(name, "len") == 0 || strcmp(name, "size") == 0 ||
            strcmp(name, "length") == 0) {
            r = xs_int((int64_t)utf8_strlen(s, slen));
        } else if (strcmp(name, "upper") == 0 ||
                   strcmp(name, "to_upper") == 0) {
            int olen = 0;
            char *u = utf8_str_upper(s, slen, &olen);
            r = xs_str(u);
            free(u);
        } else if (strcmp(name, "lower") == 0 ||
                   strcmp(name, "to_lower") == 0) {
            int olen = 0;
            char *u = utf8_str_lower(s, slen, &olen);
            r = xs_str(u);
            free(u);
        } else if (strcmp(name, "trim") == 0) {
            int b = 0, e = slen;
            while (b < e && (s[b] == ' ' || s[b] == '\t' ||
                             s[b] == '\n' || s[b] == '\r')) b++;
            while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' ||
                             s[e-1] == '\n' || s[e-1] == '\r')) e--;
            char *buf = xs_malloc((size_t)(e - b) + 1);
            memcpy(buf, s + b, (size_t)(e - b));
            buf[e - b] = 0;
            r = xs_str(buf);
            free(buf);
        } else if (strcmp(name, "byte_len") == 0) {
            r = xs_int(slen);
        }
        if (r) {
            value_decref(*--vm->sp); /* receiver */
            if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
            *vm->sp++ = r;
            frame->ip += 1;
            return 0;
        }
    }

    /* ---- Map / module IC route ---- */
    if (VAL_TAG(obj) != XS_MAP && VAL_TAG(obj) != XS_MODULE) return 1;
    int site_id = ic_site_id(
        (int)((uintptr_t)proto ^
              (uintptr_t)(frame->ip - proto->chunk.code)));
    int64_t type_tag = (int64_t)(intptr_t)obj->map;
    uint8_t is_module = 0, needs_self = 0;
    Value *fn = ic_lookup_ex(site_id, type_tag, name,
                             &is_module, &needs_self);
    if (!fn || VAL_TAG(fn) != XS_NATIVE) return 1;
    Value *r;
    if (is_module) {
        r = fn->native(NULL, args, argc);
    } else {
        Value *nargs[17];
        nargs[0] = obj;
        int total = 1 + argc;
        if (total > 17) total = 17;
        for (int j = 0; j < argc && j < 16; j++) nargs[j + 1] = args[j];
        r = fn->native(NULL, nargs, total);
    }
    for (int j = 0; j < argc; j++) value_decref(*--vm->sp);
    value_decref(*--vm->sp); /* receiver */
    if (vm->sp - vm->stack >= vm->stack_cap) vm_grow_stack(vm);
    *vm->sp++ = r ? r : value_incref(XS_NULL_VAL);
    if (g_xs_pending_throw) return 1;
    frame->ip += 1;
    return 0;
}

int vm_run_with(VM *vm, XSProto *proto, int (*entry)(VM *)) {
    if (!vm || !proto || !entry) return 1;
    vm_opcounts_init();
    g_vm_for_invoke = vm;
    g_plugin_vm = vm;
    XSClosure *top_cl = xs_malloc(sizeof *top_cl);
    top_cl->proto    = proto; proto->refcount++;
    top_cl->upvalues = NULL;
    top_cl->refcount = 1;
    Value *top_val = xs_malloc(sizeof *top_val);
    top_val->tag = XS_CLOSURE;
    top_val->refcount = 1;
    top_val->cl       = top_cl;

    if (vm->frame_count >= vm->frames_cap) vm_grow_frames(vm);
    CallFrame *frame   = &vm->frames[vm->frame_count++];
    frame->closure_val = top_val;
    frame->ip          = proto->chunk.code;
    frame->base        = vm->sp;
    frame->try_depth   = 0;
    for (int i = 0; i < proto->nlocals; i++) PUSH(xs_null());

    return entry(vm);
}
