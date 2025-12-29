#include "test.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/ast.h"

static Node *parse_source(const char *src) {
    Lexer l;
    lexer_init(&l, src, "<test>");
    TokenArray ta = lexer_tokenize(&l);
    Parser p;
    parser_init(&p, &ta, "<test>");
    Node *n = parser_parse(&p);
    /* leak tokens/comments on purpose; tests don't run long */
    return n;
}

TEST(let_stmt) {
    Node *p = parse_source("let x = 1");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->tag, NODE_PROGRAM);
    ASSERT_EQ_INT(p->program.stmts.len, 1);
    ASSERT_EQ_INT(p->program.stmts.items[0]->tag, NODE_LET);
    ASSERT_EQ_STR(p->program.stmts.items[0]->let.name, "x");
}

TEST(fn_decl) {
    Node *p = parse_source("fn add(a, b) { return a + b }");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->program.stmts.len, 1);
    Node *fn = p->program.stmts.items[0];
    ASSERT_EQ_INT(fn->tag, NODE_FN_DECL);
    ASSERT_EQ_STR(fn->fn_decl.name, "add");
    ASSERT_EQ_INT(fn->fn_decl.params.len, 2);
    ASSERT_EQ_INT(fn->fn_decl.is_generator, 0);
}

TEST(fn_star_is_generator) {
    Node *p = parse_source("fn* g() { yield 1 }");
    ASSERT_NOT_NULL(p);
    Node *fn = p->program.stmts.items[0];
    ASSERT_EQ_INT(fn->tag, NODE_FN_DECL);
    ASSERT_EQ_INT(fn->fn_decl.is_generator, 1);
}

TEST(match_with_slice_pattern) {
    Node *p = parse_source("match x { [] => 1 _ => 2 }");
    ASSERT_NOT_NULL(p);
    Node *expr_stmt = p->program.stmts.items[0];
    ASSERT_EQ_INT(expr_stmt->tag, NODE_EXPR_STMT);
    Node *m = expr_stmt->expr_stmt.expr;
    ASSERT_EQ_INT(m->tag, NODE_MATCH);
    ASSERT_EQ_INT(m->match.arms.len, 2);
    ASSERT_EQ_INT(m->match.arms.items[0].pattern->tag, NODE_PAT_SLICE);
    ASSERT_EQ_INT(m->match.arms.items[1].pattern->tag, NODE_PAT_WILD);
}

TEST(map_pattern) {
    Node *p = parse_source("match x { #{\"k\": v} => v _ => null }");
    ASSERT_NOT_NULL(p);
    Node *m = p->program.stmts.items[0]->expr_stmt.expr;
    ASSERT_EQ_INT(m->tag, NODE_MATCH);
    ASSERT_EQ_INT(m->match.arms.items[0].pattern->tag, NODE_PAT_MAP);
}

TEST(op_precedence) {
    /* a + b * c must parse as a + (b * c) */
    Node *p = parse_source("let r = a + b * c");
    Node *rhs = p->program.stmts.items[0]->let.value;
    ASSERT_EQ_INT(rhs->tag, NODE_BINOP);
    ASSERT_EQ_STR(rhs->binop.op, "+");
    /* right side is itself a * */
    ASSERT_EQ_STR(rhs->binop.right->binop.op, "*");
}

TEST(empty_input_is_valid_program) {
    Node *p = parse_source("");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->tag, NODE_PROGRAM);
    ASSERT_EQ_INT(p->program.stmts.len, 0);
}

int main(void) {
    RUN_TEST(let_stmt);
    RUN_TEST(fn_decl);
    RUN_TEST(fn_star_is_generator);
    RUN_TEST(match_with_slice_pattern);
    RUN_TEST(map_pattern);
    RUN_TEST(op_precedence);
    RUN_TEST(empty_input_is_valid_program);
    REPORT_AND_EXIT("parser");
}
