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
    int r = xs_io_mkdirs(a[0]->s);
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
    int r = xs_io_mkdirs(a[0]->s);
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


extern Value *http_do_request(const char *method, const char *url, XSMap *extra_headers, const char *body, size_t body_len);
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
