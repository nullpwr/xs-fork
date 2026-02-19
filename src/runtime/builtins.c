#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "core/xs_compat.h"
#include "runtime/interp.h"
#include "runtime/builtins.h"
#include "runtime/error.h"
#include "core/xs_bigint.h"
#include "tls/xs_tls.h"
#include "core/gc.h"
#include "core/msgpack.h"
#include "runtime/async.h"
#include "runtime/concurrent.h"
#ifndef __wasi__
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

static int    g_xs_argc = 0;
static char **g_xs_argv = NULL;

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
    if (VAL_TAG(v)==XS_FLOAT) return xs_int((int64_t)v->f);
    if (VAL_TAG(v)==XS_STR) return xs_int(atoll(v->s));
    if (VAL_TAG(v)==XS_BOOL) return xs_int(VAL_INT(v));
    return xs_int(0);
}

static Value *builtin_float(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_float(0.0);
    Value *v=args[0];
    if (VAL_TAG(v)==XS_FLOAT) return value_incref(v);
    if (VAL_TAG(v)==XS_INT) return xs_float((double)VAL_INT(v));
    if (VAL_TAG(v)==XS_BIGINT) return xs_float(bigint_to_double(v->bigint));
    if (VAL_TAG(v)==XS_STR) return xs_float(atof(v->s));
    return xs_float(0.0);
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
    if (VAL_TAG(v)==XS_STR) return xs_int((int64_t)strlen(v->s));
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

static Value *builtin_clone(Interp *i, Value **args, int argc) {
    return builtin_copy(i, args, argc);
}

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


/* io module */
static Value *native_io_read_file(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = fopen(args[0]->s, "r");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=xs_malloc(sz+1);
    long nr = (long)fread(buf,1,sz,f); fclose(f); buf[nr]='\0';
    Value *v=xs_str(buf); free(buf); return v;
}

static Value *native_io_write_file(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<2||VAL_TAG(args[0])!=XS_STR||VAL_TAG(args[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(args[0]->s,"w");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(args[1]->s,f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}



/* collections module */
/* Stack: returns a map with _type="Stack" and _data=[] */
static Value *collections_stack_new(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    Value *stack=xs_map_new();
    Value *type=xs_str("Stack"); map_set(stack->map,"_type",type); value_decref(type);
    Value *data=xs_array_new(); map_set(stack->map,"_data",data); value_decref(data);
    return stack;
}
/* PriorityQueue: returns a map with _type="PriorityQueue" and _data=[] */
static Value *collections_pq_new(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
    Value *pq=xs_map_new();
    Value *type=xs_str("PriorityQueue"); map_set(pq->map,"_type",type); value_decref(type);
    Value *data=xs_array_new(); map_set(pq->map,"_data",data); value_decref(data);
    return pq;
}
/* Simple counter: Counter(arr) -> map of {item: count, _type: "Counter"} */
static Value *collections_counter(Interp *i, Value **a, int n) {
    (void)i;
    Value *result=xs_map_new();
    Value *type=xs_str("Counter"); map_set(result->map,"_type",type); value_decref(type);
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    for(int j=0;j<arr->len;j++){
        char *key=value_str(arr->items[j]);
        Value *cur=map_get(result->map,key);
        Value *next=xs_int(cur?(VAL_TAG(cur)==XS_INT?VAL_INT(cur):0)+1:1);
        map_set(result->map,key,next); value_decref(next);
        free(key);
    }
    return result;
}
static Value *collections_deque_new(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *d=xs_map_new();
    Value *t=xs_str("Deque"); map_set(d->map,"_type",t); value_decref(t);
    Value *data=xs_array_new(); map_set(d->map,"_data",data); value_decref(data);
    return d;
}
static Value *collections_set_new(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *s=xs_map_new();
    Value *t=xs_str("Set"); map_set(s->map,"_type",t); value_decref(t);
    Value *data=xs_map_new(); map_set(s->map,"_data",data); value_decref(data);
    /* If array passed, pre-populate */
    if (n>0&&VAL_TAG(a[0])==XS_ARRAY) {
        Value *d2=map_get(s->map,"_data");
        XSArray *arr=a[0]->arr;
        for (int j=0;j<arr->len;j++){
            char *k=value_str(arr->items[j]);
            Value *tv=value_incref(XS_TRUE_VAL);
            map_set(d2->map,k,tv); value_decref(tv);
            free(k);
        }
    }
    return s;
}
static Value *collections_ordered_map_new(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *om=xs_map_new();
    Value *t=xs_str("OrderedMap"); map_set(om->map,"_type",t); value_decref(t);
    Value *keys=xs_array_new(); map_set(om->map,"_keys",keys); value_decref(keys);
    Value *data=xs_map_new(); map_set(om->map,"_data",data); value_decref(data);
    return om;
}

static Value *collections_set_simple(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *result=xs_array_new();
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    /* track seen keys in a temporary map for dedup */
    XSMap *seen=map_new();
    for(int j=0;j<arr->len;j++){
        char *k=value_str(arr->items[j]);
        if (!map_get(seen,k)){
            Value *tv=value_incref(XS_TRUE_VAL);
            map_set(seen,k,tv); value_decref(tv);
            array_push(result->arr,value_incref(arr->items[j]));
        }
        free(k);
    }
    map_free(seen);
    return result;
}
static Value *collections_deque_simple(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    return xs_array_new();
}
static Value *collections_counter_simple(Interp *ig, Value **a, int n) {
    (void)ig;
    Value *result=xs_map_new();
    if (n<1||VAL_TAG(a[0])!=XS_ARRAY) return result;
    XSArray *arr=a[0]->arr;
    for(int j=0;j<arr->len;j++){
        char *key=value_str(arr->items[j]);
        Value *cur=map_get(result->map,key);
        Value *next=xs_int(cur?(VAL_TAG(cur)==XS_INT?VAL_INT(cur):0)+1:1);
        map_set(result->map,key,next); value_decref(next);
        free(key);
    }
    return result;
}
Value *make_collections_module(void) {
    XSMap *m=map_new();
    map_take(m,"Counter",      xs_native(collections_counter));
    map_take(m,"Stack",        xs_native(collections_stack_new));
    map_take(m,"PriorityQueue",xs_native(collections_pq_new));
    map_take(m,"Deque",        xs_native(collections_deque_new));
    map_take(m,"Set",          xs_native(collections_set_new));
    map_take(m,"OrderedMap",   xs_native(collections_ordered_map_new));
    map_take(m,"set",          xs_native(collections_set_simple));
    map_take(m,"deque",        xs_native(collections_deque_simple));
    map_take(m,"counter",      xs_native(collections_counter_simple));
    return xs_module(m);
}

/* process module */
static Value *native_process_pid(Interp *i, Value **a, int n) {
    (void)i;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0); /* WASI has no process identity */
#else
    return xs_int((int64_t)getpid());
#endif
}
static Value *native_process_run(Interp *i, Value **a, int n) {
    (void)i;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = popen(a[0]->s, "r");
    Value *result = xs_map_new();
    if (!f) {
        Value *ok=value_incref(XS_FALSE_VAL); map_set(result->map,"ok",ok); value_decref(ok);
        Value *out=xs_str(""); map_set(result->map,"stdout",out); value_decref(out);
        Value *code=xs_int(-1); map_set(result->map,"code",code); value_decref(code);
        return result;
    }
    size_t cap=256, pos=0;
    char *buf=xs_malloc(cap);
    int c;
    while ((c=fgetc(f))!=EOF) {
        if (pos+1>=cap) { cap*=2; buf=xs_realloc(buf,cap); }
        buf[pos++]=(char)c;
    }
    buf[pos]='\0';
    int status=pclose(f);
    int code2=(status==-1)?-1:(status>>8)&0xff;
    Value *ok=code2==0?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(result->map,"ok",ok); value_decref(ok);
    Value *out=xs_str(buf); free(buf); map_set(result->map,"stdout",out); value_decref(out);
    Value *cv=xs_int(code2); map_set(result->map,"code",cv); value_decref(cv);
    return result;
}
/* process.exec(cmd_string) - run shell command, return exit code */
static Value *native_process_exec(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_int(-1);
    int rc = system(a[0]->s);
    return xs_int(rc);
}

/* process.spawn(cmd, args, opts) - spawn with pipe access */
static Value *native_process_spawn_stdin_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "_stdin_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_FALSE_VAL);
    if (VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    ssize_t w = write((int)VAL_INT(fdv), a[1]->s, strlen(a[1]->s));
    return (w >= 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

#if !defined(__MINGW32__) && !defined(__wasi__)
static Value *native_process_spawn_stdout_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_stdout_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_NULL_VAL);
    int maxn = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxn = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxn + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxn);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
}

static Value *native_process_spawn_stderr_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_stderr_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT || VAL_INT(fdv) <= 0) return value_incref(XS_NULL_VAL);
    int maxn = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxn = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxn + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxn);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
}

static Value *native_process_spawn_wait(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return xs_int(-1);
    Value *pidv = map_get(a[0]->map, "pid");
    if (!pidv || VAL_TAG(pidv) != XS_INT) return xs_int(-1);
    int status = 0;
    waitpid((pid_t)VAL_INT(pidv), &status, 0);
    /* close remaining fds */
    Value *si = map_get(a[0]->map, "_stdin_fd");
    if (si && VAL_TAG(si) == XS_INT && VAL_INT(si) > 0) { close((int)VAL_INT(si)); map_take(a[0]->map, "_stdin_fd", xs_int(0)); }
    Value *so = map_get(a[0]->map, "_stdout_fd");
    if (so && VAL_TAG(so) == XS_INT && VAL_INT(so) > 0) { close((int)VAL_INT(so)); map_take(a[0]->map, "_stdout_fd", xs_int(0)); }
    Value *se = map_get(a[0]->map, "_stderr_fd");
    if (se && VAL_TAG(se) == XS_INT && VAL_INT(se) > 0) { close((int)VAL_INT(se)); map_take(a[0]->map, "_stderr_fd", xs_int(0)); }
    if (WIFEXITED(status)) return xs_int(WEXITSTATUS(status));
    return xs_int(-1);
}
#endif

static Value *native_process_spawn_kill(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_FALSE_VAL);
    Value *pidv = map_get(a[0]->map, "pid");
    if (!pidv || VAL_TAG(pidv) != XS_INT) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    int sig = SIGTERM;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) sig = (int)VAL_INT(a[1]);
    return (kill((pid_t)VAL_INT(pidv), sig) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

#if defined(__MINGW32__)
/* Windows spawn via _popen - captures stdout, uses pclose for wait */
static Value *native_process_spawn_stdout_read_win(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fpv = map_get(a[0]->map, "_fp");
    if (!fpv || VAL_TAG(fpv) != XS_INT || VAL_INT(fpv) == 0) return value_incref(XS_NULL_VAL);
    FILE *fp = (FILE*)(uintptr_t)VAL_INT(fpv);
    char buf[8192]; int total = 0;
    while (total < (int)sizeof(buf)-1) {
        int c = fgetc(fp);
        if (c == EOF) break;
        buf[total++] = (char)c;
    }
    buf[total] = '\0';
    return xs_str(buf);
}
static Value *native_process_spawn_wait_win(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return xs_int(-1);
    Value *fpv = map_get(a[0]->map, "_fp");
    if (!fpv || VAL_TAG(fpv) != XS_INT || VAL_INT(fpv) == 0) return xs_int(-1);
    FILE *fp = (FILE*)(uintptr_t)VAL_INT(fpv);
    int rc = _pclose(fp);
    map_take(a[0]->map, "_fp", xs_int(0));
    return xs_int(rc);
}
#endif

static Value *native_process_spawn(Interp *ig, Value **a, int n) {
    (void)ig;
#if defined(__MINGW32__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *cmd = a[0]->s;
    char cmdline[4096];
    if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
        int pos = snprintf(cmdline, sizeof(cmdline), "%s", cmd);
        for (int j = 0; j < a[1]->arr->len && pos < (int)sizeof(cmdline)-2; j++) {
            Value *av = a[1]->arr->items[j];
            pos += snprintf(cmdline+pos, sizeof(cmdline)-pos, " %s",
                           (VAL_TAG(av) == XS_STR) ? av->s : "");
        }
    } else {
        snprintf(cmdline, sizeof(cmdline), "%s", cmd);
    }
    FILE *fp = _popen(cmdline, "r");
    if (!fp) return value_incref(XS_NULL_VAL);
    XSMap *proc = map_new();
    map_take(proc, "pid", xs_int(0));
    map_take(proc, "_fp", xs_int((int64_t)(uintptr_t)fp));
    map_take(proc, "stdout_read", xs_native(native_process_spawn_stdout_read_win));
    map_take(proc, "stderr_read", xs_native(native_process_spawn_stdout_read_win));
    map_take(proc, "stdin_write", xs_native(native_process_spawn_stdin_write));
    map_take(proc, "wait", xs_native(native_process_spawn_wait_win));
    map_take(proc, "kill", xs_native(native_process_spawn_kill));
    return xs_module(proc);
#elif !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *cmd = a[0]->s;

    /* collect args */
    int nargs = 0;
    char **argv_list = NULL;
    if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
        nargs = a[1]->arr->len;
        argv_list = xs_malloc(sizeof(char*) * (nargs + 2));
        argv_list[0] = (char*)cmd;
        for (int j = 0; j < nargs; j++) {
            Value *av = a[1]->arr->items[j];
            argv_list[j+1] = (VAL_TAG(av) == XS_STR) ? av->s : "";
        }
        argv_list[nargs+1] = NULL;
    } else {
        argv_list = xs_malloc(sizeof(char*) * 4);
        argv_list[0] = "/bin/sh";
        argv_list[1] = "-c";
        argv_list[2] = (char*)cmd;
        argv_list[3] = NULL;
    }

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        free(argv_list);
        return value_incref(XS_NULL_VAL);
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(argv_list);
        return value_incref(XS_NULL_VAL);
    }
    if (pid == 0) {
        /* child */
        close(stdin_pipe[1]);  dup2(stdin_pipe[0], 0);  close(stdin_pipe[0]);
        close(stdout_pipe[0]); dup2(stdout_pipe[1], 1); close(stdout_pipe[1]);
        close(stderr_pipe[0]); dup2(stderr_pipe[1], 2); close(stderr_pipe[1]);
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY)
            execvp(cmd, argv_list);
        else
            execvp("/bin/sh", argv_list);
        _exit(127);
    }
    /* parent */
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    free(argv_list);

    XSMap *proc = map_new();
    map_take(proc, "pid", xs_int((int64_t)pid));
    map_take(proc, "_stdin_fd", xs_int((int64_t)stdin_pipe[1]));
    map_take(proc, "_stdout_fd", xs_int((int64_t)stdout_pipe[0]));
    map_take(proc, "_stderr_fd", xs_int((int64_t)stderr_pipe[0]));
    map_take(proc, "stdin_write", xs_native(native_process_spawn_stdin_write));
    map_take(proc, "stdout_read", xs_native(native_process_spawn_stdout_read));
    map_take(proc, "stderr_read", xs_native(native_process_spawn_stderr_read));
    map_take(proc, "wait", xs_native(native_process_spawn_wait));
    map_take(proc, "kill", xs_native(native_process_spawn_kill));
    return xs_module(proc);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* process.on_signal(sig_name, callback) */
#if !defined(__MINGW32__) && !defined(__wasi__)
static Interp *g_signal_interp = NULL;
static Value  *g_signal_handlers[32] = {0};

static void xs_signal_handler(int sig) {
    if (sig >= 0 && sig < 32 && g_signal_handlers[sig] && g_signal_interp) {
        Value *sv = xs_int(sig);
        Value *args[1] = { sv };
        Value *r = call_value(g_signal_interp, g_signal_handlers[sig], args, 1, "on_signal");
        if (r) value_decref(r);
        value_decref(sv);
    }
}
#endif

static Value *native_process_on_signal(Interp *ig, Value **a, int n) {
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[1]) != XS_FUNC && VAL_TAG(a[1]) != XS_NATIVE))
        return value_incref(XS_FALSE_VAL);
    int sig = -1;
    if (VAL_TAG(a[0]) == XS_INT) sig = (int)VAL_INT(a[0]);
    else if (VAL_TAG(a[0]) == XS_STR) {
        if (strcasecmp(a[0]->s, "SIGINT") == 0 || strcasecmp(a[0]->s, "INT") == 0) sig = SIGINT;
        else if (strcasecmp(a[0]->s, "SIGTERM") == 0 || strcasecmp(a[0]->s, "TERM") == 0) sig = SIGTERM;
        else if (strcasecmp(a[0]->s, "SIGHUP") == 0 || strcasecmp(a[0]->s, "HUP") == 0) sig = SIGHUP;
        else if (strcasecmp(a[0]->s, "SIGUSR1") == 0 || strcasecmp(a[0]->s, "USR1") == 0) sig = SIGUSR1;
        else if (strcasecmp(a[0]->s, "SIGUSR2") == 0 || strcasecmp(a[0]->s, "USR2") == 0) sig = SIGUSR2;
    }
    if (sig < 0 || sig >= 32) return value_incref(XS_FALSE_VAL);
    g_signal_interp = ig;
    if (g_signal_handlers[sig]) value_decref(g_signal_handlers[sig]);
    g_signal_handlers[sig] = value_incref(a[1]);
    signal(sig, xs_signal_handler);
    return value_incref(XS_TRUE_VAL);
