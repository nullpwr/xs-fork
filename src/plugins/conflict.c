#include "plugins/conflict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* check conflicts_with and depends_on between loaded plugin metas */
int conflict_check_metas(PluginPipeline *p) {
    int err = 0;
    for (int i = 0; i < p->nmetas; i++) {
        PluginMeta *mi = &p->metas[i];

        /* check conflicts_with: if plugin A says it conflicts with B, and B is loaded */
        for (int c = 0; c < mi->nconflicts; c++) {
            for (int j = 0; j < p->nmetas; j++) {
                if (i == j) continue;
                if (strcmp(mi->conflicts_with[c], p->metas[j].id) == 0) {
                    fprintf(stderr, "plugin conflict: '%s' conflicts with '%s'\n",
                            mi->id, p->metas[j].id);
                    err = -1;
                }
            }
        }

        /* check depends_on: every dependency must be loaded */
        for (int d = 0; d < mi->ndepends; d++) {
            int found = 0;
            for (int j = 0; j < p->nmetas; j++) {
                if (strcmp(mi->depends_on[d], p->metas[j].id) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "plugin '%s' depends on '%s' which is not loaded\n",
                        mi->id, mi->depends_on[d]);
                err = -1;
            }
        }
    }
    return err;
}

/* same name + different pattern = error, same name + same pattern = silent merge */
int conflict_check_tokens(PluginPipeline *p) {
    int err = 0;
    for (int i = 0; i < p->ntokens; i++) {
        for (int j = i + 1; j < p->ntokens; j++) {
            if (strcmp(p->tokens[i].name, p->tokens[j].name) == 0) {
                if (strcmp(p->tokens[i].pattern, p->tokens[j].pattern) != 0) {
                    fprintf(stderr, "token conflict: '%s' defined by '%s' (pattern '%s') "
                            "and '%s' (pattern '%s')\n",
                            p->tokens[i].name,
                            p->tokens[i].plugin_id, p->tokens[i].pattern,
                            p->tokens[j].plugin_id, p->tokens[j].pattern);
                    err = -1;
                }
                /* same name + same pattern: silent merge, no error */
            }
        }
    }
    return err;
}

/* check provides lists across metas for duplicate node names */
int conflict_check_node_names(PluginPipeline *p) {
    int err = 0;
    for (int i = 0; i < p->nmetas; i++) {
        for (int pi_ = 0; pi_ < p->metas[i].nprovides; pi_++) {
            const char *name = p->metas[i].provides[pi_];
            for (int j = i + 1; j < p->nmetas; j++) {
                for (int pj = 0; pj < p->metas[j].nprovides; pj++) {
                    if (strcmp(name, p->metas[j].provides[pj]) == 0) {
                        fprintf(stderr, "node name conflict: '%s' provided by both '%s' and '%s'\n",
                                name, p->metas[i].id, p->metas[j].id);
                        err = -1;
                    }
                }
            }
        }
    }
    return err;
}

/* two exclusive overrides on the same target = error */
int conflict_check_sema_exclusive(PluginPipeline *p) {
    int err = 0;
    for (int i = 0; i < p->nsema_rules; i++) {
        if (p->sema_rules[i].kind != SEMA_RULE_EXCLUSIVE) continue;
        for (int j = i + 1; j < p->nsema_rules; j++) {
            if (p->sema_rules[j].kind != SEMA_RULE_EXCLUSIVE) continue;
            if (strcmp(p->sema_rules[i].target, p->sema_rules[j].target) == 0) {
                fprintf(stderr, "sema conflict: exclusive rule on '%s' from both '%s' and '%s'\n",
                        p->sema_rules[i].target,
                        p->sema_rules[i].plugin_id,
                        p->sema_rules[j].plugin_id);
                err = -1;
            }
        }
    }
    return err;
}

/* helper: find pass index by name, -1 if not found */
static int find_pass(PluginPipeline *p, const char *name) {
    for (int i = 0; i < p->npasses; i++) {
        if (strcmp(p->passes[i].name, name) == 0) return i;
    }
    return -1;
}

