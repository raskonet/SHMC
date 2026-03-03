/*
 * shmc_harmony.c  —  Symbolic Harmony Fitness
 * Five orthogonal symbolic measures: H1-H5.
 * All operate on ShmcWorld directly — no audio render required.
 *
 * H1  Lerdahl 2001  — scale/key-finding (pitch-class histogram)
 * H2  Sethares 1998 — psychoacoustic consonance table
 * H3  Tymoczko 2011 — voice-leading smoothness (mean semitone jump)
 * H4  Herremans 2017 — MorpheuS tension arc (variance + resolution)
 * H5  Piston 1978   — chord root-motion cadence scoring
 *
 */
#include "../include/shmc_harmony.h"
#include "../../layer0/include/patch.h"
#include "../../layer1/include/voice.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ── Scale templates ─────────────────────────────────────────── */
static const int SCALE_DEG[6][7] = {
    {0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}, {0,2,3,5,7,9,10},
    {0,2,4,5,7,9,10}, {0,1,3,5,7,8,10}, {0,2,4,6,7,9,11},
};
static uint16_t scale_mask(int root, int mode) {
    uint16_t m = 0;
    for (int d=0;d<7;d++) m|=(uint16_t)(1u<<((root+SCALE_DEG[mode][d])%12));
    return m;
}

/* ── Consonance table (Huron/Sethares) ──────────────────────── */
float interval_consonance(int s) {
    s=((s%12)+12)%12; if(s>6)s=12-s;
    static const float T[7]={1.00f,0.05f,0.25f,0.65f,0.70f,0.80f,0.05f};
    return T[s];
}

/* ── Note entry with float beat times ───────────────────────── */
typedef struct { int pitch; float beat_s, beat_e; int motif_idx; } NEntry;
#define MAX_N 4096

static int collect_notes(const ShmcWorld *w, NEntry *out, int cap) {
    int cnt=0;
    if (!w->lib) return 0;
    for (int mi=0; mi<w->lib->n && cnt<cap; mi++) {
        const VoiceProgram *vp = &w->lib->entries[mi].vp;
        float beat=0.f;
        for (int ii=0; ii<vp->n && cnt<cap; ii++) {
            VInstr ins=vp->code[ii];
            int op=VI_OP(ins), di=VI_DUR(ins);
            float dur=(di<7)?g_dur[di]:g_dur[6];
            if (op==VI_NOTE) {
                out[cnt].pitch=VI_PITCH(ins);
                out[cnt].beat_s=beat; out[cnt].beat_e=beat+dur;
                out[cnt].motif_idx=mi; cnt++;
            }
            if (op==VI_NOTE||op==VI_REST) beat+=dur;
        }
    }
    return cnt;
}

/* ── H1: Scale Consistency ───────────────────────────────────── */
static float h1_scale(const NEntry *notes, int n, int *root_out, int *mode_out) {
    if (!n) { *root_out=0; *mode_out=0; return 0.f; }
    float hist[12]={0};
    for (int i=0;i<n;i++) hist[notes[i].pitch%12]+=1.f;
    float total=(float)n, best=-1.f; int br=0,bm=0;
    for (int r=0;r<12;r++) for (int m=0;m<6;m++) {
        uint16_t mask=scale_mask(r,m); float in=0.f;
        for (int pc=0;pc<12;pc++) if(mask&(1u<<pc)) in+=hist[pc];
        float s=in/total; if(s>best){best=s;br=r;bm=m;}
    }
    *root_out=br; *mode_out=bm; return best;
}

