#include "vm/bytecode.h"
#include "core/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

XSProto *proto_new(const char *name, int arity) {
    XSProto *p = xs_malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->name = name ? xs_strdup(name) : NULL;
    p->arity = arity;
    p->refcount = 1;
    return p;
}

void proto_free(XSProto *p) {
    if (!p || --p->refcount > 0) return;
    free(p->name);
    for (int i = 0; i < p->chunk.nconsts; i++) value_decref(p->chunk.consts[i]);
    free(p->chunk.consts);
    free(p->chunk.code);
    /* Release inline-cached globals so their refcounts drop cleanly. */
    if (p->chunk.ic) {
        for (int i = 0; i < p->chunk.len; i++) value_decref(p->chunk.ic[i]);
        free(p->chunk.ic);
    }
    /* The field IC stores borrowed XSClass* identity pointers, not owned
     * Values, so just drop the array. */
    free(p->chunk.ic_class);
    free(p->chunk.ic_version);
    free(p->chunk.lines);
    free(p->chunk.cols);
    free(p->source_file);
    for (int i = 0; i < p->n_inner; i++) proto_free(p->inner[i]);
    free(p->inner);
    free(p->uv_descs);
    if (p->param_names) {
        for (int i = 0; i < p->n_params; i++) free(p->param_names[i]);
        free(p->param_names);
    }
    free(p);
}

int chunk_write(XSChunk *c, Instruction i) {
    if (c->len == c->cap) {
        int new_cap = c->cap ? c->cap * 2 : 16;
        c->code  = xs_realloc(c->code,  (size_t)new_cap * sizeof(Instruction));
        c->lines = xs_realloc(c->lines, (size_t)new_cap * sizeof(int));
        c->cols  = xs_realloc(c->cols,  (size_t)new_cap * sizeof(int));
        for (int k = c->cap; k < new_cap; k++) { c->lines[k] = 0; c->cols[k] = 0; }
        c->cap = new_cap;
    }
    c->code[c->len]  = i;
    c->lines[c->len] = 0;
    c->cols[c->len]  = 0;
    return c->len++;
}

void chunk_set_loc(XSChunk *c, int ip, int line, int col) {
    if (ip >= 0 && ip < c->len) {
        c->lines[ip] = line;
        c->cols[ip]  = col;
    }
}

int chunk_add_const(XSChunk *c, Value *v) {
    if (c->nconsts == c->cap_consts) {
        c->cap_consts = c->cap_consts ? c->cap_consts * 2 : 8;
        c->consts = xs_realloc(c->consts, (size_t)c->cap_consts * sizeof(Value*));
    }
    value_incref(v);
    c->consts[c->nconsts++] = v;
    return c->nconsts - 1;
}

