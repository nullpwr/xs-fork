#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "db/xsdb.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

static void *xsdb_alloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "xsdb: out of memory (%zu bytes)\n", n);
        abort();
    }
    return p;
}

static char *xsdb_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = xsdb_alloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

static XSDBResult *result_new(void) {
    XSDBResult *r = xsdb_alloc(sizeof(XSDBResult));
    r->cap = 16;
    r->rows = xsdb_alloc(sizeof(XSDBRow) * r->cap);
    return r;
}

static XSDBResult *result_error(const char *fmt, ...) {
    XSDBResult *r = result_new();
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    r->error = xsdb_strdup(buf);
    return r;
}

static XSDBResult *result_ok(int affected) {
    XSDBResult *r = result_new();
    r->affected = affected;
    return r;
}

static void result_add_row(XSDBResult *r, XSDBRow *row) {
    if (r->nrows >= r->cap) {
        r->cap *= 2;
        r->rows = realloc(r->rows, sizeof(XSDBRow) * r->cap);
    }
    r->rows[r->nrows++] = *row;
}

static void result_set_columns(XSDBResult *r, char **names, int n) {
    r->col_names = xsdb_alloc(sizeof(char *) * n);
    r->ncols = n;
    for (int i = 0; i < n; i++) {
        r->col_names[i] = xsdb_strdup(names[i]);
    }
}

/* ----------------------------------------------------------------
 * Value operations
 * ---------------------------------------------------------------- */

XSDBValue xsdb_value_text(const char *s) {
    XSDBValue v;
    v.type = XSDB_TYPE_TEXT;
    v.text = xsdb_strdup(s);
    return v;
}

XSDBValue xsdb_value_int(int64_t i) {
    XSDBValue v;
    v.type = XSDB_TYPE_INT;
    v.ival = i;
    return v;
}

XSDBValue xsdb_value_float(double f) {
    XSDBValue v;
    v.type = XSDB_TYPE_FLOAT;
    v.fval = f;
    return v;
}

XSDBValue xsdb_value_null(void) {
    XSDBValue v;
    memset(&v, 0, sizeof v);
    v.type = XSDB_TYPE_NULL;
    return v;
}

static XSDBValue xsdb_value_copy(const XSDBValue *src) {
    XSDBValue v;
    v.type = src->type;
    switch (src->type) {
    case XSDB_TYPE_TEXT:
        v.text = xsdb_strdup(src->text);
        break;
    case XSDB_TYPE_INT:
        v.ival = src->ival;
        break;
    case XSDB_TYPE_FLOAT:
        v.fval = src->fval;
        break;
    case XSDB_TYPE_BLOB:
        v.blob.len = src->blob.len;
        v.blob.data = xsdb_alloc(src->blob.len);
        memcpy(v.blob.data, src->blob.data, src->blob.len);
        break;
    case XSDB_TYPE_NULL:
    case XSDB_TYPE_AUTO:
    default:
        v.type = XSDB_TYPE_NULL;
        break;
    }
    return v;
}

void xsdb_value_free(XSDBValue *v) {
    if (!v) return;
    switch (v->type) {
    case XSDB_TYPE_TEXT:
        free(v->text);
        v->text = NULL;
        break;
    case XSDB_TYPE_BLOB:
        free(v->blob.data);
        v->blob.data = NULL;
        break;
    default:
        break;
    }
    v->type = XSDB_TYPE_NULL;
}

char *xsdb_value_to_string(const XSDBValue *v) {
    char buf[256];
    switch (v->type) {
    case XSDB_TYPE_TEXT:
        return xsdb_strdup(v->text);
    case XSDB_TYPE_INT:
        snprintf(buf, sizeof buf, "%lld", (long long)v->ival);
        return xsdb_strdup(buf);
    case XSDB_TYPE_FLOAT:
        snprintf(buf, sizeof buf, "%g", v->fval);
        return xsdb_strdup(buf);
    case XSDB_TYPE_NULL:
        return xsdb_strdup("NULL");
    case XSDB_TYPE_BLOB:
        return xsdb_strdup("<blob>");
    default:
        return xsdb_strdup("");
    }
}

/* Numeric coercion for comparisons */
static double xsdb_value_as_number(const XSDBValue *v) {
    switch (v->type) {
    case XSDB_TYPE_INT:   return (double)v->ival;
    case XSDB_TYPE_FLOAT: return v->fval;
    case XSDB_TYPE_TEXT:  {
        char *end;
        double d = strtod(v->text, &end);
        if (end != v->text && *end == '\0') return d;
        return 0.0;
    }
    default: return 0.0;
    }
}

static int xsdb_value_is_numeric(const XSDBValue *v) {
    if (v->type == XSDB_TYPE_INT || v->type == XSDB_TYPE_FLOAT) return 1;
    if (v->type == XSDB_TYPE_TEXT && v->text) {
        char *end;
        (void)strtod(v->text, &end);
        return (end != v->text && *end == '\0');
    }
    return 0;
}

int xsdb_value_compare(const XSDBValue *a, const XSDBValue *b) {
    if (a->type == XSDB_TYPE_NULL && b->type == XSDB_TYPE_NULL) return 0;
    if (a->type == XSDB_TYPE_NULL) return -1;
    if (b->type == XSDB_TYPE_NULL) return 1;

    /* Both numeric? compare numerically */
    if (xsdb_value_is_numeric(a) && xsdb_value_is_numeric(b)) {
        double da = xsdb_value_as_number(a);
        double db = xsdb_value_as_number(b);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    /* Fall back to string comparison */
    char *sa = xsdb_value_to_string(a);
    char *sb = xsdb_value_to_string(b);
    int r = strcmp(sa, sb);
    free(sa);
    free(sb);
    return r;
}

/* LIKE pattern matching (SQL wildcards: % = any chars, _ = single char) */
static int like_match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '%') {
            pattern++;
            if (*pattern == '\0') return 1;
            while (*str) {
                if (like_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '_') {
            if (*str == '\0') return 0;
            pattern++;
            str++;
        } else {
            if (tolower((unsigned char)*pattern) != tolower((unsigned char)*str))
                return 0;
            pattern++;
            str++;
        }
    }
    return (*str == '\0');
}

/* Apply a single comparison */
static int xsdb_cmp_check(const XSDBValue *cell, XSDBCmpOp op, const XSDBValue *val) {
    if (op == XSDB_CMP_IS_NULL) return cell->type == XSDB_TYPE_NULL;
    if (op == XSDB_CMP_NOT_NULL) return cell->type != XSDB_TYPE_NULL;

    if (op == XSDB_CMP_LIKE) {
        char *cs = xsdb_value_to_string(cell);
        char *vs = xsdb_value_to_string(val);
        int r = like_match(vs, cs);
        free(cs);
        free(vs);
        return r;
    }

    int c = xsdb_value_compare(cell, val);
    switch (op) {
    case XSDB_CMP_EQ: return c == 0;
    case XSDB_CMP_NE: return c != 0;
    case XSDB_CMP_LT: return c < 0;
    case XSDB_CMP_LE: return c <= 0;
    case XSDB_CMP_GT: return c > 0;
    case XSDB_CMP_GE: return c >= 0;
    default: return 0;
    }
}

/* ----------------------------------------------------------------
 * Row operations
 * ---------------------------------------------------------------- */

static XSDBRow *row_new(int ncols) {
    XSDBRow *r = xsdb_alloc(sizeof(XSDBRow));
    r->ncells = ncols;
    r->cells = xsdb_alloc(sizeof(XSDBValue) * ncols);
    for (int i = 0; i < ncols; i++) {
        r->cells[i] = xsdb_value_null();
    }
    return r;
}

static XSDBRow *row_copy(const XSDBRow *src) {
    XSDBRow *r = xsdb_alloc(sizeof(XSDBRow));
    r->rowid = src->rowid;
    r->ncells = src->ncells;
    r->cells = xsdb_alloc(sizeof(XSDBValue) * src->ncells);
    for (int i = 0; i < src->ncells; i++) {
        r->cells[i] = xsdb_value_copy(&src->cells[i]);
    }
    return r;
}

static void row_free(XSDBRow *r) {
    if (!r) return;
    for (int i = 0; i < r->ncells; i++) {
        xsdb_value_free(&r->cells[i]);
    }
    free(r->cells);
    free(r);
}

/* ----------------------------------------------------------------
 * B-Tree implementation
 * ---------------------------------------------------------------- */

XSDBBTreeNode *xsdb_btree_new_node(int is_leaf) {
    XSDBBTreeNode *n = xsdb_alloc(sizeof(XSDBBTreeNode));
    n->is_leaf = is_leaf;
    n->nkeys = 0;
    memset(n->children, 0, sizeof n->children);
    memset(n->rows, 0, sizeof n->rows);
    return n;
}

void xsdb_btree_free(XSDBBTreeNode *root) {
    if (!root) return;
    if (!root->is_leaf) {
        for (int i = 0; i <= root->nkeys; i++) {
            xsdb_btree_free(root->children[i]);
        }
    }
    for (int i = 0; i < root->nkeys; i++) {
        row_free(root->rows[i]);
    }
    free(root);
}

XSDBRow *xsdb_btree_search(XSDBBTreeNode *root, int64_t key) {
    if (!root) return NULL;

    int i = 0;
    while (i < root->nkeys && key > root->keys[i]) i++;

    if (i < root->nkeys && key == root->keys[i]) {
        return root->rows[i];
    }

    if (root->is_leaf) return NULL;
    return xsdb_btree_search(root->children[i], key);
}

/* Split child node at index idx of parent */
static void btree_split_child(XSDBBTreeNode *parent, int idx) {
    XSDBBTreeNode *child = parent->children[idx];
    int mid = (XSDB_ORDER - 1) / 2;

    XSDBBTreeNode *sibling = xsdb_btree_new_node(child->is_leaf);
    sibling->nkeys = child->nkeys - mid - 1;

    /* Copy right half of keys/rows to sibling */
    for (int j = 0; j < sibling->nkeys; j++) {
        sibling->keys[j] = child->keys[mid + 1 + j];
        sibling->rows[j] = child->rows[mid + 1 + j];
        child->rows[mid + 1 + j] = NULL;
    }

    if (!child->is_leaf) {
        for (int j = 0; j <= sibling->nkeys; j++) {
            sibling->children[j] = child->children[mid + 1 + j];
            child->children[mid + 1 + j] = NULL;
        }
    }

    /* Make room in parent */
    for (int j = parent->nkeys; j > idx; j--) {
        parent->children[j + 1] = parent->children[j];
    }
    parent->children[idx + 1] = sibling;

    for (int j = parent->nkeys - 1; j >= idx; j--) {
        parent->keys[j + 1] = parent->keys[j];
        parent->rows[j + 1] = parent->rows[j];
    }

    /* Promote middle key */
    parent->keys[idx] = child->keys[mid];
    parent->rows[idx] = child->rows[mid];
    child->rows[mid] = NULL;
    parent->nkeys++;
    child->nkeys = mid;
}

/* Insert into non-full node */
static void btree_insert_nonfull(XSDBBTreeNode *node, int64_t key, XSDBRow *row) {
    int i = node->nkeys - 1;

    if (node->is_leaf) {
        /* Shift keys right and insert */
        while (i >= 0 && key < node->keys[i]) {
            node->keys[i + 1] = node->keys[i];
            node->rows[i + 1] = node->rows[i];
            i--;
        }
        node->keys[i + 1] = key;
        node->rows[i + 1] = row;
        node->nkeys++;
    } else {
        /* Find child to insert into */
        while (i >= 0 && key < node->keys[i]) i--;
        i++;
        if (node->children[i]->nkeys == XSDB_ORDER - 1) {
            btree_split_child(node, i);
            if (key > node->keys[i]) i++;
        }
        btree_insert_nonfull(node->children[i], key, row);
    }
}

int xsdb_btree_insert(XSDBBTreeNode **root, int64_t key, XSDBRow *row) {
    if (!*root) {
        *root = xsdb_btree_new_node(1);
        (*root)->keys[0] = key;
        (*root)->rows[0] = row;
        (*root)->nkeys = 1;
        return 0;
    }

    /* Check for duplicate key */
    if (xsdb_btree_search(*root, key)) {
        return -1; /* duplicate */
    }

    if ((*root)->nkeys == XSDB_ORDER - 1) {
        XSDBBTreeNode *new_root = xsdb_btree_new_node(0);
        new_root->children[0] = *root;
        btree_split_child(new_root, 0);
        int i = 0;
        if (key > new_root->keys[0]) i++;
        btree_insert_nonfull(new_root->children[i], key, row);
        *root = new_root;
    } else {
        btree_insert_nonfull(*root, key, row);
    }
    return 0;
}

/* Find predecessor (largest key in left subtree) */
static int64_t btree_predecessor_key(XSDBBTreeNode *node) {
    while (!node->is_leaf)
        node = node->children[node->nkeys];
    return node->keys[node->nkeys - 1];
}

/* Find successor (smallest key in right subtree) */
static int64_t btree_successor_key(XSDBBTreeNode *node) {
    while (!node->is_leaf)
        node = node->children[0];
    return node->keys[0];
}

/* Merge child[idx+1] into child[idx] with parent key at idx */
static void btree_merge(XSDBBTreeNode *parent, int idx) {
    XSDBBTreeNode *left  = parent->children[idx];
    XSDBBTreeNode *right = parent->children[idx + 1];
    int mid = left->nkeys;

    /* Pull down parent key */
    left->keys[mid] = parent->keys[idx];
    left->rows[mid] = parent->rows[idx];

    /* Copy right's keys/rows to left */
    for (int j = 0; j < right->nkeys; j++) {
        left->keys[mid + 1 + j] = right->keys[j];
        left->rows[mid + 1 + j] = right->rows[j];
        right->rows[j] = NULL;
    }
    if (!left->is_leaf) {
        for (int j = 0; j <= right->nkeys; j++) {
            left->children[mid + 1 + j] = right->children[j];
            right->children[j] = NULL;
        }
    }
    left->nkeys = mid + 1 + right->nkeys;

    /* Remove key from parent */
    for (int j = idx; j < parent->nkeys - 1; j++) {
        parent->keys[j] = parent->keys[j + 1];
        parent->rows[j] = parent->rows[j + 1];
    }
    parent->rows[parent->nkeys - 1] = NULL;
    for (int j = idx + 1; j < parent->nkeys; j++) {
        parent->children[j] = parent->children[j + 1];
    }
    parent->children[parent->nkeys] = NULL;
    parent->nkeys--;

    free(right);
}

