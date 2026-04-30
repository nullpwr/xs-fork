#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/error.h"
#include "core/xs_bigint.h"
#include "core/utf8.h"
#include "tls/xs_tls.h"
#include "core/gc.h"
#include "core/msgpack.h"
#include "runtime/async.h"
#include "runtime/concurrent.h"
#if !defined(__wasi__) && !defined(XS_NO_BEARSSL)
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_kdf.h"
#include "bearssl_block.h"
#include "bearssl_aead.h"
#endif
#include <strings.h>
/* custom NFA engine uses 'nsub', POSIX uses 're_nsub' */
#ifndef re_nsub
#define re_nsub nsub
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

int    g_xs_argc = 0;
char **g_xs_argv = NULL;

void xs_set_argv(int argc, char **argv) {
    g_xs_argc = argc;
    g_xs_argv = argv;
}
#if !defined(__MINGW32__) && !defined(__wasi__)
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <sys/stat.h>
#  include <dirent.h>
#  include <glob.h>
#  include <fcntl.h>
#  if defined(__linux__)
#    include <sys/inotify.h>
#  endif
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__) || defined(__DragonFly__)
#    include <sys/event.h>
#  endif
#  include <sys/wait.h>
#  include <signal.h>
#  include <netinet/tcp.h>
#elif defined(_WIN32) && !defined(__wasi__)
#  include <windows.h>
#elif !defined(__MINGW32__)
#  include <sys/stat.h>
#  include <errno.h>
#endif
/* always use the custom NFA regex engine for consistent cross-platform behavior */
#include "core/xs_regex.h"
#include <errno.h>

#ifndef M_PI
#define M_PI   3.14159265358979323846
#endif
#ifndef M_E
#define M_E    2.71828182845904523536
#endif
#ifndef M_TAU
#define M_TAU  6.28318530717958647692
#endif

static char *inst_to_str(Interp *interp, Value *v, int repr_mode) {
    if (VAL_TAG(v) == XS_INST && v->inst) {
        const char *mname = repr_mode ? "__repr__" : "__str__";
        Value *fn = map_get(v->inst->methods, mname);
        if (!fn && v->inst->class_ && v->inst->class_->methods)
            fn = map_get(v->inst->class_->methods, mname);
        if (!fn && !repr_mode) {
            fn = map_get(v->inst->methods, "to_string");
            if (!fn && v->inst->class_ && v->inst->class_->methods)
                fn = map_get(v->inst->class_->methods, "to_string");
        }
        if (fn && (VAL_TAG(fn) == XS_FUNC || VAL_TAG(fn) == XS_NATIVE)) {
            int has_self = 0;
            if (VAL_TAG(fn) == XS_FUNC && fn->fn->nparams > 0) {
                Node *p0 = fn->fn->params[0];
                if (VAL_TAG(p0) == NODE_PAT_IDENT && strcmp(p0->pat_ident.name, "self") == 0)
                    has_self = 1;
            }
            if (VAL_TAG(fn) == XS_NATIVE) has_self = 1;
            Value *result;
            if (has_self) {
                Value *call_args[1] = { v };
                result = call_value(interp, fn, call_args, 1, mname);
            } else {
                result = call_value(interp, fn, NULL, 0, mname);
            }
            if (result && VAL_TAG(result) == XS_STR) {
                char *s = xs_strdup(result->s);
                value_decref(result);
                return s;
            }
            if (result) {
                char *s = value_str(result);
                value_decref(result);
                return s;
            }
        }
    }
    return repr_mode ? value_repr(v) : value_str(v);
}

static Value *builtin_print(Interp *i, Value **args, int argc) {
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR && args[0]->s) {
        const char *fmt = args[0]->s;
        int has_placeholder = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p == '{' && *(p+1) == '}') { has_placeholder = 1; break; }
        }
        if (has_placeholder) {
            int argidx = 1;
            for (const char *p = fmt; *p; ) {
                if (*p == '{' && *(p+1) == '}') {
                    if (argidx < argc) {
                        char *s = inst_to_str(i, args[argidx++], 0);
                        printf("%s", s); free(s);
                    }
                    p += 2;
                } else {
                    putchar(*p++);
                }
            }
            for (int j = argidx; j < argc; j++) {
                printf(" ");
                char *s = inst_to_str(i, args[j], 0); printf("%s", s); free(s);
            }
            printf("\n");
            return value_incref(XS_NULL_VAL);
        }
    }
    for (int j = 0; j < argc; j++) {
        if (j) printf(" ");
        char *s = inst_to_str(i, args[j], 0);
        printf("%s", s);
        free(s);
    }
    printf("\n");
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_print_no_nl(Interp *i, Value **args, int argc) {
    (void)i;
    for (int j = 0; j < argc; j++) {
        if (j) printf(" ");
        char *s = value_str(args[j]);
        printf("%s", s);
        free(s);
    }
    fflush(stdout);
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_eprint(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR && args[0]->s) {
        const char *fmt = args[0]->s;
        int has_placeholder = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p == '{' && *(p+1) == '}') { has_placeholder = 1; break; }
        }
        if (has_placeholder) {
            int argidx = 1;
            for (const char *p = fmt; *p; ) {
                if (*p == '{' && *(p+1) == '}') {
                    if (argidx < argc) {
                        char *s = value_str(args[argidx++]);
                        fprintf(stderr,"%s",s); free(s);
                    }
                    p += 2;
                } else { fputc(*p++, stderr); }
            }
            for (int j = argidx; j < argc; j++) {
                fprintf(stderr," ");
                char *s = value_str(args[j]); fprintf(stderr,"%s",s); free(s);
            }
            fprintf(stderr,"\n");
            return value_incref(XS_NULL_VAL);
        }
    }
    for (int j = 0; j < argc; j++) {
        if (j) fprintf(stderr," ");
        char *s = value_str(args[j]);
        fprintf(stderr,"%s",s); free(s);
    }
    fprintf(stderr,"\n");
    return value_incref(XS_NULL_VAL);
}

