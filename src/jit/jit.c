/* jit.c -- x86-64 copy-patch codegen for XS bytecode.
 *
 * Callee-saved register mapping:
 *   r12 = XS stack ptr, r13 = consts, r14 = locals, r15 = globals
 */

#ifdef XSC_ENABLE_JIT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#include "jit/jit.h"
#include "core/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__x86_64__) || defined(_M_X64)
#define JIT_ARCH_X86_64 1
#else
#define JIT_ARCH_X86_64 0
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/mman.h>
#include <unistd.h>
#define JIT_HAS_MMAP 1
#define JIT_HAS_WIN 0
#elif defined(_WIN32)
#include <windows.h>
#define JIT_HAS_MMAP 0
#define JIT_HAS_WIN 1
#else
#define JIT_HAS_MMAP 0
#define JIT_HAS_WIN 0
#endif

/* coerce both operands to double, returns 1 on success */
static int numcoerce(Value *a, Value *b, double *fa, double *fb) {
    if ((VAL_TAG(a) != XS_INT && VAL_TAG(a) != XS_FLOAT) ||
        (VAL_TAG(b) != XS_INT && VAL_TAG(b) != XS_FLOAT))
        return 0;
    *fa = VAL_TAG(a) == XS_INT ? (double)VAL_INT(a) : a->f;
    *fb = VAL_TAG(b) == XS_INT ? (double)VAL_INT(b) : b->f;
    return 1;
}

static Value *jit_rt_add(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT)
        return xs_int(VAL_INT(a) + VAL_INT(b));
    double fa, fb;
    if (numcoerce(a, b, &fa, &fb))
        return xs_float(fa + fb);
    if (VAL_TAG(a) == XS_STR && VAL_TAG(b) == XS_STR) {
        size_t la = strlen(a->s), lb = strlen(b->s);
        char *buf = xs_malloc(la + lb + 1);
        memcpy(buf, a->s, la);
        memcpy(buf + la, b->s, lb + 1);
        Value *r = xs_str(buf);
        free(buf);
        return r;
    }
    return xs_null();
}

static Value *jit_rt_sub(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT)
        return xs_int(VAL_INT(a) - VAL_INT(b));
    double fa, fb;
    if (numcoerce(a, b, &fa, &fb))
        return xs_float(fa - fb);
    return xs_null();
}

static Value *jit_rt_mul(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT)
        return xs_int(VAL_INT(a) * VAL_INT(b));
    double fa, fb;
    if (numcoerce(a, b, &fa, &fb))
        return xs_float(fa * fb);
    return xs_null();
}

static Value *jit_rt_div(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) {
        if (VAL_INT(b) == 0) return xs_null();
        return xs_int(VAL_INT(a) / VAL_INT(b));
    }
    double fa, fb;
    if (numcoerce(a, b, &fa, &fb)) {
        if (fb == 0.0) return xs_null();
        return xs_float(fa / fb);
    }
    return xs_null();
}

static Value *jit_rt_mod(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) {
        if (VAL_INT(b) == 0) return xs_null();
        return xs_int(VAL_INT(a) % VAL_INT(b));
    }
    double fa, fb;
    if (numcoerce(a, b, &fa, &fb)) {
        if (fb == 0.0) return xs_null();
        return xs_float(fmod(fa, fb));
    }
    return xs_null();
}

static Value *jit_rt_pow(Value *a, Value *b) {
    double fa = (VAL_TAG(a) == XS_INT) ? (double)VAL_INT(a) : (VAL_TAG(a) == XS_FLOAT ? a->f : 0.0);
    double fb = (VAL_TAG(b) == XS_INT) ? (double)VAL_INT(b) : (VAL_TAG(b) == XS_FLOAT ? b->f : 0.0);
    double r = pow(fa, fb);
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT && VAL_INT(b) >= 0 &&
        r == (double)(int64_t)r)
        return xs_int((int64_t)r);
    return xs_float(r);
}

static Value *jit_rt_neg(Value *a) {
    if (VAL_TAG(a) == XS_INT) return xs_int(-VAL_INT(a));
    if (VAL_TAG(a) == XS_FLOAT) return xs_float(-a->f);
    return xs_null();
}

static Value *jit_rt_not(Value *a) {
    return xs_bool(!value_truthy(a));
}

static Value *jit_rt_eq(Value *a, Value *b)  { return xs_bool(value_equal(a, b)); }
static Value *jit_rt_neq(Value *a, Value *b) { return xs_bool(!value_equal(a, b)); }
static Value *jit_rt_lt(Value *a, Value *b)  { return xs_bool(value_cmp(a, b) < 0); }
static Value *jit_rt_gt(Value *a, Value *b)  { return xs_bool(value_cmp(a, b) > 0); }
static Value *jit_rt_lte(Value *a, Value *b) { return xs_bool(value_cmp(a, b) <= 0); }
static Value *jit_rt_gte(Value *a, Value *b) { return xs_bool(value_cmp(a, b) >= 0); }

static Value *jit_rt_concat(Value *a, Value *b) {
    char *sa = value_str(a);
    char *sb = value_str(b);
    size_t la = strlen(sa), lb = strlen(sb);
    char *buf = xs_malloc(la + lb + 1);
    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb + 1);
    Value *r = xs_str(buf);
    free(sa); free(sb); free(buf);
    return r;
}

static Value *jit_rt_band(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) return xs_int(VAL_INT(a) & VAL_INT(b));
    return xs_null();
}
static Value *jit_rt_bor(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) return xs_int(VAL_INT(a) | VAL_INT(b));
    return xs_null();
}
static Value *jit_rt_bxor(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) return xs_int(VAL_INT(a) ^ VAL_INT(b));
    return xs_null();
}
static Value *jit_rt_bnot(Value *a) {
    if (VAL_TAG(a) == XS_INT) return xs_int(~VAL_INT(a));
    return xs_null();
}
static Value *jit_rt_shl(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) return xs_int(VAL_INT(a) << VAL_INT(b));
    return xs_null();
}
static Value *jit_rt_shr(Value *a, Value *b) {
    if (VAL_TAG(a) == XS_INT && VAL_TAG(b) == XS_INT) return xs_int(VAL_INT(a) >> VAL_INT(b));
    return xs_null();
}

static int jit_rt_truthy(Value *v) {
    return value_truthy(v);
}

static Value *jit_rt_floor_div(Value *a, Value *b) {
    double av = VAL_TAG(a) == XS_INT ? (double)VAL_INT(a) : a->f;
    double bv = VAL_TAG(b) == XS_INT ? (double)VAL_INT(b) : b->f;
    if (bv == 0.0) return xs_null();
    return xs_int((int64_t)floor(av / bv));
}

static Value *jit_rt_spaceship(Value *a, Value *b) {
    int cmp = value_cmp(a, b);
    return xs_int(cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
}

static Value *jit_rt_in(Value *a, Value *b) {
    int found = 0;
    if (VAL_TAG(b) == XS_ARRAY) {
        for (int j = 0; j < b->arr->len; j++)
            if (value_equal(a, b->arr->items[j])) { found = 1; break; }
    } else if (VAL_TAG(b) == XS_MAP || VAL_TAG(b) == XS_MODULE) {
        if (VAL_TAG(a) == XS_STR) found = map_has(b->map, a->s);
    } else if (VAL_TAG(b) == XS_STR && VAL_TAG(a) == XS_STR) {
        found = strstr(b->s, a->s) != NULL;
    } else if (VAL_TAG(b) == XS_RANGE) {
        if (VAL_TAG(a) == XS_INT) {
            int64_t v = VAL_INT(a);
            found = v >= b->range->start &&
                    (b->range->inclusive ? v <= b->range->end : v < b->range->end);
        }
    }
    return xs_bool(found);
}

static Value *jit_rt_is(Value *a, Value *b) {
    int match = 0;
    if (VAL_TAG(b) == XS_STR) {
        const char *t = b->s;
        if      (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0) match = (VAL_TAG(a) == XS_INT);
        else if (strcmp(t, "float") == 0 || strcmp(t, "f64") == 0) match = (VAL_TAG(a) == XS_FLOAT);
        else if (strcmp(t, "str") == 0 || strcmp(t, "string") == 0) match = (VAL_TAG(a) == XS_STR);
        else if (strcmp(t, "bool") == 0) match = (VAL_TAG(a) == XS_BOOL);
        else if (strcmp(t, "array") == 0) match = (VAL_TAG(a) == XS_ARRAY);
        else if (strcmp(t, "map") == 0) match = (VAL_TAG(a) == XS_MAP);
        else if (strcmp(t, "null") == 0) match = (VAL_TAG(a) == XS_NULL);
        else if (strcmp(t, "fn") == 0 || strcmp(t, "function") == 0) match = (VAL_TAG(a) == XS_FUNC || VAL_TAG(a) == XS_NATIVE || VAL_TAG(a) == XS_CLOSURE);
        else if (strcmp(t, "tuple") == 0) match = (VAL_TAG(a) == XS_TUPLE);
    }
    return xs_bool(match);
}

static Value *jit_rt_make_tuple(Value **sp_bottom, int n) {
    Value *tup = xs_tuple_new();
    for (int i = 0; i < n; i++) {
        array_push(tup->arr, sp_bottom[i]);
        value_decref(sp_bottom[i]);
    }
    return tup;
}

static Value *jit_rt_make_range(Value *start, Value *end, int inclusive) {
    int64_t s = VAL_TAG(start) == XS_INT ? VAL_INT(start) : 0;
    int64_t e = VAL_TAG(end) == XS_INT ? VAL_INT(end) : 0;
    value_decref(start);
    value_decref(end);
    return xs_range(s, e, inclusive);
}

static Value *jit_rt_iter_len(Value *iter) {
    int64_t len = 0;
    if (VAL_TAG(iter) == XS_ARRAY || VAL_TAG(iter) == XS_TUPLE) len = iter->arr->len;
    else if (VAL_TAG(iter) == XS_STR) len = (int64_t)strlen(iter->s);
    else if (VAL_TAG(iter) == XS_RANGE) {
        int64_t diff = iter->range->end - iter->range->start;
        if (!iter->range->inclusive) len = diff > 0 ? diff : 0;
        else len = diff >= 0 ? diff + 1 : 0;
    }
    value_decref(iter);
    return xs_int(len);
}

static Value *jit_rt_iter_get(Value *iter, Value *idx) {
    Value *r;
    int64_t i = VAL_TAG(idx) == XS_INT ? VAL_INT(idx) : 0;
    if (VAL_TAG(iter) == XS_ARRAY || VAL_TAG(iter) == XS_TUPLE) {
        r = (i >= 0 && i < iter->arr->len) ? value_incref(iter->arr->items[i]) : value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(iter) == XS_STR) {
        int64_t slen = (int64_t)strlen(iter->s);
        if (i >= 0 && i < slen) { char buf[2] = {iter->s[i], 0}; r = xs_str(buf); }
        else r = value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(iter) == XS_RANGE) {
        r = xs_int(iter->range->start + i);
    } else {
        r = value_incref(XS_NULL_VAL);
    }
    value_decref(iter);
    value_decref(idx);
    return r;
}

static Value *jit_rt_method_call(Value **sp_bottom, int argc, Value *name_val) {
    Value *obj = sp_bottom[0];
    Value **args = sp_bottom + 1;
    Value *result = NULL;

    /* Look up method on object */
    const char *mname = name_val->s;
    Value *method = NULL;

    if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
        method = map_get(obj->map, mname);
        if (!method) {
            Value *methods = map_get(obj->map, "__methods");
            if (methods && VAL_TAG(methods) == XS_MAP)
                method = map_get(methods->map, mname);
        }
    } else if (VAL_TAG(obj) == XS_INST && obj->inst) {
        method = map_get(obj->inst->fields, mname);
        if (!method && obj->inst->methods)
            method = map_get(obj->inst->methods, mname);
    }

    if (method && VAL_TAG(method) == XS_NATIVE) {
        /* For native methods, pass obj as first arg */
        Value *margs[17];
        margs[0] = obj;
        int total = 1 + argc;
        if (total > 17) total = 17;
        for (int i = 0; i < argc && i < 16; i++) margs[i + 1] = args[i];
        result = method->native(NULL, margs, total);
    }

    if (!result) result = value_incref(XS_NULL_VAL);
    for (int i = 0; i < argc; i++) value_decref(args[i]);
    value_decref(obj);
    return result;
}

