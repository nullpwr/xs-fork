#include "compiler/match_compiler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static DTNode *dt_new(DTNodeKind kind) {
    DTNode *n = xs_calloc(1, sizeof(DTNode));
    n->kind = kind;
    return n;
}

static DTNode *dt_leaf(int arm_index, Node *body) {
    DTNode *n = dt_new(DT_LEAF);
    n->leaf.arm_index = arm_index;
    n->leaf.body = body;
    n->leaf.bound_names = NULL;
    n->leaf.bound_cols = NULL;
    n->leaf.nbounds = 0;
    return n;
}

static DTNode *dt_fail(void) {
    return dt_new(DT_FAIL);
}

static DTNode *dt_switch(int col) {
    DTNode *n = dt_new(DT_SWITCH);
    n->sw.col = col;
    n->sw.branches = NULL;
    n->sw.nbranches = 0;
    n->sw.cap_branches = 0;
    n->sw.fallback = NULL;
    return n;
}

static DTNode *dt_guard(Node *cond, DTNode *then_dt, DTNode *else_dt) {
    DTNode *n = dt_new(DT_GUARD);
    n->guard.cond = cond;
    n->guard.then_dt = then_dt;
    n->guard.else_dt = else_dt;
    return n;
}

static void dt_add_branch(DTNode *sw, Constructor ctor, DTNode *child) {
    if (sw->sw.nbranches >= sw->sw.cap_branches) {
        sw->sw.cap_branches = sw->sw.cap_branches ? sw->sw.cap_branches * 2 : 8;
        sw->sw.branches = xs_realloc(sw->sw.branches,
            (size_t)sw->sw.cap_branches * sizeof(DTBranch));
    }
    DTBranch *b = &sw->sw.branches[sw->sw.nbranches++];
    b->ctor = ctor;
    b->bindings = NULL;
    b->nbindings = 0;
    b->child = child;
}

void dt_free(DTNode *dt) {
    if (!dt) return;
    switch (dt->kind) {
    case DT_LEAF:
        if (dt->leaf.bound_names) {
            for (int i = 0; i < dt->leaf.nbounds; i++)
                free(dt->leaf.bound_names[i]);
            free(dt->leaf.bound_names);
        }
        free(dt->leaf.bound_cols);
        break;
    case DT_FAIL:
        break;
    case DT_SWITCH:
        for (int i = 0; i < dt->sw.nbranches; i++) {
            DTBranch *b = &dt->sw.branches[i];
            if (b->bindings) {
                for (int j = 0; j < b->nbindings; j++)
                    free(b->bindings[j]);
                free(b->bindings);
            }
            dt_free(b->child);
        }
        free(dt->sw.branches);
        dt_free(dt->sw.fallback);
        break;
    case DT_GUARD:
        dt_free(dt->guard.then_dt);
        dt_free(dt->guard.else_dt);
        break;
    }
    free(dt);
}

static Constructor ctor_int(int64_t v) {
    Constructor c = {0};
    c.kind = CTOR_INT;
    c.ival = v;
    return c;
}

static Constructor ctor_float(double v) {
    Constructor c = {0};
    c.kind = CTOR_FLOAT;
    c.fval = v;
    return c;
}

static Constructor ctor_string(const char *s) {
    Constructor c = {0};
    c.kind = CTOR_STRING;
    c.sval = xs_strdup(s);
    return c;
}

static Constructor ctor_bool(int v) {
    Constructor c = {0};
    c.kind = CTOR_BOOL;
    c.bval = v;
    return c;
}

static Constructor ctor_null(void) {
    Constructor c = {0};
    c.kind = CTOR_NULL;
    return c;
}

static Constructor ctor_enum(const char *path, int arity) {
    Constructor c = {0};
    c.kind = CTOR_ENUM;
    c.enum_path = xs_strdup(path);
    c.arity = arity;
    return c;
}

static Constructor ctor_wild(void) {
    Constructor c = {0};
    c.kind = CTOR_WILD;
    return c;
}

