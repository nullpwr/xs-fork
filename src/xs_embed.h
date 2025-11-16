/*
 * xs_embed.h -- embed the XS interpreter in a C/C++ application.
 * Link with: libxs (or the xs object files) + -lm
 *
 * This is the only header you need. All types are opaque.
 */
#ifndef XS_EMBED_H
#define XS_EMBED_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* opaque handles */
typedef struct XSContext XSContext;

/* tagged value (stack-allocated, cheap to copy) */
typedef enum {
    XS_VAL_NULL = 0,
    XS_VAL_BOOL,
    XS_VAL_INT,
    XS_VAL_FLOAT,
    XS_VAL_STRING,
    XS_VAL_ARRAY,
    XS_VAL_MAP,
    XS_VAL_FUNC,
    XS_VAL_OTHER
} XSValueTag;

typedef struct {
    XSValueTag tag;
    union {
        int64_t     i;
        double      f;
        const char *s;    /* borrowed, valid until next xs_ call */
        int         b;
    };
    void *_internal;      /* opaque ref-counted pointer */
} XSValue;

/* result of eval/call -- either a value or an error string */
typedef struct {
    int         ok;       /* 1 = success, 0 = error */
    XSValue     value;
    const char *error;    /* NULL when ok, static lifetime otherwise */
} XSResult;

/* native function callback */
typedef XSValue (*XSNativeFunc)(XSContext *ctx, XSValue *args, int nargs);

/* ---- context management ---- */

XSContext *xs_context_new(void);
void       xs_context_free(XSContext *ctx);

/* ---- evaluate code ---- */

XSResult xs_eval(XSContext *ctx, const char *code);
XSResult xs_eval_file(XSContext *ctx, const char *path);

/* ---- call a named function ---- */

XSResult xs_call(XSContext *ctx, const char *fn_name, XSValue *args, int nargs);

/* ---- variable access ---- */

XSValue xs_get(XSContext *ctx, const char *name);
int     xs_set(XSContext *ctx, const char *name, XSValue val);

/* ---- register native C functions ---- */

int xs_define_native(XSContext *ctx, const char *name, XSNativeFunc fn);

/* ---- value constructors ---- */

XSValue xs_make_int(int64_t v);
XSValue xs_make_float(double v);
XSValue xs_make_string(const char *s);
XSValue xs_make_bool(int v);
XSValue xs_make_null(void);
XSValue xs_make_array(void);

/* ---- value inspection ---- */

int         xs_is_null(XSValue v);
int         xs_is_bool(XSValue v);
int         xs_is_int(XSValue v);
int         xs_is_float(XSValue v);
int         xs_is_string(XSValue v);
int         xs_is_array(XSValue v);

int64_t     xs_to_int(XSValue v);
double      xs_to_float(XSValue v);
const char *xs_to_string(XSValue v);
int         xs_to_bool(XSValue v);

/* ---- result handling ---- */

int         xs_result_ok(XSResult r);
XSValue     xs_result_value(XSResult r);
const char *xs_result_error(XSResult r);

/* ---- cleanup ---- */

void xs_value_release(XSValue *v);

/* ---- legacy API (kept for compat) ---- */

typedef struct XSState   XSState;
typedef struct { void *opaque; } XSRef;

/* old opaque value pointer, used by legacy callbacks */
typedef void XSVal;
typedef XSVal* (*xs_cfunc)(XSState *xs, XSVal **args, int argc);

XSState* xs_new(void);
void     xs_free(XSState *xs);

int xs_eval_legacy(XSState *xs, const char *src);
int xs_eval_file_legacy(XSState *xs, const char *path);
int xs_call_legacy(XSState *xs, const char *fn, int argc);

void xs_push_int(XSState *xs, int64_t v);
void xs_push_float(XSState *xs, double v);
void xs_push_str(XSState *xs, const char *v);
void xs_push_bool(XSState *xs, int v);
void xs_push_null(XSState *xs);

int64_t     xs_pop_int(XSState *xs);
double      xs_pop_float(XSState *xs);
char*       xs_pop_str(XSState *xs);    /* caller frees */
int         xs_pop_bool(XSState *xs);

XSRef       xs_pin(XSState *xs, int stack_index);
void        xs_unpin(XSState *xs, XSRef ref);
const char* xs_ref_str(XSRef ref);

void xs_register(XSState *xs, const char *name, xs_cfunc fn);

int         xs_error(XSState *xs);
const char* xs_error_msg(XSState *xs);

XSVal* xs_make_int_legacy(XSState *xs, int64_t v);
XSVal* xs_make_float_legacy(XSState *xs, double v);
XSVal* xs_make_str_legacy(XSState *xs, const char *s);
XSVal* xs_make_bool_legacy(XSState *xs, int v);
XSVal* xs_make_null_legacy(XSState *xs);
XSVal* xs_make_array_legacy(XSState *xs, XSVal **elems, int n);
XSVal* xs_make_map_legacy(XSState *xs);
XSVal* xs_map_set_legacy(XSState *xs, XSVal *map, const char *key, XSVal *val);

int         xs_is_int_legacy(XSVal *v);
int64_t     xs_get_int_legacy(XSVal *v);
const char* xs_get_str_legacy(XSVal *v);

#ifdef __cplusplus
}
#endif

#endif
