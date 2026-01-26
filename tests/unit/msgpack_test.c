#include "test.h"
#include "core/msgpack.h"
#include "core/value.h"
#include "core/xs.h"

static void setup(void) {
    static int done = 0;
    if (!done) { value_init_singletons(); done = 1; }
}

TEST(pack_int_fixint) {
    setup();
    MsgPackBuf buf; mp_buf_init(&buf);
    mp_pack_int(&buf, 5);
    /* positive fixint encoding: 0x00..0x7f */
    ASSERT_EQ_INT(buf.len, 1);
    ASSERT_EQ_INT(buf.data[0], 5);
    mp_buf_free(&buf);
}

TEST(pack_int_negative_fixint) {
    setup();
    MsgPackBuf buf; mp_buf_init(&buf);
    mp_pack_int(&buf, -1);
    /* negative fixint: 0xe0..0xff */
    ASSERT_EQ_INT(buf.len, 1);
    ASSERT_EQ_INT((unsigned char)buf.data[0], 0xff);
    mp_buf_free(&buf);
}

TEST(pack_nil) {
    setup();
    MsgPackBuf buf; mp_buf_init(&buf);
    mp_pack_nil(&buf);
    ASSERT_EQ_INT(buf.len, 1);
    ASSERT_EQ_INT((unsigned char)buf.data[0], 0xc0);
    mp_buf_free(&buf);
}

TEST(pack_bool) {
    setup();
    MsgPackBuf buf; mp_buf_init(&buf);
    mp_pack_bool(&buf, 1);
    mp_pack_bool(&buf, 0);
    ASSERT_EQ_INT(buf.len, 2);
    ASSERT_EQ_INT((unsigned char)buf.data[0], 0xc3);
    ASSERT_EQ_INT((unsigned char)buf.data[1], 0xc2);
    mp_buf_free(&buf);
}

TEST(roundtrip_int) {
    setup();
    Value *in = xs_int(123456789);
    Value *bytes = mp_encode(in);
    ASSERT_NOT_NULL(bytes);
    Value *out = mp_decode(bytes);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_INT(VAL_INT(out), 123456789);
    value_decref(in); value_decref(bytes); value_decref(out);
}

TEST(roundtrip_string) {
    setup();
    Value *in = xs_str("hello world");
    Value *bytes = mp_encode(in);
    Value *out = mp_decode(bytes);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_STR(out->s, "hello world");
    value_decref(in); value_decref(bytes); value_decref(out);
}

TEST(roundtrip_array) {
    setup();
    Value *in = xs_array_new();
    array_push(in->arr, xs_int(1));
    array_push(in->arr, xs_int(2));
    array_push(in->arr, xs_int(3));
    Value *bytes = mp_encode(in);
    Value *out = mp_decode(bytes);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_INT(out->arr->len, 3);
    ASSERT_EQ_INT(VAL_INT(array_get(out->arr, 1)), 2);
    value_decref(in); value_decref(bytes); value_decref(out);
}

TEST(roundtrip_nested_map) {
    setup();
    Value *in = xs_map_new();
    Value *inner = xs_array_new();
    array_push(inner->arr, xs_int(42));
    map_set(in->map, "data", inner);
    map_set(in->map, "flag", xs_bool(1));
    Value *bytes = mp_encode(in);
    Value *out = mp_decode(bytes);
    ASSERT_NOT_NULL(out);
    ASSERT(map_has(out->map, "data"));
    ASSERT(map_has(out->map, "flag"));
    value_decref(in); value_decref(bytes); value_decref(out);
}

int main(void) {
    RUN_TEST(pack_int_fixint);
    RUN_TEST(pack_int_negative_fixint);
    RUN_TEST(pack_nil);
    RUN_TEST(pack_bool);
    RUN_TEST(roundtrip_int);
    RUN_TEST(roundtrip_string);
    RUN_TEST(roundtrip_array);
    RUN_TEST(roundtrip_nested_map);
    REPORT_AND_EXIT("msgpack");
}