/* type predicates */
static Value *builtin_type(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return xs_str("null");
    switch (VAL_TAG(args[0])) {
    case XS_NULL:   return xs_str("null");
    case XS_BOOL:   return xs_str("bool");
    case XS_INT:    return xs_str("int");
    case XS_BIGINT: return xs_str("int");
    case XS_FLOAT:  return xs_str("float");
    case XS_STR:    return xs_str("str");
    case XS_CHAR:   return xs_str("char");
    case XS_ARRAY:  return xs_str("array");
    case XS_TUPLE:  return xs_str("tuple");
    case XS_MAP:    return xs_str("map");
    case XS_FUNC:   return xs_str("fn");
    case XS_NATIVE: return xs_str("fn");
    case XS_CLOSURE: return xs_str("fn");
    case XS_STRUCT_VAL: return xs_str(args[0]->st->type_name ? args[0]->st->type_name : "struct");
    case XS_ENUM_VAL:   return xs_str(args[0]->en->type_name ? args[0]->en->type_name : "enum");
    case XS_CLASS_VAL:  return xs_str(args[0]->cls->name ? args[0]->cls->name : "class");
    case XS_INST:       return xs_str(args[0]->inst->class_->name ? args[0]->inst->class_->name : "object");
    case XS_RANGE:  return xs_str("range");
    case XS_SIGNAL: return xs_str("signal");
    case XS_ACTOR:  return xs_str("actor");
    case XS_REGEX:  return xs_str("re");
    case XS_MODULE: return xs_str("module");
    default:        return xs_str("unknown");
    }
}

static Value *builtin_typeof(Interp *i, Value **args, int argc) {
    return builtin_type(i, args, argc);
}

/* type_of returns Python-compat capitalized type names */
static Value *builtin_type_of(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return xs_str("Null");
    switch (VAL_TAG(args[0])) {
    case XS_NULL:   return xs_str("Null");
    case XS_BOOL:   return xs_str("Bool");
    case XS_INT:    return xs_str("Int");
    case XS_BIGINT: return xs_str("Int");
    case XS_FLOAT:  return xs_str("Float");
    case XS_STR:    return xs_str("Str");
    case XS_CHAR:   return xs_str("Char");
    case XS_ARRAY:  return xs_str("Array");
    case XS_TUPLE:  return xs_str("Tuple");
    case XS_MAP:    return xs_str("Map");
    case XS_MODULE: return xs_str("Map");
    case XS_FUNC:   return xs_str("Fn");
    case XS_NATIVE: return xs_str("Fn");
    case XS_STRUCT_VAL: return xs_str(args[0]->st->type_name ? args[0]->st->type_name : "Struct");
    case XS_ENUM_VAL:   return xs_str(args[0]->en->type_name ? args[0]->en->type_name : "Enum");
    case XS_CLASS_VAL:  return xs_str(args[0]->cls->name ? args[0]->cls->name : "Class");
    case XS_INST:       return xs_str(args[0]->inst->class_->name ? args[0]->inst->class_->name : "Object");
    case XS_RANGE:  return xs_str("Range");
    case XS_SIGNAL: return xs_str("Signal");
    case XS_ACTOR:  return xs_str("Actor");
    case XS_REGEX:  return xs_str("Re");
    default:        return xs_str("Unknown");
    }
}

static inline Value *tag_check(Value **args, int argc, ValueTag tag) {
    return xs_bool(argc > 0 && VAL_TAG(args[0]) == tag);
}

static inline Value *tag_check2(Value **args, int argc, ValueTag t1, ValueTag t2) {
    return xs_bool(argc > 0 && (VAL_TAG(args[0]) == t1 || VAL_TAG(args[0]) == t2));
}

static Value *builtin_is_null(Interp *i, Value **a, int n)  { (void)i; return n < 1 ? xs_bool(1) : tag_check(a,n,XS_NULL); }
static Value *builtin_is_int(Interp *i, Value **a, int n)   { (void)i; return xs_bool(n >= 1 && (VAL_TAG(a[0]) == XS_INT || VAL_TAG(a[0]) == XS_BIGINT)); }
static Value *builtin_is_float(Interp *i, Value **a, int n) { (void)i; return tag_check(a, n, XS_FLOAT); }
static Value *builtin_is_str(Interp *i, Value **a, int n)   { (void)i; return tag_check(a, n, XS_STR); }
static Value *builtin_is_bool(Interp *i, Value **a, int n)  { (void)i; return tag_check(a, n, XS_BOOL); }
static Value *builtin_is_array(Interp *i, Value **a, int n) { (void)i; return tag_check2(a, n, XS_ARRAY, XS_TUPLE); }
static Value *builtin_is_fn(Interp *i, Value **a, int n)    { (void)i; return tag_check2(a, n, XS_FUNC, XS_NATIVE); }

