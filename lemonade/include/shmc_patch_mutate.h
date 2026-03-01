#pragma once
/*
 * shmc_patch_mutate.h — Structural patch mutation   v1
 *
 * Graph-rewrite mutations on PatchProgram:
 *   - Substitute operator (lpf↔hpf, saw↔tri↔square, tanh↔clip↔fold)
 *   - Insert distortion before output
 *   - Insert new filter before output
 *   - Remove a non-critical filter/distortion
 *
 * These are TYPE-SAFE rewrites — they preserve signal flow.
 * Invalid mutations (e.g. creating cycles, breaking OUT) are rejected.
 *
 * Usage:
 *   uint32_t rng = 0x1234;
 *   shmc_patch_struct_mutate(&world.patches[0], PATCH_STRUCT_ANY, &rng);
 *   // OR:
 *   shmc_world_patch_struct_mutate(&world, &rng); // picks random patch
 */
#include "../include/shmc_dsl.h"
#include "../../layer0/include/patch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PATCH_STRUCT_SUBSTITUTE  = 0,  /* swap filter/osc/dist type */
    PATCH_STRUCT_INSERT_DIST = 1,  /* add tanh/clip/fold before OUT */
    PATCH_STRUCT_INSERT_FILT = 2,  /* add lpf/hpf/bpf before OUT */
    PATCH_STRUCT_REMOVE      = 3,  /* remove a non-critical op */
    PATCH_STRUCT_ANY         = 4   /* choose randomly (4:2:2:2 weights) */
} PatchStructMutType;

/* Mutate a single PatchProgram structurally. Returns 1 if applied, 0 if no-op. */
int shmc_patch_struct_mutate(PatchProgram *pp, PatchStructMutType type, uint32_t *rng);

/* Mutate a random patch in the world. Returns 1 if applied, 0 if no-op. */
int shmc_world_patch_struct_mutate(ShmcWorld *w, uint32_t *rng);

#ifdef __cplusplus
}
#endif