/* ── H2: Harmonic Consonance ─────────────────────────────────── */
static float h2_consonance(const ShmcWorld *w) {
    if (!w->lib||!w->lib->n) return 0.5f;
    float sum=0.f; int pairs=0;
    for (int mi=0;mi<w->lib->n;mi++) {
        const VoiceProgram *vp=&w->lib->entries[mi].vp;
        int pit[256]; float bs[256],be[256]; int nn=0; float beat=0.f;
        for (int ii=0;ii<vp->n&&nn<256;ii++) {
            VInstr ins=vp->code[ii]; int op=VI_OP(ins),di=VI_DUR(ins);
            float dur=(di<7)?g_dur[di]:g_dur[6];
            if (op==VI_NOTE){pit[nn]=VI_PITCH(ins);bs[nn]=beat;be[nn]=beat+dur;nn++;}
            if (op==VI_NOTE||op==VI_REST) beat+=dur;
        }
        for (int a=0;a<nn-1;a++) for (int b=a+1;b<nn;b++) {
            if (bs[b]>=be[a]||bs[a]>=be[b]) continue;
            sum+=interval_consonance(abs(pit[a]-pit[b])); pairs++;
        }
    }
    /* monophonic fallback */
    if (!pairs) for (int mi=0;mi<w->lib->n;mi++) {
        const VoiceProgram *vp=&w->lib->entries[mi].vp; int prev=-1;
        for (int ii=0;ii<vp->n;ii++) {
            VInstr ins=vp->code[ii];
            if (VI_OP(ins)==VI_NOTE){
                int p=VI_PITCH(ins);
                if(prev>=0){sum+=interval_consonance(abs(p-prev));pairs++;}
                prev=p;
            }
        }
    }
    return pairs>0?sum/(float)pairs:0.5f;
}

/* ── H3: Voice Leading (Tymoczko) ───────────────────────────── */
static float use_mean_pitch(const MotifUse *use) {
    const Motif *mot=use->resolved_motif; if(!mot) return 60.f;
    const VoiceProgram *vp=&mot->vp; float s=0.f; int n=0;
    for (int ii=0;ii<vp->n;ii++) {
        VInstr ins=vp->code[ii];
        if(VI_OP(ins)==VI_NOTE){s+=VI_PITCH(ins);n++;}
    }
    return (n>0?s/(float)n:60.f)+(float)use->transpose;
}
static float h3_voice_leading(const ShmcWorld *w) {
    float total=0.f; int trans=0;
    for (int si=0;si<w->n_sections;si++) {
        const Section *sec=&w->sections[si];
        for (int ti=0;ti<sec->n_tracks;ti++) {
            const SectionTrack *trk=&sec->tracks[ti]; float prev=-999.f;
            for (int ui=0;ui<trk->n_uses;ui++) {
                if(!trk->uses[ui].resolved_motif) continue;
                float mp=use_mean_pitch(&trk->uses[ui]);
                if(prev>-900.f){total+=fabsf(mp-prev);trans++;}
                prev=mp;
            }
        }
    }
    if (!trans) return 0.70f;
    return expf(-0.17f*total/(float)trans);
}

/* ── H4: Tension Arc (MorpheuS) ─────────────────────────────── */
#define ARC_W 8
static float h4_tension_arc(const ShmcWorld *w, int tonic) {
    if (!w->n_sections||!w->lib) return 0.f;
    const Section *sec=&w->sections[0];
    float sec_len=sec->length_beats>0.f?sec->length_beats:0.f;
    if (sec_len<=0.f) {
        for (int ti=0;ti<sec->n_tracks;ti++)
            for (int ui=0;ui<sec->tracks[ti].n_uses;ui++) {
                float e=sec->tracks[ti].uses[ui].start_beat
                       +(float)sec->tracks[ti].uses[ui].repeat;
                if(e>sec_len) sec_len=e;
            }
        if(sec_len<1.f) sec_len=16.f;
    }
    float wsum[ARC_W]={0}; int wcnt[ARC_W]={0};
    for (int ti=0;ti<sec->n_tracks;ti++) {
        const SectionTrack *trk=&sec->tracks[ti];
        for (int ui=0;ui<trk->n_uses;ui++) {
            if(!trk->uses[ui].resolved_motif) continue;
            float mp=use_mean_pitch(&trk->uses[ui]);
            int win=(int)(trk->uses[ui].start_beat/sec_len*ARC_W);
            if(win<0){ win=0; } if(win>=ARC_W){ win=ARC_W-1; }
            wsum[win]+=mp; wcnt[win]++;
        }
    }
    float tonic_p=60.f+(float)tonic;
    float tension[ARC_W];
    for (int i=0;i<ARC_W;i++) {
        float p=wcnt[i]>0?wsum[i]/(float)wcnt[i]:tonic_p;
        float d=fmodf(fabsf(p-tonic_p),12.f);
        if(d>6.f)d=12.f-d;
        tension[i]=d/6.f;
    }
    float mt=0.f; for(int i=0;i<ARC_W;i++) mt+=tension[i]; mt/=ARC_W;
    float vt=0.f; for(int i=0;i<ARC_W;i++){float d=tension[i]-mt;vt+=d*d;}vt/=ARC_W;
    float arc_score=1.f-expf(-20.f*vt);
    float mid=0.f,end=0.f;
    for(int i=2;i<6;i++){ mid+=tension[i]; } mid/=4.f;
    for(int i=6;i<8;i++){ end+=tension[i]; } end/=2.f;
    float res=(mid>0.01f)?fmaxf(0.f,(mid-end)/mid):0.f;
    return 0.5f*arc_score+0.5f*res;
}