/* conversions */
static Value *builtin_int(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_int(0);
    Value *v=args[0];
    if (VAL_TAG(v)==XS_INT) return value_incref(v);
    if (VAL_TAG(v)==XS_BIGINT) {
        if (bigint_fits_i64(v->bigint)) return xs_int(bigint_to_i64(v->bigint));
        return value_incref(v);
    }
    if (VAL_TAG(v)==XS_BOOL) return xs_int(VAL_INT(v) ? 1 : 0);
    if (VAL_TAG(v)==XS_FLOAT) {
        if (isnan(v->f) || isinf(v->f)) {
            xs_runtime_error(span_zero(), "TypeError", NULL,
                             "int(): can't convert non-finite float");
            return value_incref(XS_NULL_VAL);
        }
        return xs_int((int64_t)v->f);
    }
    if (VAL_TAG(v)==XS_STR) {
        const char *s=v->s;
        while (*s==' '||*s=='\t') s++;
        if (!*s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): empty string");
            return value_incref(XS_NULL_VAL);
        }
        char *end=NULL; errno=0;
        long long iv=strtoll(s, &end, 0);
        if (end==s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): invalid literal: '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        while (end && (*end==' '||*end=='\t')) end++;
        if (end && *end) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "int(): trailing characters in '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        if (errno==ERANGE) {
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

static Value *builtin_float(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_float(0.0);
    Value *v=args[0];
    if (VAL_TAG(v)==XS_FLOAT) return value_incref(v);
    if (VAL_TAG(v)==XS_INT) return xs_float((double)VAL_INT(v));
    if (VAL_TAG(v)==XS_BIGINT) return xs_float(bigint_to_double(v->bigint));
    if (VAL_TAG(v)==XS_BOOL) return xs_float(VAL_INT(v) ? 1.0 : 0.0);
    if (VAL_TAG(v)==XS_STR) {
        const char *s=v->s;
        while (*s==' '||*s=='\t') s++;
        if (!*s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "float(): empty string");
            return value_incref(XS_NULL_VAL);
        }
        char *end=NULL;
        double d=strtod(s, &end);
        if (end==s) {
            xs_runtime_error(span_zero(), "ValueError", NULL,
                             "float(): invalid literal: '%s'", v->s);
            return value_incref(XS_NULL_VAL);
        }
        while (end && (*end==' '||*end=='\t')) end++;
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

static Value *builtin_str(Interp *i, Value **args, int argc) {
    if (argc<1) return xs_str("");
    char *s = inst_to_str(i, args[0], 0);
    Value *v = xs_str(s); free(s); return v;
}

static Value *builtin_bool(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return value_incref(XS_FALSE_VAL);
    return value_truthy(args[0])?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}

static Value *builtin_char(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_char(0);
    if (VAL_TAG(args[0])==XS_INT) return xs_char((char)VAL_INT(args[0]));
    if (VAL_TAG(args[0])==XS_STR&&args[0]->s[0]) return xs_char(args[0]->s[0]);
    return xs_char(0);
}

static Value *builtin_repr(Interp *i, Value **args, int argc) {
    if (argc<1) return xs_str("null");
    char *s = inst_to_str(i, args[0], 1);
    Value *v = xs_str(s); free(s); return v;
}

Value *builtin_abs(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_int(0);
    if (VAL_TAG(args[0])==XS_INT) return xs_int(VAL_INT(args[0])<0?-VAL_INT(args[0]):VAL_INT(args[0]));
    if (VAL_TAG(args[0])==XS_FLOAT) return xs_float(fabs(args[0]->f));
    return value_incref(args[0]);
}

static Value *builtin_min(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc==0) return value_incref(XS_NULL_VAL);
    if (argc==1 && VAL_TAG(args[0])==XS_ARRAY) {
        XSArray *arr=args[0]->arr;
        if (arr->len==0) return value_incref(XS_NULL_VAL);
        Value *m=arr->items[0];
        for (int j=1;j<arr->len;j++) if(value_cmp(arr->items[j],m)<0) m=arr->items[j];
        return value_incref(m);
    }
    Value *m=args[0];
    for (int j=1;j<argc;j++) if(value_cmp(args[j],m)<0) m=args[j];
    return value_incref(m);
}

static Value *builtin_max(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc==0) return value_incref(XS_NULL_VAL);
    if (argc==1 && VAL_TAG(args[0])==XS_ARRAY) {
        XSArray *arr=args[0]->arr;
        if (arr->len==0) return value_incref(XS_NULL_VAL);
        Value *m=arr->items[0];
        for (int j=1;j<arr->len;j++) if(value_cmp(arr->items[j],m)>0) m=arr->items[j];
        return value_incref(m);
    }
    Value *m=args[0];
    for (int j=1;j<argc;j++) if(value_cmp(args[j],m)>0) m=args[j];
    return value_incref(m);
}

Value *builtin_pow(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<2) return xs_float(0.0);
    double base=VAL_TAG(args[0])==XS_FLOAT?args[0]->f:(double)VAL_INT(args[0]);
    double exp2=VAL_TAG(args[1])==XS_FLOAT?args[1]->f:(double)VAL_INT(args[1]);
    return xs_float(pow(base,exp2));
}

/* one-arg math wrappers: shared with the Math.* module (builtins_math.c) */
#define MATH1(name, fn) \
    Value *builtin_##name(Interp *i, Value **args, int argc) { \
        (void)i; \
        if (argc < 1) return xs_float(0.0); \
        double v = VAL_TAG(args[0]) == XS_FLOAT ? args[0]->f : (double)VAL_INT(args[0]); \
        return xs_float(fn(v)); \
    }
MATH1(sqrt,  sqrt)
MATH1(floor, floor)
MATH1(ceil,  ceil)
MATH1(round, round)

Value *builtin_log(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_float(0.0);
    double v=VAL_TAG(args[0])==XS_FLOAT?args[0]->f:(double)VAL_INT(args[0]);
    if (argc>1) {
        double base=VAL_TAG(args[1])==XS_FLOAT?args[1]->f:(double)VAL_INT(args[1]);
        return xs_float(log(v)/log(base));
    }
    return xs_float(log(v));
}

MATH1(sin, sin)
MATH1(cos, cos)

MATH1(tan, tan)
#undef MATH1

/* collections */
static Value *builtin_len(Interp *i, Value **args, int argc) {
    if (argc<1) return xs_int(0);
    Value *v=args[0];
    if (VAL_TAG(v)==XS_ARRAY||VAL_TAG(v)==XS_TUPLE) return xs_int(v->arr->len);
    if (VAL_TAG(v)==XS_STR) {
        /* Codepoint count, not byte count: matches `chars()` and the
           VM's len(). For raw byte length use `str.bytes().len`. */
        int blen = (int)strlen(v->s);
        return xs_int((int64_t)utf8_strlen(v->s, blen));
    }
    if (VAL_TAG(v)==XS_MAP||VAL_TAG(v)==XS_MODULE) return xs_int(v->map->len);
    if (VAL_TAG(v)==XS_RANGE) {
        int64_t span = v->range->end - v->range->start;
        if (v->range->inclusive) span += (span >= 0) ? 1 : -1;
        int64_t step = v->range->step ? v->range->step : 1;
        int64_t n2;
        if (step > 0) n2 = (span > 0) ? (span + step - 1) / step : 0;
        else           n2 = (span < 0) ? (-span + (-step) - 1) / (-step) : 0;
        return xs_int(n2);
    }
    /* __len__ dunder method on instances */
    if (VAL_TAG(v)==XS_INST && v->inst) {
        Value *fn = map_get(v->inst->methods, "__len__");
        if (!fn && v->inst->class_ && v->inst->class_->methods)
            fn = map_get(v->inst->class_->methods, "__len__");
        if (fn && (VAL_TAG(fn) == XS_FUNC || VAL_TAG(fn) == XS_NATIVE)) {
            int has_self = 0;
            if (VAL_TAG(fn) == XS_FUNC && fn->fn->nparams > 0) {
                Node *p0 = fn->fn->params[0];
                if (VAL_TAG(p0) == NODE_PAT_IDENT && strcmp(p0->pat_ident.name, "self") == 0)
                    has_self = 1;
            }
            Value *result;
            if (has_self) {
                Value *call_args[1] = { v };
                result = call_value(i, fn, call_args, 1, "__len__");
            } else {
                result = call_value(i, fn, NULL, 0, "__len__");
            }
            return result;
        }
    }
    return xs_int(0);
}

