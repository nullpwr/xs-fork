#include "test.h"
#include "core/lexer.h"
#include "core/xs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TokenArray tokenize(const char *src) {
    Lexer l;
    lexer_init(&l, src, "<test>");
    return lexer_tokenize(&l);
}

TEST(empty) {
    TokenArray ta = tokenize("");
    /* always one EOF token */
    ASSERT(ta.len >= 1);
    ASSERT_EQ_INT(ta.items[ta.len - 1].kind, TK_EOF);
    token_array_free(&ta);
}

TEST(single_int) {
    TokenArray ta = tokenize("42");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_INT);
    ASSERT_EQ_INT(ta.items[0].ival, 42);
    token_array_free(&ta);
}

TEST(single_ident) {
    TokenArray ta = tokenize("foo");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_IDENT);
    ASSERT_EQ_STR(ta.items[0].sval, "foo");
    token_array_free(&ta);
}

TEST(string_escape) {
    TokenArray ta = tokenize("\"a\\nb\"");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_STRING);
    ASSERT_EQ_STR(ta.items[0].sval, "a\nb");
    token_array_free(&ta);
}

TEST(keyword_fn) {
    TokenArray ta = tokenize("fn");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_FN);
    token_array_free(&ta);
}

TEST(numbers_with_underscores) {
    TokenArray ta = tokenize("1_000_000");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_INT);
    ASSERT_EQ_INT(ta.items[0].ival, 1000000);
    token_array_free(&ta);
}

TEST(float_literal) {
    TokenArray ta = tokenize("3.14");
    ASSERT(ta.len >= 2);
    ASSERT_EQ_INT(ta.items[0].kind, TK_FLOAT);
    token_array_free(&ta);
}

TEST(comment_is_stripped) {
    TokenArray ta = tokenize("1 -- tail comment\n2");
    int ints = 0;
    for (int i = 0; i < ta.len; i++)
        if (ta.items[i].kind == TK_INT) ints++;
    ASSERT_EQ_INT(ints, 2);
    token_array_free(&ta);
}

int main(void) {
    RUN_TEST(empty);
    RUN_TEST(single_int);
    RUN_TEST(single_ident);
    RUN_TEST(string_escape);
    RUN_TEST(keyword_fn);
    RUN_TEST(numbers_with_underscores);
    RUN_TEST(float_literal);
    RUN_TEST(comment_is_stripped);
    REPORT_AND_EXIT("lexer");
}