/* Fill child[idx] that has fewer than ceil(ORDER/2)-1 keys */
static void btree_fill(XSDBBTreeNode *parent, int idx) {
    int min_keys = (XSDB_ORDER - 1) / 2;

    /* Try borrowing from left sibling */
    if (idx > 0 && parent->children[idx - 1]->nkeys > min_keys) {
        XSDBBTreeNode *child = parent->children[idx];
        XSDBBTreeNode *left  = parent->children[idx - 1];

        /* Shift child's keys right */
        for (int j = child->nkeys - 1; j >= 0; j--) {
            child->keys[j + 1] = child->keys[j];
            child->rows[j + 1] = child->rows[j];
        }
        if (!child->is_leaf) {
            for (int j = child->nkeys; j >= 0; j--) {
                child->children[j + 1] = child->children[j];
            }
        }

        child->keys[0] = parent->keys[idx - 1];
        child->rows[0] = parent->rows[idx - 1];
        if (!child->is_leaf)
            child->children[0] = left->children[left->nkeys];

        parent->keys[idx - 1] = left->keys[left->nkeys - 1];
        parent->rows[idx - 1] = left->rows[left->nkeys - 1];
        left->rows[left->nkeys - 1] = NULL;
        if (!left->is_leaf)
            left->children[left->nkeys] = NULL;

        child->nkeys++;
        left->nkeys--;
        return;
    }

    /* Try borrowing from right sibling */
    if (idx < parent->nkeys && parent->children[idx + 1]->nkeys > min_keys) {
        XSDBBTreeNode *child = parent->children[idx];
        XSDBBTreeNode *right = parent->children[idx + 1];

        child->keys[child->nkeys] = parent->keys[idx];
        child->rows[child->nkeys] = parent->rows[idx];
        if (!child->is_leaf)
            child->children[child->nkeys + 1] = right->children[0];

        parent->keys[idx] = right->keys[0];
        parent->rows[idx] = right->rows[0];

        for (int j = 0; j < right->nkeys - 1; j++) {
            right->keys[j] = right->keys[j + 1];
            right->rows[j] = right->rows[j + 1];
        }
        right->rows[right->nkeys - 1] = NULL;
        if (!right->is_leaf) {
            for (int j = 0; j < right->nkeys; j++) {
                right->children[j] = right->children[j + 1];
            }
            right->children[right->nkeys] = NULL;
        }

        child->nkeys++;
        right->nkeys--;
        return;
    }

    /* Merge with a sibling */
    if (idx < parent->nkeys) {
        btree_merge(parent, idx);
    } else {
        btree_merge(parent, idx - 1);
    }
}

/* Delete key from subtree rooted at node */
static int btree_delete_recursive(XSDBBTreeNode *node, int64_t key) {
    int i = 0;
    while (i < node->nkeys && key > node->keys[i]) i++;

    int min_keys = (XSDB_ORDER - 1) / 2;

    if (i < node->nkeys && key == node->keys[i]) {
        /* Key found in this node */
        if (node->is_leaf) {
            /* Simple removal from leaf */
            row_free(node->rows[i]);
            for (int j = i; j < node->nkeys - 1; j++) {
                node->keys[j] = node->keys[j + 1];
                node->rows[j] = node->rows[j + 1];
            }
            node->rows[node->nkeys - 1] = NULL;
            node->nkeys--;
            return 0;
        }

        /* Internal node */
        if (node->children[i]->nkeys > min_keys) {
            /* Replace with predecessor */
            int64_t pred = btree_predecessor_key(node->children[i]);
            XSDBRow *pred_row = xsdb_btree_search(node->children[i], pred);
            XSDBRow *copy = row_copy(pred_row);
            row_free(node->rows[i]);
            node->keys[i] = pred;
            node->rows[i] = copy;
            return btree_delete_recursive(node->children[i], pred);
        } else if (node->children[i + 1]->nkeys > min_keys) {
            /* Replace with successor */
            int64_t succ = btree_successor_key(node->children[i + 1]);
            XSDBRow *succ_row = xsdb_btree_search(node->children[i + 1], succ);
            XSDBRow *copy = row_copy(succ_row);
            row_free(node->rows[i]);
            node->keys[i] = succ;
            node->rows[i] = copy;
            return btree_delete_recursive(node->children[i + 1], succ);
        } else {
            /* Merge children */
            btree_merge(node, i);
            return btree_delete_recursive(node->children[i], key);
        }
    } else {
        /* Key not in this node, recurse */
        if (node->is_leaf) return -1; /* not found */

        int child_depleted = (node->children[i]->nkeys <= min_keys);
        if (child_depleted) {
            btree_fill(node, i);
        }

        /* After fill, the target child might have changed position */
        if (i > node->nkeys) {
            return btree_delete_recursive(node->children[i - 1], key);
        }
        return btree_delete_recursive(node->children[i], key);
    }
}

int xsdb_btree_delete(XSDBBTreeNode **root, int64_t key) {
    if (!*root) return -1;

    int rc = btree_delete_recursive(*root, key);
    if (rc != 0) return rc;

    /* If root is empty but has a child, shrink tree */
    if ((*root)->nkeys == 0 && !(*root)->is_leaf) {
        XSDBBTreeNode *old = *root;
        *root = old->children[0];
        old->children[0] = NULL;
        free(old);
    } else if ((*root)->nkeys == 0) {
        free(*root);
        *root = NULL;
    }
    return 0;
}

/* In-order traversal */
void xsdb_btree_scan(XSDBBTreeNode *root,
                       void (*cb)(XSDBRow *row, void *ctx), void *ctx)
{
    if (!root) return;

    for (int i = 0; i < root->nkeys; i++) {
        if (!root->is_leaf) {
            xsdb_btree_scan(root->children[i], cb, ctx);
        }
        cb(root->rows[i], ctx);
    }
    if (!root->is_leaf) {
        xsdb_btree_scan(root->children[root->nkeys], cb, ctx);
    }
}

/* Count entries in tree */
static void btree_count_cb(XSDBRow *row, void *ctx) {
    (void)row;
    (*(int *)ctx)++;
}

int xsdb_btree_count(XSDBBTreeNode *root) {
    int count = 0;
    xsdb_btree_scan(root, btree_count_cb, &count);
    return count;
}

/* ----------------------------------------------------------------
 * SQL parser
 * ---------------------------------------------------------------- */

typedef struct {
    const char *src;
    int         pos;
    int         len;
    char        error[512];
} SQLParser;

static void sql_init(SQLParser *p, const char *sql) {
    p->src = sql;
    p->pos = 0;
    p->len = (int)strlen(sql);
    p->error[0] = '\0';
}

static void sql_skip_ws(SQLParser *p) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos]))
        p->pos++;
}

static int sql_peek(SQLParser *p) {
    sql_skip_ws(p);
    if (p->pos >= p->len) return 0;
    return p->src[p->pos];
}

static int sql_match_kw(SQLParser *p, const char *kw) {
    sql_skip_ws(p);
    int klen = (int)strlen(kw);
    if (p->pos + klen > p->len) return 0;
    if (strncasecmp(p->src + p->pos, kw, klen) != 0) return 0;
    char next = p->src[p->pos + klen];
    if (next != '\0' && !isspace((unsigned char)next) && next != '('
        && next != ')' && next != ',' && next != ';')
        return 0;
    p->pos += klen;
    return 1;
}

static int sql_read_ident(SQLParser *p, char *buf, int bufsz) {
    sql_skip_ws(p);
    int i = 0;

    /* Handle quoted identifiers */
    if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '`')) {
        char q = p->src[p->pos++];
        while (p->pos < p->len && p->src[p->pos] != q && i < bufsz - 1) {
            buf[i++] = p->src[p->pos++];
        }
        if (p->pos < p->len) p->pos++; /* skip closing quote */
        buf[i] = '\0';
        return i > 0;
    }

    while (p->pos < p->len && (isalnum((unsigned char)p->src[p->pos])
           || p->src[p->pos] == '_') && i < bufsz - 1) {
        buf[i++] = p->src[p->pos++];
    }
    buf[i] = '\0';
    return i > 0;
}

/* Read a value: string literal, number, or bare identifier */
static XSDBValue sql_read_value(SQLParser *p) {
    sql_skip_ws(p);

    /* NULL keyword */
    if (sql_match_kw(p, "NULL")) {
        return xsdb_value_null();
    }

    /* String literal */
    if (p->pos < p->len && (p->src[p->pos] == '\'' || p->src[p->pos] == '"')) {
        char quote = p->src[p->pos++];
        char buf[XSDB_VAL_MAX];
        int i = 0;
        while (p->pos < p->len && p->src[p->pos] != quote && i < XSDB_VAL_MAX - 1) {
            /* Handle escape sequences */
            if (p->src[p->pos] == '\\' && p->pos + 1 < p->len) {
                p->pos++;
                switch (p->src[p->pos]) {
                case 'n':  buf[i++] = '\n'; break;
                case 't':  buf[i++] = '\t'; break;
                case '\\': buf[i++] = '\\'; break;
                case '\'': buf[i++] = '\''; break;
                case '"':  buf[i++] = '"';  break;
                default:   buf[i++] = p->src[p->pos]; break;
                }
                p->pos++;
            } else if (p->src[p->pos] == quote && p->pos + 1 < p->len
                       && p->src[p->pos + 1] == quote) {
                /* SQL-style escaped quote: '' -> ' */
                buf[i++] = quote;
                p->pos += 2;
            } else {
                buf[i++] = p->src[p->pos++];
            }
        }
        if (p->pos < p->len) p->pos++; /* skip closing quote */
        buf[i] = '\0';
        return xsdb_value_text(buf);
    }

    /* Number (integer or float) */
    if (p->pos < p->len && (isdigit((unsigned char)p->src[p->pos])
        || (p->src[p->pos] == '-' && p->pos + 1 < p->len
            && isdigit((unsigned char)p->src[p->pos + 1])))) {
        char buf[128];
        int i = 0;
        int is_float = 0;
        if (p->src[p->pos] == '-') buf[i++] = p->src[p->pos++];
        while (p->pos < p->len && (isdigit((unsigned char)p->src[p->pos])
               || p->src[p->pos] == '.') && i < 127) {
            if (p->src[p->pos] == '.') is_float = 1;
            buf[i++] = p->src[p->pos++];
        }
        /* Scientific notation */
        if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
            is_float = 1;
            buf[i++] = p->src[p->pos++];
            if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-'))
                buf[i++] = p->src[p->pos++];
            while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos]) && i < 127)
                buf[i++] = p->src[p->pos++];
        }
        buf[i] = '\0';
        if (is_float) return xsdb_value_float(strtod(buf, NULL));
        return xsdb_value_int(strtoll(buf, NULL, 10));
    }

    /* Bare identifier (treated as string) */
    char buf[XSDB_IDENT_MAX];
    if (sql_read_ident(p, buf, sizeof buf)) {
        /* Check for TRUE/FALSE */
        if (strcasecmp(buf, "TRUE") == 0) return xsdb_value_int(1);
        if (strcasecmp(buf, "FALSE") == 0) return xsdb_value_int(0);
        return xsdb_value_text(buf);
    }

    return xsdb_value_null();
}

/* Parse WHERE clause */
static XSDBWhere *sql_parse_where(SQLParser *p) {
    if (!sql_match_kw(p, "WHERE")) return NULL;

    XSDBWhere *head = NULL;
    XSDBWhere *tail = NULL;

    for (;;) {
        XSDBWhere *w = xsdb_alloc(sizeof(XSDBWhere));

        /* Read column name */
        if (!sql_read_ident(p, w->col, sizeof w->col)) {
            free(w);
            break;
        }

        sql_skip_ws(p);

        /* IS NULL / IS NOT NULL */
        if (sql_match_kw(p, "IS")) {
            if (sql_match_kw(p, "NOT")) {
                sql_match_kw(p, "NULL");
                w->op = XSDB_CMP_NOT_NULL;
                w->val = xsdb_value_null();
            } else {
                sql_match_kw(p, "NULL");
                w->op = XSDB_CMP_IS_NULL;
                w->val = xsdb_value_null();
            }
        } else if (sql_match_kw(p, "LIKE")) {
            w->op = XSDB_CMP_LIKE;
            w->val = sql_read_value(p);
        } else {
            /* Read operator */
            sql_skip_ws(p);
            if (p->pos + 1 < p->len && p->src[p->pos] == '<' && p->src[p->pos + 1] == '=') {
                w->op = XSDB_CMP_LE; p->pos += 2;
            } else if (p->pos + 1 < p->len && p->src[p->pos] == '>' && p->src[p->pos + 1] == '=') {
                w->op = XSDB_CMP_GE; p->pos += 2;
            } else if (p->pos + 1 < p->len && p->src[p->pos] == '!' && p->src[p->pos + 1] == '=') {
                w->op = XSDB_CMP_NE; p->pos += 2;
            } else if (p->pos + 1 < p->len && p->src[p->pos] == '<' && p->src[p->pos + 1] == '>') {
                w->op = XSDB_CMP_NE; p->pos += 2;
            } else if (p->src[p->pos] == '<') {
                w->op = XSDB_CMP_LT; p->pos++;
            } else if (p->src[p->pos] == '>') {
                w->op = XSDB_CMP_GT; p->pos++;
            } else if (p->src[p->pos] == '=') {
                w->op = XSDB_CMP_EQ; p->pos++;
            } else {
                free(w);
                break;
            }

            w->val = sql_read_value(p);
        }

        w->logic = XSDB_LOGIC_NONE;
        w->next = NULL;

        if (!head) { head = w; tail = w; }
        else { tail->next = w; tail = w; }

        /* Check for AND/OR */
        sql_skip_ws(p);
        if (sql_match_kw(p, "AND")) {
            w->logic = XSDB_LOGIC_AND;
        } else if (sql_match_kw(p, "OR")) {
            w->logic = XSDB_LOGIC_OR;
        } else {
            break;
        }
    }

    return head;
}

