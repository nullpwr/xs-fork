/* Tier-2 JIT driver.
 *
 * Tier-2 is now the only JIT path: bytecode is lowered through the
 * register-allocating IR pipeline in ra_lower / ra_live / ra_alloc /
 * ra_codegen{,_arm64} and emitted straight into a mmap'd (or
 * VirtualAlloc'd on Windows) executable code buffer. Protos that use
 * an opcode outside the supported set -- or hit the vstack-underflow
 * guard for cross-block operand leaks -- return NULL from ralow_lower
 * and the caller falls back to the bytecode VM. Every other piece of
 * the old tier-1 dispatch JIT (the ~1500 lines of per-opcode helpers,
 * the jump-table dispatcher, the jit_rt_* runtime shims) was deleted
 * because benchmarks showed tier-2 dominated on every workload that
 * reached the JIT at all. */

#ifdef XSC_ENABLE_JIT

#include "jit/jit.h"
#include "jit/ra_ir.h"
#include "core/xs.h"
#include "core/value.h"
#include "vm/vm.h"
#include "vm/bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define JIT_ARCH_X86_64 1
#else
#define JIT_ARCH_X86_64 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define JIT_ARCH_ARM64  1
#else
#define JIT_ARCH_ARM64  0
#endif
#define JIT_ARCH_SUPPORTED (JIT_ARCH_X86_64 || JIT_ARCH_ARM64)

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/mman.h>
#include <unistd.h>
#define JIT_HAS_MMAP 1
#define JIT_HAS_WIN  0
#elif defined(_WIN32)
#include <windows.h>
#define JIT_HAS_MMAP 0
#define JIT_HAS_WIN  1
#else
#define JIT_HAS_MMAP 0
#define JIT_HAS_WIN  0
#endif

/* Let long-running workloads ask for a bigger code buffer than the 4 MiB
 * default. XS_JIT_CODE_SIZE_MB capped at 1 GiB. */
static size_t jit_pick_code_size(void) {
    const char *env = getenv("XS_JIT_CODE_SIZE_MB");
    if (env && *env) {
        char *end = NULL;
        long mb = strtol(env, &end, 10);
        if (end != env && mb > 0 && mb <= 1024) {
            return (size_t)mb * 1024 * 1024;
        }
    }
    return XS_JIT_CODE_SIZE;
}

XSJIT *jit_new(void) {
    XSJIT *j = xs_calloc(1, sizeof *j);
    j->available = 0;

#if JIT_ARCH_SUPPORTED && JIT_HAS_MMAP
    j->code_size = jit_pick_code_size();
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    /* macOS on Apple Silicon requires MAP_JIT for executable anonymous
     * mappings, and the toggling dance via pthread_jit_write_protect_np
     * around writes. On Linux / FreeBSD plain RWX anonymous works. */
#if defined(__APPLE__) && JIT_ARCH_ARM64 && defined(MAP_JIT)
    flags |= MAP_JIT;
#endif
    j->code = (uint8_t *)mmap(NULL, j->code_size, prot, flags, -1, 0);
    if (j->code == (uint8_t *)MAP_FAILED) {
        j->code = NULL;
        j->code_size = 0;
        fprintf(stderr, "xs jit: mmap failed, JIT unavailable (VM fallback)\n");
    } else {
        j->code_used = 0;
        j->available = 1;
    }
#elif JIT_ARCH_SUPPORTED && JIT_HAS_WIN
    j->code_size = jit_pick_code_size();
    j->code = (uint8_t *)VirtualAlloc(NULL, j->code_size,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!j->code) {
        j->code_size = 0;
        fprintf(stderr, "xs jit: VirtualAlloc failed, JIT unavailable\n");
    } else {
        j->code_used = 0;
        j->available = 1;
    }
#else
    j->code = NULL;
    j->code_size = 0;
#endif

    j->n_protos    = XS_JIT_MAX_PROTOS;
    j->compiled    = xs_calloc((size_t)j->n_protos, sizeof(void *));
    j->call_counts = xs_calloc((size_t)j->n_protos, sizeof(int));
    return j;
}

void jit_free(XSJIT *j) {
    if (!j) return;
#if JIT_HAS_MMAP
    if (j->code && j->code_size) munmap(j->code, j->code_size);
#elif JIT_HAS_WIN
    if (j->code && j->code_size) VirtualFree(j->code, 0, MEM_RELEASE);
#endif
    free(j->compiled);
    free(j->call_counts);
    free(j);
}

#if JIT_ARCH_SUPPORTED && (JIT_HAS_MMAP || JIT_HAS_WIN)

