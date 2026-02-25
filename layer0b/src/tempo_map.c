/*
 * SHMC — TempoMap implementation
 * All math in double; sample indices via llround.
 */
#include "../include/tempo_map.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define MIN_BPM 1.0
#define MAX_BPM 9999.0

/* ----------------------------------------------------------------
   Per-segment beat → seconds
   ---------------------------------------------------------------- */
static double seg_b2t(const TempoSeg *s, double b){
    double x = b - s->b0;
    switch(s->type){
    case TM_STEP:
        return s->T0 + s->step.S * x;
    case TM_LINEAR_SPB:
        return s->T0 + s->lin_spb.S0 * x + 0.5 * s->lin_spb.k * x * x;
    case TM_LINEAR_BPM: {
        double A = s->lin_bpm.A, den0 = s->lin_bpm.den0;
        if(fabs(A) < 1e-12)  /* constant BPM fallback */
            return s->T0 + (60.0 / den0) * x;
        return s->T0 + (60.0 / A) * log((A * x + den0) / den0);
    }
    }
    return s->T0;
}

/* Per-segment seconds → beat (inverse of above) */
static double seg_t2b(const TempoSeg *s, double t){
    double dt = t - s->T0;
    switch(s->type){
    case TM_STEP:
        return s->b0 + (s->step.S > 1e-20 ? dt / s->step.S : 0.0);
    case TM_LINEAR_SPB: {
        double k = s->lin_spb.k, S0 = s->lin_spb.S0;
        if(fabs(k) < 1e-12)
            return s->b0 + (S0 > 1e-20 ? dt / S0 : 0.0);
        /* Solve: 0.5*k*x^2 + S0*x - dt = 0 */
        double disc = S0 * S0 + 2.0 * k * dt;
        if(disc < 0.0) disc = 0.0;
        /* Choose root that gives positive x when dt>0 */
        double x = (-S0 + sqrt(disc)) / k;
        return s->b0 + x;
    }
    case TM_LINEAR_BPM: {
        double A = s->lin_bpm.A, den0 = s->lin_bpm.den0;
        if(fabs(A) < 1e-12)
            return s->b0 + (60.0 / den0) * dt;
        /* Clamp exponent to avoid overflow */
        double exponent = (A / 60.0) * dt;
        if(exponent >  50.0) exponent =  50.0;
        if(exponent < -50.0) exponent = -50.0;
        return s->b0 + (den0 * exp(exponent) - den0) / A;
    }
    }
    return s->b0;
}

/* Find segment containing beat b (binary search) */
static const TempoSeg *find_seg_beat(const TempoMap *m, double b){
    /* Fast path: last segment */
    int lo = 0, hi = m->n_segs - 1;
    while(lo < hi){
        int mid = (lo + hi) / 2;
        if(m->segs[mid].b1 <= b) lo = mid + 1;
        else                     hi = mid;
    }
    return &m->segs[lo];
}

/* Find segment containing seconds t */
static const TempoSeg *find_seg_time(const TempoMap *m, double t){
    int lo = 0, hi = m->n_segs - 1;
    while(lo < hi){
        int mid = (lo + hi) / 2;
        if(m->segs[mid].T1 <= t) lo = mid + 1;
        else                     hi = mid;
    }
    return &m->segs[lo];
}

/* ----------------------------------------------------------------
   Public API
   ---------------------------------------------------------------- */