/* Store-local helper: decrefs old value, stores new, returns new for caller convenience */
static Value *jit_rt_store_local(Value **locals, int slot, Value *new_val) {
    Value *old = locals[slot];
    if (old) value_decref(old);
    locals[slot] = new_val;
    return new_val;
}

/* Load global: look up name in globals map */
static Value *jit_rt_load_global(XSMap *globals, Value *name_val) {
    if (!globals || !name_val || VAL_TAG(name_val) != XS_STR) return xs_null();
    Value *v = map_get(globals, name_val->s);
    return v ? value_incref(v) : value_incref(XS_NULL_VAL);
}

/* Store global: set name in globals map */
static Value *jit_rt_store_global(XSMap *globals, Value *name_val, Value *val) {
    if (!globals || !name_val || VAL_TAG(name_val) != XS_STR) return val;
    map_set(globals, name_val->s, val);
    return val;
}

/* Make array: given stack pointer and count, pop n values and build array */
static Value *jit_rt_make_array(Value **sp_bottom, int n) {
    Value *arr = xs_array_new();
    /* sp_bottom points to first element (elements are sp_bottom[0..n-1]) */
    for (int i = 0; i < n; i++) {
        array_push(arr->arr, sp_bottom[i]);
        value_decref(sp_bottom[i]);
    }
    return arr;
}

/* Make map: given stack pointer and count of pairs */
static Value *jit_rt_make_map(Value **sp_bottom, int npairs) {
    Value *m = xs_map_new();
    for (int i = 0; i < npairs; i++) {
        Value *k = sp_bottom[i * 2];
        Value *v = sp_bottom[i * 2 + 1];
        if (VAL_TAG(k) == XS_STR) map_set(m->map, k->s, v);
        value_decref(k);
        value_decref(v);
    }
    return m;
}

/* Index get: col[idx] */
static Value *jit_rt_index_get(Value *col, Value *idx) {
    Value *r;
    if ((VAL_TAG(col) == XS_ARRAY || VAL_TAG(col) == XS_TUPLE) && VAL_TAG(idx) == XS_INT) {
        int64_t i = VAL_INT(idx);
        if (i < 0) i += col->arr->len;
        r = (i >= 0 && i < col->arr->len) ? value_incref(col->arr->items[i])
                                            : value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(col) == XS_MAP && VAL_TAG(idx) == XS_STR) {
        Value *v = map_get(col->map, idx->s);
        r = v ? value_incref(v) : value_incref(XS_NULL_VAL);
    } else if (VAL_TAG(col) == XS_STR && VAL_TAG(idx) == XS_INT) {
        const char *s = col->s;
        int64_t slen = (int64_t)strlen(s);
        int64_t i = VAL_INT(idx);
        if (i < 0) i += slen;
        if (i >= 0 && i < slen) {
            char buf[2] = {s[i], 0};
            r = xs_str(buf);
        } else {
            r = value_incref(XS_NULL_VAL);
        }
    } else {
        r = value_incref(XS_NULL_VAL);
    }
    value_decref(col);
    value_decref(idx);
    return r;
}

/* Index set: col[idx] = val */
static Value *jit_rt_index_set(Value *col, Value *idx, Value *val) {
    if ((VAL_TAG(col) == XS_ARRAY || VAL_TAG(col) == XS_TUPLE) && VAL_TAG(idx) == XS_INT) {
        int64_t i = VAL_INT(idx);
        if (i >= 0 && i < col->arr->len) {
            value_decref(col->arr->items[i]);
            col->arr->items[i] = value_incref(val);
        }
    } else if (VAL_TAG(col) == XS_MAP && VAL_TAG(idx) == XS_STR) {
        map_set(col->map, idx->s, val);
    }
    value_decref(val);
    value_decref(idx);
    value_decref(col);
    return xs_null();
}

/* Load field: obj.name */
static Value *jit_rt_load_field(Value *obj, Value *name_val) {
    Value *r = NULL;
    const char *name = name_val->s;
    if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
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
    } else if (VAL_TAG(obj) == XS_INST && obj->inst) {
        Value *v = map_get(obj->inst->fields, name);
        if (v) r = value_incref(v);
        if (!r && obj->inst->methods) {
            Value *mv = map_get(obj->inst->methods, name);
            if (mv) r = value_incref(mv);
        }
    }
    if (!r) r = value_incref(XS_NULL_VAL);
    value_decref(obj);
    return r;
}

/* Store field: obj.name = val */
static Value *jit_rt_store_field(Value *obj, Value *name_val, Value *val) {
    const char *name = name_val->s;
    if (VAL_TAG(obj) == XS_MAP || VAL_TAG(obj) == XS_MODULE) {
        map_set(obj->map, name, val);
    } else if (VAL_TAG(obj) == XS_INST && obj->inst) {
        map_set(obj->inst->fields, name, val);
    }
    value_decref(val);
    value_decref(obj);
    return xs_null();
}

/* Call dispatch: native function call from JIT.
 * sp_bottom points to [callee, arg0, arg1, ...] on the XS stack.
 * argc is the number of arguments (not counting callee).
 * Returns the result value. Callee and args are decref'd.
 */
static Value *jit_rt_call(Value **sp_bottom, int argc) {
    Value *callee = sp_bottom[0];
    Value **args  = sp_bottom + 1;
    Value *result = NULL;

    if (VAL_TAG(callee) == XS_NATIVE) {
        result = callee->native(NULL, args, argc);
        if (!result) result = value_incref(XS_NULL_VAL);
    } else {
        /* Non-native callables cannot be handled purely in JIT;
         * return null to signal bailout. In practice, closures
         * trigger the bail path in jit_compile so this is only
         * reached for XS_NATIVE. */
        fprintf(stderr, "jit: call on non-native (tag=%d), returning null\n", VAL_TAG(callee));
        result = value_incref(XS_NULL_VAL);
    }

    /* Decref args and callee */
    for (int i = 0; i < argc; i++) value_decref(args[i]);
    value_decref(callee);

    return result;
}

/* emitter */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    int      overflow;
} Emitter;

static void emit_init(Emitter *e, uint8_t *buf, size_t cap, size_t start) {
    e->buf = buf; e->cap = cap; e->pos = start; e->overflow = 0;
}

static inline void emit_byte(Emitter *e, uint8_t b) {
    if (e->pos < e->cap) e->buf[e->pos++] = b;
    else e->overflow = 1;
}

static inline void emit_u32(Emitter *e, uint32_t v) {
    emit_byte(e, (uint8_t)(v));
    emit_byte(e, (uint8_t)(v >> 8));
    emit_byte(e, (uint8_t)(v >> 16));
    emit_byte(e, (uint8_t)(v >> 24));
}

static inline void emit_u64(Emitter *e, uint64_t v) {
    emit_u32(e, (uint32_t)v);
    emit_u32(e, (uint32_t)(v >> 32));
}

static inline void emit_i32(Emitter *e, int32_t v) {
    emit_u32(e, (uint32_t)v);
}