/* Drive any frames that an IR_CALL left on vm->frames down to the
 * baseline (the frame_count we stashed before the call). If the callee
 * has its own jit_entry we recurse into it; otherwise we step the
 * interpreter one opcode at a time. Shared by the x86-64 and arm64
 * codegens via a single extern declaration. */
int tier2_run_until(VM *vm, int target_fc) {
    int rc = 0;
    int saved_ss = vm->single_step;
    while (vm->frame_count > target_fc) {
        CallFrame *top = &vm->frames[vm->frame_count - 1];
        XSProto *proto = NULL;
        if (top->closure_val && VAL_TAG(top->closure_val) == XS_CLOSURE
            && top->closure_val->cl)
            proto = top->closure_val->cl->proto;
        if (proto && proto->jit_entry && top->ip == proto->chunk.code) {
            int (*fn)(VM *) = (int (*)(VM *))proto->jit_entry;
            rc = fn(vm);
        } else {
            vm->single_step = 1;
            rc = vm_step_jit(vm);
        }
        if (rc != 0) break;
    }
    vm->single_step = saved_ss;
    return rc;
}

/* Diagnostic counters, populated only when XS_JIT_STATS=1. The atexit
 * dumper below prints a per-reason histogram so we can see, for any
 * given workload, why protos fall back to the bytecode VM. */
static int g_stats_enabled    = -1;   /* -1 = uninitialised */
static int g_stats_compiled   = 0;
static int g_stats_codegen_failed = 0;
static int g_stats_lower_bail[8];     /* indexed by RALOW_BAIL_* */
static int g_stats_unsupported_op[256];

static const char *bail_kind_name(int kind) {
    switch (kind) {
        case RALOW_BAIL_NONE:                 return "none";
        case RALOW_BAIL_ACTOR_METHOD:         return "actor_method";
        case RALOW_BAIL_INNER_ACTOR:          return "inner_actor";
        case RALOW_BAIL_UNSUPPORTED_OP:       return "unsupported_op";
        case RALOW_BAIL_CALL_ARGC:            return "call_argc";
        case RALOW_BAIL_MAKE_LARGE:           return "make_large";
        case RALOW_BAIL_VSTACK_UNDERFLOW:     return "vstack_underflow";
        case RALOW_BAIL_CLOSURE_WRITTEN_LOCAL: return "closure_written_local";
        default:                              return "?";
    }
}

static void jit_stats_dump(void) {
    int total_bail = g_stats_codegen_failed;
    for (int k = 1; k < 8; k++) total_bail += g_stats_lower_bail[k];
    int total = g_stats_compiled + total_bail;
    if (total == 0) {
        fprintf(stderr, "xs jit stats: no protos reached the JIT\n");
        return;
    }
    fprintf(stderr, "xs jit stats:\n");
    fprintf(stderr, "  protos seen:       %d\n", total);
    fprintf(stderr, "  compiled:          %d (%.1f%%)\n",
            g_stats_compiled, 100.0 * g_stats_compiled / total);
    fprintf(stderr, "  bailed:            %d (%.1f%%)\n",
            total_bail, 100.0 * total_bail / total);
    for (int k = 1; k < 8; k++) {
        if (!g_stats_lower_bail[k]) continue;
        fprintf(stderr, "    %-22s %d\n",
                bail_kind_name(k), g_stats_lower_bail[k]);
    }
    if (g_stats_codegen_failed)
        fprintf(stderr, "    %-22s %d\n",
                "codegen_failed", g_stats_codegen_failed);
    if (g_stats_lower_bail[RALOW_BAIL_UNSUPPORTED_OP]) {
        fprintf(stderr, "  unsupported opcodes:\n");
        for (int o = 0; o < 256; o++) {
            if (!g_stats_unsupported_op[o]) continue;
            const char *nm = bytecode_op_name((Opcode)o);
            fprintf(stderr, "    %-22s %d\n",
                    nm ? nm : "?", g_stats_unsupported_op[o]);
        }
    }
}

static void jit_stats_init(void) {
    if (g_stats_enabled >= 0) return;
    const char *env = getenv("XS_JIT_STATS");
    g_stats_enabled = (env && *env && env[0] != '0') ? 1 : 0;
    if (g_stats_enabled) atexit(jit_stats_dump);
}

/* Run the tier-2 pipeline on one proto. NULL result = lowerer / codegen
 * declined (unsupported op, cross-block stack leak, captured written
 * local, code-buffer overflow). Caller leaves proto->jit_entry as NULL
 * and the runtime dispatcher routes through vm_step_jit. The jit_tried
 * flag memoises the NULL result so callers don't re-run the full
 * pipeline on every invocation of a proto we already know can't JIT. */
