#include "../include/motif.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void motif_lib_init(MotifLibrary *lib){
    memset(lib, 0, sizeof(*lib));
}

int motif_define(MotifLibrary *lib, const char *name, const VoiceProgram *vp){
    if(lib->n >= MOTIF_LIB_MAX) return -1;
    for(int i = 0; i < lib->n; i++)
        if(strncmp(lib->entries[i].name, name, MOTIF_NAME_LEN) == 0) return -1;
    Motif *m = &lib->entries[lib->n++];
    strncpy(m->name, name, MOTIF_NAME_LEN - 1);
    m->name[MOTIF_NAME_LEN - 1] = '\0';
    m->vp    = *vp;
    m->valid = 1;
    return 0;
}

const Motif *motif_find(const MotifLibrary *lib, const char *name){
    for(int i = 0; i < lib->n; i++)
        if(lib->entries[i].valid &&
           strncmp(lib->entries[i].name, name, MOTIF_NAME_LEN) == 0)
            return &lib->entries[i];
    return NULL;
}

void motif_transpose(const VoiceProgram *src, VoiceProgram *dst, int semitones){
    *dst = *src;
    for(int i = 0; i < dst->n; i++){
        VInstr  vi = dst->code[i];
        uint8_t op = VI_OP(vi);
        if(op == VI_NOTE || op == VI_GLIDE){
            int p = (int)VI_PITCH(vi) + semitones;
            if(p < 0)   p = 0;
            if(p > 127) p = 127;
            dst->code[i] = VI_PACK(op, (uint8_t)p, VI_DUR(vi), VI_VEL(vi));
        }
    }
}

/*
 * FIX L2-2: Tiebreak for events at identical sample positions.
 *
 * Old code:  return (int)ea->type - (int)eb->type
 *   EV_NOTE_ON=0, EV_NOTE_OFF=1  →  0-1 = -1  →  NOTE_ON sorted FIRST
 *   This means at a repeat boundary the new note fires before the old one
 *   releases, causing a pop/click.
 *
 * Fixed:     return (int)eb->type - (int)ea->type
 *   1-0 = +1  →  NOTE_OFF sorted FIRST  →  release before retrigger. Correct.
 *   EV_GLIDE=2 sorts last (after NOTE_OFF and NOTE_ON), which is also correct:
 *   GLIDE events are processed before their matching NOTE_ON (same sample) by
 *   the renderer's sequential event dispatch, so ordering GLIDE after NOTE_OFF
 *   but before the next block is fine because they share sample position with
 *   the following NOTE_ON which sorts after them anyway.
 */
static int ev_cmp(const void *a, const void *b){
    const Event *ea = a, *eb = b;
    if(ea->sample < eb->sample) return -1;
    if(ea->sample > eb->sample) return  1;
    /* FIX L2-2: NOTE_OFF before NOTE_ON at same sample */
    return (int)eb->type - (int)ea->type;
}

int motif_compile_uses(const MotifLibrary *lib,
                       const MotifUse *uses, int n_uses,
                       EventStream *out,
                       float sr, float bpm,
                       char *err, int err_sz){
    if(!err || err_sz <= 0){ static char _e[64]; err = _e; err_sz = 64; }
    err[0] = '\0';
    memset(out, 0, sizeof(*out));

    float    spb   = sr * 60.f / bpm;
    uint64_t total = 0;

    for(int u = 0; u < n_uses; u++){
        const MotifUse *use = &uses[u];
        const Motif    *m   = motif_find(lib, use->name);
        if(!m){ snprintf(err, err_sz, "motif '%s' not found", use->name); return -1; }

        VoiceProgram        transposed;
        const VoiceProgram *vp;
        if(use->transpose != 0){
            motif_transpose(&m->vp, &transposed, use->transpose);
            vp = &transposed;
        } else {
            vp = &m->vp;
        }

        EventStream tmp;
        char sub_err[128] = "";
        if(voice_compile(vp, &tmp, sr, bpm, sub_err, sizeof(sub_err)) < 0){
            snprintf(err, err_sz, "motif '%s' compile: %s", use->name, sub_err);
            return -1;
        }

        uint64_t start_sample = (uint64_t)(use->start_beat * spb + 0.5f);
        uint64_t offset       = start_sample;

        /* N5-2: compute transform parameters (clamp to safe range) */
        float ts = (use->time_scale > 0.f) ? use->time_scale : 1.f;
        float vs = (use->vel_scale  > 0.f) ? use->vel_scale  : 1.f;
        if(vs > 2.f) vs = 2.f;

        /* Scaled repeat duration: each repeat is total_samples * ts long */
        uint64_t scaled_repeat_dur = (uint64_t)(tmp.total_samples * ts + 0.5f);
        if(scaled_repeat_dur == 0) scaled_repeat_dur = tmp.total_samples;

        for(int r = 0; r < use->repeat; r++){
            for(int e = 0; e < tmp.n; e++){
                if(out->n >= VOICE_MAX_EVENTS){
                    snprintf(err, err_sz, "event overflow"); return -1;
                }
                Event ev = tmp.events[e];
                /* N5-2 time_scale: stretch sample position within this repeat */
                uint64_t local_smp = (uint64_t)(ev.sample * ts + 0.5f);
                ev.sample   = offset + local_smp;
                /* N5-2 vel_scale: scale velocity, clamp to [0,1] */
                float v = ev.velocity * vs;
                ev.velocity = v > 1.f ? 1.f : v < 0.f ? 0.f : v;
                out->events[out->n++] = ev;
            }
            offset += scaled_repeat_dur;
        }

        uint64_t end = start_sample + (uint64_t)use->repeat * scaled_repeat_dur;
        if(end > total) total = end;
    }

    /* Sort: primary key = sample, tiebreak = NOTE_OFF before NOTE_ON (FIX L2-2) */
    qsort(out->events, out->n, sizeof(Event), ev_cmp);
    out->total_samples = total;
    out->total_beats   = (float)total / spb;
    return 0;
}

/* R2-4: Pre-resolve MotifUse name strings → pointers */
int motif_resolve_uses(const MotifLibrary *lib,
                       MotifUse *uses, int n_uses,
                       char *err, int err_sz){
    if(!err || err_sz <= 0){ static char _e[128]; err = _e; err_sz = 128; }
    for(int i = 0; i < n_uses; i++){
        const Motif *m = motif_find(lib, uses[i].name);
        if(!m){
            snprintf(err, err_sz,
                     "motif_resolve_uses: '%s' not found in library", uses[i].name);
            return -1;
        }
        uses[i].resolved_motif = m;
    }
    return 0;
}
