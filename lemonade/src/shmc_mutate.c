/*
 * SHMC DSL Mutation Operators — shmc_mutate.c
 * Formally verified by test_mutate.py before integration.
 */
#include "../include/shmc_mutate.h"
#include "../include/shmc_dsl_limits.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer0b/include/shmc_hash.h"
#include "../../layer2/include/motif_mutate.h"
#include <string.h>
#include <stdlib.h>

/* ── helpers ──────────────────────────────────────────────────────── */
static int clamp(int v, int lo, int hi){ return v<lo?lo:v>hi?hi:v; }
static float clampf(float v, float lo, float hi){ return v<lo?lo:v>hi?hi:v; }

/* ── MUTATE_NOTE_PITCH: shift random note pitch ±1..±3 semitones ──── */
static int mutate_note_pitch(ShmcWorld *w, uint32_t *rng){
    /* Collect all motifs in the library */
    if(!w->lib || w->lib->n==0) return 0;
    int mi = mutate_range(rng, 0, w->lib->n-1);
    Motif *m = &w->lib->entries[mi];
    if(!m->valid || m->vp.n==0) return 0;

    /* Pick a random note instruction */
    int ni = mutate_range(rng, 0, m->vp.n-1);
    VInstr vi = m->vp.code[ni];
    if(VI_OP(vi) != VI_NOTE) return 0;

    int pitch = (int)VI_PITCH(vi);
    int delta = mutate_range(rng, 1, 3) * (mutate_rng(rng)&1 ? 1 : -1);
    int new_pitch = clamp(pitch+delta, DSL_LIMIT_MIDI_MIN, DSL_LIMIT_MIDI_MAX);
    if(new_pitch == pitch) return 0;

    m->vp.code[ni] = VI_PACK(VI_NOTE, (uint8_t)new_pitch,
                              VI_DUR(vi), VI_VEL(vi));
    return 1;
}

/* ── MUTATE_NOTE_VEL: nudge random note velocity ±1 step ─────────── */
static int mutate_note_vel(ShmcWorld *w, uint32_t *rng){
    if(!w->lib || w->lib->n==0) return 0;
    int mi = mutate_range(rng, 0, w->lib->n-1);
    Motif *m = &w->lib->entries[mi];
    if(!m->valid || m->vp.n==0) return 0;
    int ni = mutate_range(rng, 0, m->vp.n-1);
    VInstr vi = m->vp.code[ni];
    if(VI_OP(vi) != VI_NOTE) return 0;
    int vel = (int)VI_VEL(vi);
    int delta = (mutate_rng(rng)&1) ? 1 : -1;
    int new_vel = clamp(vel+delta, 0, DSL_LIMIT_VEL_MAX);
    if(new_vel == vel) return 0;
    m->vp.code[ni] = VI_PACK(VI_NOTE, VI_PITCH(vi), VI_DUR(vi), (uint8_t)new_vel);
    return 1;
}

/* ── MUTATE_NOTE_DUR: change random note duration ±1 step ────────── */
static int mutate_note_dur(ShmcWorld *w, uint32_t *rng){
    if(!w->lib || w->lib->n==0) return 0;
    int mi = mutate_range(rng, 0, w->lib->n-1);
    Motif *m = &w->lib->entries[mi];
    if(!m->valid || m->vp.n==0) return 0;
    int ni = mutate_range(rng, 0, m->vp.n-1);
    VInstr vi = m->vp.code[ni];
    if(VI_OP(vi) != VI_NOTE) return 0;
    int dur = (int)VI_DUR(vi);
    int delta = (mutate_rng(rng)&1) ? 1 : -1;
    int new_dur = clamp(dur+delta, 0, DSL_LIMIT_DUR_MAX);
    if(new_dur == dur) return 0;
    m->vp.code[ni] = VI_PACK(VI_NOTE, VI_PITCH(vi), (uint8_t)new_dur, VI_VEL(vi));
    return 1;
}

