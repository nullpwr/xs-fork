#include "plugins/pipeline.h"
#include "plugins/conflict.h"
#include <stdlib.h>
#include <string.h>

#define GROW(arr, len, cap, type) do { \
    if ((len) >= (cap)) { \
        (cap) = (cap) ? (cap) * 2 : 8; \
        (arr) = realloc((arr), (cap) * sizeof(type)); \
    } \
} while (0)

PluginPipeline *pipeline_new(void) {
    PluginPipeline *p = calloc(1, sizeof(PluginPipeline));
    return p;
}

void pipeline_free(PluginPipeline *p) {
    if (!p) return;

    for (int i = 0; i < p->ntokens; i++) {
        free(p->tokens[i].name);
        free(p->tokens[i].pattern);
        free(p->tokens[i].plugin_id);
    }
    free(p->tokens);

    for (int i = 0; i < p->npasses; i++) {
        free(p->passes[i].name);
        free(p->passes[i].plugin_id);
        free(p->passes[i].phase_ref);
        if (p->passes[i].visitors) { free(p->passes[i].visitors); p->passes[i].visitors = NULL; }
        if (p->passes[i].visitor_tags) { free(p->passes[i].visitor_tags); p->passes[i].visitor_tags = NULL; }
        if (p->passes[i].visitor_names) {
            for (int j = 0; j < p->passes[i].nvisitors; j++)
                if (p->passes[i].visitor_names[j]) free(p->passes[i].visitor_names[j]);
            free(p->passes[i].visitor_names);
            p->passes[i].visitor_names = NULL;
        }
    }
    free(p->passes); p->passes = NULL;
    free(p->sorted_passes);

    for (int i = 0; i < p->nsema_rules; i++) {
        free(p->sema_rules[i].target);
        free(p->sema_rules[i].plugin_id);
    }
    free(p->sema_rules);

    for (int i = 0; i < p->nruntime_hooks; i++) {
        free(p->runtime_hooks[i].target);
        free(p->runtime_hooks[i].plugin_id);
    }
    free(p->runtime_hooks);

    for (int i = 0; i < p->nhook_entries; i++) {
        free(p->hook_table[i].hook_kind);
        free(p->hook_table[i].target);
    }
    free(p->hook_table);

    for (int i = 0; i < p->nmetas; i++) {
        free(p->metas[i].id);
        free(p->metas[i].version);
        for (int j = 0; j < p->metas[i].nprovides; j++) free(p->metas[i].provides[j]);
        free(p->metas[i].provides);
        for (int j = 0; j < p->metas[i].nmodifies; j++) free(p->metas[i].modifies[j]);
        free(p->metas[i].modifies);
        for (int j = 0; j < p->metas[i].nconflicts; j++) free(p->metas[i].conflicts_with[j]);
        free(p->metas[i].conflicts_with);
        for (int j = 0; j < p->metas[i].ndepends; j++) free(p->metas[i].depends_on[j]);
        free(p->metas[i].depends_on);
    }
    free(p->metas);

    for (int i = 0; i < p->npipeline_constraints; i++) {
        free(p->pipeline_from[i]);
        free(p->pipeline_to[i]);
    }
    free(p->pipeline_from);
    free(p->pipeline_to);

    free(p);
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

int pipeline_register_token(PluginPipeline *p, const char *name, const char *pattern, const char *plugin_id) {
    GROW(p->tokens, p->ntokens, p->tokens_cap, PluginTokenDef);
    PluginTokenDef *t = &p->tokens[p->ntokens++];
    t->name      = dup_str(name);
    t->pattern   = dup_str(pattern);
    t->plugin_id = dup_str(plugin_id);
    t->token_kind = 0;
    return 0;
}

int pipeline_register_pass(PluginPipeline *p, CustomPass *pass) {
    GROW(p->passes, p->npasses, p->passes_cap, CustomPass);
    CustomPass *dst = &p->passes[p->npasses++];
    dst->name          = dup_str(pass->name);
    dst->plugin_id     = dup_str(pass->plugin_id);
    dst->phase_ref     = dup_str(pass->phase_ref);
    dst->is_after      = pass->is_after;
    dst->kind          = pass->kind;
    dst->visitors      = pass->visitors;
    dst->visitor_tags  = pass->visitor_tags;
    dst->visitor_names = pass->visitor_names;
    dst->nvisitors     = pass->nvisitors;
    dst->on_scope_exit = pass->on_scope_exit;
    dst->state         = pass->state;
    dst->order         = pass->order;
    return 0;
}

int pipeline_register_sema_rule(PluginPipeline *p, SemaPluginRule *rule) {
    GROW(p->sema_rules, p->nsema_rules, p->sema_rules_cap, SemaPluginRule);
    SemaPluginRule *dst = &p->sema_rules[p->nsema_rules++];
    dst->target    = dup_str(rule->target);
    dst->plugin_id = dup_str(rule->plugin_id);
    dst->kind      = rule->kind;
    dst->priority  = rule->priority;
    dst->callback  = rule->callback;
    return 0;
}

int pipeline_register_runtime_hook(PluginPipeline *p, RuntimePluginHook *hook) {
    GROW(p->runtime_hooks, p->nruntime_hooks, p->runtime_hooks_cap, RuntimePluginHook);
    RuntimePluginHook *dst = &p->runtime_hooks[p->nruntime_hooks++];
    dst->target    = dup_str(hook->target);
    dst->plugin_id = dup_str(hook->plugin_id);
    dst->kind      = hook->kind;
    dst->priority  = hook->priority;
    dst->callback  = hook->callback;
    return 0;
}

int pipeline_register_meta(PluginPipeline *p, PluginMeta *meta) {
    GROW(p->metas, p->nmetas, p->metas_cap, PluginMeta);
    PluginMeta *dst = &p->metas[p->nmetas++];
    dst->id       = dup_str(meta->id);
    dst->version  = dup_str(meta->version);
    dst->priority = meta->priority;

    dst->nprovides = meta->nprovides;
    dst->provides  = malloc(meta->nprovides * sizeof(char *));
    for (int i = 0; i < meta->nprovides; i++) dst->provides[i] = dup_str(meta->provides[i]);

    dst->nmodifies = meta->nmodifies;
    dst->modifies  = malloc(meta->nmodifies * sizeof(char *));
    for (int i = 0; i < meta->nmodifies; i++) dst->modifies[i] = dup_str(meta->modifies[i]);

    dst->nconflicts = meta->nconflicts;
    dst->conflicts_with = malloc(meta->nconflicts * sizeof(char *));
    for (int i = 0; i < meta->nconflicts; i++) dst->conflicts_with[i] = dup_str(meta->conflicts_with[i]);

    dst->ndepends = meta->ndepends;
    dst->depends_on = malloc(meta->ndepends * sizeof(char *));
    for (int i = 0; i < meta->ndepends; i++) dst->depends_on[i] = dup_str(meta->depends_on[i]);

    return 0;
}

void pipeline_add_constraint(PluginPipeline *p, const char *from, const char *to) {
    if (p->npipeline_constraints >= p->pipeline_constraints_cap) {
        p->pipeline_constraints_cap = p->pipeline_constraints_cap ? p->pipeline_constraints_cap * 2 : 8;
        p->pipeline_from = realloc(p->pipeline_from, p->pipeline_constraints_cap * sizeof(char *));
        p->pipeline_to   = realloc(p->pipeline_to,   p->pipeline_constraints_cap * sizeof(char *));
    }
    p->pipeline_from[p->npipeline_constraints] = dup_str(from);
    p->pipeline_to[p->npipeline_constraints]   = dup_str(to);
    p->npipeline_constraints++;
}

void pipeline_emit_hook(PluginPipeline *p, int node_id, const char *kind, const char *target, Span span) {
    GROW(p->hook_table, p->nhook_entries, p->hook_table_cap, HookTableEntry);
    HookTableEntry *e = &p->hook_table[p->nhook_entries++];
    e->node_id   = node_id;
    e->hook_kind = dup_str(kind);
    e->target    = dup_str(target);
    e->span      = span;
}

HookTableEntry *pipeline_lookup_hooks(PluginPipeline *p, int node_id, int *count) {
    /* count matching entries first */
    int n = 0;
    for (int i = 0; i < p->nhook_entries; i++) {
        if (p->hook_table[i].node_id == node_id) n++;
    }
    *count = n;
    if (n == 0) return NULL;

    HookTableEntry *results = malloc(n * sizeof(HookTableEntry));
    int idx = 0;
    for (int i = 0; i < p->nhook_entries; i++) {
        if (p->hook_table[i].node_id == node_id) {
            results[idx++] = p->hook_table[i];
        }
    }
    return results;
}

int pipeline_validate(PluginPipeline *p) {
    int err = 0;
    if (conflict_check_metas(p) < 0) err = -1;
    if (conflict_check_tokens(p) < 0) err = -1;
    if (conflict_check_node_names(p) < 0) err = -1;
    if (conflict_check_sema_exclusive(p) < 0) err = -1;
    if (conflict_check_pass_cycles(p) < 0) err = -1;
    if (conflict_warn_transform_visibility(p) < 0) err = -1;
    return err;
}