const char *bytecode_op_name(Opcode op) {
    static const char *names[] = {
        [OP_NOP] = "NOP",
        [OP_PUSH_CONST] = "PUSH_CONST",
        [OP_PUSH_NULL] = "PUSH_NULL",
        [OP_PUSH_TRUE] = "PUSH_TRUE",
        [OP_PUSH_FALSE] = "PUSH_FALSE",
        [OP_POP] = "POP",
        [OP_DUP] = "DUP",
        [OP_LOAD_LOCAL] = "LOAD_LOCAL",
        [OP_STORE_LOCAL] = "STORE_LOCAL",
        [OP_LOAD_UPVALUE] = "LOAD_UPVALUE",
        [OP_STORE_UPVALUE] = "STORE_UPVALUE",
        [OP_LOAD_GLOBAL] = "LOAD_GLOBAL",
        [OP_STORE_GLOBAL] = "STORE_GLOBAL",
        [OP_ADD] = "ADD",
        [OP_SUB] = "SUB",
        [OP_MUL] = "MUL",
        [OP_DIV] = "DIV",
        [OP_MOD] = "MOD",
        [OP_POW] = "POW",
        [OP_NEG] = "NEG",
        [OP_NOT] = "NOT",
        [OP_EQ] = "EQ",
        [OP_NEQ] = "NEQ",
        [OP_LT] = "LT",
        [OP_GT] = "GT",
        [OP_LTE] = "LTE",
        [OP_GTE] = "GTE",
        [OP_CONCAT] = "CONCAT",
        [OP_MAKE_ARRAY] = "MAKE_ARRAY",
        [OP_MAKE_TUPLE] = "MAKE_TUPLE",
        [OP_MAKE_MAP] = "MAKE_MAP",
        [OP_INDEX_GET] = "INDEX_GET",
        [OP_INDEX_SET] = "INDEX_SET",
        [OP_LOAD_FIELD] = "LOAD_FIELD",
        [OP_STORE_FIELD] = "STORE_FIELD",
        [OP_JUMP] = "JUMP",
        [OP_JUMP_IF_FALSE] = "JUMP_IF_FALSE",
        [OP_JUMP_IF_TRUE] = "JUMP_IF_TRUE",
        [OP_MAKE_RANGE] = "MAKE_RANGE",
        [OP_ITER_LEN] = "ITER_LEN",
        [OP_ITER_GET] = "ITER_GET",
        [OP_METHOD_CALL] = "METHOD_CALL",
        [OP_MAKE_CLOSURE] = "MAKE_CLOSURE",
        [OP_CALL] = "CALL",
        [OP_TAIL_CALL] = "TAIL_CALL",
        [OP_CALL_KW] = "CALL_KW",
        [OP_RETURN] = "RETURN",
        [OP_SWAP] = "SWAP",
        [OP_BAND] = "BAND",
        [OP_BOR] = "BOR",
        [OP_BXOR] = "BXOR",
        [OP_BNOT] = "BNOT",
        [OP_SHL] = "SHL",
        [OP_SHR] = "SHR",
        [OP_THROW] = "THROW",
        [OP_TRY_BEGIN] = "TRY_BEGIN",
        [OP_TRY_END] = "TRY_END",
        [OP_CATCH] = "CATCH",
        [OP_TRACE_CALL] = "TRACE_CALL",
        [OP_TRACE_RETURN] = "TRACE_RETURN",
        [OP_TRACE_STORE] = "TRACE_STORE",
        [OP_TRACE_IO] = "TRACE_IO",
        [OP_AND] = "AND",
        [OP_OR] = "OR",
        [OP_SPREAD] = "SPREAD",
        [OP_LOOP] = "LOOP",
        [OP_EFFECT_CALL] = "EFFECT_CALL",
        [OP_EFFECT_RESUME] = "EFFECT_RESUME",
        [OP_EFFECT_HANDLE] = "EFFECT_HANDLE",
        [OP_HANDLE_BODY_END] = "HANDLE_BODY_END",
        [OP_EFFECT_DONE] = "EFFECT_DONE",
        [OP_AWAIT] = "AWAIT",
        [OP_YIELD] = "YIELD",
        [OP_SPAWN] = "SPAWN",
        [OP_MAKE_CLASS] = "MAKE_CLASS",
        [OP_MAKE_ENUM] = "MAKE_ENUM",
        [OP_MAKE_INST] = "MAKE_INST",
        [OP_IMPL_METHOD] = "IMPL_METHOD",
        [OP_TRAIT_APPLY] = "TRAIT_APPLY",
        [OP_INHERIT] = "INHERIT",
        [OP_MAKE_MODULE] = "MAKE_MODULE",
        [OP_END_MODULE] = "END_MODULE",
        [OP_IMPORT] = "IMPORT",
        [OP_IMPORT_ITEM] = "IMPORT_ITEM",
        [OP_DEFER_PUSH] = "DEFER_PUSH",
        [OP_DEFER_RUN] = "DEFER_RUN",
        [OP_MAKE_ACTOR] = "MAKE_ACTOR",
        [OP_SEND] = "SEND",
        [OP_FLOOR_DIV] = "FLOOR_DIV",
        [OP_SPACESHIP] = "SPACESHIP",
        [OP_OPT_CHAIN] = "OPT_CHAIN",
        [OP_NULL_COALESCE] = "NULL_COALESCE",
        [OP_TRY_OP] = "TRY_OP",
        [OP_PIPE] = "PIPE",
        [OP_IN] = "IN",
        [OP_IS] = "IS",
        [OP_MAP_MERGE] = "MAP_MERGE",
        [OP_CLOSE_UPVALUES] = "CLOSE_UPVALUES",
        [OP_NURSERY_BEGIN] = "NURSERY_BEGIN",
        [OP_NURSERY_END] = "NURSERY_END",
    };
    if ((unsigned)op >= OP__MAX) return "?";
    const char *n = names[op];
    return n ? n : "?";
}

