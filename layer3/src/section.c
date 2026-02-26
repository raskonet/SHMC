#include "../include/section.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <assert.h>

void section_init(Section *s, const char *name, float length_beats){
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, SECTION_NAME_LEN - 1);
    s->length_beats = length_beats;
}

int section_add_track(Section *s, const char *name,
                      const PatchProgram *patch,
                      const MotifUse *uses, int n_uses,
                      float gain, float pan){
    if(s->n_tracks >= SECTION_MAX_TRACKS) return -1;
    if(n_uses > SECTION_MAX_USES)         return -1;
    SectionTrack *t = &s->tracks[s->n_tracks++];
    strncpy(t->name, name, SECTION_NAME_LEN - 1);
    t->name[SECTION_NAME_LEN - 1] = '\0';
    t->patch  = patch;
    t->n_uses = n_uses;
    memcpy(t->uses, uses, (size_t)n_uses * sizeof(MotifUse));
    t->gain = gain < 0.f ? 0.f : gain > 1.f ? 1.f : gain;
    t->pan  = pan  < -1.f ? -1.f : pan > 1.f ? 1.f : pan;
    return 0;
}

/* Equal-power pan: angle sweeps 0..π/2 as pan goes -1..+1 */
static void calc_pan(float pan, float *pl, float *pr){
    float angle = (pan + 1.f) * 0.25f * 3.14159265f;
    *pl = cosf(angle);
    *pr = sinf(angle);
}

int section_renderer_init(SectionRenderer *sr,
                          const Section *section,
                          const MotifLibrary *lib,
                          float sample_rate, float bpm,
                          char *err, int err_sz){
    if(!err || err_sz <= 0){ static char _e[128]; err = _e; err_sz = 128; }
    err[0] = '\0';
    memset(sr, 0, sizeof(*sr));
    sr->section  = section;
    sr->sr       = sample_rate;
    sr->bpm      = bpm;
    sr->n_tracks = section->n_tracks;

    for(int t = 0; t < section->n_tracks; t++){
        const SectionTrack *st = &section->tracks[t];
        TrackState         *ts = &sr->tracks[t];

        /* FIX L3-3: heap-allocate EventStream, not embedded */
        ts->es = (EventStream *)malloc(sizeof(EventStream));
        if(!ts->es){
            snprintf(err, err_sz, "OOM allocating EventStream for track '%s'", st->name);
            /* Free already-allocated ones */
            for(int k = 0; k < t; k++){ free(sr->tracks[k].es); sr->tracks[k].es = NULL; }
            return -1;
        }

        char sub_err[128] = "";
        int r = motif_compile_uses(lib, st->uses, st->n_uses,
                                   ts->es, sample_rate, bpm,
                                   sub_err, sizeof(sub_err));
        if(r < 0){
            snprintf(err, err_sz, "track '%s': %s", st->name, sub_err);
            for(int k = 0; k <= t; k++){ free(sr->tracks[k].es); sr->tracks[k].es = NULL; }
            return -1;
        }

        voice_renderer_init(&ts->vr, ts->es, st->patch, sample_rate);
        ts->gain = st->gain;
        calc_pan(st->pan, &ts->pan_l, &ts->pan_r);
    }
    return 0;
}

void section_renderer_destroy(SectionRenderer *sr){
    for(int t = 0; t < sr->n_tracks; t++){
        if(sr->tracks[t].es){ free(sr->tracks[t].es); sr->tracks[t].es = NULL; }
    }
}

/* Internal: render all tracks into out_l/out_r, adding to whatever's there */
static int render_into(SectionRenderer *sr,
                        float *out_l, float *out_r, int n){
    /* FIX L3-1: early-out if already finished */
    if(sr->done) return 1;

    /* N5-3: use per-instance buffer instead of static (thread-safe) */
    float *mono = sr->scratch_mono;

    int all_done = 1;
    for(int t = 0; t < sr->n_tracks; t++){
        TrackState *ts = &sr->tracks[t];
        if(ts->vr.done) continue;
        all_done = 0;

        int pos = 0;
        while(pos < n){
            int c = n - pos;
            if(c > SECTION_BLOCK_MAX) c = SECTION_BLOCK_MAX;
            voice_render_block(&ts->vr, mono, c);
            float g = ts->gain, pl = ts->pan_l, pr = ts->pan_r;
            for(int s = 0; s < c; s++){
                float v = mono[s] * g;
                out_l[pos + s] += v * pl;
                out_r[pos + s] += v * pr;
            }
            pos += c;
        }
    }

    /* Re-check done state after rendering */
    if(!all_done){
        all_done = 1;
        for(int t = 0; t < sr->n_tracks; t++)
            if(!sr->tracks[t].vr.done){ all_done = 0; break; }
    }
    if(all_done) sr->done = 1;
    return all_done;
}