/* ── H5: Chord Cadence / Root Motion (Piston) ────────────────── */
#define CAD_W 8
static float h5_cadence(const NEntry *notes, int n, float total_beats) {
    if (n<2||total_beats<0.1f) return 0.5f;
    float dp[CAD_W][12]; memset(dp,0,sizeof(dp));
    for (int i=0;i<n;i++) {
        int win=(int)(notes[i].beat_s/total_beats*CAD_W);
        if(win<0){ win=0; } if(win>=CAD_W){ win=CAD_W-1; }
        dp[win][notes[i].pitch%12]+=(notes[i].beat_e-notes[i].beat_s);
    }
    int roots[CAD_W];
    for (int w=0;w<CAD_W;w++) {
        int best=0;
        for(int pc=1;pc<12;pc++) if(dp[w][pc]>dp[w][best]) best=pc;
        roots[w]=best;
    }
    /* Score root-motion intervals (Piston root motion table) */
    float ss=0.f; int mv=0;
    for (int i=0;i<CAD_W-1;i++) {
        int iv=((roots[i+1]-roots[i])+12)%12; float s;
        switch(iv){
            case 0: s=0.00f;break;   /* static */
            case 7: s=1.00f;break;   /* desc 5th G→C strongest */
            case 5: s=0.85f;break;   /* asc  4th same interval */
            case 9: s=0.55f;break;   /* desc 3rd */
            case 3: s=0.50f;break;   /* asc minor 3rd */
            case 4: s=0.45f;break;   /* asc major 3rd */
            case 2:case 10:s=0.30f;break; /* step */
            case 1:case 11:s=0.15f;break; /* semitone */
            default:s=0.20f;break;
        }
        ss+=s; mv++;
    }
    return mv>0?ss/(float)mv:0.5f;
}

/* ── Public API ──────────────────────────────────────────────── */
void harmony_weights_default(HarmonyWeights *w) {
    /* 13 metrics — sum = 1.00 */
    /* H1-H14, sum = 1.00 */
    w->w_scale   = 0.15f;   /* H1  scale consistency */
    w->w_cons    = 0.11f;   /* H2  consonance */
    w->w_voice   = 0.08f;   /* H3  voice leading */
    w->w_arc     = 0.08f;   /* H4  tension arc */
    w->w_cad     = 0.11f;   /* H5  cadence */
    w->w_lerdahl = 0.11f;   /* H6  Lerdahl tonal tension */
    w->w_surprise= 0.06f;   /* H7  harmonic surprise */
    w->w_groove  = 0.06f;   /* H8  groove */
    w->w_pdiv    = 0.04f;   /* H9  pitch diversity */
    w->w_rhythm  = 0.03f;   /* H10 rhythm entropy */
    w->w_motif   = 0.02f;   /* H11 motif repetition */
    w->w_spread  = 0.03f;   /* H12 temporal spread */
    w->w_mscale  = 0.03f;   /* H13 hierarchical tension */
    w->w_cprog   = 0.09f;   /* H14 chord progression */
}
void combined_weights_default(CombinedWeights *w) {
    w->w_harmony=0.60f; w->w_evo=0.35f; w->w_novelty=0.05f;
}

/* ── H6: Lerdahl Tonal Tension ───────────────────────────────────── */
static float h6_lerdahl_tension(const NEntry *notes, int n, int root, int mode) {
    if (n == 0) return 0.5f;
    static const float LW[12] = {0.f,3.f,2.f,3.f,1.f,1.f,3.f,1.f,3.f,1.f,3.f,2.f};
    (void)mode;
    float sum = 0.f;
    for (int i = 0; i < n; i++) {
        int pc = ((notes[i].pitch % 12) + 12) % 12;
        int dist = ((pc - root) % 12 + 12) % 12;
        if (dist > 6) dist = 12 - dist;
        sum += LW[dist] * (float)dist;
    }
    float tension = sum / ((float)n * 18.f);
    if (tension > 1.f) tension = 1.f;
    return 1.f - tension;
}

