#include "xs_embed.h"
#include "core/xs.h"
#include "core/value.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "runtime/interp.h"
#include "core/env.h"
#include "core/limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  New embedding API (XSContext / XSValue / XSResult)
 * ================================================================ */

struct XSContext {
    Interp *interp;
    char    error_buf[512];
};

/* wrap an internal Value* into an XSValue, taking a ref */
static XSValue wrap_value(Value *v) {
    XSValue out;
    memset(&out, 0, sizeof out);
    if (!v || VAL_TAG(v) == XS_NULL) {
        out.tag = XS_VAL_NULL;
        out._internal = NULL;
        return out;
    }
    value_incref(v);
    out._internal = v;
    switch (VAL_TAG(v)) {
    case XS_BOOL:  out.tag = XS_VAL_BOOL;   out.b = (int)VAL_INT(v); break;
    case XS_INT:   out.tag = XS_VAL_INT;    out.i = VAL_INT(v);      break;
    case XS_FLOAT: out.tag = XS_VAL_FLOAT;  out.f = v->f;      break;
    case XS_STR:   out.tag = XS_VAL_STRING; out.s = v->s;      break;
    case XS_ARRAY: out.tag = XS_VAL_ARRAY;                     break;
    case XS_MAP:   out.tag = XS_VAL_MAP;                       break;
    case XS_FUNC:
    case XS_NATIVE:out.tag = XS_VAL_FUNC;                     break;
    default:       out.tag = XS_VAL_OTHER;                     break;
    }
    return out;
}

/* unwrap an XSValue to an internal Value*, borrowing the ref */
static Value *unwrap_value(XSValue *v) {
    if (!v || !v->_internal) {
        /* synthesize from the tag */
        switch (v ? VAL_TAG(v) : XS_VAL_NULL) {
        case XS_VAL_NULL:   return xs_null();
        case XS_VAL_BOOL:   return xs_bool(v->b);
        case XS_VAL_INT:    return xs_int(VAL_INT(v));
        case XS_VAL_FLOAT:  return xs_float(v->f);
        case XS_VAL_STRING: return xs_str(v->s ? v->s : "");
        default:            return xs_null();
        }
    }
    return (Value *)v->_internal;
}

static XSResult make_ok(XSValue val) {
    XSResult r;
    r.ok = 1;
    r.value = val;
    r.error = NULL;
    return r;
}

static XSResult make_err(XSContext *ctx, const char *msg) {
    XSResult r;
    r.ok = 0;
    memset(&r.value, 0, sizeof r.value);
    r.value.tag = XS_VAL_NULL;
    snprintf(ctx->error_buf, sizeof ctx->error_buf, "%s", msg ? msg : "unknown error");
    r.error = ctx->error_buf;
    return r;
}

XSContext *xs_context_new(void) {
    XSContext *ctx = (XSContext *)calloc(1, sizeof(XSContext));
    if (!ctx) return NULL;
    ctx->interp = interp_new("<embed>");
    return ctx;
}

void xs_context_free(XSContext *ctx) {
    if (!ctx) return;
    interp_free(ctx->interp);
    free(ctx);
}

void xs_set_instruction_limit(XSContext *ctx, uint64_t budget) {
    (void)ctx;
    xs_limits_set_instructions(budget);
}

void xs_set_wall_time_limit(XSContext *ctx, uint64_t ms) {
    (void)ctx;
    xs_limits_set_wall_time_ms(ms);
}

void xs_set_memory_limit(XSContext *ctx, uint64_t bytes) {
    (void)ctx;
    xs_limits_set_memory_bytes((size_t)bytes);
}

uint64_t xs_instructions_used(XSContext *ctx) {
    (void)ctx;
    return xs_limits_get_instructions_used();
}

uint64_t xs_rss_bytes(XSContext *ctx) {
    (void)ctx;
    return (uint64_t)xs_limits_get_memory_rss();
}

