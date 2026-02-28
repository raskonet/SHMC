#pragma once
/*
 * SHMC DSL Mutation Operators — shmc_mutate.h
 *
 * Symbolic mutations at the motif/section level.
 * Operates on ShmcWorld in-place. Each operator is:
 *   - Deterministic given the same RNG seed
 *   - Guaranteed to produce a valid ShmcWorld (no undefined state)
 *   - Formally verified by test_mutate.py (audio changes, structure valid)
 *
 * Mutation types (feedback-driven):
 *   MUTATE_NOTE_PITCH    — shift a random note ±1..±3 semitones
 *   MUTATE_NOTE_VEL      — nudge a random note velocity ±1 step
 *   MUTATE_NOTE_DUR      — change a random note duration ±1 step
 *   MUTATE_TRANSPOSE     — change a MotifUse transpose ±1..±5 semitones
 *   MUTATE_VEL_SCALE     — nudge a MotifUse vel_scale by ±0.1
 *   MUTATE_BEAT_OFFSET   — shift a MotifUse start_beat ±1 beat
 *   MUTATE_PATCH         — perturb a filter cutoff or ADSR param ±1 step
 *   MUTATE_ANY           — choose uniformly from all above (0-6)
 *
 * Structural motif mutations (operate on a whole motif's VoiceProgram):
 *   MUTATE_MOTIF_INVERT      — mirror intervals around first pitch (melodic inversion)
 *   MUTATE_MOTIF_RETROGRADE  — reverse note order (retrograde)
 *   MUTATE_MOTIF_AUGMENT     — increase all note durations by +1 step
 *   MUTATE_MOTIF_DIMINISH    — decrease all note durations by -1 step
 *   MUTATE_MOTIF_ADD_NOTE    — insert a new note (random pitch/dur/vel) at random position
 *   MUTATE_MOTIF_DEL_NOTE    — delete one random note (only if motif has ≥ 2 notes)
 *   MUTATE_STRUCTURAL        — choose uniformly from the 6 structural ops above
 *
 * These connect to the layer5 beam search: the search loop can call
 * shmc_mutate() instead of patch_mutate(), enabling search over full songs.
 */
#include "shmc_dsl.h"
#include <stdint.h>

typedef enum {
    MUTATE_NOTE_PITCH  = 0,
    MUTATE_NOTE_VEL    = 1,
    MUTATE_NOTE_DUR    = 2,
    MUTATE_TRANSPOSE   = 3,
    MUTATE_VEL_SCALE   = 4,
    MUTATE_BEAT_OFFSET = 5,
    MUTATE_PATCH       = 6,
    MUTATE_ANY         = 7,   /* random from 0-6 (parameter mutations) */
    /* structural motif mutations */
    MUTATE_MOTIF_INVERT     = 8,
    MUTATE_MOTIF_RETROGRADE = 9,
    MUTATE_MOTIF_AUGMENT    = 10,
    MUTATE_MOTIF_DIMINISH   = 11,
    MUTATE_MOTIF_ADD_NOTE   = 12,
    MUTATE_MOTIF_DEL_NOTE   = 13,
    MUTATE_STRUCTURAL       = 14,  /* random from 8-13 */
    /* harmonic-space mutations */
    MUTATE_HARM_CIRCLE_5TH  = 15,  /* transpose one motif by +7 semitones (circle of fifths) */
    MUTATE_HARM_CHORD_SUB   = 16,  /* substitute relative major/minor (±3 semitones) */
    MUTATE_HARM_SEC_DOM     = 17,  /* secondary dominant: transpose one voice +7 for one bar */
    MUTATE_HARMONIC         = 18   /* random from 15-17 */
} MutateType;

/*
 * Apply one mutation to world in-place.
 * rng: xorshift32 state (updated in place)
 * Returns:
 *   1  = mutation applied (world changed)
 *   0  = no viable mutation target found (world unchanged)
 *  -1  = error (world unchanged)
 */
int shmc_mutate(ShmcWorld *w, MutateType type, uint32_t *rng);

/*
 * Deep-copy a ShmcWorld (for the search rollback path).
 * dst must be a fresh zeroed ShmcWorld (shmc_world_free not yet called).
 * Does NOT copy lib — caller must call motif_lib_init + re-register motifs.
 */
int shmc_world_clone(const ShmcWorld *src, ShmcWorld *dst);

/* xorshift32 — same as layer5 */
static inline uint32_t mutate_rng(uint32_t *s){
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s;
}
static inline int mutate_range(uint32_t *s, int lo, int hi){
    if(hi<=lo) return lo;
    return lo + (int)(mutate_rng(s) % (uint32_t)(hi - lo + 1));
}