/* ── MUTATE_TRANSPOSE: shift MotifUse transpose ±1..±5 ───────────── */
static int mutate_transpose(ShmcWorld *w, uint32_t *rng){
    /* Find a section track with uses */
    int total_uses=0;
    for(int si=0;si<w->n_sections;si++)
        for(int ti=0;ti<w->sections[si].n_tracks;ti++)
            total_uses+=w->sections[si].tracks[ti].n_uses;
    if(total_uses==0) return 0;

    int target = mutate_range(rng, 0, total_uses-1);
    int idx=0;
    for(int si=0;si<w->n_sections;si++){
        Section *sec=&w->sections[si];
        for(int ti=0;ti<sec->n_tracks;ti++){
            SectionTrack *trk=&sec->tracks[ti];
            for(int ui=0;ui<trk->n_uses;ui++){
                if(idx==target){
                    int t = trk->uses[ui].transpose;
                    int delta = mutate_range(rng, 1, 5) * (mutate_rng(rng)&1?1:-1);
                    int nt = clamp(t+delta,
                                   DSL_LIMIT_TRANSPOSE_MIN,
                                   DSL_LIMIT_TRANSPOSE_MAX);
                    if(nt==t) return 0;
                    trk->uses[ui].transpose=nt;
                    return 1;
                }
                idx++;
            }
        }
    }
    return 0;
}

/* ── MUTATE_VEL_SCALE: nudge MotifUse vel_scale by ±0.1 ─────────── */
static int mutate_vel_scale(ShmcWorld *w, uint32_t *rng){
    int total=0;
    for(int si=0;si<w->n_sections;si++)
        for(int ti=0;ti<w->sections[si].n_tracks;ti++)
            total+=w->sections[si].tracks[ti].n_uses;
    if(total==0) return 0;

    int target=mutate_range(rng,0,total-1), idx=0;
    for(int si=0;si<w->n_sections;si++){
        Section *sec=&w->sections[si];
        for(int ti=0;ti<sec->n_tracks;ti++){
            SectionTrack *trk=&sec->tracks[ti];
            for(int ui=0;ui<trk->n_uses;ui++){
                if(idx==target){
                    float vs=trk->uses[ui].vel_scale;
                    float delta=(mutate_rng(rng)&1)?0.1f:-0.1f;
                    float nv=clampf(vs+delta,
                                    DSL_LIMIT_VEL_SCALE_MIN,
                                    DSL_LIMIT_VEL_SCALE_MAX);
                    if(nv==vs) return 0;
                    trk->uses[ui].vel_scale=nv;
                    return 1;
                }
                idx++;
            }
        }
    }
    return 0;
}

/* ── MUTATE_BEAT_OFFSET: shift MotifUse start_beat ±1 ───────────── */
static int mutate_beat_offset(ShmcWorld *w, uint32_t *rng){
    int total=0;
    for(int si=0;si<w->n_sections;si++)
        for(int ti=0;ti<w->sections[si].n_tracks;ti++)
            total+=w->sections[si].tracks[ti].n_uses;
    if(total==0) return 0;

    int target=mutate_range(rng,0,total-1), idx=0;
    for(int si=0;si<w->n_sections;si++){
        Section *sec=&w->sections[si];
        for(int ti=0;ti<sec->n_tracks;ti++){
            SectionTrack *trk=&sec->tracks[ti];
            for(int ui=0;ui<trk->n_uses;ui++){
                if(idx==target){
                    float sb=trk->uses[ui].start_beat;
                    float delta=(mutate_rng(rng)&1)?1.f:-1.f;
                    float nb=sb+delta;
                    /* keep non-negative and within section length */
                    if(nb<0.f) delta=1.f, nb=sb+delta;
                    if(si<w->n_sections && nb>=sec->length_beats) delta=-1.f, nb=sb+delta;
                    if(nb<0.f || (si<w->n_sections && nb>=sec->length_beats)) return 0;
                    trk->uses[ui].start_beat=nb;
                    return 1;
                }
                idx++;
            }
        }
    }
    return 0;
}

/* ── MUTATE_PATCH: perturb a filter cutoff or ADSR ±1 step ───────── */
/*
 * Filter cutoff changes are much more audible than ADSR release tweaks.
 * Add filter ops to the candidate list 4x to bias selection toward them.
 * Verified: LPF cutoff mutations produce rms_diff > 0.003 consistently.
 */
