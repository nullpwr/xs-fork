#include "vm/vm.h"
#include "vm/bytecode.h"
#include "core/value.h"
#include "core/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VM_PATTERN_CACHE_SIZE 256

typedef enum {
    PAT_WILD,
    PAT_LIT_INT,
    PAT_LIT_FLOAT,
    PAT_LIT_STR,
    PAT_LIT_BOOL,
    PAT_LIT_NULL,
    PAT_BIND,
    PAT_TUPLE,
    PAT_ENUM,
    PAT_RANGE,
    PAT_OR,
    PAT_GUARD,
    PAT_CAPTURE,
    PAT_SLICE,
    PAT_STRUCT,
} PatKind;

typedef struct PatInstr PatInstr;
struct PatInstr {
    PatKind kind;
    union {
        struct { int64_t val; } lit_int;
        struct { double val; } lit_float;
        struct { char *val; } lit_str;
        struct { int val; } lit_bool;
        struct { int slot; char name[64]; } bind;
        struct { int count; int *sub_indices; } tuple;
        struct {
            char variant[128];
            int argc;
            int *sub_indices;
        } ctor;
        struct {
            int64_t lo;
            int64_t hi;
            int inclusive;
        } range;
        struct { int left_idx; int right_idx; } or_pat;
        struct { int pat_idx; int guard_idx; } guard;
        struct { int slot; int sub_idx; char name[64]; } capture;
        struct { int count; int *sub_indices; char *rest_name; } slice;
        struct {
            int nfields;
            char **field_names;
            int *sub_indices;
        } struct_pat;
    };
};

typedef struct {
    PatInstr *instrs;
    int       len;
    int       cap;
} PatProgram;

static PatProgram pat_prog_new(void) {
    PatProgram p;
    p.instrs = NULL;
    p.len = 0;
    p.cap = 0;
    return p;
}

static int pat_prog_add(PatProgram *p, PatInstr instr) {
    if (p->len >= p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->instrs = xs_realloc(p->instrs, (size_t)p->cap * sizeof(PatInstr));
    }
    int idx = p->len;
    p->instrs[p->len++] = instr;
    return idx;
}

static void pat_prog_free(PatProgram *p) {
    for (int i = 0; i < p->len; i++) {
        PatInstr *pi = &p->instrs[i];
        switch (pi->kind) {
        case PAT_LIT_STR: free(pi->lit_str.val); break;
        case PAT_TUPLE: free(pi->tuple.sub_indices); break;
        case PAT_ENUM: free(pi->ctor.sub_indices); break;
        case PAT_SLICE:
            free(pi->slice.sub_indices);
            free(pi->slice.rest_name);
            break;
        case PAT_STRUCT:
            for (int j = 0; j < pi->struct_pat.nfields; j++)
                free(pi->struct_pat.field_names[j]);
            free(pi->struct_pat.field_names);
            free(pi->struct_pat.sub_indices);
            break;
        default: break;
        }
    }
    free(p->instrs);
}