/* Parse ORDER BY clause */
static XSDBOrderBy *sql_parse_order_by(SQLParser *p, int *count) {
    *count = 0;
    if (!sql_match_kw(p, "ORDER")) return NULL;
    if (!sql_match_kw(p, "BY")) return NULL;

    int cap = 4;
    XSDBOrderBy *orders = xsdb_alloc(sizeof(XSDBOrderBy) * cap);

    for (;;) {
        if (*count >= cap) {
            cap *= 2;
            orders = realloc(orders, sizeof(XSDBOrderBy) * cap);
        }

        if (!sql_read_ident(p, orders[*count].col, sizeof orders[*count].col))
            break;

        orders[*count].dir = XSDB_ORDER_ASC;
        sql_skip_ws(p);
        if (sql_match_kw(p, "DESC")) {
            orders[*count].dir = XSDB_ORDER_DESC;
        } else {
            sql_match_kw(p, "ASC"); /* optional */
        }

        (*count)++;

        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
        } else {
            break;
        }
    }

    return orders;
}

/* Parse LIMIT/OFFSET clause */
static XSDBLimit sql_parse_limit(SQLParser *p) {
    XSDBLimit lim = {0, 0, 0, 0};

    if (sql_match_kw(p, "LIMIT")) {
        sql_skip_ws(p);
        char buf[64];
        int i = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos]) && i < 63) {
            buf[i++] = p->src[p->pos++];
        }
        buf[i] = '\0';
        lim.has_limit = 1;
        lim.limit = strtoll(buf, NULL, 10);
    }

    if (sql_match_kw(p, "OFFSET")) {
        sql_skip_ws(p);
        char buf[64];
        int i = 0;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos]) && i < 63) {
            buf[i++] = p->src[p->pos++];
        }
        buf[i] = '\0';
        lim.has_offset = 1;
        lim.offset = strtoll(buf, NULL, 10);
    }

    return lim;
}

/* Free a WHERE chain */
static void where_free(XSDBWhere *w) {
    while (w) {
        XSDBWhere *next = w->next;
        xsdb_value_free(&w->val);
        free(w);
        w = next;
    }
}

/* Free a SET chain */
static void set_free(XSDBSet *s) {
    while (s) {
        XSDBSet *next = s->next;
        xsdb_value_free(&s->val);
        free(s);
        s = next;
    }
}

void xsdb_stmt_free(XSDBStmt *stmt) {
    if (!stmt) return;

    where_free(stmt->where);
    set_free(stmt->set_list);

    if (stmt->order_by) free(stmt->order_by);

    if (stmt->create_cols) {
        for (int i = 0; i < stmt->ncreate_cols; i++) {
            if (stmt->create_cols[i].has_default)
                xsdb_value_free(&stmt->create_cols[i].default_val);
        }
        free(stmt->create_cols);
    }

    if (stmt->insert_rows) {
        for (int i = 0; i < stmt->ninsert_rows; i++) {
            if (stmt->insert_rows[i]) {
                for (int j = 0; j < stmt->ninsert_cols; j++) {
                    xsdb_value_free(&stmt->insert_rows[i][j]);
                }
                free(stmt->insert_rows[i]);
            }
        }
        free(stmt->insert_rows);
    }

    if (stmt->insert_col_names) {
        for (int i = 0; i < stmt->ninsert_col_names; i++)
            free(stmt->insert_col_names[i]);
        free(stmt->insert_col_names);
    }

    if (stmt->select_cols) {
        for (int i = 0; i < stmt->nselect_cols; i++)
            free(stmt->select_cols[i]);
        free(stmt->select_cols);
    }

    free(stmt);
}

/* Parse CREATE TABLE */
static XSDBStmt *sql_parse_create(SQLParser *p) {
    XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
    s->type = XSDB_SQL_CREATE_TABLE;

    /* Optional: IF NOT EXISTS */
    sql_match_kw(p, "IF");
    sql_match_kw(p, "NOT");
    sql_match_kw(p, "EXISTS");

    if (!sql_read_ident(p, s->table, sizeof s->table)) {
        snprintf(p->error, sizeof p->error, "expected table name");
        xsdb_stmt_free(s);
        return NULL;
    }

    sql_skip_ws(p);
    if (sql_peek(p) != '(') {
        /* Simple CREATE TABLE name (no columns, will use dynamic schema) */
        return s;
    }
    p->pos++; /* skip '(' */

    int cap = 8;
    s->create_cols = xsdb_alloc(sizeof(XSDBColumn) * cap);
    s->ncreate_cols = 0;

    while (sql_peek(p) && sql_peek(p) != ')') {
        if (s->ncreate_cols >= cap) {
            cap *= 2;
            s->create_cols = realloc(s->create_cols, sizeof(XSDBColumn) * cap);
        }

        XSDBColumn *col = &s->create_cols[s->ncreate_cols];
        memset(col, 0, sizeof(XSDBColumn));

        if (!sql_read_ident(p, col->name, sizeof col->name)) break;

        /* Optional type */
        sql_skip_ws(p);
        col->type = XSDB_TYPE_TEXT; /* default */
        if (sql_match_kw(p, "INTEGER") || sql_match_kw(p, "INT") || sql_match_kw(p, "BIGINT")) {
            col->type = XSDB_TYPE_INT;
        } else if (sql_match_kw(p, "REAL") || sql_match_kw(p, "FLOAT") || sql_match_kw(p, "DOUBLE")) {
            col->type = XSDB_TYPE_FLOAT;
        } else if (sql_match_kw(p, "TEXT") || sql_match_kw(p, "VARCHAR") || sql_match_kw(p, "STRING")) {
            col->type = XSDB_TYPE_TEXT;
            /* Skip optional (size) */
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == '(') {
                while (p->pos < p->len && p->src[p->pos] != ')') p->pos++;
                if (p->pos < p->len) p->pos++;
            }
        } else if (sql_match_kw(p, "BLOB")) {
            col->type = XSDB_TYPE_BLOB;
        }

        /* Optional constraints */
        for (;;) {
            sql_skip_ws(p);
            if (sql_match_kw(p, "PRIMARY")) {
                sql_match_kw(p, "KEY");
                col->primary_key = 1;
            } else if (sql_match_kw(p, "NOT")) {
                sql_match_kw(p, "NULL");
                col->not_null = 1;
            } else if (sql_match_kw(p, "UNIQUE")) {
                col->unique = 1;
            } else if (sql_match_kw(p, "DEFAULT")) {
                col->has_default = 1;
                col->default_val = sql_read_value(p);
            } else if (sql_match_kw(p, "AUTOINCREMENT") || sql_match_kw(p, "AUTO_INCREMENT")) {
                /* treated as primary key */
                col->primary_key = 1;
            } else {
                break;
            }
        }

        s->ncreate_cols++;

        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
        }
    }

    if (p->pos < p->len && p->src[p->pos] == ')') p->pos++;
    return s;
}

/* Parse INSERT */
static XSDBStmt *sql_parse_insert(SQLParser *p) {
    XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
    s->type = XSDB_SQL_INSERT;

    if (!sql_match_kw(p, "INTO")) {
        snprintf(p->error, sizeof p->error, "expected INTO after INSERT");
        xsdb_stmt_free(s);
        return NULL;
    }

    if (!sql_read_ident(p, s->table, sizeof s->table)) {
        snprintf(p->error, sizeof p->error, "expected table name");
        xsdb_stmt_free(s);
        return NULL;
    }

    sql_skip_ws(p);

    /* Optional column list: INSERT INTO t (col1, col2) VALUES ... */
    if (sql_peek(p) == '(' && !sql_match_kw(p, "VALUES")) {
        /* Check if this is a column list, not VALUES */
        int save_pos = p->pos;
        p->pos++; /* skip ( */

        /* Try to read column names */
        int cap = 8;
        s->insert_col_names = xsdb_alloc(sizeof(char *) * cap);
        s->ninsert_col_names = 0;

        char buf[XSDB_IDENT_MAX];
        while (sql_peek(p) != ')' && sql_read_ident(p, buf, sizeof buf)) {
            if (s->ninsert_col_names >= cap) {
                cap *= 2;
                s->insert_col_names = realloc(s->insert_col_names, sizeof(char *) * cap);
            }
            s->insert_col_names[s->ninsert_col_names++] = xsdb_strdup(buf);
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        }

        if (sql_peek(p) == ')') p->pos++;

        /* If we didn't get any identifiers, this was actually VALUES (...) */
        if (s->ninsert_col_names == 0) {
            p->pos = save_pos;
            free(s->insert_col_names);
            s->insert_col_names = NULL;
        }
    }

    if (!sql_match_kw(p, "VALUES")) {
        snprintf(p->error, sizeof p->error, "expected VALUES");
        xsdb_stmt_free(s);
        return NULL;
    }

    /* Parse value rows: VALUES (v1, v2), (v3, v4), ... */
    int row_cap = 4;
    s->insert_rows = xsdb_alloc(sizeof(XSDBValue *) * row_cap);
    s->ninsert_rows = 0;

    while (sql_peek(p) == '(') {
        p->pos++; /* skip ( */

        int val_cap = 8;
        XSDBValue *vals = xsdb_alloc(sizeof(XSDBValue) * val_cap);
        int nvals = 0;

        while (sql_peek(p) && sql_peek(p) != ')') {
            if (nvals >= val_cap) {
                val_cap *= 2;
                vals = realloc(vals, sizeof(XSDBValue) * val_cap);
            }
            vals[nvals++] = sql_read_value(p);
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        }

        if (sql_peek(p) == ')') p->pos++;

        if (s->ninsert_rows >= row_cap) {
            row_cap *= 2;
            s->insert_rows = realloc(s->insert_rows, sizeof(XSDBValue *) * row_cap);
        }
        s->insert_rows[s->ninsert_rows++] = vals;

        if (s->ninsert_cols == 0) s->ninsert_cols = nvals;

        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
    }

    return s;
}

/* Parse SELECT */
static XSDBStmt *sql_parse_select(SQLParser *p) {
    XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
    s->type = XSDB_SQL_SELECT;

    sql_skip_ws(p);

    /* Check for COUNT(*) */
    int save = p->pos;
    if (sql_match_kw(p, "COUNT")) {
        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '(') {
            p->pos++;
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == '*') p->pos++;
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ')') p->pos++;
            s->select_count = 1;
            goto select_from;
        }
        p->pos = save;
    }

    /* SELECT * or column list */
    if (p->pos < p->len && p->src[p->pos] == '*') {
        s->select_all = 1;
        p->pos++;
    } else {
        int cap = 8;
        s->select_cols = xsdb_alloc(sizeof(char *) * cap);
        s->nselect_cols = 0;

        char buf[XSDB_IDENT_MAX];
        while (sql_read_ident(p, buf, sizeof buf)) {
            if (strcasecmp(buf, "FROM") == 0) {
                /* Oops, we read "FROM" as a column name. Rewind. */
                p->pos -= 4;
                break;
            }
            if (s->nselect_cols >= cap) {
                cap *= 2;
                s->select_cols = realloc(s->select_cols, sizeof(char *) * cap);
            }
            s->select_cols[s->nselect_cols++] = xsdb_strdup(buf);
            sql_skip_ws(p);
            if (p->pos < p->len && p->src[p->pos] == ',') {
                p->pos++;
            } else {
                break;
            }
        }
    }

select_from:
    if (!sql_match_kw(p, "FROM")) {
        snprintf(p->error, sizeof p->error, "expected FROM");
        xsdb_stmt_free(s);
        return NULL;
    }

    if (!sql_read_ident(p, s->table, sizeof s->table)) {
        snprintf(p->error, sizeof p->error, "expected table name");
        xsdb_stmt_free(s);
        return NULL;
    }

    s->where = sql_parse_where(p);
    s->order_by = sql_parse_order_by(p, &s->norder_by);
    s->limit = sql_parse_limit(p);

    return s;
}

/* Parse UPDATE */
static XSDBStmt *sql_parse_update(SQLParser *p) {
    XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
    s->type = XSDB_SQL_UPDATE;

    if (!sql_read_ident(p, s->table, sizeof s->table)) {
        snprintf(p->error, sizeof p->error, "expected table name");
        xsdb_stmt_free(s);
        return NULL;
    }

    if (!sql_match_kw(p, "SET")) {
        snprintf(p->error, sizeof p->error, "expected SET");
        xsdb_stmt_free(s);
        return NULL;
    }

    XSDBSet *head = NULL, *tail = NULL;
    for (;;) {
        XSDBSet *set = xsdb_alloc(sizeof(XSDBSet));
        set->next = NULL;

        if (!sql_read_ident(p, set->col, sizeof set->col)) {
            free(set);
            break;
        }

        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '=') p->pos++;

        set->val = sql_read_value(p);

        if (!head) { head = set; tail = set; }
        else { tail->next = set; tail = set; }

        sql_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
        } else {
            break;
        }
    }
    s->set_list = head;

    s->where = sql_parse_where(p);
    return s;
}

/* Parse DELETE */
static XSDBStmt *sql_parse_delete(SQLParser *p) {
    XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
    s->type = XSDB_SQL_DELETE;

    if (!sql_match_kw(p, "FROM")) {
        snprintf(p->error, sizeof p->error, "expected FROM after DELETE");
        xsdb_stmt_free(s);
        return NULL;
    }

    if (!sql_read_ident(p, s->table, sizeof s->table)) {
        snprintf(p->error, sizeof p->error, "expected table name");
        xsdb_stmt_free(s);
        return NULL;
    }

    s->where = sql_parse_where(p);
    return s;
}