static Value *builtin_range(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc==1) {
        int64_t end2 = (VAL_TAG(args[0])==XS_INT)?VAL_INT(args[0]):(int64_t)args[0]->f;
        return xs_range(0, end2, 0);
    }
    if (argc>=2) {
        int64_t start2=(VAL_TAG(args[0])==XS_INT)?VAL_INT(args[0]):(int64_t)args[0]->f;
        int64_t end2=(VAL_TAG(args[1])==XS_INT)?VAL_INT(args[1]):(int64_t)args[1]->f;
        int64_t step = 1;
        if (argc >= 3) {
            if (VAL_TAG(args[2])==XS_INT) step = VAL_INT(args[2]);
            else if (VAL_TAG(args[2])==XS_FLOAT) step = (int64_t)args[2]->f;
        }
        if (step == 0) {
            fprintf(stderr, "range: step cannot be zero\n");
            return xs_range(0, 0, 0);
        }
        return xs_range_step(start2, end2, 0, step);
    }
    return xs_range(0, 0, 0);
}

static Value *builtin_array(Interp *i, Value **args, int argc) {
    (void)i;
    Value *arr = xs_array_new();
    for (int j=0;j<argc;j++) array_push(arr->arr, value_incref(args[j]));
    return arr;
}

static Value *builtin_map(Interp *i, Value **args, int argc) {
    /* map() -> empty map, map(arr, fn) -> mapped array */
    if (argc == 0) return xs_map_new();
    if (argc >= 2 && (VAL_TAG(args[0]) == XS_ARRAY || VAL_TAG(args[0]) == XS_TUPLE) &&
        (VAL_TAG(args[1]) == XS_FUNC || VAL_TAG(args[1]) == XS_NATIVE || VAL_TAG(args[1]) == XS_CLOSURE)) {
        Value *arr = args[0], *fn = args[1];
        Value *result = xs_array_new();
        for (int j = 0; j < arr->arr->len; j++) {
            Value *elem = arr->arr->items[j];
            Value *call_args[] = { elem };
            Value *mapped = call_value(i, fn, call_args, 1, "map");
            array_push(result->arr, mapped);
        }
        return result;
    }
    return xs_map_new();
}

static Value *builtin_filter(Interp *i, Value **args, int argc) {
    if (argc < 2) return xs_array_new();
    Value *arr = args[0], *fn = args[1];
    if (VAL_TAG(arr) != XS_ARRAY && VAL_TAG(arr) != XS_TUPLE) return xs_array_new();
    Value *result = xs_array_new();
    for (int j = 0; j < arr->arr->len; j++) {
        Value *elem = arr->arr->items[j];
        Value *call_args[] = { elem };
        Value *keep = call_value(i, fn, call_args, 1, "filter");
        if (value_truthy(keep)) array_push(result->arr, value_incref(elem));
        value_decref(keep);
    }
    return result;
}

static Value *builtin_reduce(Interp *i, Value **args, int argc) {
    if (argc < 2) return value_incref(XS_NULL_VAL);
    Value *arr = args[0], *fn = args[1];
    if (VAL_TAG(arr) != XS_ARRAY && VAL_TAG(arr) != XS_TUPLE) return value_incref(XS_NULL_VAL);
    Value *acc = (argc >= 3) ? value_incref(args[2]) : (arr->arr->len > 0 ? value_incref(arr->arr->items[0]) : value_incref(XS_NULL_VAL));
    int start = (argc >= 3) ? 0 : 1;
    for (int j = start; j < arr->arr->len; j++) {
        Value *call_args[] = { acc, arr->arr->items[j] };
        Value *next = call_value(i, fn, call_args, 2, "reduce");
        value_decref(acc);
        acc = next;
    }
    return acc;
}

static Value *builtin_keys(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_array_new();
    Value *obj=args[0];
    if (VAL_TAG(obj)==XS_MAP||VAL_TAG(obj)==XS_MODULE) {
        int nk=0; char **ks=map_keys(obj->map,&nk);
        Value *arr=xs_array_new();
        for (int j=0;j<nk;j++){array_push(arr->arr,xs_str(ks[j]));free(ks[j]);}
        free(ks); return arr;
    }
    return xs_array_new();
}

static Value *builtin_values(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_array_new();
    Value *obj=args[0];
    if (VAL_TAG(obj)==XS_MAP||VAL_TAG(obj)==XS_MODULE) {
        int nk=0; char **ks=map_keys(obj->map,&nk);
        Value *arr=xs_array_new();
        for (int j=0;j<nk;j++){
            Value *v=map_get(obj->map,ks[j]);
            array_push(arr->arr,v?value_incref(v):value_incref(XS_NULL_VAL));
            free(ks[j]);
        }
        free(ks); return arr;
    }
    return xs_array_new();
}

static Value *builtin_entries(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_array_new();
    Value *obj=args[0];
    if (VAL_TAG(obj)==XS_MAP||VAL_TAG(obj)==XS_MODULE) {
        int nk=0; char **ks=map_keys(obj->map,&nk);
        Value *arr=xs_array_new();
        for (int j=0;j<nk;j++){
            Value *tup=xs_tuple_new();
            Value *v=map_get(obj->map,ks[j]);
            array_push(tup->arr,xs_str(ks[j]));
            array_push(tup->arr,v?value_incref(v):value_incref(XS_NULL_VAL));
            array_push(arr->arr,tup);
            free(ks[j]);
        }
        free(ks); return arr;
    }
    return xs_array_new();
}

static Value *builtin_flatten(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_ARRAY) return xs_array_new();
    Value *res=xs_array_new();
    XSArray *arr=args[0]->arr;
    for (int j=0;j<arr->len;j++) {
        if (VAL_TAG(arr->items[j])==XS_ARRAY) {
            XSArray *inner=arr->items[j]->arr;
            for (int k=0;k<inner->len;k++) array_push(res->arr,value_incref(inner->items[k]));
        } else {
            array_push(res->arr,value_incref(arr->items[j]));
        }
    }
    return res;
}

static Value *builtin_chars(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_array_new();
    Value *arr=xs_array_new();
    const char *s=args[0]->s;
    for (int j=0;s[j];j++) array_push(arr->arr,xs_str_n(s+j,1));
    return arr;
}

static Value *builtin_bytes(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_array_new();
    Value *arr=xs_array_new();
    const char *s=args[0]->s;
    for (int j=0;s[j];j++) array_push(arr->arr,xs_int((unsigned char)s[j]));
    return arr;
}