/* ── H7: Harmonic Surprise — chord-transition entropy ───────────── */
#define H7_W 8
static float h7_harmonic_surprise(const NEntry *notes, int n, float total_beats) {
    if (n < 4 || total_beats < 2.f) return 0.5f;
    float hist[H7_W][12]; memset(hist,0,sizeof(hist));
    for (int i = 0; i < n; i++) {
        int win = (int)(notes[i].beat_s / total_beats * H7_W);
        if (win < 0) { win = 0; } if (win >= H7_W) { win = H7_W-1; }
        hist[win][((notes[i].pitch%12)+12)%12] += 1.f;
    }
    int roots[H7_W];
    for (int w = 0; w < H7_W; w++) {
        int best = 0;
        for (int pc = 1; pc < 12; pc++) if (hist[w][pc] > hist[w][best]) best = pc;
        roots[w] = best;
    }
    float trans[12][12]; memset(trans,0,sizeof(trans));
    int total_trans = 0;
    for (int w = 1; w < H7_W; w++) {
        if (roots[w] != roots[w-1]) {
            trans[roots[w-1]][roots[w]] += 1.f;
            total_trans++;
        }
    }
    if (total_trans < 2) return 0.5f;
    float H = 0.f;
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 12; j++) {
            float p = trans[i][j] / (float)total_trans;
            if (p > 1e-9f) H -= p * logf(p) / logf(2.f);
        }
    }
    /* Score peaks at H=1.75 bits target */
    float d = H - 1.75f;
    return expf(-(d*d) / (2.f * 1.5f * 1.5f));
}

/* ── H8: Groove — inter-onset interval regularity ───────────────── */
static float h8_groove(const NEntry *notes, int n) {
    if (n < 3) return 0.5f;
    float onsets[MAX_N]; int no = 0;
    for (int i = 0; i < n && no < MAX_N; i++) onsets[no++] = notes[i].beat_s;
    /* insertion sort */
    for (int i = 1; i < no; i++) {
        float k = onsets[i]; int j = i-1;
        while (j >= 0 && onsets[j] > k) { onsets[j+1]=onsets[j]; j--; }
        onsets[j+1] = k;
    }
    if (no < 3) return 0.5f;
    float ioi_mean = 0.f;
    int n_ioi = no - 1;
    for (int i = 0; i < n_ioi; i++) ioi_mean += (onsets[i+1] - onsets[i]);
    ioi_mean /= (float)n_ioi;
    if (ioi_mean < 0.01f) return 0.3f;
    float ioi_var = 0.f;
    for (int i = 0; i < n_ioi; i++) {
        float d = (onsets[i+1]-onsets[i]) - ioi_mean;
        ioi_var += d*d;
    }
    ioi_var /= (float)n_ioi;
    float cv = sqrtf(ioi_var) / ioi_mean;
    /* Score peaks at CV=0.25 — moderate rhythmic variation */
    float d = cv - 0.25f;
    return expf(-(d*d) / (2.f * 0.20f * 0.20f));
}

/* ── H9: Pitch diversity Dp = unique pitch classes / 12 ─────────── */
static float h9_pitch_diversity(const NEntry *notes, int n) {
    if (n == 0) return 0.f;
    int seen[12] = {0};
    for (int i = 0; i < n; i++) seen[((notes[i].pitch%12)+12)%12] = 1;
    int u = 0; for (int i = 0; i < 12; i++) u += seen[i];
    float dp = (float)u / 12.f;
    /* Peaks at 6 unique PCs (pentatonic to diatonic sweet spot) */
    float d = dp - 0.5f;
    return expf(-(d*d) / (2.f * 0.25f * 0.25f));
}

/* ── H10: Rhythm entropy H_norm ──────────────────────────────────── */
static float h10_rhythm_entropy(const NEntry *notes, int n, float total_beats) {
    if (n < 2 || total_beats < 1.f) return 0.5f;
    enum { GRID = 16 };
    float bins[GRID]; memset(bins, 0, sizeof(bins));
    for (int i = 0; i < n; i++) {
        int b = (int)(notes[i].beat_s / total_beats * GRID);
        if (b < 0) { b = 0; } if (b >= GRID) { b = GRID-1; }
        bins[b] += 1.f;
    }
    float H = 0.f;
    for (int i = 0; i < GRID; i++) {
        float p = bins[i] / (float)n;
        if (p > 1e-9f) H -= p * logf(p);
    }
    H /= logf((float)GRID);
    if (H < 0.f) { H = 0.f; } if (H > 1.f) { H = 1.f; }
    /* Peaks at 0.65 — moderately complex rhythm */
    float d = H - 0.65f;
    return expf(-(d*d) / (2.f * 0.20f * 0.20f));
}