/* Main SQL parser entry point */
XSDBStmt *xsdb_parse_sql(const char *sql) {
    SQLParser p;
    sql_init(&p, sql);

    sql_skip_ws(&p);

    if (sql_match_kw(&p, "CREATE")) {
        if (sql_match_kw(&p, "TABLE")) {
            return sql_parse_create(&p);
        }
        if (sql_match_kw(&p, "INDEX") || sql_match_kw(&p, "UNIQUE")) {
            int unique = 0;
            if (p.src[p.pos - 6] == 'U' || p.src[p.pos - 6] == 'u') {
                unique = 1;
                sql_match_kw(&p, "INDEX");
            }
            XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
            s->type = XSDB_SQL_CREATE_INDEX;
            s->index_unique = unique;
            if (!sql_read_ident(&p, s->index_name, sizeof s->index_name)) {
                xsdb_stmt_free(s);
                return NULL;
            }
            sql_match_kw(&p, "ON");
            if (!sql_read_ident(&p, s->table, sizeof s->table)) {
                xsdb_stmt_free(s);
                return NULL;
            }
            sql_skip_ws(&p);
            if (p.pos < p.len && p.src[p.pos] == '(') p.pos++;
            sql_read_ident(&p, s->index_col, sizeof s->index_col);
            sql_skip_ws(&p);
            if (p.pos < p.len && p.src[p.pos] == ')') p.pos++;
            return s;
        }
        return NULL;
    }

    if (sql_match_kw(&p, "DROP")) {
        if (sql_match_kw(&p, "TABLE")) {
            XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
            s->type = XSDB_SQL_DROP_TABLE;
            sql_match_kw(&p, "IF");
            sql_match_kw(&p, "EXISTS");
            if (!sql_read_ident(&p, s->table, sizeof s->table)) {
                xsdb_stmt_free(s);
                return NULL;
            }
            return s;
        }
        return NULL;
    }

    if (sql_match_kw(&p, "INSERT")) return sql_parse_insert(&p);
    if (sql_match_kw(&p, "SELECT")) return sql_parse_select(&p);
    if (sql_match_kw(&p, "UPDATE")) return sql_parse_update(&p);
    if (sql_match_kw(&p, "DELETE")) return sql_parse_delete(&p);

    if (sql_match_kw(&p, "BEGIN")) {
        XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
        s->type = XSDB_SQL_BEGIN;
        return s;
    }
    if (sql_match_kw(&p, "COMMIT")) {
        XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
        s->type = XSDB_SQL_COMMIT;
        return s;
    }
    if (sql_match_kw(&p, "ROLLBACK")) {
        XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
        s->type = XSDB_SQL_ROLLBACK;
        return s;
    }

    if (sql_match_kw(&p, "ALTER")) {
        if (sql_match_kw(&p, "TABLE")) {
            XSDBStmt *s = xsdb_alloc(sizeof(XSDBStmt));
            s->type = XSDB_SQL_ALTER_TABLE;
            if (!sql_read_ident(&p, s->table, sizeof s->table)) {
                xsdb_stmt_free(s);
                return NULL;
            }
            if (sql_match_kw(&p, "ADD")) {
                sql_match_kw(&p, "COLUMN");
                s->alter_add = 1;
                sql_read_ident(&p, s->alter_col, sizeof s->alter_col);
                /* Optional type */
                if (sql_match_kw(&p, "INTEGER") || sql_match_kw(&p, "INT"))
                    s->alter_type = XSDB_TYPE_INT;
                else if (sql_match_kw(&p, "REAL") || sql_match_kw(&p, "FLOAT"))
                    s->alter_type = XSDB_TYPE_FLOAT;
                else if (sql_match_kw(&p, "TEXT"))
                    s->alter_type = XSDB_TYPE_TEXT;
                else
                    s->alter_type = XSDB_TYPE_TEXT;
            } else if (sql_match_kw(&p, "DROP")) {
                sql_match_kw(&p, "COLUMN");
                s->alter_add = 0;
                sql_read_ident(&p, s->alter_col, sizeof s->alter_col);
            }
            return s;
        }
        return NULL;
    }

    return NULL;
}

/* ----------------------------------------------------------------
 * Table operations
 * ---------------------------------------------------------------- */

static XSDBTable *db_find_table(XSDB *db, const char *name) {
    for (int i = 0; i < db->ntables; i++) {
        if (strcasecmp(db->tables[i].name, name) == 0) {
            return &db->tables[i];
        }
    }
    return NULL;
}

static int db_col_index(XSDBTable *tbl, const char *name) {
    for (int i = 0; i < tbl->ncols; i++) {
        if (strcasecmp(tbl->columns[i].name, name) == 0) return i;
    }
    return -1;
}

/* Check if a row matches a WHERE clause */
static int row_matches(XSDBTable *tbl, XSDBRow *row, XSDBWhere *where) {
    if (!where) return 1;

    int result = 1;
    XSDBLogicOp pending_op = XSDB_LOGIC_NONE;

    for (XSDBWhere *w = where; w; w = w->next) {
        int col_idx = db_col_index(tbl, w->col);
        int match;

        if (col_idx < 0 || col_idx >= row->ncells) {
            match = 0;
        } else {
            match = xsdb_cmp_check(&row->cells[col_idx], w->op, &w->val);
        }

        if (pending_op == XSDB_LOGIC_AND) {
            result = result && match;
        } else if (pending_op == XSDB_LOGIC_OR) {
            result = result || match;
        } else {
            result = match;
        }

        pending_op = w->logic;
    }

    return result;
}

/* Collect all rows from B-tree that match WHERE */
typedef struct {
    XSDBTable  *tbl;
    XSDBWhere  *where;
    XSDBRow   **out;
    int         nout;
    int         cap;
} ScanCtx;

static void scan_collect(XSDBRow *row, void *ctx) {
    ScanCtx *sc = ctx;
    if (!row_matches(sc->tbl, row, sc->where)) return;
    if (sc->nout >= sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 32;
        sc->out = realloc(sc->out, sizeof(XSDBRow *) * sc->cap);
    }
    sc->out[sc->nout++] = row;
}

/* Sort rows for ORDER BY */
typedef struct {
    XSDBTable   *tbl;
    XSDBOrderBy *orders;
    int          norders;
} SortCtx;

static SortCtx *g_sort_ctx; /* not thread safe, but fine for our use */

