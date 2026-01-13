/* Phase 3: linear-scan register allocation.
 *
 * Walk the IR in a fixed instruction order. For each vreg compute a
 * live interval [first_use_or_def, last_use]. Sort intervals by start
 * and assign physical registers in order, evicting (spilling) to the
 * native stack when we run out.
 *
 * Register pool: rbx, r14, r15 (callee-saved -- survive C calls
 * without save/restore). Everything not in a register is kept in a
 * spill slot on the JIT's native stack frame. Three registers is
 * enough to keep both operands of a binop plus one rolling tmp, which
 * matches the common hot paths we care about (ADD/LT/STORE sequences
 * in loops). The VM pointer lives in r12 and the current frame in r13,
 * leaving rax/rdi/rsi/rdx/rcx as scratch for arg passing. */

#include "jit/ra_ir.h"
#include "core/xs.h"
#include <stdlib.h>
#include <string.h>

/* Physical register indices used by the code generator. Order matters:
 * codegen translates these numerically. */
enum {
    RA_PHYS_RBX = 0,
    RA_PHYS_R14 = 1,
    RA_PHYS_R15 = 2,
    RA_N_PHYS   = 3,
};

typedef struct {
    IRVReg vreg;
    int    start, end;   /* IR instruction indices */
} LiveRange;

/* For each instruction, decide what vregs it reads and defines. */
static int ra_op_uses_call_args(IROp op) {
    return op == IR_CALL || op == IR_METHOD_CALL ||
           op == IR_INDEX_SET ||
           op == IR_MAKE_ARRAY || op == IR_MAKE_TUPLE || op == IR_MAKE_MAP ||
           op == IR_VM_STEP || op == IR_VM_STEP_CF;
}

static int inst_uses(const IRInst *in, IRVReg out[IR_MAX_CALL_ARGS + 2]) {
    int n = 0;
    if (in->src1 >= 0) out[n++] = in->src1;
    if (in->src2 >= 0) out[n++] = in->src2;
    if (ra_op_uses_call_args(in->op)) {
        for (int i = 0; i < IR_MAX_CALL_ARGS; i++)
            if (in->call_args[i] >= 0) out[n++] = in->call_args[i];
    }
    return n;
}

/* Build live intervals. For a vreg, start = first index where it's
 * defined (or live-in to the containing block); end = last index
 * where it's used (or live-out of the containing block). */
static void build_intervals(IRFunc *f, LiveRange *ranges) {
    for (int v = 0; v < f->n_vregs; v++) {
        ranges[v].vreg = (IRVReg)v;
        ranges[v].start = INT32_MAX;
        ranges[v].end = -1;
    }

    /* For each block, for each live-in vreg, extend interval to block
     * start. For each use/def, update endpoints. For each live-out,
     * extend to block end - 1. */
    for (int bi = 0; bi < f->n_blocks; bi++) {
        IRBlock *b = &f->blocks[bi];
        int nv = f->n_vregs;
        for (int v = 0; v < nv; v++) {
            if ((b->live_in[v >> 6] >> (v & 63)) & 1) {
                if (ranges[v].start > b->start) ranges[v].start = b->start;
                if (ranges[v].end < b->start)   ranges[v].end   = b->start;
            }
            if ((b->live_out[v >> 6] >> (v & 63)) & 1) {
                int last = b->end - 1;
                if (ranges[v].start > last) ranges[v].start = last;
                if (ranges[v].end < last)   ranges[v].end   = last;
            }
        }
        for (int ii = b->start; ii < b->end; ii++) {
            IRInst *in = &f->insts[ii];
            IRVReg uses[IR_MAX_CALL_ARGS + 2];
            int nu = inst_uses(in, uses);
            for (int k = 0; k < nu; k++) {
                IRVReg u = uses[k];
                if (ranges[u].start > ii) ranges[u].start = ii;
                if (ranges[u].end < ii)   ranges[u].end   = ii;
            }
            if (in->dst >= 0) {
                IRVReg d = in->dst;
                if (ranges[d].start > ii) ranges[d].start = ii;
                if (ranges[d].end < ii)   ranges[d].end   = ii;
            }
        }
    }
}

