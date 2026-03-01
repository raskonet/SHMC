#pragma once
/*
 * shmc_mut_algebra.h — Invertible mutation algebra   v1
 *
 * Each mutation is a record: (type, target_path, before_value, after_value).
 * Applying m⁻¹ exactly reverses m: apply(apply(P, m), m⁻¹) = P.
 *
 * This enables:
 *   - Beam search rollback (backtrack from dead ends without full world copy)
 *   - Mutation history logging (explain what evolved)
 *   - LLM diff interface (show mutations as text)
 *   - MCTS tree search (state = base_world + mutation_path)
 *
 * Target path encoding:
 *   MUT_TARGET_NOTE:    motif_idx, note_idx, field (PITCH/VEL/DUR)
 *   MUT_TARGET_USE:     section_idx, track_idx, use_idx, field (TRANSPOSE/VEL_SCALE/BEAT)
 *   MUT_TARGET_PATCH:   patch_idx, instr_idx, field (CUTOFF/ADSR_*)
 *   MUT_TARGET_PSTRUCT: patch_idx, struct_op (SUBSTITUTE/INSERT_DIST/INSERT_FILT/REMOVE)
 *                       — stores full before/after PatchProgram snapshot
 *
 * Usage:
 *   MutationRecord rec;
 *   int ok = shmc_mutate_tracked(world, type, rng, &rec);
 *   // ... evaluate fitness ...
 *   if (fitness < threshold)
 *       shmc_mutate_undo(world, &rec);  // exact rollback
 *
 * MutationLog tracks a sequence of mutations for a world:
 *   shmc_mut_log_push(&log, &rec);
 *   shmc_mut_log_undo_last(&log, world);
 *   shmc_mut_log_to_str(&log, buf, cap);  // human-readable diff
 */
#include "shmc_dsl.h"
#include "shmc_mutate.h"
#include "shmc_patch_mutate.h"
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Target path ────────────────────────────────────────────────── */

typedef enum {
    MUT_TARGET_NOTE    = 0,   /* motif note field */
    MUT_TARGET_USE     = 1,   /* section motif-use field */
    MUT_TARGET_PATCH   = 2,   /* patch parameter field */
    MUT_TARGET_PSTRUCT = 3,   /* patch structural change (full snapshot) */
    MUT_TARGET_MSTRUCT = 4,   /* motif structural change (full VoiceProgram snapshot) */
    MUT_TARGET_NONE    = 255  /* no-op / invalid */
} MutTargetKind;

typedef enum {
    MUT_FIELD_PITCH      = 0,
    MUT_FIELD_VEL        = 1,
    MUT_FIELD_DUR        = 2,
    MUT_FIELD_TRANSPOSE  = 3,
    MUT_FIELD_VEL_SCALE  = 4,
    MUT_FIELD_BEAT       = 5,
    MUT_FIELD_CUTOFF     = 6,
    MUT_FIELD_ADSR_ATT   = 7,
    MUT_FIELD_ADSR_DEC   = 8,
    MUT_FIELD_ADSR_SUS   = 9,
    MUT_FIELD_ADSR_REL   = 10,
    /* structural motif snapshot fields */
    MUT_FIELD_MOTIF_INVERT     = 11,
    MUT_FIELD_MOTIF_RETROGRADE = 12,
    MUT_FIELD_MOTIF_AUGMENT    = 13,
    MUT_FIELD_MOTIF_DIMINISH   = 14,
    MUT_FIELD_MOTIF_ADD_NOTE   = 15,
    MUT_FIELD_MOTIF_DEL_NOTE   = 16
} MutField;

/* Before/after value union */
typedef union {
    int   i;     /* pitch, vel, dur, transpose, adsr param (integer field) */
    float f;     /* vel_scale, beat_offset (float field) */
} MutValue;

/* ── MutationRecord ─────────────────────────────────────────────── */
/*
 * Memory design: keep records small.
 * - Parameter mutations (NOTE/USE/PATCH): store field coordinates + before/after
 *   values. Total: ~60 bytes per record.
 * - Structural mutations (PSTRUCT): store patch_idx + before/after n_instrs,
 *   plus a heap-allocated snapshot of the full PatchProgram.
 *   The heap pointer is only valid while the record lives; caller owns it.
 *   Freed by mut_record_free().
 *
 * This keeps sizeof(MutationRecord) ≈ 80 bytes instead of 16 KB.
 * MutationLog(64 entries) ≈ 5 KB instead of 512 KB.
 */
