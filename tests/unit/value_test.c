#include "test.h"
#include "core/value.h"
#include "core/xs.h"
#include <string.h>

static void ensure_singletons(void) {
    static int done = 0;
    if (!done) { value_init_singletons(); done = 1; }
}

TEST(int_roundtrip) {
    ensure_singletons();
    Value *v = xs_int(42);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ_INT(VAL_INT(v), 42);
    value_decref(v);
}

TEST(int_negative) {
    ensure_singletons();
    Value *v = xs_int(-9999);
    ASSERT_EQ_INT(VAL_INT(v), -9999);
    value_decref(v);
}

TEST(bool_singletons_are_stable) {
    ensure_singletons();
    Value *a = xs_bool(1), *b = xs_bool(1);
    ASSERT(a == b);
    Value *c = xs_bool(0), *d = xs_bool(0);
    ASSERT(c == d);
    ASSERT(a != c);
    value_decref(a); value_decref(b); value_decref(c); value_decref(d);
}

TEST(null_singleton) {
    ensure_singletons();
    Value *x = xs_null(), *y = xs_null();
    ASSERT(x == y);
    value_decref(x); value_decref(y);
}

TEST(string_basic) {
    ensure_singletons();
    Value *s = xs_str("hello");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_STR(s->s, "hello");
    value_decref(s);
}

TEST(string_with_len) {
    ensure_singletons();
    const char buf[] = { 'a', 'b', 0, 'c' };
    Value *s = xs_str_n(buf, 4);
    ASSERT_NOT_NULL(s);
    value_decref(s);
}

TEST(equal_ints) {
    ensure_singletons();
    Value *a = xs_int(5), *b = xs_int(5), *c = xs_int(6);
    ASSERT(value_equal(a, b));
    ASSERT(!value_equal(a, c));
    value_decref(a); value_decref(b); value_decref(c);
}

TEST(equal_strings) {
    ensure_singletons();
    Value *a = xs_str("x"), *b = xs_str("x"), *c = xs_str("y");
    ASSERT(value_equal(a, b));
    ASSERT(!value_equal(a, c));
    value_decref(a); value_decref(b); value_decref(c);
}

TEST(truthy_rules) {
    ensure_singletons();
    Value *zero = xs_int(0);
    Value *one  = xs_int(1);
    Value *empty = xs_str("");
    Value *hi = xs_str("hi");
    Value *n = xs_null();
    Value *t = xs_bool(1);
    Value *f = xs_bool(0);
    ASSERT(!value_truthy(zero));
    ASSERT(value_truthy(one));
    ASSERT(!value_truthy(empty));
    ASSERT(value_truthy(hi));
    ASSERT(!value_truthy(n));
    ASSERT(value_truthy(t));
    ASSERT(!value_truthy(f));
    value_decref(zero); value_decref(one); value_decref(empty);
    value_decref(hi); value_decref(n); value_decref(t); value_decref(f);
}

TEST(array_push_get_len) {
    ensure_singletons();
    Value *a = xs_array_new();
    ASSERT_NOT_NULL(a);
    array_push(a->arr, xs_int(10));
    array_push(a->arr, xs_int(20));
    ASSERT_EQ_INT(a->arr->len, 2);
    Value *e = array_get(a->arr, 1);
    ASSERT_EQ_INT(VAL_INT(e), 20);
    value_decref(a);
}

TEST(map_set_get) {
    ensure_singletons();
    Value *m = xs_map_new();
    map_set(m->map, "k1", xs_int(1));
    map_set(m->map, "k2", xs_int(2));
    ASSERT(map_has(m->map, "k1"));
    ASSERT(!map_has(m->map, "nope"));
    Value *v = map_get(m->map, "k2");
    ASSERT_EQ_INT(VAL_INT(v), 2);
    value_decref(m);
}

TEST(incref_refcount_bumps) {
    ensure_singletons();
    /* Small ints fit in a tagged SMI (no heap, no refcount), so refcount
       checks have to use a heap-boxed value -- strings are a natural fit. */
    Value *v = xs_str("refcount-test");
    int r0 = v->refcount;
    value_incref(v);
    ASSERT_EQ_INT(v->refcount, r0 + 1);
    value_decref(v);
    value_decref(v);
}

int main(void) {
    RUN_TEST(int_roundtrip);
    RUN_TEST(int_negative);
    RUN_TEST(bool_singletons_are_stable);
    RUN_TEST(null_singleton);
    RUN_TEST(string_basic);
    RUN_TEST(string_with_len);
    RUN_TEST(equal_ints);
    RUN_TEST(equal_strings);
    RUN_TEST(truthy_rules);
    RUN_TEST(array_push_get_len);
    RUN_TEST(map_set_get);
    RUN_TEST(incref_refcount_bumps);
    REPORT_AND_EXIT("value");
}
