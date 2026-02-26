#include "../include/song.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ================================================================
   Internal: build a TempoMap from Song's BpmPoint array
   ================================================================ */
static void build_tempo_map(const Song *song, TempoMap *m){
    if(song->n_bpm_points == 0){
        tempo_map_constant(m, (double)song->default_bpm, (double)song->sr);
        return;
    }
    /* Convert BpmPoint[] → TempoPoint[] */
    TempoPoint pts[SONG_MAX_BPM_POINTS];
    for(int i = 0; i < song->n_bpm_points; i++){
        pts[i].beat   = (double)song->bpm_points[i].beat;
        pts[i].bpm    = (double)song->bpm_points[i].bpm;
        pts[i].interp = song->bpm_points[i].interp;
    }
    if(tempo_map_build(m, pts, song->n_bpm_points, (double)song->sr) != 0)
        tempo_map_constant(m, (double)song->default_bpm, (double)song->sr);
}

/* ================================================================
   Song construction
   ================================================================ */

void song_init(Song *song, const char *name, float default_bpm, float sr){
    memset(song, 0, sizeof(*song));
    strncpy(song->name, name, SONG_NAME_LEN - 1);
    song->master_gain  = 1.f;
    song->sr           = sr;
    song->default_bpm  = default_bpm;
}

int song_add_bpm(Song *song, float beat, float bpm){
    /* Default: linear BPM interpolation — matches pre-TempoMap behavior */
    return song_add_bpm_ex(song, beat, bpm, TM_LINEAR_BPM);
}

int song_add_bpm_ex(Song *song, float beat, float bpm, TempoInterp interp){
    if(song->n_bpm_points >= SONG_MAX_BPM_POINTS) return -1;
    if(song->n_bpm_points > 0 &&
       beat <= song->bpm_points[song->n_bpm_points - 1].beat) return -1;
    if(bpm < 1.f) return -1;
    BpmPoint *p = &song->bpm_points[song->n_bpm_points++];
    p->beat   = beat;
    p->bpm    = bpm;
    p->interp = interp;
    return 0;
}

int song_append(Song *song, const char *name,
                const Section *section, const MotifLibrary *lib,
                int repeat,
                float fade_in_beats, float fade_out_beats,
                float xfade_beats,   float gap_beats){
    if(song->n_entries >= SONG_MAX_ENTRIES) return -1;
    SongEntry *e = &song->entries[song->n_entries++];
    strncpy(e->name, name, SONG_NAME_LEN - 1);
    e->name[SONG_NAME_LEN - 1] = '\0';
    e->section        = section;
    e->lib            = lib;
    e->repeat         = repeat < 1 ? 1 : repeat;
    e->fade_in_beats  = fade_in_beats  > 0.f ? fade_in_beats  : 0.f;
    e->fade_out_beats = fade_out_beats > 0.f ? fade_out_beats : 0.f;
    e->xfade_beats    = xfade_beats    > 0.f ? xfade_beats    : 0.f;
    e->gap_beats      = gap_beats      > 0.f ? gap_beats      : 0.f;
    return 0;
}

/* ================================================================
   Legacy BPM helpers (delegate to TempoMap)
   ================================================================ */

float song_bpm_at(const Song *song, float beat){
    TempoMap m; build_tempo_map(song, &m);
    return (float)tempo_bpm_at(&m, (double)beat);
}

uint64_t song_beats_to_samples(const Song *song, float start_beat, float n_beats){
    TempoMap m; build_tempo_map(song, &m);
    return (uint64_t)tempo_beats_to_samples(&m, (double)start_beat, (double)n_beats);
}

float song_total_beats(const Song *song){
    float total = 0.f;
    for(int i = 0; i < song->n_entries; i++){
        const SongEntry *e = &song->entries[i];
        float sec_len = (e->section->length_beats > 0.f)
                      ? e->section->length_beats : 4.f;
        float beats = sec_len * (float)e->repeat;
        if(i > 0) beats -= e->xfade_beats;  /* FIX L4-8 */
        beats += e->gap_beats;
        if(beats < 0.f) beats = 0.f;
        total += beats;
    }
    return total;
}

float song_total_seconds(const Song *song){
    TempoMap m; build_tempo_map(song, &m);
    double total_beats = (double)song_total_beats(song);
    double t1 = tempo_beat_to_seconds(&m, total_beats);
    return (float)t1;
}

