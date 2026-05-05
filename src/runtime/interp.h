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
    /* Captured at handle-setup time so a multi-shot resume(...) call
       inside the arm body can re-enter the body's continuation: each
       resume re-evaluates the body with a per-call perform-override,
       returns the body's final value back to the arm, and the outer
       body's tail (post-perform) is unwound via CF_HANDLE_DONE.
       arm_is_multishot is precomputed at frame push: 1 when the arm
       body statically contains 2+ resume calls (so re-eval is the
       only way to honour the second one), 0 for the single-shot
       case where the legacy CF_RESUME path still applies and
       composes correctly with multi-perform bodies. */
    Node             *handle_body;
    Env              *handle_body_env;
    int               arm_is_multishot;
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
    /* Lazy-generator handoff. When lazy_yield_chan is set, every
       NODE_YIELD sends the value on it and then blocks on
       lazy_resume_chan until the consumer calls .next() again. These
       used to live on the Interp but since multiple generator workers
       share the one Interp over the GIL, they had to move to
       thread-local storage (see xs_gen_tls_{get,set}) so one worker's
       channels don't get overwritten by another's. */
    Value  *lazy_yield_chan_unused_;
    Value  *lazy_resume_chan_unused_;

    /* effects */
    EffectFrame *effect_stack;
    Value       *resume_value;
    int          in_handler;
    /* When non-zero, the next perform short-circuits to perform_override
       and clears the flag. Used by NODE_RESUME during multi-shot to
       feed a resume value into a re-evaluated handle body. */
    Value       *perform_override;
    int          perform_override_active;
    /* Set by NODE_RESUME's multi-shot path so the enclosing
       NODE_PERFORM knows to bail out via CF_HANDLE_DONE (taking the
       arm body's final value as the handle expression's value)
       instead of the legacy CF_RETURN short-circuit. Saved/restored
       by NODE_PERFORM so nested perform/handle pairs don't trip on
       each other. */
    int          multi_shot_used;

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

    /* depth of NODE_TRY frames currently on the eval stack. Bumped on
       entry, decremented on exit. xs_runtime_error checks this so it
       can suppress the inline diagnostic render when the throw it
       raises will be caught upstream. */
    int          try_depth;

    /* current program AST (set during interp_run, for pass execution) */
    Node        *current_program;

    /* debug hook: called before each statement in interp_exec.
       Returns 1 to pause (enter DAP command loop), 0 to continue.
       The void* is the opaque DAP state pointer. */
    int         (*debug_hook)(void *dap_state, Interp *interp, Node *stmt);
    void        *debug_hook_data;

    /* Set when interp_run surfaced an unhandled exception at top level.
       Consulted by the CLI to return a non-zero exit code. The value
       is parked here (separately from cf.value, which gets cleared as
       soon as the diagnostic is rendered) so @on_panic can still see
       what went wrong. */
    int          had_unhandled_exception;
    Value       *unhandled_exception_value;
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

/* runtime type-annotation checking; defined in interp_typecheck.c */
int         value_matches_type(Value *v, const char *type_name);
int         value_matches_typeexpr(Value *v, TypeExpr *te);
const char *typeexpr_str(TypeExpr *te);
const char *value_type_str(Value *v);

/* derive(...) trait impls; defined in interp_derive.c */
Value *builtin_debug_to_string(Interp *i, Value **args, int argc);
Value *builtin_clone(Interp *i, Value **args, int argc);
Value *builtin_struct_eq(Interp *i, Value **args, int argc);

#endif