/* topo sort with Kahn's algorithm, tiebreak by priority then alphabetical */
int conflict_check_pass_cycles(PluginPipeline *p) {
    int n = p->npasses;
    if (n == 0) {
        p->nsorted = 0;
        return 0;
    }

    /* build adjacency from constraints + phase_ref ordering */
    size_t ns = (size_t)n;
    int *indeg = calloc(ns, sizeof(int));
    int *adj   = calloc(ns * ns, sizeof(int)); /* adj[i*n+j] = 1 means i -> j */

    /* phase_ref constraints: if pass says "after parser", it depends on passes before it */
    for (int i = 0; i < n; i++) {
        CustomPass *pi_ = &p->passes[i];
        if (!pi_->phase_ref) continue;
        int target = find_pass(p, pi_->phase_ref);
        if (target < 0) continue;
        if (pi_->is_after) {
            /* i runs after target */
            if (!adj[target * n + i]) {
                adj[target * n + i] = 1;
                indeg[i]++;
            }
        } else {
            /* i runs before target */
            if (!adj[i * n + target]) {
                adj[i * n + target] = 1;
                indeg[target]++;
            }
        }
    }

    /* explicit pipeline constraints */
    for (int c = 0; c < p->npipeline_constraints; c++) {
        int from = find_pass(p, p->pipeline_from[c]);
        int to   = find_pass(p, p->pipeline_to[c]);
        if (from < 0 || to < 0) continue;
        if (!adj[from * n + to]) {
            adj[from * n + to] = 1;
            indeg[to]++;
        }
    }

    /* Kahn's with priority queue (simple: scan for min each time) */
    free(p->sorted_passes);
    p->sorted_passes = calloc(ns, sizeof(CustomPass *));
    p->nsorted = 0;

    int *ready = calloc(ns, sizeof(int));
    int nready = 0;

    for (int i = 0; i < n; i++) {
        if (indeg[i] == 0) ready[nready++] = i;
    }

    while (nready > 0) {
        /* pick best: lowest priority value first, then alphabetical */
        int best = 0;
        for (int r = 1; r < nready; r++) {
            CustomPass *a = &p->passes[ready[best]];
            CustomPass *b = &p->passes[ready[r]];
            if (b->order < a->order ||
                (b->order == a->order && strcmp(b->name, a->name) < 0)) {
                best = r;
            }
        }
        int pick = ready[best];
        ready[best] = ready[--nready];

        p->sorted_passes[p->nsorted++] = &p->passes[pick];

        for (int j = 0; j < n; j++) {
            if (adj[pick * n + j]) {
                indeg[j]--;
                if (indeg[j] == 0) ready[nready++] = j;
            }
        }
    }

    free(ready);
    free(adj);
    free(indeg);

    if (p->nsorted != n) {
        fprintf(stderr, "pass cycle detected: sorted %d of %d passes\n", p->nsorted, n);
        return -1;
    }
    return 0;
}

/* warn if a transform pass rewrites nodes before another pass visits them.
   suppressed if the visiting pass has an explicit depends_on for the transform pass's plugin */
int conflict_warn_transform_visibility(PluginPipeline *p) {
    /* need sorted order first */
    if (p->nsorted != p->npasses) {
        if (conflict_check_pass_cycles(p) < 0) return -1;
    }

    for (int i = 0; i < p->nsorted; i++) {
        CustomPass *ti = p->sorted_passes[i];
        if (ti->kind != PASS_TRANSFORM) continue;

        for (int j = i + 1; j < p->nsorted; j++) {
            CustomPass *vj = p->sorted_passes[j];
            if (vj->kind == PASS_TRANSFORM) continue;

            /* check if vj's plugin explicitly depends on ti's plugin */
            int has_dep = 0;
            for (int m = 0; m < p->nmetas; m++) {
                if (strcmp(p->metas[m].id, vj->plugin_id) != 0) continue;
                for (int d = 0; d < p->metas[m].ndepends; d++) {
                    if (strcmp(p->metas[m].depends_on[d], ti->plugin_id) == 0) {
                        has_dep = 1;
                        break;
                    }
                }
                break;
            }
            if (has_dep) continue;

            /* check overlapping visitor tags */
            for (int vi = 0; vi < ti->nvisitors; vi++) {
                for (int vv = 0; vv < vj->nvisitors; vv++) {
                    if (ti->visitor_tags[vi] == vj->visitor_tags[vv]) {
                        fprintf(stderr, "warning: transform pass '%s' may rewrite nodes "
                                "before '%s' visits them (tag %d)\n",
                                ti->name, vj->name, ti->visitor_tags[vi]);
                    }
                }
            }
        }
    }
    return 0;
}