static inline void emit_modrm(Emitter *e, uint8_t mod, uint8_t reg, uint8_t rm) {
    emit_byte(e, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* Register encoding: low 3 bits; high bit goes in REX.R/B */
#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
/* Extended registers use encoding 0-7 with REX.B or REX.R set */


/* REX byte: 0100WRXB */
static inline uint8_t rex(int w, int r_ext, int x_ext, int b_ext) {
    return (uint8_t)(0x40 | (w ? 8 : 0) | (r_ext ? 4 : 0) | (x_ext ? 2 : 0) | (b_ext ? 1 : 0));
}

/* mov reg64, imm64 */
static void emit_mov_reg_imm64(Emitter *e, int reg, uint64_t imm) {
    int ext = (reg >= 8);
    emit_byte(e, rex(1, 0, 0, ext));
    emit_byte(e, (uint8_t)(0xB8 + (reg & 7)));
    emit_u64(e, imm);
}

/* mov dst, src (64-bit reg-reg) */
static void emit_mov_reg_reg(Emitter *e, int dst, int src) {
    emit_byte(e, rex(1, src >= 8, 0, dst >= 8));
    emit_byte(e, 0x89);
    emit_modrm(e, 3, (uint8_t)(src & 7), (uint8_t)(dst & 7));
}

/* push reg64 */
static void emit_push_reg(Emitter *e, int reg) {
    if (reg >= 8) emit_byte(e, (uint8_t)(0x41));
    emit_byte(e, (uint8_t)(0x50 + (reg & 7)));
}

/* pop reg64 */
static void emit_pop_reg(Emitter *e, int reg) {
    if (reg >= 8) emit_byte(e, (uint8_t)(0x41));
    emit_byte(e, (uint8_t)(0x58 + (reg & 7)));
}

/* add reg64, imm8 */
static void emit_add_reg_imm8(Emitter *e, int reg, int8_t imm) {
    emit_byte(e, rex(1, 0, 0, reg >= 8));
    emit_byte(e, 0x83);
    emit_modrm(e, 3, 0, (uint8_t)(reg & 7));
    emit_byte(e, (uint8_t)imm);
}

/* sub reg64, imm8 */
static void emit_sub_reg_imm8(Emitter *e, int reg, int8_t imm) {
    emit_byte(e, rex(1, 0, 0, reg >= 8));
    emit_byte(e, 0x83);
    emit_modrm(e, 3, 5, (uint8_t)(reg & 7));
    emit_byte(e, (uint8_t)imm);
}

/* call rax  (FF /2) */
static void emit_call_rax(Emitter *e) {
    emit_byte(e, 0xFF);
    emit_modrm(e, 3, 2, RAX);
}

/* ret */
static void emit_ret(Emitter *e) { emit_byte(e, 0xC3); }

/* nop */
static void emit_nop(Emitter *e) { emit_byte(e, 0x90); }

/* Load addr into rax, call rax */
static void emit_call_abs(Emitter *e, void *fn) {
    emit_mov_reg_imm64(e, RAX, (uint64_t)(uintptr_t)fn);
    emit_call_rax(e);
}

/* test eax, eax */
static void emit_test_eax_eax(Emitter *e) {
    emit_byte(e, 0x85); emit_modrm(e, 3, RAX, RAX);
}

/* jmp rel32: returns offset of the rel32 to patch */
static size_t emit_jmp_rel32(Emitter *e) {
    emit_byte(e, 0xE9);
    size_t patch = e->pos;
    emit_i32(e, 0);
    return patch;
}

/* je rel32 */
static size_t emit_je_rel32(Emitter *e) {
    emit_byte(e, 0x0F); emit_byte(e, 0x84);
    size_t patch = e->pos;
    emit_i32(e, 0);
    return patch;
}

/* jne rel32 */
static size_t emit_jne_rel32(Emitter *e) {
    emit_byte(e, 0x0F); emit_byte(e, 0x85);
    size_t patch = e->pos;
    emit_i32(e, 0);
    return patch;
}

/* Patch rel32 at patch_offset to jump to target_pos */
static void patch_rel32(Emitter *e, size_t patch_offset, size_t target_pos) {
    int32_t rel = (int32_t)(target_pos - (patch_offset + 4));
    e->buf[patch_offset + 0] = (uint8_t)(rel);
    e->buf[patch_offset + 1] = (uint8_t)(rel >> 8);
    e->buf[patch_offset + 2] = (uint8_t)(rel >> 16);
    e->buf[patch_offset + 3] = (uint8_t)(rel >> 24);
}

// XS stack ops (r12 = top)

/*
 * Memory access to [r12 + disp]:  r12 is extended (reg 12), encoded as
 *   rm=100 (RSP encoding) which triggers SIB.  SIB = 0x24 means base=r12,
 *   index=none, scale=1.
 */

/* Push rax onto XS stack: mov [r12], rax; add r12, 8 */
static void emit_xs_push_rax(Emitter *e) {
    /* mov [r12], rax: REX.WB=0x49, 0x89, ModRM(00,RAX,100), SIB(0x24) */
    emit_byte(e, 0x49); emit_byte(e, 0x89);
    emit_modrm(e, 0, RAX, RSP);
    emit_byte(e, 0x24);
    emit_add_reg_imm8(e, 12, 8);
}

/* Pop XS stack into rax: sub r12,8; mov rax,[r12] */
static void emit_xs_pop_rax(Emitter *e) {
    emit_sub_reg_imm8(e, 12, 8);
    /* mov rax, [r12]: REX.WB=0x49, 0x8B, ModRM(00,RAX,100), SIB(0x24) */
    emit_byte(e, 0x49); emit_byte(e, 0x8B);
    emit_modrm(e, 0, RAX, RSP);
    emit_byte(e, 0x24);
}

/* Pop XS stack into a GPR (0-7 range only, for rdi/rsi/rcx etc.) */
static void emit_xs_pop_gpr(Emitter *e, int reg) {
    emit_sub_reg_imm8(e, 12, 8);
    /* mov reg, [r12] */
    emit_byte(e, rex(1, reg >= 8, 0, 1)); /* REX.W, R=reg>=8, B=1 for r12 */
    emit_byte(e, 0x8B);
    emit_modrm(e, 0, (uint8_t)(reg & 7), RSP);
    emit_byte(e, 0x24);
}

/* Peek top of stack into rax: mov rax, [r12 - 8] */
static void emit_xs_peek_rax(Emitter *e) {
    /* mov rax, [r12 - 8]: disp8=-8 */
    emit_byte(e, 0x49); emit_byte(e, 0x8B);
    emit_modrm(e, 1, RAX, RSP); /* mod=01 disp8 */
    emit_byte(e, 0x24);
    emit_byte(e, 0xF8); /* -8 */
}

/* Load constant[idx] into rax: mov rax, [r13 + idx*8] */
static void emit_load_const(Emitter *e, int idx) {
    int32_t disp = idx * 8;
    /* r13 encoded as rm=101 (RBP encoding). With mod=10 it's [r13+disp32]. */
    emit_byte(e, 0x49); emit_byte(e, 0x8B);
    emit_modrm(e, 2, RAX, RBP); /* rm=101=r13 with REX.B */
    emit_i32(e, disp);
}

/* Load local[slot] into rax: mov rax, [r14 + slot*8] */
static void emit_load_local(Emitter *e, int slot) {
    int32_t disp = slot * 8;
    /* r14 encoded as rm=110 */
    emit_byte(e, 0x49); emit_byte(e, 0x8B);
    emit_modrm(e, 2, RAX, RSI); /* rm=110=r14 with REX.B */
    emit_i32(e, disp);
}


/* Call value_incref(rax), result in rax */
static void emit_incref_rax(Emitter *e) {
    emit_mov_reg_reg(e, RDI, RAX);
    emit_call_abs(e, (void *)(uintptr_t)value_incref);
}

/* Call value_decref(rax) */
static void emit_decref_rax(Emitter *e) {
    emit_mov_reg_reg(e, RDI, RAX);
    emit_call_abs(e, (void *)(uintptr_t)value_decref);
}

/* Binary op: pop b, pop a, call fn(a,b), push result */
static void emit_binary_op(Emitter *e, void *fn) {
    emit_xs_pop_gpr(e, RSI);  /* b */
    emit_xs_pop_gpr(e, RDI);  /* a */
    emit_call_abs(e, fn);
    emit_xs_push_rax(e);
}

/* Unary op: pop a, call fn(a), push result */
static void emit_unary_op(Emitter *e, void *fn) {
    emit_xs_pop_gpr(e, RDI);
    emit_call_abs(e, fn);
    emit_xs_push_rax(e);
}

/* prologue/epilogue -- 6 pushes + 8 alignment = 64 bytes (16-aligned) */
static void emit_prologue(Emitter *e) {
    emit_push_reg(e, RBP);
    emit_mov_reg_reg(e, RBP, RSP);
    emit_push_reg(e, 12);
    emit_push_reg(e, 13);
    emit_push_reg(e, 14);
    emit_push_reg(e, 15);
    emit_push_reg(e, RBX);
    emit_sub_reg_imm8(e, RSP, 8); /* align */
    /* r12 = rdi (stack), r13 = rsi (consts), r14 = rdx (locals), r15 = rcx (globals) */
    emit_byte(e, rex(1, 0, 0, 1)); emit_byte(e, 0x89); emit_modrm(e, 3, RDI, (uint8_t)(12 & 7));
    emit_byte(e, rex(1, 0, 0, 1)); emit_byte(e, 0x89); emit_modrm(e, 3, RSI, (uint8_t)(13 & 7));
    emit_byte(e, rex(1, 0, 0, 1)); emit_byte(e, 0x89); emit_modrm(e, 3, RDX, (uint8_t)(14 & 7));
    emit_byte(e, rex(1, 0, 0, 1)); emit_byte(e, 0x89); emit_modrm(e, 3, RCX, (uint8_t)(15 & 7));
}

static void emit_epilogue(Emitter *e) {
    emit_add_reg_imm8(e, RSP, 8);
    emit_pop_reg(e, RBX);
    emit_pop_reg(e, 15);
    emit_pop_reg(e, 14);
    emit_pop_reg(e, 13);
    emit_pop_reg(e, 12);
    emit_pop_reg(e, RBP);
    emit_ret(e);
}

/* Push xs_null() */
static void emit_push_null(Emitter *e) {
    emit_call_abs(e, (void *)(uintptr_t)xs_null);
    emit_xs_push_rax(e);
}

/* Push xs_bool(1) */
static void emit_push_true(Emitter *e) {
    emit_mov_reg_imm64(e, RDI, 1);
    emit_call_abs(e, (void *)(uintptr_t)xs_bool);
    emit_xs_push_rax(e);
}

/* Push xs_bool(0) */
static void emit_push_false(Emitter *e) {
    emit_mov_reg_imm64(e, RDI, 0);
    emit_call_abs(e, (void *)(uintptr_t)xs_bool);
    emit_xs_push_rax(e);
}


XSJIT *jit_new(void) {
    XSJIT *j = xs_calloc(1, sizeof *j);
    j->available = 0;

#if JIT_ARCH_X86_64 && JIT_HAS_MMAP
    j->code_size = XS_JIT_CODE_SIZE;
    j->code = (uint8_t *)mmap(NULL, j->code_size,
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->code == MAP_FAILED) {
        j->code = NULL;
        j->code_size = 0;
        fprintf(stderr, "xs jit: mmap failed, JIT unavailable (interpreter fallback)\n");
    } else {
        j->code_used = 0;
        j->available = 1;
    }
#elif JIT_ARCH_X86_64 && JIT_HAS_WIN
    j->code_size = XS_JIT_CODE_SIZE;
    j->code = (uint8_t *)VirtualAlloc(NULL, j->code_size,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!j->code) {
        j->code_size = 0;
        fprintf(stderr, "xs jit: VirtualAlloc failed, JIT unavailable\n");
    } else {
        j->code_used = 0;
        j->available = 1;
    }
#else
    j->code = NULL;
    j->code_size = 0;
#endif

    j->n_protos    = XS_JIT_MAX_PROTOS;
    j->compiled    = xs_calloc((size_t)j->n_protos, sizeof(void *));
    j->call_counts = xs_calloc((size_t)j->n_protos, sizeof(int));

    return j;
}

void jit_free(XSJIT *j) {
    if (!j) return;
#if JIT_HAS_MMAP
    if (j->code && j->code_size)
        munmap(j->code, j->code_size);
#elif JIT_HAS_WIN
    if (j->code && j->code_size)
        VirtualFree(j->code, 0, MEM_RELEASE);
#endif
    free(j->compiled);
    free(j->call_counts);
    free(j);
}


#if JIT_ARCH_X86_64 && JIT_HAS_MMAP

#include "vm/vm.h"
#include "core/xs_bigint.h"

/* Real x86-64 JIT (tier 1: per-opcode inlining).
 *
 * Calling convention: int jit_entry(VM *vm)
 *
 * Register layout:
 *   r12 = VM*                       (callee-saved, set in prologue)
 *   r13 = current CallFrame*        (refreshed after every call)
 *
 * Per-instruction strategy:
 *   - For "hot" simple opcodes we inline the body in machine code and
 *     manually advance frame->ip. No vm_step round trip.
 *   - For everything else we set frame->ip back to point at the current
 *     instruction (so the dispatcher fetches it as if it had not been
 *     pre-decoded) and call vm_step. After the call we refresh r13 in
 *     case the opcode swapped frames (CALL / RETURN / THROW catch).
 *
 * Output equality with --vm is preserved by construction: inlined ops
 * call the same xs_int / value_incref / value_decref helpers as the VM,
 * and any opcode we don't recognize delegates to vm_step verbatim. */

/* VM struct field offsets (kept in sync via tests/regression bug025). */
#define VM_OFF_SP            16
#define VM_OFF_FRAMES        24
#define VM_OFF_FRAME_COUNT   36
#define FRAME_OFF_IP          8
#define FRAME_OFF_BASE       16
#define FRAME_SIZE         1608
#define VAL_OFF_REFCOUNT      4

/* mov rdi, r12   (4c 89 e7) -- vm pointer to first arg slot */
static void emit_mov_rdi_r12(Emitter *e) {
    emit_byte(e, 0x4c); emit_byte(e, 0x89); emit_byte(e, 0xe7);
}
/* mov r12, rdi   (49 89 fc) */
static void emit_mov_r12_rdi(Emitter *e) {
    emit_byte(e, 0x49); emit_byte(e, 0x89); emit_byte(e, 0xfc);
}

/* Refresh r13 := &vm->frames[vm->frame_count - 1].
   r12 = VM*. Uses rax, rcx as scratch. */
static void emit_refresh_frame(Emitter *e) {
    /* mov rcx, [r12 + VM_OFF_FRAME_COUNT]   ; read frame_count (32-bit) */
    /* movsxd rcx, dword [r12 + 36] */
    emit_byte(e, 0x4d); emit_byte(e, 0x63); emit_byte(e, 0x4c);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_FRAME_COUNT);
    /* dec rcx */
    emit_byte(e, 0x48); emit_byte(e, 0xff); emit_byte(e, 0xc9);
    /* imul rcx, rcx, FRAME_SIZE */
    emit_byte(e, 0x48); emit_byte(e, 0x69); emit_byte(e, 0xc9);
    emit_byte(e, (uint8_t)(FRAME_SIZE & 0xff));
    emit_byte(e, (uint8_t)((FRAME_SIZE >> 8) & 0xff));
    emit_byte(e, (uint8_t)((FRAME_SIZE >> 16) & 0xff));
    emit_byte(e, (uint8_t)((FRAME_SIZE >> 24) & 0xff));
    /* mov r13, [r12 + VM_OFF_FRAMES] */
    emit_byte(e, 0x4d); emit_byte(e, 0x8b); emit_byte(e, 0x6c);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_FRAMES);
    /* add r13, rcx */
    emit_byte(e, 0x4c); emit_byte(e, 0x01); emit_byte(e, 0xed);
}