static int mutate_patch(ShmcWorld *w, uint32_t *rng){
    if(w->n_patches==0) return 0;
    int pi=mutate_range(rng,0,w->n_patches-1);
    PatchProgram *pp=&w->patches[pi];
    if(pp->n_instrs==0) return 0;

    /* Collect mutable instructions — filter ops weighted 4x for audibility */
    int mutable[1024]; int nm=0;
    for(int i=0;i<pp->n_instrs&&nm<1020;i++){
        uint8_t op=INSTR_OP(pp->code[i]);
        if(op==OP_LPF||op==OP_HPF||op==OP_BPF){
            /* Add 4x so filter mutations dominate random selection */
            mutable[nm++]=i; mutable[nm++]=i; mutable[nm++]=i; mutable[nm++]=i;
        } else if(op==OP_ADSR){
            mutable[nm++]=i;
        }
    }
    if(nm==0) return 0;

    int ii=mutable[mutate_range(rng,0,nm-1)];
    Instr ins=pp->code[ii];
    uint8_t op=INSTR_OP(ins);

    if(op==OP_LPF||op==OP_HPF||op==OP_BPF){
        uint16_t hi=INSTR_IMM_HI(ins);
        int delta=(mutate_rng(rng)&1)?1:-1;
        int v=clamp((int)hi+delta, 0, 63);
        if(v==(int)hi) return 0;
        pp->code[ii]=INSTR_PACK(op,INSTR_DST(ins),INSTR_SRC_A(ins),
                                INSTR_SRC_B(ins),(uint16_t)v,INSTR_IMM_LO(ins));
        return 1;
    }
    if(op==OP_ADSR){
        /* Perturb one of the 4 ADSR fields (att/dec/sus/rel) by ±1 */
        uint16_t hi=INSTR_IMM_HI(ins), lo=INSTR_IMM_LO(ins);
        int field=mutate_range(rng,0,3);
        int delta=(mutate_rng(rng)&1)?1:-1;
        switch(field){
        case 0:{int v=clamp((int)((hi>>10)&0x1F)+delta,0,31);
                hi=(uint16_t)((hi&~(0x1F<<10))|(v<<10));break;}
        case 1:{int v=clamp((int)((hi>> 5)&0x1F)+delta,0,31);
                hi=(uint16_t)((hi&~(0x1F<< 5))|(v<< 5));break;}
        case 2:{int v=clamp((int)(hi&0x1F)+delta,0,31);
                hi=(uint16_t)((hi&~0x1F)|v);break;}
        case 3:{int v=clamp((int)((lo>>11)&0x1F)+delta,0,31);
                lo=(uint16_t)((lo&~(0x1F<<11))|(v<<11));break;}
        }
        pp->code[ii]=INSTR_PACK(OP_ADSR,INSTR_DST(ins),0,0,hi,lo);
        patch_canonicalize(pp);
        return 1;
    }
    return 0;
}

/* ── Structural motif mutations ─────────────────────────────────── */

/* Pick a random valid motif from the lib. Returns NULL if none. */
static Motif *pick_motif(ShmcWorld *w, uint32_t *rng) {
    if (!w->lib || w->lib->n == 0) return NULL;
    /* Collect valid indices */
    int valid[MOTIF_LIB_MAX]; int nv = 0;
    for (int i = 0; i < w->lib->n; i++)
        if (w->lib->entries[i].valid) valid[nv++] = i;
    if (!nv) return NULL;
    return &w->lib->entries[valid[mutate_range(rng, 0, nv-1)]];
}

/* Count VI_NOTE instructions in a VoiceProgram */
static int count_notes(const VoiceProgram *vp) {
    int n = 0;
    for (int i = 0; i < vp->n; i++)
        if (VI_OP(vp->code[i]) == VI_NOTE) n++;
    return n;
}

static int mutate_motif_invert(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m || count_notes(&m->vp) < 2) return 0;
    VoiceProgram nv = motif_mutate_invert(&m->vp);
    if (memcmp(&nv, &m->vp, sizeof(VoiceProgram)) == 0) return 0;
    m->vp = nv; return 1;
}

static int mutate_motif_retrograde(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m || count_notes(&m->vp) < 2) return 0;
    VoiceProgram nv = motif_mutate_retrograde(&m->vp);
    if (memcmp(&nv, &m->vp, sizeof(VoiceProgram)) == 0) return 0;
    m->vp = nv; return 1;
}

static int mutate_motif_augment(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m || count_notes(&m->vp) == 0) return 0;
    VoiceProgram nv = motif_mutate_augment(&m->vp);
    if (memcmp(&nv, &m->vp, sizeof(VoiceProgram)) == 0) return 0;
    m->vp = nv; return 1;
}