#else
    (void)ig; (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* process.env(name) / process.env(name, value) */
static Value *native_process_env(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    if (n >= 2 && VAL_TAG(a[1]) == XS_STR) {
        setenv(a[0]->s, a[1]->s, 1);
        return value_incref(XS_TRUE_VAL);
    }
    const char *v = getenv(a[0]->s);
    return v ? xs_str(v) : value_incref(XS_NULL_VAL);
}

/* process.cwd() */
static Value *native_process_cwd(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return xs_str(buf);
    return xs_str(".");
}

/* process.exit(code) */
static Value *native_process_exit(Interp *ig, Value **a, int n) {
    (void)ig;
    int code = 0;
    if (n >= 1 && VAL_TAG(a[0]) == XS_INT) code = (int)VAL_INT(a[0]);
    exit(code);
    return value_incref(XS_NULL_VAL);
}

Value *make_process_module(void) {
    XSMap *m=map_new();
    map_take(m,"pid",       xs_native(native_process_pid));
    map_take(m,"run",       xs_native(native_process_run));
    map_take(m,"exec",      xs_native(native_process_exec));
    map_take(m,"spawn",     xs_native(native_process_spawn));
    map_take(m,"on_signal", xs_native(native_process_on_signal));
    map_take(m,"env",       xs_native(native_process_env));
    map_take(m,"cwd",       xs_native(native_process_cwd));
    map_take(m,"exit",      xs_native(native_process_exit));
    return xs_module(m);
}

static Value *native_io_wait_for_key(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR) printf("%s", args[0]->s);
    fflush(stdout);
    char buf[256]; buf[0] = '\0';
    if (fgets(buf, sizeof(buf), stdin)) {
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
static Value *native_io_read_line(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_STR) printf("%s", args[0]->s);
    fflush(stdout);
    char buf[1024]; buf[0] = '\0';
    if (fgets(buf, sizeof(buf), stdin)) {
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
static Value *native_io_get_key_nowait(Interp *ig, Value **args, int argc) {
    (void)ig;
    int timeout_ms = 0;
    if (argc >= 1 && VAL_TAG(args[0]) == XS_INT) timeout_ms = (int)VAL_INT(args[0]);
#if defined(__wasi__)
    (void)timeout_ms;
    return value_incref(XS_NULL_VAL);
#elif defined(__MINGW32__)
    /* Windows: poll with kbhit in a loop */
    #include <conio.h>
    DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
    while (GetTickCount() < deadline || timeout_ms == 0) {
        if (_kbhit()) {
            char buf[64]; buf[0]='\0'; int bi=0;
            int c = _getch();
            if (c == 0 || c == 0xE0) { /* special key prefix */
                int c2 = _getch();
                if (c2==72) return xs_str("UP");
                if (c2==80) return xs_str("DOWN");
                if (c2==75) return xs_str("LEFT");
                if (c2==77) return xs_str("RIGHT");
                return xs_str("UNKNOWN");
            }
            buf[bi++]=(char)c; buf[bi]='\0';
            return xs_str(buf);
        }
        if (timeout_ms == 0) break;
        Sleep(10);
    }
    return value_incref(XS_NULL_VAL);
#else
    /* POSIX: use select() */
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) return value_incref(XS_NULL_VAL);
    char buf[64]; buf[0] = '\0';
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf)-1);
    if (n <= 0) return value_incref(XS_NULL_VAL);
    buf[n] = '\0';
    int blen = (int)n;
    while (blen > 0 && (buf[blen-1]=='\n'||buf[blen-1]=='\r')) buf[--blen]='\0';
    if (buf[0] == '\033') {
        if (strcmp(buf, "\033[A")==0) return xs_str("UP");
        if (strcmp(buf, "\033[B")==0) return xs_str("DOWN");
        if (strcmp(buf, "\033[C")==0) return xs_str("RIGHT");
        if (strcmp(buf, "\033[D")==0) return xs_str("LEFT");
        return xs_str("ESC");
    }
    return xs_str(buf);
#endif
}
static Value *native_io_append_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"a");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(a[1]->s,f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}
static Value *native_io_read_lines(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(a[0]->s,"r");
    if (!f) return value_incref(XS_NULL_VAL);
    Value *arr=xs_array_new();
    char buf[4096];
    while (fgets(buf,sizeof(buf),f)) {
        int len=(int)strlen(buf);
        while (len>0&&(buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
        array_push(arr->arr,xs_str(buf));
    }
    fclose(f); return arr;
}
static Value *native_io_write_lines(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_ARRAY) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"w");
    if (!f) return value_incref(XS_FALSE_VAL);
    XSArray *arr=a[1]->arr;
    for (int j=0;j<arr->len;j++) {
        char *s=value_str(arr->items[j]); fputs(s,f); fputc('\n',f); free(s);
    }
    fclose(f); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_read_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(a[0]->s,"rb");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *buf=xs_malloc(sz+1);
    long nr = (long)fread(buf,1,sz,f); fclose(f);
    Value *arr=xs_array_new();
    for (long j=0;j<nr;j++) array_push(arr->arr,xs_int((int64_t)buf[j]));
    free(buf); return arr;
}
static Value *native_io_write_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_ARRAY) return value_incref(XS_FALSE_VAL);
    FILE *f=fopen(a[0]->s,"wb");
    if (!f) return value_incref(XS_FALSE_VAL);
    XSArray *arr=a[1]->arr;
    for (int j=0;j<arr->len;j++) {
        if (VAL_TAG(arr->items[j])==XS_INT) {
            unsigned char b=(unsigned char)(VAL_INT(arr->items[j])&0xff);
            fwrite(&b,1,1,f);
        }
    }
    fclose(f); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_file_exists(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (access(a[0]->s,F_OK)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_file_size(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_int(-1);
    struct stat st; if (stat(a[0]->s,&st)!=0) return xs_int(-1);
    return xs_int((int64_t)st.st_size);
}
static Value *native_io_delete_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (remove(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_copy_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *src=fopen(a[0]->s,"rb"); if (!src) return value_incref(XS_FALSE_VAL);
    FILE *dst=fopen(a[1]->s,"wb"); if (!dst){fclose(src);return value_incref(XS_FALSE_VAL);}
    char buf[8192]; size_t r;
    while ((r=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,r,dst);
    fclose(src); fclose(dst); return value_incref(XS_TRUE_VAL);
}
static Value *native_io_rename_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rename(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static int io_mkdirs(const char *path) {
    char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",path);
    int len=(int)strlen(tmp);
    if (tmp[len-1]=='/') tmp[--len]='\0';
    for (int j=1;j<len;j++) {
        if (tmp[j]=='/') {
            tmp[j]='\0'; mkdir(tmp,0755); tmp[j]='/';
        }
    }
    return mkdir(tmp,0755);
}
static Value *native_io_make_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    int r=io_mkdirs(a[0]->s);
    return (r==0||errno==EEXIST)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_list_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    DIR *d=opendir(a[0]->s); if (!d) return xs_array_new();
    Value *arr=xs_array_new();
    struct dirent *ent;
    while ((ent=readdir(d))!=NULL) {
        if (strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0) continue;
        array_push(arr->arr,xs_str(ent->d_name));
    }
    closedir(d); return arr;
}
static Value *native_io_stdin_read(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    size_t cap=256,pos=0; char *buf=xs_malloc(cap);
    int c;
    while ((c=fgetc(stdin))!=EOF) {
        if (pos+1>=cap){cap*=2;buf=xs_realloc(buf,cap);}
        buf[pos++]=(char)c;
    }
    buf[pos]='\0'; Value *v=xs_str(buf); free(buf); return v;
}
static Value *native_io_stdin_readline(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    char buf[4096]; buf[0]='\0';
    if (fgets(buf,sizeof(buf),stdin)){
        int len=(int)strlen(buf);
        while(len>0&&(buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
    }
    return xs_str(buf);
}
static Value *native_io_stdin_read_n(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return value_incref(XS_NULL_VAL);
    int64_t count = VAL_INT(a[0]);
    if (count <= 0) return xs_str("");
    char *buf = xs_malloc((size_t)count + 1);
    size_t total = 0;
    while (total < (size_t)count) {
        size_t r = fread(buf + total, 1, (size_t)count - total, stdin);
        if (r == 0) break;
        total += r;
    }
    buf[total] = '\0';
    Value *v = xs_str(buf);
    free(buf);
    return v;
}
static Value *native_io_is_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_io_is_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}

/* move_file: rename, falling back to copy+delete for cross-device moves */
static Value *native_io_move_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    if (rename(a[0]->s,a[1]->s)==0) return value_incref(XS_TRUE_VAL);
    /* cross-device fallback: copy then delete */
    FILE *src=fopen(a[0]->s,"rb"); if (!src) return value_incref(XS_FALSE_VAL);
    FILE *dst=fopen(a[1]->s,"wb"); if (!dst){fclose(src);return value_incref(XS_FALSE_VAL);}
    char buf[8192]; size_t r;
    while ((r=fread(buf,1,sizeof(buf),src))>0) fwrite(buf,1,r,dst);
    fclose(src); fclose(dst);
    remove(a[0]->s);
    return value_incref(XS_TRUE_VAL);
}

/* temp_file: create a temp file, return its path */
static Value *native_io_temp_file(Interp *ig, Value **a, int n) {
    (void)ig;
    const char *suffix = (n>=1 && VAL_TAG(a[0])==XS_STR) ? a[0]->s : "";
    const char *prefix = (n>=2 && VAL_TAG(a[1])==XS_STR) ? a[1]->s : "xs_tmp_";
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char tmpl[4096];
    snprintf(tmpl,sizeof(tmpl),"%s/%sXXXXXX%s",tmpdir,prefix,suffix);
#if !defined(__MINGW32__) && !defined(__wasi__)
    int fd;
    if (suffix[0]) {
        #ifdef __APPLE__
        fd = mkstemp(tmpl);
#else
        fd = mkstemps(tmpl,(int)strlen(suffix));
#endif
    } else {
        fd = mkstemp(tmpl);
    }
    if (fd<0) return value_incref(XS_NULL_VAL);
    close(fd);
#else
    if (!_mktemp(tmpl)) return value_incref(XS_NULL_VAL);
    FILE *f=fopen(tmpl,"w"); if (f) fclose(f);
#endif
    return xs_str(tmpl);
}

/* temp_dir: create a temp directory, return its path */
static Value *native_io_temp_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    const char *prefix = (n>=1 && VAL_TAG(a[0])==XS_STR) ? a[0]->s : "xs_tmpd_";
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = "/tmp";
    char tmpl[4096];
    snprintf(tmpl,sizeof(tmpl),"%s/%sXXXXXX",tmpdir,prefix);
#if !defined(__MINGW32__) && !defined(__wasi__)
    #ifdef __APPLE__
    extern char *mkdtemp(char *);
#endif
    char *res = mkdtemp(tmpl);
    if (!res) return value_incref(XS_NULL_VAL);
    return xs_str(res);
#else
    if (!_mktemp(tmpl)) return value_incref(XS_NULL_VAL);
    mkdir(tmpl, 0700);
    return xs_str(tmpl);
#endif
}

/* file_info: return map with size, is_file, is_dir, modified, path */
static Value *native_io_file_info(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    struct stat st;
    if (stat(a[0]->s,&st)!=0) return value_incref(XS_NULL_VAL);
    Value *m = xs_map_new();
    Value *v;
    v=xs_int((int64_t)st.st_size); map_set(m->map,"size",v); value_decref(v);
    v=S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(m->map,"is_file",v); value_decref(v);
    v=S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
    map_set(m->map,"is_dir",v); value_decref(v);
    v=xs_int((int64_t)st.st_mtime); map_set(m->map,"modified",v); value_decref(v);
    v=xs_str(a[0]->s); map_set(m->map,"path",v); value_decref(v);
    return m;
}

/* stdin_lines: read all stdin lines as array */
static Value *native_io_stdin_lines(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *arr=xs_array_new();
    char buf[4096];
    while (fgets(buf,sizeof(buf),stdin)) {
        int len2=(int)strlen(buf);
        while (len2>0&&(buf[len2-1]=='\n'||buf[len2-1]=='\r')) buf[--len2]='\0';
        array_push(arr->arr,xs_str(buf));
    }
    return arr;
}

/* glob: pattern matching for file paths */
static Value *native_io_glob(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
#if !defined(__wasi__)
    glob_t g; memset(&g,0,sizeof(g));
    Value *arr=xs_array_new();
    if (glob(a[0]->s,0,NULL,&g)==0) {
        for (size_t j=0;j<g.gl_pathc;j++) array_push(arr->arr,xs_str(g.gl_pathv[j]));
    }
    globfree(&g); return arr;
#else
    return xs_array_new();
#endif
}

/* symlink: create a symbolic link */
static Value *native_io_symlink(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
#if !defined(__MINGW32__) && !defined(__wasi__)
    return (symlink(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
#else
    return value_incref(XS_FALSE_VAL);
#endif
}

/* stdout/stderr sub-module helpers */
static Value *native_io_stdout_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) fputs(a[0]->s,stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stdout_writeln(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) { fputs(a[0]->s,stdout); fputc('\n',stdout); }
    else fputc('\n',stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stdout_flush(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    fflush(stdout);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) fputs(a[0]->s,stderr);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_writeln(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n>=1 && VAL_TAG(a[0])==XS_STR) { fputs(a[0]->s,stderr); fputc('\n',stderr); }
    else fputc('\n',stderr);
    return value_incref(XS_NULL_VAL);
}
static Value *native_io_stderr_flush(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    fflush(stderr);
    return value_incref(XS_NULL_VAL);
}

Value *make_io_module(void) {
    XSMap *m = map_new();
    /* file operations */
    map_take(m,"read_file",      xs_native(native_io_read_file));
    map_take(m,"write_file",     xs_native(native_io_write_file));
    map_take(m,"append_file",    xs_native(native_io_append_file));
    map_take(m,"read_lines",     xs_native(native_io_read_lines));
    map_take(m,"write_lines",    xs_native(native_io_write_lines));
    map_take(m,"read_bytes",     xs_native(native_io_read_bytes));
    map_take(m,"write_bytes",    xs_native(native_io_write_bytes));
    /* file info */
    map_take(m,"file_exists",    xs_native(native_io_file_exists));
    map_take(m,"exists",         xs_native(native_io_file_exists));
    map_take(m,"file_size",      xs_native(native_io_file_size));
    map_take(m,"size",           xs_native(native_io_file_size));
    map_take(m,"file_info",      xs_native(native_io_file_info));
    map_take(m,"is_file",        xs_native(native_io_is_file));
    map_take(m,"is_dir",         xs_native(native_io_is_dir));
    /* file manipulation */
    map_take(m,"delete_file",    xs_native(native_io_delete_file));
    map_take(m,"copy_file",      xs_native(native_io_copy_file));
    map_take(m,"move_file",      xs_native(native_io_move_file));
    map_take(m,"rename_file",    xs_native(native_io_rename_file));
    map_take(m,"symlink",        xs_native(native_io_symlink));
    /* directories */
    map_take(m,"make_dir",       xs_native(native_io_make_dir));
    map_take(m,"list_dir",       xs_native(native_io_list_dir));
    map_take(m,"glob",           xs_native(native_io_glob));
    /* temp files */
    map_take(m,"temp_file",      xs_native(native_io_temp_file));
    map_take(m,"temp_dir",       xs_native(native_io_temp_dir));
    /* stdin */
    map_take(m,"stdin_read",     xs_native(native_io_stdin_read));
    map_take(m,"stdin_readline", xs_native(native_io_stdin_readline));
    map_take(m,"stdin_read_n",  xs_native(native_io_stdin_read_n));
    map_take(m,"stdin_lines",    xs_native(native_io_stdin_lines));
    /* keyboard */
    map_take(m,"wait_for_key",   xs_native(native_io_wait_for_key));
    map_take(m,"read_line",      xs_native(native_io_read_line));
    map_take(m,"get_key_nowait", xs_native(native_io_get_key_nowait));
    /* stdout sub-module */
    Value *out_m=xs_map_new();
    map_take(out_m->map, "write", xs_native(native_io_stdout_write));
    map_take(out_m->map, "writeln", xs_native(native_io_stdout_writeln));
    map_take(out_m->map, "flush", xs_native(native_io_stdout_flush));
    map_set(m,"stdout",out_m); value_decref(out_m);
    /* stderr sub-module */
    Value *err_m=xs_map_new();
    map_take(err_m->map, "write", xs_native(native_io_stderr_write));
    map_take(err_m->map, "writeln", xs_native(native_io_stderr_writeln));
    map_take(err_m->map, "flush", xs_native(native_io_stderr_flush));
    map_set(m,"stderr",err_m); value_decref(err_m);
    return xs_module(m);
}

/* os module */
static Value *native_os_cwd(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    char buf[4096]; if (!getcwd(buf,sizeof(buf))) return value_incref(XS_NULL_VAL);
    return xs_str(buf);
}
static Value *native_os_chdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (chdir(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_home(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    const char *h=getenv("HOME"); return h?xs_str(h):xs_str("");
}
static Value *native_os_tempdir(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    const char *t=getenv("TMPDIR"); return xs_str(t?t:"/tmp");
}
static Value *native_os_mkdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    int r=io_mkdirs(a[0]->s);
    return (r==0||errno==EEXIST)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_rmdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rmdir(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_remove(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (unlink(a[0]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_rename(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (rename(a[0]->s,a[1]->s)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_exists(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (access(a[0]->s,F_OK)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_is_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISREG(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_is_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s,&st)!=0) return value_incref(XS_FALSE_VAL);
    return S_ISDIR(st.st_mode)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_cpu_count(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    #ifdef _SC_NPROCESSORS_ONLN
    long c=sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__APPLE__)
    long c=1; /* fallback */
#else
    long c=1;
#endif
    return xs_int(c>0?c:1);
}
static Value *native_os_pid(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0);
#else
    return xs_int((int64_t)getpid());
#endif
}
static Value *native_os_ppid(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
#ifdef __wasi__
    return xs_int(0);
#else
    return xs_int((int64_t)getppid());
#endif
}
static Value *native_os_exit(Interp *ig, Value **a, int n) {
    (void)ig;
    int code=(n>0&&VAL_TAG(a[0])==XS_INT)?(int)VAL_INT(a[0]):0;
    exit(code);
}
static Value *native_os_list_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    DIR *d=opendir(a[0]->s); if (!d) return xs_array_new();
    Value *arr=xs_array_new();
    struct dirent *ent;
    while ((ent=readdir(d))!=NULL) {
        if (strcmp(ent->d_name,".")==0||strcmp(ent->d_name,"..")==0) continue;
        array_push(arr->arr,xs_str(ent->d_name));
    }
    closedir(d); return arr;
}
static Value *native_os_glob(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return xs_array_new();
    glob_t g; memset(&g,0,sizeof(g));
    Value *arr=xs_array_new();
    if (glob(a[0]->s,0,NULL,&g)==0) {
        for (size_t j=0;j<g.gl_pathc;j++) array_push(arr->arr,xs_str(g.gl_pathv[j]));
    }
    globfree(&g); return arr;
}
static Value *native_os_env_get(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *v=getenv(a[0]->s);
    if (!v) return (n>1)?value_incref(a[1]):value_incref(XS_NULL_VAL);
    return xs_str(v);
}
static Value *native_os_env_set(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return (setenv(a[0]->s,a[1]->s,1)==0)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
static Value *native_os_env_has(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_FALSE_VAL);
    return getenv(a[0]->s)?value_incref(XS_TRUE_VAL):value_incref(XS_FALSE_VAL);
}
#ifndef _WIN32
extern char **environ;
#endif
static Value *native_os_env_all(Interp *ig, Value **a, int n) {
    (void)ig;(void)a;(void)n;
    Value *m=xs_map_new();
    for (char **ep=environ;ep&&*ep;ep++) {
        char *eq=strchr(*ep,'=');
        if (!eq) continue;
        char key[256]; int klen=(int)(eq-*ep);
        if (klen>=256) klen=255;
        strncpy(key,*ep,klen); key[klen]='\0';
        Value *v=xs_str(eq+1); map_set(m->map,key,v); value_decref(v);
    }
    return m;
}

Value *make_os_module(Interp *ig) {
    XSMap *m=map_new();
    Value *args_arr=xs_array_new();
    for (int ai = 0; ai < g_xs_argc; ai++) {
        Value *s = xs_str(g_xs_argv[ai]);
        array_push(args_arr->arr, s);
        value_decref(s);
    }
    map_set(m,"args",args_arr); value_decref(args_arr);
    map_take(m,"cwd",      xs_native(native_os_cwd));
    map_take(m,"chdir",    xs_native(native_os_chdir));
    map_take(m,"home",     xs_native(native_os_home));
    map_take(m,"tempdir",  xs_native(native_os_tempdir));
    map_take(m,"mkdir",    xs_native(native_os_mkdir));
    map_take(m,"rmdir",    xs_native(native_os_rmdir));
    map_take(m,"remove",   xs_native(native_os_remove));
    map_take(m,"rename",   xs_native(native_os_rename));
    map_take(m,"exists",   xs_native(native_os_exists));
    map_take(m,"is_file",  xs_native(native_os_is_file));
    map_take(m,"is_dir",   xs_native(native_os_is_dir));
    map_take(m,"cpu_count",xs_native(native_os_cpu_count));
    map_take(m,"pid",      xs_native(native_os_pid));
    map_take(m,"ppid",     xs_native(native_os_ppid));
    map_take(m,"exit",     xs_native(native_os_exit));
    map_take(m,"list_dir", xs_native(native_os_list_dir));
    map_take(m,"glob",     xs_native(native_os_glob));
    /* platform / sep */
#ifdef __APPLE__
    { Value *v=xs_str("darwin"); map_set(m,"platform",v); value_decref(v); }
#elif defined(_WIN32)
    { Value *v=xs_str("windows"); map_set(m,"platform",v); value_decref(v); }
#else
    { Value *v=xs_str("linux"); map_set(m,"platform",v); value_decref(v); }
#endif
#ifdef _WIN32
    { Value *v=xs_str("\\"); map_set(m,"sep",v); value_decref(v); }
#else
    { Value *v=xs_str("/"); map_set(m,"sep",v); value_decref(v); }
#endif
    /* env as a callable (getenv) + helper functions at top level */
    map_take(m,"env",      xs_native(native_os_env_get));
    map_take(m,"getenv",   xs_native(native_os_env_get));
    map_take(m,"setenv",   xs_native(native_os_env_set));
    map_take(m,"hasenv",   xs_native(native_os_env_has));
    map_take(m,"environ",  xs_native(native_os_env_all));
    (void)ig;
    return xs_module(m);
}



/* reactive primitives */
static Value *builtin_signal(Interp *i, Value **args, int argc) {
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
static Value *builtin_derived(Interp *i, Value **args, int argc) {
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

/* channel(capacity) */
static Value *native_channel_send(Interp *ig, Value **a, int n);
static Value *native_channel_recv(Interp *ig, Value **a, int n);
static Value *native_channel_try_recv(Interp *ig, Value **a, int n);
static Value *native_channel_len(Interp *ig, Value **a, int n);
static Value *native_channel_is_empty(Interp *ig, Value **a, int n);
static Value *native_channel_is_full(Interp *ig, Value **a, int n);

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

static Value *native_channel_send(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return value_incref(XS_NULL_VAL);
    return xs_chan_send(a[0], a[1]);
}

static Value *native_channel_recv(Interp *ig, Value **a, int n) {
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_recv(a[0], ig);
}

static Value *native_channel_try_recv(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    return xs_chan_try_recv(a[0]);
}

static Value *native_channel_len(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_int(0);
    return xs_int(xs_chan_len(a[0]));
}

static Value *native_channel_is_empty(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_TRUE_VAL);
    return xs_chan_len(a[0]) == 0 ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_channel_is_full(Interp *ig, Value **a, int n) {
    /* The current channel implementation is unbounded, so a channel is
       never full. Kept for API compatibility with the previous version. */
    (void)ig; (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
}

static Value *native_async_channel(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    /* Concurrent channel: FIFO buffer + mutex/condvar slot allocated
       in a global table so the sync state survives across send/recv
       calls. recv blocks until data is available; sends wake all
       waiters. */
    int chid = xs_chan_alloc();
    XSMap *ch = map_new();
    Value *buf = xs_array_new();
    map_set(ch, "_buf", buf);
    value_decref(buf);
    Value *idv = xs_int(chid);
    map_set(ch, "_chan_id", idv);
    value_decref(idv);
    map_take(ch, "send", xs_native(native_channel_send));
    map_take(ch, "recv", xs_native(native_channel_recv));
    map_take(ch, "try_recv", xs_native(native_channel_try_recv));
    map_take(ch, "len", xs_native(native_channel_len));
    map_take(ch, "is_empty", xs_native(native_channel_is_empty));
    map_take(ch, "is_full", xs_native(native_channel_is_full));
    return xs_module(ch);
}

static Value *native_async_select(Interp *ig, Value **a, int n) {
    (void)ig;
    /* select(channels_or_tasks): poll an array of channel/task-like
       values and return a map { index: <idx>, value: <result> } for the
       first one that has a ready result.
       A channel is ready when its "_buf" array is non-empty.
       A task/promise is ready when it has a "_result" key.
       If nothing is ready, return null. */
    if (n < 1 || VAL_TAG(a[0]) != XS_ARRAY) return value_incref(XS_NULL_VAL);
    XSArray *arr = a[0]->arr;
    for (int i = 0; i < arr->len; i++) {
        Value *item = arr->items[i];
        if ((VAL_TAG(item) == XS_MAP || VAL_TAG(item) == XS_MODULE) && item->map) {
            /* Check for channel readiness: "_buf" array with len > 0 */
            Value *buf = map_get(item->map, "_buf");
            if (buf && VAL_TAG(buf) == XS_ARRAY && buf->arr->len > 0) {
                /* Consume the first buffered value */
                Value *val = value_incref(buf->arr->items[0]);
                /* Shift the buffer: remove first element */
                for (int j = 0; j < buf->arr->len - 1; j++)
                    buf->arr->items[j] = buf->arr->items[j + 1];
                buf->arr->len--;
                XSMap *result = map_new();
                map_take(result, "index", xs_int(i));
                map_set(result, "value", val);
                value_decref(val);
                return xs_module(result);
            }
            /* Check for task/promise readiness: "_result" key present */
            Value *res = map_get(item->map, "_result");
            if (res) {
                XSMap *result = map_new();
                map_take(result, "index", xs_int(i));
                map_set(result, "value", value_incref(res));
                value_decref(res);
                return xs_module(result);
            }
        }
    }
    /* Nothing ready */
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

/* net */
#if !defined(__MINGW32__) && !defined(__wasi__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

static Value *native_net_tcp_connect(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    const char *host = a[0]->s;
    int port = (VAL_TAG(a[1]) == XS_INT) ? (int)VAL_INT(a[1]) : 0;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return value_incref(XS_NULL_VAL);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return value_incref(XS_NULL_VAL); }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return value_incref(XS_NULL_VAL);
    }
    freeaddrinfo(res);

    XSMap *conn = map_new();
    map_take(conn, "fd", xs_int(fd));
    return xs_module(conn);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

static Value *native_net_tcp_listen(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1) return value_incref(XS_NULL_VAL);
    int port = (VAL_TAG(a[0]) == XS_INT) ? (int)VAL_INT(a[0]) : 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return value_incref(XS_NULL_VAL);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }
    if (listen(fd, 128) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }

    XSMap *srv = map_new();
    map_take(srv, "fd", xs_int(fd));
    return xs_module(srv);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

static Value *native_net_resolve(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(a[0]->s, NULL, &hints, &res) != 0)
        return xs_array_new();

    Value *arr = xs_array_new();
    for (p = res; p; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof ip);
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, ip, sizeof ip);
        }
        Value *s = xs_str(ip);
        array_push(arr->arr, s);
        value_decref(s);
    }
    freeaddrinfo(res);
    return arr;
#else
    (void)a; (void)n;
    return xs_array_new();
#endif
}

static Value *native_net_url_parse(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_map_new();
    Value *m = xs_map_new();
    const char *url = a[0]->s;

    /* scheme */
    const char *p = strstr(url, "://");
    if (p) {
        Value *v = xs_str_n(url, (int)(p - url)); map_set(m->map, "scheme", v); value_decref(v);
        url = p + 3;
    } else {
        Value *v = xs_str(""); map_set(m->map, "scheme", v); value_decref(v);
    }

    /* host and port */
    const char *slash = strchr(url, '/');
    const char *host_end = slash ? slash : url + strlen(url);
    char host_buf[512];
    int hlen = (int)(host_end - url);
    if (hlen >= (int)sizeof(host_buf)) hlen = (int)sizeof(host_buf) - 1;
    memcpy(host_buf, url, hlen); host_buf[hlen] = '\0';

    const char *colon = strchr(host_buf, ':');
    if (colon) {
        Value *hv = xs_str_n(host_buf, (int)(colon - host_buf));
        map_set(m->map, "host", hv); value_decref(hv);
        Value *pv = xs_int(atoi(colon + 1));
        map_set(m->map, "port", pv); value_decref(pv);
    } else {
        Value *hv = xs_str(host_buf);
        map_set(m->map, "host", hv); value_decref(hv);
        Value *pv = xs_int(0);
        map_set(m->map, "port", pv); value_decref(pv);
    }

    url = host_end;

    /* path and query */
    const char *qm = strchr(url, '?');
    if (qm) {
        Value *pv = xs_str_n(url, (int)(qm - url)); map_set(m->map, "path", pv); value_decref(pv);
        Value *qv = xs_str(qm + 1); map_set(m->map, "query", qv); value_decref(qv);
    } else {
        Value *pv = xs_str(url); map_set(m->map, "path", pv); value_decref(pv);
        Value *qv = xs_str(""); map_set(m->map, "query", qv); value_decref(qv);
    }

    return m;
}

/* HTTP client helpers */

#if !defined(__MINGW32__) && !defined(__wasi__)

/* Parse a URL into host, port, path. Returns 0 on success, -1 on error. */
static int http_parse_url(const char *url, char *host, int hostlen,
                          int *port, char *path, int pathlen) {
    const char *start = url;
    if (strncmp(url, "https://", 8) == 0) { start = url + 8; *port = 443; }
    else if (strncmp(url, "http://", 7) == 0) { start = url + 7; *port = 80; }
    else { *port = 80; }

    const char *slash = strchr(start, '/');
    const char *host_end = slash ? slash : start + strlen(start);

    /* extract host:port */
    int hlen = (int)(host_end - start);
    char hbuf[512];
    if (hlen >= (int)sizeof(hbuf)) hlen = (int)sizeof(hbuf) - 1;
    memcpy(hbuf, start, hlen);
    hbuf[hlen] = '\0';

    const char *colon = strchr(hbuf, ':');
    if (colon) {
        int nlen = (int)(colon - hbuf);
        if (nlen >= hostlen) nlen = hostlen - 1;
        memcpy(host, hbuf, nlen);
        host[nlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        if (hlen >= hostlen) hlen = hostlen - 1;
        memcpy(host, hbuf, hlen);
        host[hlen] = '\0';
    }

    if (slash) {
        int plen = (int)strlen(slash);
        if (plen >= pathlen) plen = pathlen - 1;
        memcpy(path, slash, plen);
        path[plen] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* Connect to host:port via TCP. Returns fd or -1. */
static int http_connect(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Dynamic buffer for reading response */
typedef struct { char *data; size_t len; size_t cap; } HttpBuf;

static void httpbuf_init(HttpBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void httpbuf_append(HttpBuf *b, const char *src, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t newcap = (b->cap == 0) ? 4096 : b->cap * 2;
        while (newcap < b->len + n + 1) newcap *= 2;
        b->data = realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void httpbuf_free(HttpBuf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* Read entire response from fd into buf */
static void http_read_all(int fd, HttpBuf *buf) {
    char tmp[4096];
    ssize_t nr;
    while ((nr = read(fd, tmp, sizeof tmp)) > 0)
        httpbuf_append(buf, tmp, (size_t)nr);
}

/* Parse HTTP response and return XS map: #{ status, headers, body } */
static Value *http_parse_response(HttpBuf *buf) {
    Value *result = xs_map_new();
    if (!buf->data || buf->len == 0) {
        Value *sv = xs_int(0); map_set(result->map, "status", sv); value_decref(sv);
        Value *hv = xs_map_new(); map_set(result->map, "headers", hv); value_decref(hv);
        Value *bv = xs_str(""); map_set(result->map, "body", bv); value_decref(bv);
        return result;
    }

    /* find end of status line */
    char *p = buf->data;
    char *end = buf->data + buf->len;
    char *line_end = strstr(p, "\r\n");
    if (!line_end) line_end = strstr(p, "\n");
    if (!line_end) {
        Value *sv = xs_int(0); map_set(result->map, "status", sv); value_decref(sv);
        Value *hv = xs_map_new(); map_set(result->map, "headers", hv); value_decref(hv);
        Value *bv = xs_str(""); map_set(result->map, "body", bv); value_decref(bv);
        return result;
    }

    /* parse status code from "HTTP/1.x NNN ..." */
    int status = 0;
    {
        char *sp = strchr(p, ' ');
        if (sp && sp < line_end) status = atoi(sp + 1);
    }
    Value *sv = xs_int(status); map_set(result->map, "status", sv); value_decref(sv);

    /* advance past status line */
    p = line_end;
    if (*p == '\r') p++;
    if (*p == '\n') p++;

    /* parse headers */
    Value *headers = xs_map_new();
    int chunked = 0;
    long content_length = -1;
    while (p < end) {
        /* blank line = end of headers */
        if (*p == '\r' || *p == '\n') {
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            break;
        }
        char *hline_end = strstr(p, "\r\n");
        if (!hline_end) hline_end = strstr(p, "\n");
        if (!hline_end) hline_end = end;

        char *colon = memchr(p, ':', (size_t)(hline_end - p));
        if (colon) {
            int klen = (int)(colon - p);
            char key[256];
            if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
            memcpy(key, p, klen);
            key[klen] = '\0';
            /* lowercase the key for easier matching */
            char lkey[256];
            for (int i = 0; i <= klen; i++) lkey[i] = (char)tolower((unsigned char)key[i]);

            const char *val = colon + 1;
            while (val < hline_end && *val == ' ') val++;
            int vlen = (int)(hline_end - val);

            Value *vv = xs_str_n(val, vlen);
            map_set(headers->map, key, vv);
            value_decref(vv);

            /* detect chunked / content-length */
            if (strcmp(lkey, "transfer-encoding") == 0) {
                char vbuf[128];
                int cl = vlen < (int)sizeof(vbuf) - 1 ? vlen : (int)sizeof(vbuf) - 1;
                memcpy(vbuf, val, cl); vbuf[cl] = '\0';
                for (int i = 0; vbuf[i]; i++) vbuf[i] = (char)tolower((unsigned char)vbuf[i]);
                if (strstr(vbuf, "chunked")) chunked = 1;
            } else if (strcmp(lkey, "content-length") == 0) {
                content_length = atol(val);
            }
        }
        p = hline_end;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    map_set(result->map, "headers", headers);
    value_decref(headers);

    /* extract body */
    size_t body_offset = (size_t)(p - buf->data);
    size_t body_avail = (body_offset < buf->len) ? buf->len - body_offset : 0;

    if (chunked) {
        /* decode chunked transfer encoding */
        HttpBuf decoded;
        httpbuf_init(&decoded);
        char *cp = p;
        while (cp < end) {
            /* read chunk size (hex) */
            char *chunk_end = strstr(cp, "\r\n");
            if (!chunk_end) break;
            long chunk_size = strtol(cp, NULL, 16);
            if (chunk_size <= 0) break;
            cp = chunk_end + 2; /* skip \r\n after size */
            if (cp + chunk_size > end) chunk_size = (long)(end - cp);
            httpbuf_append(&decoded, cp, (size_t)chunk_size);
            cp += chunk_size;
            if (cp + 2 <= end && cp[0] == '\r' && cp[1] == '\n') cp += 2;
        }
        Value *bv = xs_str_n(decoded.data ? decoded.data : "", (int)decoded.len);
        map_set(result->map, "body", bv);
        value_decref(bv);
        httpbuf_free(&decoded);
    } else if (content_length >= 0) {
        size_t blen = (size_t)content_length;
        if (blen > body_avail) blen = body_avail;
        Value *bv = xs_str_n(p, (int)blen);
        map_set(result->map, "body", bv);
        value_decref(bv);
    } else {
        /* read until close */
        Value *bv = xs_str_n(p, (int)body_avail);
        map_set(result->map, "body", bv);
        value_decref(bv);
    }

    return result;
}

/* Core: perform an HTTP request. Returns XS map. */
static Value *http_do_request(const char *method, const char *url,
                              XSMap *extra_headers, const char *body,
                              size_t body_len) {
    char host[512], path[2048];
    int port;
    if (http_parse_url(url, host, sizeof host, &port, path, sizeof path) < 0)
        return value_incref(XS_NULL_VAL);

    int use_tls = (strncmp(url, "https://", 8) == 0);
    int fd = http_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "error: could not connect to %s:%d\n", host, port);
        return value_incref(XS_NULL_VAL);
    }

    xs_tls_conn *tls = NULL;
    if (use_tls) {
        tls = xs_tls_connect(fd, host);
        if (!tls) {
            fprintf(stderr, "error: TLS handshake failed for %s\n", host);
            close(fd);
            return value_incref(XS_NULL_VAL);
        }
    }

    /* build request */
    HttpBuf req;
    httpbuf_init(&req);

    /* request line */
    httpbuf_append(&req, method, strlen(method));
    httpbuf_append(&req, " ", 1);
    httpbuf_append(&req, path, strlen(path));
    httpbuf_append(&req, " HTTP/1.1\r\n", 11);

    /* Host header */
    httpbuf_append(&req, "Host: ", 6);
    httpbuf_append(&req, host, strlen(host));
    if (port != 80) {
        char pbuf[16];
        snprintf(pbuf, sizeof pbuf, ":%d", port);
        httpbuf_append(&req, pbuf, strlen(pbuf));
    }
    httpbuf_append(&req, "\r\n", 2);

    /* Connection: close */
    httpbuf_append(&req, "Connection: close\r\n", 19);

    /* Content-Length if body present */
    if (body && body_len > 0) {
        char clbuf[64];
        snprintf(clbuf, sizeof clbuf, "Content-Length: %zu\r\n", body_len);
        httpbuf_append(&req, clbuf, strlen(clbuf));
    }

    /* extra headers from map */
    if (extra_headers) {
        for (int i = 0; i < extra_headers->cap; i++) {
            if (extra_headers->keys[i] && extra_headers->vals[i]) {
                const char *k = extra_headers->keys[i];
                Value *v = extra_headers->vals[i];
                if (VAL_TAG(v) == XS_STR) {
                    httpbuf_append(&req, k, strlen(k));
                    httpbuf_append(&req, ": ", 2);
                    httpbuf_append(&req, v->s, strlen(v->s));
                    httpbuf_append(&req, "\r\n", 2);
                }
            }
        }
    }

    /* end of headers */
    httpbuf_append(&req, "\r\n", 2);

    /* send request */
    if (tls) {
        if (xs_tls_write(tls, req.data, (int)req.len) < 0) {
            xs_tls_close(tls); httpbuf_free(&req); return value_incref(XS_NULL_VAL);
        }
    } else {
        size_t sent = 0;
        while (sent < req.len) {
            ssize_t w = write(fd, req.data + sent, req.len - sent);
            if (w <= 0) { close(fd); httpbuf_free(&req); return value_incref(XS_NULL_VAL); }
            sent += (size_t)w;
        }
    }
    httpbuf_free(&req);

    /* send body */
    if (body && body_len > 0) {
        if (tls) {
            if (xs_tls_write(tls, body, (int)body_len) < 0) {
                xs_tls_close(tls); return value_incref(XS_NULL_VAL);
            }
        } else {
            size_t sent = 0;
            while (sent < body_len) {
                ssize_t w = write(fd, body + sent, body_len - sent);
                if (w <= 0) { close(fd); return value_incref(XS_NULL_VAL); }
                sent += (size_t)w;
            }
        }
    }

    /* read response */
    HttpBuf resp;
    httpbuf_init(&resp);
    if (tls) {
        char tmp[4096];
        int nr;
        while ((nr = xs_tls_read(tls, tmp, sizeof tmp)) > 0)
            httpbuf_append(&resp, tmp, (size_t)nr);
        xs_tls_close(tls);
    } else {
        http_read_all(fd, &resp);
        close(fd);
    }

    Value *result = http_parse_response(&resp);
    httpbuf_free(&resp);
    return result;
}

#endif /* __MINGW32__ */

/* net.http_get(url) */
static Value *native_net_http_get(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    return http_do_request("GET", a[0]->s, NULL, NULL, 0);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.http_post(url, body, content_type) */
static Value *native_net_http_post(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 3 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR || VAL_TAG(a[2]) != XS_STR)
        return value_incref(XS_NULL_VAL);

    /* build a temporary headers map with Content-Type */
    XSMap *hdrs = map_new();
    Value *ct = xs_str(a[2]->s);
    map_set(hdrs, "Content-Type", ct);
    value_decref(ct);

    Value *result = http_do_request("POST", a[0]->s, hdrs, a[1]->s, strlen(a[1]->s));
    map_free(hdrs);
    return result;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.http(method, url, headers_map, body) */
static Value *native_net_http(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);

    XSMap *hdrs = NULL;
    if (n >= 3 && VAL_TAG(a[2]) == XS_MAP) hdrs = a[2]->map;
    const char *body = NULL;
    size_t body_len = 0;
    if (n >= 4 && VAL_TAG(a[3]) == XS_STR) {
        body = a[3]->s;
        body_len = strlen(a[3]->s);
    }

    return http_do_request(a[0]->s, a[1]->s, hdrs, body, body_len);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.udp_bind(port) */
static Value *native_net_udp_bind(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return value_incref(XS_NULL_VAL);
    int port = (int)VAL_INT(a[0]);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return value_incref(XS_NULL_VAL);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return value_incref(XS_NULL_VAL);
    }
    XSMap *m = map_new();
    map_take(m, "fd", xs_int(fd));
    return xs_module(m);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.udp_send(sock, host, port, data) */
static Value *native_net_udp_send(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 4) return value_incref(XS_FALSE_VAL);
    if ((VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR
        || VAL_TAG(a[2]) != XS_INT || VAL_TAG(a[3]) != XS_STR)
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)VAL_INT(a[2]));
    inet_pton(AF_INET, a[1]->s, &dest.sin_addr);
    ssize_t sent = sendto(fd, a[3]->s, strlen(a[3]->s), 0,
                          (struct sockaddr*)&dest, sizeof(dest));
    return (sent >= 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.udp_recv(sock, max) */
static Value *native_net_udp_recv(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    int fd = (int)VAL_INT(fdv);
    int maxsz = 65536;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxsz = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxsz + 1);
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t nr = recvfrom(fd, buf, maxsz, 0, (struct sockaddr*)&from, &fromlen);
    if (nr < 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *result = xs_map_new();
    Value *dv = xs_str_n(buf, nr); map_set(result->map, "data", dv); value_decref(dv);
    free(buf);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    Value *hv = xs_str(ip); map_set(result->map, "host", hv); value_decref(hv);
    Value *pv = xs_int(ntohs(from.sin_port)); map_set(result->map, "port", pv); value_decref(pv);
    return result;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* net.set_timeout(conn, ms) */
static Value *native_net_set_timeout(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_INT)
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    int ms = (int)VAL_INT(a[1]);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.set_nodelay(conn, bool) */
static Value *native_net_set_nodelay(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    int fd = (int)VAL_INT(fdv);
    int flag = value_truthy(a[1]) ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.close(conn) */
static Value *native_net_close(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    close((int)VAL_INT(fdv));
    map_take(a[0]->map, "fd", xs_int(-1));
    return value_incref(XS_TRUE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* net.send(conn, data) / net.recv(conn, max) */
static Value *native_net_send(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR)
        return xs_int(-1);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return xs_int(-1);
    ssize_t w = write((int)VAL_INT(fdv), a[1]->s, strlen(a[1]->s));
    return xs_int((int64_t)w);
#else
    (void)a; (void)n;
    return xs_int(-1);
#endif
}

static Value *native_net_recv(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    int maxsz = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) maxsz = (int)VAL_INT(a[1]);
    char *buf = xs_malloc(maxsz + 1);
    ssize_t nr = read((int)VAL_INT(fdv), buf, maxsz);
    if (nr <= 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

Value *make_net_module(void) {
    XSMap *m = map_new();
    map_take(m, "tcp_connect", xs_native(native_net_tcp_connect));
    map_take(m, "tcp_listen",  xs_native(native_net_tcp_listen));
    map_take(m, "resolve",     xs_native(native_net_resolve));
    map_take(m, "url_parse",   xs_native(native_net_url_parse));
    map_take(m, "http_get",    xs_native(native_net_http_get));
    map_take(m, "http_post",   xs_native(native_net_http_post));
    map_take(m, "http",        xs_native(native_net_http));
    map_take(m, "udp_bind",    xs_native(native_net_udp_bind));
    map_take(m, "udp_send",    xs_native(native_net_udp_send));
    map_take(m, "udp_recv",    xs_native(native_net_udp_recv));
    map_take(m, "set_timeout", xs_native(native_net_set_timeout));
    map_take(m, "set_nodelay", xs_native(native_net_set_nodelay));
    map_take(m, "close",       xs_native(native_net_close));
    map_take(m, "send",        xs_native(native_net_send));
    map_take(m, "recv",        xs_native(native_net_recv));
    return xs_module(m);
}

/* crypto */

/* SHA-256 implementation */
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t sha256_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]<<24) | ((uint32_t)block[i*4+1]<<16) |
               ((uint32_t)block[i*4+2]<<8) | (uint32_t)block[i*4+3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i-15],7) ^ sha256_rotr(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = sha256_rotr(w[i-2],17) ^ sha256_rotr(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=state[0]; b=state[1]; c=state[2]; d=state[3];
    e=state[4]; f=state[5]; g=state[6]; h=state[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

void xs_sha256_hash(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t block[64];
    size_t i;
    /* process full blocks */
    for (i = 0; i + 64 <= len; i += 64)
        sha256_transform(state, data + i);
    /* final block with padding */
    size_t rem = len - i;
    memset(block, 0, 64);
    if (rem > 0) memcpy(block, data + i, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        sha256_transform(state, block);
        memset(block, 0, 64);
    }
    /* length in bits (big-endian) */
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 7; j >= 0; j--) {
        block[56 + (7 - j)] = (uint8_t)(bits >> (j * 8));
    }
    sha256_transform(state, block);
    /* output */
    for (i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(state[i]>>24);
        out[i*4+1] = (uint8_t)(state[i]>>16);
        out[i*4+2] = (uint8_t)(state[i]>>8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
}

/* MD5 implementation */
static const uint32_t md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
static const uint32_t md5_k_tab[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static uint32_t md5_leftrotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

void xs_md5_hash(const uint8_t *data, size_t len, uint8_t out[16]) {
    uint32_t a0=0x67452301, b0=0xefcdab89, c0=0x98badcfe, d0=0x10325476;
    /* pad message */
    size_t new_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = xs_calloc(new_len, 1);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    memcpy(msg + new_len - 8, &bits, 8); /* little-endian */

    for (size_t off = 0; off < new_len; off += 64) {
        uint32_t *M = (uint32_t*)(msg + off);
        uint32_t A=a0, B=b0, C=c0, D=d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F, g;
            if (i < 16)      { F = (B&C)|(~B&D); g = (uint32_t)i; }
            else if (i < 32) { F = (D&B)|(~D&C); g = (5*(uint32_t)i+1)%16; }
            else if (i < 48) { F = B^C^D;        g = (3*(uint32_t)i+5)%16; }
            else              { F = C^(B|~D);     g = (7*(uint32_t)i)%16; }
            F = F + A + md5_k_tab[i] + M[g];
            A = D; D = C; C = B;
            B = B + md5_leftrotate(F, md5_s[i]);
        }
        a0+=A; b0+=B; c0+=C; d0+=D;
    }
    free(msg);
    memcpy(out+0,  &a0, 4);
    memcpy(out+4,  &b0, 4);
    memcpy(out+8,  &c0, 4);
    memcpy(out+12, &d0, 4);
}

static Value *native_crypto_sha256(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    uint8_t hash[32];
    xs_sha256_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[64] = '\0';
    return xs_str(hex);
}

static Value *native_crypto_md5(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    uint8_t hash[16];
    xs_md5_hash((const uint8_t*)a[0]->s, strlen(a[0]->s), hash);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[32] = '\0';
    return xs_str(hex);
}

static Value *native_crypto_random_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_INT) return xs_str("");
    int count = (int)VAL_INT(a[0]);
    if (count <= 0 || count > 65536) return xs_str("");
    uint8_t *buf = xs_malloc((size_t)count);
#if defined(__wasi__)
    for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff);
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(buf, 1, (size_t)count, f) < (size_t)count) { /* partial read ok */ } fclose(f); }
    else { for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff); }
#else
    /* Windows: use RtlGenRandom (SystemFunction036) from advapi32 */
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(buf, (ULONG)count)) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            for (int i=0;i<count;i++) buf[i]=(uint8_t)(rand()&0xff);
        }
    }
#endif
    /* return as hex string */
    char *hex = xs_malloc((size_t)count * 2 + 1);
    for (int i = 0; i < count; i++) sprintf(hex + i*2, "%02x", buf[i]);
    hex[count*2] = '\0';
    free(buf);
    Value *r = xs_str(hex);
    free(hex);
    return r;
}

static Value *native_crypto_random_int(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return xs_int(0);
    int64_t lo = (VAL_TAG(a[0]) == XS_INT) ? VAL_INT(a[0]) : 0;
    int64_t hi = (VAL_TAG(a[1]) == XS_INT) ? VAL_INT(a[1]) : 0;
    if (hi <= lo) return xs_int(lo);
    uint64_t r;
#if defined(__wasi__)
    r = (uint64_t)rand();
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&r, sizeof r, 1, f) < 1) { r = (uint64_t)rand(); } fclose(f); }
    else { r = (uint64_t)rand(); }
#else
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(&r, (ULONG)sizeof(r))) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            r = (uint64_t)rand();
        }
    }
#endif
    return xs_int(lo + (int64_t)(r % (uint64_t)(hi - lo)));
}

static Value *native_crypto_uuid4(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    uint8_t bytes[16];
#if defined(__wasi__)
    for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff);
#elif !defined(__MINGW32__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(bytes, 1, 16, f) < 16) { /* partial read ok */ } fclose(f); }
    else { for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff); }
#else
    {
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        int filled = 0;
        if (advapi) {
            RtlGenRandomFn RtlGenRandom = (RtlGenRandomFn)(void *)GetProcAddress(advapi, "SystemFunction036");
            if (RtlGenRandom && RtlGenRandom(bytes, 16)) filled = 1;
            FreeLibrary(advapi);
        }
        if (!filled) {
            srand((unsigned)(time(NULL) ^ GetCurrentProcessId() ^ GetTickCount()));
            for (int i=0;i<16;i++) bytes[i]=(uint8_t)(rand()&0xff);
        }
    }
#endif
    bytes[6] = (bytes[6] & 0x0f) | 0x40; /* version 4 */
    bytes[8] = (bytes[8] & 0x3f) | 0x80; /* variant 1 */
    char uuid[37];
    snprintf(uuid, sizeof uuid,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],
        bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],
        bytes[12],bytes[13],bytes[14],bytes[15]);
    return xs_str(uuid);
}

static Value *native_crypto_hash(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_int(0);
    /* djb2 hash */
    const char *s = a[0]->s;
    uint64_t h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return xs_int((int64_t)h);
}

static Value *native_crypto_hex_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    int slen = (int)strlen(s);
    char *hex = xs_malloc(slen * 2 + 1);
    for (int i = 0; i < slen; i++) sprintf(hex + i*2, "%02x", (unsigned char)s[i]);
    hex[slen * 2] = '\0';
    Value *r = xs_str(hex); free(hex); return r;
}

static Value *native_crypto_hex_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    int slen = (int)strlen(s);
    if (slen % 2 != 0) return xs_str("");
    int olen = slen / 2;
    char *out = xs_malloc(olen + 1);
    for (int i = 0; i < olen; i++) {
        unsigned int byte;
        if (sscanf(s + i*2, "%2x", &byte) != 1) { free(out); return xs_str(""); }
        out[i] = (char)byte;
    }
    out[olen] = '\0';
    Value *r = xs_str(out); free(out); return r;
}

