#include "vm/compiler.h"
#include "core/value.h"
#include "core/xs_bigint.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define MAX_LOCALS   256
#define MAX_UPVALUES 256

typedef struct Local {
    char  name[128];
    int   slot;
    int   depth;
    int   is_captured;
} Local;

typedef struct CompilerScope {
    XSProto              *proto;
    Local                 locals[MAX_LOCALS];
    int                   n_locals;
    int                   scope_depth;
    UVDesc                uv_descs[MAX_UPVALUES];
    int                   n_upvalues;
    struct CompilerScope *enclosing;
    /* actor method state write-back */
    char                **actor_state_names;
    int                   actor_nstate;
} CompilerScope;

#define MAX_LOOP_DEPTH 64
#define MAX_BREAK_PATCHES 256

typedef struct {
    int break_patches[MAX_BREAK_PATCHES];
    int n_break_patches;
    int continue_patches[MAX_BREAK_PATCHES];
    int n_continue_patches;
    int continue_target;  /* ip to jump for continue */
    char label[64];       /* loop label (empty string if none) */
} LoopCtx;

typedef struct {
    CompilerScope *current;
    int            n_errors;
    LoopCtx        loop_stack[MAX_LOOP_DEPTH];
    int            loop_depth;
} Compiler;

static void scope_push(Compiler *c, CompilerScope *s, XSProto *proto) {
    memset(s, 0, sizeof *s);
    s->proto     = proto;
    s->enclosing = c->current;
    c->current   = s;
}

static void scope_pop(Compiler *c) {
    c->current = c->current->enclosing;
}

static void emit(Compiler *c, Instruction instr);

static void scope_begin(Compiler *c) {
    c->current->scope_depth++;
}

static void scope_end(Compiler *c) {
    CompilerScope *s = c->current;
    s->scope_depth--;
    /* If any local in the popped scope was captured by a nested closure,
       emit OP_CLOSE_UPVALUES so the upvalue takes its current value
       before the slot gets reused (e.g. the next loop iteration). */
    int min_captured_slot = -1;
    while (s->n_locals > 0 &&
           s->locals[s->n_locals - 1].depth > s->scope_depth) {
        Local *l = &s->locals[s->n_locals - 1];
        if (l->is_captured && (min_captured_slot < 0 || l->slot < min_captured_slot))
            min_captured_slot = l->slot;
        s->n_locals--;
    }
    if (min_captured_slot >= 0) {
        emit(c, MAKE_A(OP_CLOSE_UPVALUES, 0, (uint16_t)min_captured_slot));
    }
}

static int local_add(CompilerScope *scope, const char *name) {
    if (scope->n_locals >= MAX_LOCALS) {
        fprintf(stderr, "too many locals\n");
        return 0;
    }
    Local *l = &scope->locals[scope->n_locals++];
    strncpy(l->name, name, sizeof(l->name) - 1);
    l->name[sizeof(l->name) - 1] = '\0';
    l->slot        = scope->proto->nlocals++;
    l->depth       = scope->scope_depth;
    l->is_captured = 0;
    return l->slot;
}

static int local_resolve(CompilerScope *scope, const char *name) {
    for (int i = scope->n_locals - 1; i >= 0; i--)
        if (strcmp(scope->locals[i].name, name) == 0)
            return scope->locals[i].slot;
    return -1;
}

/* upvalue tracking */

static int upvalue_add(CompilerScope *scope, int is_local, int index) {
    for (int i = 0; i < scope->n_upvalues; i++)
        if (scope->uv_descs[i].is_local == is_local &&
            scope->uv_descs[i].index    == index)
            return i;
    if (scope->n_upvalues >= MAX_UPVALUES) {
        fprintf(stderr, "too many upvalues\n");
        return 0;
    }
    scope->uv_descs[scope->n_upvalues].is_local = is_local;
    scope->uv_descs[scope->n_upvalues].index    = index;
    return scope->n_upvalues++;
}

static int upvalue_resolve(CompilerScope *scope, const char *name) {
    if (!scope->enclosing) return -1;
    int slot = local_resolve(scope->enclosing, name);
    if (slot >= 0) {
        for (int i = 0; i < scope->enclosing->n_locals; i++) {
            if (scope->enclosing->locals[i].slot == slot) {
                scope->enclosing->locals[i].is_captured = 1;
                break;
            }
        }
        return upvalue_add(scope, 1, slot);
    }
    int uv = upvalue_resolve(scope->enclosing, name);
    if (uv >= 0)
        return upvalue_add(scope, 0, uv);
    return -1;
}

static void emit(Compiler *c, Instruction instr) {
    chunk_write(&c->current->proto->chunk, instr);
}

static void emit_a(Compiler *c, Opcode op, int bx) {
    emit(c, MAKE_A(op, 0, (uint16_t)(unsigned)bx));
}

static void emit_const(Compiler *c, Value *v) {
    int idx = chunk_add_const(&c->current->proto->chunk, v);
    value_decref(v);
    emit_a(c, OP_PUSH_CONST, idx);
}

static int emit_jump(Compiler *c, Opcode op) {
    int idx = c->current->proto->chunk.len;
    emit(c, MAKE_A(op, 0, 0));
    return idx;
}

static void patch_jump(Compiler *c, int instr_idx) {
    int offset = c->current->proto->chunk.len - instr_idx - 1;
    Instruction *ip = &c->current->proto->chunk.code[instr_idx];
    *ip = (*ip & 0x0000FFFFU) | ((Instruction)(uint16_t)(int16_t)offset << 16);
}

static int emit_global_name(Compiler *c, const char *name) {
    Value *v = xs_str(name);
    int idx  = chunk_add_const(&c->current->proto->chunk, v);
    value_decref(v);
    return idx;
}

static int local_add_hidden(Compiler *c) {
    static int _hidden = 0;
    char buf[32];
    snprintf(buf, sizeof buf, "__h%d", _hidden++);
    return local_add(c->current, buf);
}

static void loop_push_label(Compiler *c, int continue_target, const char *label) {
    if (c->loop_depth >= MAX_LOOP_DEPTH) return;
    LoopCtx *lc = &c->loop_stack[c->loop_depth++];
    lc->n_break_patches = 0;
    lc->n_continue_patches = 0;
    lc->continue_target = continue_target;
    if (label) { strncpy(lc->label, label, 63); lc->label[63] = '\0'; }
    else lc->label[0] = '\0';
}

static void loop_pop_patch_breaks(Compiler *c) {
    if (c->loop_depth <= 0) return;
    LoopCtx *lc = &c->loop_stack[--c->loop_depth];
    int exit_ip = c->current->proto->chunk.len;
    for (int i = 0; i < lc->n_break_patches; i++) {
        int idx = lc->break_patches[i];
        int offset = exit_ip - idx - 1;
        Instruction *ip2 = &c->current->proto->chunk.code[idx];
        *ip2 = (*ip2 & 0x0000FFFFU) | ((Instruction)(uint16_t)(int16_t)offset << 16);
    }
}

static void loop_add_break(Compiler *c, int patch_idx) {
    if (c->loop_depth <= 0) return;
    LoopCtx *lc = &c->loop_stack[c->loop_depth - 1];
    if (lc->n_break_patches < MAX_BREAK_PATCHES)
        lc->break_patches[lc->n_break_patches++] = patch_idx;
}

/* name resolution */

static void compile_name_load(Compiler *c, const char *name) {
    int slot = local_resolve(c->current, name);
    if (slot >= 0) { emit_a(c, OP_LOAD_LOCAL,   slot); return; }
    int uv = upvalue_resolve(c->current, name);
    if (uv   >= 0) { emit_a(c, OP_LOAD_UPVALUE, uv);   return; }
    /* `super` inside a method body resolves to self.super (the parent
       proxy that OP_INHERIT installed on the instance). Without this,
       the lookup falls through to OP_LOAD_GLOBAL super -> null. */
    if (strcmp(name, "super") == 0) {
        int self_slot = local_resolve(c->current, "self");
        if (self_slot >= 0) {
            emit_a(c, OP_LOAD_LOCAL, self_slot);
            int ni = emit_global_name(c, "super");
            emit_a(c, OP_LOAD_FIELD, ni);
            return;
        }
    }
    emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, name));
}

static void compile_name_store(Compiler *c, const char *name) {
    int slot = local_resolve(c->current, name);
    if (slot >= 0) { emit_a(c, OP_STORE_LOCAL,   slot); return; }
    int uv = upvalue_resolve(c->current, name);
    if (uv   >= 0) { emit_a(c, OP_STORE_UPVALUE, uv);   return; }
    emit_a(c, OP_STORE_GLOBAL, emit_global_name(c, name));
}

static void compile_node(Compiler *c, Node *n, int want_value);

/* Bind a let / var sub-pattern against the value on top of stack.
   Consumes the value. Recurses into nested tuple / slice / struct
   patterns so `let ((x, y), z) = ((10, 20), 30)` actually fills
   x, y, z and not just z. */
static void compile_let_pat(Compiler *c, Node *pat) {
    if (!pat || VAL_TAG(pat) == NODE_PAT_WILD) {
        emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }
    if (VAL_TAG(pat) == NODE_PAT_IDENT && pat->pat_ident.name) {
        int ds = local_add(c->current, pat->pat_ident.name);
        emit_a(c, OP_STORE_LOCAL, ds);
        return;
    }
    if (VAL_TAG(pat) == NODE_PAT_TUPLE || VAL_TAG(pat) == NODE_PAT_SLICE) {
        NodeList *elems = (VAL_TAG(pat) == NODE_PAT_TUPLE)
            ? &pat->pat_tuple.elems : &pat->pat_slice.elems;
        int slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, slot);
        for (int i = 0; i < elems->len; i++) {
            emit_a(c, OP_LOAD_LOCAL, slot);
            emit_const(c, xs_int(i));
            emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
            compile_let_pat(c, elems->items[i]);
        }
        if (VAL_TAG(pat) == NODE_PAT_SLICE && pat->pat_slice.rest) {
            emit_a(c, OP_LOAD_LOCAL, slot);
            emit_const(c, xs_int(elems->len));
            int rest_ni = emit_global_name(c, "slice");
            emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)rest_ni));
            int rs = local_add(c->current, pat->pat_slice.rest);
            emit_a(c, OP_STORE_LOCAL, rs);
        }
        return;
    }
    if (VAL_TAG(pat) == NODE_PAT_STRUCT) {
        int slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, slot);
        for (int i = 0; i < pat->pat_struct.fields.len; i++) {
            const char *key = pat->pat_struct.fields.items[i].key;
            Node *fpat = pat->pat_struct.fields.items[i].val;
            emit_a(c, OP_LOAD_LOCAL, slot);
            int fi_idx = emit_global_name(c, key);
            emit_a(c, OP_LOAD_FIELD, fi_idx);
            if (fpat && (VAL_TAG(fpat) == NODE_PAT_TUPLE ||
                         VAL_TAG(fpat) == NODE_PAT_SLICE ||
                         VAL_TAG(fpat) == NODE_PAT_STRUCT)) {
                compile_let_pat(c, fpat);
            } else {
                const char *bind = key;
                if (fpat && VAL_TAG(fpat) == NODE_PAT_IDENT && fpat->pat_ident.name)
                    bind = fpat->pat_ident.name;
                int ds = local_add(c->current, bind);
                emit_a(c, OP_STORE_LOCAL, ds);
            }
        }
        return;
    }
    emit(c, MAKE_A(OP_POP, 0, 0));
}

static void compile_tuple_pattern_at(Compiler *c, Node *pat, int val_slot,
                                     int *fail_jumps, int *n_fail, int max_fails);
static void compile_map_pattern_at(Compiler *c, Node *pat, int val_slot,
                                    int *fail_jumps, int *n_fail, int max_fails);
static void compile_struct_pattern_at(Compiler *c, Node *pat, int val_slot,
                                      int *fail_jumps, int *n_fail, int max_fails);

/* Dispatch a sub-pattern when the value is already on top of stack. */
static void compile_sub_pattern_tos(Compiler *c, Node *sub,
                                     int *fail_jumps, int *n_fail, int max_fails) {
    if (!sub) {
        emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }
    if (VAL_TAG(sub) == NODE_PAT_IDENT) {
        int slot = local_add(c->current, sub->pat_ident.name);
        emit_a(c, OP_STORE_LOCAL, slot);
    } else if (VAL_TAG(sub) == NODE_PAT_WILD) {
        emit(c, MAKE_A(OP_POP, 0, 0));
    } else if (VAL_TAG(sub) == NODE_PAT_LIT) {
        switch (sub->pat_lit.tag) {
        case 0: emit_const(c, xs_int(sub->pat_lit.ival)); break;
        case 1: emit_const(c, xs_float(sub->pat_lit.fval)); break;
        case 2: emit_const(c, xs_str(sub->pat_lit.sval)); break;
        case 3: emit(c, MAKE_A(sub->pat_lit.bval ? OP_PUSH_TRUE : OP_PUSH_FALSE, 0, 0)); break;
        default: emit(c, MAKE_A(OP_PUSH_NULL, 0, 0)); break;
        }
        emit(c, MAKE_A(OP_EQ, 0, 0));
        if (*n_fail < max_fails)
            fail_jumps[(*n_fail)++] = emit_jump(c, OP_JUMP_IF_FALSE);
    } else if (VAL_TAG(sub) == NODE_PAT_TUPLE) {
        int slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, slot);
        compile_tuple_pattern_at(c, sub, slot, fail_jumps, n_fail, max_fails);
    } else if (VAL_TAG(sub) == NODE_PAT_MAP) {
        int slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, slot);
        compile_map_pattern_at(c, sub, slot, fail_jumps, n_fail, max_fails);
    } else if (VAL_TAG(sub) == NODE_PAT_STRUCT) {
        int slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, slot);
        compile_struct_pattern_at(c, sub, slot, fail_jumps, n_fail, max_fails);
    } else {
        emit(c, MAKE_A(OP_POP, 0, 0));
    }
}

/* Compile a struct pattern against the value stored in val_slot.
   Appends any jumps that must be patched to the fail target to
   fail_jumps. Recursive so nested struct / tuple / map patterns
   inside fields actually compare and bind. */
static void compile_struct_pattern_at(Compiler *c, Node *pat, int val_slot,
                                      int *fail_jumps, int *n_fail, int max_fails) {
    for (int fi = 0; fi < pat->pat_struct.fields.len; fi++) {
        const char *fname = pat->pat_struct.fields.items[fi].key;
        Node *fpat = pat->pat_struct.fields.items[fi].val;
        emit_a(c, OP_LOAD_LOCAL, val_slot);
        int fi_idx = emit_global_name(c, fname);
        emit_a(c, OP_LOAD_FIELD, fi_idx);
        if (!fpat) {
            int slot = local_add(c->current, fname);
            emit_a(c, OP_STORE_LOCAL, slot);
        } else {
            compile_sub_pattern_tos(c, fpat, fail_jumps, n_fail, max_fails);
        }
    }
}

