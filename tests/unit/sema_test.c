#include "test.h"
#include "core/lexer.h"
#include "core/parser.h"
#include "core/ast.h"
#include "semantic/sema.h"
#include "diagnostic/diagnostic.h"
#include <string.h>

/* Run lexer -> parser -> sema on src. Fills *err_codes with a joined
   comma-separated list of diagnostic codes (e.g. "S0040,T0002"). Returns
   the number of error-severity diagnostics. */
static int analyze(const char *src, char *codes_out, int cap) {
    codes_out[0] = 0;

    Lexer l;
    lexer_init(&l, src, "<test>");
    TokenArray ta = lexer_tokenize(&l);

    DiagContext *diag = diag_context_new();
    diag_context_add_source(diag, "<test>", src);

    Parser p;
    parser_init(&p, &ta, "<test>");
    p.diag = diag;
    Node *prog = parser_parse(&p);

    SemaCtx sc;
    sema_init(&sc, 0, 0);
    sc.diag = diag;
    if (prog) sema_analyze(&sc, prog, "<test>");

    int n_errs = 0, used = 0;
    for (int i = 0; i < diag->n_items; i++) {
        Diagnostic *d = &diag->items[i];
        if (d->severity != DIAG_ERROR) continue;
        n_errs++;
        if (!d->code) continue;
        int want = (int)strlen(d->code) + (used ? 1 : 0);
        if (used + want + 1 >= cap) continue;
        if (used) codes_out[used++] = ',';
        strcpy(codes_out + used, d->code);
        used += (int)strlen(d->code);
    }

    sema_free(&sc);
    diag_context_free(diag);
    /* leak parser tokens on purpose */
    return n_errs;
}

TEST(yield_outside_generator_errors) {
    char codes[256];
    int n = analyze("fn f() { yield 1 }", codes, sizeof codes);
    ASSERT(n >= 1);
    ASSERT(strstr(codes, "S0040") != NULL);
}

TEST(yield_inside_generator_ok) {
    char codes[256];
    int n = analyze("fn* g() { yield 1 }", codes, sizeof codes);
    if (n && strstr(codes, "S0040")) FAIL("unexpected S0040 in fn*");
}

TEST(yield_at_top_level_errors) {
    char codes[256];
    int n = analyze("yield 1", codes, sizeof codes);
    ASSERT(n >= 1);
    ASSERT(strstr(codes, "S0040") != NULL);
}

TEST(undefined_ident_errors) {
    char codes[256];
    int n = analyze("let y = undefined_var_name", codes, sizeof codes);
    /* may be downgraded to warning under lenient/plugin-load; but with
       plain analysis an unresolved binding should raise an error. */
    ASSERT(n >= 1);
}

TEST(empty_program_no_errors) {
    char codes[256];
    int n = analyze("", codes, sizeof codes);
    ASSERT_EQ_INT(n, 0);
}

TEST(basic_let_resolves) {
    char codes[256];
    int n = analyze("let x = 1\nlet y = x + 2", codes, sizeof codes);
    ASSERT_EQ_INT(n, 0);
}

int main(void) {
    RUN_TEST(yield_outside_generator_errors);
    RUN_TEST(yield_inside_generator_ok);
    RUN_TEST(yield_at_top_level_errors);
    RUN_TEST(undefined_ident_errors);
    RUN_TEST(empty_program_no_errors);
    RUN_TEST(basic_let_resolves);
    REPORT_AND_EXIT("sema");
}