static int cmp_by_start(const void *a, const void *b) {
    const LiveRange *la = a, *lb = b;
    /* vregs that never appeared (start=INT32_MAX) come last. */
    if (la->start != lb->start) return la->start - lb->start;
    return la->vreg - lb->vreg;
}

IRAlloc *ralow_alloc(IRFunc *f) {
    IRAlloc *a = xs_malloc(sizeof *a);
    a->reg   = xs_malloc((size_t)f->n_vregs * sizeof(int8_t));
    a->spill = xs_malloc((size_t)f->n_vregs * sizeof(int8_t));
    for (int i = 0; i < f->n_vregs; i++) { a->reg[i] = -1; a->spill[i] = -1; }
    a->n_spill_slots = 0;

    LiveRange *ranges = xs_malloc((size_t)f->n_vregs * sizeof(LiveRange));
    build_intervals(f, ranges);

    /* Locals are loaded once in the prologue and only released at the
     * epilogue / error exit, so force their ranges to span the whole
     * function. This also gives them the earliest start (0), so the
     * sort below places them at the front of the queue -- linear scan
     * prefers them for physical regs over the short-lived IR temps. */
    for (int i = 0; i < f->n_locals; i++) {
        IRVReg lv = f->local_vregs[i];
        if (lv < 0 || lv >= f->n_vregs) continue;
        ranges[lv].start = 0;
        ranges[lv].end   = f->n_insts > 0 ? f->n_insts - 1 : 0;
    }

    /* Filter out vregs with no live range (never defined/used) -- they
     * shouldn't normally exist but guard anyway. */
    LiveRange *sorted = xs_malloc((size_t)f->n_vregs * sizeof(LiveRange));
    int n_sorted = 0;
    for (int v = 0; v < f->n_vregs; v++) {
        if (ranges[v].start != INT32_MAX && ranges[v].end >= ranges[v].start)
            sorted[n_sorted++] = ranges[v];
    }
    qsort(sorted, n_sorted, sizeof(LiveRange), cmp_by_start);

    /* Active list: intervals currently holding a register, sorted by
     * end ascending so we can quickly find expired ones. For N_PHYS=2
     * we just use a flat array of size RA_N_PHYS. */
    LiveRange active[RA_N_PHYS];
    int8_t active_phys[RA_N_PHYS];
    int n_active = 0;
    int free_regs[RA_N_PHYS];
    int n_free = RA_N_PHYS;
    for (int i = 0; i < RA_N_PHYS; i++) free_regs[i] = i;

    for (int i = 0; i < n_sorted; i++) {
        LiveRange cur = sorted[i];
        /* expire active intervals that end before cur.start */
        int w = 0;
        for (int k = 0; k < n_active; k++) {
            if (active[k].end < cur.start) {
                free_regs[n_free++] = active_phys[k];
            } else {
                active[w] = active[k];
                active_phys[w] = active_phys[k];
                w++;
            }
        }
        n_active = w;

        if (n_free > 0) {
            int phys = free_regs[--n_free];
            a->reg[cur.vreg] = (int8_t)phys;
            active[n_active] = cur;
            active_phys[n_active] = (int8_t)phys;
            n_active++;
        } else {
            /* Spill: pick the active interval with the latest end; if
             * cur ends later than all, spill cur itself. */
            int worst = 0;
            for (int k = 1; k < n_active; k++)
                if (active[k].end > active[worst].end) worst = k;
            if (active[worst].end > cur.end) {
                /* evict active[worst]: move to spill, give its reg to cur */
                IRVReg evicted = active[worst].vreg;
                int phys = active_phys[worst];
                a->reg[evicted] = -1;
                a->spill[evicted] = (int8_t)a->n_spill_slots++;
                a->reg[cur.vreg] = (int8_t)phys;
                active[worst] = cur;
                active_phys[worst] = (int8_t)phys;
            } else {
                a->spill[cur.vreg] = (int8_t)a->n_spill_slots++;
            }
        }
    }

    free(sorted);
    free(ranges);
    return a;
}

void iralloc_free(IRAlloc *a) {
    if (!a) return;
    free(a->reg);
    free(a->spill);
    free(a);
}