static void compile_tuple_pattern_at(Compiler *c, Node *pat, int val_slot,
                                     int *fail_jumps, int *n_fail, int max_fails) {
    /* Guard: subject must actually be a tuple. Without this, tuple
       patterns silently fired on arrays of the same length. */
    emit_a(c, OP_LOAD_LOCAL, val_slot);
    emit_const(c, xs_str("<tuple-like>"));
    emit(c, MAKE_A(OP_IS, 0, 0));
    if (*n_fail < max_fails)
        fail_jumps[(*n_fail)++] = emit_jump(c, OP_JUMP_IF_FALSE);
    /* Arity guard: a tuple pattern of length N only matches a tuple of
       length N. Without this, (_, _) silently matched (1,). */
    emit_a(c, OP_LOAD_LOCAL, val_slot);
    emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
    emit_const(c, xs_int(pat->pat_tuple.elems.len));
    emit(c, MAKE_A(OP_EQ, 0, 0));
    if (*n_fail < max_fails)
        fail_jumps[(*n_fail)++] = emit_jump(c, OP_JUMP_IF_FALSE);
    for (int ti = 0; ti < pat->pat_tuple.elems.len; ti++) {
        Node *sub = pat->pat_tuple.elems.items[ti];
        emit_a(c, OP_LOAD_LOCAL, val_slot);
        emit_const(c, xs_int(ti));
        emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
        compile_sub_pattern_tos(c, sub, fail_jumps, n_fail, max_fails);
    }
}

/* Compile a map pattern against the value stored in val_slot.
   Appends any jumps that must be patched to the fail target to
   fail_jumps. Recursive so nested #{...} patterns match correctly. */
static void compile_map_pattern_at(Compiler *c, Node *pat, int val_slot,
                                    int *fail_jumps, int *n_fail, int max_fails) {
    for (int fi = 0; fi < pat->pat_map.nfields; fi++) {
        const char *fname = pat->pat_map.keys[fi];
        Node *fpat = pat->pat_map.sub[fi];

        emit_a(c, OP_LOAD_LOCAL, val_slot);
        int fi_idx = emit_global_name(c, fname);
        emit_a(c, OP_LOAD_FIELD, fi_idx);

        if (fpat && VAL_TAG(fpat) == NODE_PAT_WILD) {
            emit(c, MAKE_A(OP_POP, 0, 0));
            continue;
        }

        /* LOAD_FIELD returns null for missing keys; non-wildcard
           sub-patterns require a real value. */
        emit(c, MAKE_A(OP_DUP, 0, 0));
        emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        emit(c, MAKE_A(OP_NEQ, 0, 0));
        if (*n_fail < max_fails)
            fail_jumps[(*n_fail)++] = emit_jump(c, OP_JUMP_IF_FALSE);

        compile_sub_pattern_tos(c, fpat, fail_jumps, n_fail, max_fails);
    }
}

static int compile_fn(Compiler *c, const char *name,
                      ParamList *params, Node *body)
{
    int total_params = params ? params->len : 0;
    int has_variadic = 0;
    int non_variadic = total_params;
    if (params) {
        for (int i = 0; i < params->len; i++) {
            if (params->items[i].variadic) { has_variadic = 1; non_variadic = i; break; }
        }
    }
    int arity = has_variadic ? -(non_variadic + 1) : non_variadic;
    XSProto *parent = c->current->proto;
    XSProto *inner  = proto_new(name ? name : "<lambda>", arity);
    inner->is_variadic = has_variadic;

    if (parent->n_inner == parent->cap_inner) {
        parent->cap_inner = parent->cap_inner ? parent->cap_inner * 2 : 4;
        parent->inner = xs_realloc(parent->inner,
                            (size_t)parent->cap_inner * sizeof(XSProto *));
    }
    int inner_idx = parent->n_inner;
    parent->inner[parent->n_inner++] = inner;

    CompilerScope fn_scope;
    scope_push(c, &fn_scope, inner);

    if (params) {
        for (int i = 0; i < total_params; i++) {
            const char *pname = params->items[i].name;
            if (params->items[i].variadic) {
                local_add(c->current, pname ? pname : "args");
            } else {
                local_add(c->current, pname ? pname : "<param>");
            }
        }
        /* remember param names so OP_CALL_KW can match kwargs by name */
        if (total_params > 0) {
            inner->param_names = xs_malloc((size_t)total_params * sizeof(char *));
            for (int i = 0; i < total_params; i++) {
                const char *pname = params->items[i].name;
                inner->param_names[i] = xs_strdup(pname ? pname : "");
            }
            inner->n_params = total_params;
        }
    }

    /* emit default value fill-ins for optional params */
    if (params) {
        for (int i = 0; i < total_params; i++) {
            if (params->items[i].default_val && !params->items[i].variadic) {
                int slot = i;
                emit_a(c, OP_LOAD_LOCAL, slot);
                emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
                emit(c, MAKE_A(OP_EQ, 0, 0));
                int skip = emit_jump(c, OP_JUMP_IF_FALSE);
                compile_node(c, params->items[i].default_val, 1);
                emit_a(c, OP_STORE_LOCAL, slot);
                patch_jump(c, skip);
            }
        }
    }

    /* emit `where` contract checks for each parameter that has one.
     * the contract expression is just an XS expression that has the
     * parameter in scope; compile it inline, jump-if-true past the
     * throw, otherwise push the violation string and OP_THROW.
     * shape mirrors interp.c's loop. */
    if (params) {
        for (int i = 0; i < total_params; i++) {
            Node *contract = params->items[i].contract;
            if (!contract) continue;
            compile_node(c, contract, 1);
            int ok = emit_jump(c, OP_JUMP_IF_TRUE);
            char msg[256];
            snprintf(msg, sizeof msg,
                "contract violation: parameter %d of '%s' does not satisfy 'where' constraint",
                i + 1, name ? name : "<anonymous>");
            emit_const(c, xs_str(msg));
            emit(c, MAKE_A(OP_THROW, 0, 0));
            patch_jump(c, ok);
        }
    }

    compile_node(c, body, 1);

    XSChunk *ch = &inner->chunk;
    if (ch->len == 0 ||
        INSTR_OPCODE(ch->code[ch->len - 1]) != OP_RETURN)
        emit(c, MAKE_A(OP_RETURN, 0, 0));

    if (fn_scope.n_upvalues > 0) {
        inner->uv_descs = xs_malloc(
            (size_t)fn_scope.n_upvalues * sizeof(UVDesc));
        memcpy(inner->uv_descs, fn_scope.uv_descs,
               (size_t)fn_scope.n_upvalues * sizeof(UVDesc));
        inner->n_upvalues = fn_scope.n_upvalues;
    }

    scope_pop(c);
    return inner_idx;
}

static void emit_make_closure(Compiler *c, int inner_idx) {
    Value *v = xs_int((int64_t)inner_idx);
    int idx  = chunk_add_const(&c->current->proto->chunk, v);
    value_decref(v);
    emit_a(c, OP_MAKE_CLOSURE, idx);
}