static void *tier2_compile_one(XSJIT *j, XSProto *proto) {
    if (!proto) return NULL;
    if (proto->jit_entry) return proto->jit_entry;
    if (proto->jit_tried)  return NULL;
    jit_stats_init();
    IRFunc *f = ralow_lower(proto);
    if (!f) {
        proto->jit_tried = 1;
        if (g_stats_enabled) {
            int kind = ralow_last_bail_kind();
            if (kind >= 0 && kind < 8) g_stats_lower_bail[kind]++;
            if (kind == RALOW_BAIL_UNSUPPORTED_OP) {
                int op = ralow_last_bail_op();
                if (op >= 0 && op < 256) g_stats_unsupported_op[op]++;
            }
        }
        return NULL;
    }
    ralow_liveness(f);
    IRAlloc *a = ralow_alloc(f);
    void *e = ralow_codegen(j, f, a);
    iralloc_free(a);
    irfunc_free(f);
    if (e) {
        proto->jit_entry = e;
        if (g_stats_enabled) g_stats_compiled++;
    } else {
        proto->jit_tried = 1;
        if (g_stats_enabled) g_stats_codegen_failed++;
    }
    return e;
}

/* Walk the whole proto->inner[] tree depth-first and tier-2-compile
 * each nested proto eligible. We always process inners even when the
 * outer proto itself couldn't be tier-2-compiled -- a function that
 * creates closures in its body (MAKE_CLOSURE captured locals guard
 * trips) still benefits from those inner closures having jit_entry
 * set when called later. */
static void tier2_compile_inner_tree(XSJIT *j, XSProto *proto) {
    if (!proto || proto->jit_entry) return;
    tier2_compile_one(j, proto);
    for (int i = 0; i < proto->n_inner; i++)
        tier2_compile_inner_tree(j, proto->inner[i]);
}

void *jit_compile(XSJIT *j, XSProto *proto) {
    if (!j || !j->available || !proto) return NULL;
    for (int i = 0; i < proto->n_inner; i++)
        tier2_compile_inner_tree(j, proto->inner[i]);
    return tier2_compile_one(j, proto);
}

#else  /* no supported JIT arch */

void *jit_compile(XSJIT *j, XSProto *proto) {
    (void)j; (void)proto;
    return NULL;
}

#endif

/* Threshold scales with proto size: tiny functions (<20 ops) reach the
 * JIT faster because compilation pays off quickly; big ones (>500 ops)
 * need a bit more confidence before paying the codegen cost. */
static int threshold_for(XSProto *proto) {
    if (!proto) return XS_JIT_THRESHOLD;
    int len = proto->chunk.len;
    if (len < 20)   return XS_JIT_THRESHOLD / 4;   /* 25 */
    if (len < 100)  return XS_JIT_THRESHOLD / 2;   /* 50 */
    if (len < 500)  return XS_JIT_THRESHOLD;       /* 100 */
    return XS_JIT_THRESHOLD * 2;                   /* 200 */
}

void *jit_maybe_compile(XSJIT *j, int proto_index, XSProto *proto) {
    if (!j || !j->available) return NULL;
    if (proto_index < 0 || proto_index >= j->n_protos) return NULL;
    if (j->compiled[proto_index]) return j->compiled[proto_index];
    if (proto && proto->jit_tried) return NULL;
    j->call_counts[proto_index]++;
    if (j->call_counts[proto_index] < threshold_for(proto)) return NULL;
    void *fn = jit_compile(j, proto);
    if (fn) j->compiled[proto_index] = fn;
    return fn;
}

JitFn jit_get_compiled(XSJIT *j, int proto_index) {
    if (!j || !j->available) return NULL;
    if (proto_index < 0 || proto_index >= j->n_protos) return NULL;
    return (JitFn)j->compiled[proto_index];
}

int jit_available(void) {
#if JIT_ARCH_SUPPORTED && (JIT_HAS_MMAP || JIT_HAS_WIN)
    return 1;
#else
    return 0;
#endif
}

