#pragma once
/*
 * SHMC Layer 3 — Section DSL  (rev 2)
 *
 * A Section is a named collection of per-voice MotifUse schedules.
 * Multiple tracks play simultaneously; SectionRenderer mixes them to stereo.
 *
 * Fixes vs rev 1:
 *   L3-1: Early-out guard in section_render_block when already done.
 *   L3-2: mono[] scratch buffer promoted to static, not stack-per-call.
 *   L3-3: TrackState EventStream heap-allocated (was ≈1.5 MB embedded in struct).
 *   L3-4: section_renderer_destroy() to free heap EventStreams.
 *   L3-7: section_render_block() documents overwrite semantics; asserts L != R.
 *         New section_render_block_mix() adds into existing buffers instead.
 *
 * Stack sizes (rev 2):
 *   Section             ≈ 36 KB   (8 tracks × 64 uses × 64 bytes)
 *   SectionRenderer     ≈ 280 KB  (8 × VoiceRenderer ≈ 34KB each + pointers)
 *   Still large — declare static or heap-allocate SectionRenderer too.
 */

#include "../../layer2/include/motif.h"
#include <assert.h>

#define SECTION_NAME_LEN   32
#define SECTION_MAX_TRACKS  8
#define SECTION_MAX_USES   64

/* ---- One voice track definition (lightweight — no EventStream here) ---- */
typedef struct {
    char               name[SECTION_NAME_LEN];
    const PatchProgram *patch;
    MotifUse           uses[SECTION_MAX_USES];
    int                n_uses;
    float              gain;   /* 0..1 */
    float              pan;    /* -1..+1 */
} SectionTrack;

/* ---- Section (definition only — no compiled state) ---- */
typedef struct {
    char         name[SECTION_NAME_LEN];
    SectionTrack tracks[SECTION_MAX_TRACKS];
    int          n_tracks;
    float        length_beats;   /* nominal length; 0 = infer from content */
} Section;

/*
 * ---- Per-track runtime state ----
 * EventStream is HEAP-ALLOCATED (fix L3-3) — pointed to, not embedded.
 */
typedef struct {
    EventStream   *es;       /* heap-allocated; owned by SectionRenderer */
    VoiceRenderer  vr;
    float          gain;
    float          pan_l;
    float          pan_r;
} TrackState;

/*
 * ---- SectionRenderer ----
 * sizeof(SectionRenderer) ≈ 8 × (sizeof(VoiceRenderer) + pointer) ≈ 280 KB.
 * Still large — declare static or heap-allocate.
 */
/* N5-3: Block size cap for internal scratch buffer */
#define SECTION_BLOCK_MAX 4096

typedef struct {
    const Section *section;
    TrackState     tracks[SECTION_MAX_TRACKS];
    int            n_tracks;
    int            done;
    float          sr;
    float          bpm;
    /* N5-3: per-instance scratch buffers — replaces static locals,
       makes rendering re-entrant and thread-safe.
       NOTE: sizeof(SectionRenderer) increases by ~16 KB. */
    float          scratch_mono[SECTION_BLOCK_MAX];
    float          scratch_l[SECTION_BLOCK_MAX];
    float          scratch_r[SECTION_BLOCK_MAX];
} SectionRenderer;

#ifdef __cplusplus
extern "C" {
#endif

void section_init(Section *s, const char *name, float length_beats);

int  section_add_track(Section *s, const char *name,
                       const PatchProgram *patch,
                       const MotifUse *uses, int n_uses,
                       float gain, float pan);

/*
 * Compile all tracks and initialise the renderer.
 * Heap-allocates one EventStream per track (fix L3-3).
 * Call section_renderer_destroy() when done to free them.
 * Returns 0 on success, -1 on error.
 */
int section_renderer_init(SectionRenderer *sr,
                          const Section *section,
                          const MotifLibrary *lib,
                          float sample_rate, float bpm,
                          char *err, int err_sz);

/* Free heap-allocated EventStreams. Safe to call on a zeroed renderer. */
void section_renderer_destroy(SectionRenderer *sr);

/*
 * R2-6: Reset a SectionRenderer to the beginning WITHOUT freeing/reallocating
 * EventStreams. Much cheaper than destroy+init for repeated sections.
 * Only resets playback heads; re-uses compiled EventStreams.
 * bpm: new BPM for this repetition (updates rendering speed).
 */
void section_renderer_reset(SectionRenderer *sr, float bpm);

/*
 * Render one block into stereo out_l[], out_r[].
 * OVERWRITES out_l and out_r (does not add to existing content).
 * out_l and out_r must be different pointers (fix L3-7).
 * Returns 0 while playing, 1 when all tracks are done.
 */
int section_render_block(SectionRenderer *sr,
                         float *out_l, float *out_r, int n);

/*
 * Like section_render_block but ADDS to out_l/out_r instead of overwriting.
 * Useful for mixing multiple sections into the same output buffer (Layer 4).
 */
int section_render_block_mix(SectionRenderer *sr,
                             float *out_l, float *out_r, int n);

/* Render to interleaved stereo. */
int section_render_interleaved(SectionRenderer *sr, float *out_lr, int n_frames);


/* T4-4: Maximum total events per track across all motif uses (prevents polyphony explosion) */
#define SECTION_MAX_EVENTS_PER_TRACK  2048

/* T4-5: Section length enforcement mode */
typedef enum {
    SLV_IGNORE = 0,   /* no validation (default) */
    SLV_WARN,         /* print warning if motif extends past section.length */
    SLV_ERROR         /* return -1 error if any motif extends past length */
} SectionLenValidate;

/*
 * T4-4/T4-5: Validate a Section for budget + length constraints.
 * mode: controls what happens when motifs exceed section.length_beats.
 * max_events_per_track: 0 → use SECTION_MAX_EVENTS_PER_TRACK default.
 * Returns 0 if valid, -1 on constraint violation (err filled).
 */
int section_validate(const Section *s, const MotifLibrary *lib,
                     float sr, float bpm,
                     SectionLenValidate mode, int max_events_per_track,
                     char *err, int err_sz);

#ifdef __cplusplus
}
#endif
