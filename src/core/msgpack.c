#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/* msgpack.c - full MessagePack encoder/decoder for XS values.
 *
 * Implements the MessagePack specification (https://msgpack.org)
 * with extension types for XS-specific values like regex, dates,
 * bigints, ranges, and tuples.
 *
 * Zero external dependencies; all encoding/decoding is done inline.
 */

#include "core/msgpack.h"
#include "core/value.h"
#include "core/xs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <float.h>

#define MP_MAX_DEPTH 128
#define MP_INITIAL_CAP 256

/* ----------------------------------------------------------------
 * Buffer management
 * ---------------------------------------------------------------- */

void mp_buf_init(MsgPackBuf *buf) {
    buf->data = xs_malloc(MP_INITIAL_CAP);
    buf->len = 0;
    buf->cap = MP_INITIAL_CAP;
}

void mp_buf_free(MsgPackBuf *buf) {
    if (buf->data) free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void mp_buf_grow(MsgPackBuf *buf, size_t need) {
    if (buf->len + need <= buf->cap) return;
    size_t newcap = buf->cap;
    while (newcap < buf->len + need)
        newcap = newcap < 1024 ? newcap * 4 : newcap * 2;
    buf->data = xs_realloc(buf->data, newcap);
    buf->cap = newcap;
}

void mp_buf_write(MsgPackBuf *buf, const void *src, size_t n) {
    mp_buf_grow(buf, n);
    memcpy(buf->data + buf->len, src, n);
    buf->len += n;
}

void mp_buf_write_u8(MsgPackBuf *buf, uint8_t v) {
    mp_buf_grow(buf, 1);
    buf->data[buf->len++] = v;
}

/* big-endian writers for wire format */
void mp_buf_write_u16(MsgPackBuf *buf, uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v);
    mp_buf_write(buf, b, 2);
}

void mp_buf_write_u32(MsgPackBuf *buf, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)(v);
    mp_buf_write(buf, b, 4);
}

void mp_buf_write_u64(MsgPackBuf *buf, uint64_t v) {
    uint8_t b[8];
    b[0] = (uint8_t)(v >> 56);
    b[1] = (uint8_t)(v >> 48);
    b[2] = (uint8_t)(v >> 40);
    b[3] = (uint8_t)(v >> 32);
    b[4] = (uint8_t)(v >> 24);
    b[5] = (uint8_t)(v >> 16);
    b[6] = (uint8_t)(v >> 8);
    b[7] = (uint8_t)(v);
    mp_buf_write(buf, b, 8);
}

void mp_buf_write_i8(MsgPackBuf *buf, int8_t v) {
    mp_buf_write_u8(buf, (uint8_t)v);
}

void mp_buf_write_i16(MsgPackBuf *buf, int16_t v) {
    mp_buf_write_u16(buf, (uint16_t)v);
}

void mp_buf_write_i32(MsgPackBuf *buf, int32_t v) {
    mp_buf_write_u32(buf, (uint32_t)v);
}

void mp_buf_write_i64(MsgPackBuf *buf, int64_t v) {
    mp_buf_write_u64(buf, (uint64_t)v);
}

void mp_buf_write_f32(MsgPackBuf *buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    mp_buf_write_u32(buf, conv.u);
}

void mp_buf_write_f64(MsgPackBuf *buf, double v) {
    union { double d; uint64_t u; } conv;
    conv.d = v;
    mp_buf_write_u64(buf, conv.u);
}

/* ----------------------------------------------------------------
 * Primitive encoders
 * ---------------------------------------------------------------- */

void mp_pack_nil(MsgPackBuf *buf) {
    mp_buf_write_u8(buf, MP_NIL);
}

void mp_pack_bool(MsgPackBuf *buf, int v) {
    mp_buf_write_u8(buf, v ? MP_TRUE : MP_FALSE);
}

void mp_pack_int(MsgPackBuf *buf, int64_t v) {
    if (v >= 0) {
        mp_pack_uint(buf, (uint64_t)v);
        return;
    }
    /* negative integers */
    if (v >= -32) {
        /* negative fixint: 111xxxxx */
        mp_buf_write_u8(buf, (uint8_t)(v & 0xff));
    } else if (v >= INT8_MIN) {
        mp_buf_write_u8(buf, MP_INT8);
        mp_buf_write_i8(buf, (int8_t)v);
    } else if (v >= INT16_MIN) {
        mp_buf_write_u8(buf, MP_INT16);
        mp_buf_write_i16(buf, (int16_t)v);
    } else if (v >= INT32_MIN) {
        mp_buf_write_u8(buf, MP_INT32);
        mp_buf_write_i32(buf, (int32_t)v);
    } else {
        mp_buf_write_u8(buf, MP_INT64);
        mp_buf_write_i64(buf, v);
    }
}