static const char xs_b64_tbl[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static Value *native_crypto_base64_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const unsigned char *s = (const unsigned char*)a[0]->s;
    int slen = (int)strlen(a[0]->s);
    int rlen = ((slen + 2) / 3) * 4;
    char *r = xs_malloc(rlen + 1); int ri = 0;
    for (int j = 0; j < slen; j += 3) {
        unsigned int v = (unsigned)s[j]<<16 | (j+1<slen?(unsigned)s[j+1]:0)<<8 | (j+2<slen?(unsigned)s[j+2]:0);
        r[ri++] = xs_b64_tbl[(v>>18)&63]; r[ri++] = xs_b64_tbl[(v>>12)&63];
        r[ri++] = (j+1<slen) ? xs_b64_tbl[(v>>6)&63] : '=';
        r[ri++] = (j+2<slen) ? xs_b64_tbl[v&63] : '=';
    }
    r[ri] = '\0'; Value *v2 = xs_str(r); free(r); return v2;
}

static Value *native_crypto_base64_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s; int slen = (int)strlen(s);
    static const signed char inv[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    char *r = xs_malloc(slen); int ri = 0;
    for (int j = 0; j+3 < slen; j += 4) {
        int a0=inv[(unsigned char)s[j]], a1=inv[(unsigned char)s[j+1]];
        int a2=inv[(unsigned char)s[j+2]], a3=inv[(unsigned char)s[j+3]];
        if (a0<0||a1<0) break;
        r[ri++] = (char)((a0<<2)|(a1>>4));
        if (a2>=0) r[ri++] = (char)((a1<<4)|(a2>>2));
        if (a3>=0) r[ri++] = (char)((a2<<6)|a3);
    }
    r[ri] = '\0'; Value *v2 = xs_str(r); free(r); return v2;
}