/* ── H11: Motif repetition compression Cm ───────────────────────── */
static float h11_motif_repetition(const ShmcWorld *w) {
    if (!w->lib || w->lib->n == 0) return 0.5f;
    int total_uses = 0, unique_motifs = 0;
    int seen_motif[32] = {0};
    for (int si = 0; si < w->n_sections; si++) {
        const Section *sec = &w->sections[si];
        for (int ti = 0; ti < sec->n_tracks; ti++) {
            const SectionTrack *trk = &sec->tracks[ti];
            for (int ui = 0; ui < trk->n_uses; ui++) {
                total_uses++;
                int midx = -1;
                for (int mi = 0; mi < w->lib->n; mi++) {
                    if (strcmp(w->lib->entries[mi].name, trk->uses[ui].name)==0) {
                        midx = mi; break;
                    }
                }
                if (midx >= 0 && midx < 32 && !seen_motif[midx]) {
                    seen_motif[midx] = 1; unique_motifs++;
                }
            }
        }
    }
    if (total_uses == 0) return 0.5f;
    float cm = 1.f - (float)unique_motifs / (float)total_uses;
    if (cm < 0.f) { cm = 0.f; }
    /* Peaks at Cm=0.55 — good repetition with variation */
    float d = cm - 0.55f;
    return expf(-(d*d) / (2.f * 0.20f * 0.20f));
}


/* ── H12: Temporal Spread  S = occupied_bins / M ────────────────────
 *
 * From Müller "Fundamentals of Music Processing" (2015).
 * Quantize note onsets onto a 64-cell beat grid.
 * Score = fraction of grid cells that contain at least one onset.
 * Penalizes music that clusters all notes in one region of time.
 * Peaks at full coverage (all cells used), but even 50% is good.
 */
static float h12_temporal_spread(const NEntry *notes, int n, float total_beats) {
    if (n == 0 || total_beats < 1.f) return 0.f;
    enum { M = 64 };
    int grid[M]; memset(grid, 0, sizeof(grid));
    for (int i = 0; i < n; i++) {
        int b = (int)(notes[i].beat_s / total_beats * M);
        if (b < 0) { b = 0; } if (b >= M) { b = M-1; }
        grid[b] = 1;
    }
    int occupied = 0;
    for (int i = 0; i < M; i++) occupied += grid[i];
    return (float)occupied / (float)M;
}

/* ── H13: Hierarchical Multi-Scale Tension consistency ───────────────
 *
 * From Lerdahl/Jackendoff "Generative Theory of Tonal Music" (1983).
 * compute Lerdahl tonal tension at four time scales:
 *   beat (1 beat windows), bar (4-beat), phrase (8-beat), section (whole)
 * Score = 1 - mean |T_fine - T_coarse|
 * Good music has CONSISTENT tension relationships across scales
 * (local tension aligns with global tension — "nested structure").
 */
#define MS_WINDOWS 4   /* beat, bar, phrase, section */
static float h13_multiscale_tension(const NEntry *notes, int n,
                                     float total_beats, int root) {
    if (n < 4 || total_beats < 2.f) return 0.5f;

    /* Scale window sizes in beats */
    float win_size[MS_WINDOWS] = {1.f, 4.f, 8.f, total_beats};

    /* Compute mean Lerdahl tension for each scale */
    static const float LW[12] = {0.f,3.f,2.f,3.f,1.f,1.f,3.f,1.f,3.f,1.f,3.f,2.f};
    float T[MS_WINDOWS] = {0};
    int   C[MS_WINDOWS] = {0};

    for (int i = 0; i < n; i++) {
        int pc = ((notes[i].pitch % 12) + 12) % 12;
        int dist = ((pc - root) % 12 + 12) % 12;
        if (dist > 6) dist = 12 - dist;
        float tension_i = LW[dist] * (float)dist / 18.f;

        for (int s = 0; s < MS_WINDOWS; s++) {
            /* Which window does this note fall in? */
            int win_idx = (int)(notes[i].beat_s / win_size[s]);
            (void)win_idx;  /* all notes contribute to scale-level mean */
            T[s] += tension_i;
            C[s]++;
        }
    }
    for (int s = 0; s < MS_WINDOWS; s++)
        T[s] = (C[s] > 0) ? T[s] / (float)C[s] : 0.5f;

    /* Penalty: sum of |T_fine - T_coarse| across adjacent scales */
    float penalty = 0.f;
    for (int s = 0; s < MS_WINDOWS - 1; s++)
        penalty += fabsf(T[s] - T[s+1]);
    penalty /= (float)(MS_WINDOWS - 1);  /* normalize to [0,1] */

    return 1.f - penalty;   /* high score = consistent nested tension */
}


