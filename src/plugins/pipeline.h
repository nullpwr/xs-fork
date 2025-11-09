#ifndef XS_PIPELINE_H
#define XS_PIPELINE_H

#include "core/ast.h"

/* forward declare */
typedef struct Value Value;

typedef struct {
    char *name;
    char *pattern;
    char *plugin_id;
    int   token_kind;
} PluginTokenDef;

typedef enum { PASS_ANALYZE, PASS_ANNOTATE, PASS_TRANSFORM } PassKind;

typedef struct {
    char    *name;
    char    *plugin_id;
    char    *phase_ref;   /* e.g. "parser", "sema", or custom pass name */
    int      is_after;    /* 1=after, 0=before */
    PassKind kind;
    Value  **visitors;
    int     *visitor_tags;
    char   **visitor_names;  /* tag name strings for name-based matching when tag == -1 */
    int      nvisitors;
    Value   *on_scope_exit;
    Value   *state;       /* shared state map for visitors */
    int      order;       /* set by topo sort */
} CustomPass;

typedef enum { SEMA_RULE_NEW, SEMA_RULE_OVERRIDE, SEMA_RULE_EXCLUSIVE } SemaRuleKind;

typedef struct {
    char        *target;
    char        *plugin_id;
    SemaRuleKind kind;
    int          priority;
    Value       *callback;
} SemaPluginRule;

typedef enum { RT_HOOK_EXEC, RT_HOOK_BEFORE, RT_HOOK_AFTER } RuntimeHookKind;

typedef struct {
    char           *target;
    char           *plugin_id;
    RuntimeHookKind kind;
    int             priority;
    Value          *callback;
} RuntimePluginHook;

typedef struct {
    int   node_id;
    char *hook_kind;
    char *target;
    Span  span;
} HookTableEntry;

typedef struct {
    char  *id;
    char  *version;
    int    priority;
    char **provides;    int nprovides;
    char **modifies;    int nmodifies;
    char **conflicts_with; int nconflicts;
    char **depends_on;  int ndepends;
} PluginMeta;

typedef struct {
    PluginTokenDef    *tokens;       int ntokens,    tokens_cap;
    CustomPass        *passes;       int npasses,    passes_cap;
    CustomPass       **sorted_passes; int nsorted;
    SemaPluginRule    *sema_rules;   int nsema_rules, sema_rules_cap;
    RuntimePluginHook *runtime_hooks; int nruntime_hooks, runtime_hooks_cap;
    HookTableEntry    *hook_table;   int nhook_entries, hook_table_cap;
    PluginMeta        *metas;        int nmetas, metas_cap;
    char **pipeline_from;
    char **pipeline_to;
    int    npipeline_constraints;
    int    pipeline_constraints_cap;
} PluginPipeline;

PluginPipeline *pipeline_new(void);
void            pipeline_free(PluginPipeline *p);

int  pipeline_register_token(PluginPipeline *p, const char *name, const char *pattern, const char *plugin_id);
int  pipeline_register_pass(PluginPipeline *p, CustomPass *pass);
int  pipeline_register_sema_rule(PluginPipeline *p, SemaPluginRule *rule);
int  pipeline_register_runtime_hook(PluginPipeline *p, RuntimePluginHook *hook);
int  pipeline_register_meta(PluginPipeline *p, PluginMeta *meta);
void pipeline_add_constraint(PluginPipeline *p, const char *from, const char *to);

void pipeline_emit_hook(PluginPipeline *p, int node_id, const char *kind, const char *target, Span span);
HookTableEntry *pipeline_lookup_hooks(PluginPipeline *p, int node_id, int *count);

int  pipeline_validate(PluginPipeline *p);

/* AST pass execution: walks the AST, calling visitor callbacks for matching node types.
   phase_ref: "parser" or "sema"; is_after: 1 for after, 0 for before.
   interp is opaque (Interp*) used for calling callbacks.
   Returns 0 on success. */
int  pipeline_run_passes(PluginPipeline *p, Node *program, const char *phase_ref, int is_after, void *interp);

/* Sema dispatch: runs sema rules as a post-sema pass over the AST.
   Returns 0 on success. */
int  pipeline_dispatch_sema(PluginPipeline *p, Node *program, void *interp);

#endif