/* ================================================================
   Soft clipper: identity for |x| <= 1, smooth knee above
   ================================================================ */
static inline float soft_clip(float x){
    float a = fabsf(x);
    if(a <= 1.f) return x;
    float sign = x > 0.f ? 1.f : -1.f;
    return sign * (1.f + tanhf(a - 1.f) * 0.5f);
}

/* ================================================================
   Renderer
   ================================================================ */

SongRenderer *song_renderer_new(const Song *song){
    SongRenderer *sr = (SongRenderer *)calloc(1, sizeof(SongRenderer));
    if(!sr) return NULL;
    sr->song      = song;
    sr->entry_idx = -1;
    build_tempo_map(song, &sr->tempo);
    return sr;
}

void song_renderer_free(SongRenderer *sr){
    if(!sr) return;
    for(int i = 0; i < 2; i++){
        if(sr->active[i]){
            section_renderer_destroy(sr->active[i]);
            free(sr->active[i]);
            sr->active[i] = NULL;
        }
    }
    free(sr);
}

/* Advance song_beat by n_samples using the tempo map */
static void advance_song_beat(SongRenderer *sr, int n_samples){
    double t0 = sr->sample_pos / (double)sr->song->sr;
    double t1 = (sr->sample_pos + n_samples) / (double)sr->song->sr;
    sr->song_beat = tempo_seconds_to_beat(&sr->tempo, t1);
    (void)t0;
}

/* Compute sample count for a beat duration starting at song_beat */
static int64_t beat_dur_to_samples(const SongRenderer *sr,
                                   double start_beat, double dur_beats){
    return tempo_beats_to_samples(&sr->tempo, start_beat, dur_beats);
}

static int advance_entry(SongRenderer *sr){
    const Song *song = sr->song;
    sr->entry_idx++;
    if(sr->entry_idx >= song->n_entries){ sr->done = 1; return -1; }
    const SongEntry *e = &song->entries[sr->entry_idx];

    /* Crossfade: move current to outgoing slot */
    if(sr->active[0] && e->xfade_beats > 0.f){
        if(sr->active[1]){
            section_renderer_destroy(sr->active[1]);
            free(sr->active[1]);
        }
        sr->active[1] = sr->active[0];
        sr->active[0] = NULL;
        sr->xfade_len = (double)beat_dur_to_samples(sr, sr->song_beat, e->xfade_beats);
        sr->xfade_pos = 0.0;
    } else {
        if(sr->active[0]){
            section_renderer_destroy(sr->active[0]);
            free(sr->active[0]);
            sr->active[0] = NULL;
        }
        sr->xfade_len = 0.0;
        sr->xfade_pos = 0.0;
    }

    /* Allocate and init new section renderer */
    sr->active[0] = (SectionRenderer *)calloc(1, sizeof(SectionRenderer));
    if(!sr->active[0]){ sr->done = 1; return -1; }

    char err[128] = "";
    float bpm_now = (float)tempo_bpm_at(&sr->tempo, sr->song_beat);
    if(section_renderer_init(sr->active[0], e->section, e->lib,
                             song->sr, bpm_now, err, sizeof(err)) < 0){
        fprintf(stderr, "[song] '%s': %s\n", e->name, err);
        free(sr->active[0]); sr->active[0] = NULL;
        sr->done = 1; return -1;
    }

    sr->repeat_left = e->repeat;

    /* R2-1: entry_sample_start tracks entry-relative timing */
    sr->entry_sample_start = sr->sample_pos;

    sr->fade_in_pos = 0.0;
    sr->fade_in_len = (double)beat_dur_to_samples(sr, sr->song_beat, e->fade_in_beats);

    /* Fade-out: offset from entry start */
    float sec_len  = (e->section->length_beats > 0.f
                    ? e->section->length_beats : 4.f) * (float)e->repeat;
    double fo_start_beats = (double)(sec_len - e->fade_out_beats);
    sr->fade_out_start = (double)beat_dur_to_samples(sr, sr->song_beat, fo_start_beats);
    sr->fade_out_len   = (double)beat_dur_to_samples(sr, sr->song_beat, e->fade_out_beats);
    return 0;
}