static Value *builtin_zip(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<2||VAL_TAG(args[0])!=XS_ARRAY||VAL_TAG(args[1])!=XS_ARRAY) return xs_array_new();
    XSArray *a=args[0]->arr, *b=args[1]->arr;
    int n2=a->len<b->len?a->len:b->len;
    Value *res=xs_array_new();
    for (int j=0;j<n2;j++) {
        Value *tup=xs_tuple_new();
        array_push(tup->arr,value_incref(a->items[j]));
        array_push(tup->arr,value_incref(b->items[j]));
        array_push(res->arr,tup);
    }
    return res;
}

static Value *builtin_enumerate(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_ARRAY) return xs_array_new();
    int64_t start = 0;
    if (argc >= 2 && VAL_TAG(args[1]) == XS_INT) start = VAL_INT(args[1]);
    else if (argc >= 2 && VAL_TAG(args[1]) == XS_FLOAT) start = (int64_t)args[1]->f;
    Value *res=xs_array_new();
    XSArray *arr=args[0]->arr;
    for (int j=0;j<arr->len;j++) {
        Value *tup=xs_tuple_new();
        array_push(tup->arr,xs_int(start + j));
        array_push(tup->arr,value_incref(arr->items[j]));
        array_push(res->arr,tup);
    }
    return res;
}

static Value *builtin_sum(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_ARRAY) return xs_int(0);
    XSArray *arr=args[0]->arr;
    int64_t si=0; double sf=0; int is_f=0;
    for (int j=0;j<arr->len;j++) {
        if (VAL_TAG(arr->items[j])==XS_FLOAT){is_f=1;sf+=arr->items[j]->f;}
        else if(VAL_TAG(arr->items[j])==XS_INT) si+=VAL_INT(arr->items[j]);
    }
    return is_f?xs_float(sf+(double)si):xs_int(si);
}

/* string helpers */
static Value *builtin_format(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_str("");
    /* Simple format: {} placeholders replaced by args in order */
    const char *fmt = args[0]->s;
    int argidx = 1;
    char *result = xs_strdup(""); int rlen = 0;
    for (const char *p = fmt; *p; ) {
        if (*p=='{' && *(p+1)=='}') {
            char *s = (argidx<argc)?value_str(args[argidx++]):xs_strdup("{}");
            int slen=(int)strlen(s);
            result=xs_realloc(result,rlen+slen+1);
            memcpy(result+rlen,s,slen+1); rlen+=slen; free(s);
            p+=2;
        } else {
            result=xs_realloc(result,rlen+2);
            result[rlen++]=*p++; result[rlen]='\0';
        }
    }
    Value *v=xs_str(result); free(result); return v;
}

/* global stdin override for WASM playground */
FILE *g_xs_stdin_override = NULL;

static Value *builtin_input(Interp *i, Value **args, int argc) {
    (void)i;
    FILE *in = g_xs_stdin_override ? g_xs_stdin_override : stdin;
    if (argc>0) {
        char *s=value_str(args[0]); printf("%s",s); free(s); fflush(stdout);
    }
    char buf[4096]; buf[0]='\0';
    if (fgets(buf,sizeof(buf),in)) {
        int n=(int)strlen(buf);
        if (n>0&&buf[n-1]=='\n') buf[n-1]='\0';
    }
    return xs_str(buf);
}

static Value *builtin_exit(Interp *i, Value **args, int argc) {
    (void)i;
    int code = (argc>0&&VAL_TAG(args[0])==XS_INT)?(int)VAL_INT(args[0]):0;
    exit(code);
}

static Value *builtin_clear(Interp *i, Value **args, int argc) {
    (void)i;(void)args;(void)argc;
    printf("\033[2J\033[H");
    fflush(stdout);
    return value_incref(XS_NULL_VAL);
}

/* sorted(arr [, key_fn]): returns a sorted copy */
static int cmp_values(const void *a, const void *b) {
    Value *va = *(Value **)a;
    Value *vb = *(Value **)b;
    if (VAL_TAG(va) == XS_INT && VAL_TAG(vb) == XS_INT)
        return (VAL_INT(va) > VAL_INT(vb)) - (VAL_INT(va) < VAL_INT(vb));
    if ((VAL_TAG(va) == XS_FLOAT || VAL_TAG(va) == XS_INT) &&
        (VAL_TAG(vb) == XS_FLOAT || VAL_TAG(vb) == XS_INT)) {
        double fa = VAL_TAG(va)==XS_FLOAT ? va->f : (double)VAL_INT(va);
        double fb = VAL_TAG(vb)==XS_FLOAT ? vb->f : (double)VAL_INT(vb);
        return (fa > fb) - (fa < fb);
    }
    char *sa = value_str(va); char *sb = value_str(vb);
    int r = strcmp(sa, sb); free(sa); free(sb);
    return r;
}
static Value *builtin_sorted(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1 || VAL_TAG(args[0]) != XS_ARRAY) return value_incref(XS_NULL_VAL);
    XSArray *src = args[0]->arr;
    Value *copy = xs_array_new();
    for (int j = 0; j < src->len; j++)
        array_push(copy->arr, value_incref(src->items[j]));
    qsort(copy->arr->items, (size_t)copy->arr->len, sizeof(Value*), cmp_values);
    return copy;
}

