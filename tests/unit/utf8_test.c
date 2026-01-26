#include "test.h"
#include "core/utf8.h"
#include <string.h>

TEST(decode_ascii) {
    int cp = 0;
    int n = utf8_decode("A", 1, &cp);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(cp, 'A');
}

TEST(decode_two_byte) {
    /* U+00E9 (é) in UTF-8 = C3 A9 */
    const char s[] = { (char)0xC3, (char)0xA9 };
    int cp = 0;
    int n = utf8_decode(s, 2, &cp);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(cp, 0xE9);
}

TEST(decode_three_byte) {
    /* U+20AC (€) = E2 82 AC */
    const char s[] = { (char)0xE2, (char)0x82, (char)0xAC };
    int cp = 0;
    int n = utf8_decode(s, 3, &cp);
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_INT(cp, 0x20AC);
}

TEST(decode_four_byte) {
    /* U+1F600 (😀) = F0 9F 98 80 */
    const char s[] = { (char)0xF0, (char)0x9F, (char)0x98, (char)0x80 };
    int cp = 0;
    int n = utf8_decode(s, 4, &cp);
    ASSERT_EQ_INT(n, 4);
    ASSERT_EQ_INT(cp, 0x1F600);
}

TEST(encode_ascii) {
    char buf[8] = {0};
    int n = utf8_encode('Z', buf);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT((unsigned char)buf[0], 'Z');
}

TEST(encode_decode_roundtrip) {
    char buf[8] = {0};
    int cps[] = { 'A', 0xE9, 0x20AC, 0x1F600 };
    for (int i = 0; i < 4; i++) {
        int n = utf8_encode(cps[i], buf);
        ASSERT(n >= 1 && n <= 4);
        int out = 0;
        int m = utf8_decode(buf, n, &out);
        ASSERT_EQ_INT(m, n);
        ASSERT_EQ_INT(out, cps[i]);
    }
}

TEST(strlen_counts_codepoints) {
    const char *s = "A\xC3\xA9\xE2\x82\xAC"; /* "Aé€" */
    int n = utf8_strlen(s, (int)strlen(s));
    ASSERT_EQ_INT(n, 3);
}

TEST(validate_good_and_bad) {
    ASSERT(utf8_validate("hello", 5));
    ASSERT(utf8_validate("\xC3\xA9", 2));
    /* lone continuation byte is invalid */
    ASSERT(!utf8_validate("\x80", 1));
    /* truncated sequence */
    ASSERT(!utf8_validate("\xC3", 1));
}

TEST(casefold_ascii) {
    ASSERT_EQ_INT(utf8_tolower('A'), 'a');
    ASSERT_EQ_INT(utf8_toupper('a'), 'A');
    ASSERT_EQ_INT(utf8_tolower('a'), 'a');
}

TEST(is_digit_is_letter) {
    ASSERT(utf8_is_digit('0'));
    ASSERT(!utf8_is_digit('A'));
    ASSERT(utf8_is_letter('A'));
    ASSERT(!utf8_is_letter('0'));
}

int main(void) {
    RUN_TEST(decode_ascii);
    RUN_TEST(decode_two_byte);
    RUN_TEST(decode_three_byte);
    RUN_TEST(decode_four_byte);
    RUN_TEST(encode_ascii);
    RUN_TEST(encode_decode_roundtrip);
    RUN_TEST(strlen_counts_codepoints);
    RUN_TEST(validate_good_and_bad);
    RUN_TEST(casefold_ascii);
    RUN_TEST(is_digit_is_letter);
    REPORT_AND_EXIT("utf8");
}