static Constructor ctor_tuple(int arity) {
    Constructor c = {0};
    c.kind = CTOR_TUPLE;
    c.arity = arity;
    return c;
}

static Constructor ctor_range(Node *start, Node *end, int inclusive) {
    Constructor c = {0};
    c.kind = CTOR_RANGE;
    c.range_start = start;
    c.range_end = end;
    c.range_inclusive = inclusive;
    return c;
}

static int ctor_equal(const Constructor *a, const Constructor *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case CTOR_INT:    return a->ival == b->ival;
    case CTOR_FLOAT:  return a->fval == b->fval;
    case CTOR_STRING: return a->sval && b->sval && strcmp(a->sval, b->sval) == 0;
    case CTOR_BOOL:   return a->bval == b->bval;
    case CTOR_NULL:   return 1;
    case CTOR_ENUM:   return a->enum_path && b->enum_path &&
                             strcmp(a->enum_path, b->enum_path) == 0;
    case CTOR_WILD:   return 1;
    case CTOR_TUPLE:  return a->arity == b->arity;
    case CTOR_RANGE:  return 0;
    }
    return 0;
}

static PatMatrix matrix_new(int ncols) {
    PatMatrix m;
    m.rows = NULL;
    m.nrows = 0;
    m.cap_rows = 0;
    m.ncols = ncols;
    return m;
}

static void matrix_push(PatMatrix *m, PatRow row) {
    if (m->nrows >= m->cap_rows) {
        m->cap_rows = m->cap_rows ? m->cap_rows * 2 : 8;
        m->rows = xs_realloc(m->rows, (size_t)m->cap_rows * sizeof(PatRow));
    }
    m->rows[m->nrows++] = row;
}

static void matrix_free(PatMatrix *m) {
    for (int i = 0; i < m->nrows; i++)
        free(m->rows[i].patterns);
    free(m->rows);
}

static int pat_is_wild(Node *p) {
    if (!p) return 1;
    return VAL_TAG(p) == NODE_PAT_WILD;
}

static int pat_is_var(Node *p) {
    if (!p) return 0;
    return VAL_TAG(p) == NODE_PAT_IDENT;
}

static int pat_is_or(Node *p) {
    if (!p) return 0;
    return VAL_TAG(p) == NODE_PAT_OR;
}

static int pat_is_guard(Node *p) {
    if (!p) return 0;
    return VAL_TAG(p) == NODE_PAT_GUARD;
}

static int pat_is_capture(Node *p) {
    if (!p) return 0;
    return VAL_TAG(p) == NODE_PAT_CAPTURE;
}

static Constructor pat_head_ctor(Node *p) {
    if (!p || pat_is_wild(p) || pat_is_var(p))
        return ctor_wild();
    if (pat_is_capture(p))
        return pat_head_ctor(p->pat_capture.pattern);
    if (pat_is_guard(p))
        return pat_head_ctor(p->pat_guard.pattern);
    switch (VAL_TAG(p)) {
    case NODE_PAT_LIT:
        switch (p->pat_lit.tag) {
        case 0: return ctor_int(p->pat_lit.ival);
        case 1: return ctor_float(p->pat_lit.fval);
        case 2: return ctor_string(p->pat_lit.sval);
        case 3: return ctor_bool(p->pat_lit.bval);
        case 4: return ctor_null();
        default: return ctor_wild();
        }
    case NODE_PAT_ENUM:
        return ctor_enum(p->pat_enum.path, p->pat_enum.args.len);
    case NODE_PAT_TUPLE:
        return ctor_tuple(p->pat_tuple.elems.len);
    case NODE_PAT_RANGE:
    case NODE_RANGE:
        if (VAL_TAG(p) == NODE_PAT_RANGE)
            return ctor_range(p->pat_range.start, p->pat_range.end,
                              p->pat_range.inclusive);
        else
            return ctor_range(p->range.start, p->range.end,
                              p->range.inclusive);
    default:
        return ctor_wild();
    }
}