static Value *builtin_assert_eq(Interp *i, Value **args, int argc) {
    if (argc < 2) {
        fprintf(stderr, "xs: assert_eq requires 2 arguments\n");
        if (i) { i->cf.signal = CF_PANIC; i->cf.value = xs_str("assert_eq requires 2 arguments"); }
        return value_incref(XS_NULL_VAL);
    }
    int equal = value_equal(args[0], args[1]);
    /* Float comparisons: allow a small relative/absolute tolerance so
       chained arithmetic (e.g. 3.14*r*r summed across shapes) still
       compares equal to a literal expected value. NaN stays unequal. */
    if (!equal && argc == 2) {
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
            if (diff <= 1e-9 + 1e-9 * scale) equal = 1;
        }
    }
    if (!equal) {
        char *a = value_repr(args[0]);
        char *b = value_repr(args[1]);
        const char *msg = (argc >= 3 && VAL_TAG(args[2]) == XS_STR) ? args[2]->s : "";
        fprintf(stderr, "xs: assertion failed: assert_eq(%s, %s)%s%s\n",
                a, b, msg[0] ? ": " : "", msg);
        free(a); free(b);
        if (i) { i->cf.signal = CF_PANIC; i->cf.value = xs_str("assert_eq failed"); }
    }
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_assert(Interp *i, Value **args, int argc) {
    if (argc<1||!value_truthy(args[0])) {
        const char *msg = (argc>1&&VAL_TAG(args[1])==XS_STR)?args[1]->s:"assertion failed";
        fprintf(stderr,"xs: assertion error: %s\n",msg);
        if (i) {
            i->cf.signal = CF_PANIC;
            i->cf.value  = xs_str(msg);
        }
    }
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_panic(Interp *i, Value **args, int argc) {
    char *msg;
    if (argc>0) msg=value_str(args[0]);
    else msg=xs_strdup("panic");
    fprintf(stderr,"xs: panic: %s\n",msg);
    free(msg);
    if (i) {
        i->cf.signal=CF_PANIC;
        i->cf.value=xs_str("panic");
    }
    exit(1);
}

static Value *builtin_copy(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return value_incref(XS_NULL_VAL);
    return value_copy(args[0]);
}

/* The canonical builtin_clone now lives in interp_derive.c (it's
 * exposed via interp.h). This file used to ship a small wrapper that
 * just delegated to value_copy; the impl in interp_derive does the
 * proper deep XSInst clone, which is what classes expect. */

/* Runtime for the todo() builtin -- panics with a message, like Rust's todo!() */
/* todo() / unreachable(): à la Rust */
static Value *builtin_todo(Interp *i, Value **args, int argc) {
    const char *msg = (argc >= 1 && VAL_TAG(args[0]) == XS_STR) ? args[0]->s : "not yet implemented";
    fprintf(stderr, "todo: %s\n", msg);
    if (i) { i->cf.signal = CF_PANIC; i->cf.value = xs_str(msg); }
    exit(1);
}

static Value *builtin_unreachable(Interp *i, Value **args, int argc) {
    (void)args; (void)argc;
    fprintf(stderr, "xs: Reached unreachable code\n");
    if (i) { i->cf.signal = CF_PANIC; i->cf.value = xs_str("Reached unreachable code"); }
    exit(1);
}

static Value *builtin_vec(Interp *i, Value **args, int argc) {
    (void)i;
    Value *arr = xs_array_new();
    for (int j = 0; j < argc; j++) {
        array_push(arr->arr, value_incref(args[j]));
    }
    return arr;
}

static Value *builtin_dbg(Interp *i, Value **args, int argc) {
    (void)i;
    for (int j=0;j<argc;j++) {
        char *s=value_repr(args[j]);
        fprintf(stderr,"[dbg] %s\n",s); free(s);
    }
    if (argc==1) return value_incref(args[0]);
    return value_incref(XS_NULL_VAL);
}

/* pprint */
static void pprint_value(Value *v, int indent) {
    char *pad = xs_malloc(indent + 1);
    memset(pad, ' ', indent); pad[indent] = '\0';

    if (VAL_TAG(v) == XS_MAP) {
        fprintf(stdout, "{\n");
        int printed = 0;
        for (int j = 0; j < v->map->cap; j++) {
            if (!v->map->keys[j]) continue;
            if (printed > 0) fprintf(stdout, ",\n");
            fprintf(stdout, "%s  \"%s\": ", pad, v->map->keys[j]);
            pprint_value(v->map->vals[j], indent + 2);
            printed++;
        }
        if (printed > 0) fprintf(stdout, "\n");
        fprintf(stdout, "%s}", pad);
    } else if (VAL_TAG(v) == XS_ARRAY) {
        fprintf(stdout, "[\n");
        for (int j = 0; j < v->arr->len; j++) {
            fprintf(stdout, "%s  ", pad);
            pprint_value(v->arr->items[j], indent + 2);
            if (j < v->arr->len - 1) fprintf(stdout, ",");
            fprintf(stdout, "\n");
        }
        fprintf(stdout, "%s]", pad);
    } else {
        char *s = value_repr(v);
        fprintf(stdout, "%s", s);
        free(s);
    }
    free(pad);
}

static Value *builtin_pprint(Interp *i, Value **args, int argc) {
    (void)i;
    for (int j = 0; j < argc; j++) {
        pprint_value(args[j], 0);
        fprintf(stdout, "\n");
    }
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_ord(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_int(0);
    if (VAL_TAG(args[0])==XS_STR) return xs_int((unsigned char)args[0]->s[0]);
    if (VAL_TAG(args[0])==XS_CHAR) return xs_int((unsigned char)args[0]->s[0]);
    return xs_int(0);
}

static Value *builtin_chr(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_INT) return xs_char(0);
    return xs_char((char)VAL_INT(args[0]));
}






/* channel(capacity) */
Value *native_channel_send(Interp *ig, Value **a, int n);
Value *native_channel_recv(Interp *ig, Value **a, int n);
Value *native_channel_try_recv(Interp *ig, Value **a, int n);
Value *native_channel_len(Interp *ig, Value **a, int n);
Value *native_channel_is_empty(Interp *ig, Value **a, int n);
Value *native_channel_is_full(Interp *ig, Value **a, int n);

static Value *builtin_channel(Interp *i, Value **args, int argc) {
    (void)i; (void)args; (void)argc;
    /* `channel()` returns a real concurrent channel: send wakes
       blocked recvs, recv blocks while empty (releasing the GIL so a
       sender on another thread can run). The optional cap arg is
       accepted but currently unbounded; .send never blocks. */
    int chid = xs_chan_alloc();
    Value *ch = xs_map_new();
    Value *t = xs_str("Channel");        map_set(ch->map,"_type",t);     value_decref(t);
    Value *idv = xs_int(chid);           map_set(ch->map,"_chan_id",idv); value_decref(idv);
    Value *data = xs_array_new();        map_set(ch->map,"_buf",data);    value_decref(data);
    map_take(ch->map, "send", xs_native(native_channel_send));
    map_take(ch->map, "recv", xs_native(native_channel_recv));
    map_take(ch->map, "try_recv", xs_native(native_channel_try_recv));
    map_take(ch->map, "len", xs_native(native_channel_len));
    map_take(ch->map, "is_empty", xs_native(native_channel_is_empty));
    map_take(ch->map, "is_full", xs_native(native_channel_is_full));
    return ch;
}

/* contains */
static Value *builtin_contains(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 2 || VAL_TAG(args[0]) != XS_STR || VAL_TAG(args[1]) != XS_STR)
        return value_incref(XS_FALSE_VAL);
    if (strstr(args[0]->s, args[1]->s))
        return value_incref(XS_TRUE_VAL);
    return value_incref(XS_FALSE_VAL);
}

