#ifndef BYTECODE_H
#define BYTECODE_H
#include "core/xs.h"
#include <stdint.h>
#include <stddef.h>

typedef uint32_t Instruction;

#define INSTR_OPCODE(i)   ((Opcode)((i) & 0xFF))
#define INSTR_A(i)        (((i) >> 8)  & 0xFF)
#define INSTR_Bx(i)       (((i) >> 16) & 0xFFFF)
#define INSTR_sBx(i)      ((int16_t)(((i) >> 16) & 0xFFFF))
#define INSTR_B(i)        (((i) >> 16) & 0xFF)
#define INSTR_C(i)        (((i) >> 24) & 0xFF)
#define MAKE_A(op,a,bx)   ((Instruction)(op)|((Instruction)(a)<<8)|((Instruction)(bx)<<16))
#define MAKE_B(op,a,b,c)  ((Instruction)(op)|((Instruction)(a)<<8)|((Instruction)(b)<<16)|((Instruction)(c)<<24))

typedef enum {
    OP_NOP = 0,

    OP_PUSH_CONST,    /* Bx=const_idx */
    OP_PUSH_NULL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_POP,
    OP_DUP,

    OP_LOAD_LOCAL,    /* Bx=slot */
    OP_STORE_LOCAL,
    OP_LOAD_UPVALUE,  /* Bx=idx */
    OP_STORE_UPVALUE,
    OP_LOAD_GLOBAL,   /* Bx=name_const */
    OP_STORE_GLOBAL,

    // --- arithmetic
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_NEG, OP_NOT,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_CONCAT,

    OP_MAKE_ARRAY,    /* C=count */
    OP_MAKE_TUPLE,
    OP_MAKE_MAP,      /* C=n_pairs */
    OP_INDEX_GET,
    OP_INDEX_SET,
    OP_LOAD_FIELD,    /* Bx=name_const */
    OP_STORE_FIELD,

    /* control */
    OP_JUMP,          /* sBx=offset */
    OP_JUMP_IF_FALSE,
    OP_JUMP_IF_TRUE,
    OP_MAKE_RANGE,    /* A=inclusive */
    OP_ITER_LEN,
    OP_ITER_GET,

    // --- calls
    OP_METHOD_CALL,   /* A=argc Bx=name_const */
    OP_MAKE_CLOSURE,  /* Bx=inner_proto_index_const */
    OP_CALL,          /* C=argc */
    OP_TAIL_CALL,     /* C=argc */
    OP_CALL_KW,       /* A=n_positional  C=n_kwargs  stack: callee [pos...] [key_str,val]* */
    OP_RETURN,
    OP_SWAP,

    OP_BAND, OP_BOR, OP_BXOR, OP_BNOT, OP_SHL, OP_SHR,

    /* error handling */
    OP_THROW,
    OP_TRY_BEGIN,     /* sBx=offset to catch */
    OP_TRY_END,
    OP_CATCH,

    OP_TRACE_CALL,
    OP_TRACE_RETURN,
    OP_TRACE_STORE,
    OP_TRACE_IO,

    OP_AND, OP_OR,
    OP_SPREAD,
    OP_LOOP,          /* backward jump */

    OP_EFFECT_CALL,   /* A=argc, Bx=effect_op_name */
    OP_EFFECT_RESUME,
    OP_EFFECT_HANDLE, /* Bx=handler_info */
    /* Multi-shot resume support: emitted at the body's TRY_END / arm
       body completion. BODY_END swaps body return value into the arm
       body's resume call site if a resume was in flight; DONE pops
       the eff_stack entry once the arm body has truly finished. */
    OP_HANDLE_BODY_END,
    OP_EFFECT_DONE,

    OP_AWAIT,
    OP_YIELD,
    OP_SPAWN,

    /* OOP */
    OP_MAKE_CLASS,    /* A=nfields Bx=name_const */
    OP_MAKE_ENUM,     /* Bx=name_const */
    OP_MAKE_INST,     /* A=nargs Bx=class_name */
    OP_IMPL_METHOD,
    OP_TRAIT_APPLY,    /* stack: class, trait -> copy defaults onto class methods */
    OP_INHERIT,

    OP_MAKE_MODULE,   /* Bx=name_const */
    OP_END_MODULE,
    OP_IMPORT,        /* Bx=name_const */
    OP_IMPORT_ITEM,   /* A=item_name Bx=module_name */

    OP_DEFER_PUSH,    /* Bx=offset to deferred code */
    OP_DEFER_RUN,

    OP_MAKE_ACTOR,    /* A=n_state_fields Bx=name_const */
    OP_SEND,

    OP_FLOOR_DIV,
    OP_SPACESHIP,     /* <=> */
    OP_OPT_CHAIN,     /* ?. */
    OP_NULL_COALESCE, /* ?? */
    OP_TRY_OP,        /* ? error propagation */
    OP_PIPE,          /* |> */
    OP_IN,
    OP_IS,
    OP_MAP_MERGE,
    OP_CLOSE_UPVALUES,  /* Bx=slot_threshold; close every open upvalue
                            whose slot >= base + Bx. Emitted on scope exit
                            so that loop-iteration locals captured by a
                            closure get their own value. */

    OP_NURSERY_BEGIN, /* push a fresh task-id collector onto the nursery
                         stack. spawn ops inside the body register their
                         task ids on the topmost collector. */
    OP_NURSERY_END,   /* await every task collected since BEGIN, then pop
                         the collector. */

    OP__MAX
} Opcode;