/* .xsc binary format:
   header: "XSC\0" (4 bytes) + version u16
   per proto (recursive):
     name_len u16, name bytes
     arity u16, nlocals u16, n_upvalues u16
     n_code u32, code (n_code * 4 bytes)
     n_consts u16, per const: tag u8 + payload
     n_uv_descs u16, per desc: is_local u8 + index u16
     n_inner u16, then each inner proto recursively
*/

static void write_u8(FILE *f, uint8_t v)   { fwrite(&v, 1, 1, f); }
static void write_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void write_i64(FILE *f, int64_t v)  { fwrite(&v, 8, 1, f); }
static void write_f64(FILE *f, double v)   { fwrite(&v, 8, 1, f); }
static void write_str(FILE *f, const char *s) {
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    write_u16(f, len);
    if (len) fwrite(s, 1, len, f);
}

/* Two backing readers: one wraps a FILE*, one walks a memory buffer
   (used by xs_run_bytecode on platforms without a filesystem). Short
   reads leave the destination at zero; corrupt bytecode surfaces as a
   null/0 slot rather than a crash. Callers that care also check the
   magic header before trusting anything past it. */
typedef struct {
    FILE          *file;
    const uint8_t *buf;
    size_t         size;
    size_t         pos;
} Reader;

static void rd_init_file(Reader *r, FILE *f) {
    r->file = f; r->buf = NULL; r->size = 0; r->pos = 0;
}
static void rd_init_buf(Reader *r, const uint8_t *buf, size_t size) {
    r->file = NULL; r->buf = buf; r->size = size; r->pos = 0;
}
static size_t rd_read(Reader *r, void *dst, size_t n) {
    if (r->file) return fread(dst, 1, n, r->file);
    size_t avail = r->pos < r->size ? r->size - r->pos : 0;
    if (n > avail) n = avail;
    if (n) memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return n;
}

static uint8_t  read_u8(Reader *r)  { uint8_t v = 0;  rd_read(r, &v, 1); return v; }
static uint16_t read_u16(Reader *r) { uint16_t v = 0; rd_read(r, &v, 2); return v; }
static uint32_t read_u32(Reader *r) { uint32_t v = 0; rd_read(r, &v, 4); return v; }
static int64_t  read_i64(Reader *r) { int64_t v = 0;  rd_read(r, &v, 8); return v; }
static double   read_f64(Reader *r) { double v = 0;   rd_read(r, &v, 8); return v; }
static char *read_str(Reader *r) {
    uint16_t len = read_u16(r);
    if (!len) return NULL;
    char *s = xs_malloc(len + 1);
    rd_read(r, s, len);
    s[len] = 0;
    return s;
}

static void proto_write(FILE *f, XSProto *p) {
    write_str(f, p->name);
    write_u16(f, (uint16_t)p->arity);
    write_u16(f, (uint16_t)p->nlocals);
    write_u16(f, (uint16_t)p->n_upvalues);
    /* source file (per-proto; inner protos write their own copy) */
    write_str(f, p->source_file);
    /* code */
    write_u32(f, (uint32_t)p->chunk.len);
    for (int i = 0; i < p->chunk.len; i++)
        write_u32(f, p->chunk.code[i]);
    /* per-instruction line/col */
    for (int i = 0; i < p->chunk.len; i++) {
        write_u32(f, (uint32_t)(p->chunk.lines ? p->chunk.lines[i] : 0));
        write_u32(f, (uint32_t)(p->chunk.cols  ? p->chunk.cols[i]  : 0));
    }
    /* constants */
    write_u16(f, (uint16_t)p->chunk.nconsts);
    for (int i = 0; i < p->chunk.nconsts; i++) {
        Value *v = p->chunk.consts[i];
        if (!v || VAL_TAG(v) == XS_NULL) { write_u8(f, 0); }
        else if (VAL_TAG(v) == XS_INT)   { write_u8(f, 1); write_i64(f, VAL_INT(v)); }
        else if (VAL_TAG(v) == XS_FLOAT) { write_u8(f, 2); write_f64(f, v->f); }
        else if (VAL_TAG(v) == XS_STR)   { write_u8(f, 3); write_str(f, v->s); }
        else if (VAL_TAG(v) == XS_BOOL)  { write_u8(f, 4); write_u8(f, VAL_INT(v) ? 1 : 0); }
        else { write_u8(f, 0); } /* unsupported const type → null */
    }
    /* upvalue descriptors */
    write_u16(f, (uint16_t)p->n_upvalues);
    for (int i = 0; i < p->n_upvalues; i++) {
        write_u8(f, (uint8_t)p->uv_descs[i].is_local);
        write_u16(f, (uint16_t)p->uv_descs[i].index);
    }
    /* inner protos */
    write_u16(f, (uint16_t)p->n_inner);
    for (int i = 0; i < p->n_inner; i++)
        proto_write(f, p->inner[i]);
}