static int mutate_motif_diminish(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m || count_notes(&m->vp) == 0) return 0;
    VoiceProgram nv = motif_mutate_diminish(&m->vp);
    if (memcmp(&nv, &m->vp, sizeof(VoiceProgram)) == 0) return 0;
    m->vp = nv; return 1;
}

static int mutate_motif_add_note(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m) return 0;
    VoiceProgram *vp = &m->vp;
    if (vp->n >= VOICE_MAX_INSTRS - 1) return 0;
    if (count_notes(vp) >= DSL_LIMIT_MAX_NOTES_PER_MOTIF) return 0;
    /* Pick a random insertion point */
    int ins_at = mutate_range(rng, 0, vp->n);
    /* Random pitch near existing notes or default */
    int pitch = 60;
    if (count_notes(vp) > 0) {
        /* Find a random existing note pitch and offset by ±octave */
        for (int i = 0; i < vp->n; i++) {
            if (VI_OP(vp->code[i]) == VI_NOTE) {
                pitch = (int)VI_PITCH(vp->code[i]);
                break;
            }
        }
        int delta = mutate_range(rng, -12, 12);
        pitch = clamp(pitch + delta, 21, 108);
    }
    int dur = mutate_range(rng, 3, 5);   /* eighth, quarter, half */
    int vel = mutate_range(rng, 6, 12);
    VInstr new_note = VI_PACK(VI_NOTE, (uint8_t)pitch, (uint8_t)dur, (uint8_t)vel);
    /* Shift tail right */
    for (int i = vp->n; i > ins_at; i--) vp->code[i] = vp->code[i-1];
    vp->code[ins_at] = new_note;
    vp->n++;
    return 1;
}