static int row_all_wild(PatRow *row, int ncols) {
    for (int j = 0; j < ncols; j++) {
        Node *p = row->patterns[j];
        if (!pat_is_wild(p) && !pat_is_var(p) && !pat_is_capture(p))
            return 0;
    }
    return 1;
}

static int best_column(PatMatrix *m) {
    if (m->ncols <= 0) return 0;
    int best = 0;
    int best_score = 0;
    for (int col = 0; col < m->ncols; col++) {
        int score = 0;
        for (int row = 0; row < m->nrows; row++) {
            Node *p = m->rows[row].patterns[col];
            if (!pat_is_wild(p) && !pat_is_var(p))
                score++;
        }
        if (score > best_score) {
            best_score = score;
            best = col;
        }
    }
    return best;
}

#define MAX_CTORS 256

static int collect_ctors(PatMatrix *m, int col, Constructor *out, int max) {
    int n = 0;
    for (int row = 0; row < m->nrows; row++) {
        Node *p = m->rows[row].patterns[col];
        if (pat_is_wild(p) || pat_is_var(p)) continue;
        Constructor c = pat_head_ctor(p);
        if (c.kind == CTOR_WILD) continue;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (ctor_equal(&out[i], &c)) { found = 1; break; }
        }
        if (!found && n < max) {
            out[n++] = c;
        }
    }
    return n;
}

static Node **sub_patterns(Node *p, int arity) {
    Node **subs = xs_calloc((size_t)arity, sizeof(Node *));
    if (!p) return subs;
    if (pat_is_capture(p))
        p = p->pat_capture.pattern;
    if (pat_is_guard(p))
        p = p->pat_guard.pattern;
    if (VAL_TAG(p) == NODE_PAT_TUPLE) {
        for (int i = 0; i < arity && i < p->pat_tuple.elems.len; i++)
            subs[i] = p->pat_tuple.elems.items[i];
    } else if (VAL_TAG(p) == NODE_PAT_ENUM) {
        for (int i = 0; i < arity && i < p->pat_enum.args.len; i++)
            subs[i] = p->pat_enum.args.items[i];
    }
    return subs;
}

static int pat_matches_ctor(Node *p, const Constructor *c) {
    if (!p || pat_is_wild(p) || pat_is_var(p)) return 1;
    if (pat_is_capture(p))
        return pat_matches_ctor(p->pat_capture.pattern, c);
    if (pat_is_guard(p))
        return pat_matches_ctor(p->pat_guard.pattern, c);
    Constructor pc = pat_head_ctor(p);
    return ctor_equal(&pc, c);
}

static PatMatrix specialize(PatMatrix *m, int col, const Constructor *c) {
    int new_cols = m->ncols - 1 + c->arity;
    PatMatrix out = matrix_new(new_cols);
    for (int row = 0; row < m->nrows; row++) {
        Node *p = m->rows[row].patterns[col];
        Node *real_p = p;
        if (pat_is_capture(p)) real_p = p->pat_capture.pattern;
        if (pat_is_guard(p)) real_p = p->pat_guard.pattern;
        if (!pat_matches_ctor(p, c) && !pat_is_wild(p) && !pat_is_var(p) &&
            !pat_is_capture(p) && !pat_is_guard(p))
            continue;
        PatRow new_row;
        new_row.patterns = xs_calloc((size_t)new_cols, sizeof(Node *));
        new_row.guard = m->rows[row].guard;
        new_row.body = m->rows[row].body;
        new_row.arm_index = m->rows[row].arm_index;
        int j = 0;
        for (int k = 0; k < col; k++)
            new_row.patterns[j++] = m->rows[row].patterns[k];
        if (pat_is_wild(p) || pat_is_var(p) || pat_is_capture(p) || pat_is_guard(p)) {
            Node **subs = sub_patterns(real_p, c->arity);
            for (int k = 0; k < c->arity; k++) {
                if (subs[k])
                    new_row.patterns[j++] = subs[k];
                else
                    new_row.patterns[j++] = NULL;
            }
            free(subs);
        } else {
            Node **subs = sub_patterns(p, c->arity);
            for (int k = 0; k < c->arity; k++)
                new_row.patterns[j++] = subs[k];
            free(subs);
        }
        for (int k = col + 1; k < m->ncols; k++)
            new_row.patterns[j++] = m->rows[row].patterns[k];
        matrix_push(&out, new_row);
    }
    return out;
}