/* Result / Option constructors */
static Value *make_result_enum(const char *type_name, const char *variant, Value **args, int argc) {
    XSEnum *en = xs_calloc(1, sizeof(XSEnum));
    en->type_name = xs_strdup(type_name);
    en->variant   = xs_strdup(variant);
    en->arr_data  = array_new();
    en->refcount  = 1;
    for (int k = 0; k < argc; k++)
        array_push(en->arr_data, value_incref(args[k]));
    Value *ev = xs_calloc(1, sizeof(Value));
    ev->tag = XS_ENUM_VAL; ev->refcount = 1; ev->en = en;
    return ev;
}

static Value *builtin_ok(Interp *interp, Value **args, int argc) {
    (void)interp;
    return make_result_enum("Ok", "Ok", args, argc);
}
static Value *builtin_err(Interp *interp, Value **args, int argc) {
    (void)interp;
    return make_result_enum("Err", "Err", args, argc);
}
static Value *builtin_some(Interp *interp, Value **args, int argc) {
    (void)interp;
    return make_result_enum("Some", "Some", args, argc);
}
static Value *builtin_none_fn(Interp *interp, Value **args, int argc) {
    (void)interp; (void)args; (void)argc;
    return value_incref(XS_NULL_VAL);
}

/* forward declarations for new modules (defined below) */
Value *make_async_module(void);
Value *make_net_module(void);
Value *make_crypto_module(void);
Value *make_thread_module(void);
Value *make_buf_module(void);
Value *make_encode_module(void);
Value *make_db_module(void);
Value *make_cli_module(void);
Value *make_ffi_module(void);
Value *make_reflect_module(void);
Value *make_gc_module(void);
Value *make_reactive_module(void);
Value *make_toml_module(void);
Value *make_http_module(void);
Value *make_fs_module(void);

