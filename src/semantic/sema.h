#ifndef SEMA_H
#define SEMA_H

#include "core/ast.h"
#include "diagnostic/diagnostic.h"
#include "semantic/symtable.h"

typedef struct {
    DiagContext *diag;
    int       lenient;
    int       strict;
    int       n_errors;
    SymTab   *st;
    /* Set when the program contains a `use plugin "..."` or `load "..."`
       statement. Plugins can inject arbitrary globals at load time, so
       undefined-name errors (T0002) are downgraded to warnings in that case. */
    int       has_plugin_load;
} SemaCtx;

void sema_init(SemaCtx *ctx, int lenient, int strict);
void sema_free(SemaCtx *ctx);
int  sema_analyze(SemaCtx *ctx, Node *program, const char *filename);

#endif