/* PUSH rax onto vm->sp (no growth check; slow path falls back to vm_step
   on stack pressure via the standard PUSH macro). */
static void emit_push_rax_to_vm_sp(Emitter *e) {
    /* mov rcx, [r12 + 16]   ; rcx = vm->sp */
    emit_byte(e, 0x49); emit_byte(e, 0x8b); emit_byte(e, 0x4c);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_SP);
    /* mov [rcx], rax        ; *sp = rax */
    emit_byte(e, 0x48); emit_byte(e, 0x89); emit_byte(e, 0x01);
    /* add qword [r12 + 16], 8  ; vm->sp++ */
    emit_byte(e, 0x49); emit_byte(e, 0x83); emit_byte(e, 0x44);
    emit_byte(e, 0x24); emit_byte(e, (uint8_t)VM_OFF_SP);
    emit_byte(e, 0x08);
}

/* Inline value_incref(rax). Skips on null or SMI (low bit set, which
   means the value lives in the pointer, no refcount to bump). */
static void emit_inline_incref_rax(Emitter *e) {
    /* test al, 1  ; low bit set = SMI, skip everything (9 bytes) */
    emit_byte(e, 0xa8); emit_byte(e, 0x01);
    /* jnz +9  (skip test+jz+add if SMI) */
    emit_byte(e, 0x75); emit_byte(e, 0x09);
    /* test rax, rax */
    emit_byte(e, 0x48); emit_byte(e, 0x85); emit_byte(e, 0xc0);
    /* jz +4  (skip the inc on null) */
    emit_byte(e, 0x74); emit_byte(e, 0x04);
    /* add dword [rax + 4], 1 */
    emit_byte(e, 0x83); emit_byte(e, 0x40);
    emit_byte(e, (uint8_t)VAL_OFF_REFCOUNT); emit_byte(e, 0x01);
}

/* Advance frame->ip by one Instruction (4 bytes). r13 = frame. */
static void emit_advance_ip(Emitter *e) {
    /* add qword [r13 + FRAME_OFF_IP], 4 */
    emit_byte(e, 0x49); emit_byte(e, 0x83); emit_byte(e, 0x45);
    emit_byte(e, (uint8_t)FRAME_OFF_IP); emit_byte(e, 0x04);
}

/* Set frame->ip = code_addr (an absolute pointer). Used before
   delegating to vm_step so the dispatcher reads the same instruction
   the JIT was about to handle. r13 = frame. Clobbers rax. */
static void emit_set_ip_abs(Emitter *e, void *addr) {
    emit_mov_reg_imm64(e, RAX, (uint64_t)(uintptr_t)addr);
    /* mov [r13 + FRAME_OFF_IP], rax */
    emit_byte(e, 0x49); emit_byte(e, 0x89); emit_byte(e, 0x45);
    emit_byte(e, (uint8_t)FRAME_OFF_IP);
}

/* test eax, eax */
static void emit_test_eax(Emitter *e) {
    emit_byte(e, 0x85); emit_byte(e, 0xc0);
}
/* jnz rel8 placeholder (returns offset of the 1-byte displacement) */
static size_t emit_jnz_rel8_placeholder(Emitter *e) {
    emit_byte(e, 0x75); emit_byte(e, 0x00);
    return e->pos - 1;
}
/* jnz rel32 placeholder: 0x0F 0x85 + 4-byte disp; returns offset of disp */
static size_t emit_jnz_rel32_placeholder(Emitter *e) {
    emit_byte(e, 0x0f); emit_byte(e, 0x85);
    emit_byte(e, 0); emit_byte(e, 0); emit_byte(e, 0); emit_byte(e, 0);
    return e->pos - 4;
}
static void patch_rel8(Emitter *e, size_t pos) {
    long rel = (long)e->pos - (long)(pos + 1);
    if (rel < -128 || rel > 127) { e->overflow = 1; return; }
    e->buf[pos] = (uint8_t)(int8_t)rel;
}

/* Tier 1 JIT: emits a native dispatch loop that reads frame_count/ip
   at runtime. For every iteration it loads the current frame's next
   instruction, peels off the opcode, and either runs a native inline
   handler (hot ops: POP, DUP, PUSH_NULL/TRUE/FALSE, LOAD_LOCAL,
   STORE_LOCAL, PUSH_CONST, JUMP, LOOP) or falls through to vm_step
   for every other opcode. Inlining these fast paths bypasses both the
   vm_step() call overhead and the giant C dispatch switch. Since we
   look up ip at runtime rather than linearizing pc at compile time,
   the same compiled trampoline correctly handles calls, returns,
   throws, and any other control flow that vm_step manages. */
