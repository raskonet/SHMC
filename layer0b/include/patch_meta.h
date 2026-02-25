#pragma once
/*
 * SHMC Layer 0b — patch_meta.h
 *
 * Canonical hashing and cost metadata for PatchProgram.
 * Required for MCTS transposition tables and search pruning.
 *
 * Fixes:
 *   R1-1: state_offset[] stored in PatchProgram at build time,
 *         not recomputed as i*4 at runtime. Mutation-safe.
 *   R1-2: hash_patch() — FNV-1a over packed instruction bytes.
 *   R1-4: PatchCost — instruction count, stateful op count, reg depth.
 *   R1-3: patch_program_valid() — enforces hard grammar bounds.
 */

#include "../../layer0/include/patch_builder.h"
#include <stdint.h>

/* Maximum number of state slots any one instruction can use */
#define MAX_STATE_PER_INSTR 4

/*
 * Extended PatchProgram that carries pre-allocated state offsets.
 *
 * state_offset[i] = index into PatchState.state[] for instruction i.
 * Assigned at build time by patch_assign_state_offsets().
 * Mutation-safe: swap instructions → state slots follow them.
 *
 * We overlay this on top of PatchProgram by making it a superset.
 * Unchanged instructions from PatchProgram are preserved.
 */
typedef struct {
    /* --- base PatchProgram fields (must be first, same layout) --- */
    Instr code[MAX_INSTRS];
    int   n_instrs;
    int   n_regs;
    int   n_state;        /* total state slots used (sum of per-instr needs) */
    /* --- extended fields --- */
    uint16_t state_offset[MAX_INSTRS]; /* state_offset[i]: start slot for instr i */
    uint64_t hash;                     /* FNV-1a over code[0..n_instrs-1] */
} PatchProgramEx;

/* Cost model for search pruning */
typedef struct {
    int n_instrs;         /* total instruction count */
    int n_stateful;       /* instructions that use state slots */
    int n_state_slots;    /* total state slots consumed */
    int max_reg;          /* highest register index used */
    int est_cpu;          /* rough CPU cost units (stateful=2, rest=1) */
} PatchCost;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Assign state offsets to each instruction in prog.
 * Sets prog->state_offset[i] and prog->n_state.
 * Must be called once after building a PatchProgram.
 * Returns total state slots used.
 */
int patch_assign_state_offsets(PatchProgramEx *prog);

/*
 * Compute FNV-1a hash over all instructions.
 * Two PatchPrograms with identical code[] produce identical hashes.
 */
uint64_t hash_patch(const PatchProgramEx *prog);
uint64_t hash_patch_raw(const PatchProgram *prog);  /* works on plain PatchProgram */

/*
 * Compute cost metadata for a PatchProgram.
 */
void patch_cost(const PatchProgram *prog, PatchCost *out);

/*
 * Validate that a PatchProgram satisfies hard grammar bounds.
 * Returns 0 if valid, -1 with reason in err.
 * Limits:
 *   max_instrs:   program length
 *   max_stateful: number of instructions that consume state slots
 *   max_regs:     register count
 */
int patch_program_valid(const PatchProgram *prog,
                        int max_instrs, int max_stateful, int max_regs,
                        char *err, int err_sz);

/*
 * Convert a PatchProgram → PatchProgramEx (assigns offsets + hash).
 */
void patch_to_ex(const PatchProgram *src, PatchProgramEx *dst);

/*
 * Convert PatchProgramEx → PatchProgram (drops extended fields).
 */
static inline PatchProgram patch_from_ex(const PatchProgramEx *ex){
    PatchProgram p;
    for(int i = 0; i < ex->n_instrs; i++) p.code[i] = ex->code[i];
    p.n_instrs = ex->n_instrs;
    p.n_regs   = ex->n_regs;
    p.n_state  = ex->n_state;
    return p;
}

/*
 * Execute one sample using state_offset[] instead of i*4.
 * Drop-in replacement for the inner loop in patch_interp.c.
 * This is what the renderer uses after R1-1 is adopted.
 */
float exec1_ex(PatchState *ps, const PatchProgramEx *prog);

#ifdef __cplusplus
}
#endif

/* ================================================================
   T4-6: Evaluative patch metadata
   Flags that guide search pruning — not just descriptive counts.
   ================================================================ */
typedef struct {
    /* Structural flags */
    int has_oscillator;    /* at least one: OSC/SAW/SQR/TRI/FM/PM/NOISE */
    int has_envelope;      /* at least one: ADSR/RAMP/EXP_DECAY */
    int has_filter;        /* at least one: LPF/HPF/BPF/ONEPOLE */
    int has_feedback;      /* OP_FM with modulator from a later register */
    int has_noise_source;  /* NOISE / LP_NOISE / RAND_STEP */

    /* Stability estimate (conservative) */
    int is_stable;         /* 0 = may diverge (e.g. high-Q BPF, FM depth) */

    /* Timbral fingerprint */
    int n_oscillators;     /* total osc-family instructions */
    int n_envelopes;       /* total envelope-family instructions */
    int n_filters;         /* total filter-family instructions */

    /* Inherited from PatchCost */
    int n_instrs;
    int n_stateful;
    int n_state_slots;
    int est_cpu;
} PatchMeta;

/*
 * Compute evaluative metadata for a PatchProgram.
 * Stability is conservative: any FM depth >= 0.8 or BPF q >= 0.9 → is_stable=0.
 */
void patch_meta(const PatchProgram *prog, PatchMeta *out);

/* Quick predicates useful in search pruning */
static inline int pmeta_search_viable(const PatchMeta *m){
    return m->has_oscillator && m->has_envelope && m->is_stable;
}
