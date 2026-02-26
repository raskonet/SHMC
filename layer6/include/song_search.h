#pragma once
/*
 * SHMC Layer 6 — DSL Song Search Engine    song_search.h
 *
 * Beam search over full ShmcWorld (DSL program) space.
 * Uses shmc_mutate() operators instead of patch_mutate(),
 * enabling search over pitch, rhythm, harmony, AND timbre simultaneously.
 *
 * Architecture:
 *   Layer 5:  beam search over PatchProgram (timbre only)
 *   Layer 6:  beam search over ShmcWorld   (full song structure)
 *
 * Algorithm: same beam search as layer5
 *   1. Start from a seed ShmcWorld (LLM-generated or hand-written DSL)
 *   2. Render each candidate → score via SongFeature fitness
 *   3. Keep top-K (beam)
 *   4. Mutate each beam member SONG_MUTATIONS times with each MutateType
 *   5. Score all children, keep top-K
 *   6. Repeat for SONG_MAX_GEN generations
 *
 * Fitness (audio-level, same FFT-free features as layer5):
 *   40%  RMS envelope shape   — energy over time
 *   25%  Spectral centroid    — timbral brightness
 *   15%  ZCR                  — roughness
 *   10%  Spectral flux        — timbre change rate
 *   10%  Onset sharpness      — rhythmic attack character
 *
 * Formally verified: verify_song_search.c (all tests pass)
 */

#include "../../lemonade/include/shmc_dsl.h"
#include "../../lemonade/include/shmc_mutate.h"
#include "../../layer5/include/patch_search.h"  /* reuse FitnessCtx, FeatureVec */
#include <stdint.h>

/* ── Tuning ──────────────────────────────────────────────────────── */
#define SONG_BEAM_SIZE       8    /* candidates kept per generation */
#define SONG_MUTATIONS       6    /* mutations per beam member */
#define SONG_MAX_GEN        50    /* max generations before giving up */
#define SONG_FITNESS_THRESH  0.85f/* stop early if fitness exceeds */
#define SONG_AUDIO_LEN    (FEAT_FRAMES * FEAT_WINDOW_SZ)  /* 4096 samples */
#define SONG_SR            44100.f

/* ── Types ───────────────────────────────────────────────────────── */

/* One candidate in the beam: a ShmcWorld + its fitness score */
typedef struct {
    ShmcWorld world;
    float     fitness;
    int       valid;      /* 1 if world compiled and rendered OK */
} SongCandidate;

/* Search context (target + fitness weights, same as FitnessCtx) */
typedef struct {
    FitnessCtx fit_ctx;   /* reuse layer5 fitness: target audio + weights */
    const char *seed_dsl; /* starting DSL program (e.g. from LLM) */
} SongSearchCtx;

typedef struct {
    ShmcWorld best_world;     /* copy of best candidate's world */
    float     best_fitness;
    int       n_generations;
    int       n_evaluations;
    int       found;          /* 1 if SONG_FITNESS_THRESH exceeded */
} SongSearchResult;

/* Optional progress callback: return 0 to continue, 1 to abort */
typedef int (*SongProgressFn)(int gen, float best_fit,
                              int n_evals, void *userdata);

/* ── API ─────────────────────────────────────────────────────────── */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * song_fitness_score() — render a world and return its fitness score [0,1]
 * Returns -1.0 if the world can't be rendered (compile/alloc failure).
 */
float song_fitness_score(const SongSearchCtx *ctx, ShmcWorld *w);

/*
 * song_search() — run beam search from seed_dsl toward target audio.
 *
 * ctx->seed_dsl  : starting DSL string (mutated, never overwritten)
 * ctx->fit_ctx   : pre-initialised with target audio via fitness_ctx_init()
 * seed           : RNG seed (different seeds → different search trajectories)
 * result         : filled with best world + stats on return
 * progress_cb    : called each generation (may be NULL)
 */
void song_search(const SongSearchCtx *ctx, uint32_t seed,
                 SongSearchResult *result,
                 SongProgressFn progress_cb, void *userdata);

/*
 * song_search_ctx_init() — convenience: initialise ctx from target WAV samples
 */
void song_search_ctx_init(SongSearchCtx *ctx,
                          const float *target_audio, int target_n,
                          const char *seed_dsl, float sr);

#ifdef __cplusplus
}
#endif