int proto_write_file(XSProto *p, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("XSC", 1, 4, f); /* includes null terminator */
    write_u16(f, 2); /* format version: v2 adds per-proto source_file +
                        per-instruction lines/cols */
    proto_write(f, p);
    fclose(f);
    return 0;
}

static XSProto *proto_read(Reader *r) {
    char *name = read_str(r);
    int arity = read_u16(r);
    XSProto *p = proto_new(name, arity);
    free(name);
    p->nlocals = read_u16(r);
    p->n_upvalues = read_u16(r);
    /* source file (per-proto) */
    p->source_file = read_str(r);
    /* code */
    int ncode = (int)read_u32(r);
    for (int i = 0; i < ncode; i++)
        chunk_write(&p->chunk, read_u32(r));
    /* per-instruction line/col */
    for (int i = 0; i < ncode; i++) {
        int line = (int)read_u32(r);
        int col  = (int)read_u32(r);
        chunk_set_loc(&p->chunk, i, line, col);
    }
    /* constants */
    int nconsts = read_u16(r);
    for (int i = 0; i < nconsts; i++) {
        uint8_t tag = read_u8(r);
        Value *v = NULL;
        switch (tag) {
            case 0: v = xs_null(); break;
            case 1: v = xs_int(read_i64(r)); break;
            case 2: v = xs_float(read_f64(r)); break;
            case 3: { char *s = read_str(r); v = xs_str(s ? s : ""); free(s); break; }
            case 4: v = read_u8(r) ? xs_bool(1) : xs_bool(0); break;
            default: v = xs_null(); break;
        }
        chunk_add_const(&p->chunk, v);
        value_decref(v);
    }
    /* upvalue descriptors */
    int nuv = read_u16(r);
    if (nuv > 0) {
        p->uv_descs = xs_malloc(nuv * sizeof(UVDesc));
        for (int i = 0; i < nuv; i++) {
            p->uv_descs[i].is_local = read_u8(r);
            p->uv_descs[i].index = read_u16(r);
        }
    }
    /* inner protos */
    int ninner = read_u16(r);
    for (int i = 0; i < ninner; i++) {
        XSProto *inner = proto_read(r);
        if (p->n_inner >= p->cap_inner) {
            p->cap_inner = p->cap_inner ? p->cap_inner * 2 : 4;
            p->inner = xs_realloc(p->inner, p->cap_inner * sizeof(XSProto*));
        }
        p->inner[p->n_inner++] = inner;
    }
    return p;
}

static int read_header(Reader *r) {
    char magic[4] = {0};
    if (rd_read(r, magic, 4) != 4 || memcmp(magic, "XSC", 4) != 0) return -1;
    uint16_t ver = read_u16(r);
    return ver == 2 ? 0 : -1;
}

XSProto *proto_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Reader r; rd_init_file(&r, f);
    if (read_header(&r) < 0) { fclose(f); return NULL; }
    XSProto *p = proto_read(&r);
    fclose(f);
    return p;
}

XSProto *proto_read_buf(const uint8_t *data, size_t size) {
    if (!data || size < 6) return NULL;
    Reader r; rd_init_buf(&r, data, size);
    if (read_header(&r) < 0) return NULL;
    return proto_read(&r);
}

void proto_dump(XSProto *p) {
    printf("=== proto <%s> arity=%d locals=%d ===\n",
           p->name ? p->name : "<anon>", p->arity, p->nlocals);
    for (int i = 0; i < p->chunk.len; i++) {
        Instruction in = p->chunk.code[i];
        printf("  %04d  %-20s A=%-3d B=%-3d C=%-3d Bx=%-5d sBx=%d\n",
               i, bytecode_op_name(INSTR_OPCODE(in)),
               INSTR_A(in), INSTR_B(in), INSTR_C(in),
               INSTR_Bx(in), (int)INSTR_sBx(in));
    }
    for (int i = 0; i < p->n_inner; i++) proto_dump(p->inner[i]);
}