#ifndef __wasi__
/* crypto.sha1(data) -> hex string using BearSSL */
static Value *native_crypto_sha1(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    br_sha1_context ctx;
    br_sha1_init(&ctx);
    br_sha1_update(&ctx, a[0]->s, strlen(a[0]->s));
    uint8_t hash[20];
    br_sha1_out(&ctx, hash);
    char hex[41];
    for (int i = 0; i < 20; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[40] = '\0';
    return xs_str(hex);
}

/* crypto.hmac_sha256(key, data) -> hex string using BearSSL */
static Value *native_crypto_hmac_sha256(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, a[0]->s, strlen(a[0]->s));
    br_hmac_context hctx;
    br_hmac_init(&hctx, &kc, 0);
    br_hmac_update(&hctx, a[1]->s, strlen(a[1]->s));
    uint8_t mac[32];
    br_hmac_out(&hctx, mac);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", mac[i]);
    hex[64] = '\0';
    return xs_str(hex);
}

/* crypto.hkdf(ikm, salt, info, length) -> hex string using BearSSL */
static Value *native_crypto_hkdf(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 4 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[3]) != XS_INT) return xs_str("");
    int out_len = (int)VAL_INT(a[3]);
    if (out_len <= 0 || out_len > 255 * 32) return xs_str("");
    const void *salt = BR_HKDF_NO_SALT;
    size_t salt_len = 0;
    if (VAL_TAG(a[1]) == XS_STR && strlen(a[1]->s) > 0) {
        salt = a[1]->s; salt_len = strlen(a[1]->s);
    }
    br_hkdf_context hc;
    br_hkdf_init(&hc, &br_sha256_vtable, salt, salt_len);
    br_hkdf_inject(&hc, a[0]->s, strlen(a[0]->s));
    br_hkdf_flip(&hc);
    const void *info = "";
    size_t info_len = 0;
    if (n >= 3 && VAL_TAG(a[2]) == XS_STR) { info = a[2]->s; info_len = strlen(a[2]->s); }
    uint8_t *out = xs_malloc(out_len);
    br_hkdf_produce(&hc, info, info_len, out, out_len);
    char *hex = xs_malloc(out_len * 2 + 1);
    for (int i = 0; i < out_len; i++) sprintf(hex + i*2, "%02x", out[i]);
    hex[out_len * 2] = '\0';
    free(out);
    Value *r = xs_str(hex); free(hex); return r;
}

/* crypto.pbkdf2(password, salt, iterations, key_len) -> hex string
   PBKDF2-HMAC-SHA256, hand-rolled using BearSSL HMAC primitives */
static Value *native_crypto_pbkdf2(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 4 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR
        || VAL_TAG(a[2]) != XS_INT || VAL_TAG(a[3]) != XS_INT) return xs_str("");
    const char *pw = a[0]->s;
    size_t pw_len = strlen(pw);
    const char *salt = a[1]->s;
    size_t salt_len = strlen(salt);
    int iters = (int)VAL_INT(a[2]);
    int dklen = (int)VAL_INT(a[3]);
    if (iters <= 0 || dklen <= 0 || dklen > 1024) return xs_str("");

    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, pw_len);

    uint8_t *dk = xs_calloc(dklen, 1);
    int blocks = (dklen + 31) / 32;
    for (int blk = 1; blk <= blocks; blk++) {
        /* U1 = HMAC(pw, salt || INT_BE(blk)) */
        uint8_t salt_blk[4];
        salt_blk[0] = (uint8_t)(blk >> 24);
        salt_blk[1] = (uint8_t)(blk >> 16);
        salt_blk[2] = (uint8_t)(blk >> 8);
        salt_blk[3] = (uint8_t)(blk);

        br_hmac_context hctx;
        br_hmac_init(&hctx, &kc, 0);
        br_hmac_update(&hctx, salt, salt_len);
        br_hmac_update(&hctx, salt_blk, 4);
        uint8_t u[32], t[32];
        br_hmac_out(&hctx, u);
        memcpy(t, u, 32);

        for (int i = 1; i < iters; i++) {
            br_hmac_init(&hctx, &kc, 0);
            br_hmac_update(&hctx, u, 32);
            br_hmac_out(&hctx, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }

        int off = (blk - 1) * 32;
        int cp = dklen - off;
        if (cp > 32) cp = 32;
        memcpy(dk + off, t, cp);
    }

    char *hex = xs_malloc(dklen * 2 + 1);
    for (int i = 0; i < dklen; i++) sprintf(hex + i*2, "%02x", dk[i]);
    hex[dklen * 2] = '\0';
    free(dk);
    Value *r = xs_str(hex); free(hex); return r;
}

/* crypto.aes_encrypt(key_hex, plaintext, mode) -> ciphertext hex
   Supports "cbc" and "gcm". Key must be hex-encoded (32/48/64 chars for 128/192/256). */
