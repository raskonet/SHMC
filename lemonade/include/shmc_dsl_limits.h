#pragma once
/*
 * SHMC DSL — LLM Output Validation Limits
 *
 * These bounds prevent pathological LLM-generated programs from:
 *   - exhausting memory (unbounded sections)
 *   - hanging the renderer (infinite loops via repeat)
 *   - producing inaudible content (0 notes)
 *   - producing clipped/distorted content (too many layers)
 *
 * Each limit is independently verifiable by test_dsl_limits.py.
 * Change these constants only with matching test updates.
 */

/* Per-PATCH limits */
#define DSL_LIMIT_MAX_PATCH_OPS      32   /* max ops inside a PATCH block */
#define DSL_LIMIT_MAX_PATCHES        16   /* max PATCH declarations per file */

/* Per-MOTIF limits */
#define DSL_LIMIT_MAX_NOTES_PER_MOTIF  64   /* max notes inside a MOTIF */
#define DSL_LIMIT_MIDI_MIN             21   /* A0 */
#define DSL_LIMIT_MIDI_MAX            108   /* C8 */
#define DSL_LIMIT_DUR_MAX               6   /* 1 full beat max */
#define DSL_LIMIT_VEL_MAX              15

/* Per-SECTION limits */
#define DSL_LIMIT_MAX_USES_PER_SECTION 64   /* max 'use' lines */
#define DSL_LIMIT_MAX_SECTION_BEATS   256.f /* 256 beats = ~2min at 120BPM */
#define DSL_LIMIT_MAX_REPEAT           64   /* xN repeat cap */
#define DSL_LIMIT_TRANSPOSE_MIN       -24   /* -2 octaves */
#define DSL_LIMIT_TRANSPOSE_MAX        24   /* +2 octaves */
#define DSL_LIMIT_VEL_SCALE_MIN       0.0f
#define DSL_LIMIT_VEL_SCALE_MAX       4.0f
#define DSL_LIMIT_TIME_SCALE_MIN      0.25f
#define DSL_LIMIT_TIME_SCALE_MAX      4.0f

/* Per-SONG limits */
#define DSL_LIMIT_MAX_PLAYS_PER_SONG   32   /* max 'play' lines */
#define DSL_LIMIT_MAX_SONG_REPEAT      16   /* song-level xN */
#define DSL_LIMIT_BPM_MIN             20.f
#define DSL_LIMIT_BPM_MAX            300.f

/* Global */
#define DSL_LIMIT_MAX_TOTAL_BEATS   4096.f  /* total song length cap */
