/* msgpack.h - MessagePack binary serialization for XS values */
#ifndef XS_MSGPACK_H
#define XS_MSGPACK_H

#include "core/xs.h"
#include "core/value.h"

/* MessagePack format types */
#define MP_FIXINT_POS   0x00  /* 0xxxxxxx */
#define MP_FIXMAP       0x80  /* 1000xxxx */
#define MP_FIXARRAY     0x90  /* 1001xxxx */
#define MP_FIXSTR       0xa0  /* 101xxxxx */
#define MP_NIL          0xc0
#define MP_FALSE        0xc2
#define MP_TRUE         0xc3
#define MP_BIN8         0xc4
#define MP_BIN16        0xc5
#define MP_BIN32        0xc6
#define MP_EXT8         0xc7
#define MP_EXT16        0xc8
#define MP_EXT32        0xc9
#define MP_FLOAT32      0xca
#define MP_FLOAT64      0xcb
#define MP_UINT8        0xcc
#define MP_UINT16       0xcd
#define MP_UINT32       0xce
#define MP_UINT64       0xcf
#define MP_INT8         0xd0
#define MP_INT16        0xd1
#define MP_INT32        0xd2
#define MP_INT64        0xd3
#define MP_FIXEXT1      0xd4
#define MP_FIXEXT2      0xd5
#define MP_FIXEXT4      0xd6
#define MP_FIXEXT8      0xd7
#define MP_FIXEXT16     0xd8
#define MP_STR8         0xd9
#define MP_STR16        0xda
#define MP_STR32        0xdb
#define MP_ARRAY16      0xdc
#define MP_ARRAY32      0xdd
#define MP_MAP16        0xde
#define MP_MAP32        0xdf
#define MP_FIXINT_NEG   0xe0  /* 111xxxxx */

/* ext type codes for XS-specific types */
#define MP_EXT_REGEX    1
#define MP_EXT_DATE     2
#define MP_EXT_BIGINT   3
#define MP_EXT_RANGE    4
#define MP_EXT_TUPLE    5

/* encoder buffer */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} MsgPackBuf;

void  mp_buf_init(MsgPackBuf *buf);
void  mp_buf_free(MsgPackBuf *buf);
void  mp_buf_grow(MsgPackBuf *buf, size_t need);
void  mp_buf_write(MsgPackBuf *buf, const void *src, size_t n);
void  mp_buf_write_u8(MsgPackBuf *buf, uint8_t v);
void  mp_buf_write_u16(MsgPackBuf *buf, uint16_t v);
void  mp_buf_write_u32(MsgPackBuf *buf, uint32_t v);
void  mp_buf_write_u64(MsgPackBuf *buf, uint64_t v);
void  mp_buf_write_i8(MsgPackBuf *buf, int8_t v);
void  mp_buf_write_i16(MsgPackBuf *buf, int16_t v);
void  mp_buf_write_i32(MsgPackBuf *buf, int32_t v);
void  mp_buf_write_i64(MsgPackBuf *buf, int64_t v);
void  mp_buf_write_f32(MsgPackBuf *buf, float v);
void  mp_buf_write_f64(MsgPackBuf *buf, double v);

/* encoder functions */
void  mp_pack_nil(MsgPackBuf *buf);
void  mp_pack_bool(MsgPackBuf *buf, int v);
void  mp_pack_int(MsgPackBuf *buf, int64_t v);
void  mp_pack_uint(MsgPackBuf *buf, uint64_t v);
void  mp_pack_float(MsgPackBuf *buf, double v);
void  mp_pack_str(MsgPackBuf *buf, const char *s, uint32_t len);
void  mp_pack_bin(MsgPackBuf *buf, const uint8_t *data, uint32_t len);
void  mp_pack_array_header(MsgPackBuf *buf, uint32_t count);
void  mp_pack_map_header(MsgPackBuf *buf, uint32_t count);
void  mp_pack_ext(MsgPackBuf *buf, int8_t type, const uint8_t *data, uint32_t len);

/* pack XS value recursively */
int   mp_pack_value(MsgPackBuf *buf, Value *v, int depth);

/* decoder state */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    char           error[256];
    int            has_error;
} MsgPackReader;

void    mp_reader_init(MsgPackReader *r, const uint8_t *data, size_t len);
int     mp_reader_has_data(MsgPackReader *r);
uint8_t mp_reader_peek(MsgPackReader *r);
uint8_t mp_reader_read_u8(MsgPackReader *r);
uint16_t mp_reader_read_u16(MsgPackReader *r);
uint32_t mp_reader_read_u32(MsgPackReader *r);
uint64_t mp_reader_read_u64(MsgPackReader *r);
int8_t  mp_reader_read_i8(MsgPackReader *r);
int16_t mp_reader_read_i16(MsgPackReader *r);
int32_t mp_reader_read_i32(MsgPackReader *r);
int64_t mp_reader_read_i64(MsgPackReader *r);
float   mp_reader_read_f32(MsgPackReader *r);
double  mp_reader_read_f64(MsgPackReader *r);

/* decoder functions */
Value  *mp_unpack_value(MsgPackReader *r);
Value  *mp_unpack_nil(MsgPackReader *r);
Value  *mp_unpack_bool(MsgPackReader *r);
Value  *mp_unpack_int(MsgPackReader *r);
Value  *mp_unpack_float(MsgPackReader *r);
Value  *mp_unpack_str(MsgPackReader *r);
Value  *mp_unpack_bin(MsgPackReader *r);
Value  *mp_unpack_array(MsgPackReader *r, uint32_t count);
Value  *mp_unpack_map(MsgPackReader *r, uint32_t count);
Value  *mp_unpack_ext(MsgPackReader *r);

/* high-level API: XS module */
Value  *mp_encode(Value *v);
Value  *mp_decode(Value *bytes);
Value  *mp_encode_stream(Value *values);
Value  *mp_decode_stream(Value *bytes);

/* benchmark helper */
Value  *mp_benchmark(Value *v, int iterations);

/* XS module factory */
Value  *make_msgpack_module(void);

#endif /* XS_MSGPACK_H */