static XSResult run_source(XSContext *ctx, const char *src, const char *fname) {
    Lexer lex;
    lexer_init(&lex, src, fname);
    TokenArray ta = lexer_tokenize(&lex);

    Parser parser;
    parser_init(&parser, &ta, fname);
    Node *program = parser_parse(&parser);
    token_array_free(&ta);

    if (!program || parser.had_error) {
        const char *msg = parser.had_error ? parser.error.msg : "parse error";
        XSResult r = make_err(ctx, msg);
        if (program) node_free(program);
        return r;
    }

    interp_run(ctx->interp, program);
    node_free(program);

    if (ctx->interp->cf.signal == CF_ERROR || ctx->interp->cf.signal == CF_PANIC) {
        Value *err = ctx->interp->cf.value;
        const char *msg = "runtime error";
        if (err && VAL_TAG(err) == XS_STR) msg = err->s;
        else if (err) {
            char *s = value_repr(err);
            if (s) { snprintf(ctx->error_buf, sizeof ctx->error_buf, "%s", s); free(s); msg = ctx->error_buf; }
        }
        XSResult r = make_err(ctx, msg);
        if (ctx->interp->cf.value) { value_decref(ctx->interp->cf.value); ctx->interp->cf.value = NULL; }
        ctx->interp->cf.signal = 0;
        return r;
    }

    XSValue val;
    memset(&val, 0, sizeof val);
    val.tag = XS_VAL_NULL;
    if (ctx->interp->cf.value && VAL_TAG(ctx->interp->cf.value) != XS_NULL) {
        val = wrap_value(ctx->interp->cf.value);
    }
    if (ctx->interp->cf.value) { value_decref(ctx->interp->cf.value); ctx->interp->cf.value = NULL; }
    ctx->interp->cf.signal = 0;
    return make_ok(val);
}

XSResult xs_eval(XSContext *ctx, const char *code) {
    return run_source(ctx, code, "<embed>");
}

XSResult xs_eval_file(XSContext *ctx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[600];
        snprintf(buf, sizeof buf, "cannot open '%s'", path);
        return make_err(ctx, buf);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)(sz + 1));
    if (!src) { fclose(f); return make_err(ctx, "out of memory"); }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src);
        return make_err(ctx, "read error");
    }
    src[sz] = '\0';
    fclose(f);

    XSResult r = run_source(ctx, src, path);
    free(src);
    return r;
}

XSResult xs_call(XSContext *ctx, const char *fn_name, XSValue *args, int nargs) {
    Value *fn_val = env_get(ctx->interp->globals, fn_name);
    if (!fn_val) {
        char buf[256];
        snprintf(buf, sizeof buf, "function '%s' not found", fn_name);
        return make_err(ctx, buf);
    }
    if (VAL_TAG(fn_val) != XS_FUNC && VAL_TAG(fn_val) != XS_NATIVE) {
        char buf[256];
        snprintf(buf, sizeof buf, "'%s' is not callable", fn_name);
        return make_err(ctx, buf);
    }

    /* convert args */
    Value **c_args = NULL;
    if (nargs > 0) {
        c_args = (Value **)malloc(sizeof(Value *) * nargs);
        for (int i = 0; i < nargs; i++)
            c_args[i] = unwrap_value(&args[i]);
    }

    ctx->interp->cf.signal = 0;
    Value *result = call_value(ctx->interp, fn_val, c_args, nargs, fn_name);

    /* free synthesized args */
    for (int i = 0; i < nargs; i++) {
        if (!args[i]._internal) value_decref(c_args[i]);
    }
    free(c_args);

    if (ctx->interp->cf.signal == CF_ERROR || ctx->interp->cf.signal == CF_PANIC) {
        Value *err = ctx->interp->cf.value;
        const char *msg = "runtime error";
        if (err && VAL_TAG(err) == XS_STR) msg = err->s;
        else if (err) {
            char *s = value_repr(err);
            if (s) { snprintf(ctx->error_buf, sizeof ctx->error_buf, "%s", s); free(s); msg = ctx->error_buf; }
        }
        XSResult r = make_err(ctx, msg);
        if (ctx->interp->cf.value) { value_decref(ctx->interp->cf.value); ctx->interp->cf.value = NULL; }
        ctx->interp->cf.signal = 0;
        return r;
    }

    XSValue val;
    memset(&val, 0, sizeof val);
    val.tag = XS_VAL_NULL;
    if (result) val = wrap_value(result);

    if (ctx->interp->cf.value) { value_decref(ctx->interp->cf.value); ctx->interp->cf.value = NULL; }
    ctx->interp->cf.signal = 0;
    return make_ok(val);
}

