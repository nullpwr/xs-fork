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
#include "runtime/triggers.h"
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
int    g_xs_user_argc = 0;
char **g_xs_user_argv = NULL;

void xs_set_argv(int argc, char **argv) {
    g_xs_argc = argc;
    g_xs_argv = argv;
    g_xs_user_argc = 0;
    g_xs_user_argv = argv + argc;
}

void xs_set_user_args(int argc, char **argv) {
    g_xs_user_argc = argc;
    g_xs_user_argv = argv;
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

char *inst_to_str_export(Interp *interp, Value *v, int repr_mode);

static char *inst_to_str(Interp *interp, Value *v, int repr_mode) {
    /* The vm stores class instances as XS_MAP {__type: "Foo", ...} so
       __str__ / __repr__ / to_string have to be looked up on the map.
       Without this, vm-mode println(some_instance) skipped over
       user-defined __str__ and printed the raw map repr. */
    if (v && VAL_TAG(v) == XS_MAP && v->map &&
        map_get(v->map, "__type")) {
        const char *mname = repr_mode ? "__repr__" : "__str__";
        Value *fn = map_get(v->map, mname);
        if (!fn && !repr_mode) fn = map_get(v->map, "to_string");
        if (fn && (VAL_TAG(fn) == XS_FUNC || VAL_TAG(fn) == XS_CLOSURE ||
                   VAL_TAG(fn) == XS_NATIVE)) {
            Value *call_args[1] = { v };
            Value *result = call_value(interp, fn, call_args, 1, mname);
            if (result && VAL_TAG(result) == XS_STR) {
                char *s = xs_strdup(result->s);
                value_decref(result);
                return s;
            }
            if (result) value_decref(result);
        }
    }
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

char *inst_to_str_export(Interp *interp, Value *v, int repr_mode) {
    return inst_to_str(interp, v, repr_mode);
}

static Value *builtin_print_impl(Interp *i, Value **args, int argc, int with_newline) {
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
            if (with_newline) printf("\n");
            return value_incref(XS_NULL_VAL);
        }
    }
    for (int j = 0; j < argc; j++) {
        if (j) printf(" ");
        char *s = inst_to_str(i, args[j], 0);
        printf("%s", s);
        free(s);
    }
    if (with_newline) printf("\n");
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_print(Interp *i, Value **args, int argc) {
    return builtin_print_impl(i, args, argc, 0);
}

static Value *builtin_println(Interp *i, Value **args, int argc) {
    return builtin_print_impl(i, args, argc, 1);
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

static Value *builtin_eprint_impl(Interp *i, Value **args, int argc, int with_newline) {
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
            if (with_newline) fprintf(stderr,"\n");
            return value_incref(XS_NULL_VAL);
        }
    }
    for (int j = 0; j < argc; j++) {
        if (j) fprintf(stderr," ");
        char *s = value_str(args[j]);
        fprintf(stderr,"%s",s); free(s);
    }
    if (with_newline) fprintf(stderr,"\n");
    return value_incref(XS_NULL_VAL);
}

static Value *builtin_eprint(Interp *i, Value **args, int argc) {
    return builtin_eprint_impl(i, args, argc, 0);
}