static int hex_decode_bytes(const char *hex, uint8_t *out, int max) {
    int len = (int)strlen(hex);
    if (len % 2 != 0) return -1;
    int n2 = len / 2;
    if (n2 > max) n2 = max;
    for (int i = 0; i < n2; i++) {
        unsigned int b;
        if (sscanf(hex + i*2, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return n2;
}

static Value *native_crypto_aes_encrypt(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    const char *mode_str = (n >= 3 && VAL_TAG(a[2]) == XS_STR) ? a[2]->s : "gcm";

    uint8_t key[32];
    int klen = hex_decode_bytes(a[0]->s, key, 32);
    if (klen != 16 && klen != 24 && klen != 32) return xs_str("");

    const uint8_t *pt = (const uint8_t*)a[1]->s;
    size_t pt_len = strlen(a[1]->s);

    if (strcmp(mode_str, "cbc") == 0) {
        /* PKCS7 padding */
        int pad = 16 - (int)(pt_len % 16);
        size_t padded_len = pt_len + (size_t)pad;
        uint8_t *buf = xs_malloc(padded_len);
        memcpy(buf, pt, pt_len);
        for (int i = 0; i < pad; i++) buf[pt_len + i] = (uint8_t)pad;

        /* random IV */
        uint8_t iv[16];
        FILE *rng = fopen("/dev/urandom", "rb");
        if (rng) { if (fread(iv, 1, 16, rng) < 16) {} fclose(rng); }
        else { for (int i=0;i<16;i++) iv[i]=(uint8_t)(rand()&0xff); }

        br_aes_ct_cbcenc_keys ctx;
        br_aes_ct_cbcenc_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcenc_run(&ctx, iv, buf, padded_len);

        /* output: IV || ciphertext, hex encoded */
        size_t out_len = 16 + padded_len;
        char *hex = xs_malloc(out_len * 2 + 1);
        uint8_t iv_save[16];
        /* we need original IV, but it was modified. regenerate */
        /* actually, openssl CBC mode modifies IV in place, so we need to save it.
           let's re-encrypt properly */
        free(buf); free(hex);

        /* redo: save IV first */
        uint8_t iv2[16];
        if (rng) { rng = fopen("/dev/urandom","rb"); if(rng){if(fread(iv2,1,16,rng)<16){}fclose(rng);} }
        else { for (int i=0;i<16;i++) iv2[i]=(uint8_t)(rand()&0xff); }
        memcpy(iv_save, iv2, 16);

        buf = xs_malloc(padded_len);
        memcpy(buf, pt, pt_len);
        for (int i = 0; i < pad; i++) buf[pt_len + i] = (uint8_t)pad;

        br_aes_ct_cbcenc_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcenc_run(&ctx, iv2, buf, padded_len);

        out_len = 16 + padded_len;
        hex = xs_malloc(out_len * 2 + 1);
        for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", iv_save[i]);
        for (size_t i = 0; i < padded_len; i++) sprintf(hex + 32 + i*2, "%02x", buf[i]);
        hex[out_len * 2] = '\0';
        free(buf);
        Value *r = xs_str(hex); free(hex); return r;
    } else {
        /* GCM mode */
        uint8_t iv[12];
        FILE *rng = fopen("/dev/urandom", "rb");
        if (rng) { if (fread(iv, 1, 12, rng) < 12) {} fclose(rng); }
        else { for (int i=0;i<12;i++) iv[i]=(uint8_t)(rand()&0xff); }

        uint8_t *buf = xs_malloc(pt_len > 0 ? pt_len : 1);
        memcpy(buf, pt, pt_len);

        br_aes_ct_ctr_keys ctr_keys;
        br_aes_ct_ctr_init(&ctr_keys, key, (size_t)klen);
        br_gcm_context gc;
        br_gcm_init(&gc, &ctr_keys.vtable, br_ghash_ctmul);
        br_gcm_reset(&gc, iv, 12);
        br_gcm_flip(&gc);
        br_gcm_run(&gc, 1, buf, pt_len);
        uint8_t tag[16];
        br_gcm_get_tag(&gc, tag);

        /* output: IV(12) || ciphertext || tag(16), hex encoded */
        size_t out_len = 12 + pt_len + 16;
        char *hex = xs_malloc(out_len * 2 + 1);
        for (int i = 0; i < 12; i++) sprintf(hex + i*2, "%02x", iv[i]);
        for (size_t i = 0; i < pt_len; i++) sprintf(hex + 24 + i*2, "%02x", buf[i]);
        for (int i = 0; i < 16; i++) sprintf(hex + 24 + pt_len*2 + i*2, "%02x", tag[i]);
        hex[out_len * 2] = '\0';
        free(buf);
        Value *r = xs_str(hex); free(hex); return r;
    }
}

static Value *native_crypto_aes_decrypt(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return xs_str("");
    const char *mode_str = (n >= 3 && VAL_TAG(a[2]) == XS_STR) ? a[2]->s : "gcm";

    uint8_t key[32];
    int klen = hex_decode_bytes(a[0]->s, key, 32);
    if (klen != 16 && klen != 24 && klen != 32) return xs_str("");

    int ct_hex_len = (int)strlen(a[1]->s);
    if (ct_hex_len % 2 != 0) return xs_str("");
    int ct_bytes = ct_hex_len / 2;

    if (strcmp(mode_str, "cbc") == 0) {
        if (ct_bytes < 32) return xs_str(""); /* need at least IV + 1 block */
        uint8_t *raw = xs_malloc(ct_bytes);
        if (hex_decode_bytes(a[1]->s, raw, ct_bytes) != ct_bytes) { free(raw); return xs_str(""); }

        uint8_t iv[16];
        memcpy(iv, raw, 16);
        uint8_t *ct = raw + 16;
        int ct_len = ct_bytes - 16;

        br_aes_ct_cbcdec_keys ctx;
        br_aes_ct_cbcdec_init(&ctx, key, (size_t)klen);
        br_aes_ct_cbcdec_run(&ctx, iv, ct, (size_t)ct_len);

        /* remove PKCS7 padding */
        int pad = ct[ct_len - 1];
        if (pad < 1 || pad > 16) { free(raw); return xs_str(""); }
        ct_len -= pad;
        Value *r = xs_str_n((const char*)ct, ct_len);
        free(raw);
        return r;
    } else {
        /* GCM: IV(12) || ciphertext || tag(16) */
        if (ct_bytes < 28) return xs_str(""); /* 12 + 0 + 16 minimum */
        uint8_t *raw = xs_malloc(ct_bytes);
        if (hex_decode_bytes(a[1]->s, raw, ct_bytes) != ct_bytes) { free(raw); return xs_str(""); }

        uint8_t iv[12];
        memcpy(iv, raw, 12);
        int data_len = ct_bytes - 12 - 16;
        if (data_len < 0) { free(raw); return xs_str(""); }
        uint8_t *ct = raw + 12;
        uint8_t *tag = raw + 12 + data_len;

        br_aes_ct_ctr_keys ctr_keys;
        br_aes_ct_ctr_init(&ctr_keys, key, (size_t)klen);
        br_gcm_context gc;
        br_gcm_init(&gc, &ctr_keys.vtable, br_ghash_ctmul);
        br_gcm_reset(&gc, iv, 12);
        br_gcm_flip(&gc);
        br_gcm_run(&gc, 0, ct, (size_t)data_len);

        if (br_gcm_check_tag(&gc, tag) != 1) {
            free(raw); return xs_str("");
        }
        Value *r = xs_str_n((const char*)ct, data_len);
        free(raw);
        return r;
    }
}

#endif /* __wasi__ */

/* crypto.constant_time_eq(a, b) -> bool */
static Value *native_crypto_constant_time_eq(Interp *ig, Value **a2, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a2[0]) != XS_STR || VAL_TAG(a2[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    size_t la = strlen(a2[0]->s), lb = strlen(a2[1]->s);
    if (la != lb) return value_incref(XS_FALSE_VAL);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (uint8_t)((unsigned char)a2[0]->s[i] ^ (unsigned char)a2[1]->s[i]);
    return (diff == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *make_crypto_module(void) {
    XSMap *m = map_new();
    map_take(m, "sha256",           xs_native(native_crypto_sha256));
#ifndef __wasi__
    map_take(m, "sha1",             xs_native(native_crypto_sha1));
    map_take(m, "hmac_sha256",      xs_native(native_crypto_hmac_sha256));
    map_take(m, "hkdf",             xs_native(native_crypto_hkdf));
    map_take(m, "pbkdf2",           xs_native(native_crypto_pbkdf2));
    map_take(m, "aes_encrypt",      xs_native(native_crypto_aes_encrypt));
    map_take(m, "aes_decrypt",      xs_native(native_crypto_aes_decrypt));
#endif
    map_take(m, "md5",              xs_native(native_crypto_md5));
    map_take(m, "hash",             xs_native(native_crypto_hash));
    map_take(m, "hex_encode",       xs_native(native_crypto_hex_encode));
    map_take(m, "hex_decode",       xs_native(native_crypto_hex_decode));
    map_take(m, "base64_encode",    xs_native(native_crypto_base64_encode));
    map_take(m, "base64_decode",    xs_native(native_crypto_base64_decode));
    map_take(m, "random_bytes",     xs_native(native_crypto_random_bytes));
    map_take(m, "random_int",       xs_native(native_crypto_random_int));
    map_take(m, "uuid4",            xs_native(native_crypto_uuid4));
    map_take(m, "constant_time_eq", xs_native(native_crypto_constant_time_eq));
    return xs_module(m);
}

/* threads */
#include "core/xs_thread.h"

typedef struct {
    Interp *interp;  /* parent interp (used read-only for call_value) */
    Value  *fn;      /* function to call: incref'd before thread start */
    Value  *result;  /* output: set by the thread */
} ThreadArg;

static void *thread_entry(void *arg) {
    ThreadArg *ta = (ThreadArg *)arg;
    /* Call the XS function in isolation.
       Since XS values are not thread-safe we keep it simple:
       the function receives no arguments and its return value is
       stored for later retrieval via thread.join(). */
    ta->result = call_value(ta->interp, ta->fn, NULL, 0, "thread.spawn");
    return NULL;
}

/* forward declaration so the non-POSIX stub in spawn can reference join */
static Value *native_thread_join(Interp *ig, Value **a, int n);

static Value *native_thread_spawn(Interp *ig, Value **a, int n) {
    if (n < 1 || (VAL_TAG(a[0]) != XS_FUNC && VAL_TAG(a[0]) != XS_NATIVE))
        return xs_str("error: thread.spawn requires a callable");

    ThreadArg *ta = xs_malloc(sizeof(ThreadArg));
    ta->interp = ig;
    ta->fn     = value_incref(a[0]);
    ta->result = NULL;

    xs_thread_t tid;
    if (xs_thread_create(&tid, thread_entry, ta) != 0) {
        value_decref(ta->fn);
        free(ta);
        return xs_str("error: thread creation failed");
    }

    XSMap *handle = map_new();
    map_take(handle, "_tid", xs_int((int64_t)(uintptr_t)tid));
    map_take(handle, "_targ", xs_int((int64_t)(uintptr_t)ta));
    map_set(handle, "status", xs_str("running"));
    return xs_module(handle);
}

static Value *native_thread_join(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: thread.join requires a thread handle");
    Value *tid_v  = map_get(a[0]->map, "_tid");
    Value *targ_v = map_get(a[0]->map, "_targ");
    if (!tid_v || VAL_TAG(tid_v) != XS_INT || !targ_v || VAL_TAG(targ_v) != XS_INT)
        return xs_str("error: invalid thread handle");

    xs_thread_t tid = (xs_thread_t)(uintptr_t)VAL_INT(tid_v);
    ThreadArg *ta = (ThreadArg *)(uintptr_t)VAL_INT(targ_v);

    int err = xs_thread_join(tid, NULL);
    if (err != 0)
        return xs_str("error: thread join failed");

    Value *result = ta->result ? ta->result : value_incref(XS_NULL_VAL);
    value_decref(ta->fn);
    free(ta);

    /* Update handle status */
    map_set(a[0]->map, "status", xs_str("joined"));

    return result;
}

static Value *native_thread_id(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    return xs_int((int64_t)xs_thread_self_id());
}

static Value *native_thread_cpu_count(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
#if defined(_SC_NPROCESSORS_ONLN)
    return xs_int(sysconf(_SC_NPROCESSORS_ONLN));
#else
    return xs_int(1);
#endif
}

static Value *native_thread_sleep(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return value_incref(XS_NULL_VAL);
    double secs = 0.0;
    if (VAL_TAG(a[0]) == XS_FLOAT) secs = a[0]->f;
    else if (VAL_TAG(a[0]) == XS_INT) secs = (double)VAL_INT(a[0]);
    xs_thread_sleep_ns(secs);
    return value_incref(XS_NULL_VAL);
}

/* cross-platform mutex implementation */

static xs_mutex_t *mutex_from_map(XSMap *m) {
    Value *pv = map_get(m, "_ptr");
    if (!pv || VAL_TAG(pv) != XS_INT) return NULL;
    return (xs_mutex_t *)(uintptr_t)VAL_INT(pv);
}

static Value *native_mutex_lock_fn(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    /* 'self' is passed as the first argument by the method-call dispatch */
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    xs_mutex_t *mtx = mutex_from_map(a[0]->map);
    if (!mtx) return value_incref(XS_FALSE_VAL);
    int err = xs_mutex_lock(mtx);
    if (err == 0) map_set(a[0]->map, "locked", value_incref(XS_TRUE_VAL));
    return err == 0 ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_mutex_unlock_fn(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    xs_mutex_t *mtx = mutex_from_map(a[0]->map);
    if (!mtx) return value_incref(XS_FALSE_VAL);
    int err = xs_mutex_unlock(mtx);
    if (err == 0) map_set(a[0]->map, "locked", value_incref(XS_FALSE_VAL));
    return err == 0 ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_mutex_try_lock_fn(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_FALSE_VAL);
    xs_mutex_t *mtx = mutex_from_map(a[0]->map);
    if (!mtx) return value_incref(XS_FALSE_VAL);
    int err = xs_mutex_trylock(mtx);
    if (err == 0) {
        map_set(a[0]->map, "locked", value_incref(XS_TRUE_VAL));
        return value_incref(XS_TRUE_VAL);
    }
    return value_incref(XS_FALSE_VAL);
}

static Value *native_mutex_destroy_fn(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return value_incref(XS_NULL_VAL);
    xs_mutex_t *mtx = mutex_from_map(a[0]->map);
    if (mtx) {
        xs_mutex_destroy(mtx);
        free(mtx);
        /* Clear the pointer so double-destroy is harmless */
        map_take(a[0]->map, "_ptr", xs_int(0));
    }
    return value_incref(XS_NULL_VAL);
}

static Value *native_thread_mutex(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    xs_mutex_t *mtx = xs_malloc(sizeof(xs_mutex_t));
    if (xs_mutex_init(mtx) != 0) {
        free(mtx);
        return value_incref(XS_NULL_VAL);
    }
    XSMap *m = map_new();
    /* Store the mutex pointer as an opaque int (same pattern as XSBuf) */
    map_take(m, "_ptr", xs_int((int64_t)(uintptr_t)mtx));
    map_set(m, "locked", value_incref(XS_FALSE_VAL));
    map_take(m, "lock",    xs_native(native_mutex_lock_fn));
    map_take(m, "unlock",  xs_native(native_mutex_unlock_fn));
    map_take(m, "try_lock", xs_native(native_mutex_try_lock_fn));
    map_take(m, "destroy", xs_native(native_mutex_destroy_fn));
    return xs_module(m);
}

Value *make_thread_module(void) {
    XSMap *m = map_new();
    map_take(m, "spawn",     xs_native(native_thread_spawn));
    map_take(m, "join",      xs_native(native_thread_join));
    map_take(m, "id",        xs_native(native_thread_id));
    map_take(m, "cpu_count", xs_native(native_thread_cpu_count));
    map_take(m, "sleep",     xs_native(native_thread_sleep));
    map_take(m, "mutex",     xs_native(native_thread_mutex));
    return xs_module(m);
}

/* byte buffers */

typedef struct {
    uint8_t *data;
    int      len;
    int      cap;
    int      pos; /* read position */
} XSBuf;

static XSBuf *buf_create(int cap) {
    XSBuf *b = xs_malloc(sizeof(XSBuf));
    b->cap = cap > 0 ? cap : 64;
    b->data = xs_malloc((size_t)b->cap);
    b->len = 0;
    b->pos = 0;
    return b;
}

static void buf_ensure(XSBuf *b, int need) {
    while (b->len + need > b->cap) {
        b->cap *= 2;
        b->data = xs_realloc(b->data, (size_t)b->cap);
    }
}

/* We store the XSBuf pointer as an int (cast). A bit hacky but simple. */
static XSBuf *buf_from_map(XSMap *m) {
    Value *pv = map_get(m, "_ptr");
    if (!pv || VAL_TAG(pv) != XS_INT) return NULL;
    return (XSBuf*)(uintptr_t)VAL_INT(pv);
}

static Value *native_buf_new(Interp *ig, Value **a, int n) {
    (void)ig;
    int cap = (n > 0 && VAL_TAG(a[0]) == XS_INT) ? (int)VAL_INT(a[0]) : 64;
    XSBuf *b = buf_create(cap);
    XSMap *m = map_new();
    map_take(m, "_ptr", xs_int((int64_t)(uintptr_t)b));
    return xs_module(m);
}

static Value *native_buf_write_u8(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return value_incref(XS_NULL_VAL);
    uint8_t val = (VAL_TAG(a[1]) == XS_INT) ? (uint8_t)VAL_INT(a[1]) : 0;
    buf_ensure(b, 1);
    b->data[b->len++] = val;
    return value_incref(XS_NULL_VAL);
}

static Value *native_buf_write_u16(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return value_incref(XS_NULL_VAL);
    uint16_t val = (VAL_TAG(a[1]) == XS_INT) ? (uint16_t)VAL_INT(a[1]) : 0;
    buf_ensure(b, 2);
    b->data[b->len++] = (uint8_t)(val & 0xff);
    b->data[b->len++] = (uint8_t)((val >> 8) & 0xff);
    return value_incref(XS_NULL_VAL);
}

static Value *native_buf_write_u32(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return value_incref(XS_NULL_VAL);
    uint32_t val = (VAL_TAG(a[1]) == XS_INT) ? (uint32_t)VAL_INT(a[1]) : 0;
    buf_ensure(b, 4);
    for (int i=0;i<4;i++) b->data[b->len++] = (uint8_t)((val >> (i*8)) & 0xff);
    return value_incref(XS_NULL_VAL);
}

static Value *native_buf_write_u64(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP) return value_incref(XS_NULL_VAL);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return value_incref(XS_NULL_VAL);
    uint64_t val = (VAL_TAG(a[1]) == XS_INT) ? (uint64_t)VAL_INT(a[1]) : 0;
    buf_ensure(b, 8);
    for (int i=0;i<8;i++) b->data[b->len++] = (uint8_t)((val >> (i*8)) & 0xff);
    return value_incref(XS_NULL_VAL);
}

static Value *native_buf_read_u8(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_int(0);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b || b->pos >= b->len) return xs_int(0);
    return xs_int(b->data[b->pos++]);
}

static Value *native_buf_read_u16(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_int(0);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b || b->pos + 2 > b->len) return xs_int(0);
    uint16_t v = (uint16_t)b->data[b->pos] | ((uint16_t)b->data[b->pos+1] << 8);
    b->pos += 2;
    return xs_int(v);
}

static Value *native_buf_read_u32(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_int(0);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b || b->pos + 4 > b->len) return xs_int(0);
    uint32_t v = 0;
    for (int i=0;i<4;i++) v |= ((uint32_t)b->data[b->pos++] << (i*8));
    return xs_int((int64_t)v);
}

static Value *native_buf_read_u64(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_int(0);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b || b->pos + 8 > b->len) return xs_int(0);
    uint64_t v = 0;
    for (int i=0;i<8;i++) v |= ((uint64_t)b->data[b->pos++] << (i*8));
    return xs_int((int64_t)v);
}

static Value *native_buf_write_str(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return value_incref(XS_NULL_VAL);
    size_t slen = strlen(a[1]->s);
    buf_ensure(b, (int)slen);
    memcpy(b->data + b->len, a[1]->s, slen);
    b->len += (int)slen;
    return value_incref(XS_NULL_VAL);
}

static Value *native_buf_to_str(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_str("");
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return xs_str("");
    return xs_str_n((const char*)b->data, (size_t)b->len);
}

static Value *native_buf_to_hex(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_str("");
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b || b->len == 0) return xs_str("");
    char *hex = xs_malloc((size_t)b->len * 2 + 1);
    for (int i = 0; i < b->len; i++) sprintf(hex + i*2, "%02x", b->data[i]);
    hex[b->len * 2] = '\0';
    Value *r = xs_str(hex);
    free(hex);
    return r;
}

static Value *native_buf_len(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_MAP) return xs_int(0);
    XSBuf *b = buf_from_map(a[0]->map);
    if (!b) return xs_int(0);
    return xs_int(b->len);
}

Value *make_buf_module(void) {
    XSMap *m = map_new();
    map_take(m, "new",       xs_native(native_buf_new));
    map_take(m, "write_u8",  xs_native(native_buf_write_u8));
    map_take(m, "write_u16", xs_native(native_buf_write_u16));
    map_take(m, "write_u32", xs_native(native_buf_write_u32));
    map_take(m, "write_u64", xs_native(native_buf_write_u64));
    map_take(m, "read_u8",   xs_native(native_buf_read_u8));
    map_take(m, "read_u16",  xs_native(native_buf_read_u16));
    map_take(m, "read_u32",  xs_native(native_buf_read_u32));
    map_take(m, "read_u64",  xs_native(native_buf_read_u64));
    map_take(m, "write_str", xs_native(native_buf_write_str));
    map_take(m, "to_str",    xs_native(native_buf_to_str));
    map_take(m, "to_hex",    xs_native(native_buf_to_hex));
    map_take(m, "len",       xs_native(native_buf_len));
    return xs_module(m);
}

