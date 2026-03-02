#pragma once
/*
 * shmc_map_elites.h  —  MAP-Elites Quality-Diversity Archive
 *
 * From "Illuminating Search Spaces by Mapping Elites" (Mouret & Clune, 2015).
 *
 * Instead of one "best world", MAP-Elites maintains a 4D grid where each
 * cell holds the BEST world discovered with a particular musical behavior.
 *
 * Behavior dimensions:
 *   b1 = brightness    — mean ZCR [0,1]
 *   b2 = rhythm_density — notes per beat [0,1], saturates at 4 notes/beat
 *   b3 = pitch_diversity — unique pitch classes / 12 [0,1]
 *   b4 = tonal_tension  — H6 Lerdahl tension [0,1]
 *
 * Grid: 6×6×6×6 = 1296 cells  (doc10 uses 8^4=4096; we use 6^4 to save ~60MB)
 *
 * Each cell stores fitness + world pointer.
 * Update rule: replace cell if empty OR new_fitness > old_fitness.
 *
 * Integration with MCTS:
 *   After each MCTS evaluation, shmc_me_update() is called.
 *   shmc_me_random_elite() selects a random filled cell for next expansion.
 */
#include "shmc_dsl.h"
#include "shmc_evo_fitness.h"
#include "shmc_harmony.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ME_BINS     6       /* bins per dimension */
#define ME_DIM      4       /* number of behavior dimensions */
#define ME_CELLS    1296    /* ME_BINS^ME_DIM */

/* Behavior descriptor: 4 normalized floats in [0,1] */
typedef struct {
    float b[ME_DIM];   /* brightness, rhythm_density, pitch_div, tonal_tension */
} MeBehavior;

/* One MAP-Elites cell */
typedef struct {
    float      fitness;    /* -1.0 = empty */
    ShmcWorld *world;      /* heap-allocated, NULL if empty */
} MeCell;

/* Full MAP-Elites archive */
typedef struct {
    MeCell   cells[ME_CELLS];
    int      n_filled;   /* number of non-empty cells */
    int      n_updates;  /* total update attempts */
    int      n_improves; /* cells replaced with better individual */
    uint32_t rng;        /* for random elite selection */
} MeArchive;

/* Initialize archive (all cells empty) */
void shmc_me_init(MeArchive *arc, uint32_t rng_seed);

/* Free all world pointers in archive */
void shmc_me_free(MeArchive *arc);

/*
 * Compute behavior descriptor from audio buffer + HarmonyFeat.
 * audio: preview PCM, n: samples, sr: sample rate
 * hf: pre-computed harmony features (for tonal_tension)
 */
MeBehavior shmc_me_describe(const float *audio, int n, float sr,
                             const HarmonyFeat *hf);

/*
 * Map behavior to grid cell index.
 * Returns index in [0, ME_CELLS).
 */
int shmc_me_cell_idx(const MeBehavior *b);

/*
 * Try to insert world into archive.
 * Returns 1 if cell was updated (new or improved), 0 otherwise.
 * world is deep-copied if accepted (caller retains ownership).
 */
int shmc_me_update(MeArchive *arc, const ShmcWorld *world,
                    float fitness, const MeBehavior *b);

/*
 * Return a random non-empty cell index, or -1 if archive is empty.
 */
int shmc_me_random_elite(MeArchive *arc);

/*
 * Print a 2D heatmap projection (b1=brightness × b2=rhythm) to stdout.
 */
void shmc_me_print_map(const MeArchive *arc);

/*
 * Copy the highest-fitness world across all cells into dst.
 * Returns 1 on success, 0 if archive empty.
 */
int shmc_me_best_world(const MeArchive *arc, ShmcWorld *dst);

#ifdef __cplusplus
}
#endif