static int row_sort_cmp(const void *a, const void *b) {
    XSDBRow *ra = *(XSDBRow **)a;
    XSDBRow *rb = *(XSDBRow **)b;
    SortCtx *sc = g_sort_ctx;

    for (int i = 0; i < sc->norders; i++) {
        int ci = db_col_index(sc->tbl, sc->orders[i].col);
        if (ci < 0) continue;

        XSDBValue *va = (ci < ra->ncells) ? &ra->cells[ci] : NULL;
        XSDBValue *vb = (ci < rb->ncells) ? &rb->cells[ci] : NULL;

        XSDBValue null_val = {XSDB_TYPE_NULL, {.ival = 0}};
        if (!va) va = &null_val;
        if (!vb) vb = &null_val;

        int cmp = xsdb_value_compare(va, vb);
        if (cmp != 0) {
            return (sc->orders[i].dir == XSDB_ORDER_DESC) ? -cmp : cmp;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * WAL (Write-Ahead Log) operations
 * ---------------------------------------------------------------- */

static void wal_write_record(XSDB *db, uint8_t type, const char *table,
                              const void *data, uint32_t data_len)
{
    if (!db->wal_fp) return;

    XSDBWalRecord rec;
    rec.type = type;
    rec.table_len = table ? (uint32_t)strlen(table) : 0;
    rec.data_len = data_len;

    fwrite(&rec, sizeof rec, 1, db->wal_fp);
    if (rec.table_len > 0) fwrite(table, rec.table_len, 1, db->wal_fp);
    if (data_len > 0 && data) fwrite(data, data_len, 1, db->wal_fp);
    fflush(db->wal_fp);
}

static void wal_open(XSDB *db) {
    if (!db->path) return; /* in-memory db, no WAL */
    size_t plen = strlen(db->path);
    db->wal_path = xsdb_alloc(plen + 5);
    snprintf(db->wal_path, plen + 5, "%s-wal", db->path);
    db->wal_fp = fopen(db->wal_path, "ab");
}

static void wal_close(XSDB *db) {
    if (db->wal_fp) {
        fclose(db->wal_fp);
        db->wal_fp = NULL;
    }
}

static void wal_truncate(XSDB *db) {
    wal_close(db);
    if (db->wal_path) {
        remove(db->wal_path);
        db->wal_fp = fopen(db->wal_path, "ab");
    }
}

/* ----------------------------------------------------------------
 * Snapshot operations (for transactions)
 * ---------------------------------------------------------------- */

/* Deep copy of a B-tree */
static XSDBBTreeNode *btree_deep_copy(XSDBBTreeNode *node) {
    if (!node) return NULL;

    XSDBBTreeNode *copy = xsdb_btree_new_node(node->is_leaf);
    copy->nkeys = node->nkeys;

    for (int i = 0; i < node->nkeys; i++) {
        copy->keys[i] = node->keys[i];
        copy->rows[i] = row_copy(node->rows[i]);
    }

    if (!node->is_leaf) {
        for (int i = 0; i <= node->nkeys; i++) {
            copy->children[i] = btree_deep_copy(node->children[i]);
        }
    }

    return copy;
}

static XSDBSnapshot *snapshot_create(XSDB *db) {
    XSDBSnapshot *snap = xsdb_alloc(sizeof(XSDBSnapshot));
    snap->ntables = db->ntables;
    snap->tables = xsdb_alloc(sizeof(XSDBTable) * db->ntables);

    for (int i = 0; i < db->ntables; i++) {
        XSDBTable *src = &db->tables[i];
        XSDBTable *dst = &snap->tables[i];

        strncpy(dst->name, src->name, sizeof(dst->name) - 1);
        dst->ncols = src->ncols;
        dst->next_rowid = src->next_rowid;
        dst->row_count = src->row_count;
        dst->indexes = NULL; /* indexes not snapshotted */

        dst->columns = xsdb_alloc(sizeof(XSDBColumn) * src->ncols);
        for (int j = 0; j < src->ncols; j++) {
            dst->columns[j] = src->columns[j];
            if (src->columns[j].has_default && src->columns[j].default_val.type == XSDB_TYPE_TEXT)
                dst->columns[j].default_val.text = xsdb_strdup(src->columns[j].default_val.text);
        }

        dst->btree = btree_deep_copy(src->btree);
    }

    return snap;
}

static void snapshot_restore(XSDB *db, XSDBSnapshot *snap) {
    /* Free current tables */
    for (int i = 0; i < db->ntables; i++) {
        xsdb_btree_free(db->tables[i].btree);
        free(db->tables[i].columns);
    }
    free(db->tables);

    /* Restore from snapshot */
    db->tables = snap->tables;
    db->ntables = snap->ntables;
    db->table_cap = snap->ntables;

    /* Don't free snap->tables since we transferred ownership */
    snap->tables = NULL;
    snap->ntables = 0;
}

static void snapshot_free(XSDBSnapshot *snap) {
    if (!snap) return;
    if (snap->tables) {
        for (int i = 0; i < snap->ntables; i++) {
            xsdb_btree_free(snap->tables[i].btree);
            free(snap->tables[i].columns);
        }
        free(snap->tables);
    }
    free(snap);
}

/* ----------------------------------------------------------------
 * Database lifecycle
 * ---------------------------------------------------------------- */

XSDB *xsdb_open(const char *path) {
    XSDB *db = xsdb_alloc(sizeof(XSDB));
    db->path = path ? xsdb_strdup(path) : NULL;
    db->table_cap = 8;
    db->tables = xsdb_alloc(sizeof(XSDBTable) * db->table_cap);
    db->ntables = 0;
    db->in_transaction = 0;
    db->auto_commit = 1;
    db->closed = 0;

    if (path) {
        /* Try loading existing database */
        XSDB *existing = xsdb_load(path);
        if (existing) {
            free(db->tables);
            db->tables = existing->tables;
            db->ntables = existing->ntables;
            db->table_cap = existing->ntables > 8 ? existing->ntables : 8;
            existing->tables = NULL;
            existing->ntables = 0;
            xsdb_close(existing);
        }
        wal_open(db);
    }

    return db;
}

void xsdb_close(XSDB *db) {
    if (!db) return;

    /* Commit pending transaction if any */
    if (db->in_transaction) {
        xsdb_commit(db);
    }

    /* Save to disk if path given */
    if (db->path && !db->closed) {
        xsdb_save(db, db->path);
    }

    wal_close(db);

    for (int i = 0; i < db->ntables; i++) {
        xsdb_btree_free(db->tables[i].btree);
        free(db->tables[i].columns);
        /* Free indexes */
        XSDBIndex *idx = db->tables[i].indexes;
        while (idx) {
            XSDBIndex *next = idx->next;
            xsdb_btree_free(idx->root);
            free(idx);
            idx = next;
        }
    }
    free(db->tables);

    if (db->snapshot) snapshot_free(db->snapshot);
    free(db->path);
    free(db->wal_path);
    db->closed = 1;
    free(db);
}

/* ----------------------------------------------------------------
 * SQL execution
 * ---------------------------------------------------------------- */

static XSDBResult *exec_create_table(XSDB *db, XSDBStmt *stmt) {
    if (db_find_table(db, stmt->table)) {
        return result_error("table '%s' already exists", stmt->table);
    }

    if (db->ntables >= db->table_cap) {
        db->table_cap *= 2;
        db->tables = realloc(db->tables, sizeof(XSDBTable) * db->table_cap);
    }

    XSDBTable *tbl = &db->tables[db->ntables++];
    memset(tbl, 0, sizeof(XSDBTable));
    strncpy(tbl->name, stmt->table, sizeof(tbl->name) - 1);
    tbl->next_rowid = 1;
    tbl->btree = NULL;
    tbl->indexes = NULL;

    if (stmt->ncreate_cols > 0) {
        tbl->ncols = stmt->ncreate_cols;
        tbl->columns = xsdb_alloc(sizeof(XSDBColumn) * tbl->ncols);
        for (int i = 0; i < tbl->ncols; i++) {
            tbl->columns[i] = stmt->create_cols[i];
            if (stmt->create_cols[i].has_default &&
                stmt->create_cols[i].default_val.type == XSDB_TYPE_TEXT) {
                tbl->columns[i].default_val.text =
                    xsdb_strdup(stmt->create_cols[i].default_val.text);
            }
            /* Clear the source so stmt_free doesn't double-free */
            stmt->create_cols[i].has_default = 0;
        }
    }

    wal_write_record(db, XSDB_WAL_CREATE, stmt->table, NULL, 0);

    return result_ok(0);
}

static XSDBResult *exec_drop_table(XSDB *db, XSDBStmt *stmt) {
    int idx = -1;
    for (int i = 0; i < db->ntables; i++) {
        if (strcasecmp(db->tables[i].name, stmt->table) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) return result_error("no such table: %s", stmt->table);

    xsdb_btree_free(db->tables[idx].btree);
    free(db->tables[idx].columns);

    XSDBIndex *ix = db->tables[idx].indexes;
    while (ix) {
        XSDBIndex *next = ix->next;
        xsdb_btree_free(ix->root);
        free(ix);
        ix = next;
    }

    /* Shift remaining tables */
    for (int i = idx; i < db->ntables - 1; i++) {
        db->tables[i] = db->tables[i + 1];
    }
    db->ntables--;

    wal_write_record(db, XSDB_WAL_DROP, stmt->table, NULL, 0);

    return result_ok(0);
}

static XSDBResult *exec_insert(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    int inserted = 0;

    for (int ri = 0; ri < stmt->ninsert_rows; ri++) {
        XSDBValue *vals = stmt->insert_rows[ri];
        XSDBRow *row = row_new(tbl->ncols > 0 ? tbl->ncols : stmt->ninsert_cols);

        if (tbl->ncols == 0 && stmt->ninsert_cols > 0) {
            /* Dynamic schema: create columns from insert data */
            tbl->ncols = stmt->ninsert_cols;
            tbl->columns = xsdb_alloc(sizeof(XSDBColumn) * tbl->ncols);
            for (int j = 0; j < tbl->ncols; j++) {
                snprintf(tbl->columns[j].name, sizeof(tbl->columns[j].name), "c%d", j);
                tbl->columns[j].type = XSDB_TYPE_AUTO;
            }
        }

        row->rowid = tbl->next_rowid++;

        /* Map values to columns */
        if (stmt->insert_col_names && stmt->ninsert_col_names > 0) {
            /* Named column insert */
            for (int j = 0; j < stmt->ninsert_col_names && j < stmt->ninsert_cols; j++) {
                int ci = db_col_index(tbl, stmt->insert_col_names[j]);
                if (ci >= 0 && ci < row->ncells) {
                    row->cells[ci] = xsdb_value_copy(&vals[j]);
                }
            }
            /* Fill defaults for unspecified columns */
            for (int j = 0; j < tbl->ncols; j++) {
                if (row->cells[j].type == XSDB_TYPE_NULL && tbl->columns[j].has_default) {
                    row->cells[j] = xsdb_value_copy(&tbl->columns[j].default_val);
                }
            }
        } else {
            /* Positional insert */
            int ncopy = stmt->ninsert_cols;
            if (ncopy > row->ncells) ncopy = row->ncells;
            for (int j = 0; j < ncopy; j++) {
                row->cells[j] = xsdb_value_copy(&vals[j]);
            }
        }

        /* Check NOT NULL constraints */
        for (int j = 0; j < tbl->ncols; j++) {
            if (tbl->columns[j].not_null && row->cells[j].type == XSDB_TYPE_NULL) {
                row_free(row);
                return result_error("NOT NULL constraint failed: %s.%s",
                                     tbl->name, tbl->columns[j].name);
            }
        }

        /* Check UNIQUE constraints */
        for (int j = 0; j < tbl->ncols; j++) {
            if (!tbl->columns[j].unique && !tbl->columns[j].primary_key) continue;
            if (row->cells[j].type == XSDB_TYPE_NULL) continue;

            /* Scan existing rows for duplicates */
            ScanCtx sc = {tbl, NULL, NULL, 0, 0};
            xsdb_btree_scan(tbl->btree, scan_collect, &sc);
            int dup = 0;
            for (int k = 0; k < sc.nout; k++) {
                if (j < sc.out[k]->ncells &&
                    xsdb_value_compare(&row->cells[j], &sc.out[k]->cells[j]) == 0) {
                    dup = 1;
                    break;
                }
            }
            free(sc.out);
            if (dup) {
                row_free(row);
                return result_error("UNIQUE constraint failed: %s.%s",
                                     tbl->name, tbl->columns[j].name);
            }
        }

        xsdb_btree_insert(&tbl->btree, row->rowid, row);
        tbl->row_count++;
        inserted++;
    }

    wal_write_record(db, XSDB_WAL_INSERT, stmt->table, NULL, 0);

    return result_ok(inserted);
}

static XSDBResult *exec_select(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    /* Collect matching rows */
    ScanCtx sc = {tbl, stmt->where, NULL, 0, 0};
    xsdb_btree_scan(tbl->btree, scan_collect, &sc);

    /* COUNT(*) */
    if (stmt->select_count) {
        XSDBResult *r = result_new();
        char *col_name = "count";
        result_set_columns(r, &col_name, 1);
        XSDBRow count_row;
        count_row.rowid = 0;
        count_row.ncells = 1;
        count_row.cells = xsdb_alloc(sizeof(XSDBValue));
        count_row.cells[0] = xsdb_value_int(sc.nout);
        result_add_row(r, &count_row);
        free(sc.out);
        return r;
    }

    /* Apply ORDER BY */
    if (stmt->norder_by > 0 && sc.nout > 1) {
        SortCtx sort_ctx = {tbl, stmt->order_by, stmt->norder_by};
        g_sort_ctx = &sort_ctx;
        qsort(sc.out, sc.nout, sizeof(XSDBRow *), row_sort_cmp);
    }

    /* Apply OFFSET */
    int start = 0;
    if (stmt->limit.has_offset) {
        start = (int)stmt->limit.offset;
        if (start > sc.nout) start = sc.nout;
    }

    /* Apply LIMIT */
    int end = sc.nout;
    if (stmt->limit.has_limit) {
        end = start + (int)stmt->limit.limit;
        if (end > sc.nout) end = sc.nout;
    }

    /* Determine output columns */
    int ncols_out;
    int *col_indices;

    if (stmt->select_all || stmt->nselect_cols == 0) {
        ncols_out = tbl->ncols;
        col_indices = xsdb_alloc(sizeof(int) * ncols_out);
        for (int i = 0; i < ncols_out; i++) col_indices[i] = i;
    } else {
        ncols_out = stmt->nselect_cols;
        col_indices = xsdb_alloc(sizeof(int) * ncols_out);
        for (int i = 0; i < ncols_out; i++) {
            col_indices[i] = db_col_index(tbl, stmt->select_cols[i]);
        }
    }

    /* Build result */
    XSDBResult *r = result_new();
    char **col_names = xsdb_alloc(sizeof(char *) * ncols_out);
    for (int i = 0; i < ncols_out; i++) {
        if (col_indices[i] >= 0 && col_indices[i] < tbl->ncols) {
            col_names[i] = tbl->columns[col_indices[i]].name;
        } else if (stmt->select_cols && i < stmt->nselect_cols) {
            col_names[i] = stmt->select_cols[i];
        } else {
            col_names[i] = "?";
        }
    }
    result_set_columns(r, col_names, ncols_out);
    free(col_names);

    for (int i = start; i < end; i++) {
        XSDBRow out_row;
        out_row.rowid = sc.out[i]->rowid;
        out_row.ncells = ncols_out;
        out_row.cells = xsdb_alloc(sizeof(XSDBValue) * ncols_out);
        for (int j = 0; j < ncols_out; j++) {
            int ci = col_indices[j];
            if (ci >= 0 && ci < sc.out[i]->ncells) {
                out_row.cells[j] = xsdb_value_copy(&sc.out[i]->cells[ci]);
            } else {
                out_row.cells[j] = xsdb_value_null();
            }
        }
        result_add_row(r, &out_row);
    }

    free(col_indices);
    free(sc.out);
    return r;
}

static XSDBResult *exec_update(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    ScanCtx sc = {tbl, stmt->where, NULL, 0, 0};
    xsdb_btree_scan(tbl->btree, scan_collect, &sc);

    int updated = 0;
    for (int i = 0; i < sc.nout; i++) {
        XSDBRow *row = sc.out[i];
        for (XSDBSet *s = stmt->set_list; s; s = s->next) {
            int ci = db_col_index(tbl, s->col);
            if (ci >= 0 && ci < row->ncells) {
                xsdb_value_free(&row->cells[ci]);
                row->cells[ci] = xsdb_value_copy(&s->val);
            }
        }
        updated++;
    }

    free(sc.out);
    wal_write_record(db, XSDB_WAL_UPDATE, stmt->table, NULL, 0);

    return result_ok(updated);
}

static XSDBResult *exec_delete(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    if (!stmt->where) {
        /* Delete all rows */
        int count = (int)tbl->row_count;
        xsdb_btree_free(tbl->btree);
        tbl->btree = NULL;
        tbl->row_count = 0;
        wal_write_record(db, XSDB_WAL_DELETE, stmt->table, NULL, 0);
        return result_ok(count);
    }

    /* Collect matching rows, then delete by rowid */
    ScanCtx sc = {tbl, stmt->where, NULL, 0, 0};
    xsdb_btree_scan(tbl->btree, scan_collect, &sc);

    /* Collect rowids to delete (can't modify tree during scan) */
    int64_t *rowids = xsdb_alloc(sizeof(int64_t) * sc.nout);
    for (int i = 0; i < sc.nout; i++) {
        rowids[i] = sc.out[i]->rowid;
    }

    int deleted = 0;
    for (int i = 0; i < sc.nout; i++) {
        if (xsdb_btree_delete(&tbl->btree, rowids[i]) == 0) {
            tbl->row_count--;
            deleted++;
        }
    }

    free(rowids);
    free(sc.out);
    wal_write_record(db, XSDB_WAL_DELETE, stmt->table, NULL, 0);

    return result_ok(deleted);
}

static XSDBResult *exec_create_index(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    int ci = db_col_index(tbl, stmt->index_col);
    if (ci < 0) return result_error("no such column: %s", stmt->index_col);

    XSDBIndex *idx = xsdb_alloc(sizeof(XSDBIndex));
    strncpy(idx->name, stmt->index_name, sizeof(idx->name) - 1);
    strncpy(idx->col_name, stmt->index_col, sizeof(idx->col_name) - 1);
    idx->col_idx = ci;
    idx->unique = stmt->index_unique;
    idx->root = NULL;
    idx->next = tbl->indexes;
    tbl->indexes = idx;

    /* Build index from existing rows */
    ScanCtx sc = {tbl, NULL, NULL, 0, 0};
    xsdb_btree_scan(tbl->btree, scan_collect, &sc);

    for (int i = 0; i < sc.nout; i++) {
        XSDBRow *ref = row_new(1);
        ref->rowid = sc.out[i]->rowid;
        if (ci < sc.out[i]->ncells) {
            ref->cells[0] = xsdb_value_copy(&sc.out[i]->cells[ci]);
        }
        xsdb_btree_insert(&idx->root, sc.out[i]->rowid, ref);
    }
    free(sc.out);

    return result_ok(0);
}

static XSDBResult *exec_alter_table(XSDB *db, XSDBStmt *stmt) {
    XSDBTable *tbl = db_find_table(db, stmt->table);
    if (!tbl) return result_error("no such table: %s", stmt->table);

    if (stmt->alter_add) {
        /* ADD COLUMN */
        if (db_col_index(tbl, stmt->alter_col) >= 0)
            return result_error("column '%s' already exists", stmt->alter_col);

        tbl->columns = realloc(tbl->columns, sizeof(XSDBColumn) * (tbl->ncols + 1));
        XSDBColumn *col = &tbl->columns[tbl->ncols];
        memset(col, 0, sizeof(XSDBColumn));
        strncpy(col->name, stmt->alter_col, sizeof(col->name) - 1);
        col->type = stmt->alter_type;
        tbl->ncols++;

        /* Expand existing rows */
        ScanCtx sc = {tbl, NULL, NULL, 0, 0};
        xsdb_btree_scan(tbl->btree, scan_collect, &sc);
        for (int i = 0; i < sc.nout; i++) {
            XSDBRow *row = sc.out[i];
            row->cells = realloc(row->cells, sizeof(XSDBValue) * tbl->ncols);
            row->cells[tbl->ncols - 1] = xsdb_value_null();
            row->ncells = tbl->ncols;
        }
        free(sc.out);
    } else {
        /* DROP COLUMN */
        int ci = db_col_index(tbl, stmt->alter_col);
        if (ci < 0) return result_error("no such column: %s", stmt->alter_col);

        /* Remove column from rows */
        ScanCtx sc = {tbl, NULL, NULL, 0, 0};
        xsdb_btree_scan(tbl->btree, scan_collect, &sc);
        for (int i = 0; i < sc.nout; i++) {
            XSDBRow *row = sc.out[i];
            xsdb_value_free(&row->cells[ci]);
            for (int j = ci; j < row->ncells - 1; j++) {
                row->cells[j] = row->cells[j + 1];
            }
            row->ncells--;
        }
        free(sc.out);

        /* Remove column definition */
        for (int j = ci; j < tbl->ncols - 1; j++) {
            tbl->columns[j] = tbl->columns[j + 1];
        }
        tbl->ncols--;
    }

    return result_ok(0);
}

XSDBResult *xsdb_exec(XSDB *db, const char *sql) {
    if (!db) return result_error("null db handle");
    if (db->closed) return result_error("database is closed");
    if (!sql || !*sql) return result_error("empty SQL");

    XSDBStmt *stmt = xsdb_parse_sql(sql);
    if (!stmt) return result_error("failed to parse SQL: %s", sql);

    XSDBResult *r;
    switch (stmt->type) {
    case XSDB_SQL_CREATE_TABLE:
        r = exec_create_table(db, stmt);
        break;
    case XSDB_SQL_DROP_TABLE:
        r = exec_drop_table(db, stmt);
        break;
    case XSDB_SQL_INSERT:
        r = exec_insert(db, stmt);
        break;
    case XSDB_SQL_SELECT:
        r = exec_select(db, stmt);
        break;
    case XSDB_SQL_UPDATE:
        r = exec_update(db, stmt);
        break;
    case XSDB_SQL_DELETE:
        r = exec_delete(db, stmt);
        break;
    case XSDB_SQL_BEGIN:
        xsdb_begin(db);
        r = result_ok(0);
        break;
    case XSDB_SQL_COMMIT:
        xsdb_commit(db);
        r = result_ok(0);
        break;
    case XSDB_SQL_ROLLBACK:
        xsdb_rollback(db);
        r = result_ok(0);
        break;
    case XSDB_SQL_CREATE_INDEX:
        r = exec_create_index(db, stmt);
        break;
    case XSDB_SQL_ALTER_TABLE:
        r = exec_alter_table(db, stmt);
        break;
    default:
        r = result_error("unsupported SQL statement type");
        break;
    }

    xsdb_stmt_free(stmt);
    return r;
}

/* ----------------------------------------------------------------
 * Prepared statements
 * ---------------------------------------------------------------- */

XSDBPrepared *xsdb_prepare(XSDB *db, const char *sql) {
    (void)db;
    XSDBPrepared *p = xsdb_alloc(sizeof(XSDBPrepared));
    p->sql = xsdb_strdup(sql);

    /* Count ? placeholders */
    p->nparam_slots = 0;
    for (const char *c = sql; *c; c++) {
        if (*c == '?') p->nparam_slots++;
    }

    if (p->nparam_slots > 0) {
        p->bound_params = xsdb_alloc(sizeof(XSDBValue) * p->nparam_slots);
        for (int i = 0; i < p->nparam_slots; i++)
            p->bound_params[i] = xsdb_value_null();
    }
    p->nbound = 0;

    return p;
}

int xsdb_bind_int(XSDBPrepared *stmt, int idx, int64_t val) {
    if (idx < 0 || idx >= stmt->nparam_slots) return -1;
    xsdb_value_free(&stmt->bound_params[idx]);
    stmt->bound_params[idx] = xsdb_value_int(val);
    stmt->nbound++;
    return 0;
}

int xsdb_bind_float(XSDBPrepared *stmt, int idx, double val) {
    if (idx < 0 || idx >= stmt->nparam_slots) return -1;
    xsdb_value_free(&stmt->bound_params[idx]);
    stmt->bound_params[idx] = xsdb_value_float(val);
    stmt->nbound++;
    return 0;
}

int xsdb_bind_text(XSDBPrepared *stmt, int idx, const char *val) {
    if (idx < 0 || idx >= stmt->nparam_slots) return -1;
    xsdb_value_free(&stmt->bound_params[idx]);
    stmt->bound_params[idx] = xsdb_value_text(val);
    stmt->nbound++;
    return 0;
}

int xsdb_bind_null(XSDBPrepared *stmt, int idx) {
    if (idx < 0 || idx >= stmt->nparam_slots) return -1;
    xsdb_value_free(&stmt->bound_params[idx]);
    stmt->bound_params[idx] = xsdb_value_null();
    stmt->nbound++;
    return 0;
}

XSDBResult *xsdb_exec_prepared(XSDB *db, XSDBPrepared *stmt) {
    if (!stmt || !stmt->sql) return result_error("invalid prepared statement");

    /* Expand ? placeholders into actual SQL */
    size_t sql_len = strlen(stmt->sql);
    size_t buf_cap = sql_len + stmt->nparam_slots * 256 + 64;
    char *expanded = xsdb_alloc(buf_cap);
    int ei = 0, pi = 0;

    for (int i = 0; stmt->sql[i]; i++) {
        if (stmt->sql[i] == '?' && pi < stmt->nparam_slots) {
            XSDBValue *v = &stmt->bound_params[pi++];
            switch (v->type) {
            case XSDB_TYPE_INT:
                ei += snprintf(expanded + ei, buf_cap - ei, "%lld", (long long)v->ival);
                break;
            case XSDB_TYPE_FLOAT:
                ei += snprintf(expanded + ei, buf_cap - ei, "%g", v->fval);
                break;
            case XSDB_TYPE_TEXT:
                expanded[ei++] = '\'';
                for (const char *c = v->text; *c; c++) {
                    if (*c == '\'') expanded[ei++] = '\'';
                    expanded[ei++] = *c;
                }
                expanded[ei++] = '\'';
                break;
            case XSDB_TYPE_NULL:
            default:
                ei += snprintf(expanded + ei, buf_cap - ei, "NULL");
                break;
            }
        } else {
            expanded[ei++] = stmt->sql[i];
        }
    }
    expanded[ei] = '\0';

    XSDBResult *r = xsdb_exec(db, expanded);
    free(expanded);
    return r;
}

void xsdb_prepared_free(XSDBPrepared *stmt) {
    if (!stmt) return;
    free(stmt->sql);
    if (stmt->bound_params) {
        for (int i = 0; i < stmt->nparam_slots; i++)
            xsdb_value_free(&stmt->bound_params[i]);
        free(stmt->bound_params);
    }
    free(stmt);
}

/* ----------------------------------------------------------------
 * Transaction control
 * ---------------------------------------------------------------- */

int xsdb_begin(XSDB *db) {
    if (db->in_transaction) return -1; /* already in transaction */

    db->snapshot = snapshot_create(db);
    db->in_transaction = 1;
    wal_write_record(db, XSDB_WAL_BEGIN, NULL, NULL, 0);
    return 0;
}

int xsdb_commit(XSDB *db) {
    if (!db->in_transaction) return -1;

    snapshot_free(db->snapshot);
    db->snapshot = NULL;
    db->in_transaction = 0;
    wal_write_record(db, XSDB_WAL_COMMIT, NULL, NULL, 0);
    wal_truncate(db);

    if (db->path) {
        xsdb_save(db, db->path);
    }

    return 0;
}

int xsdb_rollback(XSDB *db) {
    if (!db->in_transaction) return -1;

    if (db->snapshot) {
        snapshot_restore(db, db->snapshot);
        snapshot_free(db->snapshot);
        db->snapshot = NULL;
    }
    db->in_transaction = 0;
    wal_truncate(db);

    return 0;
}

/* ----------------------------------------------------------------
 * Table introspection
 * ---------------------------------------------------------------- */

int xsdb_table_count(XSDB *db) {
    return db ? db->ntables : 0;
}

const char *xsdb_table_name(XSDB *db, int idx) {
    if (!db || idx < 0 || idx >= db->ntables) return NULL;
    return db->tables[idx].name;
}

int xsdb_table_exists(XSDB *db, const char *name) {
    return db_find_table(db, name) != NULL;
}

/* ----------------------------------------------------------------
 * Persistence (binary format)
 * ---------------------------------------------------------------- */

/*
 * File format:
 *   Header: magic(4) version(4) ntables(4)
 *   Per table:
 *     name_len(4) name(name_len)
 *     ncols(4)
 *     Per column: name_len(4) name(name_len) type(4)
 *     next_rowid(8) row_count(8)
 *     Per row: rowid(8) ncells(4)
 *       Per cell: type(4) data_len(4) data(data_len)
 */

typedef struct {
    FILE *fp;
    int   ok;
} WriteCtx;

static void write_u32(WriteCtx *ctx, uint32_t v) {
    if (!ctx->ok) return;
    if (fwrite(&v, 4, 1, ctx->fp) != 1) ctx->ok = 0;
}

static void write_u64(WriteCtx *ctx, uint64_t v) {
    if (!ctx->ok) return;
    if (fwrite(&v, 8, 1, ctx->fp) != 1) ctx->ok = 0;
}

static void write_str(WriteCtx *ctx, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    write_u32(ctx, len);
    if (len > 0 && ctx->ok) {
        if (fwrite(s, len, 1, ctx->fp) != 1) ctx->ok = 0;
    }
}

static void write_value(WriteCtx *ctx, const XSDBValue *v) {
    write_u32(ctx, (uint32_t)v->type);
    switch (v->type) {
    case XSDB_TYPE_TEXT:
        write_str(ctx, v->text);
        break;
    case XSDB_TYPE_INT:
        write_u64(ctx, (uint64_t)v->ival);
        break;
    case XSDB_TYPE_FLOAT: {
        double f = v->fval;
        if (ctx->ok && fwrite(&f, sizeof(double), 1, ctx->fp) != 1) ctx->ok = 0;
        break;
    }
    case XSDB_TYPE_BLOB:
        write_u32(ctx, (uint32_t)v->blob.len);
        if (v->blob.len > 0 && ctx->ok) {
            if (fwrite(v->blob.data, v->blob.len, 1, ctx->fp) != 1) ctx->ok = 0;
        }
        break;
    default:
        /* NULL - no data needed beyond the type tag */
        break;
    }
}

typedef struct {
    WriteCtx *wctx;
} RowWriteCtx;

static void write_row_cb(XSDBRow *row, void *ctx) {
    RowWriteCtx *rctx = ctx;
    WriteCtx *wctx = rctx->wctx;
    write_u64(wctx, (uint64_t)row->rowid);
    write_u32(wctx, (uint32_t)row->ncells);
    for (int i = 0; i < row->ncells; i++) {
        write_value(wctx, &row->cells[i]);
    }
}

int xsdb_save(XSDB *db, const char *path) {
    if (!db || !path) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    WriteCtx ctx = {fp, 1};

    write_u32(&ctx, XSDB_MAGIC);
    write_u32(&ctx, XSDB_VERSION);
    write_u32(&ctx, (uint32_t)db->ntables);

    for (int t = 0; t < db->ntables; t++) {
        XSDBTable *tbl = &db->tables[t];
        write_str(&ctx, tbl->name);
        write_u32(&ctx, (uint32_t)tbl->ncols);

        for (int c = 0; c < tbl->ncols; c++) {
            write_str(&ctx, tbl->columns[c].name);
            write_u32(&ctx, (uint32_t)tbl->columns[c].type);
            write_u32(&ctx, (uint32_t)tbl->columns[c].not_null);
            write_u32(&ctx, (uint32_t)tbl->columns[c].unique);
            write_u32(&ctx, (uint32_t)tbl->columns[c].primary_key);
        }

        write_u64(&ctx, (uint64_t)tbl->next_rowid);
        write_u64(&ctx, (uint64_t)tbl->row_count);

        /* Write row count, then each row */
        int nrows = xsdb_btree_count(tbl->btree);
        write_u32(&ctx, (uint32_t)nrows);

        RowWriteCtx rctx = {&ctx};
        xsdb_btree_scan(tbl->btree, write_row_cb, &rctx);
    }

    fclose(fp);
    return ctx.ok ? 0 : -1;
}

typedef struct {
    FILE *fp;
    int   ok;
} ReadCtx;

static uint32_t read_u32(ReadCtx *ctx) {
    uint32_t v = 0;
    if (!ctx->ok) return 0;
    if (fread(&v, 4, 1, ctx->fp) != 1) ctx->ok = 0;
    return v;
}

static uint64_t read_u64(ReadCtx *ctx) {
    uint64_t v = 0;
    if (!ctx->ok) return 0;
    if (fread(&v, 8, 1, ctx->fp) != 1) ctx->ok = 0;
    return v;
}

static char *read_str(ReadCtx *ctx) {
    uint32_t len = read_u32(ctx);
    if (!ctx->ok || len == 0) return xsdb_strdup("");
    char *s = xsdb_alloc(len + 1);
    if (fread(s, len, 1, ctx->fp) != 1) {
        ctx->ok = 0;
        free(s);
        return xsdb_strdup("");
    }
    s[len] = '\0';
    return s;
}

static XSDBValue read_value(ReadCtx *ctx) {
    XSDBValue v;
    memset(&v, 0, sizeof v);
    uint32_t type = read_u32(ctx);
    v.type = (XSDBType)type;

    switch (v.type) {
    case XSDB_TYPE_TEXT:
        v.text = read_str(ctx);
        break;
    case XSDB_TYPE_INT:
        v.ival = (int64_t)read_u64(ctx);
        break;
    case XSDB_TYPE_FLOAT:
        if (ctx->ok && fread(&v.fval, sizeof(double), 1, ctx->fp) != 1) ctx->ok = 0;
        break;
    case XSDB_TYPE_BLOB: {
        uint32_t blen = read_u32(ctx);
        v.blob.len = blen;
        v.blob.data = xsdb_alloc(blen);
        if (blen > 0 && ctx->ok) {
            if (fread(v.blob.data, blen, 1, ctx->fp) != 1) ctx->ok = 0;
        }
        break;
    }
    default:
        v.type = XSDB_TYPE_NULL;
        break;
    }
    return v;
}

XSDB *xsdb_load(const char *path) {
    if (!path) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    ReadCtx ctx = {fp, 1};

    uint32_t magic = read_u32(&ctx);
    uint32_t version = read_u32(&ctx);
    (void)version;

    if (!ctx.ok || magic != XSDB_MAGIC) {
        fclose(fp);
        return NULL;
    }

    XSDB *db = xsdb_alloc(sizeof(XSDB));
    db->path = xsdb_strdup(path);

    uint32_t ntables = read_u32(&ctx);
    db->ntables = (int)ntables;
    db->table_cap = ntables > 8 ? ntables : 8;
    db->tables = xsdb_alloc(sizeof(XSDBTable) * db->table_cap);

    for (uint32_t t = 0; t < ntables && ctx.ok; t++) {
        XSDBTable *tbl = &db->tables[t];
        memset(tbl, 0, sizeof(XSDBTable));

        char *name = read_str(&ctx);
        strncpy(tbl->name, name, sizeof(tbl->name) - 1);
        free(name);

        uint32_t ncols = read_u32(&ctx);
        tbl->ncols = (int)ncols;
        tbl->columns = xsdb_alloc(sizeof(XSDBColumn) * ncols);

        for (uint32_t c = 0; c < ncols; c++) {
            char *cname = read_str(&ctx);
            strncpy(tbl->columns[c].name, cname, sizeof(tbl->columns[c].name) - 1);
            free(cname);
            tbl->columns[c].type = (XSDBType)read_u32(&ctx);
            tbl->columns[c].not_null = (int)read_u32(&ctx);
            tbl->columns[c].unique = (int)read_u32(&ctx);
            tbl->columns[c].primary_key = (int)read_u32(&ctx);
        }

        tbl->next_rowid = (int64_t)read_u64(&ctx);
        tbl->row_count = (int64_t)read_u64(&ctx);

        uint32_t nrows = read_u32(&ctx);
        for (uint32_t r = 0; r < nrows && ctx.ok; r++) {
            int64_t rowid = (int64_t)read_u64(&ctx);
            uint32_t ncells = read_u32(&ctx);
            XSDBRow *row = row_new((int)ncells);
            row->rowid = rowid;
            for (uint32_t c = 0; c < ncells; c++) {
                row->cells[c] = read_value(&ctx);
            }
            xsdb_btree_insert(&tbl->btree, row->rowid, row);
        }
    }

    fclose(fp);

    if (!ctx.ok) {
        xsdb_close(db);
        return NULL;
    }

    return db;
}

/* ----------------------------------------------------------------
 * Result helpers
 * ---------------------------------------------------------------- */

void xsdb_result_free(XSDBResult *r) {
    if (!r) return;
    for (int i = 0; i < r->nrows; i++) {
        for (int j = 0; j < r->rows[i].ncells; j++) {
            xsdb_value_free(&r->rows[i].cells[j]);
        }
        free(r->rows[i].cells);
    }
    free(r->rows);
    if (r->col_names) {
        for (int i = 0; i < r->ncols; i++)
            free(r->col_names[i]);
        free(r->col_names);
    }
    free(r->error);
    free(r);
}

XSDBValue *xsdb_result_get(XSDBResult *r, int row, int col) {
    if (!r || row < 0 || row >= r->nrows || col < 0 || col >= r->rows[row].ncells)
        return NULL;
    return &r->rows[row].cells[col];
}

const char *xsdb_result_col_name(XSDBResult *r, int col) {
    if (!r || col < 0 || col >= r->ncols) return NULL;
    return r->col_names[col];
}

/* ----------------------------------------------------------------
 * Aggregation engine
 * ---------------------------------------------------------------- */

typedef enum {
    AGG_COUNT = 0,
    AGG_SUM,
    AGG_AVG,
    AGG_MIN,
    AGG_MAX,
    AGG_COUNT_DISTINCT,
    AGG_GROUP_CONCAT
} AggType;

typedef struct {
    AggType type;
    int col_idx;
    char col_name[XSDB_IDENT_MAX];
    char alias[XSDB_IDENT_MAX];
} AggExpr;

typedef struct {
    int64_t count;
    double sum;
    double min_val;
    double max_val;
    int has_min;
    int has_max;
    char *min_text;
    char *max_text;
    char *concat_buf;
    int concat_len;
    int concat_cap;
    int64_t *distinct_ints;
    int ndistinct;
    int distinct_cap;
} AggAccum;

static void agg_accum_init(AggAccum *acc) {
    memset(acc, 0, sizeof(AggAccum));
    acc->has_min = 0;
    acc->has_max = 0;
    acc->concat_cap = 256;
    acc->concat_buf = calloc(1, acc->concat_cap);
    acc->distinct_cap = 64;
    acc->distinct_ints = calloc(acc->distinct_cap, sizeof(int64_t));
}

static void agg_accum_free(AggAccum *acc) {
    free(acc->min_text);
    free(acc->max_text);
    free(acc->concat_buf);
    free(acc->distinct_ints);
}

static double xsdb_value_to_double(const XSDBValue *v) {
    switch (v->type) {
        case XSDB_TYPE_INT:   return (double)v->ival;
        case XSDB_TYPE_FLOAT: return v->fval;
        case XSDB_TYPE_TEXT:  return v->text ? strtod(v->text, NULL) : 0.0;
        default:              return 0.0;
    }
}

static void agg_accum_add(AggAccum *acc, AggType type, const XSDBValue *val) {
    if (!val || val->type == XSDB_TYPE_NULL) {
        if (type == AGG_COUNT) acc->count++;
        return;
    }

    acc->count++;
    double d = xsdb_value_to_double(val);

    switch (type) {
        case AGG_COUNT:
            break;
        case AGG_SUM:
        case AGG_AVG:
            acc->sum += d;
            break;
        case AGG_MIN:
            if (!acc->has_min || d < acc->min_val) {
                acc->min_val = d;
                acc->has_min = 1;
                free(acc->min_text);
                acc->min_text = xsdb_value_to_string(val);
            }
            break;
        case AGG_MAX:
            if (!acc->has_max || d > acc->max_val) {
                acc->max_val = d;
                acc->has_max = 1;
                free(acc->max_text);
                acc->max_text = xsdb_value_to_string(val);
            }
            break;
        case AGG_COUNT_DISTINCT:
            if (val->type == XSDB_TYPE_INT) {
                int found = 0;
                for (int i = 0; i < acc->ndistinct; i++) {
                    if (acc->distinct_ints[i] == val->ival) { found = 1; break; }
                }
                if (!found) {
                    if (acc->ndistinct >= acc->distinct_cap) {
                        acc->distinct_cap *= 2;
                        acc->distinct_ints = realloc(acc->distinct_ints,
                            acc->distinct_cap * sizeof(int64_t));
                    }
                    acc->distinct_ints[acc->ndistinct++] = val->ival;
                }
            }
            break;
        case AGG_GROUP_CONCAT: {
            char *s = xsdb_value_to_string(val);
            int slen = (int)strlen(s);
            int need = acc->concat_len + slen + 2;
            if (need >= acc->concat_cap) {
                while (acc->concat_cap < need) acc->concat_cap *= 2;
                acc->concat_buf = realloc(acc->concat_buf, acc->concat_cap);
            }
            if (acc->concat_len > 0) {
                acc->concat_buf[acc->concat_len++] = ',';
            }
            memcpy(acc->concat_buf + acc->concat_len, s, slen);
            acc->concat_len += slen;
            acc->concat_buf[acc->concat_len] = '\0';
            free(s);
            break;
        }
    }
}

static XSDBValue agg_accum_result(AggAccum *acc, AggType type) {
    switch (type) {
        case AGG_COUNT:
            return xsdb_value_int(acc->count);
        case AGG_SUM:
            return xsdb_value_float(acc->sum);
        case AGG_AVG:
            if (acc->count == 0) return xsdb_value_null();
            return xsdb_value_float(acc->sum / (double)acc->count);
        case AGG_MIN:
            if (!acc->has_min) return xsdb_value_null();
            return xsdb_value_float(acc->min_val);
        case AGG_MAX:
            if (!acc->has_max) return xsdb_value_null();
            return xsdb_value_float(acc->max_val);
        case AGG_COUNT_DISTINCT:
            return xsdb_value_int(acc->ndistinct);
        case AGG_GROUP_CONCAT:
            return xsdb_value_text(acc->concat_buf ? acc->concat_buf : "");
    }
    return xsdb_value_null();
}

/* ----------------------------------------------------------------
 * GROUP BY execution
 * ---------------------------------------------------------------- */

#define XSDB_MAX_GROUP_COLS 8
#define XSDB_MAX_AGGS 16

typedef struct GroupKey {
    XSDBValue vals[XSDB_MAX_GROUP_COLS];
    int nvals;
} GroupKey;

typedef struct GroupBucket {
    GroupKey key;
    AggAccum accums[XSDB_MAX_AGGS];
    int naggs;
    struct GroupBucket *next;
} GroupBucket;

#define GROUP_HASH_SIZE 256

typedef struct {
    GroupBucket *buckets[GROUP_HASH_SIZE];
    int ngroups;
} GroupHash;

static uint32_t group_key_hash(const GroupKey *key) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < key->nvals; i++) {
        const XSDBValue *v = &key->vals[i];
        switch (v->type) {
            case XSDB_TYPE_INT:
                h ^= (uint32_t)(v->ival & 0xFFFFFFFF);
                h *= 16777619u;
                h ^= (uint32_t)((v->ival >> 32) & 0xFFFFFFFF);
                h *= 16777619u;
                break;
            case XSDB_TYPE_FLOAT: {
                uint64_t bits;
                memcpy(&bits, &v->fval, sizeof(bits));
                h ^= (uint32_t)(bits & 0xFFFFFFFF);
                h *= 16777619u;
                break;
            }
            case XSDB_TYPE_TEXT:
                if (v->text) {
                    for (const char *p = v->text; *p; p++) {
                        h ^= (unsigned char)*p;
                        h *= 16777619u;
                    }
                }
                break;
            default:
                h ^= 0xFF;
                h *= 16777619u;
                break;
        }
    }
    return h;
}

static int group_key_equal(const GroupKey *a, const GroupKey *b) {
    if (a->nvals != b->nvals) return 0;
    for (int i = 0; i < a->nvals; i++) {
        if (xsdb_value_compare(&a->vals[i], &b->vals[i]) != 0) return 0;
    }
    return 1;
}

static GroupBucket *group_hash_find(GroupHash *gh, const GroupKey *key) {
    uint32_t idx = group_key_hash(key) % GROUP_HASH_SIZE;
    GroupBucket *b = gh->buckets[idx];
    while (b) {
        if (group_key_equal(&b->key, key)) return b;
        b = b->next;
    }
    return NULL;
}

static GroupBucket *group_hash_insert(GroupHash *gh, const GroupKey *key, int naggs) {
    uint32_t idx = group_key_hash(key) % GROUP_HASH_SIZE;
    GroupBucket *b = calloc(1, sizeof(GroupBucket));
    b->key = *key;
    b->naggs = naggs;
    for (int i = 0; i < naggs; i++) {
        agg_accum_init(&b->accums[i]);
    }
    b->next = gh->buckets[idx];
    gh->buckets[idx] = b;
    gh->ngroups++;
    return b;
}

static void group_hash_free(GroupHash *gh) {
    for (int i = 0; i < GROUP_HASH_SIZE; i++) {
        GroupBucket *b = gh->buckets[i];
        while (b) {
            GroupBucket *next = b->next;
            for (int j = 0; j < b->naggs; j++) {
                agg_accum_free(&b->accums[j]);
            }
            for (int j = 0; j < b->key.nvals; j++) {
                xsdb_value_free(&b->key.vals[j]);
            }
            free(b);
            b = next;
        }
    }
}

/* ----------------------------------------------------------------
 * Query plan and statistics
 * ---------------------------------------------------------------- */

typedef enum {
    QPLAN_FULL_SCAN = 0,
    QPLAN_INDEX_SCAN,
    QPLAN_ROWID_LOOKUP,
    QPLAN_INDEX_RANGE
} QPlanType;

typedef struct {
    QPlanType type;
    char table[XSDB_IDENT_MAX];
    char index[XSDB_IDENT_MAX];
    int estimated_rows;
    int scan_direction;
} QueryPlan;

typedef struct {
    int64_t rows_scanned;
    int64_t rows_returned;
    int64_t index_lookups;
    double elapsed_us;
} QueryStats;

static QueryPlan xsdb_plan_query(XSDB *db, XSDBStmt *stmt) {
    QueryPlan plan;
    memset(&plan, 0, sizeof(plan));
    strncpy(plan.table, stmt->table, XSDB_IDENT_MAX - 1);

    XSDBTable *tbl = NULL;
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, stmt->table) == 0) {
            tbl = &db->tables[i];
            break;
        }
    }

    if (!tbl) {
        plan.type = QPLAN_FULL_SCAN;
        plan.estimated_rows = 0;
        return plan;
    }

    plan.estimated_rows = (int)tbl->row_count;

    if (stmt->where && stmt->where->next == NULL) {
        if (strcmp(stmt->where->col, "rowid") == 0 &&
            stmt->where->op == XSDB_CMP_EQ) {
            plan.type = QPLAN_ROWID_LOOKUP;
            plan.estimated_rows = 1;
            return plan;
        }

        XSDBIndex *idx = tbl->indexes;
        while (idx) {
            if (strcmp(idx->col_name, stmt->where->col) == 0) {
                if (stmt->where->op == XSDB_CMP_EQ) {
                    plan.type = QPLAN_INDEX_SCAN;
                    strncpy(plan.index, idx->name, XSDB_IDENT_MAX - 1);
                    plan.estimated_rows = (int)(tbl->row_count / 10);
                    if (plan.estimated_rows < 1) plan.estimated_rows = 1;
                    return plan;
                }
                if (stmt->where->op == XSDB_CMP_GT ||
                    stmt->where->op == XSDB_CMP_GE ||
                    stmt->where->op == XSDB_CMP_LT ||
                    stmt->where->op == XSDB_CMP_LE) {
                    plan.type = QPLAN_INDEX_RANGE;
                    strncpy(plan.index, idx->name, XSDB_IDENT_MAX - 1);
                    plan.estimated_rows = (int)(tbl->row_count / 3);
                    if (plan.estimated_rows < 1) plan.estimated_rows = 1;
                    return plan;
                }
            }
            idx = idx->next;
        }
    }

    plan.type = QPLAN_FULL_SCAN;
    return plan;
}