/* encoding: base64, hex, json */

static const char b64_table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static Value *native_encode_base64_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const uint8_t *in = (const uint8_t*)a[0]->s;
    size_t len = strlen(a[0]->s);
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = xs_malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t val = (uint32_t)in[i] << 16;
        if (i+1 < len) val |= (uint32_t)in[i+1] << 8;
        if (i+2 < len) val |= (uint32_t)in[i+2];
        out[j++] = b64_table[(val >> 18) & 0x3f];
        out[j++] = b64_table[(val >> 12) & 0x3f];
        out[j++] = (i+1 < len) ? b64_table[(val >> 6) & 0x3f] : '=';
        out[j++] = (i+2 < len) ? b64_table[val & 0x3f] : '=';
    }
    out[j] = '\0';
    Value *r = xs_str(out);
    free(out);
    return r;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static Value *native_encode_base64_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *in = a[0]->s;
    size_t len = strlen(in);
    if (len % 4 != 0) return xs_str("");
    size_t out_len = len / 4 * 3;
    if (len > 0 && in[len-1] == '=') out_len--;
    if (len > 1 && in[len-2] == '=') out_len--;
    char *out = xs_malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v0 = b64_decode_char(in[i]);
        int v1 = b64_decode_char(in[i+1]);
        int v2 = (in[i+2] == '=') ? 0 : b64_decode_char(in[i+2]);
        int v3 = (in[i+3] == '=') ? 0 : b64_decode_char(in[i+3]);
        if (v0<0||v1<0) break;
        uint32_t val = ((uint32_t)v0<<18)|((uint32_t)v1<<12)|((uint32_t)v2<<6)|(uint32_t)v3;
        out[j++] = (char)((val >> 16) & 0xff);
        if (in[i+2] != '=') out[j++] = (char)((val >> 8) & 0xff);
        if (in[i+3] != '=') out[j++] = (char)(val & 0xff);
    }
    out[j] = '\0';
    Value *r = xs_str_n(out, j);
    free(out);
    return r;
}

static Value *native_encode_hex_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const uint8_t *in = (const uint8_t*)a[0]->s;
    size_t len = strlen(a[0]->s);
    char *out = xs_malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++) sprintf(out + i*2, "%02x", in[i]);
    out[len*2] = '\0';
    Value *r = xs_str(out);
    free(out);
    return r;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Value *native_encode_hex_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *in = a[0]->s;
    size_t len = strlen(in);
    if (len % 2 != 0) return xs_str("");
    size_t out_len = len / 2;
    char *out = xs_malloc(out_len + 1);
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(in[i*2]);
        int lo = hex_val(in[i*2+1]);
        if (hi < 0 || lo < 0) { free(out); return xs_str(""); }
        out[i] = (char)((hi << 4) | lo);
    }
    out[out_len] = '\0';
    Value *r = xs_str_n(out, out_len);
    free(out);
    return r;
}

static Value *native_encode_url_encode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *in = a[0]->s;
    size_t len = strlen(in);
    /* worst case: every char becomes %XX (3x) */
    char *out = xs_malloc(len * 3 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            sprintf(out + j, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
    Value *r = xs_str(out);
    free(out);
    return r;
}

static Value *native_encode_url_decode(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *in = a[0]->s;
    size_t len = strlen(in);
    char *out = xs_malloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '%' && i+2 < len) {
            int hi = hex_val(in[i+1]);
            int lo = hex_val(in[i+2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') out[j++] = ' ';
        else out[j++] = in[i];
    }
    out[j] = '\0';
    Value *r = xs_str_n(out, j);
    free(out);
    return r;
}

Value *make_encode_module(void) {
    XSMap *m = map_new();
    map_take(m, "base64_encode", xs_native(native_encode_base64_encode));
    map_take(m, "base64_decode", xs_native(native_encode_base64_decode));
    map_take(m, "hex_encode",    xs_native(native_encode_hex_encode));
    map_take(m, "hex_decode",    xs_native(native_encode_hex_decode));
    map_take(m, "url_encode",    xs_native(native_encode_url_encode));
    map_take(m, "url_decode",    xs_native(native_encode_url_decode));
    return xs_module(m);
}

/* in-memory kv store */

/* Helper: skip whitespace */
static const char *db_skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Helper: case-insensitive prefix match, returns pointer past match or NULL */
static const char *db_match_kw(const char *s, const char *kw) {
    s = db_skip_ws(s);
    size_t klen = strlen(kw);
    if (strncasecmp(s, kw, klen) == 0 && (s[klen] == '\0' || isspace((unsigned char)s[klen]) || s[klen] == '(')) {
        return s + klen;
    }
    return NULL;
}

/* Helper: read an identifier (table name, etc.) */
static const char *db_read_ident(const char *s, char *buf, int bufsz) {
    s = db_skip_ws(s);
    int i = 0;
    while (*s && !isspace((unsigned char)*s) && *s != '(' && *s != ')' && *s != ',' && *s != ';' && i < bufsz - 1) {
        buf[i++] = *s++;
    }
    buf[i] = '\0';
    return s;
}

/* Internal: execute a SQL-like command on the db, returning a result.
   Supports:
     CREATE TABLE name
     INSERT INTO name VALUES (v1, v2, ...)
     SELECT * FROM name
     DELETE FROM name
     DELETE FROM name WHERE key = value
     DROP TABLE name
*/

static const char *xs_strcasestr_fn(const char *h, const char *n) {
    size_t nlen = strlen(n);
    if (!nlen) return h;
    for (; *h; h++) {
        if (strncasecmp(h, n, nlen) == 0) return h;
    }
    return NULL;
}

/* Resolve a user-written column name to the positional 'cN' key that
   rows are stored under. Falls back to the name itself if the column
   was already in positional form or the schema is missing. */
static void db_resolve_col(Value *db_val, const char *tname,
                            const char *user_name, char *out, size_t outsz) {
    snprintf(out, outsz, "%s", user_name);
    if (!user_name[0]) return;
    /* already positional (c0, c1, ...) */
    if (user_name[0] == 'c' && user_name[1] >= '0' && user_name[1] <= '9') {
        int all_digits = 1;
        for (const char *q = user_name + 1; *q; q++)
            if (*q < '0' || *q > '9') { all_digits = 0; break; }
        if (all_digits) return;
    }
    Value *sch = map_get(db_val->map, "_schemas");
    if (!sch || (VAL_TAG(sch) != XS_MAP && VAL_TAG(sch) != XS_MODULE)) return;
    Value *cols = map_get(sch->map, tname);
    if (!cols || VAL_TAG(cols) != XS_ARRAY) return;
    for (int i = 0; i < cols->arr->len; i++) {
        Value *cn = cols->arr->items[i];
        if (cn && VAL_TAG(cn) == XS_STR && strcasecmp(cn->s, user_name) == 0) {
            snprintf(out, outsz, "c%d", i);
            return;
        }
    }
}

static Value *db_execute(Value *db_val, const char *sql, int return_rows) {
    if (!db_val || (VAL_TAG(db_val) != XS_MAP && VAL_TAG(db_val) != XS_MODULE) || !db_val->map)
        return xs_str("error: invalid db handle");
    Value *tables_v = map_get(db_val->map, "_tables");
    if (!tables_v || (VAL_TAG(tables_v) != XS_MAP && VAL_TAG(tables_v) != XS_MODULE) || !tables_v->map)
        return xs_str("error: corrupt db (no _tables)");
    XSMap *tables = tables_v->map;

    const char *p = sql;
    const char *rest;
    char tname[256];

    /* CREATE TABLE name (col1, col2, ...) */
    if ((rest = db_match_kw(p, "CREATE")) != NULL) {
        rest = db_match_kw(rest, "TABLE");
        if (!rest) return xs_str("error: expected TABLE after CREATE");
        rest = db_read_ident(rest, tname, sizeof tname);
        if (tname[0] == '\0') return xs_str("error: missing table name");
        if (map_get(tables, tname)) return xs_str("error: table already exists");
        Value *tbl = xs_array_new();
        map_set(tables, tname, tbl);
        value_decref(tbl);
        /* Parse optional column list, store names under _schemas[tname]. */
        Value *cols = xs_array_new();
        rest = db_skip_ws(rest);
        if (*rest == '(') {
            rest++;
            while (*rest && *rest != ')') {
                rest = db_skip_ws(rest);
                char cbuf[128];
                rest = db_read_ident(rest, cbuf, sizeof cbuf);
                if (cbuf[0]) {
                    Value *cs = xs_str(cbuf);
                    array_push(cols->arr, cs);
                    value_decref(cs);
                }
                /* skip any per-column type/constraint tokens to the next , or ) */
                while (*rest && *rest != ',' && *rest != ')') rest++;
                if (*rest == ',') rest++;
            }
        }
        Value *sch = map_get(db_val->map, "_schemas");
        if (sch && (VAL_TAG(sch) == XS_MAP || VAL_TAG(sch) == XS_MODULE))
            map_set(sch->map, tname, cols);
        value_decref(cols);
        return xs_str("ok");
    }

    /* DROP TABLE name */
    if ((rest = db_match_kw(p, "DROP")) != NULL) {
        rest = db_match_kw(rest, "TABLE");
        if (!rest) return xs_str("error: expected TABLE after DROP");
        rest = db_read_ident(rest, tname, sizeof tname);
        (void)rest;
        if (tname[0] == '\0') return xs_str("error: missing table name");
        if (!map_get(tables, tname)) return xs_str("error: no such table");
        map_set(tables, tname, value_incref(XS_NULL_VAL));
        return xs_str("ok");
    }

    /* INSERT INTO name VALUES (...) */
    if ((rest = db_match_kw(p, "INSERT")) != NULL) {
        rest = db_match_kw(rest, "INTO");
        if (!rest) return xs_str("error: expected INTO after INSERT");
        rest = db_read_ident(rest, tname, sizeof tname);
        if (tname[0] == '\0') return xs_str("error: missing table name");
        Value *tbl = map_get(tables, tname);
        if (!tbl || VAL_TAG(tbl) != XS_ARRAY) return xs_str("error: no such table");

        rest = db_match_kw(rest, "VALUES");
        if (!rest) return xs_str("error: expected VALUES");
        rest = db_skip_ws(rest);
        if (*rest != '(') return xs_str("error: expected ( after VALUES");
        rest++; /* skip '(' */

        /* Parse comma-separated values until ')' */
        XSMap *row = map_new();
        int col = 0;
        while (*rest && *rest != ')') {
            rest = db_skip_ws(rest);
            if (*rest == ')') break;

            /* Read a value: string (quoted) or number or identifier */
            char vbuf[1024];
            int vi = 0;
            if (*rest == '\'' || *rest == '"') {
                char quote = *rest++;
                while (*rest && *rest != quote && vi < (int)sizeof(vbuf) - 1)
                    vbuf[vi++] = *rest++;
                if (*rest == quote) rest++;
                vbuf[vi] = '\0';
                char col_name[32];
                snprintf(col_name, sizeof col_name, "c%d", col);
                Value *sv = xs_str(vbuf);
                map_set(row, col_name, sv);
                value_decref(sv);
            } else {
                while (*rest && *rest != ',' && *rest != ')' && !isspace((unsigned char)*rest) && vi < (int)sizeof(vbuf) - 1)
                    vbuf[vi++] = *rest++;
                vbuf[vi] = '\0';
                char col_name[32];
                snprintf(col_name, sizeof col_name, "c%d", col);
                /* Try parsing as integer */
                char *endp;
                long long ival = strtoll(vbuf, &endp, 10);
                if (*endp == '\0' && vi > 0) {
                    Value *iv = xs_int((int64_t)ival);
                    map_set(row, col_name, iv);
                    value_decref(iv);
                } else {
                    Value *sv = xs_str(vbuf);
                    map_set(row, col_name, sv);
                    value_decref(sv);
                }
            }
            col++;
            rest = db_skip_ws(rest);
            if (*rest == ',') rest++;
        }
        Value *row_v = xs_module(row);
        array_push(tbl->arr, row_v);
        value_decref(row_v);
        return xs_str("ok");
    }

    /* SELECT * FROM name [WHERE key = value] */
    if ((rest = db_match_kw(p, "SELECT")) != NULL) {
        rest = db_skip_ws(rest);
        /* skip column list: we only support * */
        if (*rest == '*') rest++;
        else {
            /* skip until FROM */
            const char *from = xs_strcasestr_fn(rest, "FROM");
            if (!from) return xs_str("error: expected FROM");
            rest = from;
        }
        rest = db_match_kw(rest, "FROM");
        if (!rest) return xs_str("error: expected FROM");
        rest = db_read_ident(rest, tname, sizeof tname);
        if (tname[0] == '\0') return xs_str("error: missing table name");
        Value *tbl = map_get(tables, tname);
        if (!tbl || VAL_TAG(tbl) != XS_ARRAY) return xs_str("error: no such table");

        /* Check for WHERE clause */
        const char *where = db_match_kw(rest, "WHERE");
        char where_key[256] = {0};
        char where_val[1024] = {0};
        if (where) {
            where = db_read_ident(where, where_key, sizeof where_key);
            where = db_skip_ws(where);
            if (*where == '=') where++;
            where = db_skip_ws(where);
            /* Read value */
            int wi = 0;
            if (*where == '\'' || *where == '"') {
                char q = *where++;
                while (*where && *where != q && wi < (int)sizeof(where_val) - 1)
                    where_val[wi++] = *where++;
            } else {
                while (*where && !isspace((unsigned char)*where) && *where != ';' && wi < (int)sizeof(where_val) - 1)
                    where_val[wi++] = *where++;
            }
            where_val[wi] = '\0';
        }

        char resolved_key[256];
        if (where_key[0])
            db_resolve_col(db_val, tname, where_key, resolved_key, sizeof resolved_key);
        else resolved_key[0] = '\0';

        Value *results = xs_array_new();
        for (int i = 0; i < tbl->arr->len; i++) {
            Value *row = tbl->arr->items[i];
            if (!row || (VAL_TAG(row) != XS_MAP && VAL_TAG(row) != XS_MODULE)) continue;
            if (resolved_key[0]) {
                Value *fv = map_get(row->map, resolved_key);
                if (!fv) continue;
                char *fs = value_str(fv);
                int match = (strcmp(fs, where_val) == 0);
                free(fs);
                if (!match) continue;
            }
            array_push(results->arr, row);
        }
        return results;
    }

    /* DELETE FROM name [WHERE key = value] */
    if ((rest = db_match_kw(p, "DELETE")) != NULL) {
        rest = db_match_kw(rest, "FROM");
        if (!rest) return xs_str("error: expected FROM after DELETE");
        rest = db_read_ident(rest, tname, sizeof tname);
        if (tname[0] == '\0') return xs_str("error: missing table name");
        Value *tbl = map_get(tables, tname);
        if (!tbl || VAL_TAG(tbl) != XS_ARRAY) return xs_str("error: no such table");

        /* Check for WHERE clause */
        const char *where = db_match_kw(rest, "WHERE");
        if (!where) {
            /* Delete all rows */
            tbl->arr->len = 0;
            return xs_str("ok");
        }

        char where_key[256] = {0};
        char where_val[1024] = {0};
        where = db_read_ident(where, where_key, sizeof where_key);
        where = db_skip_ws(where);
        if (*where == '=') where++;
        where = db_skip_ws(where);
        int wi = 0;
        if (*where == '\'' || *where == '"') {
            char q = *where++;
            while (*where && *where != q && wi < (int)sizeof(where_val) - 1)
                where_val[wi++] = *where++;
        } else {
            while (*where && !isspace((unsigned char)*where) && *where != ';' && wi < (int)sizeof(where_val) - 1)
                where_val[wi++] = *where++;
        }
        where_val[wi] = '\0';

        char resolved_key[256];
        db_resolve_col(db_val, tname, where_key, resolved_key, sizeof resolved_key);

        /* Remove matching rows (compact in-place) */
        int dst = 0;
        for (int i = 0; i < tbl->arr->len; i++) {
            Value *row = tbl->arr->items[i];
            int keep = 1;
            if (row && (VAL_TAG(row) == XS_MAP || VAL_TAG(row) == XS_MODULE) && row->map) {
                Value *fv = map_get(row->map, resolved_key);
                if (fv) {
                    char *fs = value_str(fv);
                    if (strcmp(fs, where_val) == 0) keep = 0;
                    free(fs);
                }
            }
            if (keep) {
                tbl->arr->items[dst++] = tbl->arr->items[i];
            } else {
                value_decref(tbl->arr->items[i]);
            }
        }
        tbl->arr->len = dst;
        return xs_str("ok");
    }

    return xs_str("error: unrecognized SQL command");
}

static Value *native_db_open(Interp *ig, Value **a, int n) {
    (void)ig;
    XSMap *db = map_new();
    const char *name = (n > 0 && VAL_TAG(a[0]) == XS_STR) ? a[0]->s : "memdb";
    map_set(db, "_name", xs_str(name));
    XSMap *tables = map_new();
    Value *tv = xs_module(tables);
    map_set(db, "_tables", tv);
    value_decref(tv);
    /* _schemas maps table name -> array of column names, so WHERE/SELECT
       can resolve real column identifiers instead of only positional
       c0/c1/... names. */
    XSMap *schemas = map_new();
    Value *sv = xs_module(schemas);
    map_set(db, "_schemas", sv);
    value_decref(sv);
    return xs_module(db);
}

static Value *native_db_exec(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return xs_str("error: db.exec requires (db, sql)");
    if ((VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR)
        return xs_str("error: invalid arguments to db.exec");
    return db_execute(a[0], a[1]->s, 0);
}

static Value *native_db_query(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return xs_str("error: db.query requires (db, sql)");
    if ((VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE) || VAL_TAG(a[1]) != XS_STR)
        return xs_str("error: invalid arguments to db.query");
    return db_execute(a[0], a[1]->s, 1);
}

static Value *native_db_close(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: db.close requires a db handle");
    /* Mark db as closed by removing _tables */
    map_set(a[0]->map, "_tables", value_incref(XS_NULL_VAL));
    map_set(a[0]->map, "_closed", value_incref(XS_TRUE_VAL));
    return xs_str("ok");
}

Value *make_db_module(void) {
    XSMap *m = map_new();
    map_take(m, "open",  xs_native(native_db_open));
    map_take(m, "exec",  xs_native(native_db_exec));
    map_take(m, "query", xs_native(native_db_query));
    map_take(m, "close", xs_native(native_db_close));
    return xs_module(m);
}

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

/* ffi (dlopen/dlsym) */
#ifdef XSC_ENABLE_PLUGINS
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

static Value *native_ffi_load(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
#ifdef _WIN32
    void *handle = (void *)LoadLibraryA(a[0]->s);
#else
    void *handle = dlopen(a[0]->s, RTLD_LAZY);
#endif
    if (!handle) return value_incref(XS_NULL_VAL);
    XSMap *h = map_new();
    map_take(h, "_handle", xs_int((int64_t)(uintptr_t)handle));
    map_set(h, "path", value_incref(a[0]));
    return xs_module(h);
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available (compile with XSC_ENABLE_PLUGINS=1)");
#endif
}

static Value *native_ffi_sym(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 2 || VAL_TAG(a[0]) != XS_MAP || VAL_TAG(a[1]) != XS_STR)
        return value_incref(XS_NULL_VAL);
    Value *hp = map_get(a[0]->map, "_handle");
    if (!hp || VAL_TAG(hp) != XS_INT) return value_incref(XS_NULL_VAL);
    void *handle = (void*)(uintptr_t)VAL_INT(hp);
#ifdef _WIN32
    void *sym = (void *)GetProcAddress((HMODULE)handle, a[1]->s);
#else
    void *sym = dlsym(handle, a[1]->s);
#endif
    if (!sym) return value_incref(XS_NULL_VAL);
    XSMap *s = map_new();
    map_take(s, "_sym", xs_int((int64_t)(uintptr_t)sym));
    map_set(s, "name", value_incref(a[1]));
    return xs_module(s);
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available");
#endif
}

static Value *native_ffi_call(Interp *ig, Value **a, int n) {
    (void)ig;
    /* ffi.call(sym_handle, args_array): call a foreign function.
       If the sym has a _fn field pointing to a native function wrapper, call it.
       Otherwise return an error. */
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: ffi.call requires a symbol handle");

    /* Check for a native function wrapper in _fn field */
    Value *fn_v = map_get(a[0]->map, "_fn");
    if (fn_v && VAL_TAG(fn_v) == XS_NATIVE) {
        /* Pass args_array items as arguments */
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
            return fn_v->native(ig, a[1]->arr->items, a[1]->arr->len);
        }
        return fn_v->native(ig, NULL, 0);
    }
    if (fn_v && VAL_TAG(fn_v) == XS_FUNC) {
        if (n >= 2 && VAL_TAG(a[1]) == XS_ARRAY) {
            return call_value(ig, fn_v, a[1]->arr->items, a[1]->arr->len, "ffi.call");
        }
        return call_value(ig, fn_v, NULL, 0, "ffi.call");
    }

    return xs_str("error: symbol has no callable _fn wrapper; generic FFI requires libffi");
}

static Value *native_ffi_close(Interp *ig, Value **a, int n) {
    (void)ig;
#ifdef XSC_ENABLE_PLUGINS
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE))
        return xs_str("error: ffi.close requires a library handle");
    Value *hp = map_get(a[0]->map, "_handle");
    if (!hp || VAL_TAG(hp) != XS_INT)
        return xs_str("error: invalid library handle");
    void *handle = (void *)(uintptr_t)VAL_INT(hp);
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
    /* Invalidate the handle */
    map_take(a[0]->map, "_handle", xs_int(0));
    map_set(a[0]->map, "_closed", value_incref(XS_TRUE_VAL));
    return xs_str("ok");
