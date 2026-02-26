/*
 * SHMC Layer 2 — Motif Mutation Operators
 * Verified by verify_motif_mutations.c (all tests must pass before merging).
 */
#include "../include/motif_mutate.h"
#include <string.h>

/* ── helpers ───────────────────────────────────────────────────────── */
static inline int note_count(const VoiceProgram *vp){
    int n=0;
    for(int i=0;i<vp->n;i++) if(VI_OP(vp->code[i])==VI_NOTE) n++;
    return n;
}

/* ── pitch shift ─────────────────────────────────────────────────── */
VoiceProgram motif_mutate_pitch(const VoiceProgram *vp, int semitones){
    VoiceProgram out = *vp;
    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        int p = (int)VI_PITCH(out.code[i]) + semitones;
        p = MIDI_CLAMP(p);
        out.code[i] = VI_PACK(VI_NOTE, (uint8_t)p,
                               VI_DUR(out.code[i]),
                               VI_VEL(out.code[i]));
    }
    return out;
}

/* ── velocity shift ─────────────────────────────────────────────── */
VoiceProgram motif_mutate_velocity(const VoiceProgram *vp, int delta){
    VoiceProgram out = *vp;
    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        int v = (int)VI_VEL(out.code[i]) + delta;
        v = VEL_CLAMP(v);
        out.code[i] = VI_PACK(VI_NOTE,
                               VI_PITCH(out.code[i]),
                               VI_DUR(out.code[i]),
                               (uint8_t)v);
    }
    return out;
}

/* ── duration change ─────────────────────────────────────────────── */
VoiceProgram motif_mutate_duration(const VoiceProgram *vp,
                                    int note_idx, int new_dur_idx){
    VoiceProgram out = *vp;
    int d = DUR_CLAMP(new_dur_idx);
    int ni = 0;
    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        if(ni==note_idx){
            out.code[i] = VI_PACK(VI_NOTE,
                                   VI_PITCH(out.code[i]),
                                   (uint8_t)d,
                                   VI_VEL(out.code[i]));
            break;
        }
        ni++;
    }
    return out;
}

/* ── retrograde ──────────────────────────────────────────────────── */
VoiceProgram motif_mutate_retrograde(const VoiceProgram *vp){
    /* Collect only VI_NOTE entries, then emit in reverse order */
    VInstr notes[VOICE_MAX_INSTRS];
    int nn = 0;
    for(int i=0;i<vp->n;i++)
        if(VI_OP(vp->code[i])==VI_NOTE) notes[nn++]=vp->code[i];

    VoiceProgram out;
    out.n = nn;
    for(int i=0;i<nn;i++) out.code[i] = notes[nn-1-i];
    return out;
}

/* ── inversion ───────────────────────────────────────────────────── */
VoiceProgram motif_mutate_invert(const VoiceProgram *vp){
    VoiceProgram out = *vp;
    /* Find axis: first VI_NOTE pitch */
    int axis = -1;
    for(int i=0;i<vp->n;i++){
        if(VI_OP(vp->code[i])==VI_NOTE){ axis=(int)VI_PITCH(vp->code[i]); break; }
    }
    if(axis<0) return out;  /* no notes — return unchanged */

    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        int p = 2*axis - (int)VI_PITCH(out.code[i]);
        p = MIDI_CLAMP(p);
        out.code[i] = VI_PACK(VI_NOTE, (uint8_t)p,
                               VI_DUR(out.code[i]),
                               VI_VEL(out.code[i]));
    }
    return out;
}

/* ── augmentation (double note durations) ────────────────────────── */
VoiceProgram motif_mutate_augment(const VoiceProgram *vp){
    VoiceProgram out = *vp;
    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        int d = (int)VI_DUR(out.code[i]) + 1;
        d = DUR_CLAMP(d);
        out.code[i] = VI_PACK(VI_NOTE,
                               VI_PITCH(out.code[i]),
                               (uint8_t)d,
                               VI_VEL(out.code[i]));
    }
    return out;
}

/* ── diminution (halve note durations) ───────────────────────────── */
VoiceProgram motif_mutate_diminish(const VoiceProgram *vp){
    VoiceProgram out = *vp;
    for(int i=0;i<out.n;i++){
        if(VI_OP(out.code[i])!=VI_NOTE) continue;
        int d = (int)VI_DUR(out.code[i]) - 1;
        d = DUR_CLAMP(d);
        out.code[i] = VI_PACK(VI_NOTE,
                               VI_PITCH(out.code[i]),
                               (uint8_t)d,
                               VI_VEL(out.code[i]));
    }
    return out;
}