int tempo_map_build(TempoMap *m, const TempoPoint *pts, int n, double sr){
    if(!m || !pts || n < 1 || sr <= 0.0) return -1;
    memset(m, 0, sizeof(*m));
    m->sr = sr;

    /* Validate and extend: add a sentinel at end if needed */
    for(int i = 0; i < n; i++){
        if(i > 0 && pts[i].beat <= pts[i-1].beat) return -1; /* non-monotone */
        if(pts[i].bpm < MIN_BPM || pts[i].bpm > MAX_BPM)     return -1;
    }

    int n_segs = n - 1;   /* need at least 2 points to have 1 segment */
    if(n_segs < 1){
        /* Single point: entire map is constant at pts[0].bpm */
        tempo_map_constant(m, pts[0].bpm, sr);
        return 0;
    }
    if(n_segs > TEMPO_MAX_PTS - 1) n_segs = TEMPO_MAX_PTS - 1;

    double T_acc = 0.0;
    for(int i = 0; i < n_segs; i++){
        TempoSeg *s = &m->segs[i];
        double b0 = pts[i].beat, b1 = pts[i+1].beat;
        double bpm0 = pts[i].bpm, bpm1 = pts[i+1].bpm;

        s->b0 = b0; s->b1 = b1; s->T0 = T_acc;
        s->type = pts[i].interp;

        double S0 = 60.0 / bpm0, S1 = 60.0 / bpm1;
        double db = b1 - b0;

        switch(s->type){
        case TM_STEP:
            s->step.S = 60.0 / bpm0;
            s->T1 = T_acc + s->step.S * db;
            break;
        case TM_LINEAR_SPB:
            s->lin_spb.S0 = S0;
            s->lin_spb.k  = (S1 - S0) / db;
            s->T1 = T_acc + S0 * db + 0.5 * s->lin_spb.k * db * db;
            break;
        case TM_LINEAR_BPM: {
            double A = (bpm1 - bpm0) / db;  /* BPM slope in beats */
            s->lin_bpm.A    = A;
            s->lin_bpm.B    = bpm0;
            s->lin_bpm.den0 = bpm0;
            if(fabs(A) < 1e-12)
                s->T1 = T_acc + S0 * db;
            else
                s->T1 = T_acc + (60.0 / A) * log((A * db + bpm0) / bpm0);
            break;
        }
        }

        s->smp0 = llround(s->T0 * sr);
        s->smp1 = llround(s->T1 * sr);
        T_acc = s->T1;
    }

    m->n_segs    = n_segs;
    m->last_beat = pts[n-1].beat;
    m->last_T    = T_acc;
    m->last_bpm  = pts[n-1].bpm;
    return 0;
}

void tempo_map_constant(TempoMap *m, double bpm, double sr){
    if(bpm < MIN_BPM) bpm = MIN_BPM;
    memset(m, 0, sizeof(*m));
    m->sr = sr;
    /* Single segment spanning [0, 1e15] */
    TempoSeg *s = &m->segs[0];
    s->b0 = 0.0; s->b1 = 1e15;
    s->T0 = 0.0; s->T1 = 1e15 * (60.0 / bpm);
    s->smp0 = 0;
    s->smp1 = llround(s->T1 * sr);
    s->type   = TM_STEP;
    s->step.S = 60.0 / bpm;
    m->n_segs    = 1;
    m->last_beat = 1e15;
    m->last_T    = s->T1;
    m->last_bpm  = bpm;
}

double tempo_beat_to_seconds(const TempoMap *m, double beat){
    if(m->n_segs == 0) return beat * 0.5; /* degenerate */
    /* Before map */
    if(beat <= m->segs[0].b0)
        return m->segs[0].T0 + (beat - m->segs[0].b0) * m->segs[0].step.S;
    /* After map: hold last BPM */
    if(beat >= m->last_beat)
        return m->last_T + (beat - m->last_beat) * (60.0 / m->last_bpm);
    const TempoSeg *s = find_seg_beat(m, beat);
    return seg_b2t(s, beat);
}

double tempo_seconds_to_beat(const TempoMap *m, double t){
    if(m->n_segs == 0) return t * 2.0;
    if(t <= m->segs[0].T0){
        double S = m->segs[0].step.S;
        return m->segs[0].b0 + (S > 1e-20 ? (t - m->segs[0].T0) / S : 0.0);
    }
    if(t >= m->last_T){
        double S = 60.0 / m->last_bpm;
        return m->last_beat + (S > 1e-20 ? (t - m->last_T) / S : 0.0);
    }
    const TempoSeg *s = find_seg_time(m, t);
    return seg_t2b(s, t);
}

int64_t tempo_beat_to_sample(const TempoMap *m, double beat){
    return llround(tempo_beat_to_seconds(m, beat) * m->sr);
}

double tempo_sample_to_beat(const TempoMap *m, int64_t sample){
    return tempo_seconds_to_beat(m, (double)sample / m->sr);
}

double tempo_bpm_at(const TempoMap *m, double beat){
    if(m->n_segs == 0) return 120.0;
    if(beat >= m->last_beat) return m->last_bpm;
    const TempoSeg *s = find_seg_beat(m, beat);
    double x = beat - s->b0;
    switch(s->type){
    case TM_STEP:
        return 60.0 / s->step.S;
    case TM_LINEAR_SPB: {
        double spb = s->lin_spb.S0 + s->lin_spb.k * x;
        return spb > 1e-20 ? 60.0 / spb : MAX_BPM;
    }
    case TM_LINEAR_BPM:
        return s->lin_bpm.A * x + s->lin_bpm.den0;
    }
    return 120.0;
}

int64_t tempo_beats_to_samples(const TempoMap *m, double start_beat, double n_beats){
    double t0 = tempo_beat_to_seconds(m, start_beat);
    double t1 = tempo_beat_to_seconds(m, start_beat + n_beats);
    return llround((t1 - t0) * m->sr);
}
