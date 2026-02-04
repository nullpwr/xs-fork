/* libFuzzer driver for the XS parser.
 *
 * Build with:
 *   make fuzz-parser
 * Run with:
 *   ./fuzz_parser -max_len=65536 tests/ examples/
 *
 * Each input is treated as an XS source file, tokenized, then parsed. Crashes,
 * infinite loops, or sanitizer reports get reproducers written to the current
 * directory. The fuzzer ignores parser errors by design (we want to crash
 * on invalid inputs, not report "bad syntax").
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "core/lexer.h"
#include "core/parser.h"
#include "core/ast.h"
#include "diagnostic/diagnostic.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 1 << 20) return 0;
    char *buf = (char *)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = 0;

    Lexer lex;
    lexer_init(&lex, buf, "<fuzz>");
    TokenArray ta = lexer_tokenize(&lex);

    Parser p;
    parser_init(&p, &ta, "<fuzz>");
    Node *prog = parser_parse(&p);
    if (prog) node_free(prog);

    token_array_free(&ta);
    /* lexer accumulates a comment list during tokenize; the main xs
       binary owns and releases it via comment_list_free, so the fuzz
       harness has to do the same or libFuzzer's leak check fires on
       every input. */
    comment_list_free(&lex.comments);
    free(buf);
    return 0;
}
