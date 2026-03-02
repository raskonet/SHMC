#pragma once
/*
 * shmc_mcts.h  —  Monte Carlo Tree Search over SHMC music programs
 *
 * Architecture
 * ─────────────
 * Each MCTS node represents a WORLD STATE (a ShmcWorld).
 * An EDGE from parent→child represents a single mutation.
 * The root is the seed world.
 *
 * UCT formula (Upper Confidence Bound for Trees):
 *   UCT(n) = Q(n)/N(n) + C * sqrt(ln(N(parent)) / N(n))
 *   where C = exploration constant (default 1.41 ≈ √2)
 *
 * MCTS loop (N iterations):
 *   1. SELECT:   walk tree from root using UCT until a leaf node
 *   2. EXPAND:   generate K_EXPAND children via random mutations
 *   3. ROLLOUT:  from each child, play out R_ROLLOUT mutations and evaluate
 *   4. BACKPROP: update Q and N values from leaf to root
 *
 * Why MCTS is better than beam search for music programs:
 *   - Beam search: keeps top-K globally, discards everything else forever
 *   - MCTS: maintains a TREE, can revisit promising earlier states
 *   - Music has long-range dependencies (motif A in bar 1 affects bar 4)
 *     → MCTS can explore multiple mutation paths from the same ancestor
 *   - Beam collapses diversity; MCTS maintains it via the tree structure
 *
 * Fitness function: pluggable.
 *   Use EvoFitFn = pointer to a function(audio, n, sr) → float.
 *   The default uses evo_fitness() from shmc_evo_fitness.
 *   The old wfeat_fitness() can also be plugged in for comparison.
 *
 * Memory:
 *   Nodes are allocated from a fixed pool (MCTS_MAX_NODES).
 *   When pool is full, no new nodes are expanded (tree stops growing).
 *   Each node stores a WORLD HASH (not the full world) for dedup.
 *   Full ShmcWorld is only stored at the current best leaf.
 *
 * MCTS_MAX_NODES = 2048  (each node ~80 bytes → ~160KB total)
 */
#include "shmc_dsl.h"
#include "shmc_mutate.h"
#include "shmc_evo_fitness.h"
#include "shmc_harmony.h"
#include "shmc_map_elites.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCTS_MAX_NODES     2048
#define MCTS_K_EXPAND      4      /* children generated per expansion */
#define MCTS_R_ROLLOUT     2      /* mutations per rollout step */
#define MCTS_C_EXPLORE     1.41f  /* UCT exploration constant √2 */
#define MCTS_PREVIEW_SEC   3.0f   /* preview render length for evaluation */

/* Pluggable fitness function signature */
typedef float (*EvoFitFn)(const float *audio, int n, float sr);

/* ── MCTS Node ─────────────────────────────────────────────────── */

typedef struct MctsNode {
    int parent;
    int children[MCTS_K_EXPAND];
    int n_children;
    float  Q;
    int    N;
    uint64_t  world_hash;
    float     raw_fit;
    ShmcWorld *world;  /* world owned by this node, NULL after full expansion */
    char  mut_desc[32];
    int   depth;
} MctsNode;

/* ── Mutation History ────────────────────────────── */
#define MCTS_MUT_TYPES 19
typedef struct {
    int   trials[MCTS_MUT_TYPES];
    float reward[MCTS_MUT_TYPES];
} MutHistory;

/* ── MCTS Context ─────────────────────────────────────────────── */

typedef struct {
    MctsNode pool[MCTS_MAX_NODES];  /* flat node pool */
    int      n_nodes;               /* number of nodes allocated */

    /* Best world found so far */
    ShmcWorld *best_world;          /* heap-allocated, NULL until first eval */
    float      best_fit;
    int        best_node;           /* index into pool */

    /* Configuration */
    EvoFitFn      fit_fn;           /* legacy audio-only fn (NULL → full pipeline) */
    EvoWeights    evo_wt;           /* weights for evo component */
    HarmonyWeights harm_wt;         /* weights for harmony component */
    CombinedWeights comb_wt;        /* weights combining harmony+evo+novelty */
    float         sr;               /* sample rate */
    uint32_t      rng;              /* RNG state */

    /* Novelty archive (ring-buffer of EvoFeat spectral embeddings) */
#define MCTS_ARCHIVE_CAP 256
#define MCTS_NOVELTY_K     5
#define MCTS_NOVELTY_THRESH 0.04f  /* min novelty to archive (Lehman 2011) */
    EvoFeat   nov_archive[MCTS_ARCHIVE_CAP];
    int       nov_n;               /* entries used */
    int       nov_head;            /* ring-buffer write head */

    /* MAP-Elites quality-diversity archive */
    MeArchive map_elites;
    int       me_enabled;   /* 1=use ME for expansion roots */

    /* Mutation history */
    MutHistory mut_hist;

    /* Statistics */
    int  n_iters;
    int  n_evals;
    int  n_dedup;
} MctsCtx;

/* ── API ──────────────────────────────────────────────────────── */

/*
 * Initialise an MCTS context.
 * seed_world: the starting world (deep-copied, not retained).
 * fit_fn:     fitness function, or NULL to use evo_fitness.
 */
int  mcts_init(MctsCtx *ctx, const ShmcWorld *seed_world,
               EvoFitFn fit_fn, uint32_t rng_seed);

/*
 * Run n_iters MCTS iterations.
 * Each iteration: SELECT → EXPAND → ROLLOUT → BACKPROP.
 * Returns the current best fitness.
 */
float mcts_run(MctsCtx *ctx, int n_iters);

/*
 * Copy the best world found into dst (caller-allocated).
 * Returns 1 on success, 0 if no world found yet.
 */
int mcts_best_world(const MctsCtx *ctx, ShmcWorld *dst);

/*
 * Free all resources held by ctx (including best_world).
 */
void mcts_free(MctsCtx *ctx);

/*
 * Human-readable summary of search state.
 */
void mcts_print_stats(MctsCtx *ctx);

/*
 * UCT value for a node given its parent visit count.
 * Public for testing.
 */
float mcts_uct(const MctsNode *node, int parent_visits, float C);

#ifdef __cplusplus
}
#endif