int section_render_block(SectionRenderer *sr,
                         float *out_l, float *out_r, int n){
    /* FIX L3-7: document overwrite semantics; guard against aliasing */
    assert(out_l != out_r && "out_l and out_r must be different buffers");
    memset(out_l, 0, (size_t)n * sizeof(float));
    memset(out_r, 0, (size_t)n * sizeof(float));
    return render_into(sr, out_l, out_r, n);
}

int section_render_block_mix(SectionRenderer *sr,
                              float *out_l, float *out_r, int n){
    /* Adds to existing content — no memset */
    assert(out_l != out_r && "out_l and out_r must be different buffers");
    return render_into(sr, out_l, out_r, n);
}

int section_render_interleaved(SectionRenderer *sr, float *out_lr, int n_frames){
    /* N5-3: per-instance scratch buffers */
    float *l = sr->scratch_l, *r = sr->scratch_r;
    int pos = 0;
    while(pos < n_frames){
        int c = n_frames - pos;
        if(c > SECTION_BLOCK_MAX) c = SECTION_BLOCK_MAX;
        memset(l, 0, (size_t)c * sizeof(float));
        memset(r, 0, (size_t)c * sizeof(float));
        render_into(sr, l, r, c);
        for(int s = 0; s < c; s++){
            out_lr[(pos + s) * 2    ] = l[s];
            out_lr[(pos + s) * 2 + 1] = r[s];
        }
        pos += c;
    }
    return sr->done;
}

/* R2-6: Reset playback head without free/realloc */
void section_renderer_reset(SectionRenderer *sr, float bpm){
    if(!sr) return;
    sr->done = 0;
    sr->bpm  = bpm;
    for(int t = 0; t < sr->n_tracks; t++){
        TrackState *ts = &sr->tracks[t];
        if(!ts->es) continue;
        /* Re-init voice renderer from the same (already compiled) EventStream */
        const SectionTrack *st = &sr->section->tracks[t];
        voice_renderer_init(&ts->vr, ts->es, st->patch, sr->sr);
    }
}

/* ================================================================
   T4-4 / T4-5: section_validate
   ================================================================ */
int section_validate(const Section *s, const MotifLibrary *lib,
                     float sr, float bpm,
                     SectionLenValidate mode, int max_events_per_track,
                     char *err, int err_sz){
    if(!err || err_sz <= 0){ static char _e[256]; err=_e; err_sz=256; }
    err[0] = '\0';
    if(max_events_per_track <= 0) max_events_per_track = SECTION_MAX_EVENTS_PER_TRACK;

    for(int t = 0; t < s->n_tracks; t++){
        const SectionTrack *st = &s->tracks[t];

        /* T4-4: Estimate total events = 2 * total_notes across all uses */
        int total_est = 0;
        for(int u = 0; u < st->n_uses; u++){
            const MotifUse *mu = &st->uses[u];
            const Motif *m = mu->resolved_motif
                           ? mu->resolved_motif
                           : motif_find(lib, mu->name);
            if(!m) continue;
            /* Count NOTE instructions in the VoiceProgram (rough estimate) */
            int notes = 0;
            for(int k = 0; k < m->vp.n; k++)
                if(VI_OP(m->vp.code[k]) == VI_NOTE) notes++;
            total_est += notes * 2 * mu->repeat; /* ×2 for on+off */
        }
        if(total_est > max_events_per_track){
            snprintf(err, err_sz,
                "track '%s': estimated %d events > budget %d (T4-4)",
                st->name, total_est, max_events_per_track);
            return -1;
        }

        /* T4-5: Check motif end time vs. section length */
        if(mode != SLV_IGNORE && s->length_beats > 0.f){
            for(int u = 0; u < st->n_uses; u++){
                const MotifUse *mu = &st->uses[u];
                const Motif *m = mu->resolved_motif
                               ? mu->resolved_motif
                               : motif_find(lib, mu->name);
                if(!m) continue;

                /* Rough duration: compile to count beats */
                EventStream es; char sub[64]="";
                voice_compile(&m->vp, &es, sr, bpm, sub, sizeof(sub));
                float motif_end = mu->start_beat + es.total_beats * (float)mu->repeat;

                if(motif_end > s->length_beats + 0.01f){
                    if(mode == SLV_ERROR){
                        snprintf(err, err_sz,
                            "track '%s' motif '%s' ends at beat %.2f > section length %.2f (T4-5)",
                            st->name, mu->name, motif_end, s->length_beats);
                        return -1;
                    } else { /* SLV_WARN */
                        fprintf(stderr,
                            "[section_validate] WARN track '%s' motif '%s' "
                            "ends %.2f > section %.2f\n",
                            st->name, mu->name, motif_end, s->length_beats);
                    }
                }
            }
        }
    }
    return 0;
}