static PatMatrix default_matrix(PatMatrix *m, int col) {
    int new_cols = m->ncols - 1;
    PatMatrix out = matrix_new(new_cols < 0 ? 0 : new_cols);
    for (int row = 0; row < m->nrows; row++) {
        Node *p = m->rows[row].patterns[col];
        if (!pat_is_wild(p) && !pat_is_var(p) &&
            !pat_is_capture(p) && !pat_is_guard(p))
            continue;
        PatRow new_row;
        new_row.patterns = xs_calloc((size_t)(new_cols > 0 ? new_cols : 1), sizeof(Node *));
        new_row.guard = m->rows[row].guard;
        new_row.body = m->rows[row].body;
        new_row.arm_index = m->rows[row].arm_index;
        int j = 0;
        for (int k = 0; k < col; k++)
            new_row.patterns[j++] = m->rows[row].patterns[k];
        for (int k = col + 1; k < m->ncols; k++)
            new_row.patterns[j++] = m->rows[row].patterns[k];
        matrix_push(&out, new_row);
    }
    return out;
}

static PatMatrix expand_or(PatMatrix *m, int col) {
    PatMatrix out = matrix_new(m->ncols);
    for (int row = 0; row < m->nrows; row++) {
        Node *p = m->rows[row].patterns[col];
        if (pat_is_or(p)) {
            PatRow left = {0};
            left.patterns = xs_calloc((size_t)m->ncols, sizeof(Node *));
            left.guard = m->rows[row].guard;
            left.body = m->rows[row].body;
            left.arm_index = m->rows[row].arm_index;
            memcpy(left.patterns, m->rows[row].patterns, (size_t)m->ncols * sizeof(Node *));
            left.patterns[col] = p->pat_or.left;
            matrix_push(&out, left);
            PatRow right = {0};
            right.patterns = xs_calloc((size_t)m->ncols, sizeof(Node *));
            right.guard = m->rows[row].guard;
            right.body = m->rows[row].body;
            right.arm_index = m->rows[row].arm_index;
            memcpy(right.patterns, m->rows[row].patterns, (size_t)m->ncols * sizeof(Node *));
            right.patterns[col] = p->pat_or.right;
            matrix_push(&out, right);
        } else {
            PatRow copy = {0};
            copy.patterns = xs_calloc((size_t)m->ncols, sizeof(Node *));
            copy.guard = m->rows[row].guard;
            copy.body = m->rows[row].body;
            copy.arm_index = m->rows[row].arm_index;
            memcpy(copy.patterns, m->rows[row].patterns, (size_t)m->ncols * sizeof(Node *));
            matrix_push(&out, copy);
        }
    }
    return out;
}

static void collect_bindings_from_pat(Node *p, char ***names, int **cols,
                                       int *nbounds, int col) {
    if (!p) return;
    if (pat_is_var(p) && p->pat_ident.name) {
        int idx = *nbounds;
        (*nbounds)++;
        *names = xs_realloc(*names, (size_t)(*nbounds) * sizeof(char *));
        *cols = xs_realloc(*cols, (size_t)(*nbounds) * sizeof(int));
        (*names)[idx] = xs_strdup(p->pat_ident.name);
        (*cols)[idx] = col;
    }
    if (pat_is_capture(p) && p->pat_capture.name) {
        int idx = *nbounds;
        (*nbounds)++;
        *names = xs_realloc(*names, (size_t)(*nbounds) * sizeof(char *));
        *cols = xs_realloc(*cols, (size_t)(*nbounds) * sizeof(int));
        (*names)[idx] = xs_strdup(p->pat_capture.name);
        (*cols)[idx] = col;
        collect_bindings_from_pat(p->pat_capture.pattern, names, cols, nbounds, col);
    }
}