void mp_pack_uint(MsgPackBuf *buf, uint64_t v) {
    if (v <= 0x7f) {
        /* positive fixint */
        mp_buf_write_u8(buf, (uint8_t)v);
    } else if (v <= 0xff) {
        mp_buf_write_u8(buf, MP_UINT8);
        mp_buf_write_u8(buf, (uint8_t)v);
    } else if (v <= 0xffff) {
        mp_buf_write_u8(buf, MP_UINT16);
        mp_buf_write_u16(buf, (uint16_t)v);
    } else if (v <= 0xffffffff) {
        mp_buf_write_u8(buf, MP_UINT32);
        mp_buf_write_u32(buf, (uint32_t)v);
    } else {
        mp_buf_write_u8(buf, MP_UINT64);
        mp_buf_write_u64(buf, v);
    }
}

void mp_pack_float(MsgPackBuf *buf, double v) {
    /* try float32 if it round-trips without precision loss */
    float f32 = (float)v;
    if ((double)f32 == v && !isnan(v)) {
        mp_buf_write_u8(buf, MP_FLOAT32);
        mp_buf_write_f32(buf, f32);
    } else {
        mp_buf_write_u8(buf, MP_FLOAT64);
        mp_buf_write_f64(buf, v);
    }
}

void mp_pack_str(MsgPackBuf *buf, const char *s, uint32_t len) {
    if (len <= 31) {
        mp_buf_write_u8(buf, (uint8_t)(MP_FIXSTR | len));
    } else if (len <= 0xff) {
        mp_buf_write_u8(buf, MP_STR8);
        mp_buf_write_u8(buf, (uint8_t)len);
    } else if (len <= 0xffff) {
        mp_buf_write_u8(buf, MP_STR16);
        mp_buf_write_u16(buf, (uint16_t)len);
    } else {
        mp_buf_write_u8(buf, MP_STR32);
        mp_buf_write_u32(buf, len);
    }
    if (len > 0) mp_buf_write(buf, s, len);
}

void mp_pack_bin(MsgPackBuf *buf, const uint8_t *data, uint32_t len) {
    if (len <= 0xff) {
        mp_buf_write_u8(buf, MP_BIN8);
        mp_buf_write_u8(buf, (uint8_t)len);
    } else if (len <= 0xffff) {
        mp_buf_write_u8(buf, MP_BIN16);
        mp_buf_write_u16(buf, (uint16_t)len);
    } else {
        mp_buf_write_u8(buf, MP_BIN32);
        mp_buf_write_u32(buf, len);
    }
    if (len > 0) mp_buf_write(buf, data, len);
}

void mp_pack_array_header(MsgPackBuf *buf, uint32_t count) {
    if (count <= 15) {
        mp_buf_write_u8(buf, (uint8_t)(MP_FIXARRAY | count));
    } else if (count <= 0xffff) {
        mp_buf_write_u8(buf, MP_ARRAY16);
        mp_buf_write_u16(buf, (uint16_t)count);
    } else {
        mp_buf_write_u8(buf, MP_ARRAY32);
        mp_buf_write_u32(buf, count);
    }
}

void mp_pack_map_header(MsgPackBuf *buf, uint32_t count) {
    if (count <= 15) {
        mp_buf_write_u8(buf, (uint8_t)(MP_FIXMAP | count));
    } else if (count <= 0xffff) {
        mp_buf_write_u8(buf, MP_MAP16);
        mp_buf_write_u16(buf, (uint16_t)count);
    } else {
        mp_buf_write_u8(buf, MP_MAP32);
        mp_buf_write_u32(buf, count);
    }
}

void mp_pack_ext(MsgPackBuf *buf, int8_t type, const uint8_t *data, uint32_t len) {
    if (len == 1) {
        mp_buf_write_u8(buf, MP_FIXEXT1);
    } else if (len == 2) {
        mp_buf_write_u8(buf, MP_FIXEXT2);
    } else if (len == 4) {
        mp_buf_write_u8(buf, MP_FIXEXT4);
    } else if (len == 8) {
        mp_buf_write_u8(buf, MP_FIXEXT8);
    } else if (len == 16) {
        mp_buf_write_u8(buf, MP_FIXEXT16);
    } else if (len <= 0xff) {
        mp_buf_write_u8(buf, MP_EXT8);
        mp_buf_write_u8(buf, (uint8_t)len);
    } else if (len <= 0xffff) {
        mp_buf_write_u8(buf, MP_EXT16);
        mp_buf_write_u16(buf, (uint16_t)len);
    } else {
        mp_buf_write_u8(buf, MP_EXT32);
        mp_buf_write_u32(buf, len);
    }
    mp_buf_write_u8(buf, (uint8_t)type);
    if (len > 0) mp_buf_write(buf, data, len);
}

/* ----------------------------------------------------------------
 * Pack an XS Value recursively
 * ---------------------------------------------------------------- */