XSValue xs_get(XSContext *ctx, const char *name) {
    Value *v = env_get(ctx->interp->globals, name);
    if (!v) { XSValue nul; memset(&nul, 0, sizeof nul); nul.tag = XS_VAL_NULL; return nul; }
    return wrap_value(v);
}

int xs_set(XSContext *ctx, const char *name, XSValue val) {
    Value *v = unwrap_value(&val);
    int is_synth = (val._internal == NULL);
    /* try set first (existing binding), then define */
    if (!env_set(ctx->interp->globals, name, v)) {
        env_define(ctx->interp->globals, name, v, 1);
    }
    if (is_synth) value_decref(v);
    return 0;
}

/* native function trampolines for new API */
#define XS_MAX_NATIVES 64

typedef struct {
    XSNativeFunc fn;
    XSContext   *ctx;
    int          used;
} NativeSlot;

static NativeSlot native_slots[XS_MAX_NATIVES];
static int        native_slot_count = 0;

static Value *native_dispatch(int slot, Interp *interp, Value **args, int argc) {
    (void)interp;
    NativeSlot *s = &native_slots[slot];

    XSValue *xargs = NULL;
    if (argc > 0) {
        xargs = (XSValue *)calloc(argc, sizeof(XSValue));
        for (int i = 0; i < argc; i++)
            xargs[i] = wrap_value(args[i]);
    }

    XSValue result = s->fn(s->ctx, xargs, argc);

    /* release wrapped args */
    for (int i = 0; i < argc; i++)
        xs_value_release(&xargs[i]);
    free(xargs);

    /* convert result back */
    Value *rv = unwrap_value(&result);
    if (!result._internal) {
        /* synthesized, already has refcount 1 from unwrap */
        return rv;
    }
    value_incref(rv);
    xs_value_release(&result);
    return rv;
}

#define NATIVE_TRAMP(N) \
    static Value *native_tramp_##N(Interp *interp, Value **args, int argc) { \
        return native_dispatch(N, interp, args, argc); \
    }

NATIVE_TRAMP(0)  NATIVE_TRAMP(1)  NATIVE_TRAMP(2)  NATIVE_TRAMP(3)
NATIVE_TRAMP(4)  NATIVE_TRAMP(5)  NATIVE_TRAMP(6)  NATIVE_TRAMP(7)
NATIVE_TRAMP(8)  NATIVE_TRAMP(9)  NATIVE_TRAMP(10) NATIVE_TRAMP(11)
NATIVE_TRAMP(12) NATIVE_TRAMP(13) NATIVE_TRAMP(14) NATIVE_TRAMP(15)
NATIVE_TRAMP(16) NATIVE_TRAMP(17) NATIVE_TRAMP(18) NATIVE_TRAMP(19)
NATIVE_TRAMP(20) NATIVE_TRAMP(21) NATIVE_TRAMP(22) NATIVE_TRAMP(23)
NATIVE_TRAMP(24) NATIVE_TRAMP(25) NATIVE_TRAMP(26) NATIVE_TRAMP(27)
NATIVE_TRAMP(28) NATIVE_TRAMP(29) NATIVE_TRAMP(30) NATIVE_TRAMP(31)
NATIVE_TRAMP(32) NATIVE_TRAMP(33) NATIVE_TRAMP(34) NATIVE_TRAMP(35)
NATIVE_TRAMP(36) NATIVE_TRAMP(37) NATIVE_TRAMP(38) NATIVE_TRAMP(39)
NATIVE_TRAMP(40) NATIVE_TRAMP(41) NATIVE_TRAMP(42) NATIVE_TRAMP(43)
NATIVE_TRAMP(44) NATIVE_TRAMP(45) NATIVE_TRAMP(46) NATIVE_TRAMP(47)
NATIVE_TRAMP(48) NATIVE_TRAMP(49) NATIVE_TRAMP(50) NATIVE_TRAMP(51)
NATIVE_TRAMP(52) NATIVE_TRAMP(53) NATIVE_TRAMP(54) NATIVE_TRAMP(55)
NATIVE_TRAMP(56) NATIVE_TRAMP(57) NATIVE_TRAMP(58) NATIVE_TRAMP(59)
NATIVE_TRAMP(60) NATIVE_TRAMP(61) NATIVE_TRAMP(62) NATIVE_TRAMP(63)

