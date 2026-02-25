#pragma once
/*
 * SHMC — TempoMap  (review 3 full implementation)
 *
 * Maps beats ↔ seconds with piecewise tempo segments.
 * Three interpolation modes per segment:
 *
 *   TM_STEP        — constant BPM from b0 to b1 (instant change at b0)
 *   TM_LINEAR_SPB  — SPB = 60/BPM interpolated linearly in seconds-per-beat;
 *                    integral is a quadratic → exact closed-form inverse.
 *                    This is the *recommended* mode: produces intuitive ramps.
 *   TM_LINEAR_BPM  — BPM itself interpolated linearly;
 *                    integral is logarithmic → exp/log inverse.
 *
 * All math in double precision. beat_to_sample uses llround.
 * Values are precomputed per-segment at build time → O(log n) queries.
 */

#include <stdint.h>

#define TEMPO_MAX_PTS  128

typedef enum {
    TM_STEP,         /* instant step to new BPM at control point */
    TM_LINEAR_SPB,   /* linear ramp of SPB=60/BPM from this pt to next */
    TM_LINEAR_BPM    /* linear ramp of BPM from this pt to next */
} TempoInterp;

/* One user-provided control point */
typedef struct {
    double      beat;    /* beat position (must be monotone increasing) */
    double      bpm;     /* BPM at this beat (must be > 0) */
    TempoInterp interp;  /* interpolation TO the *next* segment */
} TempoPoint;

/* One precomputed segment [b0,b1] */
typedef struct {
    double      b0, b1;     /* beat range */
    double      T0;         /* cumulative seconds at b0 */
    double      T1;         /* cumulative seconds at b1 */
    int64_t     smp0;       /* llround(T0 * sr) — set by tempo_map_build() */
    int64_t     smp1;       /* llround(T1 * sr) */
    TempoInterp type;
    /* Per-type constants */
    union {
        struct { double S; }          step;    /* SPB constant */
        struct { double S0, k;  }     lin_spb; /* SPB(b) = S0 + k*(b-b0) */
        struct { double A,  B, den0;} lin_bpm; /* BPM(b) = A*(b-b0)+B;
                                                  den0 = B (BPM at b0) */
    };
} TempoSeg;

typedef struct {
    TempoSeg  segs[TEMPO_MAX_PTS];  /* segments[i] spans [pts[i]..pts[i+1]] */
    int       n_segs;
    double    sr;
    double    last_beat;   /* beat of last control point (end of map) */
    double    last_T;      /* seconds at last_beat */
    double    last_bpm;    /* BPM after last control point (held constant) */
} TempoMap;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build a TempoMap from an array of TempoPoints (must be in beat order).
 * sr: sample rate (used to precompute smp0/smp1).
 * Returns 0 on success, -1 on error (bad pts, n<1, non-monotone, bpm<=0).
 */
int tempo_map_build(TempoMap *m, const TempoPoint *pts, int n, double sr);

/* Convenience: build a constant-tempo map (single BPM throughout) */
void tempo_map_constant(TempoMap *m, double bpm, double sr);

/* beat → seconds (double precision) */
double tempo_beat_to_seconds(const TempoMap *m, double beat);

/* seconds → beat (double precision) */
double tempo_seconds_to_beat(const TempoMap *m, double seconds);

/* beat → sample index (llround) */
int64_t tempo_beat_to_sample(const TempoMap *m, double beat);

/* sample index → beat */
double tempo_sample_to_beat(const TempoMap *m, int64_t sample);

/* Query instantaneous BPM at a beat position */
double tempo_bpm_at(const TempoMap *m, double beat);

/* Duration: beats_to_samples for an interval starting at start_beat */
int64_t tempo_beats_to_samples(const TempoMap *m, double start_beat, double n_beats);

#ifdef __cplusplus
}
#endif
