/* Phase 2: liveness analysis.
 *
 * For each basic block compute:
 *   use[v]     = v is read before any def in the block
 *   def[v]     = v is written in the block (without first being read)
 *   live_in    = use ∪ (live_out - def)
 *   live_out   = union of successors' live_in
 *
 * Iterate to fixed point. Worklist over blocks; a block's live_out
 * re-enters the queue whenever a successor's live_in changes. */

#include "jit/ra_ir.h"
#include "core/xs.h"
#include <stdlib.h>
#include <string.h>

static inline int bs_words(int n_bits) { return (n_bits + 63) / 64; }

static uint64_t *bs_new(int n_bits) {
    int w = bs_words(n_bits);
    return xs_calloc((size_t)w, sizeof(uint64_t));
}

static inline void bs_set(uint64_t *bs, int bit) {
    bs[bit >> 6] |= ((uint64_t)1 << (bit & 63));
}
static inline int bs_test(const uint64_t *bs, int bit) {
    return (bs[bit >> 6] >> (bit & 63)) & 1;
}

/* dst |= src, returns 1 if dst changed. */
static int bs_or_into(uint64_t *dst, const uint64_t *src, int n_bits) {
    int w = bs_words(n_bits);
    int changed = 0;
    for (int i = 0; i < w; i++) {
        uint64_t prev = dst[i];
        dst[i] |= src[i];
        if (dst[i] != prev) changed = 1;
    }
    return changed;
}

/* dst = a ∪ (b - c) */
static void bs_live_in(uint64_t *dst, const uint64_t *use, const uint64_t *live_out,
                       const uint64_t *def, int n_bits) {
    int w = bs_words(n_bits);
    for (int i = 0; i < w; i++)
        dst[i] = use[i] | (live_out[i] & ~def[i]);
}

/* Returns 1 if two bitsets differ. */
static int bs_equal(const uint64_t *a, const uint64_t *b, int n_bits) {
    int w = bs_words(n_bits);
    for (int i = 0; i < w; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* For a single instruction, what vregs does it define, and what vregs
 * does it read? Fills `reads[]` and returns the number of reads. The
 * single def (if any) is in `*def_out` (set to -1 if no def). */
static int op_uses_call_args_as_vregs(IROp op) {
    return op == IR_CALL || op == IR_METHOD_CALL ||
           op == IR_INDEX_SET ||
           op == IR_MAKE_ARRAY || op == IR_MAKE_TUPLE || op == IR_MAKE_MAP ||
           op == IR_VM_STEP || op == IR_VM_STEP_CF;
}

static int ir_inst_reads(const IRInst *in, IRVReg reads[IR_MAX_CALL_ARGS + 2],
                         IRVReg *def_out) {
    int n = 0;
    *def_out = in->dst;
    if (in->src1 >= 0) reads[n++] = in->src1;
    if (in->src2 >= 0) reads[n++] = in->src2;
    if (op_uses_call_args_as_vregs(in->op)) {
        for (int i = 0; i < IR_MAX_CALL_ARGS; i++)
            if (in->call_args[i] >= 0) reads[n++] = in->call_args[i];
    }
    return n;
}

void ralow_liveness(IRFunc *f) {
    int nv = f->n_vregs;
    /* Compute use/def for each block by walking its instructions in
     * order. A vreg read before it's been locally defined counts in
     * `use`; a vreg defined (anywhere) counts in `def`. */
    for (int bi = 0; bi < f->n_blocks; bi++) {
        IRBlock *b = &f->blocks[bi];
        b->use = bs_new(nv);
        b->def = bs_new(nv);
        b->live_in  = bs_new(nv);
        b->live_out = bs_new(nv);
        for (int ii = b->start; ii < b->end; ii++) {
            IRInst *in = &f->insts[ii];
            IRVReg reads[IR_MAX_CALL_ARGS + 2];
            IRVReg def;
            int nr = ir_inst_reads(in, reads, &def);
            for (int k = 0; k < nr; k++) {
                IRVReg r = reads[k];
                if (!bs_test(b->def, r)) bs_set(b->use, r);
            }
            if (def >= 0) bs_set(b->def, def);
        }
    }
    /* Iterate to fixed point. Start each block's live_out as the
     * union of its successors' live_in (initially empty). Always
     * recompute live_in on each pass -- an earlier version only
     * refreshed it when live_out changed, which silently dropped the
     * USE set of any terminal block (e.g. the post-RETURN fall-through)
     * and left the rest of the CFG unaware that those vregs were live
     * upstream. The missing uses then collapsed live ranges and the
     * register allocator handed out the same physical register to
     * values that overlapped across a CALL, corrupting results. */
    uint64_t *tmp = bs_new(nv);
    uint64_t *new_in = bs_new(nv);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int bi = f->n_blocks - 1; bi >= 0; bi--) {
            IRBlock *b = &f->blocks[bi];
            memset(tmp, 0, (size_t)bs_words(nv) * sizeof(uint64_t));
            for (int s = 0; s < b->n_succ; s++) {
                IRBlock *sb = &f->blocks[b->succ[s]];
                bs_or_into(tmp, sb->live_in, nv);
            }
            memcpy(b->live_out, tmp, (size_t)bs_words(nv) * sizeof(uint64_t));
            bs_live_in(new_in, b->use, b->live_out, b->def, nv);
            if (!bs_equal(new_in, b->live_in, nv)) {
                memcpy(b->live_in, new_in,
                       (size_t)bs_words(nv) * sizeof(uint64_t));
                changed = 1;
            }
        }
    }
    free(new_in);
    free(tmp);
}