static NativeFn native_tramp_table[XS_MAX_NATIVES] = {
    native_tramp_0,  native_tramp_1,  native_tramp_2,  native_tramp_3,
    native_tramp_4,  native_tramp_5,  native_tramp_6,  native_tramp_7,
    native_tramp_8,  native_tramp_9,  native_tramp_10, native_tramp_11,
    native_tramp_12, native_tramp_13, native_tramp_14, native_tramp_15,
    native_tramp_16, native_tramp_17, native_tramp_18, native_tramp_19,
    native_tramp_20, native_tramp_21, native_tramp_22, native_tramp_23,
    native_tramp_24, native_tramp_25, native_tramp_26, native_tramp_27,
    native_tramp_28, native_tramp_29, native_tramp_30, native_tramp_31,
    native_tramp_32, native_tramp_33, native_tramp_34, native_tramp_35,
    native_tramp_36, native_tramp_37, native_tramp_38, native_tramp_39,
    native_tramp_40, native_tramp_41, native_tramp_42, native_tramp_43,
    native_tramp_44, native_tramp_45, native_tramp_46, native_tramp_47,
    native_tramp_48, native_tramp_49, native_tramp_50, native_tramp_51,
    native_tramp_52, native_tramp_53, native_tramp_54, native_tramp_55,
    native_tramp_56, native_tramp_57, native_tramp_58, native_tramp_59,
    native_tramp_60, native_tramp_61, native_tramp_62, native_tramp_63,
};

int xs_define_native(XSContext *ctx, const char *name, XSNativeFunc fn) {
    if (native_slot_count >= XS_MAX_NATIVES) return -1;
    int slot = native_slot_count++;
    native_slots[slot].fn   = fn;
    native_slots[slot].ctx  = ctx;
    native_slots[slot].used = 1;

    Value *nval = xs_native(native_tramp_table[slot]);
    env_define(ctx->interp->globals, name, nval, 0);
    value_decref(nval);
    return 0;
}

/* value constructors */
XSValue xs_make_int(int64_t v) {
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_INT; r.i = v; r._internal = NULL;
    return r;
}

XSValue xs_make_float(double v) {
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_FLOAT; r.f = v; r._internal = NULL;
    return r;
}

XSValue xs_make_string(const char *s) {
    /* create an actual Value so the string is owned */
    Value *v = xs_str(s ? s : "");
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_STRING;
    r.s = v->s;
    r._internal = v; /* owns the ref */
    return r;
}

XSValue xs_make_bool(int v) {
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_BOOL; r.b = v ? 1 : 0; r._internal = NULL;
    return r;
}

XSValue xs_make_null(void) {
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_NULL; r._internal = NULL;
    return r;
}

XSValue xs_make_array(void) {
    Value *v = xs_array_new();
    XSValue r; memset(&r, 0, sizeof r);
    r.tag = XS_VAL_ARRAY;
    r._internal = v;
    return r;
}

/* value inspection */
int xs_is_null(XSValue v)   { return v.tag == XS_VAL_NULL; }
int xs_is_bool(XSValue v)   { return v.tag == XS_VAL_BOOL; }
int xs_is_int(XSValue v)    { return v.tag == XS_VAL_INT; }
int xs_is_float(XSValue v)  { return v.tag == XS_VAL_FLOAT; }
int xs_is_string(XSValue v) { return v.tag == XS_VAL_STRING; }
int xs_is_array(XSValue v)  { return v.tag == XS_VAL_ARRAY; }

int64_t xs_to_int(XSValue v) {
    if (v.tag == XS_VAL_INT) return v.i;
    if (v.tag == XS_VAL_FLOAT) return (int64_t)v.f;
    if (v.tag == XS_VAL_BOOL) return v.b;
    return 0;
}

double xs_to_float(XSValue v) {
    if (v.tag == XS_VAL_FLOAT) return v.f;
    if (v.tag == XS_VAL_INT) return (double)v.i;
    return 0.0;
}

