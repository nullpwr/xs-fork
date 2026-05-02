#ifndef VM_H
#define VM_H
#include "vm/bytecode.h"
#include "core/value.h"

#ifdef XSC_ENABLE_TRACER
#include "tracer/tracer.h"
#endif

#define VM_STACK_INIT    4096
#define VM_FRAMES_INIT    256
#define VM_TRY_STACK_MAX 64
#define VM_DEFER_MAX     64
#define VM_YIELD_MAX     1024
#define VM_MAX_TASKS     64

typedef struct {
    Value *fn;
    Value *result;
    int    done;
} VMTask;

typedef struct {
    Instruction *catch_ip;
    Value      **stack_top;  /* sp at TRY_BEGIN (for unwinding) */
} TryEntry;

/* open upvalue points to stack slot; closed holds its own copy */
typedef struct Upvalue {
    Value          **ptr;
    Value           *closed_val;
    int              is_open;
    int              refcount;
    struct Upvalue  *next;
} Upvalue;

typedef struct {
    Instruction *defer_ip;
} DeferEntry;

/* call frame */
typedef struct {
    Value       *closure_val;
    Instruction *ip;
    Value      **base;
    TryEntry     try_stack[VM_TRY_STACK_MAX];
    int          try_depth;
    DeferEntry   defer_stack[VM_DEFER_MAX];
    int          defer_depth;
    Instruction *defer_return_ip;
    int          is_generator;
    Value       *yield_arr;
    int          yield_index;
    /* Set when this frame is the constructor-init call invoked by
       OP_MAKE_INST. Only this frame's OP_RETURN consumes vm->init_inst,
       so a nested super.init(...) returning while init_inst is still
       held doesn't strip values from the outer init's operand stack. */
    int          owns_init_inst;
} CallFrame;

typedef struct {
    CallFrame  *frames;
    int         frames_cap;
    int         frame_count;
    int         sp_off;          /* offset from stack base */
    int         stack_top_off;   /* handler frame's sp at TRY_BEGIN */
    int         valid;
    /* Multi-shot snapshot: stack values for the suspended-body range
     * [stack_top_off .. sp_off) at perform time, each ref-counted.
     * Capturing only that slice (rather than the whole stack) leaves
     * the handler's own frame state untouched so a closure-mutated
     * outer var in the arm body persists across resume; the second
     * resume still sees the captured suspended-frames slots. */
    Value     **stack_snapshot;
    int         snapshot_len;
    int         snapshot_cap;
    /* Arm-body snapshot for multi-shot resume. When the handler arm
     * calls resume, we snapshot the arm's frames + stack here, then
     * restore the body. Once the body completes, OP_HANDLE_BODY_END
     * swaps in the saved arm state and the second resume can replay
     * the body snapshot above as many times as it likes. */
    int         in_resume;
    CallFrame  *arm_frames;
    int         arm_frames_cap;
    int         arm_frame_count;
    int         arm_sp_off;
    Value     **arm_stack_snapshot;
    int         arm_snapshot_len;
    int         arm_snapshot_cap;
} EffectCont;

typedef struct VM {
    Value     **stack;
    int         stack_cap;
    Value     **sp;
    CallFrame  *frames;
    int         frames_cap;
    int         frame_count;
    Upvalue    *open_upvalues;
    XSMap      *globals;
    int         main_called;
    Value      *init_inst;
    Value      *spawn_task;
    VMTask      tasks[VM_MAX_TASKS];
    int         n_tasks;
    EffectCont  eff_cont;
    /* LIFO stack of suspended continuations. Nested perform/handle
     * pairs each push a fresh snapshot so the inner handler's resume
     * lands on the inner body, and the outer handler's resume still
     * has its own captured frames + stack to rewind to. Heap-allocated
     * to keep the VM struct small (the JIT hardcodes a few VM offsets;
     * a fixed inline array would shift the rest of the struct). */
    EffectCont *eff_stack;
    int         eff_stack_count;
    int         eff_stack_cap;
#ifdef XSC_ENABLE_TRACER
    XSTracer   *tracer;
#endif
    /* When non-zero, vm_dispatch returns to its caller after each
       opcode instead of looping. The JIT relies on this to drive a
       single instruction per emitted call. */
    int         single_step;
    /* Sentinel set by vm_dispatch to signal it returned because of
       single_step rather than because the program finished. The JIT
       checks this to know whether to dispatch the next instruction. */
    int         step_yielded;
    /* Cached &stack[stack_cap - 16]. Recomputed by vm_grow_stack and
       in vm_new. Lets the JIT loop-top headroom check be a single
       cmp+jl instead of an arithmetic recompute every iteration. */
    Value     **stack_soft_limit;
    /* Monotonic counter bumped on every OP_STORE_GLOBAL. OP_LOAD_GLOBAL
       inline caches stash the version they observed; a mismatch forces
       a fresh map lookup. Lets hot code (fib calling itself) skip the
       repeated hashmap lookup without losing correctness when a program
       actually reassigns a global. */
    uint64_t    global_version;
    /* Exception unwinding state. When OP_THROW encounters a frame with
       pending defers but no try, it has to run those defers before the
       frame is popped; pending_throw_exc parks the in-flight value
       across the defer run so OP_RETURN's defer-drain completion can
       pick it up and continue unwinding. Lives on the VM rather than
       on CallFrame so the JIT's hardcoded FRAME_SIZE stays stable. */
    int         pending_throw_frame;
    Value      *pending_throw_exc;
    /* When non-zero, an unhandled OP_THROW captures the exception value
       on this VM (uncaught_thread_exc) instead of printing it to stderr.
       Used by spawn workers so a throw can be surfaced via the future. */
    int         is_thread_worker;
    Value      *uncaught_thread_exc;
    /* Stack of in-flight nurseries. Each entry collects the spawn task
       ids registered while its body was executing; OP_NURSERY_END awaits
       every id then drops the entry. Lets `nursery { spawn x; spawn y; }`
       run children in parallel and still join before the block exits. */
    int       **nursery_stack;
    int        *nursery_lens;
    int        *nursery_caps;
    int        *nursery_ids;       /* per-depth: this nursery's instance id */
    int        *nursery_prev_ids;  /* per-depth: tls_nursery_id at BEGIN */
    int         nursery_depth;
    int         nursery_stack_cap;
} VM;

VM  *vm_new(void);
void vm_free(VM *vm);
int  vm_run(VM *vm, XSProto *proto);
/* Run exactly one bytecode instruction from the current frame's ip.
   Returns 0 to continue, 1 if the program finished (frame_count==0),
   negative on error. Used by the JIT-generated machine code. */
int  vm_step(VM *vm);
int  vm_step_jit(VM *vm);  /* faster variant for JIT (assumes single_step=1) */
void vm_grow_stack(VM *vm); /* exposed for JIT's loop-top capacity check */
int  vm_call_closure_fast(VM *vm, int argc);  /* JIT OP_CALL fast path */
int  vm_return_fast(VM *vm);                  /* JIT OP_RETURN fast path */
Value *vm_load_global_ic(VM *vm, int ip_idx, uint16_t const_idx);  /* JIT IC */
/* Set up the top frame for `proto` then hand control to `entry` (which
   must be a function pointer to JIT-emitted machine code that takes
   the VM and returns an exit code). vm_run_with sweeps the same global
   state as vm_run on entry/exit so the two backends stay swappable. */
int  vm_run_with(VM *vm, XSProto *proto, int (*entry)(VM *));
void upvalue_close_all(Upvalue **list, Value **cutoff);

#endif /* VM_H */