typedef struct { int is_local; int index; } UVDesc;

typedef struct {
    Instruction *code;
    int          len, cap;
    Value      **consts;
    int          nconsts, cap_consts;
    /* Inline caches: one slot per instruction. Populated lazily on
       first execution. Per-opcode interpretation:
         OP_LOAD_GLOBAL: ic[ip] = cached Value* of the global
                         ic_version[ip] = vm->global_version snapshot
         OP_LOAD_FIELD:  ic_class[ip] = receiver's XSClass* (identity)
                         ic_version[ip] = (fields->cap << 32) | bucket
       ic[] is owned (refcounted) and freed via value_decref in
       proto_free. ic_class[] is borrowed -- the class pointer is
       kept alive by the live instances that reach the IC, so we
       just free the array itself. */
    Value      **ic;           /* parallel to code[], owned Value* (LOAD_GLOBAL) */
    struct XSClass **ic_class; /* parallel to code[], borrowed (LOAD_FIELD) */
    uint64_t    *ic_version;   /* parallel to code[], opcode-specific */
} XSChunk;

typedef struct XSProto XSProto;
struct XSProto {
    char       *name;
    int         arity;
    int         nlocals;
    XSChunk     chunk;
    UVDesc     *uv_descs;
    int         n_upvalues;
    XSProto   **inner;
    int         n_inner, cap_inner;
    int         refcount;
    char      **param_names;   /* owned, NULL-terminated slots */
    int         n_params;
    int         is_generator;  /* 1 if declared with `fn*` */
    int         is_actor_method; /* 1 if body of an `actor { ... }` method.
                                  * The JIT bails on these: the actor
                                  * dispatcher feeds them an implicit
                                  * self-as-arg plus state-field locals
                                  * that the current call-emission path
                                  * doesn't model, so lowering segfaults
                                  * at runtime when the method also
                                  * captures outer upvalues. */
    int         is_variadic;
    /* Cached pointer to the proto's tier-2 JIT entry (int(*)(VM*)).
     * Populated by jit_compile when lowering succeeds; read by the
     * tier-2 inner-frame dispatcher (tier2_run_until) so recursive
     * calls stay in native code instead of falling back to the
     * interpreter. NULL when no tier-2 entry is available -- the
     * dispatcher then interprets. */
    void       *jit_entry;
    /* Set once tier-2 has been tried and declined (unsupported op,
     * cross-block operand leak, code buffer full, ...). Keeps the hot
     * path from re-running lowerer / liveness / codegen on every
     * invocation of a proto we already know we can't JIT. */
    unsigned char jit_tried;
};

XSProto *proto_new(const char *name, int arity);
void     proto_free(XSProto *p);
int      chunk_write(XSChunk *c, Instruction i);
int      chunk_add_const(XSChunk *c, Value *v);
void     proto_dump(XSProto *p);
const char *bytecode_op_name(Opcode op);

/* bytecode serialization (.xsc format) */
int      proto_write_file(XSProto *p, const char *path);
XSProto *proto_read_file(const char *path);
XSProto *proto_read_buf(const uint8_t *data, size_t size);

#endif /* BYTECODE_H */