static void compile_node(Compiler *c, Node *n, int want_value) {
    if (!n) {
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    switch (VAL_TAG(n)) {

    case NODE_LIT_INT:
        emit_const(c, xs_int(n->lit_int.ival));
        break;

    case NODE_LIT_BIGINT: {
        /* base=0 auto-detects 0x/0b/0o prefixes (lexer keeps them on
           the literal string for hex/bin/oct overflows). */
        XSBigInt *b = bigint_from_str(n->lit_bigint.bigint_str, 0);
        emit_const(c, xs_bigint_val(b));
        break;
    }

    case NODE_LIT_FLOAT:
        emit_const(c, xs_float(n->lit_float.fval));
        break;

    case NODE_LIT_STRING:
        emit_const(c, xs_str(n->lit_string.sval));
        break;

    case NODE_LIT_BOOL:
        emit(c, MAKE_A(n->lit_bool.bval ? OP_PUSH_TRUE : OP_PUSH_FALSE, 0, 0));
        break;

    case NODE_LIT_NULL:
        emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        break;

    case NODE_LIT_CHAR: {
        /* 'a' is a char, not a one-byte string. Match the interpreter's
           xs_char() so type('a') returns "char" on both backends. */
        emit_const(c, xs_char(n->lit_char.cval));
        break;
    }

    case NODE_LIT_ARRAY: {
        int cnt = n->lit_array.elems.len;
        int has_spread = 0;
        for (int i = 0; i < cnt; i++)
            if (VAL_TAG(n->lit_array.elems.items[i]) == NODE_SPREAD) { has_spread = 1; break; }
        if (has_spread) {
            emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0, 0));
            int arr_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, arr_slot);
            for (int i = 0; i < cnt; i++) {
                Node *elem = n->lit_array.elems.items[i];
                if (VAL_TAG(elem) == NODE_SPREAD) {
                    int sp_slot = local_add_hidden(c);
                    int sp_len  = local_add_hidden(c);
                    int sp_idx  = local_add_hidden(c);
                    compile_node(c, elem->spread.expr, 1);
                    emit_a(c, OP_STORE_LOCAL, sp_slot);
                    emit_a(c, OP_LOAD_LOCAL, sp_slot);
                    emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
                    emit_a(c, OP_STORE_LOCAL, sp_len);
                    emit_const(c, xs_int(0));
                    emit_a(c, OP_STORE_LOCAL, sp_idx);
                    int loop_top = c->current->proto->chunk.len;
                    emit_a(c, OP_LOAD_LOCAL, sp_idx);
                    emit_a(c, OP_LOAD_LOCAL, sp_len);
                    emit(c, MAKE_A(OP_LT, 0, 0));
                    int j_exit = emit_jump(c, OP_JUMP_IF_FALSE);
                    emit_a(c, OP_LOAD_LOCAL, arr_slot);
                    emit_a(c, OP_LOAD_LOCAL, sp_slot);
                    emit_a(c, OP_LOAD_LOCAL, sp_idx);
                    emit(c, MAKE_A(OP_ITER_GET, 0, 0));
                    {
                        int pi = emit_global_name(c, "push");
                        emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)pi));
                    }
                    emit(c, MAKE_A(OP_POP, 0, 0));
                    emit_a(c, OP_LOAD_LOCAL, sp_idx);
                    emit_const(c, xs_int(1));
                    emit(c, MAKE_A(OP_ADD, 0, 0));
                    emit_a(c, OP_STORE_LOCAL, sp_idx);
                    int back_off = loop_top - (c->current->proto->chunk.len + 1);
                    emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)back_off));
                    patch_jump(c, j_exit);
                } else {
                    emit_a(c, OP_LOAD_LOCAL, arr_slot);
                    compile_node(c, elem, 1);
                    {
                        int pi = emit_global_name(c, "push");
                        emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)pi));
                    }
                    emit(c, MAKE_A(OP_POP, 0, 0));
                }
            }
            emit_a(c, OP_LOAD_LOCAL, arr_slot);
        } else {
            for (int i = 0; i < cnt; i++)
                compile_node(c, n->lit_array.elems.items[i], 1);
            emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0, (uint8_t)(unsigned)cnt));
        }
        break;
    }

    case NODE_LIT_TUPLE: {
        int cnt = n->lit_array.elems.len;
        for (int i = 0; i < cnt; i++)
            compile_node(c, n->lit_array.elems.items[i], 1);
        emit(c, MAKE_B(OP_MAKE_TUPLE, 0, 0, (uint8_t)(unsigned)cnt));
        break;
    }

    case NODE_IDENT:
        compile_name_load(c, n->ident.name);
        break;

    case NODE_BINOP: {
        const char *op = n->binop.op;
        if (strcmp(op, "&&") == 0 || strcmp(op, "and") == 0) {
            compile_node(c, n->binop.left, 1);
            emit(c, MAKE_A(OP_DUP, 0, 0));
            int jf = emit_jump(c, OP_JUMP_IF_FALSE);
            emit(c, MAKE_A(OP_POP, 0, 0));
            compile_node(c, n->binop.right, 1);
            patch_jump(c, jf);
            break;
        }
        if (strcmp(op, "||") == 0 || strcmp(op, "or") == 0) {
            compile_node(c, n->binop.left, 1);
            emit(c, MAKE_A(OP_DUP, 0, 0));
            int jt = emit_jump(c, OP_JUMP_IF_TRUE);
            emit(c, MAKE_A(OP_POP, 0, 0));
            compile_node(c, n->binop.right, 1);
            patch_jump(c, jt);
            break;
        }
        if (strcmp(op, "??") == 0) {
            compile_node(c, n->binop.left, 1);
            emit(c, MAKE_A(OP_DUP, 0, 0));
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            emit(c, MAKE_A(OP_NEQ, 0, 0));
            int jt = emit_jump(c, OP_JUMP_IF_TRUE);
            emit(c, MAKE_A(OP_POP, 0, 0));
            compile_node(c, n->binop.right, 1);
            patch_jump(c, jt);
            break;
        }
        if (strcmp(op, "|>") == 0) {
            compile_node(c, n->binop.right, 1);
            compile_node(c, n->binop.left, 1);
            emit(c, MAKE_B(OP_CALL, 0, 0, 1));
            break;
        }
        /* Fold a few of the obvious LIT op LIT cases: numeric arithmetic
           on int / float literals where the result is exact. Catches
           the `2 ** 31` shape from random / hash code that's otherwise
           recomputed every loop iteration. Skip when overflow could
           happen (signed int **, *, +, -); the slow path handles those
           with bigint promotion. */
        if (n->binop.left  && n->binop.left->tag  == NODE_LIT_INT &&
            n->binop.right && n->binop.right->tag == NODE_LIT_INT) {
            int64_t a = n->binop.left->lit_int.ival;
            int64_t b = n->binop.right->lit_int.ival;
            int64_t r = 0;
            int folded = 0;
            if (strcmp(op, "**") == 0 && b >= 0 && b < 63) {
                r = 1; folded = 1;
                for (int64_t e = 0; e < b && folded; e++) {
                    if (__builtin_mul_overflow(r, a, &r)) folded = 0;
                }
            } else if (strcmp(op, "+") == 0) {
                folded = !__builtin_add_overflow(a, b, &r);
            } else if (strcmp(op, "-") == 0) {
                folded = !__builtin_sub_overflow(a, b, &r);
            } else if (strcmp(op, "*") == 0) {
                folded = !__builtin_mul_overflow(a, b, &r);
            }
            if (folded) {
                emit_const(c, xs_int(r));
                break;
            }
        }
        if (n->binop.left  && n->binop.left->tag  == NODE_LIT_FLOAT &&
            n->binop.right && n->binop.right->tag == NODE_LIT_FLOAT) {
            double a = n->binop.left->lit_float.fval;
            double b = n->binop.right->lit_float.fval;
            double r = 0.0;
            int folded = 1;
            if      (strcmp(op, "+") == 0) r = a + b;
            else if (strcmp(op, "-") == 0) r = a - b;
            else if (strcmp(op, "*") == 0) r = a * b;
            else folded = 0;
            if (folded) {
                emit_const(c, xs_float(r));
                break;
            }
        }

        compile_node(c, n->binop.left,  1);
        compile_node(c, n->binop.right, 1);
        Opcode bop = OP_NOP;
        if      (strcmp(op, "+")  == 0) bop = OP_ADD;
        else if (strcmp(op, "-")  == 0) bop = OP_SUB;
        else if (strcmp(op, "*")  == 0) bop = OP_MUL;
        else if (strcmp(op, "/")  == 0) bop = OP_DIV;
        else if (strcmp(op, "%")  == 0) bop = OP_MOD;
        else if (strcmp(op, "**") == 0) bop = OP_POW;
        else if (strcmp(op, "++") == 0) bop = OP_CONCAT;
        else if (strcmp(op, "==") == 0) bop = OP_EQ;
        else if (strcmp(op, "!=") == 0) bop = OP_NEQ;
        else if (strcmp(op, "<")  == 0) bop = OP_LT;
        else if (strcmp(op, ">")  == 0) bop = OP_GT;
        else if (strcmp(op, "<=") == 0) bop = OP_LTE;
        else if (strcmp(op, ">=") == 0) bop = OP_GTE;
        else if (strcmp(op, "&")  == 0) bop = OP_BAND;
        else if (strcmp(op, "|")  == 0) bop = OP_BOR;
        else if (strcmp(op, "^")  == 0) bop = OP_BXOR;
        else if (strcmp(op, "<<") == 0) bop = OP_SHL;
        else if (strcmp(op, ">>") == 0) bop = OP_SHR;
        else if (strcmp(op, "//") == 0) bop = OP_FLOOR_DIV;
        else if (strcmp(op, "<=>") == 0) bop = OP_SPACESHIP;
        else if (strcmp(op, "in") == 0) bop = OP_IN;
        else if (strcmp(op, "is") == 0) bop = OP_IS;
        else if (strcmp(op, "not in") == 0) { emit(c, MAKE_A(OP_IN, 0, 0)); emit(c, MAKE_A(OP_NOT, 0, 0)); break; }
        else {
            fprintf(stderr, "unknown binop '%s'\n", op);
            emit(c, MAKE_A(OP_POP, 0, 0));
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            return;
        }
        emit(c, MAKE_A(bop, 0, 0));
        break;
    }

    case NODE_UNARY:
        /* Fold `-LIT` into a single PUSH_CONST so the JIT (and VM) don't
           dispatch a runtime NEG for source like `-2.0` or `-1`. INT64_MIN
           is left to the runtime to handle (it has overflow logic). */
        if (strcmp(n->unary.op, "-") == 0 &&
            n->unary.expr && n->unary.expr->tag == NODE_LIT_INT &&
            n->unary.expr->lit_int.ival != INT64_MIN) {
            emit_const(c, xs_int(-n->unary.expr->lit_int.ival));
            break;
        }
        if (strcmp(n->unary.op, "-") == 0 &&
            n->unary.expr && n->unary.expr->tag == NODE_LIT_FLOAT) {
            emit_const(c, xs_float(-n->unary.expr->lit_float.fval));
            break;
        }
        compile_node(c, n->unary.expr, 1);
        if (strcmp(n->unary.op, "-") == 0)
            emit(c, MAKE_A(OP_NEG, 0, 0));
        else if (strcmp(n->unary.op, "~") == 0)
            emit(c, MAKE_A(OP_BNOT, 0, 0));
        else
            emit(c, MAKE_A(OP_NOT, 0, 0));
        break;

    case NODE_ASSIGN: {
        Node *tgt = n->assign.target;
        Opcode compound_op = OP_NOP;
        const char *aop = n->assign.op;
        if (aop[0] != '=' || aop[1] != '\0') {
            if      (strcmp(aop, "+=")  == 0) compound_op = OP_ADD;
            else if (strcmp(aop, "-=")  == 0) compound_op = OP_SUB;
            else if (strcmp(aop, "*=")  == 0) compound_op = OP_MUL;
            else if (strcmp(aop, "/=")  == 0) compound_op = OP_DIV;
            else if (strcmp(aop, "%=")  == 0) compound_op = OP_MOD;
            else if (strcmp(aop, "**=") == 0) compound_op = OP_POW;
            else if (strcmp(aop, "&=")  == 0) compound_op = OP_BAND;
            else if (strcmp(aop, "|=")  == 0) compound_op = OP_BOR;
            else if (strcmp(aop, "^=")  == 0) compound_op = OP_BXOR;
            else if (strcmp(aop, "<<=") == 0) compound_op = OP_SHL;
            else if (strcmp(aop, ">>=") == 0) compound_op = OP_SHR;
            else if (strcmp(aop, "++=") == 0) compound_op = OP_CONCAT;
            else if (strcmp(aop, "//=") == 0) compound_op = OP_FLOOR_DIV;
        }

        if (VAL_TAG(tgt) == NODE_IDENT) {
            if (compound_op != OP_NOP) {
                compile_name_load(c, tgt->ident.name);
                compile_node(c, n->assign.value, 1);
                emit(c, MAKE_A(compound_op, 0, 0));
            } else {
                compile_node(c, n->assign.value, 1);
            }
            if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
            compile_name_store(c, tgt->ident.name);
        } else if (VAL_TAG(tgt) == NODE_FIELD) {
            compile_node(c, tgt->field.obj, 1);
            if (compound_op != OP_NOP) {
                emit(c, MAKE_A(OP_DUP, 0, 0));
                int fi = emit_global_name(c, tgt->field.name);
                emit_a(c, OP_LOAD_FIELD, fi);
                compile_node(c, n->assign.value, 1);
                emit(c, MAKE_A(compound_op, 0, 0));
            } else {
                compile_node(c, n->assign.value, 1);
            }
            if (want_value) {
                /* stack: obj val → store val to field, return val */
                int tmp = local_add_hidden(c);
                emit(c, MAKE_A(OP_DUP, 0, 0));     /* obj val val */
                emit_a(c, OP_STORE_LOCAL, tmp);      /* obj val */
                int ni = emit_global_name(c, tgt->field.name);
                emit_a(c, OP_STORE_FIELD, ni);       /* (empty) */
                emit_a(c, OP_LOAD_LOCAL, tmp);       /* val */
            } else {
                int ni = emit_global_name(c, tgt->field.name);
                emit_a(c, OP_STORE_FIELD, ni);
            }
        } else if (VAL_TAG(tgt) == NODE_INDEX) {
            compile_node(c, tgt->index.obj,   1);
            compile_node(c, tgt->index.index, 1);
            if (compound_op != OP_NOP) {
                    emit(c, MAKE_A(OP_DUP, 0, 0));
                int idx_tmp = local_add_hidden(c);
                int col_tmp = local_add_hidden(c);
                emit(c, MAKE_A(OP_POP, 0, 0));
                emit_a(c, OP_STORE_LOCAL, idx_tmp);
                emit_a(c, OP_STORE_LOCAL, col_tmp);
                emit_a(c, OP_LOAD_LOCAL, col_tmp);
                emit_a(c, OP_LOAD_LOCAL, idx_tmp);
                emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                compile_node(c, n->assign.value, 1);
                emit(c, MAKE_A(compound_op, 0, 0));
                int new_tmp = local_add_hidden(c);
                emit_a(c, OP_STORE_LOCAL, new_tmp);
                emit_a(c, OP_LOAD_LOCAL, col_tmp);
                emit_a(c, OP_LOAD_LOCAL, idx_tmp);
                emit_a(c, OP_LOAD_LOCAL, new_tmp);
                emit(c, MAKE_A(OP_INDEX_SET, 0, 0));
                if (want_value) emit_a(c, OP_LOAD_LOCAL, new_tmp);
            } else {
                compile_node(c, n->assign.value,  1);
                if (want_value) {
                    int tmp = local_add_hidden(c);
                    emit_a(c, OP_STORE_LOCAL, tmp);
                    emit(c, MAKE_A(OP_INDEX_SET, 0, 0));
                    emit_a(c, OP_LOAD_LOCAL, tmp);
                } else {
                    emit(c, MAKE_A(OP_INDEX_SET, 0, 0));
                }
            }
        } else if (VAL_TAG(tgt) == NODE_LIT_TUPLE) {
            /* Parallel tuple assignment `(a, b) = (b, a)`. Build the RHS
               into a temp and then assign element-by-element so the LHS
               reads of each variable see the original values. */
            compile_node(c, n->assign.value, 1);
            int tup_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, tup_slot);
            NodeList *elems = &tgt->lit_array.elems;
            for (int ei = 0; ei < elems->len; ei++) {
                Node *sub = elems->items[ei];
                emit_a(c, OP_LOAD_LOCAL, tup_slot);
                emit_const(c, xs_int(ei));
                emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                if (VAL_TAG(sub) == NODE_IDENT) {
                    compile_name_store(c, sub->ident.name);
                } else if (VAL_TAG(sub) == NODE_FIELD) {
                    compile_node(c, sub->field.obj, 1);
                    emit(c, MAKE_A(OP_SWAP, 0, 0));
                    int ni = emit_global_name(c, sub->field.name);
                    emit_a(c, OP_STORE_FIELD, ni);
                } else {
                    /* unsupported sub-target: drop the value */
                    emit(c, MAKE_A(OP_POP, 0, 0));
                }
            }
            if (want_value) emit_a(c, OP_LOAD_LOCAL, tup_slot);
        } else {
            compile_node(c, n->assign.value, 0);
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        return;
    }

    case NODE_LET:
    case NODE_VAR: {
        if (n->let.value)
            compile_node(c, n->let.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        Node *pat = n->let.pattern;
        if (pat && (VAL_TAG(pat) == NODE_PAT_TUPLE || VAL_TAG(pat) == NODE_PAT_SLICE
                    || VAL_TAG(pat) == NODE_PAT_STRUCT)) {
            int val_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, val_slot);
            emit_a(c, OP_LOAD_LOCAL, val_slot);
            compile_let_pat(c, pat);
            if (want_value) emit_a(c, OP_LOAD_LOCAL, val_slot);
        } else {
            if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
            int slot = local_add(c->current, n->let.name ? n->let.name : "<anon>");
            emit_a(c, OP_STORE_LOCAL, slot);
            /* `let x where pred = expr` -- check pred with x in scope */
            if (n->let.contract) {
                compile_node(c, n->let.contract, 1);
                int ok = emit_jump(c, OP_JUMP_IF_TRUE);
                char msg[256];
                snprintf(msg, sizeof msg,
                    "contract violation: value does not satisfy 'where' constraint for '%s'",
                    n->let.name ? n->let.name : "<pattern>");
                emit_const(c, xs_str(msg));
                emit(c, MAKE_A(OP_THROW, 0, 0));
                patch_jump(c, ok);
            }
        }
        return;
    }

    case NODE_CONST: {
        if (n->const_.value)
            compile_node(c, n->const_.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        int slot = local_add(c->current, n->const_.name);
        emit_a(c, OP_STORE_LOCAL, slot);
        return;
    }

    case NODE_BLOCK: {
        scope_begin(c);
        /* Hoist named fn decls in nested scopes so the body can refer
           to its own name (or sibling fns for mutual recursion) via a
           local slot instead of falling through to STORE_GLOBAL, which
           would have two calls to a factory clobber the same binding. */
        if (c->current->enclosing) {
            for (int i = 0; i < n->block.stmts.len; i++) {
                Node *s = n->block.stmts.items[i];
                if (!s || VAL_TAG(s) != NODE_FN_DECL) continue;
                const char *fname = s->fn_decl.name;
                if (!fname || !fname[0] || fname[0] == '<') continue;
                if (local_resolve(c->current, fname) >= 0) continue;
                local_add(c->current, fname);
            }
        }
        for (int i = 0; i < n->block.stmts.len; i++)
            compile_node(c, n->block.stmts.items[i], 0);
        if (n->block.expr)
            compile_node(c, n->block.expr, want_value);
        else if (want_value)
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        scope_end(c);
        return;
    }

    case NODE_IF: {
        int n_elif = n->if_expr.elif_conds.len;
        int end_jumps[256];
        int n_end_jumps = 0;

        compile_node(c, n->if_expr.cond, 1);
        int jf = emit_jump(c, OP_JUMP_IF_FALSE);
        compile_node(c, n->if_expr.then, want_value);
        end_jumps[n_end_jumps++] = emit_jump(c, OP_JUMP);
        patch_jump(c, jf);

        for (int i = 0; i < n_elif; i++) {
            compile_node(c, n->if_expr.elif_conds.items[i], 1);
            int jf2 = emit_jump(c, OP_JUMP_IF_FALSE);
            compile_node(c, n->if_expr.elif_thens.items[i], want_value);
            end_jumps[n_end_jumps++] = emit_jump(c, OP_JUMP);
            patch_jump(c, jf2);
        }

        if (n->if_expr.else_branch)
            compile_node(c, n->if_expr.else_branch, want_value);
        else if (want_value)
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));

        for (int i = 0; i < n_end_jumps; i++)
            patch_jump(c, end_jumps[i]);
        return;
    }

    case NODE_WHILE: {
        int loop_start = c->current->proto->chunk.len;
        loop_push_label(c, loop_start, n->while_loop.label);
        compile_node(c, n->while_loop.cond, 1);
        int j_exit = emit_jump(c, OP_JUMP_IF_FALSE);
        compile_node(c, n->while_loop.body, 0);
        int back_off = loop_start - (c->current->proto->chunk.len + 1);
        emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)back_off));
        patch_jump(c, j_exit);
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        loop_pop_patch_breaks(c);
        return;
    }

    case NODE_RETURN: {
        if (n->ret.value)
            compile_node(c, n->ret.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        /* actor method: write back state fields before returning */
        if (c->current->actor_state_names && c->current->actor_nstate > 0) {
            for (int si = 0; si < c->current->actor_nstate; si++) {
                int slot = local_resolve(c->current, c->current->actor_state_names[si]);
                if (slot >= 0) {
                    emit_a(c, OP_LOAD_LOCAL, 0); /* self */
                    emit_a(c, OP_LOAD_LOCAL, slot);
                    int fi = emit_global_name(c, c->current->actor_state_names[si]);
                    emit_a(c, OP_STORE_FIELD, fi);
                }
            }
        }
        emit(c, MAKE_A(OP_RETURN, 0, 0));
        return;
    }

    case NODE_EXPR_STMT:
        compile_node(c, n->expr_stmt.expr, 0);
        return;

    case NODE_CALL: {
        compile_node(c, n->call.callee, 1);
        int argc  = n->call.args.len;
        int nkw   = n->call.kwargs.len;
        for (int i = 0; i < argc; i++)
            compile_node(c, n->call.args.items[i], 1);
        if (nkw > 0) {
            for (int i = 0; i < nkw; i++) {
                const char *kname = n->call.kwargs.items[i].key;
                emit_const(c, xs_str(kname ? kname : ""));
                compile_node(c, n->call.kwargs.items[i].val, 1);
            }
            emit(c, MAKE_B(OP_CALL_KW, (uint8_t)(unsigned)argc, 0, (uint8_t)(unsigned)nkw));
        } else {
            emit(c, MAKE_B(OP_CALL, 0, 0, (uint8_t)(unsigned)argc));
        }
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_FN_DECL: {
        /* Nested fn decls bind to a local slot; the block-level hoist
           above already reserved it. At top level (enclosing == NULL)
           keep the historical global binding so other modules can
           import and call the function. */
        const char *fname = n->fn_decl.name;
        int nested = c->current->enclosing != NULL;
        int local_slot = -1;
        if (nested && fname && fname[0] && fname[0] != '<')
            local_slot = local_resolve(c->current, fname);
        int idx = compile_fn(c, fname,
                             &n->fn_decl.params,
                             n->fn_decl.body);
        if (n->fn_decl.is_generator)
            c->current->proto->inner[idx]->is_generator = 1;
        emit_make_closure(c, idx);
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        if (local_slot >= 0) emit_a(c, OP_STORE_LOCAL, local_slot);
        else                 compile_name_store(c, fname);
        return;
    }

    case NODE_LAMBDA: {
        int idx = compile_fn(c, NULL,
                             &n->lambda.params,
                             n->lambda.body);
        if (n->lambda.is_generator)
            c->current->proto->inner[idx]->is_generator = 1;
        emit_make_closure(c, idx);
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_PROGRAM:
        for (int i = 0; i < n->program.stmts.len; i++)
            compile_node(c, n->program.stmts.items[i], 0);
        return;

    case NODE_LIT_MAP: {
        int cnt = n->lit_map.keys.len;
        int has_spread = 0;
        for (int i = 0; i < cnt; i++)
            if (n->lit_map.keys.items[i] && VAL_TAG(n->lit_map.keys.items[i]) == NODE_SPREAD) { has_spread = 1; break; }
        /* In `#{ name: v }`, a bareword key parses as NODE_IDENT. The
           interpreter treats it as the string "name", so we must too -
           otherwise the VM emits LOAD_GLOBAL "name" which usually
           returns null and silently produces an empty map. */
        #define EMIT_KEY(KEYNODE) do { \
            Node *_k = (KEYNODE); \
            if (_k && VAL_TAG(_k) == NODE_IDENT && _k->ident.name) \
                emit_const(c, xs_str(_k->ident.name)); \
            else \
                compile_node(c, _k, 1); \
        } while (0)
        if (has_spread) {
            emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 0));
            int map_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, map_slot);
            for (int i = 0; i < cnt; i++) {
                Node *key = n->lit_map.keys.items[i];
                if (key && VAL_TAG(key) == NODE_SPREAD) {
                    emit_a(c, OP_LOAD_LOCAL, map_slot);
                    compile_node(c, key->spread.expr, 1);
                    emit(c, MAKE_A(OP_MAP_MERGE, 0, 0));
                    emit(c, MAKE_A(OP_POP, 0, 0));
                } else {
                    emit_a(c, OP_LOAD_LOCAL, map_slot);
                    EMIT_KEY(key);
                    compile_node(c, n->lit_map.vals.items[i], 1);
                    emit(c, MAKE_A(OP_INDEX_SET, 0, 0));
                }
            }
            emit_a(c, OP_LOAD_LOCAL, map_slot);
        } else {
            for (int i = 0; i < cnt; i++) {
                EMIT_KEY(n->lit_map.keys.items[i]);
                compile_node(c, n->lit_map.vals.items[i], 1);
            }
            emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)cnt));
        }
        #undef EMIT_KEY
        break;
    }

    case NODE_INDEX:
        compile_node(c, n->index.obj,   1);
        compile_node(c, n->index.index, 1);
        emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
        break;

    case NODE_FIELD: {
        compile_node(c, n->field.obj, 1);
        if (n->field.optional) {
            emit(c, MAKE_A(OP_DUP, 0, 0));
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            emit(c, MAKE_A(OP_EQ, 0, 0));
            int skip = emit_jump(c, OP_JUMP_IF_TRUE);
            int ni = emit_global_name(c, n->field.name);
            emit_a(c, OP_LOAD_FIELD, ni);
            int end = emit_jump(c, OP_JUMP);
            patch_jump(c, skip);
            emit(c, MAKE_A(OP_POP, 0, 0));
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            patch_jump(c, end);
        } else {
            int ni = emit_global_name(c, n->field.name);
            emit_a(c, OP_LOAD_FIELD, ni);
        }
        break;
    }

    case NODE_SCOPE: {
        if (n->scope.nparts == 0) {
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            break;
        }
        compile_name_load(c, n->scope.parts[0]);
        for (int i = 1; i < n->scope.nparts; i++) {
            int ni = emit_global_name(c, n->scope.parts[i]);
            emit_a(c, OP_LOAD_FIELD, ni);
        }
        break;
    }

    case NODE_RANGE:
        if (n->range.start) compile_node(c, n->range.start, 1);
        else emit_const(c, xs_int(0));
        if (n->range.end) compile_node(c, n->range.end, 1);
        else {
            /* `..end`-less range: sentinel meaning "to length" so the
               slicer clamps to `len(col)` instead of treating it as 0. */
            emit_const(c, xs_int(INT64_MAX));
        }
        emit(c, MAKE_A(OP_MAKE_RANGE, n->range.inclusive, 0));
        break;

    case NODE_LIT_REGEX: {
        /* /pat/ is a regex literal. Compile it into a runtime xs_regex value
           and push it as a constant. The interpreter does the same thing
           inline; the VM emits it as a prebuilt constant in the proto. */
        Value *rv = xs_regex(n->lit_regex.pattern ? n->lit_regex.pattern : "");
        emit_const(c, rv);
        break;
    }

    case NODE_INTERP_STRING: {
        int cnt = n->lit_string.parts.len;
        if (cnt == 0) {
            emit_const(c, xs_str(""));
            break;
        }
        for (int i = 0; i < cnt; i++) {
            Node *part = n->lit_string.parts.items[i];
            if (VAL_TAG(part) == NODE_LIT_STRING) {
                emit_const(c, xs_str(part->lit_string.sval ? part->lit_string.sval : ""));
            } else {
                emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, "str"));
                compile_node(c, part, 1);
                emit(c, MAKE_B(OP_CALL, 0, 0, 1));
            }
        }
        for (int i = 1; i < cnt; i++)
            emit(c, MAKE_A(OP_CONCAT, 0, 0));
        break;
    }

    case NODE_METHOD_CALL: {
        compile_node(c, n->method_call.obj, 1);
        if (n->method_call.optional) {
            emit(c, MAKE_A(OP_DUP, 0, 0));
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            emit(c, MAKE_A(OP_EQ, 0, 0));
            int skip = emit_jump(c, OP_JUMP_IF_TRUE);
            int argc = n->method_call.args.len;
            for (int i = 0; i < argc; i++)
                compile_node(c, n->method_call.args.items[i], 1);
            int ni = emit_global_name(c, n->method_call.method);
            emit(c, MAKE_A(OP_METHOD_CALL, (uint8_t)(unsigned)argc, (uint16_t)(unsigned)ni));
            int end = emit_jump(c, OP_JUMP);
            patch_jump(c, skip);
            emit(c, MAKE_A(OP_POP, 0, 0));
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            patch_jump(c, end);
        } else {
            int argc = n->method_call.args.len;
            for (int i = 0; i < argc; i++)
                compile_node(c, n->method_call.args.items[i], 1);
            int ni = emit_global_name(c, n->method_call.method);
            emit(c, MAKE_A(OP_METHOD_CALL, (uint8_t)(unsigned)argc, (uint16_t)(unsigned)ni));
        }
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_STRUCT_INIT: {
        /* Go through the class so default field values are applied. Loading
           the class by name first lets OP_MAKE_INST copy __fields defaults,
           then explicit fields override them via OP_STORE_FIELD. */
        const char *path = n->struct_init.path ? n->struct_init.path : "struct";
        int cnt = n->struct_init.fields.len;

        emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, path));
        int path_k = emit_global_name(c, path);
        emit(c, MAKE_A(OP_MAKE_INST, 0, (uint16_t)(unsigned)path_k));
        int inst_slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, inst_slot);

        if (n->struct_init.rest) {
            emit_a(c, OP_LOAD_LOCAL, inst_slot);
            compile_node(c, n->struct_init.rest, 1);
            emit(c, MAKE_A(OP_MAP_MERGE, 0, 0));
            emit(c, MAKE_A(OP_POP, 0, 0));
        }

        for (int i = 0; i < cnt; i++) {
            const char *k = n->struct_init.fields.items[i].key;
            Node *val     = n->struct_init.fields.items[i].val;
            if (!k) continue;
            emit_a(c, OP_LOAD_LOCAL, inst_slot);
            compile_node(c, val, 1);
            emit_a(c, OP_STORE_FIELD, emit_global_name(c, k));
        }

        emit_a(c, OP_LOAD_LOCAL, inst_slot);
        break;
    }

    case NODE_FOR: {
        int iter_slot = local_add_hidden(c);
        int len_slot  = local_add_hidden(c);
        int idx_slot  = local_add_hidden(c);

        compile_node(c, n->for_loop.iter, 1);
        emit_a(c, OP_STORE_LOCAL, iter_slot);

        emit_a(c, OP_LOAD_LOCAL, iter_slot);
        emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
        emit_a(c, OP_STORE_LOCAL, len_slot);

        emit_const(c, xs_int(0));
        emit_a(c, OP_STORE_LOCAL, idx_slot);

        int loop_top = c->current->proto->chunk.len;
        loop_push_label(c, 0, n->for_loop.label); /* continue_target patched below */

        emit_a(c, OP_LOAD_LOCAL, idx_slot);
        emit_a(c, OP_LOAD_LOCAL, len_slot);
        emit(c, MAKE_A(OP_LT, 0, 0));
        int j_exit = emit_jump(c, OP_JUMP_IF_FALSE);

        /* Push a fresh scope for the iteration variable so any closure
           that captures it gets a per-iteration slot via the
           OP_CLOSE_UPVALUES emitted by scope_end. Without this,
           `for i in 0..3 { fns.push(|| i) }` ends up with every
           closure pointing at the same final-iteration slot. */
        scope_begin(c);

        emit_a(c, OP_LOAD_LOCAL, iter_slot);
        emit_a(c, OP_LOAD_LOCAL, idx_slot);
        Node *pat = n->for_loop.pattern;
        int want_pairs = (pat && VAL_TAG(pat) == NODE_PAT_TUPLE) ? 1 : 0;
        emit(c, MAKE_A(OP_ITER_GET, want_pairs, 0));
        const char *pat_name = NULL;
        if (pat) {
            if (VAL_TAG(pat) == NODE_IDENT)     pat_name = pat->ident.name;
            if (VAL_TAG(pat) == NODE_PAT_IDENT) pat_name = pat->pat_ident.name;
        }
        if (pat_name) {
            int vs = local_add(c->current, pat_name);
            emit_a(c, OP_STORE_LOCAL, vs);
        } else if (pat && (VAL_TAG(pat) == NODE_PAT_TUPLE || VAL_TAG(pat) == NODE_PAT_SLICE)) {
            NodeList *elems = (VAL_TAG(pat) == NODE_PAT_TUPLE)
                ? &pat->pat_tuple.elems : &pat->pat_slice.elems;
            int elem_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, elem_slot);
            for (int di = 0; di < elems->len; di++) {
                Node *sub = elems->items[di];
                emit_a(c, OP_LOAD_LOCAL, elem_slot);
                emit_const(c, xs_int(di));
                emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                if (VAL_TAG(sub) == NODE_PAT_IDENT && sub->pat_ident.name) {
                    int ds = local_add(c->current, sub->pat_ident.name);
                    emit_a(c, OP_STORE_LOCAL, ds);
                } else if (VAL_TAG(sub) == NODE_PAT_WILD) {
                    emit(c, MAKE_A(OP_POP, 0, 0));
                } else {
                    emit(c, MAKE_A(OP_POP, 0, 0));
                }
            }
        } else {
            emit(c, MAKE_A(OP_POP, 0, 0));
        }

        compile_node(c, n->for_loop.body, 0);

        /* End the iteration scope (closes captured upvalues so the next
           iteration sees fresh slots). */
        scope_end(c);

        /* patch continue target to here (the increment) */
        {
            LoopCtx *lc = &c->loop_stack[c->loop_depth - 1];
            int cont_ip = c->current->proto->chunk.len;
            lc->continue_target = cont_ip;
            /* patch deferred continue jumps */
            for (int ci = 0; ci < lc->n_continue_patches; ci++) {
                int pidx = lc->continue_patches[ci];
                int offset = cont_ip - pidx - 1;
                Instruction *ip2 = &c->current->proto->chunk.code[pidx];
                *ip2 = (*ip2 & 0x0000FFFFU) | ((Instruction)(uint16_t)(int16_t)offset << 16);
            }
        }

        emit_a(c, OP_LOAD_LOCAL, idx_slot);
        emit_const(c, xs_int(1));
        emit(c, MAKE_A(OP_ADD, 0, 0));
        emit_a(c, OP_STORE_LOCAL, idx_slot);

        int back_off = loop_top - (c->current->proto->chunk.len + 1);
        emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)back_off));

        patch_jump(c, j_exit);
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        loop_pop_patch_breaks(c);
        return;
    }

    case NODE_LOOP: {
        int loop_top = c->current->proto->chunk.len;
        loop_push_label(c, loop_top, n->loop.label);
        compile_node(c, n->loop.body, 0);
        int back_off = loop_top - (c->current->proto->chunk.len + 1);
        emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)back_off));
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        loop_pop_patch_breaks(c);
        return;
    }

    case NODE_BREAK: {
        if (n->brk.value) {
            compile_node(c, n->brk.value, 1);
        }
        int idx = emit_jump(c, OP_JUMP);
        if (n->brk.label && c->loop_depth > 0) {
            int found_label = 0;
            for (int li = c->loop_depth - 1; li >= 0; li--) {
                if (c->loop_stack[li].label[0] && strcmp(c->loop_stack[li].label, n->brk.label) == 0) {
                    if (c->loop_stack[li].n_break_patches < MAX_BREAK_PATCHES)
                        c->loop_stack[li].break_patches[c->loop_stack[li].n_break_patches++] = idx;
                    found_label = 1;
                    break;
                }
            }
            if (!found_label) loop_add_break(c, idx);
        } else {
            loop_add_break(c, idx);
        }
        return;
    }
    case NODE_CONTINUE: {
        int target_depth = c->loop_depth - 1;
        if (n->cont.label) {
            for (int li = c->loop_depth - 1; li >= 0; li--) {
                if (c->loop_stack[li].label[0] && strcmp(c->loop_stack[li].label, n->cont.label) == 0) {
                    target_depth = li; break;
                }
            }
        }
        if (target_depth >= 0) {
            int top = c->loop_stack[target_depth].continue_target;
            if (top == 0) {
                /* continue target not yet known (for loop), defer via continue_patches */
                int idx = emit_jump(c, OP_JUMP);
                LoopCtx *lc = &c->loop_stack[target_depth];
                if (lc->n_continue_patches < MAX_BREAK_PATCHES)
                    lc->continue_patches[lc->n_continue_patches++] = idx;
            } else {
                int off = top - (c->current->proto->chunk.len + 1);
                emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)off));
            }
        }
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    // --- match
    case NODE_MATCH: {
        int subj_slot = local_add_hidden(c);
        compile_node(c, n->match.subject, 1);
        emit_a(c, OP_STORE_LOCAL, subj_slot);

        int n_arms = n->match.arms.len;
        int *arm_jumps = n_arms > 0 ? xs_malloc((size_t)n_arms * sizeof(int)) : NULL;
        int n_arm_jumps = 0;

        for (int ai = 0; ai < n_arms; ai++) {
            MatchArm *arm = &n->match.arms.items[ai];
            Node *pat = arm->pattern;

            int j_next = -1;
            int tuple_jumps[16];
            int n_tuple_jumps = 0;

            if (!pat || VAL_TAG(pat) == NODE_PAT_WILD) {
                /* wildcard: always matches, no binding */
            } else if (VAL_TAG(pat) == NODE_PAT_IDENT && !arm->guard) {
                /* Catchall binding: `n => ...` matches anything and binds
                   `n` to the subject. Without this, the arm body can't
                   reference the bound name. */
                if (pat->pat_ident.name) {
                    int bs = local_add(c->current, pat->pat_ident.name);
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    emit_a(c, OP_STORE_LOCAL, bs);
                }
            } else if (VAL_TAG(pat) == NODE_PAT_LIT) {
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                switch (pat->pat_lit.tag) {
                case 0: emit_const(c, xs_int(pat->pat_lit.ival));   break;
                case 1: emit_const(c, xs_float(pat->pat_lit.fval)); break;
                case 2: emit_const(c, xs_str(pat->pat_lit.sval));   break;
                case 3: emit(c, MAKE_A(pat->pat_lit.bval ? OP_PUSH_TRUE : OP_PUSH_FALSE, 0, 0)); break;
                default: emit(c, MAKE_A(OP_PUSH_NULL, 0, 0)); break;
                }
                emit(c, MAKE_A(OP_EQ, 0, 0));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
            } else if (VAL_TAG(pat) == NODE_PAT_RANGE || VAL_TAG(pat) == NODE_RANGE) {
                Node *start = (VAL_TAG(pat) == NODE_PAT_RANGE)
                    ? pat->pat_range.start : pat->range.start;
                Node *end_n = (VAL_TAG(pat) == NODE_PAT_RANGE)
                    ? pat->pat_range.end   : pat->range.end;
                int incl = (VAL_TAG(pat) == NODE_PAT_RANGE)
                    ? pat->pat_range.inclusive : pat->range.inclusive;
                compile_node(c, start, 1);
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit(c, MAKE_A(OP_LTE, 0, 0));
                int jf_lo = emit_jump(c, OP_JUMP_IF_FALSE);
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                compile_node(c, end_n, 1);
                emit(c, incl ? MAKE_A(OP_LTE, 0, 0) : MAKE_A(OP_LT, 0, 0));
                int jf_hi = emit_jump(c, OP_JUMP_IF_FALSE);
                if (arm->guard) {
                    compile_node(c, arm->guard, 1);
                    int jf_guard = emit_jump(c, OP_JUMP_IF_FALSE);
                    compile_node(c, arm->body, want_value);
                    arm_jumps[n_arm_jumps++] = emit_jump(c, OP_JUMP);
                    patch_jump(c, jf_lo);
                    patch_jump(c, jf_hi);
                    patch_jump(c, jf_guard);
                } else {
                    compile_node(c, arm->body, want_value);
                    arm_jumps[n_arm_jumps++] = emit_jump(c, OP_JUMP);
                    patch_jump(c, jf_lo);
                    patch_jump(c, jf_hi);
                }
                continue; /* handled */
            } else if (VAL_TAG(pat) == NODE_PAT_CAPTURE) {
                Node *sub = pat->pat_capture.pattern;
                if (sub && VAL_TAG(sub) == NODE_PAT_RANGE) {
                    Node *start = sub->range.start;
                    Node *end_n = sub->range.end;
                    int incl = sub->range.inclusive;
                    compile_node(c, start, 1);
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    emit(c, MAKE_A(OP_LTE, 0, 0));
                    int jf_start = emit_jump(c, OP_JUMP_IF_FALSE);
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    compile_node(c, end_n, 1);
                    emit(c, incl ? MAKE_A(OP_LTE, 0, 0) : MAKE_A(OP_LT, 0, 0));
                    int jf_end = emit_jump(c, OP_JUMP_IF_FALSE);
                    if (pat->pat_capture.name) {
                        int bs = local_add(c->current, pat->pat_capture.name);
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        emit_a(c, OP_STORE_LOCAL, bs);
                    }
                    if (arm->guard) {
                        compile_node(c, arm->guard, 1);
                        j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                    }
                    compile_node(c, arm->body, want_value);
                    arm_jumps[n_arm_jumps++] = emit_jump(c, OP_JUMP);
                    patch_jump(c, jf_start);
                    patch_jump(c, jf_end);
                    if (j_next >= 0) patch_jump(c, j_next);
                    continue; /* handled */
                } else if (!sub || VAL_TAG(sub) == NODE_PAT_WILD) {
                    if (pat->pat_capture.name) {
                        int bs = local_add(c->current, pat->pat_capture.name);
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        emit_a(c, OP_STORE_LOCAL, bs);
                    }
                } else {
                    /* unsupported sub-pattern */
                }
            } else if (VAL_TAG(pat) == NODE_PAT_IDENT) {
                int bs = local_add(c->current, pat->pat_ident.name);
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit_a(c, OP_STORE_LOCAL, bs);
            } else if (VAL_TAG(pat) == NODE_PAT_TUPLE) {
                compile_tuple_pattern_at(c, pat, subj_slot,
                                         tuple_jumps, &n_tuple_jumps, 16);
            } else if (VAL_TAG(pat) == NODE_PAT_ENUM) {
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                {
                    int tag_idx = emit_global_name(c, "_tag");
                    emit_a(c, OP_LOAD_FIELD, tag_idx);
                }
                {
                    const char *epath = pat->pat_enum.path;
                    const char *last_colon = strrchr(epath, ':');
                    const char *variant = (last_colon && last_colon > epath) ? last_colon + 1 : epath;
                    emit_const(c, xs_str(variant));
                }
                emit(c, MAKE_A(OP_EQ, 0, 0));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                for (int eai = 0; eai < pat->pat_enum.args.len; eai++) {
                    Node *arg_pat = pat->pat_enum.args.items[eai];
                    if (!arg_pat || VAL_TAG(arg_pat) == NODE_PAT_WILD) continue;
                    if (VAL_TAG(arg_pat) == NODE_PAT_IDENT) {
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        {
                            int val_idx = emit_global_name(c, "_val");
                            emit_a(c, OP_LOAD_FIELD, val_idx);
                        }
                        if (pat->pat_enum.args.len > 1) {
                            emit_const(c, xs_int(eai));
                            emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                        }
                        int slot = local_add(c->current, arg_pat->pat_ident.name);
                        emit_a(c, OP_STORE_LOCAL, slot);
                    } else {
                        /* Nested struct / tuple / map / lit pattern inside
                           an enum arg. Push the arg value, then route
                           through compile_sub_pattern_tos so its tests
                           run and any fails skip past the arm. */
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        int val_idx = emit_global_name(c, "_val");
                        emit_a(c, OP_LOAD_FIELD, val_idx);
                        if (pat->pat_enum.args.len > 1) {
                            emit_const(c, xs_int(eai));
                            emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                        }
                        compile_sub_pattern_tos(c, arg_pat,
                                                tuple_jumps,
                                                &n_tuple_jumps, 16);
                    }
                }
            } else if (VAL_TAG(pat) == NODE_PAT_OR) {
                /* Flatten the OR tree into leaves. Leaves can be PAT_LIT,
                   PAT_IDENT, PAT_WILD, or PAT_ENUM. Each leaf either tests
                   the subject and jumps to a shared hit target on success,
                   or unconditionally falls through (catchall). PAT_ENUM
                   leaves with payload args bind the args using slots
                   allocated once; alternatives must bind the same names. */
                Node *stack[32]; int stop = 0;
                stack[stop++] = pat;
                Node *leaves[64]; int nleaves = 0;
                while (stop > 0 && nleaves < 64) {
                    Node *cur = stack[--stop];
                    if (cur && VAL_TAG(cur) == NODE_PAT_OR) {
                        if (stop < 30) {
                            if (cur->pat_or.right) stack[stop++] = cur->pat_or.right;
                            if (cur->pat_or.left)  stack[stop++] = cur->pat_or.left;
                        }
                    } else if (cur) {
                        leaves[nleaves++] = cur;
                    }
                }
                /* Pre-allocate slots for any enum payload bindings using
                   the first enum leaf as the template. All enum leaves in
                   an or-pattern must bind the same set of names. */
                int payload_slots[8]; int n_payload = 0;
                int payload_total = 0;
                for (int li = 0; li < nleaves; li++) {
                    Node *lf = leaves[li];
                    if (VAL_TAG(lf) == NODE_PAT_ENUM) {
                        int n_args = lf->pat_enum.args.len;
                        if (n_args > payload_total) payload_total = n_args;
                        for (int eai = 0; eai < n_args && n_payload < 8; eai++) {
                            Node *arg_pat = lf->pat_enum.args.items[eai];
                            if (arg_pat && VAL_TAG(arg_pat) == NODE_PAT_IDENT &&
                                arg_pat->pat_ident.name && eai >= n_payload) {
                                payload_slots[n_payload++] =
                                    local_add(c->current, arg_pat->pat_ident.name);
                            }
                        }
                        break;
                    }
                }
                int hit_jumps[64]; int n_hit = 0;
                int has_catchall = 0;
                for (int li = 0; li < nleaves; li++) {
                    Node *lf = leaves[li];
                    if (VAL_TAG(lf) == NODE_PAT_LIT) {
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        switch (lf->pat_lit.tag) {
                        case 0: emit_const(c, xs_int(lf->pat_lit.ival)); break;
                        case 1: emit_const(c, xs_float(lf->pat_lit.fval)); break;
                        case 2: emit_const(c, xs_str(lf->pat_lit.sval)); break;
                        case 3: emit(c, MAKE_A(lf->pat_lit.bval ? OP_PUSH_TRUE : OP_PUSH_FALSE, 0, 0)); break;
                        default: emit(c, MAKE_A(OP_PUSH_NULL, 0, 0)); break;
                        }
                        emit(c, MAKE_A(OP_EQ, 0, 0));
                        if (n_hit < 64)
                            hit_jumps[n_hit++] = emit_jump(c, OP_JUMP_IF_TRUE);
                    } else if (VAL_TAG(lf) == NODE_PAT_ENUM) {
                        /* Test enum tag; if mismatch, skip to next leaf.
                           If match, bind args into pre-allocated slots and
                           jump to the shared hit target. */
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        {
                            int tag_idx = emit_global_name(c, "_tag");
                            emit_a(c, OP_LOAD_FIELD, tag_idx);
                        }
                        {
                            const char *epath = lf->pat_enum.path;
                            const char *last_colon = epath ? strrchr(epath, ':') : NULL;
                            const char *variant = (last_colon && last_colon > epath)
                                                  ? last_colon + 1 : epath;
                            emit_const(c, xs_str(variant ? variant : ""));
                        }
                        emit(c, MAKE_A(OP_EQ, 0, 0));
                        int j_skip_leaf = emit_jump(c, OP_JUMP_IF_FALSE);
                        for (int eai = 0; eai < lf->pat_enum.args.len && eai < n_payload; eai++) {
                            Node *arg_pat = lf->pat_enum.args.items[eai];
                            if (!arg_pat || VAL_TAG(arg_pat) != NODE_PAT_IDENT) continue;
                            emit_a(c, OP_LOAD_LOCAL, subj_slot);
                            int val_idx = emit_global_name(c, "_val");
                            emit_a(c, OP_LOAD_FIELD, val_idx);
                            if (payload_total > 1) {
                                emit_const(c, xs_int(eai));
                                emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                            }
                            emit_a(c, OP_STORE_LOCAL, payload_slots[eai]);
                        }
                        if (n_hit < 64)
                            hit_jumps[n_hit++] = emit_jump(c, OP_JUMP);
                        patch_jump(c, j_skip_leaf);
                    } else if (VAL_TAG(lf) == NODE_PAT_WILD || VAL_TAG(lf) == NODE_PAT_IDENT) {
                        has_catchall = 1;
                        if (VAL_TAG(lf) == NODE_PAT_IDENT) {
                            int bs = local_add(c->current, lf->pat_ident.name);
                            emit_a(c, OP_LOAD_LOCAL, subj_slot);
                            emit_a(c, OP_STORE_LOCAL, bs);
                        }
                    }
                }
                if (!has_catchall) {
                    /* No leaf matched: jump past the arm. */
                    j_next = emit_jump(c, OP_JUMP);
                }
                /* all hit jumps land here */
                for (int hi = 0; hi < n_hit; hi++)
                    patch_jump(c, hit_jumps[hi]);
            } else if (VAL_TAG(pat) == NODE_PAT_SLICE) {
                int head_n = pat->pat_slice.elems.len;
                /* Guard: subject must actually be array-like. Without this
                   an int like 99 would sail through because ITER_LEN returns
                   0 for it and the pattern [] would spuriously match. */
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit_const(c, xs_str("<array-like>"));
                emit(c, MAKE_A(OP_IS, 0, 0));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                /* Closed pattern (no rest): require exact length. Open
                   pattern: require at least head_n. */
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
                emit_const(c, xs_int(head_n));
                emit(c, pat->pat_slice.rest
                        ? MAKE_A(OP_GTE, 0, 0)
                        : MAKE_A(OP_EQ, 0, 0));
                if (n_tuple_jumps < 16)
                    tuple_jumps[n_tuple_jumps++] = emit_jump(c, OP_JUMP_IF_FALSE);
                for (int si = 0; si < head_n; si++) {
                    Node *elem_pat = pat->pat_slice.elems.items[si];
                    if (VAL_TAG(elem_pat) == NODE_PAT_IDENT) {
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        emit_const(c, xs_int(si));
                        emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                        int slot = local_add(c->current, elem_pat->pat_ident.name);
                        emit_a(c, OP_STORE_LOCAL, slot);
                    } else if (VAL_TAG(elem_pat) == NODE_PAT_LIT) {
                        emit_a(c, OP_LOAD_LOCAL, subj_slot);
                        emit_const(c, xs_int(si));
                        emit(c, MAKE_A(OP_INDEX_GET, 0, 0));
                        switch (elem_pat->pat_lit.tag) {
                        case 0: emit_const(c, xs_int(elem_pat->pat_lit.ival)); break;
                        case 1: emit_const(c, xs_float(elem_pat->pat_lit.fval)); break;
                        case 2: emit_const(c, xs_str(elem_pat->pat_lit.sval)); break;
                        case 3: emit(c, MAKE_A(elem_pat->pat_lit.bval ? OP_PUSH_TRUE : OP_PUSH_FALSE, 0, 0)); break;
                        default: emit(c, MAKE_A(OP_PUSH_NULL, 0, 0)); break;
                        }
                        emit(c, MAKE_A(OP_EQ, 0, 0));
                        j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                    } else if (VAL_TAG(elem_pat) == NODE_PAT_WILD) {
                        /* nothing to do */
                    }
                }
                /* rest binding: subject.slice(head_n) */
                if (pat->pat_slice.rest) {
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    emit_const(c, xs_int(head_n));
                    int sl_k = emit_global_name(c, "slice");
                    emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)sl_k));
                    int rs = local_add(c->current, pat->pat_slice.rest);
                    emit_a(c, OP_STORE_LOCAL, rs);
                }
            } else if (VAL_TAG(pat) == NODE_PAT_EXPR) {
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                compile_node(c, pat->pat_expr.expr, 1);
                emit(c, MAKE_A(OP_EQ, 0, 0));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
            } else if (VAL_TAG(pat) == NODE_PAT_GUARD) {
                if (pat->pat_guard.pattern && VAL_TAG(pat->pat_guard.pattern) == NODE_PAT_IDENT) {
                    int bs = local_add(c->current, pat->pat_guard.pattern->pat_ident.name);
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    emit_a(c, OP_STORE_LOCAL, bs);
                }
                if (pat->pat_guard.guard) {
                    compile_node(c, pat->pat_guard.guard, 1);
                    j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                }
            } else if (VAL_TAG(pat) == NODE_PAT_STRUCT) {
                /* Routes through compile_struct_pattern_at so nested
                   patterns inside fields actually run their tests. The
                   common per-arm fail-jump array (tuple_jumps) gets
                   any new fails appended. */
                compile_struct_pattern_at(c, pat, subj_slot,
                                          tuple_jumps, &n_tuple_jumps, 16);
            } else if (VAL_TAG(pat) == NODE_PAT_STRING_CONCAT) {
                /* "prefix" ++ rest: match iff subject is a string starting
                   with prefix, bind rest to the tail. */
                const char *prefix = pat->pat_str_concat.prefix ? pat->pat_str_concat.prefix : "";
                size_t plen = strlen(prefix);

                /* starts_with(subject, prefix) as a bool, using method call */
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit_const(c, xs_str(prefix));
                int sw_k = emit_global_name(c, "starts_with");
                emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)sw_k));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);

                /* Compute rest = subject.slice(plen). slice(start) with one
                   arg returns the substring from start to end. */
                Node *rest = pat->pat_str_concat.rest;
                if (rest && VAL_TAG(rest) == NODE_PAT_IDENT) {
                    emit_a(c, OP_LOAD_LOCAL, subj_slot);
                    emit_const(c, xs_int((int64_t)plen));
                    int sl_k = emit_global_name(c, "slice");
                    emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)sl_k));
                    int rs = local_add(c->current, rest->pat_ident.name);
                    emit_a(c, OP_STORE_LOCAL, rs);
                } else if (rest && VAL_TAG(rest) == NODE_PAT_WILD) {
                    /* no binding needed */
                }
            } else if (VAL_TAG(pat) == NODE_PAT_MAP) {
                /* Guard: subject must be map-like. Without this the
                   pattern #{} would spuriously match any value, including
                   ints, because the field loop below has nothing to check
                   on an empty pattern. */
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                emit_const(c, xs_str("<map-like>"));
                emit(c, MAKE_A(OP_IS, 0, 0));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
                compile_map_pattern_at(c, pat, subj_slot,
                                       tuple_jumps, &n_tuple_jumps, 16);
            } else if (VAL_TAG(pat) == NODE_PAT_REGEX) {
                /* /re/ arm: match iff subject is a string that the regex
                   matches fully. Anchor the pattern with ^...$ at compile
                   time and call test(). */
                const char *raw = pat->pat_regex.pattern ? pat->pat_regex.pattern : "";
                size_t rlen = strlen(raw);
                char *anchored = xs_malloc(rlen + 3);
                anchored[0] = '^';
                memcpy(anchored + 1, raw, rlen);
                anchored[rlen + 1] = '$';
                anchored[rlen + 2] = '\0';
                Value *rv = xs_regex(anchored);
                free(anchored);
                int rk = chunk_add_const(&c->current->proto->chunk, rv);
                value_decref(rv);
                emit_a(c, OP_PUSH_CONST, rk);
                emit_a(c, OP_LOAD_LOCAL, subj_slot);
                int test_k = emit_global_name(c, "test");
                emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)test_k));
                j_next = emit_jump(c, OP_JUMP_IF_FALSE);
            }

            int j_guard = -1;
            if (arm->guard) {
                compile_node(c, arm->guard, 1);
                j_guard = emit_jump(c, OP_JUMP_IF_FALSE);
            }

            compile_node(c, arm->body, want_value);
            arm_jumps[n_arm_jumps++] = emit_jump(c, OP_JUMP);
            if (j_next >= 0)  patch_jump(c, j_next);
            if (j_guard >= 0) patch_jump(c, j_guard);
            /* patch per-element fail jumps from tuple / slice / map /
               struct patterns, plus the nested-arg path on enum. */
            if (pat && (VAL_TAG(pat) == NODE_PAT_TUPLE ||
                        VAL_TAG(pat) == NODE_PAT_SLICE ||
                        VAL_TAG(pat) == NODE_PAT_MAP ||
                        VAL_TAG(pat) == NODE_PAT_STRUCT ||
                        VAL_TAG(pat) == NODE_PAT_ENUM)) {
                for (int tj = 0; tj < n_tuple_jumps; tj++)
                    patch_jump(c, tuple_jumps[tj]);
            }
        }

        for (int i = 0; i < n_arm_jumps; i++)
            patch_jump(c, arm_jumps[i]);
        if (arm_jumps) free(arm_jumps);

        if (want_value) {
            /* fallthrough with null if no arm matched */
        }
        return;
    }

    case NODE_STRUCT_DECL: {
        int nfields = n->struct_decl.fields.len;
        for (int fi = 0; fi < nfields; fi++) {
            const char *fname = n->struct_decl.fields.items[fi].key;
            Node *fdefault = n->struct_decl.fields.items[fi].val;
            emit_const(c, xs_str(fname ? fname : "?"));
            if (fdefault)
                compile_node(c, fdefault, 1);
            else
                emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        int name_idx = emit_global_name(c, n->struct_decl.name);
        emit(c, MAKE_A(OP_MAKE_CLASS, (uint8_t)(unsigned)nfields, (uint16_t)(unsigned)name_idx));
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->struct_decl.name);
        return;
    }

    case NODE_ENUM_DECL: {
        int nvariants = n->enum_decl.variants.len;
        for (int vi = 0; vi < nvariants; vi++) {
            EnumVariant *v = &n->enum_decl.variants.items[vi];
            emit_const(c, xs_str(v->name));
            if (v->fields.len == 0) {
                emit_const(c, xs_str("_tag"));
                emit_const(c, xs_str(v->name));
                emit_const(c, xs_str("__type"));
                emit_const(c, xs_str(n->enum_decl.name));
                emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 2));
            } else {
                int arity = v->fields.len;
                XSProto *parent = c->current->proto;
                XSProto *ctor = proto_new(v->name, arity);

                if (parent->n_inner == parent->cap_inner) {
                    parent->cap_inner = parent->cap_inner ? parent->cap_inner * 2 : 4;
                    parent->inner = xs_realloc(parent->inner,
                                        (size_t)parent->cap_inner * sizeof(XSProto *));
                }
                int inner_idx = parent->n_inner;
                parent->inner[parent->n_inner++] = ctor;

                CompilerScope ctor_scope;
                scope_push(c, &ctor_scope, ctor);

                for (int pi = 0; pi < arity; pi++) {
                    char pbuf[32];
                    snprintf(pbuf, sizeof pbuf, "p%d", pi);
                    local_add(c->current, pbuf);
                }

                emit_const(c, xs_str("_tag"));
                emit_const(c, xs_str(v->name));
                emit_const(c, xs_str("__type"));
                emit_const(c, xs_str(n->enum_decl.name));
                emit_const(c, xs_str("_val"));
                if (arity == 1) {
                    emit_a(c, OP_LOAD_LOCAL, 0);
                } else {
                    for (int pi = 0; pi < arity; pi++)
                        emit_a(c, OP_LOAD_LOCAL, pi);
                    emit(c, MAKE_B(OP_MAKE_TUPLE, 0, 0, (uint8_t)(unsigned)arity));
                }
                emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 3));
                emit(c, MAKE_A(OP_RETURN, 0, 0));

                scope_pop(c);
                emit_make_closure(c, inner_idx);
            }
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)nvariants));
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->enum_decl.name);
        return;
    }

    case NODE_CLASS_DECL: {
        int nbases = n->class_decl.nbases;
        int field_count = 0, method_count = 0;
        for (int mi = 0; mi < n->class_decl.members.len; mi++) {
            Node *mem = n->class_decl.members.items[mi];
            if (VAL_TAG(mem) == NODE_LET || VAL_TAG(mem) == NODE_VAR) field_count++;
            else if (VAL_TAG(mem) == NODE_FN_DECL) method_count++;
        }

        int fields_slot = local_add_hidden(c);
        int methods_slot = local_add_hidden(c);
        int bases_slot = local_add_hidden(c);
        for (int mi = 0; mi < n->class_decl.members.len; mi++) {
            Node *mem = n->class_decl.members.items[mi];
            if (VAL_TAG(mem) == NODE_LET || VAL_TAG(mem) == NODE_VAR) {
                emit_const(c, xs_str(mem->let.name ? mem->let.name : "?"));
                if (mem->let.value) compile_node(c, mem->let.value, 1);
                else emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            }
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)field_count));
        emit_a(c, OP_STORE_LOCAL, fields_slot);

        for (int mi = 0; mi < n->class_decl.members.len; mi++) {
            Node *mem = n->class_decl.members.items[mi];
            if (VAL_TAG(mem) == NODE_FN_DECL) {
                emit_const(c, xs_str(mem->fn_decl.name ? mem->fn_decl.name : "?"));
                int fidx = compile_fn(c, mem->fn_decl.name,
                                      &mem->fn_decl.params, mem->fn_decl.body);
                emit_make_closure(c, fidx);
            }
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)method_count));
        emit_a(c, OP_STORE_LOCAL, methods_slot);

        for (int bi = 0; bi < nbases; bi++)
            compile_name_load(c, n->class_decl.bases[bi]);
        emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0, (uint8_t)(unsigned)nbases));
        emit_a(c, OP_STORE_LOCAL, bases_slot);

        emit_const(c, xs_str("__name"));
        emit_const(c, xs_str(n->class_decl.name));
        emit_const(c, xs_str("__fields"));
        emit_a(c, OP_LOAD_LOCAL, fields_slot);
        emit_const(c, xs_str("__methods"));
        emit_a(c, OP_LOAD_LOCAL, methods_slot);
        emit_const(c, xs_str("__bases"));
        emit_a(c, OP_LOAD_LOCAL, bases_slot);
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 4));

        for (int bi = 0; bi < nbases; bi++) {
            compile_name_load(c, n->class_decl.bases[bi]);
            emit(c, MAKE_A(OP_INHERIT, 0, 0));
        }

        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->class_decl.name);
        return;
    }

    case NODE_TRAIT_DECL: {
        int npairs = 0;

        /* __name */
        emit_const(c, xs_str("__name"));
        emit_const(c, xs_str(n->trait_decl.name));
        npairs++;

        /* __methods array */
        emit_const(c, xs_str("__methods"));
        for (int mi = 0; mi < n->trait_decl.n_methods; mi++)
            emit_const(c, xs_str(n->trait_decl.method_names[mi]));
        emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0,
                        (uint8_t)(unsigned)n->trait_decl.n_methods));
        npairs++;

        /* __defaults map: name -> closure for methods with bodies */
        emit_const(c, xs_str("__defaults"));
        int ndefaults = 0;
        for (int j = 0; j < n->trait_decl.methods.len; j++) {
            Node *meth = n->trait_decl.methods.items[j];
            if (VAL_TAG(meth) != NODE_FN_DECL || !meth->fn_decl.body ||
                !meth->fn_decl.name) continue;
            emit_const(c, xs_str(meth->fn_decl.name));
            int fidx = compile_fn(c, meth->fn_decl.name,
                                  &meth->fn_decl.params,
                                  meth->fn_decl.body);
            emit_make_closure(c, fidx);
            ndefaults++;
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)ndefaults));
        npairs++;

        /* __assoc_types array */
        emit_const(c, xs_str("__assoc_types"));
        for (int ai = 0; ai < n->trait_decl.n_assoc_types; ai++)
            emit_const(c, xs_str(n->trait_decl.assoc_types[ai]));
        emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0,
                        (uint8_t)(unsigned)n->trait_decl.n_assoc_types));
        npairs++;

        /* __super (parent trait or null) */
        emit_const(c, xs_str("__super"));
        if (n->trait_decl.super_trait)
            emit_const(c, xs_str(n->trait_decl.super_trait));
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        npairs++;

        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)npairs));

        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->trait_decl.name);
        return;
    }

    // --- impl
    case NODE_IMPL_DECL: {
        for (int mi = 0; mi < n->impl_decl.members.len; mi++) {
            Node *mem = n->impl_decl.members.items[mi];
            if (VAL_TAG(mem) != NODE_FN_DECL || !mem->fn_decl.name) continue;

            compile_name_load(c, n->impl_decl.type_name);
            emit_const(c, xs_str(mem->fn_decl.name));

            int fidx = compile_fn(c, mem->fn_decl.name,
                                  &mem->fn_decl.params,
                                  mem->fn_decl.body);
            emit_make_closure(c, fidx);
            emit(c, MAKE_A(OP_IMPL_METHOD, 0, 0));

            emit_make_closure(c, fidx);
            compile_name_store(c, mem->fn_decl.name);
        }
        /* impl T for C: fold trait default methods into C's method map */
        if (n->impl_decl.trait_name) {
            compile_name_load(c, n->impl_decl.type_name);
            compile_name_load(c, n->impl_decl.trait_name);
            emit(c, MAKE_A(OP_TRAIT_APPLY, 0, 0));
        }
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    case NODE_IMPORT: {
        if (n->import.nparts == 0) {
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            return;
        }
        char *modname = n->import.path[0];

        if (n->import.nitems > 0) {
            compile_name_load(c, modname);
            int mod_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, mod_slot);
            for (int ii = 0; ii < n->import.nitems; ii++) {
                emit_a(c, OP_LOAD_LOCAL, mod_slot);
                int ni = emit_global_name(c, n->import.items[ii]);
                emit_a(c, OP_LOAD_FIELD, ni);
                compile_name_store(c, n->import.items[ii]);
            }
        } else if (n->import.alias) {
            compile_name_load(c, modname);
            compile_name_store(c, n->import.alias);
        } else {
            compile_name_load(c, modname);
            int slot = local_add(c->current, modname);
            emit_a(c, OP_STORE_LOCAL, slot);
        }
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    /* module decl */
    case NODE_MODULE_DECL: {
        scope_begin(c);
        int mod_start_locals = c->current->n_locals;

        for (int mi = 0; mi < n->module_decl.body.len; mi++) {
            Node *item = n->module_decl.body.items[mi];
            if (VAL_TAG(item) == NODE_FN_DECL && item->fn_decl.name) {
                int fidx = compile_fn(c, item->fn_decl.name,
                                      &item->fn_decl.params, item->fn_decl.body);
                if (item->fn_decl.is_generator) {
                    XSProto *inner = c->current->proto->inner[fidx];
                    inner->arity = -(inner->arity + 1);
                }
                emit_make_closure(c, fidx);
                int slot = local_add(c->current, item->fn_decl.name);
                emit_a(c, OP_STORE_LOCAL, slot);
            } else if (VAL_TAG(item) == NODE_LET || VAL_TAG(item) == NODE_VAR) {
                compile_node(c, item, 0);
            } else {
                compile_node(c, item, 0);
            }
        }

        int mod_end_locals = c->current->n_locals;
        int n_mod_locals = mod_end_locals - mod_start_locals;

        for (int li = mod_start_locals; li < mod_end_locals; li++) {
            emit_const(c, xs_str(c->current->locals[li].name));
            emit_a(c, OP_LOAD_LOCAL, c->current->locals[li].slot);
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)n_mod_locals));

        scope_end(c);

        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->module_decl.name);
        return;
    }

    case NODE_TYPE_ALIAS:
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;

    case NODE_CAST: {
        /* `x as T` in the interpreter actually coerces. Mirror that by
           desugaring into the builtin conversion function for the target
           type: int/float/str/bool/char. Unknown target names pass through
           unchanged, matching the interpreter's default branch. */
        const char *t = n->cast.type_name ? n->cast.type_name : "";
        const char *fn = NULL;
        if (strcmp(t,"i64")==0 || strcmp(t,"int")==0 || strcmp(t,"i32")==0 ||
            strcmp(t,"i128")==0|| strcmp(t,"isize")==0||
            strcmp(t,"u64")==0 || strcmp(t,"u32")==0 || strcmp(t,"u8")==0 ||
            strcmp(t,"u16")==0 || strcmp(t,"u128")==0|| strcmp(t,"usize")==0) fn = "int";
        else if (strcmp(t,"f64")==0 || strcmp(t,"float")==0 || strcmp(t,"f32")==0) fn = "float";
        else if (strcmp(t,"str")==0 || strcmp(t,"String")==0) fn = "str";
        else if (strcmp(t,"bool")==0) fn = "bool";
        else if (strcmp(t,"char")==0) fn = "char";

        if (fn && want_value) {
            emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, fn));
            compile_node(c, n->cast.expr, 1);
            emit(c, MAKE_B(OP_CALL, 0, 0, 1));
        } else {
            compile_node(c, n->cast.expr, want_value);
        }
        return;
    }

    case NODE_THROW:
        if (n->throw_.value)
            compile_node(c, n->throw_.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        emit(c, MAKE_A(OP_THROW, 0, 0));
        return;

    // --- try/catch/finally
    case NODE_TRY: {
        /* If there's a finally block, register it as a defer first so
           any path out of the body (return/throw/break/continue/normal
           fall-through) runs the finally. The deferred code lives at
           defer_finally; OP_DEFER_PUSH skips over it on the regular
           forward path. After the try body + catch, we explicitly
           OP_DEFER_RUN it so the finally fires on the success path too. */
        int defer_jmp = -1;
        if (n->try_.finally_block) {
            defer_jmp = emit_jump(c, OP_DEFER_PUSH);
            compile_node(c, n->try_.finally_block, 0);
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            emit(c, MAKE_A(OP_RETURN, 0, 0));
            patch_jump(c, defer_jmp);
        }
        int try_start = emit_jump(c, OP_TRY_BEGIN);
        compile_node(c, n->try_.body, want_value);
        emit(c, MAKE_A(OP_TRY_END, 0, 0));
        int over_catch = emit_jump(c, OP_JUMP);
        patch_jump(c, try_start);
        emit(c, MAKE_A(OP_CATCH, 0, 0));
        if (n->try_.catch_arms.len > 0) {
            MatchArm *arm = &n->try_.catch_arms.items[0];
            if (arm->pattern && VAL_TAG(arm->pattern) == NODE_IDENT
                    && arm->pattern->ident.name) {
                int slot = local_add(c->current, arm->pattern->ident.name);
                emit_a(c, OP_STORE_LOCAL, slot);
            } else if (arm->pattern && VAL_TAG(arm->pattern) == NODE_PAT_IDENT
                    && arm->pattern->pat_ident.name) {
                int slot = local_add(c->current, arm->pattern->pat_ident.name);
                emit_a(c, OP_STORE_LOCAL, slot);
            } else {
                emit(c, MAKE_A(OP_POP, 0, 0));
            }
            if (arm->body)
                compile_node(c, arm->body, want_value);
            else if (want_value)
                emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        } else if (n->try_.finally_block) {
            /* Bare try/finally with no catch: re-throw whatever the
               body raised so an outer handler still sees it. The
               finally runs first because it's already deferred. */
            emit(c, MAKE_A(OP_THROW, 0, 0));
        } else {
            emit(c, MAKE_A(OP_POP, 0, 0));
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        patch_jump(c, over_catch);
        if (n->try_.finally_block) {
            emit(c, MAKE_A(OP_DEFER_RUN, 0, 0));
        }
        return;
    }

    case NODE_SPREAD:
        compile_node(c, n->spread.expr, 1);
        emit(c, MAKE_A(OP_SPREAD, 0, 0));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;

    case NODE_LIST_COMP: {
        emit(c, MAKE_B(OP_MAKE_ARRAY, 0, 0, 0));
        int arr_slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, arr_slot);

        for (int ci = 0; ci < n->list_comp.clause_iters.len; ci++) {
            Node *iter_expr = n->list_comp.clause_iters.items[ci];
            Node *cpat = n->list_comp.clause_pats.items[ci];
            Node *cond = (ci < n->list_comp.clause_conds.len)
                         ? n->list_comp.clause_conds.items[ci] : NULL;

            /* compile iterator */
            compile_node(c, iter_expr, 1);
            int iter_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, iter_slot);

            /* len = iter.len */
            emit_a(c, OP_LOAD_LOCAL, iter_slot);
            emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
            int len_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, len_slot);

            /* idx = 0 */
            emit_const(c, xs_int(0));
            int idx_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, idx_slot);

            /* loop: */
            int loop_top = c->current->proto->chunk.len;

            /* if idx >= len: break */
            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit_a(c, OP_LOAD_LOCAL, len_slot);
            emit(c, MAKE_A(OP_GTE, 0, 0));
            int exit_jump = emit_jump(c, OP_JUMP_IF_TRUE);

            /* elem = iter[idx] */
            emit_a(c, OP_LOAD_LOCAL, iter_slot);
            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit(c, MAKE_A(OP_ITER_GET, 0, 0));

            /* bind pattern variable */
            const char *cpat_name = NULL;
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT) cpat_name = cpat->pat_ident.name;
            else if (cpat && VAL_TAG(cpat) == NODE_IDENT) cpat_name = cpat->ident.name;
            if (cpat_name) {
                int var_slot = local_add(c->current, cpat_name);
                emit_a(c, OP_STORE_LOCAL, var_slot);
            } else {
                emit(c, MAKE_A(OP_POP, 0, 0));
            }

            /* if cond: only add if true */
            int skip_push = -1;
            if (cond) {
                compile_node(c, cond, 1);
                skip_push = emit_jump(c, OP_JUMP_IF_FALSE);
            }

            /* result.push(element_expr) */
            emit_a(c, OP_LOAD_LOCAL, arr_slot);
            compile_node(c, n->list_comp.element, 1);
            {
                int push_idx = emit_global_name(c, "push");
                emit(c, MAKE_A(OP_METHOD_CALL, 1, (uint16_t)(unsigned)push_idx));
            }
            emit(c, MAKE_A(OP_POP, 0, 0)); /* discard push return */

            if (skip_push >= 0)
                patch_jump(c, skip_push);

            /* idx++ */
            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit_const(c, xs_int(1));
            emit(c, MAKE_A(OP_ADD, 0, 0));
            emit_a(c, OP_STORE_LOCAL, idx_slot);

            /* jump to loop top */
            {
                int back = c->current->proto->chunk.len;
                emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)(loop_top - back - 1)));
            }

            patch_jump(c, exit_jump);
        }

        /* Push result array */
        if (want_value)
            emit_a(c, OP_LOAD_LOCAL, arr_slot);
        return;
    }


    case NODE_MAP_COMP: {
        /* result = {} */
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 0));
        int map_slot = local_add_hidden(c);
        emit_a(c, OP_STORE_LOCAL, map_slot);

        for (int ci = 0; ci < n->map_comp.clause_iters.len; ci++) {
            Node *iter_expr = n->map_comp.clause_iters.items[ci];
            Node *cpat = n->map_comp.clause_pats.items[ci];
            Node *cond = (ci < n->map_comp.clause_conds.len)
                         ? n->map_comp.clause_conds.items[ci] : NULL;

            compile_node(c, iter_expr, 1);
            int iter_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, iter_slot);
            emit_a(c, OP_LOAD_LOCAL, iter_slot);
            emit(c, MAKE_A(OP_ITER_LEN, 0, 0));
            int len_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, len_slot);
            emit_const(c, xs_int(0));
            int idx_slot = local_add_hidden(c);
            emit_a(c, OP_STORE_LOCAL, idx_slot);

            int loop_top = c->current->proto->chunk.len;
            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit_a(c, OP_LOAD_LOCAL, len_slot);
            emit(c, MAKE_A(OP_GTE, 0, 0));
            int exit_jump = emit_jump(c, OP_JUMP_IF_TRUE);

            emit_a(c, OP_LOAD_LOCAL, iter_slot);
            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit(c, MAKE_A(OP_ITER_GET, 0, 0));

            const char *cpat_name = NULL;
            if (cpat && VAL_TAG(cpat) == NODE_PAT_IDENT) cpat_name = cpat->pat_ident.name;
            else if (cpat && VAL_TAG(cpat) == NODE_IDENT) cpat_name = cpat->ident.name;
            if (cpat_name) {
                int var_slot = local_add(c->current, cpat_name);
                emit_a(c, OP_STORE_LOCAL, var_slot);
            } else {
                emit(c, MAKE_A(OP_POP, 0, 0));
            }

            int skip_push = -1;
            if (cond) {
                compile_node(c, cond, 1);
                skip_push = emit_jump(c, OP_JUMP_IF_FALSE);
            }

            /* map[key] = value */
            emit_a(c, OP_LOAD_LOCAL, map_slot);
            compile_node(c, n->map_comp.key, 1);
            compile_node(c, n->map_comp.value, 1);
            emit(c, MAKE_A(OP_INDEX_SET, 0, 0));

            if (skip_push >= 0) patch_jump(c, skip_push);

            emit_a(c, OP_LOAD_LOCAL, idx_slot);
            emit_const(c, xs_int(1));
            emit(c, MAKE_A(OP_ADD, 0, 0));
            emit_a(c, OP_STORE_LOCAL, idx_slot);

            int back = c->current->proto->chunk.len;
            emit(c, MAKE_A(OP_JUMP, 0, (uint16_t)(int16_t)(loop_top - back - 1)));
            patch_jump(c, exit_jump);
        }

        if (want_value)
            emit_a(c, OP_LOAD_LOCAL, map_slot);
        return;
    }

    case NODE_DEFER: {
        int defer_start = emit_jump(c, OP_DEFER_PUSH);
        compile_node(c, n->defer_.body, 0);
        emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        emit(c, MAKE_A(OP_RETURN, 0, 0));
        patch_jump(c, defer_start);
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    case NODE_YIELD: {
        /* tagged block: NODE_TAG_DECL desugared the body into a fn with
           an implicit __block param. inside that body `yield` means
           "invoke the block" -- not "produce the next generator value".
           interp does the same check at runtime via env_get("__block");
           we resolve it at compile time so the VM/JIT pick it up too.
           checks the local first, then walks the upvalue chain so a
           nested fn inside the tag still yields-as-call (interp does
           the env-walk for free). */
        int block_slot = local_resolve(c->current, "__block");
        int block_uv   = block_slot < 0
                        ? upvalue_resolve(c->current, "__block") : -1;
        if (block_slot >= 0 || block_uv >= 0) {
            if (block_slot >= 0) emit_a(c, OP_LOAD_LOCAL,   block_slot);
            else                 emit_a(c, OP_LOAD_UPVALUE, block_uv);
            emit(c, MAKE_B(OP_CALL, 0, 0, 0));
            if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
            return;
        }
        if (n->yield_.value)
            compile_node(c, n->yield_.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        emit(c, MAKE_A(OP_YIELD, 0, 0));
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    /* actor */
    case NODE_ACTOR_DECL: {
        int nstate = n->actor_decl.state_fields.len;
        int nmethods = n->actor_decl.methods.len;
        int state_slot = local_add_hidden(c);
        int meth_slot = local_add_hidden(c);

        for (int si = 0; si < nstate; si++) {
            const char *fname = n->actor_decl.state_fields.items[si].key;
            Node *def = n->actor_decl.state_fields.items[si].val;
            emit_const(c, xs_str(fname ? fname : "?"));
            if (def) compile_node(c, def, 1);
            else emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)nstate));
        emit_a(c, OP_STORE_LOCAL, state_slot);

        /* compile actor methods: add implicit 'self' param and state var accessors */
        char **state_names = NULL;
        if (nstate > 0) {
            state_names = xs_malloc((size_t)nstate * sizeof(char*));
            for (int si = 0; si < nstate; si++)
                state_names[si] = xs_strdup(n->actor_decl.state_fields.items[si].key ?
                    n->actor_decl.state_fields.items[si].key : "?");
        }
        int actual_methods = 0;
        for (int mi = 0; mi < nmethods; mi++) {
            Node *m = n->actor_decl.methods.items[mi];
            if (VAL_TAG(m) != NODE_FN_DECL) continue;
            emit_const(c, xs_str(m->fn_decl.name ? m->fn_decl.name : "?"));
            /* compile actor method with self + state field loading */
            {
                int method_arity = m->fn_decl.params.len + 1; /* +1 for self */
                XSProto *parent = c->current->proto;
                XSProto *inner = proto_new(m->fn_decl.name ? m->fn_decl.name : "<actor_method>", method_arity);
                inner->is_actor_method = 1;
                if (parent->n_inner == parent->cap_inner) {
                    parent->cap_inner = parent->cap_inner ? parent->cap_inner * 2 : 4;
                    parent->inner = xs_realloc(parent->inner, (size_t)parent->cap_inner * sizeof(XSProto *));
                }
                int inner_idx = parent->n_inner;
                parent->inner[parent->n_inner++] = inner;
                CompilerScope fn_scope;
                scope_push(c, &fn_scope, inner);
                /* self is local 0 */
                int self_slot = local_add(c->current, "self");
                (void)self_slot;
                /* add user params */
                for (int pi = 0; pi < m->fn_decl.params.len; pi++) {
                    const char *pname = m->fn_decl.params.items[pi].name;
                    local_add(c->current, pname ? pname : "<param>");
                }
                /* add state field locals and load from self */
                for (int si = 0; si < nstate; si++) {
                    int slot = local_add(c->current, state_names[si]);
                    emit_a(c, OP_LOAD_LOCAL, 0); /* self */
                    int fi = emit_global_name(c, state_names[si]);
                    emit_a(c, OP_LOAD_FIELD, fi);
                    emit_a(c, OP_STORE_LOCAL, slot);
                }
                /* save state field info for write-back before returns */
                c->current->actor_state_names = state_names;
                c->current->actor_nstate = nstate;
                compile_node(c, m->fn_decl.body, 1);
                c->current->actor_state_names = NULL;
                c->current->actor_nstate = 0;
                /* write back state fields to self (for fall-through) */
                for (int si = 0; si < nstate; si++) {
                    int slot = local_resolve(c->current, state_names[si]);
                    if (slot >= 0) {
                        emit_a(c, OP_LOAD_LOCAL, 0); /* self */
                        emit_a(c, OP_LOAD_LOCAL, slot);
                        int fi = emit_global_name(c, state_names[si]);
                        emit_a(c, OP_STORE_FIELD, fi);
                    }
                }
                XSChunk *ch = &inner->chunk;
                if (ch->len == 0 || INSTR_OPCODE(ch->code[ch->len - 1]) != OP_RETURN)
                    emit(c, MAKE_A(OP_RETURN, 0, 0));
                if (fn_scope.n_upvalues > 0) {
                    inner->uv_descs = xs_malloc((size_t)fn_scope.n_upvalues * sizeof(UVDesc));
                    memcpy(inner->uv_descs, fn_scope.uv_descs, (size_t)fn_scope.n_upvalues * sizeof(UVDesc));
                    inner->n_upvalues = fn_scope.n_upvalues;
                }
                scope_pop(c);
                emit_make_closure(c, inner_idx);
            }
            actual_methods++;
        }
        if (state_names) { for (int si = 0; si < nstate; si++) free(state_names[si]); free(state_names); }
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, (uint8_t)(unsigned)actual_methods));
        emit_a(c, OP_STORE_LOCAL, meth_slot);

        emit_const(c, xs_str("__actor_name"));
        emit_const(c, xs_str(n->actor_decl.name));
        emit_const(c, xs_str("__state"));
        emit_a(c, OP_LOAD_LOCAL, state_slot);
        emit_const(c, xs_str("__methods"));
        emit_a(c, OP_LOAD_LOCAL, meth_slot);
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 3));

        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->actor_decl.name);
        return;
    }

    case NODE_SEND_EXPR: {
        compile_node(c, n->send_expr.target, 1);
        compile_node(c, n->send_expr.message, 1);
        emit(c, MAKE_A(OP_SEND, 0, 0));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_AWAIT:
        compile_node(c, n->await_.expr, 1);
        emit(c, MAKE_A(OP_AWAIT, 0, 0));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;

    case NODE_SPAWN: {
        /* `spawn Counter` (a bare identifier referring to an actor class)
           must instantiate the actor synchronously so the caller sees a
           usable actor-instance map, not a future to await. Push the
           identifier directly so OP_SPAWN's actor branch handles it. */
        if (n->spawn_.expr && n->spawn_.expr->tag == NODE_IDENT) {
            compile_node(c, n->spawn_.expr, 1);
            emit(c, MAKE_A(OP_SPAWN, 0, 0));
            if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
            return;
        }
        int idx = compile_fn(c, "__spawn__", NULL, n->spawn_.expr);
        emit_make_closure(c, idx);
        emit(c, MAKE_A(OP_SPAWN, 0, 0));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_NURSERY:
        emit(c, MAKE_A(OP_NURSERY_BEGIN, 0, 0));
        compile_node(c, n->nursery_.body, 0);
        emit(c, MAKE_A(OP_NURSERY_END, 0, 0));
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;

    case NODE_PERFORM: {
        for (int i = 0; i < n->perform.args.len; i++)
            compile_node(c, n->perform.args.items[i], 1);
        char buf[256];
        snprintf(buf, sizeof buf, "%s.%s", n->perform.effect_name, n->perform.op_name);
        int name_idx = emit_global_name(c, buf);
        emit(c, MAKE_A(OP_EFFECT_CALL, (uint8_t)n->perform.args.len, (uint16_t)(unsigned)name_idx));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }


    case NODE_HANDLE: {
        /* OP_EFFECT_CALL leaves [..., eff_val, eff_name_str] on the
         * stack at catch entry. Each arm checks the effect name and
         * skips on mismatch; if no arm matches we throw a
         * "unhandled effect" error. Matches interp's strict semantics
         * (no implicit catch-all on the last arm) so a typo in the
         * arm name surfaces instead of silently routing every effect
         * through it. Body and each arm are compiled with want_value=1
         * so HANDLE_BODY_END always has a single value to swap into
         * the arm's resume call site (and arm bodies are
         * structurally consistent for OP_EFFECT_DONE). The trailing
         * pop below restores the caller's want_value contract. */
        int try_start = emit_jump(c, OP_TRY_BEGIN);
        compile_node(c, n->handle.expr, 1);
        emit(c, MAKE_A(OP_TRY_END, 0, 0));
        emit(c, MAKE_A(OP_HANDLE_BODY_END, 0, 0));
        int over_handler = emit_jump(c, OP_JUMP);
        patch_jump(c, try_start);
        emit(c, MAKE_A(OP_CATCH, 0, 0));

        int n_arms = n->handle.arms.len;
        if (n_arms == 0) {
            emit(c, MAKE_A(OP_POP, 0, 0)); /* drop eff_name */
            emit(c, MAKE_A(OP_POP, 0, 0)); /* drop eff_val */
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            emit(c, MAKE_A(OP_EFFECT_DONE, 0, 0));
        } else {
            int *out_jumps = xs_calloc((size_t)n_arms, sizeof(int));
            int n_out = 0;
            int *skip_jumps = xs_calloc((size_t)n_arms, sizeof(int));

            for (int ai = 0; ai < n_arms; ai++) {
                EffectArm *earm = &n->handle.arms.items[ai];

                char fullname[256];
                snprintf(fullname, sizeof fullname, "%s.%s",
                         earm->effect_name ? earm->effect_name : "?",
                         earm->op_name ? earm->op_name : "?");
                emit(c, MAKE_A(OP_DUP, 0, 0));
                emit_const(c, xs_str(fullname));
                emit(c, MAKE_A(OP_EQ, 0, 0));
                skip_jumps[ai] = emit_jump(c, OP_JUMP_IF_FALSE);

                /* matched: drop eff_name, bind eff_val to the arm's
                 * first param. */
                emit(c, MAKE_A(OP_POP, 0, 0));
                if (earm->params.len > 0) {
                    int slot = local_add(c->current, earm->params.items[0].name);
                    emit_a(c, OP_STORE_LOCAL, slot);
                } else {
                    emit(c, MAKE_A(OP_POP, 0, 0));
                }
                compile_node(c, earm->body, 1);
                emit(c, MAKE_A(OP_EFFECT_DONE, 0, 0));
                out_jumps[n_out++] = emit_jump(c, OP_JUMP);

                patch_jump(c, skip_jumps[ai]);
            }

            /* No arm matched: throw an unhandled-effect error carrying
             * the effect name. The catch's stack top has [eff_val,
             * eff_name]; build "unhandled effect: <name>" and re-throw. */
            emit_const(c, xs_str("unhandled effect: "));
            emit(c, MAKE_A(OP_SWAP, 0, 0));
            emit(c, MAKE_A(OP_CONCAT, 0, 0));
            emit(c, MAKE_A(OP_SWAP, 0, 0));
            emit(c, MAKE_A(OP_POP, 0, 0)); /* drop eff_val */
            emit(c, MAKE_A(OP_THROW, 0, 0));

            for (int i = 0; i < n_out; i++) patch_jump(c, out_jumps[i]);
            free(out_jumps);
            free(skip_jumps);
        }

        patch_jump(c, over_handler);
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    case NODE_RESUME:
        if (n->resume_.value)
            compile_node(c, n->resume_.value, 1);
        else
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        emit(c, MAKE_A(OP_EFFECT_RESUME, 0, 0));
        if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        return;

    case NODE_EFFECT_DECL:
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;

    case NODE_USE: {
        if (n->use_.is_plugin && n->use_.path) {
            /* emit: __load_plugin("path") */
            emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, "__load_plugin"));
            emit_const(c, xs_str(n->use_.path));
            emit(c, MAKE_B(OP_CALL, 0, 0, 1));
            if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        } else if (n->use_.path) {
            /* regular use: treat as no-op in VM for now */
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        } else {
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        return;
    }

    case NODE_LOAD: {
        /* `load "path"` is the canonical plugin load. Same runtime as
           `use plugin "path"` just with a different surface syntax. */
        if (n->load_.path) {
            emit_a(c, OP_LOAD_GLOBAL, emit_global_name(c, "__load_plugin"));
            emit_const(c, xs_str(n->load_.path));
            emit(c, MAKE_B(OP_CALL, 0, 0, 1));
            if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
        } else if (want_value) {
            emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        }
        return;
    }

    case NODE_INLINE_C:
        /* inline C not supported in VM mode */
        fprintf(stderr, "xs: error: inline C blocks require transpilation, not VM execution\n");
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;

    case NODE_BIND: {
        /* Compile bind as a simple let (no reactive in VM mode) */
        compile_node(c, n->bind_decl.expr, 1);
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->bind_decl.name);
        return;
    }

    case NODE_TAG_DECL: {
        /* Compile tag as a function with implicit __block param */
        ParamList augmented = paramlist_new();
        for (int ti = 0; ti < n->tag_decl.params.len; ti++)
            paramlist_push(&augmented, n->tag_decl.params.items[ti]);
        Param block_param = {0};
        block_param.name = "__block";
        Node *bp = node_new(NODE_PAT_IDENT, n->span);
        bp->pat_ident.name = xs_strdup("__block");
        bp->pat_ident.mutable = 0;
        block_param.pattern = bp;
        paramlist_push(&augmented, block_param);
        int idx = compile_fn(c, n->tag_decl.name, &augmented, n->tag_decl.body);
        (void)idx;
        emit_make_closure(c, idx);
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->tag_decl.name);
        return;
    }

    case NODE_ADAPT_FN: {
        /* Select "native" branch, fallback to first */
        int sel = 0;
        for (int ai = 0; ai < n->adapt_fn.nbranches; ai++) {
            if (strcmp(n->adapt_fn.targets[ai], "native") == 0) { sel = ai; break; }
        }
        if (n->adapt_fn.nbranches == 0) {
            if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
            return;
        }
        int idx = compile_fn(c, n->adapt_fn.name,
                             &n->adapt_fn.params,
                             n->adapt_fn.bodies[sel]);
        emit_make_closure(c, idx);
        if (want_value) emit(c, MAKE_A(OP_DUP, 0, 0));
        compile_name_store(c, n->adapt_fn.name);
        return;
    }

    case NODE_LIT_DURATION:
        emit_const(c, xs_float(n->lit_duration.ms));
        break;

    case NODE_LIT_COLOR: {
        /* build a map with r,g,b,a fields */
        emit_const(c, xs_str("r"));
        emit_const(c, xs_int(n->lit_color.r));
        emit_const(c, xs_str("g"));
        emit_const(c, xs_int(n->lit_color.g));
        emit_const(c, xs_str("b"));
        emit_const(c, xs_int(n->lit_color.b));
        emit_const(c, xs_str("a"));
        emit_const(c, xs_int(n->lit_color.a));
        emit(c, MAKE_B(OP_MAKE_MAP, 0, 0, 4));
        break;
    }

    case NODE_LIT_DATE:
        emit_const(c, xs_str(n->lit_date.value));
        break;

    case NODE_LIT_SIZE:
        emit_const(c, xs_float(n->lit_size.bytes));
        break;

    case NODE_LIT_ANGLE:
        emit_const(c, xs_float(n->lit_angle.radians));
        break;

    case NODE_EVERY:
        compile_node(c, n->every_.interval, 1);
        emit(c, MAKE_A(OP_POP, 0, 0));
        compile_node(c, n->every_.body, want_value);
        return;

    case NODE_AFTER:
        compile_node(c, n->after_.delay, 1);
        emit(c, MAKE_A(OP_POP, 0, 0));
        compile_node(c, n->after_.body, want_value);
        return;

    case NODE_TIMEOUT:
        compile_node(c, n->timeout_.body, want_value);
        return;

    case NODE_DEBOUNCE:
        compile_node(c, n->debounce_.delay, 1);
        emit(c, MAKE_A(OP_POP, 0, 0));
        compile_node(c, n->debounce_.body, want_value);
        return;

    case NODE_PLUGIN_DECL:
        /* Plugin metadata and parser productions are registered during
           parsing; nothing to emit at runtime. */
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;

    case NODE_DO_EXPR:
        compile_node(c, n->do_expr.body, want_value);
        return;

    case NODE_WITH: {
        /* Evaluate the resource, optionally bind it, run the body, then
           call .close() on the resource if it has one. */
        int res_slot = local_add_hidden(c);
        compile_node(c, n->with_.expr, 1);
        emit_a(c, OP_STORE_LOCAL, res_slot);

        if (n->with_.name) {
            int bs = local_add(c->current, n->with_.name);
            emit_a(c, OP_LOAD_LOCAL, res_slot);
            emit_a(c, OP_STORE_LOCAL, bs);
        }

        compile_node(c, n->with_.body, want_value);

        emit_a(c, OP_LOAD_LOCAL, res_slot);
        int close_k = emit_global_name(c, "close");
        emit(c, MAKE_A(OP_METHOD_CALL, 0, (uint16_t)(unsigned)close_k));
        emit(c, MAKE_A(OP_POP, 0, 0));
        return;
    }

    default:
        fprintf(stderr, "unhandled node tag %d\n", (int)VAL_TAG(n));
        if (want_value) emit(c, MAKE_A(OP_PUSH_NULL, 0, 0));
        return;
    }

    if (!want_value) emit(c, MAKE_A(OP_POP, 0, 0));
}

XSProto *compile_program(Node *program) {
    Compiler c = {0};
    CompilerScope top;
    XSProto *p = proto_new("<main>", 0);
    scope_push(&c, &top, p);
    compile_node(&c, program, 0);
    emit(&c, MAKE_A(OP_PUSH_NULL, 0, 0));
    emit(&c, MAKE_A(OP_RETURN,    0, 0));
    scope_pop(&c);
    return p;
}
