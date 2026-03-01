/*
 * shmc_patch_mutate.c — Structural patch mutation   v1
 *
 * Adds/substitutes/removes DSP operators in a PatchProgram.
 * These are "graph rewrite" mutations — they change the patch structure,
 * not just parameter values. Enables new timbres (FM, distortion, etc.)
 * that parameter-only mutation can never reach.
 *
 * Safety rules (enforced):
 *   1. Max ops: DSL_LIMIT_MAX_PATCH_OPS (from shmc_dsl_limits.h)
 *   2. Min ops: 3 (osc + envelope + out) — always keep something audible
 *   3. OUT instruction is never removed or substituted
 *   4. Substitutions preserve signal flow: audio→audio, control→control
 *   5. After every mutation, patch_canonicalize() is called
 *
 * Mutation types:
 *   PATCH_STRUCT_SUBSTITUTE  — swap a filter op (lpf↔hpf↔bpf) or osc type (saw↔tri↔square)
 *   PATCH_STRUCT_INSERT_DIST — insert tanh/clip/fold before OUT (distortion chain)
 *   PATCH_STRUCT_INSERT_FILT — insert a new filter stage before OUT
 *   PATCH_STRUCT_REMOVE      — remove a non-critical op (filter, distortion)
 *   PATCH_STRUCT_ANY         — choose randomly
 *
 * Verified: verify_patch_struct.c N/N PASSED
 */
#include "../include/shmc_patch_mutate.h"
#include "../../layer0/include/opcodes.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer0b/include/shmc_hash.h"
#include "../../layer0b/include/patch_meta.h"
#include "../include/shmc_dsl_limits.h"
#include <string.h>
#include <stdlib.h>

/* ── RNG helper ─────────────────────────────────────────────────── */
static uint32_t rng_next(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s;
}

/* ── Op type classification ─────────────────────────────────────── */
static int is_filter(uint8_t op) {
    return op == OP_LPF || op == OP_HPF || op == OP_BPF;
}
static int is_osc(uint8_t op) {
    return op == OP_OSC || op == OP_SAW || op == OP_SQUARE || op == OP_TRI;
}
static int is_distortion(uint8_t op) {
    return op == OP_TANH || op == OP_CLIP || op == OP_FOLD;
}
static int is_removable(uint8_t op) {
    return is_filter(op) || is_distortion(op);
}

/* ── SUBSTITUTE: swap op type preserving signal flow ────────────── */
static int mutate_substitute(PatchProgram *pp, uint32_t *rng) {
    /* Collect candidate positions */
    int cands[64]; int nc = 0;
    for (int i = 0; i < pp->n_instrs && nc < 64; i++) {
        uint8_t op = INSTR_OP(pp->code[i]);
        if (is_filter(op) || is_osc(op) || is_distortion(op)) cands[nc++] = i;
    }
    if (nc == 0) return 0;
    int idx = cands[rng_next(rng) % nc];
    uint8_t op = INSTR_OP(pp->code[idx]);

    uint8_t new_op;
    if (is_filter(op)) {
        /* Cycle: lpf→hpf→bpf→lpf */
        if (op == OP_LPF) new_op = OP_HPF;
        else if (op == OP_HPF) new_op = OP_BPF;
        else new_op = OP_LPF;
    } else if (is_osc(op)) {
        /* Cycle: saw→tri→square→osc→saw */
        if (op == OP_SAW)    new_op = OP_TRI;
        else if (op == OP_TRI) new_op = OP_SQUARE;
        else if (op == OP_SQUARE) new_op = OP_OSC;
        else new_op = OP_SAW;
    } else {
        /* Distortion: tanh→clip→fold→tanh */
        if (op == OP_TANH) new_op = OP_CLIP;
        else if (op == OP_CLIP) new_op = OP_FOLD;
        else new_op = OP_TANH;
    }
    if (new_op == op) return 0;

    /* Patch instruction in place — only change the opcode byte (bits 56-63) */
    uint64_t instr = pp->code[idx];
    instr = (instr & 0x00FFFFFFFFFFFFFFULL) | ((uint64_t)new_op << 56);
    pp->code[idx] = instr;
    patch_canonicalize(pp);
    return 1;
}

/* ── INSERT_DIST: add tanh/clip/fold before OUT ─────────────────── */
static int mutate_insert_dist(PatchProgram *pp, uint32_t *rng) {
    if (pp->n_instrs >= DSL_LIMIT_MAX_PATCH_OPS) return 0;
    if (pp->n_regs >= MAX_REGS - 2) return 0;

    /* Find OUT instruction */
    int out_idx = -1;
    for (int i = pp->n_instrs - 1; i >= 0; i--)
        if (INSTR_OP(pp->code[i]) == OP_OUT) { out_idx = i; break; }
    if (out_idx < 0) return 0;

    /* What register does OUT read from? */
    uint8_t src = INSTR_SRC_A(pp->code[out_idx]);

    /* New register for distorted signal */
    uint8_t new_reg = (uint8_t)pp->n_regs;
    pp->n_regs++;

    /* Choose distortion type */
    static const uint8_t dist_ops[] = { OP_TANH, OP_CLIP, OP_FOLD };
    uint8_t dist_op = dist_ops[rng_next(rng) % 3];

    /* Insert distortion instruction before OUT */
    /* Shift instructions from out_idx onwards right by 1 */
    if (pp->n_instrs >= MAX_INSTRS) return 0;
    for (int i = pp->n_instrs; i > out_idx; i--)
        pp->code[i] = pp->code[i-1];
    pp->n_instrs++;

    /* dist_new_reg = dist_op(src) */
    pp->code[out_idx] = INSTR_PACK(dist_op, new_reg, src, 0, 0, 0);

    /* Update OUT to read from new_reg */
    pp->code[out_idx + 1] = INSTR_PACK(OP_OUT, 0, new_reg, 0, 0, 0);

    patch_canonicalize(pp);
    return 1;
}