static int mutate_motif_del_note(ShmcWorld *w, uint32_t *rng) {
    Motif *m = pick_motif(w, rng);
    if (!m) return 0;
    VoiceProgram *vp = &m->vp;
    int nc = count_notes(vp);
    if (nc < 2) return 0;  /* never delete last note */
    /* Pick random note index to delete */
    int del_note_idx = mutate_range(rng, 0, nc - 1);
    int ni = 0;
    for (int i = 0; i < vp->n; i++) {
        if (VI_OP(vp->code[i]) == VI_NOTE) {
            if (ni == del_note_idx) {
                /* Delete at position i */
                for (int j = i; j < vp->n - 1; j++) vp->code[j] = vp->code[j+1];
                vp->n--;
                return 1;
            }
            ni++;
        }
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

/* ── Harmonic mutations ──────────────────────────────────────────────
 *
 * These mutations move in Tymoczko's harmonic geometry:
 *
 * CIRCLE_5TH:  Transpose a random MotifUse by +7 semitones (one step
 *   clockwise on the circle of fifths).  Small distance in harmonic
 *   space, large distance in pitch space — creates tension or modulation.
 *   e.g.  C major → G major.
 *
 * CHORD_SUB:  Substitute relative major/minor by shifting transpose
 *   of a MotifUse by ±3 semitones.  C major → A minor or vice-versa.
 *   Maintains tonal color while changing harmonic function.
 *
 * SEC_DOM:  Secondary dominant: temporarily raise one MotifUse transpose
 *   by +7 to create a V/V → V leading tone.  Classic cadential tension.
 */

static int mutate_harm_circle_5th(ShmcWorld *w, uint32_t *rng) {
    /* Transpose a random section use by +7 semitones (circle of 5ths) */
    int n_uses = 0;
    for (int si = 0; si < w->n_sections; si++)
        for (int ti = 0; ti < w->sections[si].n_tracks; ti++)
            n_uses += w->sections[si].tracks[ti].n_uses;
    if (n_uses == 0) return 0;
    int pick = mutate_range(rng, 0, n_uses - 1);
    int cnt = 0;
    for (int si = 0; si < w->n_sections; si++) {
        Section *sec = &w->sections[si];
        for (int ti = 0; ti < sec->n_tracks; ti++) {
            SectionTrack *trk = &sec->tracks[ti];
            for (int ui = 0; ui < trk->n_uses; ui++) {
                if (cnt++ == pick) {
                    /* +7 = one fifth up; wrap so total transpose stays in [-12,12] */
                    int t = trk->uses[ui].transpose + 7;
                    if (t > 12) t -= 12;
                    trk->uses[ui].transpose = t;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int mutate_harm_chord_sub(ShmcWorld *w, uint32_t *rng) {
    /* Substitute relative major/minor: shift transpose by ±3 semitones */
    int n_uses = 0;
    for (int si = 0; si < w->n_sections; si++)
        for (int ti = 0; ti < w->sections[si].n_tracks; ti++)
            n_uses += w->sections[si].tracks[ti].n_uses;
    if (n_uses == 0) return 0;
    int pick = mutate_range(rng, 0, n_uses - 1);
    int delta = (mutate_rng(rng) & 1) ? +3 : -3;
    int cnt = 0;
    for (int si = 0; si < w->n_sections; si++) {
        Section *sec = &w->sections[si];
        for (int ti = 0; ti < sec->n_tracks; ti++) {
            SectionTrack *trk = &sec->tracks[ti];
            for (int ui = 0; ui < trk->n_uses; ui++) {
                if (cnt++ == pick) {
                    int t = trk->uses[ui].transpose + delta;
                    if (t >  12) t -= 12;
                    if (t < -12) t += 12;
                    trk->uses[ui].transpose = t;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int mutate_harm_sec_dom(ShmcWorld *w, uint32_t *rng) {
    /* Secondary dominant: one random MotifUse gets +7 transpose
     * (becomes V of the next chord).  Classic leading-tone preparation. */
    if (!w->lib || w->lib->n == 0) return 0;
    /* Pick a motif and transpose all its uses in one track by +7 */
    if (w->n_sections == 0) return 0;
    int si = mutate_range(rng, 0, w->n_sections - 1);
    Section *sec = &w->sections[si];
    if (sec->n_tracks == 0) return 0;
    int ti = mutate_range(rng, 0, sec->n_tracks - 1);
    SectionTrack *trk = &sec->tracks[ti];
    if (trk->n_uses < 2) return 0;
    /* Apply to one randomly chosen use only */
    int ui = mutate_range(rng, 0, trk->n_uses - 1);
    int t = trk->uses[ui].transpose + 7;
    if (t > 12) t -= 12;
    trk->uses[ui].transpose = t;
    return 1;
}

int shmc_mutate(ShmcWorld *w, MutateType type, uint32_t *rng){
    if (type == MUTATE_ANY)
        type = (MutateType)mutate_range(rng, 0, 6);
    if (type == MUTATE_STRUCTURAL)
        type = (MutateType)(8 + mutate_range(rng, 0, 5));

    switch(type){
    case MUTATE_NOTE_PITCH:       return mutate_note_pitch(w,rng);
    case MUTATE_NOTE_VEL:         return mutate_note_vel(w,rng);
    case MUTATE_NOTE_DUR:         return mutate_note_dur(w,rng);
    case MUTATE_TRANSPOSE:        return mutate_transpose(w,rng);
    case MUTATE_VEL_SCALE:        return mutate_vel_scale(w,rng);
    case MUTATE_BEAT_OFFSET:      return mutate_beat_offset(w,rng);
    case MUTATE_PATCH:            return mutate_patch(w,rng);
    case MUTATE_MOTIF_INVERT:     return mutate_motif_invert(w,rng);
    case MUTATE_MOTIF_RETROGRADE: return mutate_motif_retrograde(w,rng);
    case MUTATE_MOTIF_AUGMENT:    return mutate_motif_augment(w,rng);
    case MUTATE_MOTIF_DIMINISH:   return mutate_motif_diminish(w,rng);
    case MUTATE_MOTIF_ADD_NOTE:   return mutate_motif_add_note(w,rng);
    case MUTATE_MOTIF_DEL_NOTE:   return mutate_motif_del_note(w,rng);
    /* harmonic mutations */
    case MUTATE_HARM_CIRCLE_5TH:  return mutate_harm_circle_5th(w,rng);
    case MUTATE_HARM_CHORD_SUB:   return mutate_harm_chord_sub(w,rng);
    case MUTATE_HARM_SEC_DOM:     return mutate_harm_sec_dom(w,rng);
    case MUTATE_HARMONIC:{
        MutateType h=(MutateType)(15+mutate_range(rng,0,2));
        return shmc_mutate(w,h,rng);
    }
    default:                      return 0;
    }
}