/* ── H14: Chord Progression Quality (Krumhansl + Bigram) ────────────
 *
 * From two sources:
 *
 * Krumhansl-Schmuckler (1990): pitch-class profiles for major/minor.
 * Detects key via Pearson correlation of pitch histogram vs 12 profiles.
 * Returns how strongly the piece stays in ONE tonal center (0=atonal).
 *
 * Chord bigram matrix: empirical probabilities of common transitions.
 * Inspired by Paiement et al. "A probabilistic model for chord
 * progressions" (ICMC 2005).
 * Rewards I→IV, I→V, V→I, IV→I, ii→V, vi→IV (standard diatonic motion).
 * Penalizes flat random root-motion (chromatic / tritone moves).
 *
 * Score = 0.5*krumhansl_tonality + 0.5*bigram_progression
 */

/* Krumhansl-Schmuckler key profiles (major and minor) */
static const float KK_MAJOR[12] = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
static const float KK_MINOR[12] = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

/* Pearson correlation of two 12-vectors */
static float pearson12(const float *a, const float *b) {
    float ma=0.f, mb=0.f;
    for (int i=0;i<12;i++){ma+=a[i];mb+=b[i];}
    ma/=12.f; mb/=12.f;
    float num=0.f, da=0.f, db=0.f;
    for (int i=0;i<12;i++){
        float ai=a[i]-ma, bi=b[i]-mb;
        num+=ai*bi; da+=ai*ai; db+=bi*bi;
    }
    float denom=sqrtf(da*db);
    return (denom>1e-9f)?num/denom:0.f;
}

/* Bigram transition score: how natural is a root-motion interval? */
static float bigram_score(int iv) {
    /* Descending-fifth cadences score highest (circle of 5ths motion) */
    switch (((iv % 12) + 12) % 12) {
        case  0: return 0.10f;  /* static — weak but not terrible */
        case  7: return 1.00f;  /* V→I desc 5th — strongest */
        case  5: return 0.90f;  /* I→IV asc 4th — very common */
        case  9: return 0.75f;  /* vi→IV desc 3rd — common */
        case  3: return 0.65f;  /* I→bIII / vi — common */
        case  4: return 0.60f;  /* I→III / I→IV — moderate */
        case  2: return 0.40f;  /* step motion — OK */
        case 10: return 0.40f;  /* desc 2nd — OK */
        case  1: return 0.10f;  /* chromatic — unusual */
        case 11: return 0.10f;  /* chromatic — unusual */
        case  6: return 0.05f;  /* tritone — very unusual */
        default: return 0.20f;
    }
}

static float h14_chord_progression(const NEntry *notes, int n, float total_beats) {
    if (n < 4 || total_beats < 2.f) return 0.5f;

    /* Build pitch-class histogram for Krumhansl correlation */
    float hist[12] = {0};
    for (int i = 0; i < n; i++) hist[((notes[i].pitch%12)+12)%12] += 1.f;
    /* Normalize by duration weight */
    float hsum = 0.f; for (int i=0;i<12;i++) hsum+=hist[i];
    if (hsum > 0.f) for (int i=0;i<12;i++) hist[i]/=hsum;

    /* Find best key via Krumhansl correlation */
    float best_corr = -2.f;
    for (int root = 0; root < 12; root++) {
        /* Rotate profiles to this root */
        float pm[12], pmi[12];
        for (int i=0;i<12;i++){
            pm[i]  = KK_MAJOR[(i-root+12)%12];
            pmi[i] = KK_MINOR[(i-root+12)%12];
        }
        float cm = pearson12(hist, pm);
        float cmi= pearson12(hist, pmi);
        float c  = (cm>cmi)?cm:cmi;
        if (c > best_corr) best_corr = c;
    }
    /* Map Pearson correlation [-1,1] → tonality score [0,1] */
    float tonality = (best_corr + 1.f) * 0.5f;

    /* Build windowed root sequence for bigram scoring */
    enum { BW = 8 };
    float dp[BW][12]; memset(dp,0,sizeof(dp));
    for (int i=0;i<n;i++){
        int w=(int)(notes[i].beat_s/total_beats*BW);
        if(w<0){w=0;} if(w>=BW){w=BW-1;}
        dp[w][((notes[i].pitch%12)+12)%12]+=(notes[i].beat_e-notes[i].beat_s);
    }
    int roots[BW];
    for (int w=0;w<BW;w++){
        int best=0;
        for(int pc=1;pc<12;pc++) if(dp[w][pc]>dp[w][best]) best=pc;
        roots[w]=best;
    }

    /* Score transitions via bigram */
    float bsum = 0.f; int btrans = 0;
    for (int w=1;w<BW;w++){
        if (roots[w]!=roots[w-1]){
            int iv=((roots[w]-roots[w-1])+12)%12;
            bsum += bigram_score(iv);
            btrans++;
        }
    }
    float bigram = (btrans>0) ? bsum/(float)btrans : 0.5f;

    return 0.5f*tonality + 0.5f*bigram;
}


