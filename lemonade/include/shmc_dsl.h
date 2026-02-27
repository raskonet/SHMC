#pragma once
/*
 * SHMC DSL Text Layer — shmc_dsl.h
 * ===================================
 * A human-readable / LLM-readable text DSL that compiles down to
 * SHMC C API calls (layers 2–4).
 *
 * Grammar (EBNF):
 *
 *   program    = { statement }
 *   statement  = patch_stmt | motif_stmt | section_stmt | song_stmt
 *
 *   patch_stmt = "PATCH" name "{" { patch_op } "}"
 *   patch_op   = "osc" osc_type param* ";"
 *              | "fm" param param param ";"
 *              | "lpf" param param ";"
 *              | "adsr" param param param param ";"
 *              | "out" param ";"
 *
 *   motif_stmt = "MOTIF" name "{" { note_line } "}"
 *   note_line  = "note" midi_num dur_idx vel_idx ";"
 *
 *   section_stmt = "SECTION" name length_beats "{" { use_line } "}"
 *   use_line     = "use" motif_name "@" beat_f ["x" repeat] [modifier*] ";"
 *   modifier     = "t=" int | "v=" float | "ts=" float
 *                       (transpose / vel_scale / time_scale)
 *
 *   song_stmt  = "SONG" name bpm "{" { entry_line } "}"
 *   entry_line = "play" section_name ["patch" patch_name] ["x" repeat] ";"
 *
 * Example:
 *
 *   PATCH bass {
 *     osc saw ONE;
 *     lpf $0 20;
 *     adsr 0 6 15 8;
 *     out mul $1 $2;
 *   }
 *
 *   MOTIF walk_C {
 *     note 36 2 7;
 *     note 43 2 7;
 *   }
 *
 *   SECTION blues_A 48.0 {
 *     use walk_C @  0 x4;
 *     use walk_C @ 16 x2 t=5;
 *     use walk_C @ 24 x2;
 *     use walk_C @ 32 x1 t=7;
 *     use walk_C @ 36 x1 t=5;
 *     use walk_C @ 40 x2;
 *   }
 *
 *   SONG my_blues 120.0 {
 *     play blues_A patch bass;
 *   }
 */

#include "../../layer4/include/song.h"
#include "../../layer5/include/patch_search.h"
#include <stddef.h>

#define DSL_MAX_PATCHES   16
#define DSL_MAX_MOTIFS    64
#define DSL_MAX_SECTIONS  16
#define DSL_MAX_SONGS      4
#define DSL_ERR_SZ       512

/* Compiled DSL world — all objects live here */
typedef struct {
    PatchProgram  patches[DSL_MAX_PATCHES];
    char          patch_names[DSL_MAX_PATCHES][32];
    int           n_patches;

    MotifLibrary *lib;         /* heap-allocated (1MB) */

    Section       sections[DSL_MAX_SECTIONS];
    char          section_names[DSL_MAX_SECTIONS][64];
    int           n_sections;

    Song          songs[DSL_MAX_SONGS];
    int           n_songs;
} ShmcWorld;

#ifdef __cplusplus
extern "C" {
#endif

/* Compile DSL source text into a ShmcWorld.
 * Returns 0 on success, -1 on error (err filled). */
int   shmc_dsl_compile(const char *src, ShmcWorld *world,
                       char *err, int err_sz);

/* Free resources inside a ShmcWorld */
void  shmc_world_free(ShmcWorld *w);

/* Render the first song in the world to a heap-allocated float buffer.
 * Caller must free(*out). *n_frames set on success. */
int   shmc_world_render(ShmcWorld *w, float **out, int *n_frames, float sr);
/* Like shmc_world_render but stops after at most max_frames samples.
 * Use for preview rendering to avoid allocating/rendering the full song. */
int   shmc_world_render_n(ShmcWorld *w, float **out, int *n_frames, float sr, int max_frames);

/* Write rendered audio as 16-bit stereo WAV. */
int   shmc_write_wav(const char *path, const float *buf, int n_frames,
                     int n_ch, float sr);

#ifdef __cplusplus
}
#endif
