#pragma once
/*
 * SHMC Layer 5 — Patch Search Engine
 *
 * Given a target audio clip, searches the discrete PatchProgram space
 * to find a patch whose rendered output matches target audio features.
 *
 * Algorithm: Beam search with random restarts
 *   1. Generate BEAM_SIZE random PatchPrograms
 *   2. Score each via fitness() against the target
 *   3. Keep top-K
 *   4. Mutate each K member SEARCH_MUTATIONS times
 *   5. Score all, keep top-K, repeat
 *   6. Random restart every SEARCH_RESTART_EVERY generations
 *
 * Fitness (all FFT-free, computed on FEAT_FRAMES windows):
 *   40% RMS envelope match    — energy shape over time
 *   25% Spectral centroid     — brightness of timbre
 *   15% Zero-crossing rate    — roughness / noise
 *   10% Spectral flux         — how fast spectrum changes
 *   10% Onset sharpness       — attack transient character
 */

#include "../../layer0/include/patch_builder.h"
#include <stdint.h>
#include <time.h>

#define FEAT_FRAMES      16
#define FEAT_WINDOW_SZ   256
#define FEAT_TOTAL_LEN   (FEAT_FRAMES * FEAT_WINDOW_SZ)

#define SEARCH_BEAM_SIZE      16
#define SEARCH_MUTATIONS       8
#define SEARCH_MAX_INSTRS     12
#define SEARCH_MIN_INSTRS      3
#define SEARCH_MAX_GEN        200
#define SEARCH_RESTART_EVERY   40
#define SEARCH_FITNESS_THRESH  0.92f
#define SEARCH_MIDI_NOTE       60
#define SEARCH_SR              44100.f

typedef struct {
    float rms[FEAT_FRAMES];
    float centroid[FEAT_FRAMES];
    float zcr[FEAT_FRAMES];
    float flux;
    float onset;
} FeatureVec;

typedef struct {
    float      target_audio[FEAT_TOTAL_LEN];
    FeatureVec target_feat;
    float      sr;
    int        midi;
    float      w_rms;       /* 0.40 */
    float      w_centroid;  /* 0.25 */
    float      w_zcr;       /* 0.15 */
    float      w_flux;      /* 0.10 */
    float      w_onset;     /* 0.10 */
} FitnessCtx;

typedef struct {
    PatchProgram prog;
    float        fitness;
} Candidate;

typedef struct {
    Candidate best;
    int       n_generations;
    int       n_evaluations;
    double    time_seconds;
} SearchResult;

typedef int (*SearchProgressFn)(int gen, float best_fitness, void *userdata);

#ifdef __cplusplus
extern "C" {
#endif

void  feat_extract(const float *audio, int n, FeatureVec *out);
float feat_fitness(const FitnessCtx *ctx, const FeatureVec *cand);
void  fitness_ctx_init(FitnessCtx *ctx, const float *audio, int n,
                       int midi, float sr);
float fitness_score(const FitnessCtx *ctx, const PatchProgram *prog);
PatchProgram patch_random(uint32_t *rng, int n_instrs);
PatchProgram patch_mutate(const PatchProgram *src, uint32_t *rng);
void  patch_search(const FitnessCtx *ctx, uint32_t seed,
                   SearchResult *result,
                   SearchProgressFn progress_cb, void *userdata);

#ifdef __cplusplus
}
#endif