static DTNode *compile_matrix(PatMatrix *m, int depth);

static DTNode *compile_matrix(PatMatrix *m, int depth) {
    if (depth > 100) return dt_fail();
    if (m->nrows == 0) return dt_fail();

    PatRow *first = &m->rows[0];
    if (m->ncols == 0 || row_all_wild(first, m->ncols)) {
        if (first->guard) {
            DTNode *then_dt = dt_leaf(first->arm_index, first->body);
            char **names = NULL;
            int *cols = NULL;
            int nbounds = 0;
            for (int j = 0; j < m->ncols; j++)
                collect_bindings_from_pat(first->patterns[j], &names, &cols, &nbounds, j);
            then_dt->leaf.bound_names = names;
            then_dt->leaf.bound_cols = cols;
            then_dt->leaf.nbounds = nbounds;
            PatMatrix rest = matrix_new(m->ncols);
            for (int r = 1; r < m->nrows; r++) {
                PatRow copy;
                copy.patterns = xs_calloc((size_t)(m->ncols > 0 ? m->ncols : 1), sizeof(Node *));
                if (m->ncols > 0)
                    memcpy(copy.patterns, m->rows[r].patterns,
                           (size_t)m->ncols * sizeof(Node *));
                copy.guard = m->rows[r].guard;
                copy.body = m->rows[r].body;
                copy.arm_index = m->rows[r].arm_index;
                matrix_push(&rest, copy);
            }
            DTNode *else_dt = compile_matrix(&rest, depth + 1);
            matrix_free(&rest);
            return dt_guard(first->guard, then_dt, else_dt);
        }
        DTNode *leaf = dt_leaf(first->arm_index, first->body);
        char **names = NULL;
        int *cols = NULL;
        int nbounds = 0;
        for (int j = 0; j < m->ncols; j++)
            collect_bindings_from_pat(first->patterns[j], &names, &cols, &nbounds, j);
        leaf->leaf.bound_names = names;
        leaf->leaf.bound_cols = cols;
        leaf->leaf.nbounds = nbounds;
        return leaf;
    }

    int has_or = 0;
    for (int col = 0; col < m->ncols; col++) {
        for (int row = 0; row < m->nrows; row++) {
            if (pat_is_or(m->rows[row].patterns[col])) {
                has_or = 1;
                PatMatrix expanded = expand_or(m, col);
                DTNode *result = compile_matrix(&expanded, depth + 1);
                matrix_free(&expanded);
                return result;
            }
        }
        if (has_or) break;
    }

    int col = best_column(m);
    Constructor ctors[MAX_CTORS];
    int nctors = collect_ctors(m, col, ctors, MAX_CTORS);

    if (nctors == 0) {
        PatMatrix def = default_matrix(m, col);
        DTNode *result = compile_matrix(&def, depth + 1);
        matrix_free(&def);
        return result;
    }

    DTNode *sw = dt_switch(col);
    for (int ci = 0; ci < nctors; ci++) {
        PatMatrix spec = specialize(m, col, &ctors[ci]);
        DTNode *child = compile_matrix(&spec, depth + 1);
        matrix_free(&spec);
        dt_add_branch(sw, ctors[ci], child);
    }

    PatMatrix def = default_matrix(m, col);
    if (def.nrows > 0) {
        sw->sw.fallback = compile_matrix(&def, depth + 1);
    } else {
        sw->sw.fallback = dt_fail();
    }
    matrix_free(&def);

    return sw;
}

