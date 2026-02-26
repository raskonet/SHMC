#pragma once
/*
 * SHMC Layer 2 — Motif Mutation Operators
 *
 * Pure functions: input VoiceProgram + parameters → new VoiceProgram.
 * No allocation. No side effects.
 *
 * Design invariants (each verified by verify_motif_mutations.c):
 *
 *  PITCH:      mutate_pitch(vp, +N) → all VI_NOTE pitches += N (clamped 21-108)
 *  VELOCITY:   mutate_velocity(vp, ±1) → vel_idx ±1 (clamped 0-15)
 *  DURATION:   mutate_duration(vp, note_idx, new_dur) → specific note dur changes
 *  RETROGRADE: mutate_retrograde(vp) → VI_NOTE sequence reversed, non-notes unchanged
 *  INVERT:     mutate_invert(vp) → intervals mirrored around first VI_NOTE pitch
 *  ROUND-TRIP: mutate_pitch(mutate_pitch(vp, +N), -N) == vp for any N (idempotent)
 *
 * Limits from shmc_dsl_limits.h apply:
 *   MIDI 21..108  (A0..C8)
 *   dur_idx 0..6
 *   vel_idx 0..15
 */
#include "../../layer1/include/voice.h"

/* Clamp helpers */
#define MIDI_CLAMP(p)  ((p)<21?21:((p)>108?108:(p)))
#define DUR_CLAMP(d)   ((d)<0?0:((d)>6?6:(d)))
#define VEL_CLAMP(v)   ((v)<0?0:((v)>15?15:(v)))

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shift all VI_NOTE pitches by semitones (positive or negative).
 * Pitches are clamped to [21,108]. Non-NOTE instructions unchanged.
 */
VoiceProgram motif_mutate_pitch(const VoiceProgram *vp, int semitones);

/*
 * Shift all VI_NOTE velocity indices by delta.
 * Clamped to [0,15].
 */
VoiceProgram motif_mutate_velocity(const VoiceProgram *vp, int delta);

/*
 * Replace the duration index of the note at note_idx (0-based count of
 * VI_NOTE instructions) with new_dur_idx. Other instructions unchanged.
 */
VoiceProgram motif_mutate_duration(const VoiceProgram *vp,
                                    int note_idx, int new_dur_idx);

/*
 * Reverse the order of all VI_NOTE instructions.
 * Non-NOTE instructions (rests, ties) are dropped — the result contains
 * only VI_NOTE entries in reversed pitch order.
 * This is "retrograde" in music theory.
 */
VoiceProgram motif_mutate_retrograde(const VoiceProgram *vp);

/*
 * Mirror all intervals around the first VI_NOTE pitch (axis).
 * new_pitch[i] = 2*axis - original_pitch[i]
 * Clamped to [21,108].
 * This is "inversion" in music theory.
 */
VoiceProgram motif_mutate_invert(const VoiceProgram *vp);

/*
 * Augmentation: multiply all duration indices by 1 step (each ×2 in time).
 * Clamped to [0,6]. Converts eighth→quarter→half→whole.
 */
VoiceProgram motif_mutate_augment(const VoiceProgram *vp);

/*
 * Diminution: divide all duration indices by 1 step (each ÷2 in time).
 * Clamped at 0. Converts quarter→eighth→sixteenth.
 */
VoiceProgram motif_mutate_diminish(const VoiceProgram *vp);

#ifdef __cplusplus
}
#endif
