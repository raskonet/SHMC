#pragma once
/*
 * SHMC Layer 4 — Song DSL  (rev 3)
 *
 * Changelog vs rev 2:
 *   R3: BpmPoint[] + naive integration replaced by TempoMap (layer0b).
 *       All beat→sample conversions are now sample-accurate with closed-form
 *       integrals (piecewise-constant, linear-SPB, linear-BPM segments).
 *   R2-6: section_renderer_reset() used for repeats (no destroy/realloc).
 *   BPM automation API unchanged; internally builds a TempoMap at render start.
 */

#include "../../layer3/include/section.h"
#include "../../layer0b/include/tempo_map.h"

#define SONG_NAME_LEN       64
#define SONG_MAX_ENTRIES    64
#define SONG_MAX_BPM_POINTS 32

/* ---- BPM automation point ---- */
typedef struct {
    float       beat;
    float       bpm;
    TempoInterp interp;   /* TM_STEP (default), TM_LINEAR_SPB, TM_LINEAR_BPM */
} BpmPoint;

/* ---- One entry in the song arrangement ---- */
typedef struct {
    char               name[SONG_NAME_LEN];
    const Section     *section;
    const MotifLibrary *lib;
    int                repeat;
    float              fade_in_beats;
    float              fade_out_beats;
    float              xfade_beats;
    float              gap_beats;
} SongEntry;

/* ---- Song definition ---- */
typedef struct {
    char      name[SONG_NAME_LEN];
    float     master_gain;
    float     sr;
    float     default_bpm;
    BpmPoint  bpm_points[SONG_MAX_BPM_POINTS];
    int       n_bpm_points;
    SongEntry entries[SONG_MAX_ENTRIES];
    int       n_entries;
} Song;

/* ---- Renderer ---- */
typedef struct {
    const Song      *song;
    TempoMap         tempo;              /* built once at renderer_new time */
    SectionRenderer *active[2];          /* [0]=current, [1]=outgoing xfade */
    int              entry_idx;
    int              repeat_left;
    int              done;
    double           sample_pos;         /* global sample counter (double) */
    double           entry_sample_start; /* sample_pos at entry start (R2-1) */
    double           xfade_pos;          /* samples into crossfade */
    double           xfade_len;          /* crossfade length in samples */
    double           fade_in_pos;        /* samples into fade-in */
    double           fade_in_len;        /* fade-in length in samples */
    double           fade_out_start;     /* samples from entry start */
    double           fade_out_len;       /* fade-out length in samples */
    double           song_beat;          /* beat position of current sample */
    /* N5-3: per-instance scratch buffers — replaces static locals in song_render_block.
       Makes SongRenderer re-entrant and thread-safe.
       Cost: ~32 KB per SongRenderer instance (already heap-allocated). */
    float scratch_l [4096];
    float scratch_r [4096];
    float scratch_xl[4096];
    float scratch_xr[4096];
} SongRenderer;

#ifdef __cplusplus
extern "C" {
#endif

void song_init(Song *song, const char *name, float default_bpm, float sr);

/*
 * Add a BPM automation point.  interp = TM_STEP by default.
 * Points must be added in beat order.
 */
int song_add_bpm(Song *song, float beat, float bpm);
int song_add_bpm_ex(Song *song, float beat, float bpm, TempoInterp interp);

int song_append(Song *song, const char *name,
                const Section *section, const MotifLibrary *lib,
                int repeat,
                float fade_in_beats, float fade_out_beats,
                float xfade_beats,   float gap_beats);

float song_total_beats(const Song *song);
float song_total_seconds(const Song *song);

SongRenderer *song_renderer_new(const Song *song);
void          song_renderer_free(SongRenderer *sr);
int           song_render_block(SongRenderer *sr, float *out_lr, int n_frames);

/* Legacy helpers (delegate to TempoMap internally) */
float    song_bpm_at(const Song *song, float beat);
uint64_t song_beats_to_samples(const Song *song, float start_beat, float n_beats);

#ifdef __cplusplus
}
#endif