DTNode *match_compile(Node *match_node) {
    if (!match_node || VAL_TAG(match_node) != NODE_MATCH)
        return dt_fail();

    int n_arms = match_node->match.arms.len;
    if (n_arms == 0) return dt_fail();

    PatMatrix m = matrix_new(1);
    for (int i = 0; i < n_arms; i++) {
        MatchArm *arm = &match_node->match.arms.items[i];
        PatRow row;
        row.patterns = xs_calloc(1, sizeof(Node *));
        row.patterns[0] = arm->pattern;
        row.guard = arm->guard;
        row.body = arm->body;
        row.arm_index = i;
        matrix_push(&m, row);
    }

    DTNode *dt = compile_matrix(&m, 0);
    matrix_free(&m);
    return dt;
}

static Node *make_ident(const char *name, Span span) {
    Node *n = node_new(NODE_IDENT, span);
    n->ident.name = xs_strdup(name);
    return n;
}

static Node *make_int_node(int64_t v, Span span) {
    Node *n = node_new(NODE_LIT_INT, span);
    n->lit_int.ival = v;
    return n;
}

static Node *make_bool_node(int v, Span span) {
    Node *n = node_new(NODE_LIT_BOOL, span);
    n->lit_bool.bval = v;
    return n;
}

static Node *make_null_node(Span span) {
    return node_new(NODE_LIT_NULL, span);
}

static Node *make_string_node(const char *s, Span span) {
    Node *n = node_new(NODE_LIT_STRING, span);
    n->lit_string.sval = xs_strdup(s);
    n->lit_string.interpolated = 0;
    n->lit_string.parts = nodelist_new();
    return n;
}

static Node *make_float_node(double v, Span span) {
    Node *n = node_new(NODE_LIT_FLOAT, span);
    n->lit_float.fval = v;
    return n;
}

static Node *make_eq(Node *left, Node *right, Span span) {
    Node *n = node_new(NODE_BINOP, span);
    strcpy(n->binop.op, "==");
    n->binop.left = left;
    n->binop.right = right;
    return n;
}

static Node *make_and(Node *left, Node *right, Span span) {
    Node *n = node_new(NODE_BINOP, span);
    strcpy(n->binop.op, "&&");
    n->binop.left = left;
    n->binop.right = right;
    return n;
}

static Node *make_lte(Node *left, Node *right, Span span) {
    Node *n = node_new(NODE_BINOP, span);
    strcpy(n->binop.op, "<=");
    n->binop.left = left;
    n->binop.right = right;
    return n;
}

static Node *make_lt(Node *left, Node *right, Span span) {
    Node *n = node_new(NODE_BINOP, span);
    strcpy(n->binop.op, "<");
    n->binop.left = left;
    n->binop.right = right;
    return n;
}

static Node *make_if(Node *cond, Node *then_blk, Node *else_blk, Span span) {
    Node *n = node_new(NODE_IF, span);
    n->if_expr.cond = cond;
    n->if_expr.then = then_blk;
    n->if_expr.elif_conds = nodelist_new();
    n->if_expr.elif_thens = nodelist_new();
    n->if_expr.else_branch = else_blk;
    return n;
}

static Node *make_let(const char *name, Node *val, Span span) {
    Node *n = node_new(NODE_LET, span);
    n->let.name = xs_strdup(name);
    n->let.value = val;
    n->let.mutable = 0;
    n->let.pattern = NULL;
    n->let.type_ann = NULL;
    n->let.contract = NULL;
    return n;
}

static Node *make_block_from_stmts(NodeList stmts, Node *expr, Span span) {
    Node *n = node_new(NODE_BLOCK, span);
    n->block.stmts = stmts;
    n->block.expr = expr;
    n->block.has_decls = -1;
    n->block.is_unsafe = 0;
    return n;
}

static Node *make_field_access(Node *obj, const char *field, Span span) {
    Node *n = node_new(NODE_FIELD, span);
    n->field.obj = obj;
    n->field.name = xs_strdup(field);
    n->field.optional = 0;
    return n;
}

static Node *make_index(Node *obj, Node *idx, Span span) {
    Node *n = node_new(NODE_INDEX, span);
    n->index.obj = obj;
    n->index.index = idx;
    return n;
}

