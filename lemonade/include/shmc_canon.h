#pragma once
/*
 * shmc_canon.h — Musical symmetry canonicalization   v2
 *
 * Two key capabilities:
 *
 * 1. shmc_world_hash() — complete world hash including motif note content.
 *    The standard hash_section() only hashes motif-use metadata (name, beat,
 *    repeat, transpose). shmc_world_hash() also hashes every motif's note
 *    content via hash_motif(), so two worlds with different notes → different
 *    hashes.
 *
 * 2. shmc_world_canonicalize() — sort motif-uses within sections by
 *    (start_beat, name). Audio-safe: the scheduler renders by beat position
 *    regardless of struct array order.
 *
 * Usage in search loop:
 *   shmc_world_canonicalize(&world);       // normalize ordering symmetries
 *   uint64_t h = shmc_world_hash(&world);  // full content hash
 *   if (visited(h)) continue;
 *   render_and_score(&world);
 */
#include "../include/shmc_dsl.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sort motif-uses within each section track by (start_beat, name) */
void shmc_section_canonicalize(Section *s);

/* Apply all canonicalization passes to a full world (audio-safe) */
void shmc_world_canonicalize(ShmcWorld *w);

/* Complete structural hash including motif note content */
uint64_t shmc_world_hash(const ShmcWorld *w);

#ifdef __cplusplus
}
#endif
