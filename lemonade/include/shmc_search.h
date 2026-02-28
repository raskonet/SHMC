#pragma once
/*
 * SHMC Layer 6 — DSL-Space Iterative Beam Search
 *
 * Beam search over the space of ShmcWorld mutations.
 * Operates at DSL/motif/patch level (above the patch-VM search in layer5).
 *
 * Algorithm:
 *   1. Start with beam_width copies of seed world
 *   2. Render each → extract WorldFeatures → score fitness
 *   3. Keep top-K by fitness
 *   4. Mutate each K member muts_per_cand times → new candidate pool
 *   5. Score pool, keep top-K, repeat for max_generations
 *
 * WorldFitness (no target audio — intrinsic music quality):
 *   30% Audibility       — RMS above silence floor
 *   25% Envelope variety — windowed RMS variance (non-flat = musical)
 *   20% Temporal spread  — beat positions fill the section
 *   15% Pitch diversity  — unique pitches / total notes
 *   10% Dynamic range    — peak/RMS crest factor
 *
 * Verified: verify_search.c N/N PASSED before integration.
 */
#include "shmc_dsl.h"
#include "shmc_mutate.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEARCH_BEAM_W        8
#ifndef SEARCH_MAX_GEN
#define SEARCH_MAX_GEN       50
#endif
#define SEARCH_MUTS_PER_CAND 4
#define SEARCH_SR            44100.f
#define SEARCH_FEAT_WIN      16
#define SEARCH_WIN_SZ        512

typedef struct {
    float rms;
    float rms_env[SEARCH_FEAT_WIN];   /* envelope over time */
    float zcr_brightness;             /* ZCR-based spectral proxy */
    float temporal_spread;
    float pitch_diversity;
    float dynamic_range;
    /* symbolic structure features */
    float motif_repetition;   /* fraction of section uses that repeat a motif (0-1) */
    float rhythm_entropy;     /* entropy of note duration distribution (0-1) */
} WFeat;  /* v3: added motif_repetition, rhythm_entropy */

typedef struct {
    float w_audibility;      /* 0.25 */
    float w_env_variety;     /* 0.20 */
    float w_brightness;      /* 0.15  — ZCR spectral brightness */
    float w_temporal;        /* 0.15 */
    float w_pitch_div;       /* 0.10 */
    float w_dynamics;        /* 0.10 */
    /* symbolic weights */
    float w_motif_rep;       /* 0.03 — reward motif reuse across section */
    float w_rhythm_ent;      /* 0.02 — reward rhythmic diversity */
} WWeights;

typedef struct {
    const char *seed_dsl;
    int         beam_width;      /* default SEARCH_BEAM_W */
    int         max_generations; /* default SEARCH_MAX_GEN */
    int         muts_per_cand;   /* default SEARCH_MUTS_PER_CAND */
    float       sr;
    WWeights    weights;
    uint32_t    seed;
    void (*progress_cb)(int gen, float best_fit, void *ud);
    void       *userdata;
} ShmcSearchCfg;

typedef struct {
    float best_fitness;
    int   generations_run;
    int   total_mutations;
    int   total_renders;
    int   n_dedup_skipped;    /* worlds skipped by hash dedup (v2) */
    /* Best world stored in-place for caller to use */
    ShmcWorld best_world;
    int       best_world_valid;
} ShmcSearchResult;

/* Feature extraction + fitness */
void   wfeat_extract(const float *audio, int n, float sr,
                     const ShmcWorld *w, WFeat *out);
float  wfeat_fitness(const WFeat *f, const WWeights *w);
void   wweights_default(WWeights *out);
void   search_cfg_default(ShmcSearchCfg *cfg, const char *seed_dsl, uint32_t seed);

/* Run beam search. result->best_world must be freed with shmc_world_free(). */
int    shmc_search_run(const ShmcSearchCfg *cfg, ShmcSearchResult *result,
                       char *err, int err_sz);

#ifdef __cplusplus
}
#endif