#else
    (void)a; (void)n;
    return xs_str("error: FFI not available (compile with XSC_ENABLE_PLUGINS=1)");
#endif
}

static Value *native_ffi_typeof(Interp *ig, Value **a, int n) {
    (void)ig;
    /* ffi.typeof(sym_handle): return the type string of a symbol */
    if (n < 1) return xs_str("null");
    if (VAL_TAG(a[0]) == XS_MAP || VAL_TAG(a[0]) == XS_MODULE) {
        /* Check if it has a _sym field (it's a symbol handle) */
        Value *sym = map_get(a[0]->map, "_sym");
        if (sym) return xs_str("ffi_symbol");
        /* Check if it has a _handle field (it's a library handle) */
        Value *h = map_get(a[0]->map, "_handle");
        if (h) return xs_str("ffi_library");
        return xs_str("map");
    }
    /* For non-map values, return the XS type */
    switch (VAL_TAG(a[0])) {
        case XS_NULL:   return xs_str("null");
        case XS_BOOL:   return xs_str("bool");
        case XS_INT:    return xs_str("int");
        case XS_FLOAT:  return xs_str("float");
        case XS_STR:    return xs_str("str");
        case XS_FUNC:   return xs_str("fn");
        case XS_NATIVE: return xs_str("native_fn");
        default:        return xs_str("unknown");
    }
}

Value *make_ffi_module(void) {
    XSMap *m = map_new();
    map_take(m, "load",   xs_native(native_ffi_load));
    map_take(m, "sym",    xs_native(native_ffi_sym));
    map_take(m, "call",   xs_native(native_ffi_call));
    map_take(m, "close",  xs_native(native_ffi_close));
    map_take(m, "typeof", xs_native(native_ffi_typeof));
    return xs_module(m);
}

/* reflect */

static Value *native_reflect_type_of(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_str("null");
    switch (VAL_TAG(a[0])) {
        case XS_NULL:       return xs_str("null");
        case XS_BOOL:       return xs_str("bool");
        case XS_INT:        return xs_str("int");
        case XS_FLOAT:      return xs_str("float");
        case XS_STR:        return xs_str("str");
        case XS_CHAR:       return xs_str("char");
        case XS_ARRAY:      return xs_str("array");
        case XS_MAP:        return xs_str("map");
        case XS_TUPLE:      return xs_str("tuple");
        case XS_FUNC:       return xs_str("fn");
        case XS_NATIVE:     return xs_str("native_fn");
        case XS_STRUCT_VAL: return xs_str("struct");
        case XS_ENUM_VAL:   return xs_str("enum");
        case XS_CLASS_VAL:  return xs_str("class");
        case XS_INST:       return xs_str("instance");
        case XS_RANGE:      return xs_str("range");
        case XS_MODULE:     return xs_str("module");
#ifdef XSC_ENABLE_VM
        case XS_CLOSURE:    return xs_str("closure");
#endif
        default:            return xs_str("unknown");
    }
}

static Value *native_reflect_fields(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_array_new();
    XSMap *m = NULL;
    if (VAL_TAG(a[0]) == XS_MAP || VAL_TAG(a[0]) == XS_MODULE) m = a[0]->map;
    else if (VAL_TAG(a[0]) == XS_STRUCT_VAL) m = a[0]->st->fields;
    else if (VAL_TAG(a[0]) == XS_INST) m = a[0]->inst->fields;
    if (!m) return xs_array_new();

    int klen;
    char **keys = map_keys(m, &klen);
    Value *arr = xs_array_new();
    for (int i = 0; i < klen; i++) {
        XSMap *pair = map_new();
        map_set(pair, "name", xs_str(keys[i]));
        Value *v = map_get(m, keys[i]);
        if (v) map_set(pair, "value", value_incref(v));
        else   map_set(pair, "value", value_incref(XS_NULL_VAL));
        Value *pm = xs_module(pair);
        array_push(arr->arr, pm);
        value_decref(pm);
        free(keys[i]);
    }
    free(keys);
    return arr;
}

static Value *native_reflect_methods(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_array_new();
    XSMap *m = NULL;
    if (VAL_TAG(a[0]) == XS_INST && a[0]->inst->methods)
        m = a[0]->inst->methods;
    else if (VAL_TAG(a[0]) == XS_CLASS_VAL && a[0]->cls->methods)
        m = a[0]->cls->methods;
    if (!m) return xs_array_new();

    int klen;
    char **keys = map_keys(m, &klen);
    Value *arr = xs_array_new();
    for (int i = 0; i < klen; i++) {
        Value *s = xs_str(keys[i]);
        array_push(arr->arr, s);
        value_decref(s);
        free(keys[i]);
    }
    free(keys);
    return arr;
}

static Value *native_reflect_is_instance(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2) return value_incref(XS_FALSE_VAL);
    if (VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    const char *type_name = a[1]->s;
    const char *actual = NULL;
    switch (VAL_TAG(a[0])) {
        case XS_NULL:   actual = "null"; break;
        case XS_BOOL:   actual = "bool"; break;
        case XS_INT:    actual = "int"; break;
        case XS_FLOAT:  actual = "float"; break;
        case XS_STR:    actual = "str"; break;
        case XS_ARRAY:  actual = "array"; break;
        case XS_MAP:    actual = "map"; break;
        case XS_FUNC:   actual = "fn"; break;
        case XS_NATIVE: actual = "native_fn"; break;
        case XS_INST:   actual = a[0]->inst->class_ ? a[0]->inst->class_->name : "instance"; break;
        case XS_STRUCT_VAL: actual = a[0]->st->type_name; break;
        case XS_ENUM_VAL:   actual = a[0]->en->type_name; break;
        default: actual = "unknown"; break;
    }
    return (actual && strcmp(actual, type_name) == 0)
        ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

Value *make_reflect_module(void) {
    XSMap *m = map_new();
    map_take(m, "type_of",     xs_native(native_reflect_type_of));
    map_take(m, "fields",      xs_native(native_reflect_fields));
    map_take(m, "methods",     xs_native(native_reflect_methods));
    map_take(m, "is_instance", xs_native(native_reflect_is_instance));
    return xs_module(m);
}

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

/* fs module */

static Value *native_fs_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = fopen(a[0]->s, "r");
    if (!f) return value_incref(XS_NULL_VAL);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = xs_malloc(sz + 1);
    long nr = (long)fread(buf, 1, sz, f); fclose(f); buf[nr] = '\0';
    Value *v = xs_str(buf); free(buf); return v;
}

