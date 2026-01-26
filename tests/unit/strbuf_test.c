#include "test.h"
#include "core/strbuf.h"
#include <string.h>

TEST(init_is_empty) {
    SB s; sb_init(&s);
    ASSERT_EQ_INT(s.len, 0);
    ASSERT_EQ_INT(s.cap, 0);
    sb_free(&s);
}

TEST(add_and_finish) {
    SB s; sb_init(&s);
    sb_add(&s, "hello ");
    sb_add(&s, "world");
    char *r = sb_finish(&s);
    ASSERT_EQ_STR(r, "hello world");
    sb_free(&s);
}

TEST(addc_appends_one_byte) {
    SB s; sb_init(&s);
    sb_addc(&s, 'a');
    sb_addc(&s, 'b');
    sb_addc(&s, 'c');
    ASSERT_EQ_STR(sb_finish(&s), "abc");
    sb_free(&s);
}

TEST(addn_with_length) {
    SB s; sb_init(&s);
    sb_addn(&s, "abcdef", 3);
    ASSERT_EQ_STR(sb_finish(&s), "abc");
    sb_free(&s);
}

TEST(indent_four_spaces_per_level) {
    SB s; sb_init(&s);
    sb_indent(&s, 2);
    sb_add(&s, "x");
    ASSERT_EQ_STR(sb_finish(&s), "        x");
    sb_free(&s);
}

TEST(printf_formats) {
    SB s; sb_init(&s);
    sb_printf(&s, "%d:%s", 42, "ok");
    ASSERT_EQ_STR(sb_finish(&s), "42:ok");
    sb_free(&s);
}

TEST(grows_with_large_input) {
    SB s; sb_init(&s);
    for (int i = 0; i < 1000; i++) sb_addc(&s, 'x');
    ASSERT_EQ_INT(s.len, 1000);
    sb_free(&s);
}

TEST(free_resets_state) {
    SB s; sb_init(&s);
    sb_add(&s, "abc");
    sb_free(&s);
    ASSERT_EQ_INT(s.len, 0);
    ASSERT_EQ_INT(s.cap, 0);
    ASSERT(s.data == NULL);
}

int main(void) {
    RUN_TEST(init_is_empty);
    RUN_TEST(add_and_finish);
    RUN_TEST(addc_appends_one_byte);
    RUN_TEST(addn_with_length);
    RUN_TEST(indent_four_spaces_per_level);
    RUN_TEST(printf_formats);
    RUN_TEST(grows_with_large_input);
    RUN_TEST(free_resets_state);
    REPORT_AND_EXIT("strbuf");
}