int song_render_block(SongRenderer *sr, float *out_lr, int n_frames){
    if(sr->done){
        memset(out_lr, 0, (size_t)n_frames * 2 * sizeof(float));
        return 1;
    }
    if(sr->entry_idx < 0){
        if(advance_entry(sr) < 0){
            memset(out_lr, 0, (size_t)n_frames * 2 * sizeof(float));
            return 1;
        }
    }

    /* N5-3: use per-instance scratch buffers (thread-safe) */
    float *l = sr->scratch_l, *r = sr->scratch_r;
    float *xl = sr->scratch_xl, *xr = sr->scratch_xr;
    const Song *song = sr->song;

    int pos = 0;
    while(pos < n_frames){
        int c = n_frames - pos;
        if(c > 4096) c = 4096;

        memset(l,  0, (size_t)c * sizeof(float));
        memset(r,  0, (size_t)c * sizeof(float));

        /* Render current section */
        if(sr->active[0])
            section_render_block_mix(sr->active[0], l, r, c);

        /* FIX L4-1: render outgoing BEFORE the gain loop */
        if(sr->active[1]){
            memset(xl, 0, (size_t)c * sizeof(float));
            memset(xr, 0, (size_t)c * sizeof(float));
            section_render_block_mix(sr->active[1], xl, xr, c);
        }

        /* Per-sample gain envelope */
        for(int s = 0; s < c; s++){
            float g = song->master_gain;

            /* Crossfade incoming ramp */
            if(sr->xfade_len > 0.0){
                float t = (float)(sr->xfade_pos / sr->xfade_len);
                if(t > 1.f) t = 1.f;
                g *= t;
            }
            /* Fade-in */
            if(sr->fade_in_len > 0.0 && sr->fade_in_pos < sr->fade_in_len){
                g *= (float)(sr->fade_in_pos / sr->fade_in_len);
                sr->fade_in_pos += 1.0;
            }
            /* FIX L4-2: fade-out relative to entry start */
            double entry_elapsed = sr->sample_pos - sr->entry_sample_start
                                   - sr->fade_out_start;
            if(sr->fade_out_len > 0.0 && entry_elapsed >= 0.0){
                float fo = (float)(1.0 - entry_elapsed / sr->fade_out_len);
                if(fo < 0.f) fo = 0.f;
                g *= fo;
            }

            l[s] *= g;
            r[s] *= g;

            /* Mix outgoing crossfade */
            if(sr->active[1] && sr->xfade_len > 0.0){
                float t_out = (float)(sr->xfade_pos / sr->xfade_len);
                if(t_out > 1.f) t_out = 1.f;
                float gout = song->master_gain * (1.f - t_out);
                l[s] += xl[s] * gout;
                r[s] += xr[s] * gout;
            }

            sr->sample_pos += 1.0;
            sr->xfade_pos  += 1.0;
        }

        /* Clean up outgoing when xfade done */
        if(sr->active[1] && sr->xfade_pos >= sr->xfade_len){
            section_renderer_destroy(sr->active[1]);
            free(sr->active[1]);
            sr->active[1] = NULL;
            sr->xfade_len = 0.0;
        }

        /* Soft clip + interleave */
        for(int s = 0; s < c; s++){
            out_lr[(pos+s)*2    ] = soft_clip(l[s]);
            out_lr[(pos+s)*2+1  ] = soft_clip(r[s]);
        }

        /* FIX L4-3: advance song_beat using tempo map */
        advance_song_beat(sr, c);

        /* Advance entry when current section finishes */
        if(sr->active[0] && sr->active[0]->done){
            sr->repeat_left--;
            if(sr->repeat_left > 0){
                /* R2-6: use reset instead of destroy+init */
                float bpm_now = (float)tempo_bpm_at(&sr->tempo, sr->song_beat);
                section_renderer_reset(sr->active[0], bpm_now);
                sr->entry_sample_start = sr->sample_pos;
                const SongEntry *e = &song->entries[sr->entry_idx];
                float sec_len = (e->section->length_beats > 0.f
                               ? e->section->length_beats : 4.f);
                double fo_start = (double)(sec_len - e->fade_out_beats);
                sr->fade_out_start = (double)beat_dur_to_samples(sr, sr->song_beat, fo_start);
            } else {
                pos += c;
                if(advance_entry(sr) < 0){
                    /* FIX L4-10: zero tail */
                    if(pos < n_frames)
                        memset(out_lr + pos*2, 0,
                               (size_t)(n_frames - pos) * 2 * sizeof(float));
                    return 1;
                }
                continue;
            }
        }
        pos += c;
    }
    return sr->done ? 1 : 0;
}