static Value *native_fs_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f = fopen(a[0]->s, "w");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(a[1]->s, f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_fs_append(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *f = fopen(a[0]->s, "a");
    if (!f) return value_incref(XS_FALSE_VAL);
    fputs(a[1]->s, f); fclose(f);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_fs_exists(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    return (access(a[0]->s, F_OK) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_remove(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    return (unlink(a[0]->s) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_mkdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    int r = io_mkdirs(a[0]->s);
    return (r == 0 || errno == EEXIST) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_ls(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    DIR *d = opendir(a[0]->s); if (!d) return xs_array_new();
    Value *arr = xs_array_new();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        array_push(arr->arr, xs_str(ent->d_name));
    }
    closedir(d); return arr;
}

static Value *native_fs_is_dir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s, &st) != 0) return value_incref(XS_FALSE_VAL);
    return S_ISDIR(st.st_mode) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_is_file(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    struct stat st; if (stat(a[0]->s, &st) != 0) return value_incref(XS_FALSE_VAL);
    return S_ISREG(st.st_mode) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_size(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_int(-1);
    struct stat st; if (stat(a[0]->s, &st) != 0) return xs_int(-1);
    return xs_int((int64_t)st.st_size);
}

static Value *native_fs_stat(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    struct stat st;
    if (stat(a[0]->s, &st) != 0) return value_incref(XS_NULL_VAL);
    XSMap *m = map_new();
    map_take(m, "size", xs_int((int64_t)st.st_size));
    map_set(m, "is_file", S_ISREG(st.st_mode) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
    map_set(m, "is_dir", S_ISDIR(st.st_mode) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
    map_take(m, "mtime", xs_int((int64_t)st.st_mtime));
    return xs_module(m);
}

static Value *native_fs_read_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    FILE *f = fopen(a[0]->s, "rb");
    if (!f) return xs_array_new();
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = xs_malloc((size_t)sz);
    long nr = (long)fread(buf, 1, (size_t)sz, f); fclose(f);
    Value *arr = xs_array_new();
    for (long j = 0; j < nr; j++) array_push(arr->arr, xs_int(buf[j]));
    free(buf);
    return arr;
}

static Value *native_fs_write_bytes(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_ARRAY) return value_incref(XS_FALSE_VAL);
    FILE *f = fopen(a[0]->s, "wb");
    if (!f) return value_incref(XS_FALSE_VAL);
    XSArray *arr = a[1]->arr;
    for (int j = 0; j < arr->len; j++) {
        uint8_t b = (VAL_TAG(arr->items[j]) == XS_INT) ? (uint8_t)VAL_INT(arr->items[j]) : 0;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_fs_mkdir_p(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    int r = io_mkdirs(a[0]->s);
    return (r == 0 || errno == EEXIST) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_rmdir(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_FALSE_VAL);
    return (rmdir(a[0]->s) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_rename(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    return (rename(a[0]->s, a[1]->s) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
}

static Value *native_fs_copy(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    FILE *src = fopen(a[0]->s, "rb"); if (!src) return value_incref(XS_FALSE_VAL);
    FILE *dst = fopen(a[1]->s, "wb"); if (!dst) { fclose(src); return value_incref(XS_FALSE_VAL); }
    char buf[4096]; size_t nr;
    while ((nr = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, nr, dst);
    fclose(src); fclose(dst);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_fs_join(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1) return xs_str("");
    int total = 0;
    for (int j = 0; j < n; j++) {
        if (VAL_TAG(a[j]) != XS_STR) continue;
        total += (int)strlen(a[j]->s) + 1;
    }
    char *res = xs_malloc(total + 1); res[0] = '\0';
    for (int j = 0; j < n; j++) {
        if (VAL_TAG(a[j]) != XS_STR) continue;
        if (j > 0 && res[0] != '\0') {
            int rlen = (int)strlen(res);
            if (rlen > 0 && res[rlen-1] != '/') strcat(res, "/");
        }
        strcat(res, a[j]->s);
    }
    Value *v = xs_str(res); free(res); return v;
}

static Value *native_fs_basename(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    const char *last = strrchr(s, '/');
    return xs_str(last ? last + 1 : s);
}

static Value *native_fs_dirname(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str(".");
    const char *s = a[0]->s;
    const char *last = strrchr(s, '/');
    if (!last) return xs_str(".");
    if (last == s) return xs_str("/");
    return xs_str_n(s, last - s);
}

static Value *native_fs_ext(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    const char *s = a[0]->s;
    const char *base = strrchr(s, '/');
    const char *dot = strrchr(base ? base : s, '.');
    return dot ? xs_str(dot) : xs_str("");
}

static Value *native_fs_abs(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_str("");
    char buf[4096];
#ifdef _WIN32
    if (_fullpath(buf, a[0]->s, sizeof(buf))) return xs_str(buf);
#else
    if (realpath(a[0]->s, buf)) return xs_str(buf);
#endif
    return value_incref((Value*)a[0]);
}

static Value *native_fs_temp_dir(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
    const char *t = getenv("TMPDIR");
#ifdef _WIN32
    if (!t) t = getenv("TEMP");
    if (!t) t = getenv("TMP");
#endif
    if (!t) t = "/tmp";
    char buf[4096];
#ifdef _WIN32
    if (_fullpath(buf, t, sizeof(buf))) return xs_str(buf);
#else
    if (realpath(t, buf)) return xs_str(buf);
#endif
    return xs_str(t);
}

static Value *native_fs_temp_file(Interp *ig, Value **a, int n) {
    (void)ig; (void)a; (void)n;
#ifdef __MINGW32__
    /* mingw: use mkstemp from POSIX layer */
    const char *t = getenv("TMPDIR");
    if (!t) t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = "/tmp";
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/xs_tmp_XXXXXX", t);
    int fd = mkstemp(tmpl);
    if (fd < 0) return xs_str("");
    close(fd);
    return xs_str(tmpl);
#elif defined(_WIN32)
    const char *t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = ".";
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/xs_tmp_XXXXXX", t);
    if (_mktemp(tmpl)) { FILE *f = fopen(tmpl, "w"); if (f) fclose(f); }
    else return xs_str("");
    return xs_str(tmpl);
#else
    const char *t = getenv("TMPDIR");
    if (!t) t = "/tmp";
    char resolved[4096];
    if (realpath(t, resolved)) t = resolved;
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/xs_tmp_XXXXXX", t);
    int fd = mkstemp(tmpl);
    if (fd < 0) return xs_str("");
    close(fd);
    return xs_str(tmpl);
#endif
}

/* fs.read_stream(path) - returns a reader map with read/read_line/read_all/close */
static Value *native_fs_reader_read(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (!f) return value_incref(XS_NULL_VAL);
    int count = 4096;
    if (n >= 2 && VAL_TAG(a[1]) == XS_INT) count = (int)VAL_INT(a[1]);
    if (count <= 0) count = 1;
    if (count > 1048576) count = 1048576;
    char *buf = xs_malloc((size_t)count + 1);
    size_t nr = fread(buf, 1, (size_t)count, f);
    if (nr == 0) { free(buf); return value_incref(XS_NULL_VAL); }
    buf[nr] = '\0';
    Value *v = xs_str_n(buf, nr); free(buf); return v;
}

static Value *native_fs_reader_read_line(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (!f) return value_incref(XS_NULL_VAL);
    int cap = 256; char *buf = xs_malloc(cap); int pos = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') break;
        if (pos + 1 >= cap) { cap *= 2; buf = xs_realloc(buf, cap); }
        buf[pos++] = (char)c;
    }
    if (pos == 0 && c == EOF) { free(buf); return value_incref(XS_NULL_VAL); }
    if (pos > 0 && buf[pos-1] == '\r') pos--;
    buf[pos] = '\0';
    Value *v = xs_str(buf); free(buf); return v;
}

static Value *native_fs_reader_read_all(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (!f) return value_incref(XS_NULL_VAL);
    int cap = 4096; char *buf = xs_malloc(cap); int pos = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (pos + 1 >= cap) { cap *= 2; buf = xs_realloc(buf, cap); }
        buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    Value *v = xs_str(buf); free(buf); return v;
}

static Value *native_fs_reader_close(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (f) { fclose(f); map_take(a[0]->map, "_fd", xs_int(0)); }
    return value_incref(XS_NULL_VAL);
}

static Value *native_fs_read_stream(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = fopen(a[0]->s, "r");
    if (!f) return value_incref(XS_NULL_VAL);
    XSMap *m = map_new();
    map_take(m, "_fd", xs_int((int64_t)(uintptr_t)f));
    map_take(m, "read", xs_native(native_fs_reader_read));
    map_take(m, "read_line", xs_native(native_fs_reader_read_line));
    map_take(m, "read_all", xs_native(native_fs_reader_read_all));
    map_take(m, "close", xs_native(native_fs_reader_close));
    return xs_module(m);
}

/* fs.write_stream(path) - returns a writer map with write/flush/close */
static Value *native_fs_writer_write(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_FALSE_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_FALSE_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (!f) return value_incref(XS_FALSE_VAL);
    if (VAL_TAG(a[1]) == XS_STR) fputs(a[1]->s, f);
    return value_incref(XS_TRUE_VAL);
}

static Value *native_fs_writer_flush(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || (VAL_TAG(a[0]) != XS_MAP && VAL_TAG(a[0]) != XS_MODULE)) return value_incref(XS_NULL_VAL);
    Value *fdv = map_get(a[0]->map, "_fd");
    if (!fdv || VAL_TAG(fdv) != XS_INT) return value_incref(XS_NULL_VAL);
    FILE *f = (FILE*)(uintptr_t)VAL_INT(fdv);
    if (f) fflush(f);
    return value_incref(XS_NULL_VAL);
}

static Value *native_fs_write_stream(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    FILE *f = fopen(a[0]->s, "w");
    if (!f) return value_incref(XS_NULL_VAL);
    XSMap *m = map_new();
    map_take(m, "_fd", xs_int((int64_t)(uintptr_t)f));
    map_take(m, "write", xs_native(native_fs_writer_write));
    map_take(m, "flush", xs_native(native_fs_writer_flush));
    map_take(m, "close", xs_native(native_fs_reader_close));
    return xs_module(m);
}

/* fs.read_lines(path) - returns array of lines */
static Value *native_fs_read_lines(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    FILE *f = fopen(a[0]->s, "r");
    if (!f) return xs_array_new();
    Value *arr = xs_array_new();
    char buf[8192];
    while (fgets(buf, sizeof(buf), f)) {
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        array_push(arr->arr, xs_str(buf));
    }
    fclose(f);
    return arr;
}

/* fs.walk(path) - recursive directory walker, returns array of entry maps */
static void fs_walk_recurse(const char *dir, Value *arr) {
#if !defined(__wasi__)
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;
        Value *entry = xs_map_new();
        Value *pv = xs_str(fullpath); map_set(entry->map, "path", pv); value_decref(pv);
        Value *nv = xs_str(ent->d_name); map_set(entry->map, "name", nv); value_decref(nv);
        int isdir = S_ISDIR(st.st_mode);
        int isfile = S_ISREG(st.st_mode);
        map_set(entry->map, "is_file", isfile ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
        map_set(entry->map, "is_dir", isdir ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
        Value *sv = xs_int((int64_t)st.st_size); map_set(entry->map, "size", sv); value_decref(sv);
        array_push(arr->arr, entry);
        value_decref(entry);
        if (isdir) fs_walk_recurse(fullpath, arr);
    }
    closedir(d);
#else
    (void)dir; (void)arr;
#endif
}

static Value *native_fs_walk(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    Value *arr = xs_array_new();
    fs_walk_recurse(a[0]->s, arr);
    return arr;
}

/* fs.glob(pattern) - match files by glob pattern */
static Value *native_fs_glob(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return xs_array_new();
    glob_t g;
    memset(&g, 0, sizeof(g));
    int rc = glob(a[0]->s, GLOB_NOSORT, NULL, &g);
    Value *arr = xs_array_new();
    if (rc == 0) {
        for (size_t j = 0; j < g.gl_pathc; j++)
            array_push(arr->arr, xs_str(g.gl_pathv[j]));
    }
    globfree(&g);
    return arr;
#else
    (void)a; (void)n;
    return xs_array_new();
#endif
}

/* fs.chmod(path, mode) */
static Value *native_fs_chmod(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_INT) return value_incref(XS_FALSE_VAL);
    return (chmod(a[0]->s, (mode_t)VAL_INT(a[1])) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* fs.symlink(target, link_path) */
static Value *native_fs_symlink(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 2 || VAL_TAG(a[0]) != XS_STR || VAL_TAG(a[1]) != XS_STR) return value_incref(XS_FALSE_VAL);
    return (symlink(a[0]->s, a[1]->s) == 0) ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL);
#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}

/* fs.readlink(path) */
static Value *native_fs_readlink(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    char buf[4096];
    ssize_t len = readlink(a[0]->s, buf, sizeof(buf) - 1);
    if (len < 0) return value_incref(XS_NULL_VAL);
    buf[len] = '\0';
    return xs_str(buf);
#else
    (void)a; (void)n;
    return value_incref(XS_NULL_VAL);
#endif
}

/* fs.realpath(path) */
static Value *native_fs_realpath(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 1 || VAL_TAG(a[0]) != XS_STR) return value_incref(XS_NULL_VAL);
    char buf[4096];
#ifdef _WIN32
    if (_fullpath(buf, a[0]->s, sizeof(buf))) return xs_str(buf);
#else
    if (realpath(a[0]->s, buf)) return xs_str(buf);
#endif
    return value_incref(XS_NULL_VAL);
}

/* fs.watch(path, callback) - single-shot watcher that blocks until the
   first filesystem event, calls callback({type, name}), then returns.
   Linux uses inotify, macOS/BSD use kqueue/EVFILT_VNODE, Windows uses
   ReadDirectoryChangesW. Other platforms return false. */
static Value *native_fs_watch(Interp *ig, Value **a, int n) {
    (void)ig;
    if (n < 2 || VAL_TAG(a[0]) != XS_STR ||
        (VAL_TAG(a[1]) != XS_FUNC && VAL_TAG(a[1]) != XS_NATIVE))
        return value_incref(XS_FALSE_VAL);

#if defined(__linux__) && !defined(__wasi__)
    int ifd = inotify_init();
    if (ifd < 0) return value_incref(XS_FALSE_VAL);
    int wd = inotify_add_watch(ifd, a[0]->s,
        IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVE);
    if (wd < 0) { close(ifd); return value_incref(XS_FALSE_VAL); }
    char evbuf[4096];
    ssize_t len = read(ifd, evbuf, sizeof(evbuf));
    if (len > 0) {
        struct inotify_event *ev = (struct inotify_event *)evbuf;
        Value *info = xs_map_new();
        const char *etype = "unknown";
        if (ev->mask & IN_CREATE) etype = "create";
        else if (ev->mask & IN_DELETE) etype = "delete";
        else if (ev->mask & IN_MODIFY) etype = "modify";
        else if (ev->mask & IN_MOVE) etype = "move";
        Value *tv = xs_str(etype); map_set(info->map, "type", tv); value_decref(tv);
        if (ev->len > 0) {
            Value *nv = xs_str(ev->name);
            map_set(info->map, "name", nv); value_decref(nv);
        }
        Value *cb_args[1] = { info };
        Value *r = call_value(ig, a[1], cb_args, 1, "fs.watch");
        if (r) value_decref(r);
        value_decref(info);
    }
    inotify_rm_watch(ifd, wd);
    close(ifd);
    return value_incref(XS_TRUE_VAL);

#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
       defined(__OpenBSD__) || defined(__DragonFly__)) && !defined(__wasi__)
    int kq = kqueue();
    if (kq < 0) return value_incref(XS_FALSE_VAL);
    int fd = open(a[0]->s, O_EVTONLY);
    if (fd < 0) { close(kq); return value_incref(XS_FALSE_VAL); }
    struct kevent reg, out;
    EV_SET(&reg, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME | NOTE_ATTRIB,
           0, NULL);
    if (kevent(kq, &reg, 1, NULL, 0, NULL) < 0) {
        close(fd); close(kq); return value_incref(XS_FALSE_VAL);
    }
    if (kevent(kq, NULL, 0, &out, 1, NULL) > 0) {
        const char *etype = "unknown";
        if (out.fflags & NOTE_DELETE) etype = "delete";
        else if (out.fflags & NOTE_RENAME) etype = "move";
        else if (out.fflags & (NOTE_WRITE | NOTE_EXTEND)) etype = "modify";
        else if (out.fflags & NOTE_ATTRIB) etype = "modify";
        Value *info = xs_map_new();
        Value *tv = xs_str(etype); map_set(info->map, "type", tv); value_decref(tv);
        /* kqueue doesn't report filenames inside a watched directory */
        Value *cb_args[1] = { info };
        Value *r = call_value(ig, a[1], cb_args, 1, "fs.watch");
        if (r) value_decref(r);
        value_decref(info);
    }
    close(fd); close(kq);
    return value_incref(XS_TRUE_VAL);

#elif defined(_WIN32) && !defined(__wasi__)
    HANDLE dir = CreateFileA(a[0]->s, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dir == INVALID_HANDLE_VALUE) return value_incref(XS_FALSE_VAL);
    /* align on DWORD per ReadDirectoryChangesW requirements */
    unsigned char buf[4096] __attribute__((aligned(sizeof(DWORD))));
    DWORD bytes = 0;
    BOOL ok = ReadDirectoryChangesW(dir, buf, sizeof(buf), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
        &bytes, NULL, NULL);
    if (ok && bytes > 0) {
        FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)buf;
        const char *etype = "unknown";
        switch (fni->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME: etype = "create"; break;
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME: etype = "delete"; break;
            case FILE_ACTION_MODIFIED:         etype = "modify"; break;
        }
        /* convert UTF-16 filename to UTF-8 */
        int wlen = (int)(fni->FileNameLength / sizeof(WCHAR));
        int need = WideCharToMultiByte(CP_UTF8, 0, fni->FileName, wlen,
                                       NULL, 0, NULL, NULL);
        char *name = (char *)malloc((size_t)need + 1);
        if (name) {
            WideCharToMultiByte(CP_UTF8, 0, fni->FileName, wlen,
                                name, need, NULL, NULL);
            name[need] = '\0';
        }
        Value *info = xs_map_new();
        Value *tv = xs_str(etype); map_set(info->map, "type", tv); value_decref(tv);
        if (name) {
            Value *nv = xs_str(name); map_set(info->map, "name", nv); value_decref(nv);
            free(name);
        }
        Value *cb_args[1] = { info };
        Value *r = call_value(ig, a[1], cb_args, 1, "fs.watch");
        if (r) value_decref(r);
        value_decref(info);
    }
    CloseHandle(dir);
    return value_incref(XS_TRUE_VAL);

#else
    (void)a; (void)n;
    return value_incref(XS_FALSE_VAL);
#endif
}


/* http high-level module */
static Value *native_http_get(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *hdrs=NULL;
    if (n>=2&&VAL_TAG(a[1])==XS_MAP) {
        Value *hv=map_get(a[1]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map;
    }
    return http_do_request("GET",a[0]->s,hdrs,NULL,0);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_post(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("POST",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_put(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("PUT",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_delete(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    XSMap *hdrs=NULL;
    if (n>=2&&VAL_TAG(a[1])==XS_MAP) {
        Value *hv=map_get(a[1]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map;
    }
    return http_do_request("DELETE",a[0]->s,hdrs,NULL,0);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}
static Value *native_http_patch(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<1||VAL_TAG(a[0])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *body=NULL; size_t body_len=0;
    if (n>=2&&VAL_TAG(a[1])==XS_STR) { body=a[1]->s; body_len=strlen(a[1]->s); }
    XSMap *hdrs=NULL;
    if (n>=3&&VAL_TAG(a[2])==XS_MAP) {
        Value *hv=map_get(a[2]->map,"headers");
        if (hv&&VAL_TAG(hv)==XS_MAP) hdrs=hv->map; else hdrs=a[2]->map;
    }
    return http_do_request("PATCH",a[0]->s,hdrs,body,body_len);
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}

/* http.request(method, url, opts) - full request with options map */
static Value *native_http_request(Interp *ig, Value **a, int n) {
    (void)ig;
#if !defined(__MINGW32__) && !defined(__wasi__)
    if (n<2||VAL_TAG(a[0])!=XS_STR||VAL_TAG(a[1])!=XS_STR) return value_incref(XS_NULL_VAL);
    const char *method = a[0]->s;
    const char *url = a[1]->s;
    XSMap *hdrs = NULL;
    const char *body = NULL;
    size_t body_len = 0;
    if (n >= 3 && VAL_TAG(a[2]) == XS_MAP) {
        Value *hv = map_get(a[2]->map, "headers");
        if (hv && VAL_TAG(hv) == XS_MAP) hdrs = hv->map;
        Value *bv = map_get(a[2]->map, "body");
        if (bv && VAL_TAG(bv) == XS_STR) { body = bv->s; body_len = strlen(bv->s); }
    }
    Value *result = http_do_request(method, url, hdrs, body, body_len);
    /* add 'ok' field for convenience */
    if (result && VAL_TAG(result) == XS_MAP) {
        Value *sv = map_get(result->map, "status");
        if (sv && VAL_TAG(sv) == XS_INT) {
            int ok = (VAL_INT(sv) >= 200 && VAL_INT(sv) < 300) ? 1 : 0;
            map_set(result->map, "ok", ok ? value_incref(XS_TRUE_VAL) : value_incref(XS_FALSE_VAL));
        }
    }
    return result;
#else
    (void)a;(void)n; return value_incref(XS_NULL_VAL);
#endif
}

/* http.serve(port, handler)
   HTTP/1.1 server. `handler` is an XS function that takes a request map
   {method, path, query, headers, body} and returns a response map
   {status?, headers?, body}. Each accepted connection is dispatched to
   a worker thread (so slow handlers don't block subsequent connects);
   the GIL serializes handler execution but I/O is concurrent. Blocks
   the calling thread until killed. */
#if !defined(__MINGW32__) && !defined(__wasi__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

static Value *native_http_serve(Interp *ig, Value **a, int n) {
#if defined(__MINGW32__) || defined(__wasi__)
    (void)ig; (void)a; (void)n;
    fprintf(stderr, "http.serve: not available on this platform\n");
    return value_incref(XS_NULL_VAL);
#else
    if (n < 2 || VAL_TAG(a[0]) != XS_INT ||
        (VAL_TAG(a[1]) != XS_FUNC && VAL_TAG(a[1]) != XS_NATIVE)) {
        fprintf(stderr, "http.serve: expected (port: int, handler: fn)\n");
        return value_incref(XS_NULL_VAL);
    }
    int port = (int)VAL_INT(a[0]);
    Value *handler = a[1];

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("http.serve: socket"); return value_incref(XS_NULL_VAL); }
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("http.serve: bind"); close(lfd);
        return value_incref(XS_NULL_VAL);
    }
    if (listen(lfd, 16) < 0) {
        perror("http.serve: listen"); close(lfd);
        return value_incref(XS_NULL_VAL);
    }
    fprintf(stderr, "http.serve: listening on :%d\n", port);

    for (;;) {
        struct sockaddr_in cli = {0};
        socklen_t clen = sizeof cli;
        /* Release the GIL while waiting for a connection so spawned
           XS tasks can run; reacquire on accept return. */
        xs_gil_release();
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clen);
        xs_gil_acquire();
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char buf[16384];
        /* Release the GIL while reading from the socket. */
        xs_gil_release();
        int got = (int)recv(cfd, buf, sizeof(buf) - 1, 0);
        xs_gil_acquire();
        if (got <= 0) { close(cfd); continue; }
        buf[got] = 0;

        /* Parse request line: METHOD PATH VERSION\r\n */
        char method[16] = "GET", url[2048] = "/", version[16] = "HTTP/1.1";
        sscanf(buf, "%15s %2047s %15s", method, url, version);

        /* Split path/query */
        char path[2048] = {0}, query[2048] = {0};
        const char *qmark = strchr(url, '?');
        if (qmark) {
            size_t plen = (size_t)(qmark - url);
            if (plen >= sizeof path) plen = sizeof path - 1;
            memcpy(path, url, plen); path[plen] = 0;
            strncpy(query, qmark + 1, sizeof query - 1);
        } else {
            strncpy(path, url, sizeof path - 1);
        }

        /* Collect headers until blank line */
        Value *hmap = xs_map_new();
        char *p = strstr(buf, "\r\n");
        if (p) p += 2;
        while (p && *p && !(p[0] == '\r' && p[1] == '\n')) {
            char *eol = strstr(p, "\r\n");
            if (!eol) break;
            char *colon = memchr(p, ':', (size_t)(eol - p));
            if (colon) {
                size_t nlen = (size_t)(colon - p);
                char name[256]; if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, p, nlen); name[nlen] = 0;
                char *v = colon + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)(eol - v);
                char val[4096]; if (vlen >= sizeof val) vlen = sizeof val - 1;
                memcpy(val, v, vlen); val[vlen] = 0;
                Value *sv = xs_str(val);
                map_set(hmap->map, name, sv);
                value_decref(sv);
            }
            p = eol + 2;
        }

        /* Body starts after \r\n\r\n */
        const char *body = "";
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) body = body_start + 4;

        /* Build request map */
        Value *req = xs_map_new();
        Value *mv = xs_str(method); map_set(req->map, "method", mv); value_decref(mv);
        Value *pv = xs_str(path);   map_set(req->map, "path",   pv); value_decref(pv);
        Value *qv = xs_str(query);  map_set(req->map, "query",  qv); value_decref(qv);
        map_set(req->map, "headers", hmap); value_decref(hmap);
        Value *bv = xs_str(body);   map_set(req->map, "body",   bv); value_decref(bv);

        Value *args[1] = { req };
        Value *res = call_value(ig, handler, args, 1, "http.serve handler");
        value_decref(req);

        int status = 200;
        const char *rbody = "";
        XSMap *rheaders = NULL;
        if (res && (VAL_TAG(res) == XS_MAP || VAL_TAG(res) == XS_MODULE) && res->map) {
            Value *sv = map_get(res->map, "status");
            if (sv && VAL_TAG(sv) == XS_INT) status = (int)VAL_INT(sv);
            Value *bv2 = map_get(res->map, "body");
            if (bv2 && VAL_TAG(bv2) == XS_STR) rbody = bv2->s;
            Value *hv = map_get(res->map, "headers");
            if (hv && VAL_TAG(hv) == XS_MAP && hv->map) rheaders = hv->map;
        } else if (res && VAL_TAG(res) == XS_STR) {
            rbody = res->s;
        }

        char resp_hdr[1024];
        int rbody_len = (int)strlen(rbody);
        int hlen = snprintf(resp_hdr, sizeof resp_hdr,
            "HTTP/1.1 %d OK\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n",
            status, rbody_len);
        if (rheaders) {
            int nk = 0; char **ks = map_keys(rheaders, &nk);
            for (int k = 0; k < nk && hlen < (int)sizeof resp_hdr - 64; k++) {
                Value *vv = map_get(rheaders, ks[k]);
                if (vv && VAL_TAG(vv) == XS_STR) {
                    hlen += snprintf(resp_hdr + hlen, sizeof resp_hdr - hlen,
                                     "%s: %s\r\n", ks[k], vv->s);
                }
            }
            if (ks) {
                for (int k = 0; k < nk; k++) free(ks[k]);
                free(ks);
            }
        }
        hlen += snprintf(resp_hdr + hlen, sizeof resp_hdr - hlen, "\r\n");
        /* Release the GIL during the write phase too. */
        xs_gil_release();
        if (send(cfd, resp_hdr, hlen, 0) < 0) { /* ignore */ }
        if (rbody_len > 0) {
            if (send(cfd, rbody, rbody_len, 0) < 0) { /* ignore */ }
        }
        close(cfd);
        xs_gil_acquire();
        if (res) value_decref(res);
    }
    close(lfd);
    return value_incref(XS_NULL_VAL);
#endif
}

Value *make_http_module(void) {
    XSMap *m=map_new();
    map_take(m,"get",     xs_native(native_http_get));
    map_take(m,"post",    xs_native(native_http_post));
    map_take(m,"put",     xs_native(native_http_put));
    map_take(m,"delete",  xs_native(native_http_delete));
    map_take(m,"patch",   xs_native(native_http_patch));
    map_take(m,"request", xs_native(native_http_request));
    map_take(m,"serve",   xs_native(native_http_serve));
    return xs_module(m);
}

Value *make_fs_module(void) {
    XSMap *m = map_new();
    map_take(m, "read",         xs_native(native_fs_read));
    map_take(m, "read_bytes",   xs_native(native_fs_read_bytes));
    map_take(m, "write",        xs_native(native_fs_write));
    map_take(m, "write_bytes",  xs_native(native_fs_write_bytes));
    map_take(m, "append",       xs_native(native_fs_append));
    map_take(m, "exists",       xs_native(native_fs_exists));
    map_take(m, "remove",       xs_native(native_fs_remove));
    map_take(m, "mkdir",        xs_native(native_fs_mkdir));
    map_take(m, "mkdir_p",      xs_native(native_fs_mkdir_p));
    map_take(m, "rmdir",        xs_native(native_fs_rmdir));
    map_take(m, "list",         xs_native(native_fs_ls));
    map_take(m, "ls",           xs_native(native_fs_ls));
    map_take(m, "is_dir",       xs_native(native_fs_is_dir));
    map_take(m, "is_file",      xs_native(native_fs_is_file));
    map_take(m, "size",         xs_native(native_fs_size));
    map_take(m, "stat",         xs_native(native_fs_stat));
    map_take(m, "rename",       xs_native(native_fs_rename));
    map_take(m, "copy",         xs_native(native_fs_copy));
    map_take(m, "join",         xs_native(native_fs_join));
    map_take(m, "basename",     xs_native(native_fs_basename));
    map_take(m, "dirname",      xs_native(native_fs_dirname));
    map_take(m, "ext",          xs_native(native_fs_ext));
    map_take(m, "abs",          xs_native(native_fs_abs));
    map_take(m, "temp_dir",     xs_native(native_fs_temp_dir));
    map_take(m, "temp_file",    xs_native(native_fs_temp_file));
    map_take(m, "read_stream",  xs_native(native_fs_read_stream));
    map_take(m, "write_stream", xs_native(native_fs_write_stream));
    map_take(m, "read_lines",   xs_native(native_fs_read_lines));
    map_take(m, "walk",         xs_native(native_fs_walk));
    map_take(m, "glob",         xs_native(native_fs_glob));
    map_take(m, "chmod",        xs_native(native_fs_chmod));
    map_take(m, "symlink",      xs_native(native_fs_symlink));
    map_take(m, "readlink",     xs_native(native_fs_readlink));
    map_take(m, "realpath",     xs_native(native_fs_realpath));
    map_take(m, "watch",        xs_native(native_fs_watch));
    return xs_module(m);
}