static void mp_pack_struct(MsgPackBuf *buf, XSStruct *st, int depth);
static void mp_pack_enum(MsgPackBuf *buf, XSEnum *en, int depth);

int mp_pack_value(MsgPackBuf *buf, Value *v, int depth) {
    if (depth > MP_MAX_DEPTH) return -1;
    if (!v) { mp_pack_nil(buf); return 0; }

    switch (v->tag) {
    case XS_NULL:
        mp_pack_nil(buf);
        break;

    case XS_BOOL:
        mp_pack_bool(buf, v->i != 0);
        break;

    case XS_INT:
        mp_pack_int(buf, v->i);
        break;

    case XS_FLOAT:
        mp_pack_float(buf, v->f);
        break;

    case XS_STR: {
        uint32_t slen = v->s ? (uint32_t)strlen(v->s) : 0;
        mp_pack_str(buf, v->s ? v->s : "", slen);
        break;
    }
    case XS_CHAR: {
        char cs[2] = { (char)v->i, 0 };
        mp_pack_str(buf, cs, 1);
        break;
    }
    case XS_ARRAY: {
        if (!v->arr) { mp_pack_array_header(buf, 0); break; }
        uint32_t n = (uint32_t)v->arr->len;
        mp_pack_array_header(buf, n);
        for (uint32_t i = 0; i < n; i++) {
            if (mp_pack_value(buf, v->arr->items[i], depth + 1) < 0)
                return -1;
        }
        break;
    }
    case XS_TUPLE: {
        if (!v->arr) { mp_pack_array_header(buf, 0); break; }
        uint32_t n = (uint32_t)v->arr->len;
        /* encode as ext type so we can round-trip tuple vs array */
        MsgPackBuf inner;
        mp_buf_init(&inner);
        mp_pack_array_header(&inner, n);
        for (uint32_t i = 0; i < n; i++) {
            if (mp_pack_value(&inner, v->arr->items[i], depth + 1) < 0) {
                mp_buf_free(&inner);
                return -1;
            }
        }
        mp_pack_ext(buf, MP_EXT_TUPLE, inner.data, (uint32_t)inner.len);
        mp_buf_free(&inner);
        break;
    }
    case XS_MAP: {
        if (!v->map) { mp_pack_map_header(buf, 0); break; }
        int nk = 0;
        char **ks = map_keys(v->map, &nk);
        mp_pack_map_header(buf, (uint32_t)nk);
        for (int ki = 0; ki < nk; ki++) {
            uint32_t klen = ks[ki] ? (uint32_t)strlen(ks[ki]) : 0;
            mp_pack_str(buf, ks[ki] ? ks[ki] : "", klen);
            Value *val = map_get(v->map, ks[ki]);
            if (mp_pack_value(buf, val, depth + 1) < 0) {
                for (int j = ki; j < nk; j++) free(ks[j]);
                free(ks);
                return -1;
            }
            free(ks[ki]);
        }
        free(ks);
        break;
    }
    case XS_STRUCT_VAL:
        mp_pack_struct(buf, v->st, depth);
        break;

    case XS_ENUM_VAL:
        mp_pack_enum(buf, v->en, depth);
        break;

    case XS_RANGE: {
        if (!v->range) { mp_pack_nil(buf); break; }
        /* encode range as ext with 4 int64 values: start, end, step, inclusive */
        MsgPackBuf inner;
        mp_buf_init(&inner);
        mp_pack_int(&inner, v->range->start);
        mp_pack_int(&inner, v->range->end);
        mp_pack_int(&inner, v->range->step);
        mp_pack_int(&inner, v->range->inclusive);
        mp_pack_ext(buf, MP_EXT_RANGE, inner.data, (uint32_t)inner.len);
        mp_buf_free(&inner);
        break;
    }
    case XS_REGEX: {
        const char *pattern = v->s ? v->s : "";
        uint32_t plen = (uint32_t)strlen(pattern);
        mp_pack_ext(buf, MP_EXT_REGEX, (const uint8_t *)pattern, plen);
        break;
    }
    case XS_BIGINT: {
        /* serialize bigint as string representation */
        char *bs = value_str(v);
        uint32_t blen = bs ? (uint32_t)strlen(bs) : 0;
        mp_pack_ext(buf, MP_EXT_BIGINT, (const uint8_t *)bs, blen);
        if (bs) free(bs);
        break;
    }
    case XS_MODULE: {
        /* encode module fields as a map, skip functions */
        if (!v->map) { mp_pack_map_header(buf, 0); break; }
        int nk = 0;
        char **ks = map_keys(v->map, &nk);
        /* count serializable entries (skip functions) */
        int ser_count = 0;
        for (int ki = 0; ki < nk; ki++) {
            Value *val = map_get(v->map, ks[ki]);
            if (val && (val->tag == XS_FUNC || val->tag == XS_NATIVE))
                continue;
            ser_count++;
        }
        mp_pack_map_header(buf, (uint32_t)ser_count);
        for (int ki = 0; ki < nk; ki++) {
            Value *val = map_get(v->map, ks[ki]);
            if (val && (val->tag == XS_FUNC || val->tag == XS_NATIVE)) {
                free(ks[ki]);
                continue;
            }
            uint32_t klen = ks[ki] ? (uint32_t)strlen(ks[ki]) : 0;
            mp_pack_str(buf, ks[ki] ? ks[ki] : "", klen);
            mp_pack_value(buf, val, depth + 1);
            free(ks[ki]);
        }
        free(ks);
        break;
    }
    default:
        /* functions and other non-serializable types become nil */
        mp_pack_nil(buf);
        break;
    }
    return 0;
}

