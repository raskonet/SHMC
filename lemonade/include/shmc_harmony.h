#pragma once
/*
 * shmc_harmony.h  —  Symbolic Harmony Fitness
 *
 * Five orthogonal measures from symbolic ShmcWorld — no audio needed.
 *
 * H1  scale_consistency  — Lerdahl key-finding:  pitch-class histogram
 *                           vs 72 scale templates (12 roots × 6 modes).
 *                           Score = fraction of note-events in best scale.
 *
 * H2  consonance         — Sethares psychoacoustic roughness:  consonance
 *                           table applied to all simultaneous note pairs.
 *                           Monophonic fallback: melodic intervals.
 *
 * H3  voice_leading      — Tymoczko geometry:  exp(-k * mean_semitone_jump)
 *                           across consecutive MotifUse entries per track.
 *
 * H4  tension_arc        — MorpheuS: section split into 8 windows.
 *                           Score = arc_variance + end_resolution.
 *
 * H5  cadence            — Piston root-motion table: chord root per window
 *                           scored by interval class (desc-5th = 1.0, etc.)
 *
 * Combined:
 *   harmony_score = 0.18*H1+0.12*H2+0.10*H3+0.10*H4+0.12*H5
 *                +0.12*H6+0.08*H7+0.08*H8+0.05*H9+0.03*H10+0.02*H11
 *                +0.04*H12+0.03*H13
 *
 * Total fitness (with evo_fitness + novelty):
 *   total = 0.60*harmony + 0.35*evo + 0.05*novelty
 */
#include "shmc_dsl.h"
#include "shmc_evo_fitness.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float scale_consistency;   /* H1 [0,1] — Lerdahl key-finding */
    float consonance;          /* H2 [0,1] — Sethares roughness */
    float voice_leading;       /* H3 [0,1] — Tymoczko geometry */
    float tension_arc;         /* H4 [0,1] — MorpheuS arc */
    float cadence;             /* H5 [0,1] — Piston root motion */

    float lerdahl_tension;     /* H6 [0,1] — weighted tonal-distance score */
    float harmonic_surprise;   /* H7 [0,1] — chord-transition entropy */
    float groove;              /* H8 [0,1] — inter-onset interval regularity */
    float pitch_diversity;     /* H9 [0,1] — unique pitch classes / 12 */
    float rhythm_entropy;      /* H10 [0,1] — onset distribution entropy */
    float motif_repetition;    /* H11 [0,1] — compression ratio (0.4-0.7 ideal) */
    float temporal_spread;     /* H12 [0,1] — beat-grid coverage */
    float multiscale_tension;  /* H13 [0,1] — hierarchical tension consistency */
    float chord_progression;   /* H14 [0,1] — Krumhansl bigram transition quality */
    float harmony_score;       /* weighted H1-H14 [0,1] */
    int   best_root;           /* detected tonal centre 0-11 */
    int   best_scale_type;     /* 0=major 1=min 2=dor 3=mix 4=phryg 5=lyd */
} HarmonyFeat;

typedef struct {
    float w_scale;    /* H1  — default 0.18 */
    float w_cons;     /* H2  — default 0.12 */
    float w_voice;    /* H3  — default 0.10 */
    float w_arc;      /* H4  — default 0.10 */
    float w_cad;      /* H5  — default 0.12 */
    float w_lerdahl;  /* H6  — default 0.12 */
    float w_surprise; /* H7  — default 0.08 */
    float w_groove;   /* H8  — default 0.08 */
    float w_pdiv;     /* H9  — default 0.05 */
    float w_rhythm;   /* H10 — default 0.03 */
    float w_motif;    /* H11 — default 0.02 */
    float w_spread;   /* H12 — default 0.04 */
    float w_mscale;   /* H13 — default 0.03 */
    float w_cprog;    /* H14 — default 0.07 */
} HarmonyWeights;

typedef struct {
    float w_harmony;  /* default 0.60 */
    float w_evo;      /* default 0.35 */
    float w_novelty;  /* default 0.05 — small novelty bonus */
} CombinedWeights;

void harmony_weights_default(HarmonyWeights *w);
void combined_weights_default(CombinedWeights *w);

/*
 * Extract H1-H5 from world symbolically (no render).
 * Returns 0 on success, -1 if world has no motifs.
 */
int   harmony_feat_extract(const ShmcWorld *world, HarmonyFeat *out,
                             const HarmonyWeights *w);

/*
 * Combine pre-computed harmony + evo + novelty into one score.
 * novelty: normalised novelty score from archive [0,1].
 */
float combined_fitness(const HarmonyFeat *hf, const EvoFeat *ef,
                        float novelty,
                        const HarmonyWeights *hw, const EvoWeights *ew,
                        const CombinedWeights *cw);

/*
 * Convenience: extract harmony + combine.  novelty = 0 if unknown.
 */
float world_total_fitness(const ShmcWorld *world, const EvoFeat *ef,
                           float novelty,
                           const HarmonyWeights *hw, const EvoWeights *ew,
                           const CombinedWeights *cw);

/* Psychoacoustic consonance for a semitone interval (0-11). */
float interval_consonance(int semitones);

#ifdef __cplusplus
}
#endif
