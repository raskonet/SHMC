#pragma once
/*
 * shmc_dsl_emit.h — World-to-DSL serializer API   v1
 *
 * Reverse of shmc_dsl_compile(): converts a ShmcWorld back to DSL text.
 * Enables the closed generative loop:
 *
 *   DSL → compile → ShmcWorld → search/mutate → best_world
 *                                                    ↓
 *                              shmc_world_to_dsl() → DSL' → LLM refinement
 *
 * Round-trip:
 *   DSL → world1 → DSL2 → world2: hash(world1) == hash(world2)
 *
 * Usage:
 *   char buf[16384];
 *   int n = shmc_world_to_dsl(&world, buf, sizeof(buf));
 *   if (n < 0) { ... } // buffer too small
 *
 * Returns: number of bytes written (excluding null terminator), or -1 on overflow.
 */
#include "shmc_dsl.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int shmc_world_to_dsl(const ShmcWorld *w, char *out, int cap);

#ifdef __cplusplus
}
#endif