void *jit_compile(XSJIT *j, XSProto *proto) {
    if (!j || !j->available || !proto) return NULL;

    size_t est = 4096;
    if (j->code_used + est > j->code_size) return NULL;

    Emitter em;
    emit_init(&em, j->code, j->code_size, j->code_used);
    void *fn = (void *)(j->code + em.pos);

    /* We use:
         r12 = VM*
         r13 = current CallFrame*   (refreshed at top of each iteration)
         eax = loaded instruction word
       All other temp regs (rcx, rdx, rsi, rdi) are scratch. */

    /* ----- prologue -----
       At entry rsp % 16 == 8 (caller's call pushed ret addr). Three
       saved callee regs (r12, r13, r14) + rbp = 4 pushes (32B), so
       after the prologue rsp % 16 == 8. We then sub rsp, 8 below to
       re-align before any inner call. */
    emit_byte(&em, 0x55);                        /* push rbp */
    emit_mov_reg_reg(&em, RBP, RSP);             /* mov rbp, rsp */
    emit_byte(&em, 0x41); emit_byte(&em, 0x54);  /* push r12 */
    emit_byte(&em, 0x41); emit_byte(&em, 0x55);  /* push r13 */
    emit_byte(&em, 0x41); emit_byte(&em, 0x56);  /* push r14 (cached jump table base) */
    emit_mov_r12_rdi(&em);                       /* r12 = vm */
    /* movabs r14, <jump_table addr placeholder> -- patched after the
       table is emitted at the end. Caching it shaves a 10-byte
       movabs out of every dispatch loop iteration. */
    emit_byte(&em, 0x49); emit_byte(&em, 0xbe);
    size_t r14_table_patch = em.pos;
    emit_u64(&em, 0);
    /* mov dword [r12 + offsetof(VM, single_step)], 1
       Pinning single_step=1 once lets us call vm_step_jit (which
       skips the per-call save/restore of single_step) for the slow
       path, saving three memory writes per inlined op. The dispatch
       loop in vm.c bails out as soon as it sees single_step==1. */
    {
        size_t single_step_off = offsetof(VM, single_step);
        if (single_step_off <= 127) {
            emit_byte(&em, 0x41); emit_byte(&em, 0xc7);
            emit_byte(&em, 0x44); emit_byte(&em, 0x24);
            emit_byte(&em, (uint8_t)single_step_off);
            emit_i32(&em, 1);
        } else {
            emit_byte(&em, 0x41); emit_byte(&em, 0xc7);
            emit_byte(&em, 0x84); emit_byte(&em, 0x24);
            emit_i32(&em, (int32_t)single_step_off);
            emit_i32(&em, 1);
        }
    }
    /* Reserve 24 bytes of stack: 16 for arithmetic fast-path spill
       slots ([rsp], [rsp+8]) + 8 to re-align rsp to 16 (since the
       extra push r14 in the prologue made rsp 8-misaligned). */
    emit_sub_reg_imm8(&em, RSP, 24);

    /* Exit patches land here (after the shared epilogue gets emitted). */
    size_t *exit_patches = xs_calloc(256, sizeof(size_t));
    int n_exits = 0;
    size_t *done_patches = xs_calloc(256, sizeof(size_t));
    int n_dones = 0;
    /* Fast-path fallback jumps (rel32 jne placeholders) land at the
       slow path's vm_step call. Used by arithmetic ops that only fire
       their inline form when both operands are tagged XS_INT. */
    size_t *slow_patches = xs_calloc(256, sizeof(size_t));
    int n_slow = 0;
    /* Per-opcode handler offsets, used to fill the dispatch jump table
       at the end. Entries left at 0 fall through to the slow path. */
    size_t handler_offsets[256] = {0};

    /* ----- loop top ----- */
    size_t loop_top = em.pos;

    /* if (vm->frame_count == 0) goto done_ok; */
    /* mov ecx, [r12 + VM_OFF_FRAME_COUNT] */
    emit_byte(&em, 0x41); emit_byte(&em, 0x8b); emit_byte(&em, 0x4c);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAME_COUNT);
    /* test ecx, ecx */
    emit_byte(&em, 0x85); emit_byte(&em, 0xc9);
    /* jz done_ok (rel32 placeholder) */
    emit_byte(&em, 0x0f); emit_byte(&em, 0x84);
    emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0);
    done_patches[n_dones++] = em.pos - 4;

    /* Stack-headroom check: PUSH macro in vm.c calls vm_grow_stack if
       sp >= stack + cap. Our inlined push paths bypass that check, so
       at deep recursion we'd write past the buffer. Pre-grow when sp
       reaches the cached soft_limit (= stack + cap - 16); a single
       cmp + cold call keeps the loop overhead at one instruction. */
    /* mov rcx, [r12 + VM_OFF_SP] */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x4c);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    /* cmp rcx, [r12 + offsetof(VM, stack_soft_limit)] */
    {
        size_t off = offsetof(VM, stack_soft_limit);
        if (off < 128) {
            emit_byte(&em, 0x49); emit_byte(&em, 0x3b); emit_byte(&em, 0x4c);
            emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)off);
        } else {
            emit_byte(&em, 0x49); emit_byte(&em, 0x3b); emit_byte(&em, 0x8c);
            emit_byte(&em, 0x24); emit_i32(&em, (int32_t)off);
        }
    }
    /* jl .ok (rel8 placeholder) */
    emit_byte(&em, 0x7c); emit_byte(&em, 0x00);
    size_t grow_skip = em.pos - 1;
    /* mov rdi, r12; call vm_grow_stack */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
    emit_call_abs(&em, (void *)(uintptr_t)vm_grow_stack);
    em.buf[grow_skip] = (uint8_t)(em.pos - grow_skip - 1);

    /* r13 = &vm->frames[frame_count - 1] */
    /* movsxd rcx, dword [r12 + 36] */
    emit_byte(&em, 0x49); emit_byte(&em, 0x63); emit_byte(&em, 0x4c);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAME_COUNT);
    /* dec rcx */
    emit_byte(&em, 0x48); emit_byte(&em, 0xff); emit_byte(&em, 0xc9);
    /* imul rcx, rcx, FRAME_SIZE */
    emit_byte(&em, 0x48); emit_byte(&em, 0x69); emit_byte(&em, 0xc9);
    emit_i32(&em, FRAME_SIZE);
    /* mov rdx, [r12 + VM_OFF_FRAMES] */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x54);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_FRAMES);
    /* lea r13, [rdx + rcx] */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x8d); emit_byte(&em, 0x2c);
    emit_byte(&em, 0x0a);

    /* rax = *frame->ip  (32-bit instruction word) */
    /* mov rsi, [r13 + FRAME_OFF_IP] */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x75);
    emit_byte(&em, (uint8_t)FRAME_OFF_IP);
    /* mov eax, dword [rsi] */
    emit_byte(&em, 0x8b); emit_byte(&em, 0x06);

    /* O(1) opcode dispatch through a 256-entry jump table appended
       at the end of the emitted code. The table base lives in r14,
       cached once in the prologue, so each iteration costs just a
       movzx + indirect jmp. */
    /* movzx ecx, al */
    emit_byte(&em, 0x0f); emit_byte(&em, 0xb6); emit_byte(&em, 0xc8);
    /* jmp qword [r14 + rcx*8]   (4 bytes: 41 ff 24 ce) */
    emit_byte(&em, 0x41); emit_byte(&em, 0xff); emit_byte(&em, 0x24); emit_byte(&em, 0xce);

    /* The cmp/je chain that follows is now dead code reached only via
       the table; each handler's leading cmp+jne always matches because
       the table sent us here, so jne is never taken. Kept for clarity
       so the per-handler structure stays uniform. */

    #define INLINE_CMP_JNE(opval) do { \
        handler_offsets[(uint8_t)(opval)] = em.pos; \
        /* cmp al, opval */ \
        emit_byte(&em, 0x3c); emit_byte(&em, (uint8_t)(opval)); \
        /* jne rel32 placeholder (0x0F 0x85 + 4B disp) */ \
        emit_byte(&em, 0x0f); emit_byte(&em, 0x85); \
        emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); \
    } while (0)

    #define PATCH_JNE_HERE(patch_off) do { \
        size_t disp_pos = (patch_off); \
        long rel = (long)em.pos - (long)(disp_pos + 4); \
        em.buf[disp_pos + 0] = (uint8_t)(rel & 0xff); \
        em.buf[disp_pos + 1] = (uint8_t)((rel >> 8) & 0xff); \
        em.buf[disp_pos + 2] = (uint8_t)((rel >> 16) & 0xff); \
        em.buf[disp_pos + 3] = (uint8_t)((rel >> 24) & 0xff); \
    } while (0)

    #define JMP_REL32_TO(target) do { \
        /* jmp rel32 */ \
        emit_byte(&em, 0xe9); \
        long rel = (long)(target) - (long)(em.pos + 4); \
        emit_i32(&em, (int32_t)rel); \
    } while (0)

    size_t jne_patch;

    /* ---- OP_POP ---- */
    INLINE_CMP_JNE(OP_POP);
    jne_patch = em.pos - 4;
    /* mov rsi, [r12 + VM_OFF_SP] ; rsi = sp */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    /* sub rsi, 8 */
    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xee); emit_byte(&em, 0x08);
    /* mov [r12 + VM_OFF_SP], rsi */
    emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0x74);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    /* mov rdi, [rsi] */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x3e);
    emit_call_abs(&em, (void *)(uintptr_t)value_decref);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_DUP ---- */
    INLINE_CMP_JNE(OP_DUP);
    jne_patch = em.pos - 4;
    /* mov rsi, [r12 + 16]; rax = [rsi - 8]; mov [rsi], rax; add [r12+16], 8 */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    /* mov rax, [rsi - 8] */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x46);
    emit_byte(&em, 0xf8);
    /* mov [rsi], rax */
    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x06);
    /* add qword [r12 + 16], 8 */
    emit_byte(&em, 0x49); emit_byte(&em, 0x83); emit_byte(&em, 0x44);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    emit_byte(&em, 0x08);
    emit_inline_incref_rax(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_PUSH_NULL / OP_PUSH_TRUE / OP_PUSH_FALSE ----
       The singleton pointers are runtime-initialized externs (NULL at
       link time), so we load &VAR and dereference at native runtime. */
    extern Value *XS_NULL_VAL, *XS_TRUE_VAL, *XS_FALSE_VAL;
    INLINE_CMP_JNE(OP_PUSH_NULL);
    jne_patch = em.pos - 4;
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_NULL_VAL);
    /* mov rax, [rax] */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    INLINE_CMP_JNE(OP_PUSH_TRUE);
    jne_patch = em.pos - 4;
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    INLINE_CMP_JNE(OP_PUSH_FALSE);
    jne_patch = em.pos - 4;
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_LOAD_LOCAL ---- (Bx = slot, from instr >> 16) */
    INLINE_CMP_JNE(OP_LOAD_LOCAL);
    jne_patch = em.pos - 4;
    /* rcx = (instr >> 16) & 0xffff  -- slot index */
    /* mov ecx, eax */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    /* shr ecx, 16 */
    emit_byte(&em, 0xc1); emit_byte(&em, 0xe9); emit_byte(&em, 0x10);
    /* and ecx, 0xffff (optional, shr already zero-extended) */
    /* mov rdx, [r13 + FRAME_OFF_BASE]  ; rdx = frame->base */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x55);
    emit_byte(&em, (uint8_t)FRAME_OFF_BASE);
    /* mov rax, [rdx + rcx*8] */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x04);
    emit_byte(&em, 0xca);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_PUSH_CONST ---- (Bx = const idx)
       consts = frame->closure_val->cl->proto->chunk.consts
       offsets: closure_val=0 in CallFrame, cl=8 in Value, proto=0 in
       XSClosure, chunk.consts = 16 + 16 = 32 in XSProto. */
    INLINE_CMP_JNE(OP_PUSH_CONST);
    jne_patch = em.pos - 4;
    /* mov rdx, [r13]            ; rdx = closure_val (Value*) */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x55); emit_byte(&em, 0x00);
    /* mov rdx, [rdx + 8]        ; rdx = value->cl (XSClosure*) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x52); emit_byte(&em, 0x08);
    /* mov rdx, [rdx]            ; rdx = closure->proto */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x12);
    /* mov rdx, [rdx + 32]       ; rdx = chunk.consts (Value**) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x52); emit_byte(&em, 0x20);
    /* mov ecx, eax; shr ecx, 16  ; ecx = Bx */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    emit_byte(&em, 0xc1); emit_byte(&em, 0xe9); emit_byte(&em, 0x10);
    /* mov rax, [rdx + rcx*8] */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x04);
    emit_byte(&em, 0xca);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_STORE_LOCAL ---- (Bx = slot)
       Pop top, decref the existing local, store new pointer. */
    INLINE_CMP_JNE(OP_STORE_LOCAL);
    jne_patch = em.pos - 4;
    /* mov ecx, eax; shr ecx, 16  ; ecx = bx */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    emit_byte(&em, 0xc1); emit_byte(&em, 0xe9); emit_byte(&em, 0x10);
    /* mov rdx, [r13 + 16]       ; rdx = frame->base */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x55);
    emit_byte(&em, (uint8_t)FRAME_OFF_BASE);
    /* mov rsi, [r12 + 16]; sub rsi, 8; mov [r12+16], rsi  -- pop sp */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xee); emit_byte(&em, 0x08);
    emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0x74);
    emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP);
    /* mov rax, [rsi]            ; rax = popped Value*  */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x06);
    /* mov rdi, [rdx + rcx*8]    ; rdi = old local (to decref) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x3c);
    emit_byte(&em, 0xca);
    /* mov [rdx + rcx*8], rax    ; store new */
    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x04);
    emit_byte(&em, 0xca);
    /* test rdi, rdi; jz skip; call value_decref; */
    emit_byte(&em, 0x48); emit_byte(&em, 0x85); emit_byte(&em, 0xff);
    emit_byte(&em, 0x74); emit_byte(&em, 0x0c);  /* jz +12 (over the call) */
    emit_call_abs(&em, (void *)(uintptr_t)value_decref);
    /* skip: */
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_NOP ---- (just advance ip) */
    INLINE_CMP_JNE(OP_NOP);
    jne_patch = em.pos - 4;
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_JUMP / OP_LOOP ----
       In vm_dispatch, ip is pre-incremented before the switch fires
       (instr = *ip++), then JUMP adds sBx instructions. We handle the
       same net effect: ip += (sBx + 1) * 4 = (sBx + 1) instructions. */
    INLINE_CMP_JNE(OP_JUMP);
    jne_patch = em.pos - 4;
    /* movsx ecx, ax            ; sign-extended sBx is in bits 16-31 */
    /* sar eax, 16              ; eax = (int32_t)sBx (sign-extended) */
    emit_byte(&em, 0xc1); emit_byte(&em, 0xf8); emit_byte(&em, 0x10);
    /* movsxd rcx, eax          ; rcx = (int64_t)sBx */
    emit_byte(&em, 0x48); emit_byte(&em, 0x63); emit_byte(&em, 0xc8);
    /* inc rcx                   ; (sBx + 1) */
    emit_byte(&em, 0x48); emit_byte(&em, 0xff); emit_byte(&em, 0xc1);
    /* shl rcx, 2                ; * 4 bytes */
    emit_byte(&em, 0x48); emit_byte(&em, 0xc1); emit_byte(&em, 0xe1); emit_byte(&em, 0x02);
    /* add qword [r13 + 8], rcx */
    emit_byte(&em, 0x49); emit_byte(&em, 0x01); emit_byte(&em, 0x4d);
    emit_byte(&em, (uint8_t)FRAME_OFF_IP);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    INLINE_CMP_JNE(OP_LOOP);
    jne_patch = em.pos - 4;
    emit_byte(&em, 0xc1); emit_byte(&em, 0xf8); emit_byte(&em, 0x10);
    emit_byte(&em, 0x48); emit_byte(&em, 0x63); emit_byte(&em, 0xc8);
    emit_byte(&em, 0x48); emit_byte(&em, 0xff); emit_byte(&em, 0xc1);
    emit_byte(&em, 0x48); emit_byte(&em, 0xc1); emit_byte(&em, 0xe1); emit_byte(&em, 0x02);
    emit_byte(&em, 0x49); emit_byte(&em, 0x01); emit_byte(&em, 0x4d);
    emit_byte(&em, (uint8_t)FRAME_OFF_IP);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- Integer-fast-path arithmetic ----
       For OP_ADD, OP_SUB, OP_LT, OP_GT: if both operands are tagged
       XS_INT, compute natively and skip the vm_step trampoline. Any
       other operand type (float, string, bigint, map with dunder, ...)
       falls through to vm_step_jit, which handles the full semantics
       byte-for-byte like the VM. The int path matches VM behaviour via
       xs_safe_add/xs_safe_sub (which handle overflow to bigint). */

    /* Helper macro: emit "jne slow_path" rel32 placeholder and record
       the patch offset in slow_patches[]. */
    #define EMIT_JNE_TO_SLOW() do { \
        emit_byte(&em, 0x0f); emit_byte(&em, 0x85); \
        emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); \
        slow_patches[n_slow++] = em.pos - 4; \
    } while (0)

    /* Common prefix: load b (sp[-1]) and a (sp[-2]), check XS_INT/XS_INT.
       Leaves rax = b, rcx = a on success. rsi clobbered. */
    #define EMIT_BINOP_INT_CHECK() do { \
        /* mov rsi, [r12 + VM_OFF_SP] */ \
        emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        /* mov rax, [rsi - 8]   ; b */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x46); \
        emit_byte(&em, 0xf8); \
        /* mov rcx, [rsi - 16]  ; a */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x4e); \
        emit_byte(&em, 0xf0); \
        /* cmp dword [rax], XS_INT */ \
        emit_byte(&em, 0x83); emit_byte(&em, 0x38); emit_byte(&em, (uint8_t)XS_INT); \
        EMIT_JNE_TO_SLOW(); \
        /* cmp dword [rcx], XS_INT */ \
        emit_byte(&em, 0x83); emit_byte(&em, 0x39); emit_byte(&em, (uint8_t)XS_INT); \
        EMIT_JNE_TO_SLOW(); \
        /* mov [rsp], rcx       ; spill a */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x0c); \
        emit_byte(&em, 0x24); \
        /* mov [rsp + 8], rax   ; spill b */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x44); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
    } while (0)

    /* Common suffix: take computed result in rax, push to sp[-2],
       decref both original operands from spill, advance ip, loop. */
    #define EMIT_BINOP_FINISH() do { \
        /* mov rsi, [r12 + 16]; sub rsi, 8; mov [rsi - 8], rax; */ \
        emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xee); emit_byte(&em, 0x08); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x46); \
        emit_byte(&em, 0xf8); \
        /* mov [r12 + 16], rsi */ \
        emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        /* mov rdi, [rsp]; call value_decref */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x3c); emit_byte(&em, 0x24); \
        emit_call_abs(&em, (void *)(uintptr_t)value_decref); \
        /* mov rdi, [rsp + 8]; call value_decref */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x7c); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
        emit_call_abs(&em, (void *)(uintptr_t)value_decref); \
        emit_advance_ip(&em); \
        JMP_REL32_TO(loop_top); \
    } while (0)

    /* SMI-only fast paths. With SMIs, int arithmetic operates on the
       pointer bits directly -- no dereference, no malloc, no refcount.
       Identities used:
         a_smi + b_smi - 1 == ((a + b) << 1 | 1) == result_smi
         a_smi - b_smi + 1 == ((a - b) << 1 | 1) == result_smi
       Both work because SMI = (int << 1) | 1; the extra 1 cancels on
       add and reappears on sub. `jo` catches overflow beyond 63-bit
       SMI range and routes to the slow path. Any non-SMI operand
       (including heap XS_INT, float, bigint, map with dunder, ...)
       falls through to vm_step_jit which handles full semantics. */

    /* rax = b, rcx = a, rsi = sp; jump to slow path if either operand
       isn't a SMI. */
    #define EMIT_BOTH_SMI_CHECK() do { \
        emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x46); \
        emit_byte(&em, 0xf8); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x4e); \
        emit_byte(&em, 0xf0); \
        /* mov rdx, rax; and rdx, rcx; test dl, 1 */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xc2); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x21); emit_byte(&em, 0xca); \
        emit_byte(&em, 0xf6); emit_byte(&em, 0xc2); emit_byte(&em, 0x01); \
        /* jz slow_path (not both SMI) */ \
        emit_byte(&em, 0x0f); emit_byte(&em, 0x84); \
        emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); \
        slow_patches[n_slow++] = em.pos - 4; \
    } while (0)

    /* After an SMI arith op, rcx holds the result SMI pointer.
       Overwrite sp[-2] with it, sp -= 1, advance ip, loop.
       No decref needed -- SMIs have no heap refs. */
    #define EMIT_SMI_BINOP_FINISH() do { \
        emit_byte(&em, 0x49); emit_byte(&em, 0x83); emit_byte(&em, 0x6c); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); emit_byte(&em, 0x08); \
        emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        /* mov [rsi - 8], rcx */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x4e); \
        emit_byte(&em, 0xf8); \
        emit_advance_ip(&em); \
        JMP_REL32_TO(loop_top); \
    } while (0)

    /* emit "jo rel32" -> slow_patches */
    #define EMIT_JO_SLOW() do { \
        emit_byte(&em, 0x0f); emit_byte(&em, 0x80); \
        emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); \
        slow_patches[n_slow++] = em.pos - 4; \
    } while (0)

    /* ---- OP_ADD (SMI + SMI) ---- */
    INLINE_CMP_JNE(OP_ADD);
    jne_patch = em.pos - 4;
    EMIT_BOTH_SMI_CHECK();
    /* sub rcx, 1; add rcx, rax; jo slow */
    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xe9); emit_byte(&em, 0x01);
    emit_byte(&em, 0x48); emit_byte(&em, 0x01); emit_byte(&em, 0xc1);
    EMIT_JO_SLOW();
    EMIT_SMI_BINOP_FINISH();
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_SUB (SMI - SMI) ---- */
    INLINE_CMP_JNE(OP_SUB);
    jne_patch = em.pos - 4;
    EMIT_BOTH_SMI_CHECK();
    /* sub rcx, rax; jo slow; add rcx, 1 */
    emit_byte(&em, 0x48); emit_byte(&em, 0x29); emit_byte(&em, 0xc1);
    EMIT_JO_SLOW();
    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xc1); emit_byte(&em, 0x01);
    EMIT_SMI_BINOP_FINISH();
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_LT (SMI < SMI) ----
       Signed compare on SMI pointers works because (int << 1) | 1
       preserves sign ordering. Result is TRUE_VAL/FALSE_VAL singleton,
       pushed with incref (bool singletons are pinned but incref is a
       few bytes). */
    INLINE_CMP_JNE(OP_LT);
    jne_patch = em.pos - 4;
    EMIT_BOTH_SMI_CHECK();
    /* cmp rcx, rax */
    emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xc1);
    /* jl +12 to is_lt */
    emit_byte(&em, 0x7c); emit_byte(&em, 12);
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
    emit_byte(&em, 0xeb); emit_byte(&em, 10);
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
    /* mov rax, [rax] -- deref to get singleton */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    /* mov rcx, rax (result into rcx for SMI finish) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    EMIT_SMI_BINOP_FINISH();
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_GT (SMI > SMI) ---- */
    INLINE_CMP_JNE(OP_GT);
    jne_patch = em.pos - 4;
    EMIT_BOTH_SMI_CHECK();
    emit_byte(&em, 0x48); emit_byte(&em, 0x39); emit_byte(&em, 0xc1);
    emit_byte(&em, 0x7f); emit_byte(&em, 12);
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_FALSE_VAL);
    emit_byte(&em, 0xeb); emit_byte(&em, 10);
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_TRUE_VAL);
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    EMIT_SMI_BINOP_FINISH();
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_JUMP_IF_FALSE / OP_JUMP_IF_TRUE ----
       Pop the condition off the stack, run value_truthy, decref the
       condition, then either advance ip by 4 (no branch) or by
       (sBx + 1) * 4 (branch taken). Spill slot 0 holds instr across
       the calls; slot 1 holds the cond pointer then the truthy
       result. */

    /* Helper: emit the common cond-pop / truthy / decref preamble.
       After it: rax = instr (reloaded), ecx = truthy result.
       Clobbers rsi, rdi. */
    #define EMIT_JIF_PREAMBLE() do { \
        /* mov [rsp], rax  -- spill instr (slot 0) */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x04); \
        emit_byte(&em, 0x24); \
        /* Pop cond from value stack */ \
        emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xee); emit_byte(&em, 0x08); \
        emit_byte(&em, 0x49); emit_byte(&em, 0x89); emit_byte(&em, 0x74); \
        emit_byte(&em, 0x24); emit_byte(&em, (uint8_t)VM_OFF_SP); \
        /* mov rdi, [rsi]  -- cond Value*  */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x3e); \
        /* mov [rsp + 8], rdi  -- spill cond ptr (slot 1) */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x7c); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
        emit_call_abs(&em, (void *)(uintptr_t)value_truthy); \
        /* mov ecx, eax  -- save truthy (0/1) */ \
        emit_byte(&em, 0x89); emit_byte(&em, 0xc1); \
        /* mov rdi, [rsp + 8]  -- reload cond ptr */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x7c); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
        /* mov [rsp + 8], rcx  -- save truthy across decref */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x89); emit_byte(&em, 0x4c); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
        emit_call_abs(&em, (void *)(uintptr_t)value_decref); \
        /* mov rcx, [rsp + 8]  -- reload truthy */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x4c); \
        emit_byte(&em, 0x24); emit_byte(&em, 0x08); \
        /* mov rax, [rsp]  -- reload instr */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x04); \
        emit_byte(&em, 0x24); \
    } while (0)

    /* Helper: take a branch when ecx satisfies the condition encoded
       in `branch_when_set` (1 = truthy taken, 0 = falsy taken). On
       branch: ip += (sBx + 1) * 4. Else: ip += 4. */
    #define EMIT_JIF_TAIL(branch_when_set) do { \
        emit_byte(&em, 0x85); emit_byte(&em, 0xc9);                /* test ecx, ecx */ \
        emit_byte(&em, (branch_when_set) ? 0x74 : 0x75);            /* j(z|nz) skip_branch */ \
        size_t skip_pos = em.pos; emit_byte(&em, 0); \
        /* branch taken: ip += (sBx + 1) * 4 */ \
        emit_byte(&em, 0xc1); emit_byte(&em, 0xf8); emit_byte(&em, 0x10);  /* sar eax, 16 */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0x63); emit_byte(&em, 0xc8);  /* movsxd rcx, eax */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0xff); emit_byte(&em, 0xc1);  /* inc rcx */ \
        emit_byte(&em, 0x48); emit_byte(&em, 0xc1); emit_byte(&em, 0xe1); emit_byte(&em, 0x02); /* shl rcx, 2 */ \
        emit_byte(&em, 0x49); emit_byte(&em, 0x01); emit_byte(&em, 0x4d); emit_byte(&em, (uint8_t)FRAME_OFF_IP); \
        emit_byte(&em, 0xeb);                                              /* jmp .done */ \
        size_t done_pos = em.pos; emit_byte(&em, 0); \
        /* skip_branch: ip += 4 */ \
        em.buf[skip_pos] = (uint8_t)(em.pos - skip_pos - 1); \
        emit_advance_ip(&em); \
        /* .done: */ \
        em.buf[done_pos] = (uint8_t)(em.pos - done_pos - 1); \
    } while (0)

    /* ---- OP_LOAD_GLOBAL ----
       Decode Bx, fetch consts[Bx]->s (the string name) from the
       current proto, call map_get(vm->globals, name), incref result
       (or XS_NULL_VAL on miss), push, advance ip. */
    INLINE_CMP_JNE(OP_LOAD_GLOBAL);
    jne_patch = em.pos - 4;
    /* rdx = consts ptr (same chain as OP_PUSH_CONST) */
    /* mov rdx, [r13]            ; closure_val */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x55); emit_byte(&em, 0x00);
    /* mov rdx, [rdx + 8]        ; cl */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x52); emit_byte(&em, 0x08);
    /* mov rdx, [rdx]            ; proto */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x12);
    /* mov rdx, [rdx + 32]       ; chunk.consts */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x52); emit_byte(&em, 0x20);
    /* mov ecx, eax; shr ecx, 16 */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    emit_byte(&em, 0xc1); emit_byte(&em, 0xe9); emit_byte(&em, 0x10);
    /* mov rdx, [rdx + rcx*8]    ; consts[Bx]  (Value*) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x14); emit_byte(&em, 0xca);
    /* mov rsi, [rdx + 8]        ; .s (string union member, offset 8) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x72); emit_byte(&em, 0x08);
    /* mov rdi, [r12 + 48]       ; vm->globals */
    emit_byte(&em, 0x49); emit_byte(&em, 0x8b); emit_byte(&em, 0x7c);
    emit_byte(&em, 0x24); emit_byte(&em, 0x30);
    emit_call_abs(&em, (void *)(uintptr_t)map_get);
    /* If rax == NULL, replace with *XS_NULL_VAL. */
    /* test rax, rax */
    emit_byte(&em, 0x48); emit_byte(&em, 0x85); emit_byte(&em, 0xc0);
    /* jnz +13 (skip 10-byte movabs + 3-byte deref of NULL_VAL) */
    emit_byte(&em, 0x75); emit_byte(&em, 13);
    /* mov rax, &XS_NULL_VAL  (10 bytes) */
    emit_mov_reg_imm64(&em, RAX, (uint64_t)(uintptr_t)&XS_NULL_VAL);
    /* mov rax, [rax]  (3 bytes) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x8b); emit_byte(&em, 0x00);
    emit_inline_incref_rax(&em);
    emit_push_rax_to_vm_sp(&em);
    emit_advance_ip(&em);
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_RETURN (simple frame teardown) ----
       Calls vm_return_fast, which bails out (returns 1) on defer,
       generator, top-frame, init/spawn, or open upvalues. On 0, the
       caller's frame is now top and its ip is already past OP_CALL. */
    INLINE_CMP_JNE(OP_RETURN);
    jne_patch = em.pos - 4;
    /* mov rdi, r12; call vm_return_fast */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
    emit_call_abs(&em, (void *)(uintptr_t)vm_return_fast);
    emit_byte(&em, 0x85); emit_byte(&em, 0xc0);
    emit_byte(&em, 0x75); emit_byte(&em, 0x05);  /* jnz fallback */
    JMP_REL32_TO(loop_top);
    /* fallback: vm_step_jit */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
    emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
    emit_byte(&em, 0x85); emit_byte(&em, 0xc0);
    emit_byte(&em, 0x0f); emit_byte(&em, 0x85);
    emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0);
    exit_patches[n_exits++] = em.pos - 4;
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- OP_CALL (closure-only fast path) ----
       Decode argc from byte C (instr >> 24), call vm_call_closure_fast.
       It returns 0 if it took the call (frame pushed), 1 if not
       eligible (overload, native, varargs, gen, ...). On 1 we fall
       through to vm_step_jit which handles the full semantics. */
    INLINE_CMP_JNE(OP_CALL);
    jne_patch = em.pos - 4;
    /* mov esi, eax; shr esi, 24  ; esi = argc */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc6);
    emit_byte(&em, 0xc1); emit_byte(&em, 0xee); emit_byte(&em, 0x18);
    /* mov rdi, r12 */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
    emit_call_abs(&em, (void *)(uintptr_t)vm_call_closure_fast);
    /* test eax, eax */
    emit_byte(&em, 0x85); emit_byte(&em, 0xc0);
    /* jnz .fallback (fall through to vm_step) */
    emit_byte(&em, 0x75); emit_byte(&em, 0x05);
    /* eax was 0 = success: jmp loop_top */
    JMP_REL32_TO(loop_top);
    /* .fallback: roll back -- on failure vm_call_closure_fast didn't
       touch state, so just fall to vm_step which re-reads the same
       instruction. */
    {
        /* mov rdi, r12; call vm_step_jit; test eax,eax; jnz exit;
           jmp loop_top -- duplicates the slow path inline so we don't
           need a backward jump. Cheaper than a jmp + label. */
        emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
        emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
        emit_byte(&em, 0x85); emit_byte(&em, 0xc0);
        emit_byte(&em, 0x0f); emit_byte(&em, 0x85);
        emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0);
        exit_patches[n_exits++] = em.pos - 4;
        JMP_REL32_TO(loop_top);
    }
    PATCH_JNE_HERE(jne_patch);

    /* OP_JUMP_IF_FALSE: branch when cond is falsy (truthy == 0). */
    INLINE_CMP_JNE(OP_JUMP_IF_FALSE);
    jne_patch = em.pos - 4;
    EMIT_JIF_PREAMBLE();
    EMIT_JIF_TAIL(0);  /* branch when truthy clear -> skip with jnz */
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* OP_JUMP_IF_TRUE: branch when cond is truthy. */
    INLINE_CMP_JNE(OP_JUMP_IF_TRUE);
    jne_patch = em.pos - 4;
    EMIT_JIF_PREAMBLE();
    EMIT_JIF_TAIL(1);  /* branch when truthy set -> skip with jz */
    JMP_REL32_TO(loop_top);
    PATCH_JNE_HERE(jne_patch);

    /* ---- Slow path: call vm_step_jit ---- */
    size_t slow_path_pos = em.pos;
    /* mov rdi, r12 */
    emit_byte(&em, 0x4c); emit_byte(&em, 0x89); emit_byte(&em, 0xe7);
    emit_call_abs(&em, (void *)(uintptr_t)vm_step_jit);
    /* test eax, eax */
    emit_test_eax(&em);
    /* jnz exit (rel32) */
    emit_byte(&em, 0x0f); emit_byte(&em, 0x85);
    emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0); emit_byte(&em, 0);
    exit_patches[n_exits++] = em.pos - 4;
    /* jmp loop_top (rel32) */
    JMP_REL32_TO(loop_top);

    /* ----- epilogue (error exit: rc in eax, negative = error) ----- */
    size_t epilogue_pos = em.pos;
    /* mov ecx, eax  (preserve rc) */
    emit_byte(&em, 0x89); emit_byte(&em, 0xc1);
    /* xor eax, eax */
    emit_byte(&em, 0x31); emit_byte(&em, 0xc0);
    /* test ecx, ecx */
    emit_byte(&em, 0x85); emit_byte(&em, 0xc9);
    /* jns +5 */
    emit_byte(&em, 0x79); emit_byte(&em, 0x05);
    /* mov eax, 1 */
    emit_byte(&em, 0xb8);
    emit_i32(&em, 1);

    /* done_ok target: rc = 0 */
    size_t done_ok_pos = em.pos;
    /* xor eax, eax */
    emit_byte(&em, 0x31); emit_byte(&em, 0xc0);

    /* shared return: clear single_step before exiting so unrelated
       vm_dispatch callers downstream (REPL, embed, recursive callbacks
       outside the JIT) see normal behaviour. */
    {
        size_t single_step_off = offsetof(VM, single_step);
        if (single_step_off <= 127) {
            emit_byte(&em, 0x41); emit_byte(&em, 0xc7);
            emit_byte(&em, 0x44); emit_byte(&em, 0x24);
            emit_byte(&em, (uint8_t)single_step_off);
            emit_i32(&em, 0);
        } else {
            emit_byte(&em, 0x41); emit_byte(&em, 0xc7);
            emit_byte(&em, 0x84); emit_byte(&em, 0x24);
            emit_i32(&em, (int32_t)single_step_off);
            emit_i32(&em, 0);
        }
    }
    /* add rsp, 24  (free the spill + alignment pad) */
    emit_byte(&em, 0x48); emit_byte(&em, 0x83); emit_byte(&em, 0xc4); emit_byte(&em, 0x18);
    /* pop r14 */
    emit_byte(&em, 0x41); emit_byte(&em, 0x5e);
    /* pop r13 */
    emit_byte(&em, 0x41); emit_byte(&em, 0x5d);
    /* pop r12 */
    emit_byte(&em, 0x41); emit_byte(&em, 0x5c);
    /* pop rbp */
    emit_byte(&em, 0x5d);
    /* ret */
    emit_byte(&em, 0xc3);

    /* Patch arithmetic fast-path "jne slow_path" placeholders. */
    for (int i = 0; i < n_slow; i++) {
        size_t disp_pos = slow_patches[i];
        long rel = (long)slow_path_pos - (long)(disp_pos + 4);
        em.buf[disp_pos + 0] = (uint8_t)(rel & 0xff);
        em.buf[disp_pos + 1] = (uint8_t)((rel >> 8) & 0xff);
        em.buf[disp_pos + 2] = (uint8_t)((rel >> 16) & 0xff);
        em.buf[disp_pos + 3] = (uint8_t)((rel >> 24) & 0xff);
    }
    free(slow_patches);

    /* Append the jump table after the epilogue, 8-byte aligned. Each
       slot holds the absolute address (in this code page) of a handler
       or the slow path. Patch the prologue's r14 = table_addr load. */
    while (em.pos % 8) emit_byte(&em, 0x90);
    size_t table_pos = em.pos;
    for (int op = 0; op < 256; op++) {
        size_t off = handler_offsets[op] ? handler_offsets[op] : slow_path_pos;
        emit_u64(&em, (uint64_t)(uintptr_t)(j->code + off));
    }
    {
        uint64_t addr = (uint64_t)(uintptr_t)(j->code + table_pos);
        memcpy(&em.buf[r14_table_patch], &addr, 8);
    }

    /* Patch slow-path "jnz exit" -> epilogue_pos */
    for (int i = 0; i < n_exits; i++) {
        size_t disp_pos = exit_patches[i];
        long rel = (long)epilogue_pos - (long)(disp_pos + 4);
        em.buf[disp_pos + 0] = (uint8_t)(rel & 0xff);
        em.buf[disp_pos + 1] = (uint8_t)((rel >> 8) & 0xff);
        em.buf[disp_pos + 2] = (uint8_t)((rel >> 16) & 0xff);
        em.buf[disp_pos + 3] = (uint8_t)((rel >> 24) & 0xff);
    }
    /* Patch "jz done_ok" at top of loop */
    for (int i = 0; i < n_dones; i++) {
        size_t disp_pos = done_patches[i];
        long rel = (long)done_ok_pos - (long)(disp_pos + 4);
        em.buf[disp_pos + 0] = (uint8_t)(rel & 0xff);
        em.buf[disp_pos + 1] = (uint8_t)((rel >> 8) & 0xff);
        em.buf[disp_pos + 2] = (uint8_t)((rel >> 16) & 0xff);
        em.buf[disp_pos + 3] = (uint8_t)((rel >> 24) & 0xff);
    }
    free(exit_patches);
    free(done_patches);

    if (em.overflow) return NULL;
    j->code_used = em.pos;
    (void)patch_rel8;
    (void)emit_refresh_frame;
    (void)emit_set_ip_abs;
    (void)emit_jnz_rel8_placeholder;
    (void)proto;
    if (getenv("XS_JIT_DUMP")) {
        FILE *fp = fopen("/tmp/jit.bin", "wb");
        if (fp) {
            size_t n = em.pos - (size_t)((uint8_t *)fn - j->code);
            fwrite(fn, 1, n, fp); fclose(fp);
            fprintf(stderr, "[jit] %zuB at %p\n", n, fn);
        }
    }
    return fn;
}

#else

void *jit_compile(XSJIT *j, XSProto *proto) {
    (void)j; (void)proto;
    return NULL;
}

#endif


void *jit_maybe_compile(XSJIT *j, int proto_index, XSProto *proto) {
    if (!j || !j->available) return NULL;
    if (proto_index < 0 || proto_index >= j->n_protos) return NULL;

    if (j->compiled[proto_index]) return j->compiled[proto_index];

    j->call_counts[proto_index]++;
    if (j->call_counts[proto_index] < XS_JIT_THRESHOLD) return NULL;

    void *fn = jit_compile(j, proto);
    if (fn) j->compiled[proto_index] = fn;
    return fn;
}

JitFn jit_get_compiled(XSJIT *j, int proto_index) {
    if (!j || !j->available) return NULL;
    if (proto_index < 0 || proto_index >= j->n_protos) return NULL;
    return (JitFn)j->compiled[proto_index];
}

int jit_available(void) {
#if JIT_ARCH_X86_64 && JIT_HAS_MMAP
    return 1;
#else
    return 0;
#endif
}

#else
/* jit.h provides static inline fallbacks when JIT is disabled. */
#pragma GCC diagnostic pop
#endif