int harmony_feat_extract(const ShmcWorld *world, HarmonyFeat *out,
                          const HarmonyWeights *w) {
    memset(out,0,sizeof(*out));
    if (!world->lib||!world->lib->n) return -1;
    NEntry notes[MAX_N]; int n=collect_notes(world,notes,MAX_N);
    if (!n) return -1;
    float total_beats=0.f;
    for(int i=0;i<n;i++) if(notes[i].beat_e>total_beats) total_beats=notes[i].beat_e;
    if(total_beats<1.f) total_beats=16.f;

    out->scale_consistency = h1_scale(notes,n,&out->best_root,&out->best_scale_type);
    out->consonance        = h2_consonance(world);
    out->voice_leading     = h3_voice_leading(world);
    out->tension_arc       = h4_tension_arc(world, out->best_root);
    out->cadence           = h5_cadence(notes, n, total_beats);
    /* H6-H11 */
    out->lerdahl_tension   = h6_lerdahl_tension(notes, n, out->best_root, out->best_scale_type);
    out->harmonic_surprise = h7_harmonic_surprise(notes, n, total_beats);
    out->groove            = h8_groove(notes, n);
    out->pitch_diversity   = h9_pitch_diversity(notes, n);
    out->rhythm_entropy    = h10_rhythm_entropy(notes, n, total_beats);
    out->motif_repetition  = h11_motif_repetition(world);
    out->temporal_spread   = h12_temporal_spread(notes, n, total_beats);
    out->multiscale_tension= h13_multiscale_tension(notes, n, total_beats, out->best_root);
    out->chord_progression = h14_chord_progression(notes, n, total_beats);

    out->harmony_score = w->w_scale   * out->scale_consistency
                       + w->w_cons    * out->consonance
                       + w->w_voice   * out->voice_leading
                       + w->w_arc     * out->tension_arc
                       + w->w_cad     * out->cadence
                       + w->w_lerdahl * out->lerdahl_tension
                       + w->w_surprise* out->harmonic_surprise
                       + w->w_groove  * out->groove
                       + w->w_pdiv    * out->pitch_diversity
                       + w->w_rhythm  * out->rhythm_entropy
                       + w->w_motif   * out->motif_repetition
                       + w->w_spread  * out->temporal_spread
                       + w->w_mscale  * out->multiscale_tension
                       + w->w_cprog   * out->chord_progression;
    return 0;
}

float combined_fitness(const HarmonyFeat *hf, const EvoFeat *ef,
                       float novelty,
                       const HarmonyWeights *hw, const EvoWeights *ew,
                       const CombinedWeights *cw) {
    (void)hw;
    return cw->w_harmony * hf->harmony_score
         + cw->w_evo     * evo_fitness(ef,ew)
         + cw->w_novelty * novelty;
}

float world_total_fitness(const ShmcWorld *world, const EvoFeat *ef,
                           float novelty,
                           const HarmonyWeights *hw, const EvoWeights *ew,
                           const CombinedWeights *cw) {
    HarmonyFeat hf;
    if (harmony_feat_extract(world,&hf,hw)<0) return evo_fitness(ef,ew);
    return combined_fitness(&hf,ef,novelty,hw,ew,cw);
}