static const char *jit_ir_op_name(IROp op) {
    switch (op) {
        case IR_CONST:        return "CONST";
        case IR_LOAD_LOCAL:   return "LOAD_LOCAL";
        case IR_LOAD_GLOBAL:  return "LOAD_GLOBAL";
        case IR_LOAD_UP:      return "LOAD_UP";
        case IR_PUSH_NULL:    return "PUSH_NULL";
        case IR_PUSH_TRUE:    return "PUSH_TRUE";
        case IR_PUSH_FALSE:   return "PUSH_FALSE";
        case IR_MAKE_CLOSURE: return "MAKE_CLOSURE";
        case IR_INDEX_GET:    return "INDEX_GET";
        case IR_INDEX_SET:    return "INDEX_SET";
        case IR_LOAD_FIELD:   return "LOAD_FIELD";
        case IR_STORE_FIELD:  return "STORE_FIELD";
        case IR_MAKE_RANGE:   return "MAKE_RANGE";
        case IR_MAKE_ARRAY:   return "MAKE_ARRAY";
        case IR_MAKE_TUPLE:   return "MAKE_TUPLE";
        case IR_MAKE_MAP:     return "MAKE_MAP";
        case IR_METHOD_CALL:  return "METHOD_CALL";
        case IR_CONCAT:       return "CONCAT";
        case IR_ITER_GET:     return "ITER_GET";
        case IR_VM_STEP:      return "VM_STEP";
        case IR_VM_STEP_CF:   return "VM_STEP_CF";
        case IR_VM_STEP_DRAIN:return "VM_STEP_DRAIN";
        case IR_STORE_LOCAL:  return "STORE_LOCAL";
        case IR_STORE_UP:     return "STORE_UP";
        case IR_POP:          return "POP";
        case IR_ADD:          return "ADD";
        case IR_SUB:          return "SUB";
        case IR_MUL:          return "MUL";
        case IR_DIV:          return "DIV";
        case IR_MOD:          return "MOD";
        case IR_BAND:         return "BAND";
        case IR_BOR:          return "BOR";
        case IR_BXOR:         return "BXOR";
        case IR_SHL:          return "SHL";
        case IR_SHR:          return "SHR";
        case IR_LT:           return "LT";
        case IR_GT:           return "GT";
        case IR_LE:           return "LE";
        case IR_GE:           return "GE";
        case IR_EQ:           return "EQ";
        case IR_NE:           return "NE";
        case IR_NEG:          return "NEG";
        case IR_NOT:          return "NOT";
        case IR_BNOT:         return "BNOT";
        case IR_JUMP:         return "JUMP";
        case IR_JIF_FALSE:    return "JIF_FALSE";
        case IR_JIF_TRUE:     return "JIF_TRUE";
        case IR_CMP_BR:       return "CMP_BR";
        case IR_NOP:          return "NOP";
        case IR_CALL:         return "CALL";
        case IR_RETURN:       return "RETURN";
        case IR_DUP:          return "DUP";
        case IR_MOVE:         return "MOVE";
        default:              return "?";
    }
}

void jit_dump_ir(XSProto *proto) {
    if (!proto) return;
    IRFunc *f = ralow_lower(proto);
    if (f) {
        printf("# proto %s  (%d vregs, %d blocks, %d insts)\n",
               proto->name ? proto->name : "<top>",
               f->n_vregs, f->n_blocks, f->n_insts);
        for (int b = 0; b < f->n_blocks; b++) {
            IRBlock *blk = &f->blocks[b];
            printf("  block %d:\n", b);
            for (int i = blk->start; i < blk->end; i++) {
                IRInst *in = &f->insts[i];
                printf("    %4d  %-15s", i, jit_ir_op_name(in->op));
                if (in->dst >= 0)  printf(" v%d =", in->dst);
                if (in->src1 >= 0) printf(" v%d", in->src1);
                if (in->src2 >= 0) printf(", v%d", in->src2);
                if (in->op == IR_CONST || in->op == IR_LOAD_LOCAL ||
                    in->op == IR_STORE_LOCAL || in->op == IR_LOAD_GLOBAL ||
                    in->op == IR_LOAD_UP || in->op == IR_STORE_UP ||
                    in->op == IR_JUMP || in->op == IR_JIF_FALSE ||
                    in->op == IR_JIF_TRUE || in->op == IR_LOAD_FIELD ||
                    in->op == IR_STORE_FIELD || in->op == IR_MAKE_RANGE ||
                    in->op == IR_MAKE_ARRAY || in->op == IR_MAKE_TUPLE ||
                    in->op == IR_MAKE_MAP)
                    printf(" #%d", in->imm);
                printf("\n");
            }
        }
        printf("\n");
        irfunc_free(f);
    } else {
        printf("# proto %s: lower bailed (kind=%d, op=%d)\n",
               proto->name ? proto->name : "<top>",
               ralow_last_bail_kind(), ralow_last_bail_op());
    }
    for (int i = 0; i < proto->n_inner; i++)
        jit_dump_ir(proto->inner[i]);
}

#endif /* XSC_ENABLE_JIT */