static Value *builtin_eprintln(Interp *i, Value **args, int argc) {
    return builtin_eprint_impl(i, args, argc, 1);
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
    case XS_DURATION: return xs_str("duration");
    case XS_STR:    return xs_str("str");
    case XS_CHAR:   return xs_str("char");
    case XS_ARRAY:  return xs_str("array");
    case XS_TUPLE:  return xs_str("tuple");
    case XS_MAP: {
        /* The VM stores struct/class instances as XS_MAP tagged with
           __type so the constructor can be a map literal. Return the
           tagged type name so `type(Foo())` is "Foo" on every backend
           rather than "map" under --vm and "Foo" under --interp. */
        Value *tn = args[0]->map ? map_get(args[0]->map, "__type") : NULL;
        if (tn && VAL_TAG(tn) == XS_STR && tn->s && tn->s[0])
            return xs_str(tn->s);
        return xs_str("map");
    }
    case XS_FUNC:   return xs_str("fn");
    case XS_NATIVE: return xs_str("fn");
    case XS_CLOSURE: return xs_str("fn");
    /* Named `fn h(){}` under --interp wraps the function in an
       XS_OVERLOAD set of size 1 (so adding a second arity later is
       transparent). The wrapper is still callable like a fn -- report
       the same name so backend choice doesn't change type() output. */
    case XS_OVERLOAD: return xs_str("fn");
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
    case XS_DURATION: return xs_str("Duration");
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

/* Helper for spread calls: __xs_call_with_array(fn, [a, b, c]) calls
   fn with the array's elements as positional arguments. The VM
   lowering for `f(...arr)` emits this so the static argc immediate on
   OP_CALL stays correct -- the array is built (with all spreads
   inline-expanded) and unpacked by the helper. */
static Value *builtin_xs_call_with_array(Interp *interp, Value **args, int argc) {
    if (argc < 1 || !args[0]) return value_incref(XS_NULL_VAL);
    Value *callee = args[0];
    Value *arr = (argc >= 2) ? args[1] : NULL;
    int n = (arr && (VAL_TAG(arr) == XS_ARRAY || VAL_TAG(arr) == XS_TUPLE) &&
             arr->arr) ? arr->arr->len : 0;
    Value **pargs = n ? xs_malloc(sizeof(Value*) * n) : NULL;
    for (int j = 0; j < n; j++) pargs[j] = arr->arr->items[j];
    Value *r = call_value(interp, callee, pargs, n, "apply");
    free(pargs);
    return r ? r : value_incref(XS_NULL_VAL);
}

/* Apply an f-string format spec like "%.2f" / ">5" / "x" / "b" / "%".
 * Mirrors the Python mini-language: alignment + width + precision +
 * type. Unknown specs leave the value as-is (default str). */
Value *builtin_xs_fmt_export(Interp *i, Value **args, int argc);
static Value *builtin_xs_fmt(Interp *i, Value **args, int argc) {
    if (argc < 1) return xs_str("");
    Value *v = args[0];
    const char *spec = (argc >= 2 && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : "";
    int slen = (int)strlen(spec);

    char align = 0;
    char fill = ' ';
    char sign = 0;       /* '+', '-', ' ', or 0 */
    int alt_form = 0;    /* '#' alternate form (0b/0o/0x prefixes) */
    int zero_pad = 0;    /* leading '0' before width: sign-aware zero fill */
    int width = 0;
    int precision = -1;
    char type = 0;
    int has_pct = 0;
    int comma = 0;

    int sp = 0;
    if (slen >= 2 && (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
        fill = spec[0];
        align = spec[1];
        sp = 2;
    } else if (slen >= 1 && (spec[0] == '<' || spec[0] == '>' || spec[0] == '^')) {
        align = spec[0];
        sp = 1;
    }
    if (sp < slen && (spec[sp] == '+' || spec[sp] == '-' || spec[sp] == ' ')) {
        sign = spec[sp]; sp++;
    }
    if (sp < slen && spec[sp] == '#') { alt_form = 1; sp++; }
    if (sp < slen && spec[sp] == '0' && !align) {
        zero_pad = 1; fill = '0'; align = '>'; sp++;
    }
    while (sp < slen && spec[sp] >= '0' && spec[sp] <= '9') {
        width = width * 10 + (spec[sp] - '0'); sp++;
    }
    if (sp < slen && spec[sp] == ',') { comma = 1; sp++; }
    if (sp < slen && spec[sp] == '.') {
        sp++;
        precision = 0;
        while (sp < slen && spec[sp] >= '0' && spec[sp] <= '9') {
            precision = precision * 10 + (spec[sp] - '0'); sp++;
        }
    }
    if (sp < slen) {
        if (spec[sp] == '%') { has_pct = 1; type = 'f'; if (precision < 0) precision = 2; }
        else type = spec[sp];
    }

    char buf[256];
    char *body = NULL;
    if (type == 'x' || type == 'X' || type == 'b' || type == 'o') {
        int64_t iv = (VAL_TAG(v) == XS_INT) ? VAL_INT(v)
                    : (VAL_TAG(v) == XS_FLOAT) ? (int64_t)v->f : 0;
        if (type == 'x') snprintf(buf, sizeof(buf), "%llx", (long long)iv);
        else if (type == 'X') snprintf(buf, sizeof(buf), "%llX", (long long)iv);
        else if (type == 'o') snprintf(buf, sizeof(buf), "%llo", (long long)iv);
        else {
            uint64_t uv = (uint64_t)(iv < 0 ? -iv : iv);
            char tmp[68]; int p = 66; tmp[66] = '\0';
            if (uv == 0) tmp[--p] = '0';
            while (uv) { tmp[--p] = (uv & 1) ? '1' : '0'; uv >>= 1; }
            if (iv < 0) tmp[--p] = '-';
            snprintf(buf, sizeof(buf), "%s", tmp + p);
        }
        body = xs_strdup(buf);
        if (alt_form) {
            const char *prefix =
                type == 'x' ? "0x" : type == 'X' ? "0X" :
                type == 'o' ? "0o" : "0b";
            int neg = (body[0] == '-');
            int bl = (int)strlen(body);
            char *nb = xs_malloc(bl + 3);
            int wi = 0;
            if (neg) nb[wi++] = '-';
            nb[wi++] = prefix[0]; nb[wi++] = prefix[1];
            memcpy(nb + wi, body + neg, (size_t)(bl - neg));
            nb[wi + bl - neg] = '\0';
            free(body); body = nb;
        }
    } else if (type == 'f' || (precision >= 0 && (VAL_TAG(v) == XS_FLOAT || VAL_TAG(v) == XS_INT))) {
        double fv = (VAL_TAG(v) == XS_FLOAT) ? v->f
                  : (VAL_TAG(v) == XS_INT) ? (double)VAL_INT(v) : 0.0;
        if (has_pct) fv *= 100.0;
        int p = precision >= 0 ? precision : 6;
        snprintf(buf, sizeof(buf), "%.*f", p, fv);
        body = xs_strdup(buf);
        if (has_pct) {
            int bl = (int)strlen(body);
            body = xs_realloc(body, bl + 2);
            body[bl] = '%'; body[bl+1] = '\0';
        }
    } else if (type == 'e' || type == 'E') {
        double fv = (VAL_TAG(v) == XS_FLOAT) ? v->f
                  : (VAL_TAG(v) == XS_INT) ? (double)VAL_INT(v) : 0.0;
        int p = precision >= 0 ? precision : 6;
        snprintf(buf, sizeof(buf), type == 'e' ? "%.*e" : "%.*E", p, fv);
        body = xs_strdup(buf);
    } else {
        body = inst_to_str(i, v, 0);
        if (precision >= 0 && body) {
            int bl = (int)strlen(body);
            if (bl > precision) body[precision] = '\0';
        }
    }
    if (comma && body) {
        int neg = (body[0] == '-') ? 1 : 0;
        char *dot = strchr(body, '.');
        int int_end = dot ? (int)(dot - body) : (int)strlen(body);
        int int_len = int_end - neg;
        int n_commas = (int_len - 1) / 3;
        if (n_commas > 0) {
            int new_len = (int)strlen(body) + n_commas + 1;
            char *nb = xs_malloc(new_len);
            int wi = 0;
            if (neg) nb[wi++] = '-';
            int first = int_len % 3; if (first == 0) first = 3;
            for (int j = 0; j < int_len; j++) {
                if (j > 0 && (j - first) % 3 == 0) nb[wi++] = ',';
                nb[wi++] = body[neg + j];
            }
            int tail_len = (int)strlen(body) - int_end;
            memcpy(nb + wi, body + int_end, tail_len);
            wi += tail_len;
            nb[wi] = '\0';
            free(body); body = nb;
        }
    }
    if (!body) body = xs_strdup("");

    /* Sign prefix: '+' forces a sign on non-negatives; ' ' uses a space
       in front of non-negatives; '-' is the default. Numbers already
       carry a leading '-' when negative, so we only synthesise the
       prefix for non-negative numeric output. */
    int is_numeric = (type == 'd' || type == 'f' || type == 'e' ||
                      type == 'E' || type == 'g' || type == 'G' ||
                      type == 'x' || type == 'X' || type == 'b' ||
                      type == 'o' || type == 0 ||
                      VAL_TAG(v) == XS_INT || VAL_TAG(v) == XS_FLOAT);
    if (sign && is_numeric && body[0] != '-' &&
        body[0] != '\0' && (sign == '+' || sign == ' ')) {
        int bl = (int)strlen(body);
        char *nb = xs_malloc(bl + 2);
        nb[0] = sign;
        memcpy(nb + 1, body, (size_t)(bl + 1));
        free(body); body = nb;
    }

    int blen = (int)strlen(body);
    if (width > blen) {
        int pad = width - blen;
        char *r;
        if (align == '<') {
            r = xs_malloc(width + 1);
            memcpy(r, body, blen);
            for (int j = 0; j < pad; j++) r[blen + j] = fill;
            r[width] = '\0';
        } else if (align == '^') {
            int lpad = pad / 2, rpad = pad - lpad;
            r = xs_malloc(width + 1);
            for (int j = 0; j < lpad; j++) r[j] = fill;
            memcpy(r + lpad, body, blen);
            for (int j = 0; j < rpad; j++) r[lpad + blen + j] = fill;
            r[width] = '\0';
        } else if (zero_pad && is_numeric) {
            /* Sign- and alt-form-aware zero pad: keep any sign and the
               `0x`/`0b`/`0o` prefix anchored at the front, fill between
               the prefix and digits with '0'. Without this `{-42:05}`
               would render `00-42` and `{42:#08x}` `00000x2a`. */
            int prefix_len = 0;
            int neg = (body[0] == '-' || body[0] == '+' || body[0] == ' ');
            if (neg) prefix_len = 1;
            if (alt_form && (type == 'x' || type == 'X' ||
                             type == 'b' || type == 'o') &&
                blen - prefix_len >= 2 &&
                body[prefix_len] == '0' &&
                (body[prefix_len+1] == 'x' || body[prefix_len+1] == 'X' ||
                 body[prefix_len+1] == 'b' || body[prefix_len+1] == 'o')) {
                prefix_len += 2;
            }
            r = xs_malloc(width + 1);
            for (int j = 0; j < prefix_len; j++) r[j] = body[j];
            for (int j = 0; j < pad; j++) r[prefix_len + j] = '0';
            memcpy(r + prefix_len + pad, body + prefix_len,
                   (size_t)(blen - prefix_len));
            r[width] = '\0';
        } else {
            r = xs_malloc(width + 1);
            for (int j = 0; j < pad; j++) r[j] = fill;
            memcpy(r + pad, body, blen);
            r[width] = '\0';
        }
        free(body); body = r;
    }
    Value *out = xs_str(body);
    free(body);
    return out;
}

Value *builtin_abs(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1) return xs_int(0);
    if (VAL_TAG(args[0])==XS_INT) {
        int64_t v = VAL_INT(args[0]);
        if (v == INT64_MIN) {
            /* -INT64_MIN doesn't fit in int64; promote to bigint. */
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
    /* map() -> empty map, map(arr, fn) -> mapped array.
       The fn-arg type check covers OVERLOAD too because top-level
       `fn foo(...)` declarations end up as overload sets after the
       hoist+stmt double pass. */
    if (argc == 0) return xs_map_new();
    int fn_ok = argc >= 2 &&
        (VAL_TAG(args[1]) == XS_FUNC || VAL_TAG(args[1]) == XS_NATIVE ||
         VAL_TAG(args[1]) == XS_CLOSURE || VAL_TAG(args[1]) == XS_OVERLOAD);
    if (argc >= 2 && (VAL_TAG(args[0]) == XS_ARRAY || VAL_TAG(args[0]) == XS_TUPLE) && fn_ok) {
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
/* Render `v` per a Python-like spec: optional positional index has been
   parsed by the caller; `spec` is just whatever's after the colon (may
   be empty). Supports: '<' '>' '^' alignment, '0' zero-pad, width, '.'
   precision, type 'd' 'x' 'X' 'b' 'o' 'f' 'e' 'g' 's'. */
static char *format_value_with_spec(Value *v, const char *spec) {
    char fill = ' ';
    char align = '\0';   /* default depends on type */
    int  zero_pad = 0;
    int  width = 0;
    int  prec = -1;
    char type = '\0';
    const char *s = spec;
    if (s[0] && (s[1] == '<' || s[1] == '>' || s[1] == '^')) {
        fill = s[0]; align = s[1]; s += 2;
    } else if (*s == '<' || *s == '>' || *s == '^') {
        align = *s++;
    }
    if (*s == '0') { zero_pad = 1; s++; }
    while (*s >= '0' && *s <= '9') { width = width * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++; prec = 0;
        while (*s >= '0' && *s <= '9') { prec = prec * 10 + (*s - '0'); s++; }
    }
    if (*s) type = *s;
    char buf[128];
    char *body;
    int  bodylen;
    int free_body = 0;
    if ((type == 'd' || type == 'x' || type == 'X' || type == 'b' || type == 'o')
        && (VAL_TAG(v) == XS_INT || VAL_TAG(v) == XS_BOOL)) {
        long long iv = VAL_INT(v);
        if (type == 'b') {
            char tmp[80]; int n = 0;
            unsigned long long u = (unsigned long long)(iv < 0 ? -iv : iv);
            if (u == 0) tmp[n++] = '0';
            while (u) { tmp[n++] = '0' + (u & 1); u >>= 1; }
            int neg = iv < 0;
            int total = n + (neg ? 1 : 0);
            if (total >= (int)sizeof(buf)) total = sizeof(buf) - 1;
            int j = 0;
            if (neg) buf[j++] = '-';
            for (int k = n - 1; k >= 0 && j < (int)sizeof(buf) - 1; k--) buf[j++] = tmp[k];
            buf[j] = '\0';
            bodylen = j;
        } else {
            const char *fc = type == 'x' ? "%llx"
                            : type == 'X' ? "%llX"
                            : type == 'o' ? "%llo"
                            : "%lld";
            bodylen = snprintf(buf, sizeof buf, fc, iv);
        }
        body = buf;
    } else if (VAL_TAG(v) == XS_FLOAT
               || type == 'f' || type == 'e' || type == 'g') {
        double f = (VAL_TAG(v) == XS_FLOAT) ? v->f
                  : (VAL_TAG(v) == XS_INT) ? (double)VAL_INT(v)
                  : 0.0;
        const char *fc;
        char fmtbuf[16];
        if (type == 'e') {
            snprintf(fmtbuf, sizeof fmtbuf, "%%.%de", prec >= 0 ? prec : 6);
            fc = fmtbuf;
        } else if (type == 'g') {
            snprintf(fmtbuf, sizeof fmtbuf, "%%.%dg", prec >= 0 ? prec : 6);
            fc = fmtbuf;
        } else {
            snprintf(fmtbuf, sizeof fmtbuf, "%%.%df", prec >= 0 ? prec : 6);
            fc = fmtbuf;
        }
        bodylen = snprintf(buf, sizeof buf, fc, f);
        body = buf;
    } else {
        /* string-ish: precision truncates, default left-align */
        char *vs = value_str(v);
        bodylen = (int)strlen(vs);
        if (prec >= 0 && bodylen > prec) bodylen = prec;
        body = vs;
        free_body = 1;
    }
    /* default alignment: numbers right, strings left. */
    if (align == '\0') {
        align = (VAL_TAG(v) == XS_INT || VAL_TAG(v) == XS_FLOAT
                 || VAL_TAG(v) == XS_BOOL) ? '>' : '<';
    }
    int pad = width > bodylen ? width - bodylen : 0;
    char padc = zero_pad && (align == '>') ? '0' : fill;
    char *out = xs_malloc((size_t)(bodylen + pad + 1));
    int op = 0;
    if (align == '>') {
        for (int j = 0; j < pad; j++) out[op++] = padc;
        memcpy(out + op, body, bodylen); op += bodylen;
    } else if (align == '<') {
        memcpy(out + op, body, bodylen); op += bodylen;
        for (int j = 0; j < pad; j++) out[op++] = padc;
    } else { /* '^' */
        int lp = pad / 2, rp = pad - lp;
        for (int j = 0; j < lp; j++) out[op++] = padc;
        memcpy(out + op, body, bodylen); op += bodylen;
        for (int j = 0; j < rp; j++) out[op++] = padc;
    }
    out[op] = '\0';
    if (free_body) free(body);
    return out;
}

Value *builtin_format(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc<1||VAL_TAG(args[0])!=XS_STR) return xs_str("");
    const char *fmt = args[0]->s;
    int argidx = 1;
    char *result = xs_strdup(""); int rlen = 0;
    for (const char *p = fmt; *p; ) {
        if (*p == '{' && *(p+1) == '{') {
            result = xs_realloc(result, rlen + 2);
            result[rlen++] = '{'; result[rlen] = '\0';
            p += 2;
            continue;
        }
        if (*p == '}' && *(p+1) == '}') {
            result = xs_realloc(result, rlen + 2);
            result[rlen++] = '}'; result[rlen] = '\0';
            p += 2;
            continue;
        }
        if (*p == '{') {
            const char *q = p + 1;
            int idx = -1;
            while (*q >= '0' && *q <= '9') {
                if (idx < 0) idx = 0;
                idx = idx * 10 + (*q - '0'); q++;
            }
            const char *spec_start = NULL;
            if (*q == ':') { spec_start = q + 1; q = spec_start; while (*q && *q != '}') q++; }
            if (*q != '}') {
                /* malformed; emit literally */
                result = xs_realloc(result, rlen + 2);
                result[rlen++] = *p++; result[rlen] = '\0';
                continue;
            }
            char specbuf[64];
            int speclen = spec_start ? (int)(q - spec_start) : 0;
            if (speclen >= (int)sizeof(specbuf)) speclen = sizeof(specbuf) - 1;
            if (spec_start) memcpy(specbuf, spec_start, speclen);
            specbuf[speclen] = '\0';
            int use_idx = idx >= 0 ? (idx + 1) : argidx++;
            char *piece = (use_idx < argc)
                ? format_value_with_spec(args[use_idx], specbuf)
                : xs_strdup("");
            int slen = (int)strlen(piece);
            result = xs_realloc(result, rlen + slen + 1);
            memcpy(result + rlen, piece, slen + 1); rlen += slen;
            free(piece);
            p = q + 1;
        } else {
            result = xs_realloc(result, rlen + 2);
            result[rlen++] = *p++; result[rlen] = '\0';
        }
    }
    Value *v = xs_str(result); free(result); return v;
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
    (void)i;
    if (argc < 2) {
        xs_runtime_error(span_zero(), "AssertionError", NULL, "assert_eq requires 2 arguments");
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
        char detail[512];
        if (msg[0])
            snprintf(detail, sizeof detail, "assert_eq(%s, %s): %s", a, b, msg);
        else
            snprintf(detail, sizeof detail, "assert_eq(%s, %s)", a, b);
        free(a); free(b);
        xs_runtime_error(span_zero(), "AssertionError", NULL, "assertion failed: %s", detail);
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
Value *native_channel_recv_pair(Interp *ig, Value **a, int n);
Value *native_channel_try_recv(Interp *ig, Value **a, int n);
Value *native_channel_close(Interp *ig, Value **a, int n);
Value *native_channel_is_closed(Interp *ig, Value **a, int n);
Value *native_channel_len(Interp *ig, Value **a, int n);
Value *native_channel_cap(Interp *ig, Value **a, int n);
Value *native_channel_is_empty(Interp *ig, Value **a, int n);
Value *native_channel_is_full(Interp *ig, Value **a, int n);

static Value *builtin_channel(Interp *i, Value **args, int argc) {
    (void)i;
    /* `channel()` returns a real concurrent channel: send wakes
       blocked recvs, recv blocks while empty (releasing the GIL so a
       sender on another thread can run). With a positive cap, send
       blocks while the buffer is full. */
    int cap = 0;
    if (argc > 0 && args[0]) {
        if (VAL_TAG(args[0]) == XS_INT) cap = (int)VAL_INT(args[0]);
        else if (VAL_TAG(args[0]) == XS_FLOAT) cap = (int)args[0]->f;
    }
    int chid = xs_chan_alloc(cap);
    Value *ch = xs_map_new();
    Value *t = xs_str("Channel");        map_set(ch->map,"_type",t);     value_decref(t);
    Value *idv = xs_int(chid);           map_set(ch->map,"_chan_id",idv); value_decref(idv);
    Value *capv = xs_int(cap);           map_set(ch->map,"_cap",capv);    value_decref(capv);
    Value *data = xs_array_new();        map_set(ch->map,"_buf",data);    value_decref(data);
    map_take(ch->map, "send", xs_native(native_channel_send));
    map_take(ch->map, "recv", xs_native(native_channel_recv));
    map_take(ch->map, "recv_pair", xs_native(native_channel_recv_pair));
    map_take(ch->map, "try_recv", xs_native(native_channel_try_recv));
    map_take(ch->map, "close", xs_native(native_channel_close));
    map_take(ch->map, "is_closed", xs_native(native_channel_is_closed));
    map_take(ch->map, "len", xs_native(native_channel_len));
    map_take(ch->map, "cap", xs_native(native_channel_cap));
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
Value *make_toml_module(void);
Value *make_http_module(void);
Value *make_fs_module(void);

/* register all */
void stdlib_register(Interp *i) {
    /* Core */
    interp_define_native(i, "print",     builtin_print);
    interp_define_native(i, "println",   builtin_println);
    interp_define_native(i, "eprint",    builtin_eprint);
    interp_define_native(i, "eprintln",  builtin_eprintln);
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
    interp_define_native(i, "__xs_fmt",  builtin_xs_fmt);
    interp_define_native(i, "__xs_call_with_array", builtin_xs_call_with_array);
    {
        extern Value *builtin_wrap_memoize_export(Interp *, Value **, int);
        extern Value *builtin_wrap_retry_export(Interp *, Value **, int);
        extern Value *builtin_wrap_trace_export(Interp *, Value **, int);
        extern Value *builtin_wrap_timed_export(Interp *, Value **, int);
        interp_define_native(i, "__wrap_memoize", builtin_wrap_memoize_export);
        interp_define_native(i, "__wrap_retry",   builtin_wrap_retry_export);
        interp_define_native(i, "__wrap_trace",   builtin_wrap_trace_export);
        interp_define_native(i, "__wrap_timed",   builtin_wrap_timed_export);
    }
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
    interp_define_native(i, "__trigger_registry_size", trigger_native_size);
    interp_define_native(i, "__trigger_registry_name", trigger_native_name);
    interp_define_native(i, "__register_decorator",    trigger_native_register);
    interp_define_native(i, "copy",      builtin_copy);
    interp_define_native(i, "clone",     builtin_clone);
    interp_define_native(i, "ord",       builtin_ord);
    interp_define_native(i, "chr",       builtin_chr);
    interp_define_native(i, "type_of",   builtin_type_of);
    interp_define_native(i, "contains",  builtin_contains);
    interp_define_native(i, "channel",   builtin_channel);
    {
        extern Value *native_async_select(Interp *, Value **, int);
        interp_define_native(i, "select", native_async_select);
    }

    /* Result / Option constructors */
    interp_define_native(i, "Ok",   builtin_ok);
    interp_define_native(i, "Err",  builtin_err);
    interp_define_native(i, "Some", builtin_some);
    interp_define_native(i, "None", builtin_none_fn);

    /* Stdlib modules used to be auto-bound on every Interp init; now
       they require an explicit `import math` / `import os` / etc. The
       lookup table for lazy materialisation lives in stdlib_load_module
       at the bottom of this file. The one bit of init that still has
       to happen at startup is the random seed, since that's
       process-wide rather than per-module. */
    srand((unsigned)time(NULL));

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
    /* Modules also gated on import; see stdlib_load_module below. */
}

/* Lazy stdlib materialisation. The import statement (NODE_IMPORT in
   the interp, OP_IMPORT in the VM) calls in here when env_get
   doesn't find the name -- we either build the module fresh and
   hand it back, or return null so the caller can fall through to
   plugin / file-based modules. */
Value *stdlib_load_module(Interp *i, const char *name) {
    if (!name) return NULL;

    /* The io module patches read_json / write_json onto its map
       after construction so it can borrow json's parser. */
    if (strcmp(name, "io") == 0) {
        Value *m = make_io_module();
        extern Value *native_io_read_json(Interp *, Value **, int);
        extern Value *native_io_write_json(Interp *, Value **, int);
        map_take(m->map, "read_json",  xs_native(native_io_read_json));
        map_take(m->map, "write_json", xs_native(native_io_write_json));
        return m;
    }
    /* os reads g_xs_argc / g_xs_argv but doesn't actually need the
       interp pointer passed in; we forward it so the existing
       signature stays untouched. */
    if (strcmp(name, "os") == 0) return make_os_module(i);

    /* Plain factories: name -> constructor. The list mirrors the
       module set that stdlib_register / vm globals used to seed. */
    struct StdMod { const char *n; Value *(*make)(void); };
    static const struct StdMod stdmods[] = {
        { "math",        make_math_module },
        { "time",        make_time_module },
        { "string",      make_string_module },
        { "path",        make_path_module },
        { "base64",      make_base64_module },
        { "hash",        make_hash_module },
        { "uuid",        make_uuid_module },
        { "collections", make_collections_module },
        { "process",     make_process_module },
        { "random",      make_random_module },
        { "json",        make_json_module },
        { "log",         make_log_module },
        { "fmt",         make_fmt_module },
        { "test",        make_test_module },
        { "tracing",     make_tracing_module },
        { "csv",         make_csv_module },
        { "url",         make_url_module },
        { "re",          make_re_module },
        { "msgpack",     make_msgpack_module },
        { "Promise",     make_promise_module },
        { "async",       make_async_module },
        { "net",         make_net_module },
        { "crypto",      make_crypto_module },
        { "thread",      make_thread_module },
        { "buf",         make_buf_module },
        { "encode",      make_encode_module },
        { "db",          make_db_module },
        { "cli",         make_cli_module },
        { "ffi",         make_ffi_module },
        { "reflect",     make_reflect_module },
        { "gc",          make_gc_module },
        { "toml",        make_toml_module },
        { "http",        make_http_module },
        { "fs",          make_fs_module },
        { NULL,          NULL }
    };
    for (const struct StdMod *m = stdmods; m->n; m++) {
        if (strcmp(m->n, name) == 0) return m->make();
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  12 NEW STDLIB MODULES
 * ═══════════════════════════════════════════════════════════════ */












Value *builtin_xs_fmt_export(Interp *i, Value **args, int argc) {
    return builtin_xs_fmt(i, args, argc);
}

Value *builtin_xs_call_with_array_export(Interp *i, Value **args, int argc) {
    return builtin_xs_call_with_array(i, args, argc);
}

/* === Wrapping decorators ============================================
 * The fn_decl runtime path detects @memoize / @retry / @trace / @timed
 * and calls the matching __wrap_*_make below to produce a wrapper map
 * that the call dispatchers (interp call_value, vm OP_CALL) recognise
 * via _wrap_kind. Wrappers carry the original fn in _wrap_fn and
 * whatever per-decorator state they need (cache, retry count, name).
 * The dispatch enters wrap_call_dispatch which runs the wrapper logic
 * and may invoke the original through the same call_value path. */

static Value *wrap_make_base(const char *kind, Value *fn, const char *name) {
    Value *m = xs_map_new();
    Value *kv = xs_str(kind);
    map_set(m->map, "_wrap_kind", kv); value_decref(kv);
    Value *fv = value_incref(fn);
    map_set(m->map, "_wrap_fn", fv); value_decref(fv);
    if (name) {
        Value *nv = xs_str(name);
        map_set(m->map, "_wrap_name", nv); value_decref(nv);
    }
    return m;
}

static Value *builtin_wrap_memoize(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return value_incref(XS_NULL_VAL);
    const char *name = (argc >= 2 && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : NULL;
    Value *m = wrap_make_base("memoize", args[0], name);
    Value *cache = xs_map_new();
    map_set(m->map, "_wrap_cache", cache); value_decref(cache);
    return m;
}

static Value *builtin_wrap_retry(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return value_incref(XS_NULL_VAL);
    /* The hoist always passes the fn name as the trailing string arg.
       With @retry it lands at args[1]; with @retry(n) it's at args[2].
       Pick whichever trailing slot holds a string so the wrapper map
       carries _wrap_name in both forms. */
    const char *name = NULL;
    if (argc >= 2 && VAL_TAG(args[argc-1]) == XS_STR)
        name = args[argc-1]->s;
    Value *m = wrap_make_base("retry", args[0], name);
    int64_t n = 3;
    if (argc >= 2 && VAL_TAG(args[1]) == XS_INT) n = VAL_INT(args[1]);
    if (n < 1) n = 1;
    Value *nv = xs_int(n);
    map_set(m->map, "_wrap_n", nv); value_decref(nv);
    return m;
}

static Value *builtin_wrap_trace(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return value_incref(XS_NULL_VAL);
    const char *name = (argc >= 2 && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : NULL;
    return wrap_make_base("trace", args[0], name);
}

static Value *builtin_wrap_timed(Interp *i, Value **args, int argc) {
    (void)i;
    if (argc < 1) return value_incref(XS_NULL_VAL);
    const char *name = (argc >= 2 && VAL_TAG(args[1]) == XS_STR) ? args[1]->s : NULL;
    return wrap_make_base("timed", args[0], name);
}

/* Dispatch a call on a wrapper map. Returns NULL if the receiver
   isn't a wrapper (caller falls through to ordinary dispatch). */
Value *wrap_call_dispatch(Interp *interp, Value *wrap, Value **args, int argc) {
    if (!wrap || VAL_TAG(wrap) != XS_MAP || !wrap->map) return NULL;
    Value *kv = map_get(wrap->map, "_wrap_kind");
    if (!kv || VAL_TAG(kv) != XS_STR || !kv->s) return NULL;
    const char *kind = kv->s;
    Value *fn = map_get(wrap->map, "_wrap_fn");
    if (!fn) return value_incref(XS_NULL_VAL);
    Value *name_v = map_get(wrap->map, "_wrap_name");
    const char *name = (name_v && VAL_TAG(name_v) == XS_STR) ? name_v->s : "fn";

    if (strcmp(kind, "memoize") == 0) {
        Value *cache_v = map_get(wrap->map, "_wrap_cache");
        if (!cache_v || VAL_TAG(cache_v) != XS_MAP)
            return call_value(interp, fn, args, argc, name);
        /* Build a key from arg values; their str repr is the storage
           index. Cheap and cycle-free; meant for value-typed inputs. */
        size_t cap = 64;
        char *kbuf = xs_malloc(cap);
        size_t kp = 0;
        kbuf[kp] = '\0';
        for (int j = 0; j < argc; j++) {
            char *as = value_str(args[j]);
            size_t al = strlen(as);
            while (kp + al + 2 > cap) { cap *= 2; kbuf = xs_realloc(kbuf, cap); }
            if (j) kbuf[kp++] = '|';
            memcpy(kbuf + kp, as, al); kp += al;
            kbuf[kp] = '\0';
            free(as);
        }
        Value *cached = map_get(cache_v->map, kbuf);
        if (cached) { Value *r = value_incref(cached); free(kbuf); return r; }
        Value *r = call_value(interp, fn, args, argc, name);
        if (r) map_set(cache_v->map, kbuf, value_incref(r));
        free(kbuf);
        return r;
    }
    if (strcmp(kind, "retry") == 0) {
        Value *nv = map_get(wrap->map, "_wrap_n");
        int64_t n = (nv && VAL_TAG(nv) == XS_INT) ? VAL_INT(nv) : 3;
        Value *r = NULL;
        extern __thread int g_xs_in_try;
        Value *last_exc = NULL;
        int last_threw = 0;
        for (int64_t a = 0; a < n; a++) {
            /* Pre-clear any stale throw left from a previous attempt. */
            if (g_xs_pending_throw) {
                value_decref(g_xs_pending_throw);
                g_xs_pending_throw = NULL;
            }
            if (interp) {
                if (interp->cf.value) value_decref(interp->cf.value);
                interp->cf.value = NULL;
                interp->cf.signal = 0;
            }
            /* Mark this region as inside a try so xs_runtime_error
               parks the throw on g_xs_pending_throw / cf.signal
               instead of unwinding past us. interp counts try_depth;
               vm uses g_xs_in_try. */
            g_xs_in_try++;
            int saved_try_depth = 0;
            if (interp) { saved_try_depth = interp->try_depth; interp->try_depth++; }
            r = call_value(interp, fn, args, argc, name);
            g_xs_in_try--;
            if (interp) interp->try_depth = saved_try_depth;
            int threw = (g_xs_pending_throw != NULL) ||
                        (interp && interp->cf.signal == CF_THROW);
            if (!threw) { last_threw = 0; break; }
            last_threw = 1;
            /* swallow the error so next attempt sees a clean slate;
               keep a copy in case all attempts fail and we re-throw. */
            if (last_exc) { value_decref(last_exc); last_exc = NULL; }
            if (g_xs_pending_throw) {
                last_exc = g_xs_pending_throw;
                g_xs_pending_throw = NULL;
            } else if (interp && interp->cf.value) {
                last_exc = value_incref(interp->cf.value);
            }
            if (interp) {
                interp->cf.signal = 0;
                if (interp->cf.value) {
                    value_decref(interp->cf.value);
                    interp->cf.value = NULL;
                }
            }
            if (r) value_decref(r);
            r = NULL;
        }
        if (last_threw) {
            /* All attempts failed; re-raise the last exception so the
               caller's surrounding try/catch (or top-level) sees it. */
            if (interp) {
                interp->cf.signal = CF_THROW;
                interp->cf.value  = last_exc;
            } else {
                g_xs_pending_throw = last_exc;
            }
            last_exc = NULL;
            if (r) { value_decref(r); r = NULL; }
            return value_incref(XS_NULL_VAL);
        }
        if (last_exc) value_decref(last_exc);
        return r ? r : value_incref(XS_NULL_VAL);
    }
    if (strcmp(kind, "trace") == 0) {
        fprintf(stderr, "[trace] -> %s(", name);
        for (int j = 0; j < argc; j++) {
            char *as = value_str(args[j]);
            fprintf(stderr, "%s%s", j ? ", " : "", as);
            free(as);
        }
        fprintf(stderr, ")\n");
        Value *r = call_value(interp, fn, args, argc, name);
        char *rs = value_str(r);
        fprintf(stderr, "[trace] <- %s = %s\n", name, rs);
        free(rs);
        return r;
    }
    if (strcmp(kind, "timed") == 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        Value *r = call_value(interp, fn, args, argc, name);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "[timed] %s: %.3f ms\n", name, ms);
        return r;
    }
    return call_value(interp, fn, args, argc, name);
}

Value *builtin_wrap_memoize_export(Interp *i, Value **a, int n) { return builtin_wrap_memoize(i, a, n); }
Value *builtin_wrap_retry_export(Interp *i, Value **a, int n) { return builtin_wrap_retry(i, a, n); }
Value *builtin_wrap_trace_export(Interp *i, Value **a, int n) { return builtin_wrap_trace(i, a, n); }
Value *builtin_wrap_timed_export(Interp *i, Value **a, int n) { return builtin_wrap_timed(i, a, n); }