static int compile_pattern(PatProgram *prog, Node *pat) {
    if (!pat) {
        PatInstr wild = {.kind = PAT_WILD};
        return pat_prog_add(prog, wild);
    }
    switch (VAL_TAG(pat)) {
    case NODE_PAT_WILD: {
        PatInstr wild = {.kind = PAT_WILD};
        return pat_prog_add(prog, wild);
    }
    case NODE_PAT_IDENT: {
        PatInstr bind = {.kind = PAT_BIND};
        bind.bind.slot = -1;
        strncpy(bind.bind.name, pat->pat_ident.name ? pat->pat_ident.name : "_",
                sizeof bind.bind.name - 1);
        return pat_prog_add(prog, bind);
    }
    case NODE_PAT_LIT: {
        PatInstr pi = {0};
        switch (pat->pat_lit.tag) {
        case 0:
            pi.kind = PAT_LIT_INT;
            pi.lit_int.val = pat->pat_lit.ival;
            break;
        case 1:
            pi.kind = PAT_LIT_FLOAT;
            pi.lit_float.val = pat->pat_lit.fval;
            break;
        case 2:
            pi.kind = PAT_LIT_STR;
            pi.lit_str.val = pat->pat_lit.sval ? xs_strdup(pat->pat_lit.sval) : xs_strdup("");
            break;
        case 3:
            pi.kind = PAT_LIT_BOOL;
            pi.lit_bool.val = pat->pat_lit.bval;
            break;
        default:
            pi.kind = PAT_LIT_NULL;
            break;
        }
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_TUPLE: {
        int cnt = pat->pat_tuple.elems.len;
        int *subs = xs_malloc((size_t)cnt * sizeof(int));
        for (int i = 0; i < cnt; i++)
            subs[i] = compile_pattern(prog, pat->pat_tuple.elems.items[i]);
        PatInstr pi = {.kind = PAT_TUPLE};
        pi.tuple.count = cnt;
        pi.tuple.sub_indices = subs;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_ENUM: {
        int cnt = pat->pat_enum.args.len;
        int *subs = cnt > 0 ? xs_malloc((size_t)cnt * sizeof(int)) : NULL;
        for (int i = 0; i < cnt; i++)
            subs[i] = compile_pattern(prog, pat->pat_enum.args.items[i]);
        PatInstr pi = {.kind = PAT_ENUM};
        const char *path = pat->pat_enum.path;
        const char *last = strrchr(path, ':');
        const char *variant = (last && last > path) ? last + 1 : path;
        strncpy(pi.ctor.variant, variant, sizeof pi.ctor.variant - 1);
        pi.ctor.argc = cnt;
        pi.ctor.sub_indices = subs;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_OR: {
        int left = compile_pattern(prog, pat->pat_or.left);
        int right = compile_pattern(prog, pat->pat_or.right);
        PatInstr pi = {.kind = PAT_OR};
        pi.or_pat.left_idx = left;
        pi.or_pat.right_idx = right;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_RANGE: {
        PatInstr pi = {.kind = PAT_RANGE};
        pi.range.lo = pat->pat_range.start && VAL_TAG(pat->pat_range.start) == NODE_LIT_INT
            ? pat->pat_range.start->lit_int.ival : 0;
        pi.range.hi = pat->pat_range.end && VAL_TAG(pat->pat_range.end) == NODE_LIT_INT
            ? pat->pat_range.end->lit_int.ival : 0;
        pi.range.inclusive = pat->pat_range.inclusive;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_GUARD: {
        int sub = compile_pattern(prog, pat->pat_guard.pattern);
        PatInstr pi = {.kind = PAT_GUARD};
        pi.guard.pat_idx = sub;
        pi.guard.guard_idx = -1;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_CAPTURE: {
        int sub = compile_pattern(prog, pat->pat_capture.pattern);
        PatInstr pi = {.kind = PAT_CAPTURE};
        strncpy(pi.capture.name, pat->pat_capture.name ? pat->pat_capture.name : "_",
                sizeof pi.capture.name - 1);
        pi.capture.slot = -1;
        pi.capture.sub_idx = sub;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_SLICE: {
        int cnt = pat->pat_slice.elems.len;
        int *subs = cnt > 0 ? xs_malloc((size_t)cnt * sizeof(int)) : NULL;
        for (int i = 0; i < cnt; i++)
            subs[i] = compile_pattern(prog, pat->pat_slice.elems.items[i]);
        PatInstr pi = {.kind = PAT_SLICE};
        pi.slice.count = cnt;
        pi.slice.sub_indices = subs;
        pi.slice.rest_name = pat->pat_slice.rest ? xs_strdup(pat->pat_slice.rest) : NULL;
        return pat_prog_add(prog, pi);
    }
    case NODE_PAT_STRUCT: {
        int cnt = pat->pat_struct.fields.len;
        char **names = cnt > 0 ? xs_malloc((size_t)cnt * sizeof(char *)) : NULL;
        int *subs = cnt > 0 ? xs_malloc((size_t)cnt * sizeof(int)) : NULL;
        for (int i = 0; i < cnt; i++) {
            names[i] = xs_strdup(pat->pat_struct.fields.items[i].key);
            subs[i] = compile_pattern(prog, pat->pat_struct.fields.items[i].val);
        }
        PatInstr pi = {.kind = PAT_STRUCT};
        pi.struct_pat.nfields = cnt;
        pi.struct_pat.field_names = names;
        pi.struct_pat.sub_indices = subs;
        return pat_prog_add(prog, pi);
    }
    default: {
        PatInstr wild = {.kind = PAT_WILD};
        return pat_prog_add(prog, wild);
    }
    }
}

static int pat_match(PatProgram *prog, int idx, Value *subject,
                     Value **bindings, int *nbindings, int max_binds) {
    if (idx < 0 || idx >= prog->len) return 1;
    PatInstr *pi = &prog->instrs[idx];
    switch (pi->kind) {
    case PAT_WILD:
        return 1;
    case PAT_LIT_INT:
        return VAL_TAG(subject) == XS_INT && VAL_INT(subject) == pi->lit_int.val;
    case PAT_LIT_FLOAT:
        return VAL_TAG(subject) == XS_FLOAT && subject->f == pi->lit_float.val;
    case PAT_LIT_STR:
        return VAL_TAG(subject) == XS_STR && strcmp(subject->s, pi->lit_str.val) == 0;
    case PAT_LIT_BOOL:
        return VAL_TAG(subject) == XS_BOOL && value_truthy(subject) == pi->lit_bool.val;
    case PAT_LIT_NULL:
        return VAL_TAG(subject) == XS_NULL;
    case PAT_BIND:
        if (*nbindings < max_binds) {
            bindings[*nbindings] = value_incref(subject);
            (*nbindings)++;
        }
        return 1;
    case PAT_TUPLE:
        if ((VAL_TAG(subject) != XS_ARRAY && VAL_TAG(subject) != XS_TUPLE) ||
            subject->arr->len != pi->tuple.count)
            return 0;
        for (int i = 0; i < pi->tuple.count; i++) {
            if (!pat_match(prog, pi->tuple.sub_indices[i],
                           subject->arr->items[i], bindings, nbindings, max_binds))
                return 0;
        }
        return 1;
    case PAT_ENUM: {
        if (VAL_TAG(subject) == XS_ENUM_VAL) {
            if (!subject->en->variant ||
                strcmp(subject->en->variant, pi->ctor.variant) != 0)
                return 0;
            if (pi->ctor.argc > 0) {
                XSArray *data = subject->en->arr_data;
                if (!data) return pi->ctor.argc == 0;
                if (pi->ctor.argc == 1 && data->len == 1) {
                    return pat_match(prog, pi->ctor.sub_indices[0],
                                     data->items[0], bindings, nbindings, max_binds);
                }
                for (int i = 0; i < pi->ctor.argc && i < data->len; i++) {
                    if (!pat_match(prog, pi->ctor.sub_indices[i],
                                   data->items[i], bindings, nbindings, max_binds))
                        return 0;
                }
            }
            return 1;
        }
        if (VAL_TAG(subject) == XS_MAP) {
            Value *tag = map_get(subject->map, "_tag");
            if (!tag || VAL_TAG(tag) != XS_STR ||
                strcmp(tag->s, pi->ctor.variant) != 0)
                return 0;
            if (pi->ctor.argc > 0) {
                Value *val = map_get(subject->map, "_val");
                if (!val) return pi->ctor.argc == 0;
                if (pi->ctor.argc == 1) {
                    return pat_match(prog, pi->ctor.sub_indices[0],
                                     val, bindings, nbindings, max_binds);
                }
                if (VAL_TAG(val) == XS_ARRAY || VAL_TAG(val) == XS_TUPLE) {
                    for (int i = 0; i < pi->ctor.argc && i < val->arr->len; i++) {
                        if (!pat_match(prog, pi->ctor.sub_indices[i],
                                       val->arr->items[i], bindings, nbindings, max_binds))
                            return 0;
                    }
                }
            }
            return 1;
        }
        return 0;
    }
    case PAT_RANGE: {
        if (VAL_TAG(subject) != XS_INT) return 0;
        int64_t v = VAL_INT(subject);
        if (v < pi->range.lo) return 0;
        if (pi->range.inclusive ? v > pi->range.hi : v >= pi->range.hi) return 0;
        return 1;
    }
    case PAT_OR: {
        int saved = *nbindings;
        if (pat_match(prog, pi->or_pat.left_idx, subject, bindings, nbindings, max_binds))
            return 1;
        *nbindings = saved;
        return pat_match(prog, pi->or_pat.right_idx, subject, bindings, nbindings, max_binds);
    }
    case PAT_GUARD:
        return pat_match(prog, pi->guard.pat_idx, subject, bindings, nbindings, max_binds);
    case PAT_CAPTURE: {
        if (!pat_match(prog, pi->capture.sub_idx, subject, bindings, nbindings, max_binds))
            return 0;
        if (*nbindings < max_binds) {
            bindings[*nbindings] = value_incref(subject);
            (*nbindings)++;
        }
        return 1;
    }
    case PAT_SLICE: {
        if (VAL_TAG(subject) != XS_ARRAY && VAL_TAG(subject) != XS_TUPLE) return 0;
        if (subject->arr->len < pi->slice.count) return 0;
        for (int i = 0; i < pi->slice.count; i++) {
            if (!pat_match(prog, pi->slice.sub_indices[i],
                           subject->arr->items[i], bindings, nbindings, max_binds))
                return 0;
        }
        if (pi->slice.rest_name && *nbindings < max_binds) {
            Value *rest = xs_array_new();
            for (int i = pi->slice.count; i < subject->arr->len; i++)
                array_push(rest->arr, value_incref(subject->arr->items[i]));
            bindings[*nbindings] = rest;
            (*nbindings)++;
        }
        return 1;
    }
    case PAT_STRUCT: {
        if (VAL_TAG(subject) != XS_MAP && VAL_TAG(subject) != XS_STRUCT_VAL) return 0;
        XSMap *m = VAL_TAG(subject) == XS_MAP ? subject->map : subject->st->fields;
        for (int i = 0; i < pi->struct_pat.nfields; i++) {
            Value *fv = map_get(m, pi->struct_pat.field_names[i]);
            if (!fv) fv = (Value *)XS_NULL_VAL;
            if (!pat_match(prog, pi->struct_pat.sub_indices[i],
                           fv, bindings, nbindings, max_binds))
                return 0;
        }
        return 1;
    }
    }
    return 0;
}

typedef struct {
    int  active;
    int  depth;
    int  locals_start;
} DestructCtx;

static int destructure_value(Value *val, Node *pattern, Value **out_binds,
                              char **out_names, int *nbinds, int max) {
    if (!pattern || *nbinds >= max) return 1;

    switch (VAL_TAG(pattern)) {
    case NODE_PAT_WILD:
        return 1;
    case NODE_PAT_IDENT:
        if (*nbinds < max) {
            out_binds[*nbinds] = value_incref(val);
            out_names[*nbinds] = pattern->pat_ident.name;
            (*nbinds)++;
        }
        return 1;
    case NODE_PAT_TUPLE:
    case NODE_PAT_SLICE: {
        NodeList *elems = VAL_TAG(pattern) == NODE_PAT_TUPLE
            ? &pattern->pat_tuple.elems : &pattern->pat_slice.elems;
        if ((VAL_TAG(val) != XS_ARRAY && VAL_TAG(val) != XS_TUPLE) ||
            val->arr->len < elems->len)
            return 0;
        for (int i = 0; i < elems->len; i++) {
            if (!destructure_value(val->arr->items[i], elems->items[i],
                                   out_binds, out_names, nbinds, max))
                return 0;
        }
        if (VAL_TAG(pattern) == NODE_PAT_SLICE && pattern->pat_slice.rest) {
            if (*nbinds < max) {
                Value *rest = xs_array_new();
                for (int i = elems->len; i < val->arr->len; i++)
                    array_push(rest->arr, value_incref(val->arr->items[i]));
                out_binds[*nbinds] = rest;
                out_names[*nbinds] = pattern->pat_slice.rest;
                (*nbinds)++;
            }
        }
        return 1;
    }
    case NODE_PAT_STRUCT: {
        XSMap *m = NULL;
        if (VAL_TAG(val) == XS_MAP) m = val->map;
        else if (VAL_TAG(val) == XS_STRUCT_VAL) m = val->st->fields;
        if (!m) return 0;
        for (int i = 0; i < pattern->pat_struct.fields.len; i++) {
            const char *key = pattern->pat_struct.fields.items[i].key;
            Node *sub = pattern->pat_struct.fields.items[i].val;
            Value *fv = map_get(m, key);
            if (!fv) fv = (Value *)XS_NULL_VAL;
            if (sub && VAL_TAG(sub) == NODE_PAT_IDENT && sub->pat_ident.name) {
                if (*nbinds < max) {
                    out_binds[*nbinds] = value_incref(fv);
                    out_names[*nbinds] = sub->pat_ident.name;
                    (*nbinds)++;
                }
            } else if (sub) {
                if (!destructure_value(fv, sub, out_binds, out_names, nbinds, max))
                    return 0;
            }
        }
        return 1;
    }
    case NODE_PAT_CAPTURE: {
        if (*nbinds < max) {
            out_binds[*nbinds] = value_incref(val);
            out_names[*nbinds] = pattern->pat_capture.name;
            (*nbinds)++;
        }
        return destructure_value(val, pattern->pat_capture.pattern,
                                 out_binds, out_names, nbinds, max);
    }
    default:
        return 1;
    }
}

typedef struct {
    char     *name;
    Value    *methods;
    Value    *fields;
    Value   **bases;
    int       nbases;
    int       refcount;
} VMClass;

typedef struct {
    VMClass  *cls;
    Value    *fields;
    Value    *methods;
    int       refcount;
} VMInst;

static Value *vm_class_new_inst(VMClass *cls, Value **args, int argc) {
    Value *inst = xs_map_new();
    map_set(inst->map, "__class", xs_str(cls->name));
    if (cls->fields && VAL_TAG(cls->fields) == XS_MAP) {
        int nkeys = 0;
        char **keys = map_keys(cls->fields->map, &nkeys);
        for (int i = 0; i < nkeys; i++) {
            Value *def = map_get(cls->fields->map, keys[i]);
            if (i < argc)
                map_set(inst->map, keys[i], value_incref(args[i]));
            else if (def)
                map_set(inst->map, keys[i], value_incref(def));
            else
                map_set(inst->map, keys[i], value_incref(XS_NULL_VAL));
            free(keys[i]);
        }
        free(keys);
    }
    if (cls->methods && VAL_TAG(cls->methods) == XS_MAP) {
        int nkeys = 0;
        char **keys = map_keys(cls->methods->map, &nkeys);
        for (int i = 0; i < nkeys; i++) {
            Value *m = map_get(cls->methods->map, keys[i]);
            if (m) map_set(inst->map, keys[i], value_incref(m));
            free(keys[i]);
        }
        free(keys);
    }
    return inst;
}

typedef struct {
    Value   *saved_stack;
    int      saved_sp;
    int      state;
    Value   *last_yield;
    int      done;
} GenState;

typedef struct {
    PatProgram  prog;
    int         root_idx;
    int         arm_index;
} CachedPattern;

static CachedPattern pattern_cache[VM_PATTERN_CACHE_SIZE];
static int pattern_cache_count = 0;

static void pattern_cache_init(void) {
    pattern_cache_count = 0;
    memset(pattern_cache, 0, sizeof pattern_cache);
}

static int pattern_cache_lookup(int arm_index) {
    for (int i = 0; i < pattern_cache_count; i++) {
        if (pattern_cache[i].arm_index == arm_index)
            return i;
    }
    return -1;
}

static int pattern_cache_store(PatProgram prog, int root_idx, int arm_index) {
    if (pattern_cache_count >= VM_PATTERN_CACHE_SIZE)
        return -1;
    int slot = pattern_cache_count++;
    pattern_cache[slot].prog = prog;
    pattern_cache[slot].root_idx = root_idx;
    pattern_cache[slot].arm_index = arm_index;
    return slot;
}

typedef struct {
    int handler_ip;
    int stack_depth;
    int frame_idx;
    char effect_name[128];
} EffectHandler;

#define MAX_EFFECT_HANDLERS 32

static EffectHandler effect_handlers[MAX_EFFECT_HANDLERS];
static int n_effect_handlers = 0;

void vm_push_effect_handler(const char *name, int handler_ip,
                            int stack_depth, int frame_idx) {
    if (n_effect_handlers >= MAX_EFFECT_HANDLERS) return;
    EffectHandler *h = &effect_handlers[n_effect_handlers++];
    strncpy(h->effect_name, name, sizeof h->effect_name - 1);
    h->handler_ip = handler_ip;
    h->stack_depth = stack_depth;
    h->frame_idx = frame_idx;
}

void vm_pop_effect_handler(void) {
    if (n_effect_handlers > 0)
        n_effect_handlers--;
}

int vm_find_effect_handler(const char *name) {
    for (int i = n_effect_handlers - 1; i >= 0; i--) {
        if (strncmp(effect_handlers[i].effect_name, name,
                    sizeof effect_handlers[i].effect_name) == 0)
            return i;
    }
    return -1;
}

typedef struct {
    int  pc;
    int  local_count;
    int  try_depth;
} CheckPoint;

#define MAX_CHECKPOINTS 64
static CheckPoint checkpoints[MAX_CHECKPOINTS];
static int n_checkpoints = 0;

void vm_save_checkpoint(int pc, int locals, int try_depth) {
    if (n_checkpoints >= MAX_CHECKPOINTS) return;
    CheckPoint *cp = &checkpoints[n_checkpoints++];
    cp->pc = pc;
    cp->local_count = locals;
    cp->try_depth = try_depth;
}

int vm_restore_checkpoint(int *pc, int *locals, int *try_depth) {
    if (n_checkpoints <= 0) return 0;
    CheckPoint *cp = &checkpoints[--n_checkpoints];
    *pc = cp->pc;
    *locals = cp->local_count;
    *try_depth = cp->try_depth;
    return 1;
}

typedef struct {
    uint64_t total_instrs;
    uint64_t match_attempts;
    uint64_t match_hits;
    uint64_t closure_creates;
    uint64_t class_creates;
    uint64_t try_enters;
    uint64_t exceptions_thrown;
    uint64_t yields_done;
    uint64_t effect_calls;
    uint64_t destructures;
} VMStats;

static VMStats vm_stats = {0};

void vm_stats_reset(void) {
    memset(&vm_stats, 0, sizeof vm_stats);
}

void vm_stats_get(uint64_t *total, uint64_t *matches, uint64_t *closures,
                  uint64_t *classes, uint64_t *trys, uint64_t *throws) {
    if (total)    *total    = vm_stats.total_instrs;
    if (matches)  *matches  = vm_stats.match_attempts;
    if (closures) *closures = vm_stats.closure_creates;
    if (classes)  *classes  = vm_stats.class_creates;
    if (trys)     *trys     = vm_stats.try_enters;
    if (throws)   *throws   = vm_stats.exceptions_thrown;
}

static Value *vm_match_exec(Value *subject, Node *match_node) {
    if (!match_node || VAL_TAG(match_node) != NODE_MATCH) return value_incref(XS_NULL_VAL);
    vm_stats.match_attempts++;

    PatProgram prog = pat_prog_new();
    int n_arms = match_node->match.arms.len;

    int *root_indices = xs_malloc((size_t)n_arms * sizeof(int));
    for (int i = 0; i < n_arms; i++)
        root_indices[i] = compile_pattern(&prog, match_node->match.arms.items[i].pattern);

    Value *bindings[64];
    int nbindings = 0;
    Value *result = NULL;

    for (int i = 0; i < n_arms; i++) {
        nbindings = 0;
        if (pat_match(&prog, root_indices[i], subject, bindings, &nbindings, 64)) {
            vm_stats.match_hits++;
            result = value_incref(XS_NULL_VAL);
            for (int b = 0; b < nbindings; b++)
                value_decref(bindings[b]);
            break;
        }
        for (int b = 0; b < nbindings; b++)
            value_decref(bindings[b]);
    }

    free(root_indices);
    pat_prog_free(&prog);
    return result ? result : value_incref(XS_NULL_VAL);
}

typedef struct UpvalueChain {
    int slot;
    int is_local;
    struct UpvalueChain *next;
} UpvalueChain;

static UpvalueChain *uv_chain_new(int slot, int is_local) {
    UpvalueChain *c = xs_malloc(sizeof(UpvalueChain));
    c->slot = slot;
    c->is_local = is_local;
    c->next = NULL;
    return c;
}

static void uv_chain_free(UpvalueChain *c) {
    while (c) {
        UpvalueChain *next = c->next;
        free(c);
        c = next;
    }
}

static int uv_chain_resolve(UpvalueChain *c, int slot) {
    while (c) {
        if (c->slot == slot) return 1;
        c = c->next;
    }
    return 0;
}

typedef struct ExnTable {
    int try_start;
    int try_end;
    int catch_start;
    int finally_start;
    int handler_local;
} ExnTable;

#define MAX_EXN_ENTRIES 128

typedef struct {
    ExnTable entries[MAX_EXN_ENTRIES];
    int      len;
} ExnTableList;

static ExnTableList g_exn_tables = {.len = 0};

void vm_register_exn_handler(int try_start, int try_end,
                              int catch_start, int finally_start,
                              int handler_local) {
    if (g_exn_tables.len >= MAX_EXN_ENTRIES) return;
    ExnTable *e = &g_exn_tables.entries[g_exn_tables.len++];
    e->try_start = try_start;
    e->try_end = try_end;
    e->catch_start = catch_start;
    e->finally_start = finally_start;
    e->handler_local = handler_local;
}

int vm_find_exn_handler(int pc) {
    for (int i = g_exn_tables.len - 1; i >= 0; i--) {
        ExnTable *e = &g_exn_tables.entries[i];
        if (pc >= e->try_start && pc < e->try_end)
            return i;
    }
    return -1;
}

void vm_clear_exn_tables(void) {
    g_exn_tables.len = 0;
}

typedef struct {
    int   depth;
    int   max_depth;
    int   n_closures;
    int   n_deferred;
    int   has_error;
    char  error_msg[256];
} VMDebugInfo;

static VMDebugInfo debug_info = {0};

void vm_debug_info_get(int *depth, int *closures, int *deferred) {
    if (depth)    *depth    = debug_info.depth;
    if (closures) *closures = debug_info.n_closures;
    if (deferred) *deferred = debug_info.n_deferred;
}

void vm_debug_error(const char *msg) {
    debug_info.has_error = 1;
    strncpy(debug_info.error_msg, msg, sizeof debug_info.error_msg - 1);
}

void vm_debug_clear(void) {
    memset(&debug_info, 0, sizeof debug_info);
}

static int match_struct_pattern(Value *subject, Node *pat, Value **binds,
                                 char **names, int *nbinds) {
    if (!pat || VAL_TAG(pat) != NODE_PAT_STRUCT) return 0;
    XSMap *m = NULL;
    if (VAL_TAG(subject) == XS_MAP) m = subject->map;
    else if (VAL_TAG(subject) == XS_STRUCT_VAL && subject->st) m = subject->st->fields;
    if (!m) return 0;

    for (int i = 0; i < pat->pat_struct.fields.len; i++) {
        const char *key = pat->pat_struct.fields.items[i].key;
        Node *sub = pat->pat_struct.fields.items[i].val;
        Value *fv = map_get(m, key);
        if (!fv) return 0;

        if (sub && VAL_TAG(sub) == NODE_PAT_LIT) {
            int match = 0;
            switch (sub->pat_lit.tag) {
            case 0: match = VAL_TAG(fv) == XS_INT && VAL_INT(fv) == sub->pat_lit.ival; break;
            case 1: match = VAL_TAG(fv) == XS_FLOAT && fv->f == sub->pat_lit.fval; break;
            case 2: match = VAL_TAG(fv) == XS_STR && strcmp(fv->s, sub->pat_lit.sval) == 0; break;
            case 3: match = value_truthy(fv) == sub->pat_lit.bval; break;
            default: match = VAL_TAG(fv) == XS_NULL; break;
            }
            if (!match) return 0;
        } else if (sub && VAL_TAG(sub) == NODE_PAT_IDENT && sub->pat_ident.name) {
            if (*nbinds < 64) {
                binds[*nbinds] = value_incref(fv);
                names[*nbinds] = sub->pat_ident.name;
                (*nbinds)++;
            }
        }
    }
    return 1;
}

static int match_tuple_pattern(Value *subject, Node *pat, Value **binds,
                                char **names, int *nbinds) {
    if (!pat || VAL_TAG(pat) != NODE_PAT_TUPLE) return 0;
    if (VAL_TAG(subject) != XS_ARRAY && VAL_TAG(subject) != XS_TUPLE) return 0;
    if (subject->arr->len != pat->pat_tuple.elems.len) return 0;

    for (int i = 0; i < pat->pat_tuple.elems.len; i++) {
        Node *sub = pat->pat_tuple.elems.items[i];
        Value *elem = subject->arr->items[i];
        if (VAL_TAG(sub) == NODE_PAT_IDENT && sub->pat_ident.name) {
            if (*nbinds < 64) {
                binds[*nbinds] = value_incref(elem);
                names[*nbinds] = sub->pat_ident.name;
                (*nbinds)++;
            }
        } else if (VAL_TAG(sub) == NODE_PAT_LIT) {
            int match = 0;
            switch (sub->pat_lit.tag) {
            case 0: match = VAL_TAG(elem) == XS_INT && VAL_INT(elem) == sub->pat_lit.ival; break;
            case 1: match = VAL_TAG(elem) == XS_FLOAT && elem->f == sub->pat_lit.fval; break;
            case 2: match = VAL_TAG(elem) == XS_STR && strcmp(elem->s, sub->pat_lit.sval) == 0; break;
            case 3: match = value_truthy(elem) == sub->pat_lit.bval; break;
            default: match = VAL_TAG(elem) == XS_NULL; break;
            }
            if (!match) return 0;
        } else if (VAL_TAG(sub) == NODE_PAT_WILD) {
            /* ok */
        } else if (VAL_TAG(sub) == NODE_PAT_TUPLE) {
            if (!match_tuple_pattern(elem, sub, binds, names, nbinds))
                return 0;
        }
    }
    return 1;
}

static int match_enum_pattern(Value *subject, const char *variant, int argc,
                               Node **args, Value **binds, char **names, int *nbinds) {
    const char *tag_str = NULL;
    Value *val_field = NULL;
    if (VAL_TAG(subject) == XS_ENUM_VAL) {
        tag_str = subject->en->variant;
        if (subject->en->arr_data && subject->en->arr_data->len > 0) {
            if (subject->en->arr_data->len == 1)
                val_field = subject->en->arr_data->items[0];
        }
    } else if (VAL_TAG(subject) == XS_MAP) {
        Value *tv = map_get(subject->map, "_tag");
        if (tv && VAL_TAG(tv) == XS_STR) tag_str = tv->s;
        val_field = map_get(subject->map, "_val");
    }
    if (!tag_str) return 0;

    const char *last = strrchr(variant, ':');
    const char *cmp = (last && last > variant) ? last + 1 : variant;
    if (strcmp(tag_str, cmp) != 0) return 0;

    if (argc > 0 && val_field) {
        if (argc == 1 && args[0]) {
            if (VAL_TAG(args[0]) == NODE_PAT_IDENT && args[0]->pat_ident.name) {
                if (*nbinds < 64) {
                    binds[*nbinds] = value_incref(val_field);
                    names[*nbinds] = args[0]->pat_ident.name;
                    (*nbinds)++;
                }
            }
        } else if (VAL_TAG(val_field) == XS_ARRAY || VAL_TAG(val_field) == XS_TUPLE) {
            for (int i = 0; i < argc && i < val_field->arr->len; i++) {
                Node *ap = args[i];
                if (ap && VAL_TAG(ap) == NODE_PAT_IDENT && ap->pat_ident.name) {
                    if (*nbinds < 64) {
                        binds[*nbinds] = value_incref(val_field->arr->items[i]);
                        names[*nbinds] = ap->pat_ident.name;
                        (*nbinds)++;
                    }
                }
            }
        }
    }
    return 1;
}

static Value *do_closure_call(VM *vm, Value *closure, Value **args, int argc) {
    (void)vm; (void)closure; (void)args; (void)argc;
    vm_stats.closure_creates++;
    return value_incref(XS_NULL_VAL);
}

static Value *do_class_instantiate(VM *vm, const char *cls_name,
                                    Value *cls_val, Value **args, int argc) {
    (void)vm;
    vm_stats.class_creates++;
    if (VAL_TAG(cls_val) == XS_MAP) {
        Value *fields = map_get(cls_val->map, "__fields");
        Value *methods = map_get(cls_val->map, "__methods");
        Value *inst = xs_map_new();
        map_set(inst->map, "__class", xs_str(cls_name));
        if (fields && VAL_TAG(fields) == XS_MAP) {
            int nkeys = 0;
            char **keys = map_keys(fields->map, &nkeys);
            for (int i = 0; i < nkeys; i++) {
                Value *def = map_get(fields->map, keys[i]);
                if (i < argc)
                    map_set(inst->map, keys[i], value_incref(args[i]));
                else if (def)
                    map_set(inst->map, keys[i], value_incref(def));
                free(keys[i]);
            }
            free(keys);
        }
        if (methods && VAL_TAG(methods) == XS_MAP) {
            int nkeys = 0;
            char **keys = map_keys(methods->map, &nkeys);
            for (int i = 0; i < nkeys; i++) {
                Value *m = map_get(methods->map, keys[i]);
                if (m) map_set(inst->map, keys[i], value_incref(m));
                free(keys[i]);
            }
            free(keys);
        }
        Value *init = map_get(inst->map, "init");
        if (!init && methods)
            init = map_get(methods->map, "init");
        (void)init;
        return inst;
    }
    return value_incref(XS_NULL_VAL);
}

static int vm_try_catch(VM *vm, Value *exception, int *caught) {
    (void)vm;
    vm_stats.exceptions_thrown++;
    *caught = 0;
    return 0;
}

static Value *vm_yield_value(VM *vm, Value *val) {
    (void)vm;
    vm_stats.yields_done++;
    return value_incref(val);
}

static int vm_handle_effect(VM *vm, const char *effect_name,
                             Value **args, int argc, Value **result) {
    (void)vm; (void)args; (void)argc;
    vm_stats.effect_calls++;
    int idx = vm_find_effect_handler(effect_name);
    if (idx < 0) {
        *result = value_incref(XS_NULL_VAL);
        return 0;
    }
    *result = value_incref(XS_NULL_VAL);
    return 1;
}

typedef struct {
    int generator;
    int done;
    Value *last_value;
    int saved_pc;
    int saved_locals;
} GenCtx;

#define MAX_GENERATORS 256
static GenCtx generators[MAX_GENERATORS];
static int n_generators = 0;

int vm_gen_create(int start_pc, int nlocals) {
    if (n_generators >= MAX_GENERATORS) return -1;
    int id = n_generators++;
    generators[id].generator = id;
    generators[id].done = 0;
    generators[id].last_value = value_incref(XS_NULL_VAL);
    generators[id].saved_pc = start_pc;
    generators[id].saved_locals = nlocals;
    return id;
}

int vm_gen_next(int id, Value *send_val, Value **out) {
    if (id < 0 || id >= n_generators) {
        *out = value_incref(XS_NULL_VAL);
        return 0;
    }
    GenCtx *g = &generators[id];
    if (g->done) {
        *out = value_incref(XS_NULL_VAL);
        return 0;
    }
    (void)send_val;
    *out = value_incref(g->last_value);
    return 1;
}

void vm_gen_set_done(int id) {
    if (id >= 0 && id < n_generators)
        generators[id].done = 1;
}

void vm_gen_free_all(void) {
    for (int i = 0; i < n_generators; i++) {
        if (generators[i].last_value)
            value_decref(generators[i].last_value);
    }
    n_generators = 0;
}

typedef struct {
    Value *key;
    Value *value;
    int    used;
} WeakEntry;

#define WEAK_MAP_SIZE 512

typedef struct {
    WeakEntry entries[WEAK_MAP_SIZE];
    int       count;
} WeakMap;

static WeakMap *weak_map_new(void) {
    WeakMap *wm = xs_calloc(1, sizeof(WeakMap));
    return wm;
}

static void weak_map_free(WeakMap *wm) {
    if (!wm) return;
    for (int i = 0; i < WEAK_MAP_SIZE; i++) {
        if (wm->entries[i].used) {
            value_decref(wm->entries[i].key);
            value_decref(wm->entries[i].value);
        }
    }
    free(wm);
}

static uint32_t weak_hash(Value *key) {
    uint64_t h = (uint64_t)(uintptr_t)key;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return (uint32_t)(h % WEAK_MAP_SIZE);
}

static void weak_map_set(WeakMap *wm, Value *key, Value *value) {
    uint32_t idx = weak_hash(key);
    for (int i = 0; i < WEAK_MAP_SIZE; i++) {
        uint32_t slot = (idx + (uint32_t)i) % WEAK_MAP_SIZE;
        if (!wm->entries[slot].used) {
            wm->entries[slot].key = value_incref(key);
            wm->entries[slot].value = value_incref(value);
            wm->entries[slot].used = 1;
            wm->count++;
            return;
        }
        if (wm->entries[slot].used && value_equal(wm->entries[slot].key, key)) {
            value_decref(wm->entries[slot].value);
            wm->entries[slot].value = value_incref(value);
            return;
        }
    }
}

static Value *weak_map_get(WeakMap *wm, Value *key) {
    uint32_t idx = weak_hash(key);
    for (int i = 0; i < WEAK_MAP_SIZE; i++) {
        uint32_t slot = (idx + (uint32_t)i) % WEAK_MAP_SIZE;
        if (!wm->entries[slot].used) return NULL;
        if (value_equal(wm->entries[slot].key, key))
            return wm->entries[slot].value;
    }
    return NULL;
}

static int weak_map_has(WeakMap *wm, Value *key) {
    return weak_map_get(wm, key) != NULL;
}

static void weak_map_del(WeakMap *wm, Value *key) {
    uint32_t idx = weak_hash(key);
    for (int i = 0; i < WEAK_MAP_SIZE; i++) {
        uint32_t slot = (idx + (uint32_t)i) % WEAK_MAP_SIZE;
        if (!wm->entries[slot].used) return;
        if (value_equal(wm->entries[slot].key, key)) {
            value_decref(wm->entries[slot].key);
            value_decref(wm->entries[slot].value);
            wm->entries[slot].used = 0;
            wm->count--;
            return;
        }
    }
}

typedef struct {
    int      active;
    int      suspended;
    int      completed;
    Value   *result;
    int      priority;
} VMFiber;

#define MAX_FIBERS 128
static VMFiber fibers[MAX_FIBERS];
static int n_fibers = 0;
static int current_fiber = -1;

int vm_fiber_create(void) {
    if (n_fibers >= MAX_FIBERS) return -1;
    int id = n_fibers++;
    fibers[id].active = 1;
    fibers[id].suspended = 0;
    fibers[id].completed = 0;
    fibers[id].result = value_incref(XS_NULL_VAL);
    fibers[id].priority = 0;
    return id;
}

void vm_fiber_suspend(int id) {
    if (id >= 0 && id < n_fibers)
        fibers[id].suspended = 1;
}

void vm_fiber_resume(int id) {
    if (id >= 0 && id < n_fibers)
        fibers[id].suspended = 0;
}

int vm_fiber_is_done(int id) {
    if (id < 0 || id >= n_fibers) return 1;
    return fibers[id].completed;
}

void vm_fiber_complete(int id, Value *result) {
    if (id < 0 || id >= n_fibers) return;
    fibers[id].completed = 1;
    if (fibers[id].result) value_decref(fibers[id].result);
    fibers[id].result = result ? value_incref(result) : value_incref(XS_NULL_VAL);
}

Value *vm_fiber_result(int id) {
    if (id < 0 || id >= n_fibers) return value_incref(XS_NULL_VAL);
    return value_incref(fibers[id].result);
}

void vm_fibers_free(void) {
    for (int i = 0; i < n_fibers; i++) {
        if (fibers[i].result) value_decref(fibers[i].result);
    }
    n_fibers = 0;
    current_fiber = -1;
}

int vm_fiber_schedule(void) {
    for (int i = 0; i < n_fibers; i++) {
        int idx = (current_fiber + 1 + i) % n_fibers;
        if (fibers[idx].active && !fibers[idx].suspended && !fibers[idx].completed) {
            current_fiber = idx;
            return idx;
        }
    }
    return -1;
}

typedef struct {
    char    *name;
    Value   *state;
    int      running;
    int      fiber_id;
} VMActorState;

#define MAX_ACTORS 64
static VMActorState actors[MAX_ACTORS];
static int n_actors = 0;

int vm_actor_create(const char *name, Value *initial_state) {
    if (n_actors >= MAX_ACTORS) return -1;
    int id = n_actors++;
    actors[id].name = xs_strdup(name);
    actors[id].state = initial_state ? value_incref(initial_state) : value_incref(XS_NULL_VAL);
    actors[id].running = 1;
    actors[id].fiber_id = vm_fiber_create();
    return id;
}

Value *vm_actor_get_state(int id) {
    if (id < 0 || id >= n_actors) return value_incref(XS_NULL_VAL);
    return value_incref(actors[id].state);
}

void vm_actor_set_state(int id, Value *state) {
    if (id < 0 || id >= n_actors) return;
    value_decref(actors[id].state);
    actors[id].state = value_incref(state);
}

int vm_actor_send(int id, Value *message) {
    (void)id; (void)message;
    return 0;
}

void vm_actors_free(void) {
    for (int i = 0; i < n_actors; i++) {
        free(actors[i].name);
        value_decref(actors[i].state);
    }
    n_actors = 0;
}

typedef struct {
    int      slot;
    int      target_depth;
    Value   *value;
} DeferredAction;

#define MAX_DEFERRED_ACTIONS 256
static DeferredAction deferred[MAX_DEFERRED_ACTIONS];
static int n_deferred = 0;

void vm_defer_push(int slot, int depth, Value *val) {
    if (n_deferred >= MAX_DEFERRED_ACTIONS) return;
    deferred[n_deferred].slot = slot;
    deferred[n_deferred].target_depth = depth;
    deferred[n_deferred].value = val ? value_incref(val) : NULL;
    n_deferred++;
}

int vm_defer_count(void) {
    return n_deferred;
}

Value *vm_defer_pop(void) {
    if (n_deferred <= 0) return NULL;
    DeferredAction *d = &deferred[--n_deferred];
    return d->value;
}

void vm_deferred_free(void) {
    for (int i = 0; i < n_deferred; i++) {
        if (deferred[i].value) value_decref(deferred[i].value);
    }
    n_deferred = 0;
}

typedef struct {
    char *module_name;
    Value *exports;
} ModuleEntry;

#define MAX_MODULE_CACHE 128
static ModuleEntry module_cache[MAX_MODULE_CACHE];
static int n_module_cache = 0;

Value *vm_module_lookup(const char *name) {
    for (int i = 0; i < n_module_cache; i++) {
        if (strcmp(module_cache[i].module_name, name) == 0)
            return value_incref(module_cache[i].exports);
    }
    return NULL;
}

void vm_module_register(const char *name, Value *exports) {
    if (n_module_cache >= MAX_MODULE_CACHE) return;
    module_cache[n_module_cache].module_name = xs_strdup(name);
    module_cache[n_module_cache].exports = value_incref(exports);
    n_module_cache++;
}

void vm_modules_free(void) {
    for (int i = 0; i < n_module_cache; i++) {
        free(module_cache[i].module_name);
        value_decref(module_cache[i].exports);
    }
    n_module_cache = 0;
}

void vm_complete_init(void) {
    pattern_cache_init();
    vm_stats_reset();
    vm_clear_exn_tables();
    vm_debug_clear();
    vm_gen_free_all();
    vm_fibers_free();
    vm_actors_free();
    vm_deferred_free();
    vm_modules_free();
    n_effect_handlers = 0;
    n_checkpoints = 0;
}

void vm_complete_cleanup(void) {
    for (int i = 0; i < pattern_cache_count; i++)
        pat_prog_free(&pattern_cache[i].prog);
    pattern_cache_count = 0;
    vm_gen_free_all();
    vm_fibers_free();
    vm_actors_free();
    vm_deferred_free();
    vm_modules_free();
}