static const char *qplan_type_str(QPlanType t) {
    switch (t) {
        case QPLAN_FULL_SCAN:    return "FULL_SCAN";
        case QPLAN_INDEX_SCAN:   return "INDEX_SCAN";
        case QPLAN_ROWID_LOOKUP: return "ROWID_LOOKUP";
        case QPLAN_INDEX_RANGE:  return "INDEX_RANGE";
    }
    return "UNKNOWN";
}

char *xsdb_explain(XSDB *db, const char *sql) {
    XSDBStmt *stmt = xsdb_parse_sql(sql);
    if (!stmt) return xsdb_strdup("error: parse failed");

    QueryPlan plan = xsdb_plan_query(db, stmt);
    char buf[1024];
    snprintf(buf, sizeof buf,
        "plan: %s\ntable: %s\nindex: %s\nestimated_rows: %d",
        qplan_type_str(plan.type),
        plan.table,
        plan.index[0] ? plan.index : "(none)",
        plan.estimated_rows);

    xsdb_stmt_free(stmt);
    return xsdb_strdup(buf);
}

/* ----------------------------------------------------------------
 * Schema introspection helpers
 * ---------------------------------------------------------------- */

int xsdb_column_count(XSDB *db, const char *table) {
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            return db->tables[i].ncols;
        }
    }
    return -1;
}