const char *xs_to_string(XSValue v) {
    if (v.tag == XS_VAL_STRING) return v.s ? v.s : "";
    if (v._internal) {
        Value *iv = (Value *)v._internal;
        if (VAL_TAG(iv) == XS_STR) return iv->s;
    }
    return "";
}

int xs_to_bool(XSValue v) {
    if (v.tag == XS_VAL_BOOL) return v.b;
    if (v.tag == XS_VAL_INT) return v.i != 0;
    if (v.tag == XS_VAL_NULL) return 0;
    return 1;
}

/* result handling */
int         xs_result_ok(XSResult r)    { return r.ok; }
XSValue     xs_result_value(XSResult r) { return r.value; }
const char *xs_result_error(XSResult r) { return r.error; }

/* release internal ref if any */
void xs_value_release(XSValue *v) {
    if (v && v->_internal) {
        value_decref((Value *)v->_internal);
        v->_internal = NULL;
    }
}


/* ================================================================
 *  Legacy API (XSState + stack-based)
 * ================================================================ */

#define XS_EMBED_STACK_SIZE 256

struct XSState {
    Interp  *interp;
    Value   *stack[XS_EMBED_STACK_SIZE];
    int      sp;
    int      has_error;
    char     error_msg[512];
};

XSState *xs_new(void) {
    XSState *xs = (XSState *)calloc(1, sizeof(XSState));
    if (!xs) return NULL;
    xs->interp = interp_new("<embed>");
    xs->sp = 0;
    xs->has_error = 0;
    xs->error_msg[0] = '\0';
    return xs;
}

void xs_free(XSState *xs) {
    if (!xs) return;
    for (int i = 0; i < xs->sp; i++) {
        if (xs->stack[i]) value_decref(xs->stack[i]);
    }
    interp_free(xs->interp);
    free(xs);
}

static void xs_set_error(XSState *xs, const char *msg) {
    xs->has_error = 1;
    snprintf(xs->error_msg, sizeof(xs->error_msg), "%.*s", (int)(sizeof(xs->error_msg)-1), msg);
}

static void xs_clear_error(XSState *xs) {
    xs->has_error = 0;
    xs->error_msg[0] = '\0';
}

static void xs_push(XSState *xs, Value *v) {
    if (xs->sp >= XS_EMBED_STACK_SIZE) {
        xs_set_error(xs, "embed stack overflow");
        return;
    }
    xs->stack[xs->sp++] = value_incref(v);
}

static Value *xs_pop(XSState *xs) {
    if (xs->sp <= 0) {
        xs_set_error(xs, "embed stack underflow");
        return NULL;
    }
    return xs->stack[--xs->sp];
}

int xs_eval_legacy(XSState *xs, const char *src) {
    xs_clear_error(xs);

    Lexer lex;
    lexer_init(&lex, src, "<embed>");
    TokenArray ta = lexer_tokenize(&lex);

    Parser parser;
    parser_init(&parser, &ta, "<embed>");
    Node *program = parser_parse(&parser);
    token_array_free(&ta);

    if (!program || parser.had_error) {
        xs_set_error(xs, parser.had_error ? parser.error.msg : "parse error");
        if (program) node_free(program);
        return 1;
    }

    interp_run(xs->interp, program);
    node_free(program);

    if (xs->interp->cf.signal == CF_ERROR || xs->interp->cf.signal == CF_PANIC) {
        Value *err = xs->interp->cf.value;
        if (err && VAL_TAG(err) == XS_STR) {
            xs_set_error(xs, err->s);
        } else if (err) {
            char *s = value_repr(err);
            if (s) { xs_set_error(xs, s); free(s); }
            else   { xs_set_error(xs, "runtime error"); }
        } else {
            xs_set_error(xs, "runtime error");
        }
        if (xs->interp->cf.value) {
            value_decref(xs->interp->cf.value);
            xs->interp->cf.value = NULL;
        }
        xs->interp->cf.signal = 0;
        return 1;
    }

    if (xs->interp->cf.value && VAL_TAG(xs->interp->cf.value) != XS_NULL) {
        xs_push(xs, xs->interp->cf.value);
    }
    if (xs->interp->cf.value) {
        value_decref(xs->interp->cf.value);
        xs->interp->cf.value = NULL;
    }
    xs->interp->cf.signal = 0;
    return 0;
}

