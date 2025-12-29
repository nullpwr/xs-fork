/* Stubs for globals/functions the unit-test link pulls in transitively
   but does not actually exercise. Keeps the test binary tiny and
   independent of the interpreter, VM, and plugin stacks. */
#include "core/parser.h"
#include "core/value.h"

int g_no_color = 1;

/* From src/vm/bytecode.c - tests don't run bytecode so a no-op is fine. */
void proto_free(void *p) { (void)p; }

/* From src/runtime/interp.c - reactive bindings never fire in unit tests. */
Value *interp_eval(void *interp, Node *expr) {
    (void)interp; (void)expr;
    return NULL;
}

/* From src/plugins/plugin_parse.c - plugin decls never appear in unit-test
   source fixtures. Returning NULL is a parse error, which is fine. */
Node *parse_plugin_decl(Parser *p) {
    (void)p;
    return NULL;
}