/* ── INSERT_FILT: add a new filter before OUT ───────────────────── */
static int mutate_insert_filt(PatchProgram *pp, uint32_t *rng) {
    if (pp->n_instrs >= DSL_LIMIT_MAX_PATCH_OPS) return 0;
    if (pp->n_regs >= MAX_REGS - 2) return 0;

    int out_idx = -1;
    for (int i = pp->n_instrs - 1; i >= 0; i--)
        if (INSTR_OP(pp->code[i]) == OP_OUT) { out_idx = i; break; }
    if (out_idx < 0) return 0;

    uint8_t src = INSTR_SRC_A(pp->code[out_idx]);
    uint8_t new_reg = (uint8_t)pp->n_regs;
    pp->n_regs++;

    static const uint8_t filt_ops[] = { OP_LPF, OP_HPF, OP_BPF };
    uint8_t filt_op = filt_ops[rng_next(rng) % 3];
    /* Cutoff index: middle range (12-24) for musical results */
    uint16_t ci = (uint16_t)(12 + (rng_next(rng) % 13));

    if (pp->n_instrs >= MAX_INSTRS) return 0;
    for (int i = pp->n_instrs; i > out_idx; i--)
        pp->code[i] = pp->code[i-1];
    pp->n_instrs++;

    pp->code[out_idx] = INSTR_PACK(filt_op, new_reg, src, 0, ci, 0);
    pp->code[out_idx + 1] = INSTR_PACK(OP_OUT, 0, new_reg, 0, 0, 0);

    patch_canonicalize(pp);
    return 1;
}

/* ── REMOVE: delete a non-critical filter/dist, rewire around it ── */
static int mutate_remove(PatchProgram *pp, uint32_t *rng) {
    if (pp->n_instrs <= 3) return 0; /* keep minimum structure */

    /* Find removable ops */
    int cands[64]; int nc = 0;
    for (int i = 0; i < pp->n_instrs - 1 && nc < 64; i++) {
        uint8_t op = INSTR_OP(pp->code[i]);
        if (!is_removable(op)) continue;
        /* Check: is its output used exactly once (the next instr that reads it)? */
        uint8_t dst = INSTR_DST(pp->code[i]);
        int use_count = 0;
        for (int j = i+1; j < pp->n_instrs; j++) {
            if (INSTR_SRC_A(pp->code[j]) == dst) use_count++;
            if (INSTR_SRC_B(pp->code[j]) == dst) use_count++;
        }
        if (use_count == 1) cands[nc++] = i; /* safe to remove */
    }
    if (nc == 0) return 0;
    int idx = cands[rng_next(rng) % nc];

    uint8_t rm_dst = INSTR_DST(pp->code[idx]);
    uint8_t rm_src = INSTR_SRC_A(pp->code[idx]); /* bypass input */

    /* Replace all uses of rm_dst with rm_src */
    for (int i = idx+1; i < pp->n_instrs; i++) {
        uint64_t instr = pp->code[i];
        uint8_t a = INSTR_SRC_A(instr), b = INSTR_SRC_B(instr);
        if (a == rm_dst) instr = (instr & ~(0xFFULL<<40)) | ((uint64_t)rm_src<<40);
        if (b == rm_dst) instr = (instr & ~(0xFFULL<<32)) | ((uint64_t)rm_src<<32);
        pp->code[i] = instr;
    }

    /* Shift instructions left to close gap */
    for (int i = idx; i < pp->n_instrs - 1; i++) pp->code[i] = pp->code[i+1];
    pp->n_instrs--;

    patch_canonicalize(pp);
    return 1;
}

/* ── Public API ─────────────────────────────────────────────────── */

int shmc_patch_struct_mutate(PatchProgram *pp, PatchStructMutType type, uint32_t *rng) {
    if (!pp || !rng) return 0;

    if (type == PATCH_STRUCT_ANY) {
        /* Weighted: substitute most common (safe), insert less common, remove least */
        uint32_t r = rng_next(rng) % 10;
        if (r < 4) type = PATCH_STRUCT_SUBSTITUTE;
        else if (r < 6) type = PATCH_STRUCT_INSERT_DIST;
        else if (r < 8) type = PATCH_STRUCT_INSERT_FILT;
        else             type = PATCH_STRUCT_REMOVE;
    }

    switch (type) {
        case PATCH_STRUCT_SUBSTITUTE:  return mutate_substitute(pp, rng);
        case PATCH_STRUCT_INSERT_DIST: return mutate_insert_dist(pp, rng);
        case PATCH_STRUCT_INSERT_FILT: return mutate_insert_filt(pp, rng);
        case PATCH_STRUCT_REMOVE:      return mutate_remove(pp, rng);
        default: return 0;
    }
}

/* Apply structural mutation to a random patch in world */
int shmc_world_patch_struct_mutate(ShmcWorld *w, uint32_t *rng) {
    if (!w || w->n_patches == 0) return 0;
    int pi = (int)(rng_next(rng) % (uint32_t)w->n_patches);
    return shmc_patch_struct_mutate(&w->patches[pi], PATCH_STRUCT_ANY, rng);
}