static void mp_pack_struct(MsgPackBuf *buf, XSStruct *st, int depth) {
    if (!st) { mp_pack_nil(buf); return; }
    int nk = 0;
    char **ks = st->fields ? map_keys(st->fields, &nk) : NULL;
    mp_pack_map_header(buf, (uint32_t)(nk + 1));

    mp_pack_str(buf, "__type", 6);
    uint32_t tlen = st->type_name ? (uint32_t)strlen(st->type_name) : 0;
    mp_pack_str(buf, st->type_name ? st->type_name : "", tlen);

    for (int i = 0; i < nk; i++) {
        uint32_t klen = ks[i] ? (uint32_t)strlen(ks[i]) : 0;
        mp_pack_str(buf, ks[i] ? ks[i] : "", klen);
        Value *val = map_get(st->fields, ks[i]);
        mp_pack_value(buf, val, depth + 1);
        free(ks[i]);
    }
    free(ks);
}

static void mp_pack_enum(MsgPackBuf *buf, XSEnum *en, int depth) {
    if (!en) { mp_pack_nil(buf); return; }
    /* enum -> map with __enum and __variant */
    int extra_fields = 2;
    int data_count = en->arr_data ? en->arr_data->len : 0;
    int map_count = en->map_data ? en->map_data->len : 0;
    int total = extra_fields + (data_count > 0 ? 1 : 0) + (map_count > 0 ? 1 : 0);
    mp_pack_map_header(buf, (uint32_t)total);

    mp_pack_str(buf, "__enum", 6);
    uint32_t tlen = en->type_name ? (uint32_t)strlen(en->type_name) : 0;
    mp_pack_str(buf, en->type_name ? en->type_name : "", tlen);

    mp_pack_str(buf, "__variant", 9);
    uint32_t vlen = en->variant ? (uint32_t)strlen(en->variant) : 0;
    mp_pack_str(buf, en->variant ? en->variant : "", vlen);

    if (data_count > 0) {
        mp_pack_str(buf, "__data", 6);
        mp_pack_array_header(buf, (uint32_t)data_count);
        for (int i = 0; i < data_count; i++)
            mp_pack_value(buf, en->arr_data->items[i], depth + 1);
    }
    if (map_count > 0) {
        int mnk = 0;
        char **mks = map_keys(en->map_data, &mnk);
        mp_pack_str(buf, "__fields", 8);
        mp_pack_map_header(buf, (uint32_t)mnk);
        for (int i = 0; i < mnk; i++) {
            uint32_t klen = mks[i] ? (uint32_t)strlen(mks[i]) : 0;
            mp_pack_str(buf, mks[i] ? mks[i] : "", klen);
            Value *val = map_get(en->map_data, mks[i]);
            mp_pack_value(buf, val, depth + 1);
            free(mks[i]);
        }
        free(mks);
    }
}

/* ----------------------------------------------------------------
 * Reader (decoder) primitives
 * ---------------------------------------------------------------- */

void mp_reader_init(MsgPackReader *r, const uint8_t *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->error[0] = '\0';
    r->has_error = 0;
}

int mp_reader_has_data(MsgPackReader *r) {
    return !r->has_error && r->pos < r->len;
}

static void mp_reader_set_error(MsgPackReader *r, const char *msg) {
    if (!r->has_error) {
        snprintf(r->error, sizeof(r->error), "%s at offset %zu", msg, r->pos);
        r->has_error = 1;
    }
}

static int mp_reader_ensure(MsgPackReader *r, size_t n) {
    if (r->pos + n > r->len) {
        mp_reader_set_error(r, "unexpected end of data");
        return 0;
    }
    return 1;
}

uint8_t mp_reader_peek(MsgPackReader *r) {
    if (!mp_reader_ensure(r, 1)) return 0;
    return r->data[r->pos];
}

uint8_t mp_reader_read_u8(MsgPackReader *r) {
    if (!mp_reader_ensure(r, 1)) return 0;
    return r->data[r->pos++];
}