int xs_eval_file_legacy(XSState *xs, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[600];
        snprintf(buf, sizeof(buf), "cannot open '%s'", path);
        xs_set_error(xs, buf);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)(sz + 1));
    if (!src) { fclose(f); xs_set_error(xs, "out of memory"); return 1; }
    if (fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(src);
        xs_set_error(xs, "read error");
        return 1;
    }
    src[sz] = '\0';
    fclose(f);

    int rc = xs_eval_legacy(xs, src);
    free(src);
    return rc;
}

int xs_call_legacy(XSState *xs, const char *fn_name, int argc) {
    if (argc > xs->sp) {
        xs_set_error(xs, "xs_call: not enough values on stack");
        return 1;
    }

    if (argc == 0) {
        size_t len = strlen(fn_name) + 4;
        char *call_str = (char *)malloc(len);
        snprintf(call_str, len, "%s()", fn_name);
        int rc = xs_eval_legacy(xs, call_str);
        free(call_str);
        return rc;
    }

    int base = xs->sp - argc;
    char *arg_strs[256];
    size_t total_len = strlen(fn_name) + 3;
    for (int i = 0; i < argc; i++) {
        arg_strs[i] = value_repr(xs->stack[base + i]);
        if (!arg_strs[i]) arg_strs[i] = xs_strdup("null");
        total_len += strlen(arg_strs[i]);
        if (i > 0) total_len += 2;
    }

    char *call_str = (char *)malloc(total_len);
    char *p = call_str;
    p += sprintf(p, "%s(", fn_name);
    for (int i = 0; i < argc; i++) {
        if (i > 0) { *p++ = ','; *p++ = ' '; }
        size_t slen = strlen(arg_strs[i]);
        memcpy(p, arg_strs[i], slen);
        p += slen;
        free(arg_strs[i]);
    }
    *p++ = ')';
    *p = '\0';

    for (int i = 0; i < argc; i++) {
        Value *v = xs_pop(xs);
        if (v) value_decref(v);
    }

    int rc = xs_eval_legacy(xs, call_str);
    free(call_str);
    return rc;
}

void xs_push_int(XSState *xs, int64_t v) {
    Value *val = xs_int(v);
    xs_push(xs, val);
    value_decref(val);
}

void xs_push_float(XSState *xs, double v) {
    Value *val = xs_float(v);
    xs_push(xs, val);
    value_decref(val);
}

void xs_push_str(XSState *xs, const char *v) {
    Value *val = xs_str(v);
    xs_push(xs, val);
    value_decref(val);
}

void xs_push_bool(XSState *xs, int v) {
    Value *val = xs_bool(v);
    xs_push(xs, val);
}

void xs_push_null(XSState *xs) {
    Value *val = xs_null();
    xs_push(xs, val);
}

int64_t xs_pop_int(XSState *xs) {
    Value *v = xs_pop(xs);
    if (!v) return 0;
    int64_t result = 0;
    if (VAL_TAG(v) == XS_INT) result = VAL_INT(v);
    else xs_set_error(xs, "pop_int: value is not an int");
    value_decref(v);
    return result;
}

double xs_pop_float(XSState *xs) {
    Value *v = xs_pop(xs);
    if (!v) return 0.0;
    double result = 0.0;
    if (VAL_TAG(v) == XS_FLOAT) result = v->f;
    else if (VAL_TAG(v) == XS_INT) result = (double)VAL_INT(v);
    else xs_set_error(xs, "pop_float: value is not a float");
    value_decref(v);
    return result;
}

char *xs_pop_str(XSState *xs) {
    Value *v = xs_pop(xs);
    if (!v) return NULL;
    char *result = NULL;
    if (VAL_TAG(v) == XS_STR) result = xs_strdup(v->s);
    else result = value_repr(v);
    value_decref(v);
    return result;
}

int xs_pop_bool(XSState *xs) {
    Value *v = xs_pop(xs);
    if (!v) return 0;
    int result = value_truthy(v);
    value_decref(v);
    return result;
}

