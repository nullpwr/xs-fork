#include "test.h"
#include "core/regex.h"
#include <string.h>

TEST(literal_match) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "hello", 0), 0);
    XSMatch m;
    int r = xs_regex_search(&re, "say hello world", 15, 0, &m);
    ASSERT_EQ_INT(r, 1);
    ASSERT_EQ_INT(m.start, 4);
    ASSERT_EQ_INT(m.end, 9);
    xs_regex_free(&re);
}

TEST(no_match) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "zzz", 0), 0);
    XSMatch m;
    int r = xs_regex_search(&re, "abcdef", 6, 0, &m);
    ASSERT_EQ_INT(r, 0);
    xs_regex_free(&re);
}

TEST(full_match) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "[0-9]+", 0), 0);
    ASSERT(xs_regex_full_match(&re, "12345", 5));
    ASSERT(!xs_regex_full_match(&re, "12a45", 5));
    xs_regex_free(&re);
}

TEST(quantifier_star) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "ab*c", 0), 0);
    XSMatch m;
    ASSERT(xs_regex_search(&re, "ac", 2, 0, &m));
    ASSERT(xs_regex_search(&re, "abc", 3, 0, &m));
    ASSERT(xs_regex_search(&re, "abbbbc", 6, 0, &m));
    ASSERT(!xs_regex_search(&re, "abd", 3, 0, &m));
    xs_regex_free(&re);
}

TEST(alternation) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "cat|dog", 0), 0);
    XSMatch m;
    ASSERT(xs_regex_search(&re, "i have a cat", 12, 0, &m));
    ASSERT(xs_regex_search(&re, "my dog", 6, 0, &m));
    ASSERT(!xs_regex_search(&re, "a bird", 6, 0, &m));
    xs_regex_free(&re);
}

TEST(char_class) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "[aeiou]", 0), 0);
    XSMatch m;
    ASSERT(xs_regex_search(&re, "xyz e", 5, 0, &m));
    ASSERT_EQ_INT(m.start, 4);
    xs_regex_free(&re);
}

TEST(anchors) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "^abc", 0), 0);
    XSMatch m;
    ASSERT(xs_regex_search(&re, "abcdef", 6, 0, &m));
    ASSERT(!xs_regex_search(&re, "xabcdef", 7, 0, &m));
    xs_regex_free(&re);
}

TEST(captures) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "([a-z]+)-([0-9]+)", 0), 0);
    XSMatch m;
    int r = xs_regex_search(&re, "item-42", 7, 0, &m);
    ASSERT_EQ_INT(r, 1);
    ASSERT(m.ngroups >= 2);
    ASSERT_EQ_INT(m.group_starts[1], 0);
    ASSERT_EQ_INT(m.group_ends[1], 4);
    ASSERT_EQ_INT(m.group_starts[2], 5);
    ASSERT_EQ_INT(m.group_ends[2], 7);
    xs_regex_free(&re);
}

TEST(icase_flag) {
    XSRegex re;
    ASSERT_EQ_INT(xs_regex_compile(&re, "hello", RE_FLAG_ICASE), 0);
    XSMatch m;
    ASSERT(xs_regex_search(&re, "HELLO", 5, 0, &m));
    ASSERT(xs_regex_search(&re, "HeLLo", 5, 0, &m));
    xs_regex_free(&re);
}

int main(void) {
    RUN_TEST(literal_match);
    RUN_TEST(no_match);
    RUN_TEST(full_match);
    RUN_TEST(quantifier_star);
    RUN_TEST(alternation);
    RUN_TEST(char_class);
    RUN_TEST(anchors);
    RUN_TEST(captures);
    RUN_TEST(icase_flag);
    REPORT_AND_EXIT("regex");
}