const char *xsdb_column_name(XSDB *db, const char *table, int col) {
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            if (col >= 0 && col < db->tables[i].ncols)
                return db->tables[i].columns[col].name;
            return NULL;
        }
    }
    return NULL;
}

XSDBType xsdb_column_type(XSDB *db, const char *table, int col) {
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            if (col >= 0 && col < db->tables[i].ncols)
                return db->tables[i].columns[col].type;
            return XSDB_TYPE_NULL;
        }
    }
    return XSDB_TYPE_NULL;
}

int64_t xsdb_row_count(XSDB *db, const char *table) {
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            return db->tables[i].row_count;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------
 * LIKE pattern matching (SQL LIKE with % and _)
 * ---------------------------------------------------------------- */

static int xsdb_like_match(const char *pattern, const char *text, int case_insensitive) {
    const char *p = pattern;
    const char *t = text;
    const char *star_p = NULL;
    const char *star_t = NULL;

    while (*t) {
        if (*p == '%') {
            star_p = ++p;
            star_t = t;
            continue;
        }
        if (*p == '_') {
            p++;
            t++;
            continue;
        }
        int pc = *p;
        int tc = *t;
        if (case_insensitive) {
            pc = (pc >= 'A' && pc <= 'Z') ? pc + 32 : pc;
            tc = (tc >= 'A' && tc <= 'Z') ? tc + 32 : tc;
        }
        if (pc == tc) {
            p++;
            t++;
            continue;
        }
        if (star_p) {
            p = star_p;
            t = ++star_t;
            continue;
        }
        return 0;
    }

    while (*p == '%') p++;
    return *p == '\0';
}

/* ----------------------------------------------------------------
 * B-tree range scan with bounds
 * ---------------------------------------------------------------- */

typedef struct {
    int64_t lo;
    int64_t hi;
    int lo_inclusive;
    int hi_inclusive;
    int has_lo;
    int has_hi;
} BTreeRange;

static void xsdb_btree_range_scan(XSDBBTreeNode *node, const BTreeRange *range,
                                    void (*cb)(XSDBRow *row, void *ctx), void *ctx)
{
    if (!node) return;

    for (int i = 0; i < node->nkeys; i++) {
        if (!node->is_leaf) {
            int should_recurse = 1;
            if (range->has_hi) {
                if (node->keys[i] > range->hi) should_recurse = 1;
                else if (!range->hi_inclusive && node->keys[i] == range->hi)
                    should_recurse = 1;
            }
            if (should_recurse)
                xsdb_btree_range_scan(node->children[i], range, cb, ctx);
        }

        int in_range = 1;
        if (range->has_lo) {
            if (range->lo_inclusive) {
                if (node->keys[i] < range->lo) in_range = 0;
            } else {
                if (node->keys[i] <= range->lo) in_range = 0;
            }
        }
        if (range->has_hi) {
            if (range->hi_inclusive) {
                if (node->keys[i] > range->hi) in_range = 0;
            } else {
                if (node->keys[i] >= range->hi) in_range = 0;
            }
        }

        if (in_range && node->rows[i]) {
            cb(node->rows[i], ctx);
        }
    }

    if (!node->is_leaf) {
        xsdb_btree_range_scan(node->children[node->nkeys], range, cb, ctx);
    }
}

/* ----------------------------------------------------------------
 * B-tree iterator (in-order traversal without callback)
 * ---------------------------------------------------------------- */

#define BTREE_ITER_STACK 64

typedef struct {
    XSDBBTreeNode *node;
    int idx;
} BTreeIterFrame;

typedef struct {
    BTreeIterFrame stack[BTREE_ITER_STACK];
    int top;
    int started;
} BTreeIter;

void xsdb_btree_iter_init(BTreeIter *it, XSDBBTreeNode *root) {
    memset(it, 0, sizeof(BTreeIter));
    it->top = -1;
    it->started = 0;

    XSDBBTreeNode *n = root;
    while (n) {
        it->top++;
        it->stack[it->top].node = n;
        it->stack[it->top].idx = 0;
        if (n->is_leaf) break;
        n = n->children[0];
    }
}

XSDBRow *xsdb_btree_iter_next(BTreeIter *it) {
    while (it->top >= 0) {
        BTreeIterFrame *frame = &it->stack[it->top];
        XSDBBTreeNode *node = frame->node;

        if (frame->idx < node->nkeys) {
            XSDBRow *row = node->rows[frame->idx];
            frame->idx++;

            if (!node->is_leaf && frame->idx <= node->nkeys) {
                XSDBBTreeNode *child = node->children[frame->idx];
                while (child) {
                    it->top++;
                    if (it->top >= BTREE_ITER_STACK) {
                        it->top--;
                        break;
                    }
                    it->stack[it->top].node = child;
                    it->stack[it->top].idx = 0;
                    if (child->is_leaf) break;
                    child = child->children[0];
                }
            }

            return row;
        }

        it->top--;
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Table copy for snapshots (uses xsdb_value_copy defined above)
 * ---------------------------------------------------------------- */

static XSDBRow *xsdb_snapshot_row_copy(const XSDBRow *src) {
    XSDBRow *dst = xsdb_alloc(sizeof(XSDBRow));
    dst->rowid = src->rowid;
    dst->ncells = src->ncells;
    dst->cells = xsdb_alloc(sizeof(XSDBValue) * src->ncells);
    for (int i = 0; i < src->ncells; i++) {
        dst->cells[i] = xsdb_value_copy(&src->cells[i]);
    }
    return dst;
}

static void xsdb_btree_snapshot_copy(XSDBBTreeNode *node,
                                   XSDBBTreeNode **dst_root)
{
    if (!node) return;
    for (int i = 0; i < node->nkeys; i++) {
        if (!node->is_leaf)
            xsdb_btree_snapshot_copy(node->children[i], dst_root);
        if (node->rows[i]) {
            XSDBRow *copy = xsdb_snapshot_row_copy(node->rows[i]);
            xsdb_btree_insert(dst_root, copy->rowid, copy);
        }
    }
    if (!node->is_leaf)
        xsdb_btree_snapshot_copy(node->children[node->nkeys], dst_root);
}

/* ----------------------------------------------------------------
 * Index management helpers
 * ---------------------------------------------------------------- */

int xsdb_create_index(XSDB *db, const char *table, const char *col,
                       const char *idx_name, int unique)
{
    XSDBTable *tbl = NULL;
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            tbl = &db->tables[i];
            break;
        }
    }
    if (!tbl) return -1;

    int col_idx = -1;
    for (int i = 0; i < tbl->ncols; i++) {
        if (strcmp(tbl->columns[i].name, col) == 0) {
            col_idx = i;
            break;
        }
    }
    if (col_idx < 0) return -2;

    XSDBIndex *idx = xsdb_alloc(sizeof(XSDBIndex));
    strncpy(idx->name, idx_name, XSDB_IDENT_MAX - 1);
    strncpy(idx->col_name, col, XSDB_IDENT_MAX - 1);
    idx->col_idx = col_idx;
    idx->unique = unique;
    idx->root = NULL;
    idx->next = tbl->indexes;
    tbl->indexes = idx;

    return 0;
}

int xsdb_drop_index(XSDB *db, const char *table, const char *idx_name) {
    XSDBTable *tbl = NULL;
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            tbl = &db->tables[i];
            break;
        }
    }
    if (!tbl) return -1;

    XSDBIndex **pp = &tbl->indexes;
    while (*pp) {
        if (strcmp((*pp)->name, idx_name) == 0) {
            XSDBIndex *rem = *pp;
            *pp = rem->next;
            xsdb_btree_free(rem->root);
            free(rem);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -2;
}

/* ----------------------------------------------------------------
 * Bulk insert optimization
 * ---------------------------------------------------------------- */

int xsdb_bulk_insert(XSDB *db, const char *table,
                      XSDBRow *rows, int nrows)
{
    XSDBTable *tbl = NULL;
    for (int i = 0; i < db->ntables; i++) {
        if (strcmp(db->tables[i].name, table) == 0) {
            tbl = &db->tables[i];
            break;
        }
    }
    if (!tbl) return -1;

    for (int i = 0; i < nrows; i++) {
        XSDBRow *row = xsdb_snapshot_row_copy(&rows[i]);
        row->rowid = tbl->next_rowid++;
        xsdb_btree_insert(&tbl->btree, row->rowid, row);
        tbl->row_count++;
    }

    return nrows;
}

/* ----------------------------------------------------------------
 * Data integrity checks
 * ---------------------------------------------------------------- */

typedef struct {
    int64_t prev_key;
    int first;
    int order_ok;
    int total_keys;
    int leaf_count;
    int internal_count;
} IntegrityCtx;

static void integrity_check_node(XSDBBTreeNode *node, IntegrityCtx *ctx, int depth) {
    if (!node) return;

    if (node->is_leaf) ctx->leaf_count++;
    else ctx->internal_count++;

    for (int i = 0; i < node->nkeys; i++) {
        if (!node->is_leaf)
            integrity_check_node(node->children[i], ctx, depth + 1);

        if (!ctx->first) {
            if (node->keys[i] <= ctx->prev_key) {
                ctx->order_ok = 0;
            }
        }
        ctx->prev_key = node->keys[i];
        ctx->first = 0;
        ctx->total_keys++;
    }

    if (!node->is_leaf)
        integrity_check_node(node->children[node->nkeys], ctx, depth + 1);
}

int xsdb_integrity_check(XSDB *db) {
    int issues = 0;
    for (int t = 0; t < db->ntables; t++) {
        XSDBTable *tbl = &db->tables[t];
        IntegrityCtx ctx = {0, 1, 1, 0, 0, 0};
        integrity_check_node(tbl->btree, &ctx, 0);

        if (!ctx.order_ok) issues++;
        if (ctx.total_keys != (int)tbl->row_count) issues++;
    }
    return issues;
}

/* ----------------------------------------------------------------
 * Sort result rows
 * ---------------------------------------------------------------- */

typedef struct {
    int col_idx;
    int descending;
} ResultSortKey;

static ResultSortKey g_res_sort_key;

static int result_row_sort_cmp(const void *a, const void *b) {
    const XSDBRow *ra = (const XSDBRow *)a;
    const XSDBRow *rb = (const XSDBRow *)b;

    int ci = g_res_sort_key.col_idx;
    if (ci >= ra->ncells || ci >= rb->ncells) return 0;

    int c = xsdb_value_compare(&ra->cells[ci], &rb->cells[ci]);
    return g_res_sort_key.descending ? -c : c;
}

void xsdb_result_sort(XSDBResult *r, int col_idx, int descending) {
    if (!r || r->nrows <= 1 || col_idx < 0) return;
    g_res_sort_key.col_idx = col_idx;
    g_res_sort_key.descending = descending;
    qsort(r->rows, r->nrows, sizeof(XSDBRow), result_row_sort_cmp);
}

/* ----------------------------------------------------------------
 * Result set operations
 * ---------------------------------------------------------------- */

XSDBResult *xsdb_result_limit(XSDBResult *src, int limit, int offset) {
    if (!src) return NULL;
    XSDBResult *dst = result_new();
    if (src->col_names) {
        result_set_columns(dst, src->col_names, src->ncols);
    }

    int start = offset < 0 ? 0 : offset;
    int end = (limit < 0) ? src->nrows : start + limit;
    if (end > src->nrows) end = src->nrows;

    for (int i = start; i < end; i++) {
        result_add_row(dst, &src->rows[i]);
    }
    dst->affected = dst->nrows;
    return dst;
}

XSDBResult *xsdb_result_project(XSDBResult *src, int *col_idxs, int ncols) {
    if (!src) return NULL;
    XSDBResult *dst = result_new();

    char *names[XSDB_MAX_COLS];
    for (int i = 0; i < ncols && i < XSDB_MAX_COLS; i++) {
        int ci = col_idxs[i];
        names[i] = (ci >= 0 && ci < src->ncols) ? src->col_names[ci] : "?";
    }
    result_set_columns(dst, names, ncols);

    for (int r = 0; r < src->nrows; r++) {
        XSDBRow row;
        row.rowid = src->rows[r].rowid;
        row.ncells = ncols;
        row.cells = xsdb_alloc(sizeof(XSDBValue) * ncols);
        for (int c = 0; c < ncols; c++) {
            int ci = col_idxs[c];
            if (ci >= 0 && ci < src->rows[r].ncells)
                row.cells[c] = xsdb_value_copy(&src->rows[r].cells[ci]);
            else
                row.cells[c] = xsdb_value_null();
        }
        result_add_row(dst, &row);
    }
    return dst;
}

/* ----------------------------------------------------------------
 * Database statistics
 * ---------------------------------------------------------------- */

typedef struct {
    int total_tables;
    int total_rows;
    int total_indexes;
    int total_columns;
    size_t estimated_memory;
} XSDBStats;

XSDBStats xsdb_get_stats(XSDB *db) {
    XSDBStats stats = {0};
    stats.total_tables = db->ntables;
    stats.estimated_memory = sizeof(XSDB);

    for (int i = 0; i < db->ntables; i++) {
        XSDBTable *tbl = &db->tables[i];
        stats.total_rows += (int)tbl->row_count;
        stats.total_columns += tbl->ncols;
        stats.estimated_memory += sizeof(XSDBTable);
        stats.estimated_memory += sizeof(XSDBColumn) * tbl->ncols;

        XSDBIndex *idx = tbl->indexes;
        while (idx) {
            stats.total_indexes++;
            idx = idx->next;
        }
    }
    return stats;
}
