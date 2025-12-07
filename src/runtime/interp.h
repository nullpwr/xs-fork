// interp.h -- tree-walking interpreter
#ifndef INTERP_H
#define INTERP_H

#include "core/xs.h"
#include "core/value.h"
#include "core/env.h"
#include "core/ast.h"
#include "coverage/coverage.h"
#include "diagnostic/diagnostic.h"
#include "runtime/scheduler.h"

typedef struct {
    const char *func_name;
    Span call_span;
} InterpFrame;

/* control flow */
typedef struct {
    int    signal;
    Value *value;
    char  *label;
} CFResult;

typedef struct EffectFrame {
    char             *effect_name;
    char             *op_name;
    ParamList         params;
    Node             *handler_body;
    Env              *handler_env;
    struct EffectFrame *prev;
} EffectFrame;

#define CF_RESULT_OK    { 0, NULL }
#define CF_CLEAR(i)  do { (i)->cf.signal=0; value_decref((i)->cf.value); (i)->cf.value=NULL; free((i)->cf.label); (i)->cf.label=NULL; } while(0)

typedef struct Interp Interp;
struct Interp {
    Env    *globals;
    Env    *env;
    CFResult cf;
    int      max_depth;
    int      depth;
    const char *filename;

    struct { Node **items; int len, cap; } defers;
    Value  *yield_collect;
    int     yield_limit;     /* max yields before stopping, 0 = unlimited */

    /* effects */
    EffectFrame *effect_stack;
    Value       *resume_value;
    int          in_handler;

    Value       *nursery_queue;

    /* cooperative task scheduler */
    Scheduler   *scheduler;

    /* legacy task queue (kept for compat) */
    struct { Value *fn; Value *result; int done; } task_queue[64];
    int          n_tasks;

    Span         current_span;
    XSCoverage  *coverage;

    /* tail call trampoline */
    Value       *tc_callee;
    Value      **tc_args;
    int          tc_argc;

    InterpFrame *call_stack;
    int          call_stack_len;
    int          call_stack_cap;
    DiagContext  *diag;

    /* tracer */
    void        *tracer;  /* XSTracer*, or NULL if not recording */

    /* phase 2: source kept alive for plugin re-parse */
    const char  *source;
    int          needs_reparse;

    /* plugin pipeline (for plugin block declarations) */
    void        *pipeline;  /* PluginPipeline*, or NULL */

    /* runtime hook cancel flag */
    int          hook_cancelled;

    /* current program AST (set during interp_run, for pass execution) */
    Node        *current_program;

    /* debug hook: called before each statement in interp_exec.
       Returns 1 to pause (enter DAP command loop), 0 to continue.
       The void* is the opaque DAP state pointer. */
    int         (*debug_hook)(void *dap_state, Interp *interp, Node *stmt);
    void        *debug_hook_data;

    /* Set when interp_run surfaced an unhandled exception at top level.
       Consulted by the CLI to return a non-zero exit code. */
    int          had_unhandled_exception;
};

Interp *interp_new(const char *filename);
void    interp_free(Interp *i);
void    interp_run(Interp *i, Node *program);
void    interp_exec(Interp *i, Node *stmt);
void    interp_setup_tracer_suppress(Interp *i);

/* caller does NOT own the refcount; incref to keep */
Value  *interp_eval(Interp *i, Node *expr);

Value  *call_value(Interp *i, Value *callee, Value **args, int argc, const char *label);
void    interp_define_native(Interp *i, const char *name, NativeFn fn);
void    stdlib_register(Interp *i);

#endif
