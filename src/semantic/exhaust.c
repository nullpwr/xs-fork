#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/xs.h"
#include "semantic/exhaust.h"

static int is_catchall(Node *pat) {
    if (!pat) return 0;
    switch (VAL_TAG(pat)) {
        case NODE_PAT_WILD:    return 1;
        case NODE_PAT_IDENT:   return 1;
        case NODE_PAT_CAPTURE: return is_catchall(pat->pat_capture.pattern);
        case NODE_PAT_OR:
            return is_catchall(pat->pat_or.left) ||
                   is_catchall(pat->pat_or.right);
        default:               return 0;
    }
}

/* Does pattern (possibly an or-pattern tree) match the enum variant
   identified by `variant`? The variant string is the bare variant name,
   e.g. "A". Patterns store paths like "E::A" or just "A". */
static int pat_matches_variant(Node *pat, const char *variant) {
    if (!pat || !variant) return 0;
    switch (VAL_TAG(pat)) {
        case NODE_PAT_OR:
            return pat_matches_variant(pat->pat_or.left, variant) ||
                   pat_matches_variant(pat->pat_or.right, variant);
        case NODE_PAT_CAPTURE:
            return pat_matches_variant(pat->pat_capture.pattern, variant);
        case NODE_PAT_ENUM: {
            const char *path = pat->pat_enum.path;
            if (!path) return 0;
            if (strcmp(path, variant) == 0) return 1;
            size_t plen = strlen(path);
            size_t vlen = strlen(variant);
            if (plen > vlen + 2 &&
                path[plen - vlen - 2] == ':' &&
                path[plen - vlen - 1] == ':' &&
                strcmp(path + plen - vlen, variant) == 0)
                return 1;
            return 0;
        }
        case NODE_PAT_STRUCT: {
            const char *path = pat->pat_struct.path;
            if (!path) return 0;
            if (strcmp(path, variant) == 0) return 1;
            size_t plen = strlen(path);
            size_t vlen = strlen(variant);
            if (plen > vlen + 2 &&
                path[plen - vlen - 2] == ':' &&
                path[plen - vlen - 1] == ':' &&
                strcmp(path + plen - vlen, variant) == 0)
                return 1;
            return 0;
        }
        default: return 0;
    }
}

/* Returns NULL if exhaustive, or a malloc'd description of the missing case. */
char *exhaust_check(MatchArm *arms, int n_arms,
                    const char **variants, int n_variants) {
    if (!arms || n_arms == 0) return xs_strdup("(no arms)");

    for (int i = 0; i < n_arms; i++) {
        if (is_catchall(arms[i].pattern)) return NULL;
    }

    /* bool coverage */
    {
        int all_bool = 1, has_true = 0, has_false = 0;
        for (int i = 0; i < n_arms; i++) {
            Node *p = arms[i].pattern;
            if (p && VAL_TAG(p) == NODE_PAT_LIT && p->pat_lit.tag == 3) {
                if (p->pat_lit.bval) has_true  = 1;
                else                 has_false = 1;
            } else {
                all_bool = 0;
            }
        }
        if (all_bool) {
            if (!has_true)  return xs_strdup("true");
            if (!has_false) return xs_strdup("false");
            return NULL;
        }
    }

    // --- enum variant coverage
    if (n_variants > 0) {
        for (int v = 0; v < n_variants; v++) {
            int found = 0;
            for (int i = 0; i < n_arms && !found; i++) {
                if (arms[i].guard) continue;  /* guards may fail at runtime */
                if (pat_matches_variant(arms[i].pattern, variants[v]))
                    found = 1;
            }
            if (!found) {
                size_t len = strlen(variants[v]) + 4;
                char *msg = (char *)xs_malloc(len);
                snprintf(msg, len, "%s(_)", variants[v]);
                return msg;
            }
        }
        return NULL;
    }

    /* conservative: no variant info */
    {
        int has_wildcard = 0;
        for (int i = 0; i < n_arms; i++) {
            if (is_catchall(arms[i].pattern)) { has_wildcard = 1; break; }
        }
        if (!has_wildcard) {
            return xs_strdup("_ (exhaustiveness cannot be verified: "
                             "consider adding a wildcard pattern)");
        }
    }
    return NULL;
}