XSRef xs_pin(XSState *xs, int stack_index) {
    XSRef ref = { NULL };
    if (stack_index < 0 || stack_index >= xs->sp) {
        xs_set_error(xs, "pin: invalid stack index");
        return ref;
    }
    Value *v = xs->stack[stack_index];
    value_incref(v);
    ref.opaque = v;
    return ref;
}

void xs_unpin(XSState *xs, XSRef ref) {
    (void)xs;
    Value *v = (Value *)ref.opaque;
    if (v) value_decref(v);
}

const char *xs_ref_str(XSRef ref) {
    Value *v = (Value *)ref.opaque;
    if (!v || VAL_TAG(v) != XS_STR) return NULL;
    return v->s;
}

/* C function trampolines for legacy API */
#define XS_MAX_REGISTERED 64

typedef struct {
    xs_cfunc  fn;
    XSState  *xs;
    int       used;
} CFuncSlot;

static CFuncSlot cfunc_slots[XS_MAX_REGISTERED];
static int       cfunc_slot_count = 0;

static Value *cfunc_dispatch(int slot, Interp *interp, Value **args, int argc) {
    (void)interp;
    CFuncSlot *s = &cfunc_slots[slot];
    XSVal *result = s->fn(s->xs, (XSVal **)args, argc);
    return (Value *)result;
}

#define TRAMPOLINE(N) \
    static Value *cfunc_trampoline_##N(Interp *interp, Value **args, int argc) { \
        return cfunc_dispatch(N, interp, args, argc); \
    }

TRAMPOLINE(0)  TRAMPOLINE(1)  TRAMPOLINE(2)  TRAMPOLINE(3)
TRAMPOLINE(4)  TRAMPOLINE(5)  TRAMPOLINE(6)  TRAMPOLINE(7)
TRAMPOLINE(8)  TRAMPOLINE(9)  TRAMPOLINE(10) TRAMPOLINE(11)
TRAMPOLINE(12) TRAMPOLINE(13) TRAMPOLINE(14) TRAMPOLINE(15)
TRAMPOLINE(16) TRAMPOLINE(17) TRAMPOLINE(18) TRAMPOLINE(19)
TRAMPOLINE(20) TRAMPOLINE(21) TRAMPOLINE(22) TRAMPOLINE(23)
TRAMPOLINE(24) TRAMPOLINE(25) TRAMPOLINE(26) TRAMPOLINE(27)
TRAMPOLINE(28) TRAMPOLINE(29) TRAMPOLINE(30) TRAMPOLINE(31)
TRAMPOLINE(32) TRAMPOLINE(33) TRAMPOLINE(34) TRAMPOLINE(35)
TRAMPOLINE(36) TRAMPOLINE(37) TRAMPOLINE(38) TRAMPOLINE(39)
TRAMPOLINE(40) TRAMPOLINE(41) TRAMPOLINE(42) TRAMPOLINE(43)
TRAMPOLINE(44) TRAMPOLINE(45) TRAMPOLINE(46) TRAMPOLINE(47)
TRAMPOLINE(48) TRAMPOLINE(49) TRAMPOLINE(50) TRAMPOLINE(51)
TRAMPOLINE(52) TRAMPOLINE(53) TRAMPOLINE(54) TRAMPOLINE(55)
TRAMPOLINE(56) TRAMPOLINE(57) TRAMPOLINE(58) TRAMPOLINE(59)
TRAMPOLINE(60) TRAMPOLINE(61) TRAMPOLINE(62) TRAMPOLINE(63)