/* register all */
void stdlib_register(Interp *i) {
    /* Core */
    interp_define_native(i, "print",     builtin_print);
    interp_define_native(i, "println",   builtin_print);
    interp_define_native(i, "eprint",    builtin_eprint);
    interp_define_native(i, "eprintln",  builtin_eprint);
    interp_define_native(i, "print_no_nl", builtin_print_no_nl);
    interp_define_native(i, "type",      builtin_type);
    interp_define_native(i, "typeof",    builtin_typeof);
    interp_define_native(i, "is_null",   builtin_is_null);
    interp_define_native(i, "is_int",    builtin_is_int);
    interp_define_native(i, "is_float",  builtin_is_float);
    interp_define_native(i, "is_str",    builtin_is_str);
    interp_define_native(i, "is_bool",   builtin_is_bool);
    interp_define_native(i, "is_array",  builtin_is_array);
    interp_define_native(i, "is_fn",     builtin_is_fn);
    interp_define_native(i, "int",       builtin_int);
    interp_define_native(i, "i64",       builtin_int);
    interp_define_native(i, "float",     builtin_float);
    interp_define_native(i, "f64",       builtin_float);
    interp_define_native(i, "str",       builtin_str);
    interp_define_native(i, "bool",      builtin_bool);
    interp_define_native(i, "char",      builtin_char);
    interp_define_native(i, "repr",      builtin_repr);
    interp_define_native(i, "dbg",       builtin_dbg);
    interp_define_native(i, "pprint",    builtin_pprint);
    interp_define_native(i, "len",       builtin_len);
    interp_define_native(i, "range",     builtin_range);
    interp_define_native(i, "array",     builtin_array);
    interp_define_native(i, "map",       builtin_map);
    interp_define_native(i, "filter",    builtin_filter);
    interp_define_native(i, "reduce",    builtin_reduce);
    interp_define_native(i, "keys",      builtin_keys);
    interp_define_native(i, "values",    builtin_values);
    interp_define_native(i, "entries",   builtin_entries);
    interp_define_native(i, "flatten",   builtin_flatten);
    interp_define_native(i, "chars",     builtin_chars);
    interp_define_native(i, "bytes",     builtin_bytes);
    interp_define_native(i, "zip",       builtin_zip);
    interp_define_native(i, "enumerate", builtin_enumerate);
    interp_define_native(i, "sum",       builtin_sum);
    interp_define_native(i, "abs",       builtin_abs);
    interp_define_native(i, "min",       builtin_min);
    interp_define_native(i, "max",       builtin_max);
    interp_define_native(i, "pow",       builtin_pow);
    interp_define_native(i, "sqrt",      builtin_sqrt);
    interp_define_native(i, "floor",     builtin_floor);
    interp_define_native(i, "ceil",      builtin_ceil);
    interp_define_native(i, "round",     builtin_round);
    interp_define_native(i, "log",       builtin_log);
    interp_define_native(i, "sin",       builtin_sin);
    interp_define_native(i, "cos",       builtin_cos);
    interp_define_native(i, "tan",       builtin_tan);
    interp_define_native(i, "format",    builtin_format);
    interp_define_native(i, "sprintf",   builtin_format);
    interp_define_native(i, "todo",      builtin_todo);
    interp_define_native(i, "unreachable", builtin_unreachable);
    interp_define_native(i, "vec",       builtin_vec);
    interp_define_native(i, "sorted",    builtin_sorted);
    interp_define_native(i, "input",     builtin_input);
    interp_define_native(i, "exit",      builtin_exit);
    interp_define_native(i, "clear",     builtin_clear);
    interp_define_native(i, "assert",    builtin_assert);
    interp_define_native(i, "assert_eq", builtin_assert_eq);
    interp_define_native(i, "panic",     builtin_panic);
    interp_define_native(i, "copy",      builtin_copy);
    interp_define_native(i, "clone",     builtin_clone);
    interp_define_native(i, "ord",       builtin_ord);
    interp_define_native(i, "chr",       builtin_chr);
extern Value *builtin_signal(Interp *, Value **, int);
extern Value *builtin_derived(Interp *, Value **, int);
    interp_define_native(i, "signal",    builtin_signal);
    interp_define_native(i, "derived",   builtin_derived);
    interp_define_native(i, "type_of",   builtin_type_of);
    interp_define_native(i, "contains",  builtin_contains);
    interp_define_native(i, "channel",   builtin_channel);

    /* Result / Option constructors */
    interp_define_native(i, "Ok",   builtin_ok);
    interp_define_native(i, "Err",  builtin_err);
    interp_define_native(i, "Some", builtin_some);
    interp_define_native(i, "None", builtin_none_fn);

    /* Modules */
    Value *math_mod = make_math_module();
    env_define(i->globals, "math", math_mod, 1);
    value_decref(math_mod);

    Value *time_mod = make_time_module();
    env_define(i->globals, "time", time_mod, 1);
    value_decref(time_mod);

    Value *io_mod = make_io_module();
    /* read_json / write_json live in builtins_json.c so they can reuse the
       json helpers; patched onto the io module here. */
    extern Value *native_io_read_json(Interp *, Value **, int);
    extern Value *native_io_write_json(Interp *, Value **, int);
    map_take(io_mod->map, "read_json", xs_native(native_io_read_json));
    map_take(io_mod->map, "write_json", xs_native(native_io_write_json));
    env_define(i->globals, "io", io_mod, 1);
    value_decref(io_mod);

    Value *string_mod = make_string_module();
    env_define(i->globals, "string", string_mod, 1);
    value_decref(string_mod);

    Value *path_mod = make_path_module();
    env_define(i->globals, "path", path_mod, 1);
    value_decref(path_mod);

    Value *base64_mod = make_base64_module();
    env_define(i->globals, "base64", base64_mod, 1);
    value_decref(base64_mod);

    Value *hash_mod = make_hash_module();
    env_define(i->globals, "hash", hash_mod, 1);
    value_decref(hash_mod);

    Value *uuid_mod = make_uuid_module();
    env_define(i->globals, "uuid", uuid_mod, 1);
    value_decref(uuid_mod);

    Value *collections_mod = make_collections_module();
    env_define(i->globals, "collections", collections_mod, 1);
    value_decref(collections_mod);

    Value *process_mod = make_process_module();
    env_define(i->globals, "process", process_mod, 1);
    value_decref(process_mod);

    Value *random_mod = make_random_module();
    env_define(i->globals, "random", random_mod, 1);
    value_decref(random_mod);
    srand((unsigned)time(NULL));

    Value *os_mod = make_os_module(i);
    env_define(i->globals, "os", os_mod, 1);
    value_decref(os_mod);

    Value *json_mod = make_json_module();
    env_define(i->globals, "json", json_mod, 1);
    value_decref(json_mod);

    Value *log_mod = make_log_module();
    env_define(i->globals, "log", log_mod, 1);
    value_decref(log_mod);

    Value *fmt_mod = make_fmt_module();
    env_define(i->globals, "fmt", fmt_mod, 1);
    value_decref(fmt_mod);

    Value *test_mod = make_test_module();
    env_define(i->globals, "test", test_mod, 1);
    value_decref(test_mod);

    Value *tracing_mod = make_tracing_module();
    env_define(i->globals, "tracing", tracing_mod, 1);
    value_decref(tracing_mod);

    Value *csv_mod = make_csv_module();
    env_define(i->globals, "csv", csv_mod, 1);
    value_decref(csv_mod);

    Value *url_mod = make_url_module();
    env_define(i->globals, "url", url_mod, 1);
    value_decref(url_mod);

    Value *re_mod = make_re_module();
    env_define(i->globals, "re", re_mod, 1);
    value_decref(re_mod);

    Value *msgpack_mod = make_msgpack_module();
    env_define(i->globals, "msgpack", msgpack_mod, 1);
    value_decref(msgpack_mod);

    Value *promise_mod = make_promise_module();
    env_define(i->globals, "Promise", promise_mod, 1);
    value_decref(promise_mod);

    /* Constants */
    {
        Value *v = xs_float(M_PI);
        env_define(i->globals, "PI", v, 0); value_decref(v);
    }
    {
        Value *v = xs_float(M_E);
        env_define(i->globals, "E", v, 0); value_decref(v);
    }
    {
        Value *v = xs_float(HUGE_VAL);
        env_define(i->globals, "INF", v, 0); value_decref(v);
    }
    {
        Value *v = xs_float(0.0/0.0);
        env_define(i->globals, "NAN", v, 0); value_decref(v);
    }

    /* Also expose math functions directly */
    interp_define_native(i, "sqrt", builtin_sqrt);
    interp_define_native(i, "floor",builtin_floor);
    interp_define_native(i, "ceil", builtin_ceil);
    interp_define_native(i, "round",builtin_round);

    /* new stdlib modules (12) */
    Value *async_mod = make_async_module();
    env_define(i->globals, "async", async_mod, 1);
    value_decref(async_mod);

    Value *net_mod = make_net_module();
    env_define(i->globals, "net", net_mod, 1);
    value_decref(net_mod);

    Value *crypto_mod = make_crypto_module();
    env_define(i->globals, "crypto", crypto_mod, 1);
    value_decref(crypto_mod);

    Value *thread_mod = make_thread_module();
    env_define(i->globals, "thread", thread_mod, 1);
    value_decref(thread_mod);

    Value *buf_mod = make_buf_module();
    env_define(i->globals, "buf", buf_mod, 1);
    value_decref(buf_mod);

    Value *encode_mod = make_encode_module();
    env_define(i->globals, "encode", encode_mod, 1);
    value_decref(encode_mod);

    Value *db_mod = make_db_module();
    env_define(i->globals, "db", db_mod, 1);
    value_decref(db_mod);

    Value *cli_mod = make_cli_module();
    env_define(i->globals, "cli", cli_mod, 1);
    value_decref(cli_mod);

    Value *ffi_mod = make_ffi_module();
    env_define(i->globals, "ffi", ffi_mod, 1);
    value_decref(ffi_mod);

    Value *reflect_mod = make_reflect_module();
    env_define(i->globals, "reflect", reflect_mod, 1);
    value_decref(reflect_mod);

    Value *gc_mod = make_gc_module();
    env_define(i->globals, "gc", gc_mod, 1);
    value_decref(gc_mod);

    Value *reactive_mod = make_reactive_module();
    env_define(i->globals, "reactive", reactive_mod, 1);
    value_decref(reactive_mod);

    Value *toml_mod = make_toml_module();
    env_define(i->globals, "toml", toml_mod, 1);
    value_decref(toml_mod);

    Value *http_mod = make_http_module();
    env_define(i->globals, "http", http_mod, 1);
    value_decref(http_mod);

    Value *fs_mod = make_fs_module();
    env_define(i->globals, "fs", fs_mod, 1);
    value_decref(fs_mod);
}

/* ═══════════════════════════════════════════════════════════════
 *  12 NEW STDLIB MODULES
 * ═══════════════════════════════════════════════════════════════ */