typedef struct {
    MutTargetKind target_kind;
    MutField      field;

    /* Target coordinates (all types share this block) */
    int16_t motif_idx;    /* MUT_TARGET_NOTE */
    int16_t note_idx;
    int16_t section_idx;  /* MUT_TARGET_USE */
    int16_t track_idx;
    int16_t use_idx;
    int16_t patch_idx;    /* MUT_TARGET_PATCH / PSTRUCT */
    int16_t instr_idx;    /* MUT_TARGET_PATCH only */
    int16_t _pad;

    /* Before/after values for param mutations */
    MutValue before;
    MutValue after;

    /* Structural patch snapshot: heap-allocated, NULL for non-PSTRUCT.
     * Points to a malloc'd PatchProgram copy of the state before mutation.
     * Freed by mut_record_free(). */
    PatchProgram *snap_before;

    /* Structural motif snapshot: heap-allocated, NULL for non-MSTRUCT.
     * Points to a malloc'd VoiceProgram copy of the state before mutation.
     * Freed by mut_record_free(). */
    VoiceProgram *snap_vp;

    /* Human-readable description */
    char desc[64];
} MutationRecord;

/* Free heap resources in a record (only needed for PSTRUCT records). */
static inline void mut_record_free(MutationRecord *rec) {
    if (rec && rec->snap_before) {
        free(rec->snap_before);
        rec->snap_before = NULL;
    }
    if (rec && rec->snap_vp) {
        free(rec->snap_vp);
        rec->snap_vp = NULL;
    }
}

/* ── MutationLog ────────────────────────────────────────────────── */

#define MUT_LOG_MAX 64    /* max mutations tracked — now 64 × ~80B = ~5 KB */

typedef struct {
    MutationRecord entries[MUT_LOG_MAX];
    int n;
} MutationLog;

static inline void mut_log_init(MutationLog *log) { log->n = 0; }

/* Free all heap resources held by the log (call before discarding) */
static inline void mut_log_free_records(MutationLog *log) {
    for (int i = 0; i < log->n; i++)
        mut_record_free(&log->entries[i]);
    log->n = 0;
}

/* Push a record (takes ownership of heap snap_before pointer).
 * If log is full, frees the oldest entry before overwriting. */
static inline void mut_log_push(MutationLog *log, const MutationRecord *rec) {
    if (log->n < MUT_LOG_MAX) {
        log->entries[log->n++] = *rec;
    } else {
        /* Free oldest (index 0), shift left */
        mut_record_free(&log->entries[0]);
        for (int i = 0; i < MUT_LOG_MAX - 1; i++)
            log->entries[i] = log->entries[i+1];
        log->entries[MUT_LOG_MAX-1] = *rec;
    }
}

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Apply a mutation AND record before/after values.
 * Returns 1 if applied, 0 if no-op.
 * rec is populated only if return value is 1.
 */
int shmc_mutate_tracked(ShmcWorld *w, MutateType type, uint32_t *rng,
                        MutationRecord *rec);

/*
 * Apply structural patch mutation AND record before/after snapshot.
 * Returns 1 if applied, 0 if no-op.
 */
int shmc_patch_struct_mutate_tracked(ShmcWorld *w, uint32_t *rng,
                                     MutationRecord *rec);

/*
 * Apply one of the 6 structural motif mutations (INVERT/RETROGRADE/AUGMENT/
 * DIMINISH/ADD_NOTE/DEL_NOTE) AND record a full VoiceProgram snapshot.
 * type must be MUTATE_MOTIF_* or MUTATE_STRUCTURAL.
 * Returns 1 if applied, 0 if no-op.
 */
int shmc_mutate_structural_tracked(ShmcWorld *w, MutateType type, uint32_t *rng,
                                   MutationRecord *rec);

/*
 * Undo a mutation record — exact inverse.
 * apply(apply(P, m), undo(m)) == P
 * Returns 1 on success.
 */
int shmc_mutate_undo(ShmcWorld *w, const MutationRecord *rec);

/*
 * Undo last mutation in log.
 * Returns 1 if undone, 0 if log empty.
 */
int mut_log_undo_last(MutationLog *log, ShmcWorld *w);

/*
 * Format log as human-readable mutation diff.
 * Returns bytes written (excluding null).
 */
int mut_log_to_str(const MutationLog *log, char *buf, int cap);

/*
 * Format a single record as a one-line description.
 */
int mut_record_to_str(const MutationRecord *rec, char *buf, int cap);

#ifdef __cplusplus
}
#endif