uint16_t mp_reader_read_u16(MsgPackReader *r) {
    if (!mp_reader_ensure(r, 2)) return 0;
    uint16_t v = ((uint16_t)r->data[r->pos] << 8) |
                  (uint16_t)r->data[r->pos + 1];
    r->pos += 2;
    return v;
}

uint32_t mp_reader_read_u32(MsgPackReader *r) {
    if (!mp_reader_ensure(r, 4)) return 0;
    uint32_t v = ((uint32_t)r->data[r->pos] << 24) |
                 ((uint32_t)r->data[r->pos + 1] << 16) |
                 ((uint32_t)r->data[r->pos + 2] << 8) |
                  (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return v;
}

uint64_t mp_reader_read_u64(MsgPackReader *r) {
    if (!mp_reader_ensure(r, 8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | r->data[r->pos + i];
    r->pos += 8;
    return v;
}

int8_t mp_reader_read_i8(MsgPackReader *r) {
    return (int8_t)mp_reader_read_u8(r);
}

int16_t mp_reader_read_i16(MsgPackReader *r) {
    return (int16_t)mp_reader_read_u16(r);
}

int32_t mp_reader_read_i32(MsgPackReader *r) {
    return (int32_t)mp_reader_read_u32(r);
}

int64_t mp_reader_read_i64(MsgPackReader *r) {
    return (int64_t)mp_reader_read_u64(r);
}

float mp_reader_read_f32(MsgPackReader *r) {
    union { float f; uint32_t u; } conv;
    conv.u = mp_reader_read_u32(r);
    return conv.f;
}

double mp_reader_read_f64(MsgPackReader *r) {
    union { double d; uint64_t u; } conv;
    conv.u = mp_reader_read_u64(r);
    return conv.d;
}

/* ----------------------------------------------------------------
 * Decoder: unpack msgpack -> XS Values
 * ---------------------------------------------------------------- */

static Value *mp_unpack_str_raw(MsgPackReader *r, uint32_t len) {
    if (!mp_reader_ensure(r, len)) return xs_null();
    char *s = xs_malloc(len + 1);
    memcpy(s, r->data + r->pos, len);
    s[len] = '\0';
    r->pos += len;
    Value *v = xs_str(s);
    free(s);
    return v;
}

static Value *mp_unpack_bin_raw(MsgPackReader *r, uint32_t len) {
    if (!mp_reader_ensure(r, len)) return xs_null();
    Value *arr = xs_array_new();
    for (uint32_t i = 0; i < len; i++)
        array_push(arr->arr, xs_int(r->data[r->pos + i]));
    r->pos += len;
    return arr;
}

static Value *mp_unpack_ext_raw(MsgPackReader *r, uint32_t len) {
    int8_t type = mp_reader_read_i8(r);
    if (r->has_error) return xs_null();

    switch (type) {
    case MP_EXT_REGEX: {
        if (!mp_reader_ensure(r, len)) return xs_null();
        char *pattern = xs_malloc(len + 1);
        memcpy(pattern, r->data + r->pos, len);
        pattern[len] = '\0';
        r->pos += len;
        Value *v = xs_regex(pattern);
        free(pattern);
        return v;
    }
    case MP_EXT_BIGINT: {
        if (!mp_reader_ensure(r, len)) return xs_null();
        char *numstr = xs_malloc(len + 1);
        memcpy(numstr, r->data + r->pos, len);
        numstr[len] = '\0';
        r->pos += len;
        int64_t val = strtoll(numstr, NULL, 10);
        free(numstr);
        return xs_int(val);
    }
    case MP_EXT_RANGE: {
        if (!mp_reader_ensure(r, len)) return xs_null();
        MsgPackReader sub;
        mp_reader_init(&sub, r->data + r->pos, len);
        Value *start_v = mp_unpack_value(&sub);
        Value *end_v = mp_unpack_value(&sub);
        Value *step_v = mp_unpack_value(&sub);
        Value *inc_v = mp_unpack_value(&sub);
        int64_t s = (start_v && start_v->tag == XS_INT) ? start_v->i : 0;
        int64_t e = (end_v && end_v->tag == XS_INT) ? end_v->i : 0;
        int64_t st = (step_v && step_v->tag == XS_INT) ? step_v->i : 1;
        int inc = (inc_v && inc_v->tag == XS_INT) ? (int)inc_v->i : 0;
        r->pos += len;
        Value *range = xs_range_step(s, e, inc, st);
        return range;
    }
    case MP_EXT_TUPLE: {
        if (!mp_reader_ensure(r, len)) return xs_null();
        MsgPackReader sub;
        mp_reader_init(&sub, r->data + r->pos, len);
        /* the inner data is an array; unpack as tuple */
        Value *inner = mp_unpack_value(&sub);
        r->pos += len;
        if (inner && inner->tag == XS_ARRAY) {
            Value *tup = xs_tuple_new();
            for (int i = 0; i < inner->arr->len; i++)
                array_push(tup->arr, inner->arr->items[i]);
            return tup;
        }
        return inner ? inner : xs_null();
    }
    case MP_EXT_DATE: {
        if (!mp_reader_ensure(r, len)) return xs_null();
        char *ds = xs_malloc(len + 1);
        memcpy(ds, r->data + r->pos, len);
        ds[len] = '\0';
        r->pos += len;
        Value *v = xs_str(ds);
        free(ds);
        return v;
    }
    default: {
        /* unknown ext type, return as binary array */
        if (!mp_reader_ensure(r, len)) return xs_null();
        Value *arr = xs_array_new();
        for (uint32_t i = 0; i < len; i++)
            array_push(arr->arr, xs_int(r->data[r->pos + i]));
        r->pos += len;
        return arr;
    }
    }
}

Value *mp_unpack_nil(MsgPackReader *r) {
    (void)r;
    return xs_null();
}

Value *mp_unpack_bool(MsgPackReader *r) {
    (void)r;
    return xs_bool(1);
}

Value *mp_unpack_int(MsgPackReader *r) {
    (void)r;
    return xs_int(0);
}

Value *mp_unpack_float(MsgPackReader *r) {
    (void)r;
    return xs_float(0.0);
}

Value *mp_unpack_str(MsgPackReader *r) {
    (void)r;
    return xs_str("");
}

Value *mp_unpack_bin(MsgPackReader *r) {
    (void)r;
    return xs_array_new();
}

Value *mp_unpack_array(MsgPackReader *r, uint32_t count) {
    Value *arr = xs_array_new();
    for (uint32_t i = 0; i < count; i++) {
        if (r->has_error) break;
        Value *elem = mp_unpack_value(r);
        array_push(arr->arr, elem);
    }
    return arr;
}

Value *mp_unpack_map(MsgPackReader *r, uint32_t count) {
    Value *m = xs_map_new();
    for (uint32_t i = 0; i < count; i++) {
        if (r->has_error) break;
        Value *key = mp_unpack_value(r);
        Value *val = mp_unpack_value(r);
        char *ks = value_str(key);
        if (ks) {
            map_set(m->map, ks, val);
            free(ks);
        }
    }
    return m;
}

Value *mp_unpack_ext(MsgPackReader *r) {
    (void)r;
    return xs_null();
}

/* main unpack dispatch */
Value *mp_unpack_value(MsgPackReader *r) {
    if (r->has_error || r->pos >= r->len)
        return xs_null();

    uint8_t tag = mp_reader_read_u8(r);
    if (r->has_error) return xs_null();

    /* positive fixint: 0xxxxxxx */
    if (tag <= 0x7f)
        return xs_int((int64_t)tag);

    /* fixmap: 1000xxxx */
    if ((tag & 0xf0) == MP_FIXMAP) {
        uint32_t count = tag & 0x0f;
        return mp_unpack_map(r, count);
    }

    /* fixarray: 1001xxxx */
    if ((tag & 0xf0) == MP_FIXARRAY) {
        uint32_t count = tag & 0x0f;
        return mp_unpack_array(r, count);
    }

    /* fixstr: 101xxxxx */
    if ((tag & 0xe0) == MP_FIXSTR) {
        uint32_t len = tag & 0x1f;
        return mp_unpack_str_raw(r, len);
    }

    /* negative fixint: 111xxxxx */
    if (tag >= 0xe0)
        return xs_int((int64_t)(int8_t)tag);

    switch (tag) {
    case MP_NIL:     return xs_null();
    case MP_FALSE:   return xs_bool(0);
    case MP_TRUE:    return xs_bool(1);

    case MP_UINT8:   return xs_int((int64_t)mp_reader_read_u8(r));
    case MP_UINT16:  return xs_int((int64_t)mp_reader_read_u16(r));
    case MP_UINT32:  return xs_int((int64_t)mp_reader_read_u32(r));
    case MP_UINT64: {
        uint64_t v = mp_reader_read_u64(r);
        return xs_int((int64_t)v);
    }
    case MP_INT8:    return xs_int((int64_t)mp_reader_read_i8(r));
    case MP_INT16:   return xs_int((int64_t)mp_reader_read_i16(r));
    case MP_INT32:   return xs_int((int64_t)mp_reader_read_i32(r));
    case MP_INT64:   return xs_int(mp_reader_read_i64(r));

    case MP_FLOAT32: return xs_float((double)mp_reader_read_f32(r));
    case MP_FLOAT64: return xs_float(mp_reader_read_f64(r));

    case MP_STR8: {
        uint32_t len = mp_reader_read_u8(r);
        return mp_unpack_str_raw(r, len);
    }
    case MP_STR16: {
        uint32_t len = mp_reader_read_u16(r);
        return mp_unpack_str_raw(r, len);
    }
    case MP_STR32: {
        uint32_t len = mp_reader_read_u32(r);
        return mp_unpack_str_raw(r, len);
    }
    case MP_BIN8: {
        uint32_t len = mp_reader_read_u8(r);
        return mp_unpack_bin_raw(r, len);
    }
    case MP_BIN16: {
        uint32_t len = mp_reader_read_u16(r);
        return mp_unpack_bin_raw(r, len);
    }
    case MP_BIN32: {
        uint32_t len = mp_reader_read_u32(r);
        return mp_unpack_bin_raw(r, len);
    }
    case MP_ARRAY16: {
        uint32_t count = mp_reader_read_u16(r);
        return mp_unpack_array(r, count);
    }
    case MP_ARRAY32: {
        uint32_t count = mp_reader_read_u32(r);
        return mp_unpack_array(r, count);
    }
    case MP_MAP16: {
        uint32_t count = mp_reader_read_u16(r);
        return mp_unpack_map(r, count);
    }
    case MP_MAP32: {
        uint32_t count = mp_reader_read_u32(r);
        return mp_unpack_map(r, count);
    }
    case MP_FIXEXT1:  return mp_unpack_ext_raw(r, 1);
    case MP_FIXEXT2:  return mp_unpack_ext_raw(r, 2);
    case MP_FIXEXT4:  return mp_unpack_ext_raw(r, 4);
    case MP_FIXEXT8:  return mp_unpack_ext_raw(r, 8);
    case MP_FIXEXT16: return mp_unpack_ext_raw(r, 16);
    case MP_EXT8: {
        uint32_t len = mp_reader_read_u8(r);
        return mp_unpack_ext_raw(r, len);
    }
    case MP_EXT16: {
        uint32_t len = mp_reader_read_u16(r);
        return mp_unpack_ext_raw(r, len);
    }
    case MP_EXT32: {
        uint32_t len = mp_reader_read_u32(r);
        return mp_unpack_ext_raw(r, len);
    }
    default:
        mp_reader_set_error(r, "unknown msgpack tag");
        return xs_null();
    }
}

/* ----------------------------------------------------------------
 * High-level API
 * ---------------------------------------------------------------- */

Value *mp_encode(Value *v) {
    MsgPackBuf buf;
    mp_buf_init(&buf);
    if (mp_pack_value(&buf, v, 0) < 0) {
        mp_buf_free(&buf);
        return xs_null();
    }
    /* return as array of bytes */
    Value *result = xs_array_new();
    for (size_t i = 0; i < buf.len; i++)
        array_push(result->arr, xs_int(buf.data[i]));
    mp_buf_free(&buf);
    return result;
}

Value *mp_decode(Value *bytes) {
    if (!bytes || bytes->tag != XS_ARRAY || !bytes->arr)
        return xs_null();

    size_t len = (size_t)bytes->arr->len;
    uint8_t *data = xs_malloc(len);
    for (size_t i = 0; i < len; i++) {
        Value *b = bytes->arr->items[i];
        data[i] = (b && b->tag == XS_INT) ? (uint8_t)b->i : 0;
    }

    MsgPackReader reader;
    mp_reader_init(&reader, data, len);
    Value *result = mp_unpack_value(&reader);
    free(data);

    if (reader.has_error) return xs_null();
    return result;
}

Value *mp_encode_stream(Value *values) {
    if (!values || values->tag != XS_ARRAY || !values->arr)
        return xs_null();

    MsgPackBuf buf;
    mp_buf_init(&buf);

    for (int i = 0; i < values->arr->len; i++) {
        if (mp_pack_value(&buf, values->arr->items[i], 0) < 0) {
            mp_buf_free(&buf);
            return xs_null();
        }
    }

    Value *result = xs_array_new();
    for (size_t i = 0; i < buf.len; i++)
        array_push(result->arr, xs_int(buf.data[i]));
    mp_buf_free(&buf);
    return result;
}

Value *mp_decode_stream(Value *bytes) {
    if (!bytes || bytes->tag != XS_ARRAY || !bytes->arr)
        return xs_null();

    size_t len = (size_t)bytes->arr->len;
    uint8_t *data = xs_malloc(len);
    for (size_t i = 0; i < len; i++) {
        Value *b = bytes->arr->items[i];
        data[i] = (b && b->tag == XS_INT) ? (uint8_t)b->i : 0;
    }

    MsgPackReader reader;
    mp_reader_init(&reader, data, len);

    Value *results = xs_array_new();
    while (mp_reader_has_data(&reader)) {
        Value *val = mp_unpack_value(&reader);
        if (reader.has_error) break;
        array_push(results->arr, val);
    }

    free(data);
    return results;
}

/* ----------------------------------------------------------------
 * Benchmark helper
 * ---------------------------------------------------------------- */

static int64_t mp_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

Value *mp_benchmark(Value *v, int iterations) {
    if (iterations <= 0) iterations = 1000;

    /* encode benchmark */
    int64_t enc_start = mp_time_ms();
    size_t total_bytes = 0;
    for (int i = 0; i < iterations; i++) {
        MsgPackBuf buf;
        mp_buf_init(&buf);
        mp_pack_value(&buf, v, 0);
        total_bytes = buf.len;
        mp_buf_free(&buf);
    }
    int64_t enc_end = mp_time_ms();

    /* prepare encoded data for decode benchmark */
    MsgPackBuf enc_buf;
    mp_buf_init(&enc_buf);
    mp_pack_value(&enc_buf, v, 0);

    int64_t dec_start = mp_time_ms();
    for (int i = 0; i < iterations; i++) {
        MsgPackReader reader;
        mp_reader_init(&reader, enc_buf.data, enc_buf.len);
        mp_unpack_value(&reader);
    }
    int64_t dec_end = mp_time_ms();
    mp_buf_free(&enc_buf);

    /* return result map */
    Value *result = xs_map_new();
    map_set(result->map, "encode_ms", xs_int(enc_end - enc_start));
    map_set(result->map, "decode_ms", xs_int(dec_end - dec_start));
    map_set(result->map, "bytes", xs_int((int64_t)total_bytes));
    map_set(result->map, "iterations", xs_int(iterations));
    return result;
}

/* ----------------------------------------------------------------
 * Native bindings for the XS module
 * ---------------------------------------------------------------- */

static Value *native_mp_encode(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    return mp_encode(args[0]);
}

static Value *native_mp_decode(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    return mp_decode(args[0]);
}

static Value *native_mp_encode_stream(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    return mp_encode_stream(args[0]);
}

static Value *native_mp_decode_stream(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    return mp_decode_stream(args[0]);
}

static Value *native_mp_benchmark(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    int iters = (argc >= 2 && args[1]->tag == XS_INT) ? (int)args[1]->i : 1000;
    return mp_benchmark(args[0], iters);
}

/* pack to raw bytes, return size info */
static Value *native_mp_size(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_int(0);
    MsgPackBuf buf;
    mp_buf_init(&buf);
    mp_pack_value(&buf, args[0], 0);
    int64_t sz = (int64_t)buf.len;
    mp_buf_free(&buf);
    return xs_int(sz);
}

/* round-trip: encode then decode, useful for testing */
static Value *native_mp_roundtrip(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    Value *encoded = mp_encode(args[0]);
    Value *decoded = mp_decode(encoded);
    return decoded;
}

/* pack/unpack individual types for testing */
static Value *native_mp_pack_int(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || args[0]->tag != XS_INT) return xs_null();
    MsgPackBuf buf;
    mp_buf_init(&buf);
    mp_pack_int(&buf, args[0]->i);
    Value *result = xs_array_new();
    for (size_t i = 0; i < buf.len; i++)
        array_push(result->arr, xs_int(buf.data[i]));
    mp_buf_free(&buf);
    return result;
}

static Value *native_mp_pack_float(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1) return xs_null();
    double val = args[0]->tag == XS_FLOAT ? args[0]->f
               : args[0]->tag == XS_INT ? (double)args[0]->i : 0.0;
    MsgPackBuf buf;
    mp_buf_init(&buf);
    mp_pack_float(&buf, val);
    Value *result = xs_array_new();
    for (size_t i = 0; i < buf.len; i++)
        array_push(result->arr, xs_int(buf.data[i]));
    mp_buf_free(&buf);
    return result;
}

static Value *native_mp_pack_str(Interp *interp, Value **args, int argc) {
    (void)interp;
    if (argc < 1 || args[0]->tag != XS_STR) return xs_null();
    MsgPackBuf buf;
    mp_buf_init(&buf);
    mp_pack_str(&buf, args[0]->s, (uint32_t)strlen(args[0]->s));
    Value *result = xs_array_new();
    for (size_t i = 0; i < buf.len; i++)
        array_push(result->arr, xs_int(buf.data[i]));
    mp_buf_free(&buf);
    return result;
}

Value *make_msgpack_module(void) {
    XSMap *m = map_new();
    map_set(m, "encode", xs_native(native_mp_encode));
    map_set(m, "decode", xs_native(native_mp_decode));
    map_set(m, "encode_stream", xs_native(native_mp_encode_stream));
    map_set(m, "decode_stream", xs_native(native_mp_decode_stream));
    map_set(m, "benchmark", xs_native(native_mp_benchmark));
    map_set(m, "size", xs_native(native_mp_size));
    map_set(m, "roundtrip", xs_native(native_mp_roundtrip));
    map_set(m, "pack_int", xs_native(native_mp_pack_int));
    map_set(m, "pack_float", xs_native(native_mp_pack_float));
    map_set(m, "pack_str", xs_native(native_mp_pack_str));
    return xs_module(m);
}