static Node *ctor_test_expr(const Constructor *c, Node *subject, Span span) {
    switch (c->kind) {
    case CTOR_INT:
        return make_eq(subject, make_int_node(c->ival, span), span);
    case CTOR_FLOAT:
        return make_eq(subject, make_float_node(c->fval, span), span);
    case CTOR_STRING:
        return make_eq(subject, make_string_node(c->sval, span), span);
    case CTOR_BOOL:
        return make_eq(subject, make_bool_node(c->bval, span), span);
    case CTOR_NULL:
        return make_eq(subject, make_null_node(span), span);
    case CTOR_ENUM: {
        Node *tag = make_field_access(subject, "_tag", span);
        const char *path = c->enum_path;
        const char *last = strrchr(path, ':');
        const char *variant = (last && last > path) ? last + 1 : path;
        return make_eq(tag, make_string_node(variant, span), span);
    }
    case CTOR_RANGE: {
        Node *lo = make_lte(c->range_start, subject, span);
        Node *hi = c->range_inclusive
            ? make_lte(subject, c->range_end, span)
            : make_lt(subject, c->range_end, span);
        return make_and(lo, hi, span);
    }
    case CTOR_TUPLE:
    case CTOR_WILD:
        return make_bool_node(1, span);
    }
    return make_bool_node(1, span);
}

static Node *ctor_extract_expr(const Constructor *c, Node *subject,
                                int sub_index, Span span) {
    switch (c->kind) {
    case CTOR_TUPLE:
        return make_index(subject, make_int_node(sub_index, span), span);
    case CTOR_ENUM:
        if (c->arity == 1)
            return make_field_access(subject, "_val", span);
        else {
            Node *val = make_field_access(subject, "_val", span);
            return make_index(val, make_int_node(sub_index, span), span);
        }
    default:
        return subject;
    }
}

static Node *dt_to_ast_inner(DTNode *dt, Node *subject, Span span,
                              char **col_names, int ncol_names) {
    if (!dt) return make_null_node(span);

    switch (dt->kind) {
    case DT_FAIL:
        return make_null_node(span);

    case DT_LEAF: {
        if (dt->leaf.nbounds == 0)
            return dt->leaf.body;
        NodeList stmts = nodelist_new();
        for (int i = 0; i < dt->leaf.nbounds; i++) {
            int col = dt->leaf.bound_cols[i];
            Node *val;
            if (col == 0 && ncol_names <= 1)
                val = subject;
            else if (col < ncol_names && col_names[col])
                val = make_ident(col_names[col], span);
            else
                val = subject;
            nodelist_push(&stmts, make_let(dt->leaf.bound_names[i], val, span));
        }
        return make_block_from_stmts(stmts, dt->leaf.body, span);
    }

    case DT_GUARD: {
        Node *then_ast = dt_to_ast_inner(dt->guard.then_dt, subject, span,
                                          col_names, ncol_names);
        Node *else_ast = dt_to_ast_inner(dt->guard.else_dt, subject, span,
                                          col_names, ncol_names);
        NodeList then_stmts = nodelist_new();
        nodelist_push(&then_stmts, then_ast);
        Node *then_blk = make_block_from_stmts(then_stmts, NULL, span);
        NodeList else_stmts = nodelist_new();
        nodelist_push(&else_stmts, else_ast);
        Node *else_blk = make_block_from_stmts(else_stmts, NULL, span);
        return make_if(dt->guard.cond, then_blk, else_blk, span);
    }

    case DT_SWITCH: {
        Node *result = dt_to_ast_inner(dt->sw.fallback, subject, span,
                                        col_names, ncol_names);
        for (int i = dt->sw.nbranches - 1; i >= 0; i--) {
            DTBranch *br = &dt->sw.branches[i];
            Node *test = ctor_test_expr(&br->ctor, subject, span);
            Node *child = dt_to_ast_inner(br->child, subject, span,
                                           col_names, ncol_names);
            NodeList then_stmts = nodelist_new();
            if (br->ctor.arity > 0) {
                for (int s = 0; s < br->ctor.arity; s++) {
                    char name[64];
                    snprintf(name, sizeof name, "__mc_%d_%d", dt->sw.col, s);
                    Node *extract = ctor_extract_expr(&br->ctor, subject, s, span);
                    nodelist_push(&then_stmts, make_let(name, extract, span));
                }
            }
            nodelist_push(&then_stmts, child);
            Node *then_blk = make_block_from_stmts(then_stmts, NULL, span);
            NodeList else_stmts = nodelist_new();
            nodelist_push(&else_stmts, result);
            Node *else_blk = make_block_from_stmts(else_stmts, NULL, span);
            result = make_if(test, then_blk, else_blk, span);
        }
        return result;
    }
    }
    return make_null_node(span);
}

