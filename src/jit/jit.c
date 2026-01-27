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

XSJIT *jit_new(void) {
    XSJIT *j = xs_calloc(1, sizeof *j);
    j->available = 0;

#if JIT_ARCH_SUPPORTED && JIT_HAS_MMAP
    j->code_size = XS_JIT_CODE_SIZE;
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
    j->code_size = XS_JIT_CODE_SIZE;
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

/* Run the tier-2 pipeline on one proto. NULL result = lowerer / codegen
 * declined (unsupported op, cross-block stack leak, captured written
 * local, code-buffer overflow). Caller leaves proto->jit_entry as NULL
 * and the runtime dispatcher routes through vm_step_jit. */
static void *tier2_compile_one(XSJIT *j, XSProto *proto) {
    if (!proto) return NULL;
    if (proto->jit_entry) return proto->jit_entry;
    IRFunc *f = ralow_lower(proto);
    if (!f) return NULL;
    ralow_liveness(f);
    IRAlloc *a = ralow_alloc(f);
    void *e = ralow_codegen(j, f, a);
    iralloc_free(a);
    irfunc_free(f);
    if (e) proto->jit_entry = e;
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

void *jit_maybe_compile(XSJIT *j, int proto_index, XSProto *proto) {
    if (!j || !j->available) return NULL;
    if (proto_index < 0 || proto_index >= j->n_protos) return NULL;
    if (j->compiled[proto_index]) return j->compiled[proto_index];
    j->call_counts[proto_index]++;
    if (j->call_counts[proto_index] < XS_JIT_THRESHOLD) return NULL;
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

#endif /* XSC_ENABLE_JIT */