static NativeFn trampoline_table[XS_MAX_REGISTERED] = {
    cfunc_trampoline_0,  cfunc_trampoline_1,  cfunc_trampoline_2,  cfunc_trampoline_3,
    cfunc_trampoline_4,  cfunc_trampoline_5,  cfunc_trampoline_6,  cfunc_trampoline_7,
    cfunc_trampoline_8,  cfunc_trampoline_9,  cfunc_trampoline_10, cfunc_trampoline_11,
    cfunc_trampoline_12, cfunc_trampoline_13, cfunc_trampoline_14, cfunc_trampoline_15,
    cfunc_trampoline_16, cfunc_trampoline_17, cfunc_trampoline_18, cfunc_trampoline_19,
    cfunc_trampoline_20, cfunc_trampoline_21, cfunc_trampoline_22, cfunc_trampoline_23,
    cfunc_trampoline_24, cfunc_trampoline_25, cfunc_trampoline_26, cfunc_trampoline_27,
    cfunc_trampoline_28, cfunc_trampoline_29, cfunc_trampoline_30, cfunc_trampoline_31,
    cfunc_trampoline_32, cfunc_trampoline_33, cfunc_trampoline_34, cfunc_trampoline_35,
    cfunc_trampoline_36, cfunc_trampoline_37, cfunc_trampoline_38, cfunc_trampoline_39,
    cfunc_trampoline_40, cfunc_trampoline_41, cfunc_trampoline_42, cfunc_trampoline_43,
    cfunc_trampoline_44, cfunc_trampoline_45, cfunc_trampoline_46, cfunc_trampoline_47,
    cfunc_trampoline_48, cfunc_trampoline_49, cfunc_trampoline_50, cfunc_trampoline_51,
    cfunc_trampoline_52, cfunc_trampoline_53, cfunc_trampoline_54, cfunc_trampoline_55,
    cfunc_trampoline_56, cfunc_trampoline_57, cfunc_trampoline_58, cfunc_trampoline_59,
    cfunc_trampoline_60, cfunc_trampoline_61, cfunc_trampoline_62, cfunc_trampoline_63,
};

void xs_register(XSState *xs, const char *name, xs_cfunc fn) {
    if (cfunc_slot_count >= XS_MAX_REGISTERED) {
        xs_set_error(xs, "xs_register: too many registered C functions (max 64)");
        return;
    }
    int slot = cfunc_slot_count++;
    cfunc_slots[slot].fn   = fn;
    cfunc_slots[slot].xs   = xs;
    cfunc_slots[slot].used = 1;

    Value *nval = xs_native(trampoline_table[slot]);
    env_define(xs->interp->globals, name, nval, 0);
    value_decref(nval);
}

int xs_error(XSState *xs) {
    return xs->has_error;
}

const char *xs_error_msg(XSState *xs) {
    return xs->error_msg;
}

XSVal *xs_make_int_legacy(XSState *xs, int64_t v) {
    (void)xs;
    return (XSVal *)xs_int(v);
}

XSVal *xs_make_float_legacy(XSState *xs, double v) {
    (void)xs;
    return (XSVal *)xs_float(v);
}

XSVal *xs_make_str_legacy(XSState *xs, const char *s) {
    (void)xs;
    return (XSVal *)xs_str(s);
}

XSVal *xs_make_bool_legacy(XSState *xs, int v) {
    (void)xs;
    return (XSVal *)xs_bool(v);
}

XSVal *xs_make_null_legacy(XSState *xs) {
    (void)xs;
    return (XSVal *)xs_null();
}

XSVal *xs_make_array_legacy(XSState *xs, XSVal **elems, int n) {
    (void)xs;
    Value *arr = xs_array_new();
    for (int i = 0; i < n; i++) {
        array_push(arr->arr, (Value *)elems[i]);
    }
    return (XSVal *)arr;
}

XSVal *xs_make_map_legacy(XSState *xs) {
    (void)xs;
    return (XSVal *)xs_map_new();
}

XSVal *xs_map_set_legacy(XSState *xs, XSVal *map_val, const char *key, XSVal *val) {
    (void)xs;
    Value *m = (Value *)map_val;
    if (!m || VAL_TAG(m) != XS_MAP) return map_val;
    map_set(m->map, key, (Value *)val);
    return map_val;
}

int xs_is_int_legacy(XSVal *v) {
    Value *val = (Value *)v;
    return val && VAL_TAG(val) == XS_INT;
}

int64_t xs_get_int_legacy(XSVal *v) {
    Value *val = (Value *)v;
    if (!val || VAL_TAG(val) != XS_INT) return 0;
    return VAL_INT(val);
}

const char *xs_get_str_legacy(XSVal *v) {
    Value *val = (Value *)v;
    if (!val || VAL_TAG(val) != XS_STR) return NULL;
    return val->s;
}