Node *dt_to_ast(DTNode *dt, Node *subject, Span span) {
    return dt_to_ast_inner(dt, subject, span, NULL, 0);
}

static int count_arms(Node *n) {
    if (!n || VAL_TAG(n) != NODE_MATCH) return 0;
    return n->match.arms.len;
}

static int should_compile_match(Node *n) {
    int arms = count_arms(n);
    if (arms < 3) return 0;
    int has_complex = 0;
    for (int i = 0; i < n->match.arms.len; i++) {
        Node *p = n->match.arms.items[i].pattern;
        if (!p) continue;
        if (VAL_TAG(p) == NODE_PAT_ENUM || VAL_TAG(p) == NODE_PAT_TUPLE ||
            VAL_TAG(p) == NODE_PAT_OR || VAL_TAG(p) == NODE_PAT_RANGE ||
            VAL_TAG(p) == NODE_PAT_GUARD || VAL_TAG(p) == NODE_PAT_CAPTURE)
            has_complex = 1;
    }
    return arms >= 4 || has_complex;
}

static Node *optimize_node(Node *n);

static void optimize_list(NodeList *nl) {
    for (int i = 0; i < nl->len; i++)
        nl->items[i] = optimize_node(nl->items[i]);
}

static Node *optimize_node(Node *n) {
    if (!n) return NULL;

    switch (VAL_TAG(n)) {
    case NODE_BLOCK:
        optimize_list(&n->block.stmts);
        n->block.expr = optimize_node(n->block.expr);
        break;
    case NODE_FN_DECL:
        n->fn_decl.body = optimize_node(n->fn_decl.body);
        break;
    case NODE_IF:
        n->if_expr.cond = optimize_node(n->if_expr.cond);
        n->if_expr.then = optimize_node(n->if_expr.then);
        optimize_list(&n->if_expr.elif_conds);
        optimize_list(&n->if_expr.elif_thens);
        n->if_expr.else_branch = optimize_node(n->if_expr.else_branch);
        break;
    case NODE_WHILE:
        n->while_loop.body = optimize_node(n->while_loop.body);
        break;
    case NODE_FOR:
        n->for_loop.body = optimize_node(n->for_loop.body);
        break;
    case NODE_MATCH:
        if (should_compile_match(n)) {
            DTNode *dt = match_compile(n);
            Node *result = dt_to_ast(dt, n->match.subject, n->span);
            dt_free(dt);
            return result;
        }
        break;
    case NODE_PROGRAM:
        optimize_list(&n->program.stmts);
        break;
    case NODE_LAMBDA:
        n->lambda.body = optimize_node(n->lambda.body);
        break;
    case NODE_CLASS_DECL:
        optimize_list(&n->class_decl.members);
        break;
    case NODE_IMPL_DECL:
        optimize_list(&n->impl_decl.members);
        break;
    case NODE_MODULE_DECL:
        optimize_list(&n->module_decl.body);
        break;
    case NODE_TRY:
        n->try_.body = optimize_node(n->try_.body);
        if (n->try_.finally_block)
            n->try_.finally_block = optimize_node(n->try_.finally_block);
        break;
    default:
        break;
    }
    return n;
}

void match_compiler_optimize(Node *program) {
    optimize_node(program);
}

