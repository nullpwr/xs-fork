#include "test.h"
#include "core/xs_bigint.h"
#include <string.h>
#include <stdlib.h>

TEST(from_i64_roundtrip) {
    XSBigInt *a = bigint_from_i64(1234567890LL);
    ASSERT(bigint_fits_i64(a));
    ASSERT_EQ_INT(bigint_to_i64(a), 1234567890);
    bigint_free(a);
}

TEST(from_i64_negative) {
    XSBigInt *a = bigint_from_i64(-42);
    ASSERT(bigint_fits_i64(a));
    ASSERT_EQ_INT(bigint_to_i64(a), -42);
    bigint_free(a);
}

TEST(from_str_decimal) {
    XSBigInt *a = bigint_from_str("1000000000000000000", 10);
    ASSERT_NOT_NULL(a);
    ASSERT(bigint_fits_i64(a));
    ASSERT_EQ_INT(bigint_to_i64(a), 1000000000000000000LL);
    bigint_free(a);
}

TEST(add_basic) {
    XSBigInt *a = bigint_from_i64(100);
    XSBigInt *b = bigint_from_i64(23);
    XSBigInt *c = bigint_add(a, b);
    ASSERT_EQ_INT(bigint_to_i64(c), 123);
    bigint_free(a); bigint_free(b); bigint_free(c);
}

TEST(sub_negative_result) {
    XSBigInt *a = bigint_from_i64(10);
    XSBigInt *b = bigint_from_i64(25);
    XSBigInt *c = bigint_sub(a, b);
    ASSERT_EQ_INT(bigint_to_i64(c), -15);
    bigint_free(a); bigint_free(b); bigint_free(c);
}

TEST(mul_basic) {
    XSBigInt *a = bigint_from_i64(7);
    XSBigInt *b = bigint_from_i64(6);
    XSBigInt *c = bigint_mul(a, b);
    ASSERT_EQ_INT(bigint_to_i64(c), 42);
    bigint_free(a); bigint_free(b); bigint_free(c);
}

TEST(large_mul_does_not_fit_i64) {
    XSBigInt *a = bigint_from_str("99999999999999999999", 10);
    XSBigInt *b = bigint_from_str("99999999999999999999", 10);
    XSBigInt *c = bigint_mul(a, b);
    ASSERT(!bigint_fits_i64(c));
    char *s = bigint_to_str(c, 10);
    ASSERT_NOT_NULL(s);
    ASSERT(strlen(s) >= 30);
    free(s);
    bigint_free(a); bigint_free(b); bigint_free(c);
}

TEST(cmp_basic) {
    XSBigInt *a = bigint_from_i64(10);
    XSBigInt *b = bigint_from_i64(20);
    ASSERT_EQ_INT(bigint_cmp(a, b), -1);
    ASSERT_EQ_INT(bigint_cmp(b, a), 1);
    ASSERT_EQ_INT(bigint_cmp(a, a), 0);
    bigint_free(a); bigint_free(b);
}

TEST(is_zero) {
    XSBigInt *z = bigint_from_i64(0);
    XSBigInt *o = bigint_from_i64(1);
    ASSERT(bigint_is_zero(z));
    ASSERT(!bigint_is_zero(o));
    bigint_free(z); bigint_free(o);
}

TEST(neg_and_abs) {
    XSBigInt *a = bigint_from_i64(-50);
    XSBigInt *na = bigint_neg(a);
    ASSERT_EQ_INT(bigint_to_i64(na), 50);
    XSBigInt *aa = bigint_abs(a);
    ASSERT_EQ_INT(bigint_to_i64(aa), 50);
    bigint_free(a); bigint_free(na); bigint_free(aa);
}

TEST(pow_basic) {
    XSBigInt *a = bigint_from_i64(2);
    XSBigInt *r = bigint_pow(a, 10);
    ASSERT_EQ_INT(bigint_to_i64(r), 1024);
    bigint_free(a); bigint_free(r);
}

TEST(to_str_decimal) {
    XSBigInt *a = bigint_from_i64(1234);
    char *s = bigint_to_str(a, 10);
    ASSERT_EQ_STR(s, "1234");
    free(s);
    bigint_free(a);
}

int main(void) {
    RUN_TEST(from_i64_roundtrip);
    RUN_TEST(from_i64_negative);
    RUN_TEST(from_str_decimal);
    RUN_TEST(add_basic);
    RUN_TEST(sub_negative_result);
    RUN_TEST(mul_basic);
    RUN_TEST(large_mul_does_not_fit_i64);
    RUN_TEST(cmp_basic);
    RUN_TEST(is_zero);
    RUN_TEST(neg_and_abs);
    RUN_TEST(pow_basic);
    RUN_TEST(to_str_decimal);
    REPORT_AND_EXIT("bigint");
}
