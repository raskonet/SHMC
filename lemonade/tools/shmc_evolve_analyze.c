/*
 * shmc_evolve_analyze.c  v3
 *
 * ASAN-CLEAN rewrite. Root cause of all previous crashes:
 *   ShmcWorld (693 KB) declared on the stack. With multiple copies in
 *   main() and advanced_search(), the stack hits ~3.5 MB and MotifUse
 *   pointers inside Sections point into stack frames that ASAN marks as
 *   out-of-scope. Fix: every ShmcWorld is malloc'd.
 *
 * Pipeline:
 *   1. Analyse seed → seed.wav + Analysis report
 *   2. Beam search with invertible algebra + structural mutations + canon dedup
 *   3. Analyse evolved → evolved.wav + Analysis report
 *   4. Diagnosis: before vs after, recommendations
 *   5. Emit evolved.dsl
 *
 * Usage:  ./shmc_evolve_analyze <seed.dsl> [generations] [beam_width]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../../lemonade/include/shmc_dsl.h"
#include "../../lemonade/include/shmc_dsl_emit.h"
#include "../../lemonade/include/shmc_mutate.h"
#include "../../lemonade/include/shmc_search.h"
#include "../../lemonade/include/shmc_canon.h"
#include "../../lemonade/include/shmc_patch_mutate.h"
#include "../../lemonade/include/shmc_mut_algebra.h"
#include "../../layer0b/include/shmc_hash.h"

#define SR        44100.f
#define PREVIEW_S 3.0f
#define PREVIEW_N ((int)(PREVIEW_S * SR))

/* ═══════════════════════════════════════════════════════════════════
   Utilities
   ═══════════════════════════════════════════════════════════════════ */

static void write_wav(const char *path, const float *buf, int n, float sr) {
    FILE *f = fopen(path,"wb");
    if (!f) { fprintf(stderr,"Cannot write %s\n",path); return; }
    int ds=n*2, fs=36+ds;
    fwrite("RIFF",1,4,f); fwrite(&fs,4,1,f);
    fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f);
    int c16=16; fwrite(&c16,4,1,f);
    short fmt=1,ch=1; fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    int isr=(int)sr; fwrite(&isr,4,1,f);
    int br=isr*2; fwrite(&br,4,1,f);
    short ba=2,bps=16; fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f);
    for (int i=0;i<n;i++) {
        float s=buf[i]; if(s>1.f)s=1.f; if(s<-1.f)s=-1.f;
        short si=(short)(s*32767.f); fwrite(&si,2,1,f);
    }
    fclose(f);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path,"rb");
    if (!f) { fprintf(stderr,"Cannot open %s\n",path); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=(char*)malloc((size_t)sz+1);
    if (!buf) { fclose(f); return NULL; }
    long got=(long)fread(buf,1,(size_t)sz,f); buf[got]=0;
    fclose(f); return buf;
}

/*
 * world_new: malloc a fresh zeroed ShmcWorld, compile DSL into it.
 * Returns heap pointer or NULL. Caller must shmc_world_free()+free().
 */
static ShmcWorld *world_new(const char *dsl, char *err, int err_sz) {
    ShmcWorld *w = (ShmcWorld*)calloc(1, sizeof(ShmcWorld));
    if (!w) return NULL;
    if (shmc_dsl_compile(dsl, w, err, err_sz) < 0) { free(w); return NULL; }
    return w;
}

/* world_dup: deep-copy src to a new heap ShmcWorld. Returns NULL on OOM.
 *
 * ShmcWorld contains several inline arrays whose addresses change when the
 * struct is copied. Every pointer that points INTO these arrays must be
 * re-based to the new struct's address:
 *
 *   src->patches[]    → SectionTrack.patch        (const PatchProgram*)
 *   src->sections[]   → SongEntry.section          (const Section*)
 *   src->lib (heap)   → SongEntry.lib              (MotifLibrary*)
 *                        MotifUse.resolved_motif    (via motif_resolve_uses)
 *
 * Failure to re-base any of these causes heap-use-after-free when the
 * original world is freed and the copy is still in use.
 */
static ShmcWorld *world_dup(const ShmcWorld *src) {
    ShmcWorld *dst = (ShmcWorld*)malloc(sizeof(ShmcWorld));
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(ShmcWorld));

    /* 1. Fresh lib allocation */
    dst->lib = NULL;
    if (src->lib) {
        dst->lib = (MotifLibrary*)malloc(sizeof(MotifLibrary));
        if (!dst->lib) { free(dst); return NULL; }
        memcpy(dst->lib, src->lib, sizeof(MotifLibrary));
    }

    /* 2. Re-base SongEntry.section (points into src->sections[]) and .lib */
    for (int si=0; si<dst->n_songs; si++) {
        Song *song = &dst->songs[si];
        for (int ei=0; ei<song->n_entries; ei++) {
            SongEntry *ent = &song->entries[ei];
            /* Re-base section pointer */
            if (ent->section) {
                ptrdiff_t idx = ent->section - src->sections;
                ent->section = (idx >= 0 && idx < dst->n_sections)
                               ? &dst->sections[idx] : NULL;
            }
            /* Re-base lib pointer */
            ent->lib = dst->lib;
        }
    }

    /* 3. Re-base SectionTrack.patch (points into src->patches[])
     *    and re-resolve MotifUse.resolved_motif via motif_resolve_uses */
    char e[64]="";
    for (int si=0; si<dst->n_sections; si++) {
        Section *sec = &dst->sections[si];
        for (int ti=0; ti<sec->n_tracks; ti++) {
            SectionTrack *trk = &sec->tracks[ti];
            if (trk->patch) {
                ptrdiff_t idx = trk->patch - src->patches;
                trk->patch = (idx >= 0 && idx < dst->n_patches)
                             ? &dst->patches[idx] : NULL;
            }
            motif_resolve_uses(dst->lib, trk->uses, trk->n_uses, e, 64);
        }
    }
    return dst;
}

/* world_destroy: free world and the struct itself */
static void world_destroy(ShmcWorld *w) {
    if (!w) return;
    shmc_world_free(w);
    free(w);
}

/* Preview: first PREVIEW_S seconds only. Uses render_n to avoid
 * allocating/rendering the full song — critical for long seeds. */
static float *render_preview(ShmcWorld *w, int *n_out) {
    float *buf=NULL; int nout=0;
    if (shmc_world_render_n(w, &buf, &nout, SR, PREVIEW_N) < 0 || nout == 0) {
        free(buf); *n_out=0; return NULL;
    }
    *n_out = nout; return buf;
}

/* ═══════════════════════════════════════════════════════════════════
   Audio analysis
   ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    float rms, peak, crest;
    float zcr_brightness;
    float env_std;
    float pitch_diversity;
    float temporal_spread;
    float motif_repetition;
    float rhythm_entropy;
    float fitness;
    float s_audibility, s_env_variety, s_brightness;
    float s_temporal, s_pitch_div, s_dynamics;
    float s_motif_rep, s_rhythm_ent;
    float rms_env[16];
} Analysis;

static void do_analysis(ShmcWorld *w, const WWeights *wt, Analysis *a) {
    memset(a, 0, sizeof(*a));
    int n=0;
    float *audio = render_preview(w, &n);
    if (!audio || n==0) { free(audio); return; }

    /* RMS + peak */
    double s=0; float pk=0;
    for (int i=0;i<n;i++){s+=(double)audio[i]*audio[i];if(fabsf(audio[i])>pk)pk=fabsf(audio[i]);}
    a->rms=(float)sqrt(s/n); a->peak=pk;
    a->crest=(a->rms>1e-7f)?pk/a->rms:1.f;

    /* 16-window envelope */
    int wsz=n/16; if(wsz<2)wsz=2;
    float emean=0;
    for (int wi=0;wi<16;wi++) {
        int ab=wi*wsz, bb=ab+wsz; if(bb>n)bb=n; if(ab>=bb){a->rms_env[wi]=0;continue;}
        double ws=0; for(int i=ab;i<bb;i++) ws+=(double)audio[i]*audio[i];
        a->rms_env[wi]=(float)sqrt(ws/(bb-ab)); emean+=a->rms_env[wi];
    }
    emean/=16;
    float evar=0;
    for (int i=0;i<16;i++){float d=a->rms_env[i]-emean;evar+=d*d;} evar/=16;
    a->env_std=(float)sqrt(evar);

    /* ZCR brightness */
    { int win=256,tzc=0,nw=0;
      for (int off=0;off+win<=n;off+=win/2){
          int zc=0;for(int i=off+1;i<off+win;i++)if((audio[i-1]>=0.f)!=(audio[i]>=0.f))zc++;
          tzc+=zc;nw++;
      }
      a->zcr_brightness=(nw>0)?fminf((float)tzc/nw/((float)win/2.f),1.f):0.f;
    }

    /* Pitch diversity */
    if (w->lib) {
        int seen[128]={0},total=0;
        for (int mi=0;mi<w->lib->n;mi++) {
            if (!w->lib->entries[mi].valid) continue;
            const VoiceProgram *vp=&w->lib->entries[mi].vp;
            for (int ni=0;ni<vp->n;ni++) {
                VInstr vi=vp->code[ni];
                if (VI_OP(vi)==VI_NOTE){seen[VI_PITCH(vi)&127]=1;total++;}
            }
        }
        int uniq=0;for(int i=0;i<128;i++)if(seen[i])uniq++;
        a->pitch_diversity=(total>0)?(float)uniq/total:0.f;

        /* rhythm entropy */
        int dur_counts[7]={0}; int total_dur=0;
        for (int mi=0;mi<w->lib->n;mi++) {
            if (!w->lib->entries[mi].valid) continue;
            const VoiceProgram *vp=&w->lib->entries[mi].vp;
            for (int ni=0;ni<vp->n;ni++) {
                VInstr vi=vp->code[ni];
                if (VI_OP(vi)==VI_NOTE) {
                    int d=(int)VI_DUR(vi);
                    if(d>=0&&d<7){dur_counts[d]++;total_dur++;}
                }
            }
        }
        float entropy=0.f;
        if (total_dur>0) {
            for (int i=0;i<7;i++) {
                if (dur_counts[i]>0) {
                    float p=(float)dur_counts[i]/(float)total_dur;
                    entropy-=p*logf(p);
                }
            }
            entropy/=logf(7.f);
        }
        a->rhythm_entropy=entropy;
    }

    /* Temporal spread + motif repetition */
    if (w->n_sections>0) {
        const Section *sec=&w->sections[0];
        float len=sec->length_beats>0?sec->length_beats:1.f;
        int grid[64]={0};
        int total_uses=0, repeated_uses=0;
        for (int ti=0;ti<sec->n_tracks;ti++) {
            const SectionTrack *trk=&sec->tracks[ti];
            for (int ui=0;ui<trk->n_uses;ui++) {
                int sl=(int)(trk->uses[ui].start_beat/len*64.f);
                if(sl>=0&&sl<64) grid[sl]=1;
                total_uses++;
                if (trk->uses[ui].repeat>1) repeated_uses+=trk->uses[ui].repeat-1;
            }
        }
        int f2=0;for(int i=0;i<64;i++)if(grid[i])f2++;
        a->temporal_spread=(float)f2/64.f;
        a->motif_repetition=(total_uses>0)
            ?fminf((float)repeated_uses/(float)(total_uses+repeated_uses),1.f):0.f;
    }
    free(audio);

    /* Component scores */
    a->s_audibility = 1.f/(1.f+expf(-50.f*(a->rms-0.02f)));
    { float mean=0;for(int i=0;i<16;i++)mean+=a->rms_env[i];mean/=16;
      float var=0;for(int i=0;i<16;i++){float d=a->rms_env[i]-mean;var+=d*d;}var/=16;
      a->s_env_variety=1.f-expf(-500.f*var); }
    float zcr=a->zcr_brightness;
    a->s_brightness=expf(-0.5f*((zcr-0.012f)/0.010f)*((zcr-0.012f)/0.010f));
    a->s_temporal=a->temporal_spread;
    a->s_pitch_div=fminf(a->pitch_diversity*2.f,1.f);
    float dr=a->crest;
    a->s_dynamics=expf(-0.5f*((dr-5.f)/3.f)*((dr-5.f)/3.f));
    a->s_motif_rep=a->motif_repetition;
    a->s_rhythm_ent=a->rhythm_entropy;
    a->fitness = wt->w_audibility*a->s_audibility
               + wt->w_env_variety*a->s_env_variety
               + wt->w_brightness*a->s_brightness
               + wt->w_temporal*a->s_temporal
               + wt->w_pitch_div*a->s_pitch_div
               + wt->w_dynamics*a->s_dynamics
               + wt->w_motif_rep*a->s_motif_rep
               + wt->w_rhythm_ent*a->s_rhythm_ent;
}
/* ═══════════════════════════════════════════════════════════════════
   Reporting
   ═══════════════════════════════════════════════════════════════════ */
static void bar(const char *label, float v, int w) {
    printf("  %-20s [", label);
    int f=(int)(v*w); if(f>w)f=w; if(f<0)f=0;
    for(int i=0;i<w;i++) printf(i<f?"█":"░");
    printf("] %.3f\n",v);
}

static void print_analysis(const Analysis *a, const char *title) {
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  AUDIO ANALYSIS: %-31s║\n", title);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  RMS loudness:    %6.4f                         ║\n", a->rms);
    printf("║  Peak amplitude:  %6.4f                         ║\n", a->peak);
    printf("║  Crest factor:    %6.2f   (target ~5.0)         ║\n", a->crest);
    printf("║  ZCR brightness:  %6.4f   (target ~0.012)       ║\n", a->zcr_brightness);
    printf("║  Env std dev:     %6.4f   (higher=more dynamic) ║\n", a->env_std);
    printf("║  Pitch diversity: %6.4f   (unique/total)        ║\n", a->pitch_diversity);
    printf("║  Temporal spread: %6.4f   (beat grid coverage)  ║\n", a->temporal_spread);
    printf("║  Motif repetition:%6.4f   (repeat-use density)  ║\n", a->motif_repetition);
    printf("║  Rhythm entropy:  %6.4f   (dur variety 0-1)     ║\n", a->rhythm_entropy);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  FITNESS SCORE:   %6.4f                         ║\n", a->fitness);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Component breakdown:                            ║\n");
    bar("audibility(×0.25)",  a->s_audibility,  28);
    bar("env_variety(×0.20)", a->s_env_variety, 28);
    bar("brightness(×0.15)",  a->s_brightness,  28);
    bar("temporal(×0.15)",    a->s_temporal,    28);
    bar("pitch_div(×0.10)",   a->s_pitch_div,   28);
    bar("dynamics(×0.10)",    a->s_dynamics,    28);
    bar("motif_rep(×0.03)",   a->s_motif_rep,   28);
    bar("rhythm_ent(×0.02)",  a->s_rhythm_ent,  28);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  RMS envelope (16 windows, L→R = time):          ║\n  ");
    float mx=0.001f; for(int i=0;i<16;i++) if(a->rms_env[i]>mx) mx=a->rms_env[i];
    const char *blks[]={"▁","▂","▃","▄","▅","▆","▇","█"};
    for(int i=0;i<16;i++){int h=(int)(a->rms_env[i]/mx*8);printf("%s",h>0?blks[h-1]:"▁");}
    printf("\n╚══════════════════════════════════════════════════╝\n");
}

static void print_diagnosis(const Analysis *bef, const Analysis *aft) {
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  EVOLUTION DIAGNOSIS                             ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    float df=aft->fitness-bef->fitness;
    printf("║  Fitness: %.4f → %.4f  (Δ%+.4f) %s            ║\n",
           bef->fitness, aft->fitness, df, df>=0?"▲":"▼");
    struct{const char *n;float b,a,tgt;}m[]={
        {"audibility",  bef->s_audibility,  aft->s_audibility,  0.90f},
        {"env_variety", bef->s_env_variety, aft->s_env_variety, 0.70f},
        {"brightness",  bef->s_brightness,  aft->s_brightness,  0.80f},
        {"temporal",    bef->s_temporal,    aft->s_temporal,    0.15f},
        {"pitch_div",   bef->s_pitch_div,   aft->s_pitch_div,   0.50f},
        {"dynamics",    bef->s_dynamics,    aft->s_dynamics,    0.65f},
        {"motif_rep",   bef->s_motif_rep,   aft->s_motif_rep,   0.30f},
        {"rhythm_ent",  bef->s_rhythm_ent,  aft->s_rhythm_ent,  0.40f},
    };
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Metric        Before  After   Target  Status   ║\n");
    printf("║  ─────────────────────────────────────────────  ║\n");
    for (int i=0;i<8;i++) {
        float d=m[i].a-m[i].b;
        const char *st=m[i].a>=m[i].tgt?"✓ MET ":d>0.01f?"↑ IMP":d<-0.01f?"↓ REG":"── NEU";
        printf("║  %-12s %.3f   %.3f   %.3f   %-8s ║\n",m[i].n,m[i].b,m[i].a,m[i].tgt,st);
    }
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  RECOMMENDATIONS:                                ║\n");
    if(aft->s_audibility<0.90f) printf("║  ⚠ Low RMS %.4f — raise note velocities       ║\n",aft->rms);
    if(aft->s_env_variety<0.50f) printf("║  ⚠ Flat dynamics — vary velocities/durations   ║\n");
    if(aft->zcr_brightness<0.005f) printf("║  ⚠ Very dark (ZCR<0.005) — remove LPF or use noise  ║\n");
    if(aft->zcr_brightness>0.040f) printf("║  ⚠ Harsh (ZCR>0.040) — add lpf stage               ║\n");
    if(aft->s_temporal<0.30f) printf("║  ⚠ Sparse grid — more motif placements          ║\n");
    if(aft->s_pitch_div<0.30f) printf("║  ⚠ Low variety — widen pitch range              ║\n");
    if(aft->s_motif_rep<0.20f) printf("║  ⚠ Low repetition — add x2/x4 repeats to uses  ║\n");
    if(aft->s_rhythm_ent<0.30f) printf("║  ⚠ Monotone rhythm — mix note lengths           ║\n");
    if(aft->crest<2.0f) printf("║  ⚠ Crest<2 — possible clipping                  ║\n");
    if(aft->crest>12.f) printf("║  ⚠ Crest>12 — very transient, add sustain       ║\n");
    if(aft->fitness>=bef->fitness) printf("║  ✓ Evolution improved or maintained fitness      ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
}

/* ═══════════════════════════════════════════════════════════════════
   Search
   ═══════════════════════════════════════════════════════════════════ */
#define HIST_MAX 64
typedef struct { char desc[64]; float fit_after; } HistEntry;
typedef struct { HistEntry e[HIST_MAX]; int n; } History;

static void hist_push(History *h, const char *desc, float fit) {
    if (h->n>=HIST_MAX) {
        memmove(&h->e[0],&h->e[1],(HIST_MAX-1)*sizeof(HistEntry));
        h->n=HIST_MAX-1;
    }
    snprintf(h->e[h->n].desc, 64, "%s", desc);
    h->e[h->n].fit_after=fit; h->n++;
}

typedef struct {
    ShmcWorld *world;  /* heap pointer — always malloc'd */
    float fit;
    WFeat feat;
    uint64_t hash;
    int valid;
} Slot;

static void slot_free(Slot *s) {
    if (s->valid && s->world) { world_destroy(s->world); s->world=NULL; s->valid=0; }
}

#define VS_CAP 8192
typedef struct { uint64_t keys[VS_CAP]; int cnt; } VS;
static void vs_init(VS *v) { memset(v,0,sizeof(*v)); }
static int vs_has(const VS *v, uint64_t h) {
    if(!h)h=1; uint32_t idx=(uint32_t)(h&(VS_CAP-1));
    for(int k=0;k<VS_CAP;k++){uint32_t s=(idx+k)&(VS_CAP-1);if(!v->keys[s])return 0;if(v->keys[s]==h)return 1;}
    return 0;
}
static void vs_add(VS *v, uint64_t h) {
    if(!h)h=1; if(v->cnt>=VS_CAP*3/4) return;
    uint32_t idx=(uint32_t)(h&(VS_CAP-1));
    for(int k=0;k<VS_CAP;k++){uint32_t s=(idx+k)&(VS_CAP-1);if(!v->keys[s]||v->keys[s]==h){v->keys[s]=h;v->cnt++;return;}}
}

/* ═══════════════════════════════════════════════════════════════════
   Novelty Search + MAP-Elites
   ═══════════════════════════════════════════════════════════════════

   BVec: 4D behavior descriptor extracted from WFeat.
     [zcr_brightness, temporal_spread, rhythm_entropy, pitch_diversity]
   These were chosen because they measure independent musical dimensions
   and cannot all be simultaneously maxed by a single degenerate solution.

   Archive: ring-buffer of past BVecs.  Any individual with novelty score
   above ARCHIVE_THRESH is added.  Size capped at ARCHIVE_CAP.

   MapElites: 8×8 grid.  Axes: zcr_cell (0-7) × temporal_cell (0-7).
   Each cell stores the best fitness and world hash seen so far.
   Used for diversity reporting only — not for selection.

   Novelty score: mean L2 distance to K=5 nearest neighbours in archive.
   Combined score: fitness + LAMBDA * novelty.
   Stagnation boost: if best fitness hasn't improved for STAG_GENS,
   raise LAMBDA to LAMBDA_BOOST until progress resumes.
   ═══════════════════════════════════════════════════════════════════ */

#define BVEC_DIM        4
#define ARCHIVE_CAP     512
#define ARCHIVE_THRESH  0.04f   /* novelty threshold to enter archive */
#define NOVELTY_K       5       /* k-nearest neighbours */
#define LAMBDA          0.15f   /* novelty weight in combined score */
#define LAMBDA_BOOST    0.40f   /* boosted novelty weight during stagnation */
#define STAG_GENS       5       /* generations without improvement → stagnation */
#define MAP_DIM         8       /* cells per axis in MAP-Elites grid */

typedef struct {
    float v[BVEC_DIM];  /* zcr_brightness, temporal_spread, rhythm_entropy, pitch_diversity */
} BVec;

typedef struct {
    BVec  entries[ARCHIVE_CAP];
    int   n;            /* current count */
    int   head;         /* ring-buffer write position */
} Archive;

typedef struct {
    float fitness;
    uint64_t hash;      /* 0 = empty cell */
} MapCell;

typedef struct {
    MapCell cells[MAP_DIM][MAP_DIM];  /* [zcr_cell][temporal_cell] */
    int     n_occupied;
} MapElites;

/* Extract behavior vector from WFeat */
static BVec bvec_from_feat(const WFeat *f) {
    BVec b;
    b.v[0] = f->zcr_brightness;
    b.v[1] = f->temporal_spread;
    b.v[2] = f->rhythm_entropy;
    b.v[3] = f->pitch_diversity;
    return b;
}

/* L2 distance between two BVecs */
static float bvec_dist(const BVec *a, const BVec *b) {
    float s = 0.f;
    for (int i = 0; i < BVEC_DIM; i++) {
        float d = a->v[i] - b->v[i]; s += d*d;
    }
    return sqrtf(s);
}

/* Novelty score: mean distance to K nearest neighbours in archive.
 * Returns 0 if archive is empty. */
static float archive_novelty(const Archive *ar, const BVec *q) {
    if (ar->n == 0) return 0.f;
    /* Collect distances, partial-sort to find K smallest */
    float dists[ARCHIVE_CAP];
    for (int i = 0; i < ar->n; i++)
        dists[i] = bvec_dist(q, &ar->entries[i]);
    int k = (ar->n < NOVELTY_K) ? ar->n : NOVELTY_K;
    /* Simple selection for k smallest — O(n*k) fine for n<=512 */
    float sum = 0.f;
    for (int j = 0; j < k; j++) {
        int mi = j;
        for (int i = j+1; i < ar->n; i++)
            if (dists[i] < dists[mi]) mi = i;
        float tmp = dists[j]; dists[j] = dists[mi]; dists[mi] = tmp;
        sum += dists[j];
    }
    return sum / (float)k;
}

static void archive_init(Archive *ar) { ar->n = 0; ar->head = 0; }

/* Add to archive if novelty > threshold */
static void archive_maybe_add(Archive *ar, const BVec *b) {
    float nov = archive_novelty(ar, b);
    if (ar->n < 2 || nov > ARCHIVE_THRESH) {
        ar->entries[ar->head] = *b;
        ar->head = (ar->head + 1) % ARCHIVE_CAP;
        if (ar->n < ARCHIVE_CAP) ar->n++;
    }
}

/* MAP-Elites: update cell for this individual */
static void map_update(MapElites *me, const WFeat *f, float fit, uint64_t hash) {
    /* Cell: zcr_brightness → [0,7], temporal_spread → [0,7] */
    int cx = (int)(f->zcr_brightness / 0.040f * (MAP_DIM-1));
    int cy = (int)(f->temporal_spread * (MAP_DIM-1));
    if (cx < 0) cx = 0; if (cx >= MAP_DIM) cx = MAP_DIM-1;
    if (cy < 0) cy = 0; if (cy >= MAP_DIM) cy = MAP_DIM-1;
    MapCell *cell = &me->cells[cx][cy];
    if (cell->hash == 0) { me->n_occupied++; }
    if (cell->hash == 0 || fit > cell->fitness) {
        cell->fitness = fit;
        cell->hash = hash;
    }
}

static void map_init(MapElites *me) { memset(me, 0, sizeof(*me)); }

/* Print a compact MAP-Elites heatmap to stdout */
static void map_print(const MapElites *me) {
    printf("  MAP-Elites grid (%d/%d cells occupied)\n",
           me->n_occupied, MAP_DIM*MAP_DIM);
    printf("  ZCR(y) × Temporal(x)\n");
    /* Render with block chars — rows = zcr (0=dark, 7=bright), cols = temporal */
    for (int r = MAP_DIM-1; r >= 0; r--) {
        printf("  ZCR%d |", r);
        for (int c = 0; c < MAP_DIM; c++) {
            const MapCell *cell = &me->cells[r][c];
            if (cell->hash == 0) { printf(" .. "); continue; }
            /* Shade by fitness: 0.0-1.0 → ▁▂▃▄▅▆▇█ */
            const char *shd[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
            int idx = (int)(cell->fitness * 7.99f);
            if (idx<0)idx=0; if(idx>7)idx=7;
            printf(" %s%s ", shd[idx], shd[idx]);
        }
        printf("|\n");
    }
    printf("       +");
    for (int c=0;c<MAP_DIM;c++) printf("----");
    printf("\n        ");
    for (int c=0;c<MAP_DIM;c++) printf("T%-3d", c);
    printf("\n");
}

static float score(ShmcWorld *w, const WWeights *wt, WFeat *fo) {
    int n=0; float *a=render_preview(w,&n);
    if (!a||n==0){free(a);return -1.f;}
    WFeat f; wfeat_extract(a,n,SR,w,&f); free(a);
    if (fo)*fo=f; return wfeat_fitness(&f,wt);
}


/*
 * advanced_search -- beam search with novelty archive + MAP-Elites.
 * Returns 0 on success, -1 on error.
 * best_out: caller-allocated ShmcWorld* (must be pre-malloc'd and zeroed).
 */
static int advanced_search(const char *seed_dsl, int max_gen, int beam_w,
                            ShmcWorld *best_out, History *hist, float *fit_out) {
    WWeights wt; wweights_default(&wt);
    uint32_t rng=(uint32_t)time(NULL)^0xDEAD1337;
    int pool_sz=beam_w+beam_w*4;

    Slot      *beam=(Slot*)calloc((size_t)beam_w, sizeof(Slot));
    Slot      *pool=(Slot*)calloc((size_t)pool_sz, sizeof(Slot));
    VS        *vs  =(VS*)calloc(1, sizeof(VS));
    Archive   *ar  =(Archive*)calloc(1, sizeof(Archive));
    MapElites *me  =(MapElites*)calloc(1, sizeof(MapElites));

    if (!beam||!pool||!vs||!ar||!me){
        free(beam);free(pool);free(vs);free(ar);free(me);return -1;
    }
    vs_init(vs); archive_init(ar); map_init(me);

    /* Seed beam */
    {
        char ce[256]="";
        ShmcWorld *seed = world_new(seed_dsl, ce, 256);
        if (!seed) { fprintf(stderr,"Seed compile: %s\n",ce); free(beam);free(pool);free(vs);free(ar);free(me); return -1; }
        shmc_world_canonicalize(seed);
        uint64_t sh=shmc_world_hash(seed);
        vs_add(vs, sh);

        WFeat sf; float sfit=score(seed,&wt,&sf);
        printf("  seed fitness: %.4f\n", sfit);

        /* Prime archive with seed so novelty is meaningful from gen 1 */
        BVec sb = bvec_from_feat(&sf);
        ar->entries[0] = sb; ar->n = 1; ar->head = 1;
        map_update(me, &sf, sfit, sh);

        for (int i=0;i<beam_w;i++) {
            beam[i].world=world_dup(seed);
            if (!beam[i].world) {
                world_destroy(seed);
                for(int j=0;j<i;j++) slot_free(&beam[j]);
                free(beam);free(pool);free(vs);free(ar);free(me); return -1;
            }
            beam[i].feat=sf; beam[i].fit=sfit;
            beam[i].hash=sh; beam[i].valid=1;
        }
        if (fit_out) *fit_out=sfit;
        world_destroy(seed);
    }

    float best_fit=beam[0].fit;
    float last_improved = best_fit;
    int   stag_count = 0;

    for (int gen=0; gen<max_gen; gen++) {
        int pn=0, n_dedup=0, n_struct=0;

        /* Stagnation detection: boost lambda when stuck */
        float lam = (stag_count >= STAG_GENS) ? LAMBDA_BOOST : LAMBDA;
        int   gen_improved = 0;

        for (int bi=0; bi<beam_w&&pn<pool_sz; bi++) {
            if (!beam[bi].valid) continue;

            /* Carry parent forward unchanged */
            pool[pn].world=world_dup(beam[bi].world);
            if (!pool[pn].world) break;
            pool[pn].feat=beam[bi].feat; pool[pn].fit=beam[bi].fit;
            pool[pn].hash=beam[bi].hash; pool[pn].valid=1; pn++;

            /* Generate 4 mutated children */
            for (int m=0; m<4&&pn<pool_sz; m++) {
                Slot *s=&pool[pn];
                s->world=world_dup(beam[bi].world);
                if (!s->world) break;

                MutationRecord rec; memset(&rec,0,sizeof(rec));
                int applied=0;

                rng=rng*6364136223846793005ULL+1442695040888963407ULL;
                int roll=(int)((rng>>24)&0xFF);

                for (int t=0; t<8&&!applied; t++) {
                    rng=rng*6364136223846793005ULL+1442695040888963407ULL;
                    if (roll < (255*15/100)) {
                        applied=shmc_patch_struct_mutate_tracked(s->world,&rng,&rec);
                        if(applied) n_struct++;
                    } else if (roll < (255*25/100)) {
                        applied=shmc_mutate_structural_tracked(s->world,MUTATE_STRUCTURAL,&rng,&rec);
                        if(applied) n_struct++;
                    } else {
                        applied=shmc_mutate_tracked(s->world,MUTATE_ANY,&rng,&rec);
                    }
                }
                if (!applied) { world_destroy(s->world); s->world=NULL; continue; }

                /* Canonicalize + world-hash dedup */
                shmc_world_canonicalize(s->world);
                s->hash=shmc_world_hash(s->world);
                if (vs_has(vs,s->hash)) {
                    mut_record_free(&rec);
                    world_destroy(s->world); s->world=NULL;
                    n_dedup++; continue;
                }
                vs_add(vs, s->hash);

                WFeat mf; float mfit=score(s->world,&wt,&mf);
                if (mfit<0.f) { mut_record_free(&rec); world_destroy(s->world); s->world=NULL; continue; }

                /* compute novelty, update archive + MAP */
                BVec bv = bvec_from_feat(&mf);
                float nov = archive_novelty(ar, &bv);
                float combined = mfit + lam * nov;
                archive_maybe_add(ar, &bv);
                map_update(me, &mf, mfit, s->hash);

                /* Beam selection uses combined score */
                s->feat=mf; s->fit=combined; s->valid=1;

                if (mfit > last_improved) {
                    last_improved = mfit;
                    if (mfit > best_fit) best_fit = mfit;
                    gen_improved = 1;
                    if (hist) hist_push(hist, rec.desc, mfit);
                }
                mut_record_free(&rec);
                pn++;
            }
        }

        /* Sort pool descending by combined score */
        for (int i=0; i<beam_w&&i<pn; i++) {
            int best=i;
            for(int j=i+1;j<pn;j++) if(pool[j].fit>pool[best].fit) best=j;
            if(best!=i){Slot tmp=pool[i];pool[i]=pool[best];pool[best]=tmp;}
        }

        stag_count = gen_improved ? 0 : stag_count+1;

        printf("  gen %2d: fit=%.4f  pool=%d dedup=%d struct=%d  arch=%d map=%d  lam=%.2f%s\n",
               gen+1, best_fit, pn, n_dedup, n_struct, ar->n, me->n_occupied, lam,
               stag_count>=STAG_GENS?" [STAG]":"");

        /* Swap beam <- top-K pool */
        for (int i=0;i<beam_w;i++) slot_free(&beam[i]);
        for (int i=0;i<beam_w;i++) {
            if (i<pn&&pool[i].valid) {
                beam[i]=pool[i]; pool[i].valid=0; pool[i].world=NULL;
            } else {
                char ce2[64]="";
                beam[i].world=world_new(seed_dsl, ce2, 64);
                beam[i].fit=0.f; beam[i].valid=beam[i].world?1:0;
            }
        }
        for (int i=beam_w; i<pn; i++) slot_free(&pool[i]);

        if (best_fit>=0.95f) break;
    }

    /* Print MAP-Elites diversity grid + archive summary */
    printf("\n");
    map_print(me);
    printf("\n  Novelty archive: %d/%d entries  (threshold=%.3f)\n",
           ar->n, ARCHIVE_CAP, ARCHIVE_THRESH);

    free(ar); free(me);

    for (int i=0;i<beam_w;i++) slot_free(&beam[i]);
    free(beam); free(pool); free(vs);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    const char *dsl_path = argc>1?argv[1]:NULL;
    int max_gen  = argc>2?atoi(argv[2]):20;
    int beam_w   = argc>3?atoi(argv[3]):6;

    if (!dsl_path) {
        fprintf(stderr,"Usage: %s <seed.dsl> [generations] [beam_width]\n",argv[0]);
        return 1;
    }

    tables_init();

    char *dsl = read_file(dsl_path);
    if (!dsl) return 1;

    printf("\n════════════════════════════════════════════════════\n");
    printf("  SHMC Evolutionary Search + Audio Analysis  v3\n");
    printf("  Seed: %-44s\n", dsl_path);
    printf("  Gens: %-3d  Beam: %-3d  Preview: %.0fs\n", max_gen, beam_w, PREVIEW_S);
    printf("════════════════════════════════════════════════════\n");

    WWeights wt; wweights_default(&wt);

    /* ── Phase 1: Analyse seed ── */
    printf("\n[1/4] Rendering and analysing seed...\n");
    char err[256]="";
    ShmcWorld *wseed = world_new(dsl, err, 256);
    if (!wseed) { fprintf(stderr,"Compile: %s\n",err); free(dsl); return 1; }

    /* Full render for WAV */
    float *full_seed=NULL; int nfull=0;
    shmc_world_render(wseed, &full_seed, &nfull, SR);
    write_wav("/mnt/user-data/outputs/seed.wav", full_seed, nfull, SR);
    printf("  seed.wav: %d samples (%.1fs)\n", nfull, (float)nfull/SR);
    free(full_seed);

    Analysis before; do_analysis(wseed, &wt, &before);
    print_analysis(&before, "SEED (3s preview)");
    world_destroy(wseed); wseed=NULL;

    /* ── Phase 2: Run search ── */
    printf("\n[2/4] Running advanced beam search (%d gens × beam %d)...\n", max_gen, beam_w);

    /* best is heap-allocated — no stack-allocated ShmcWorld */
    ShmcWorld *best = (ShmcWorld*)calloc(1, sizeof(ShmcWorld));
    if (!best) { free(dsl); return 1; }
    History hist; hist.n=0;
    float best_fit=0;

    if (advanced_search(dsl, max_gen, beam_w, best, &hist, &best_fit)<0) {
        fprintf(stderr,"Search failed\n"); free(best); free(dsl); return 1;
    }

    /* ── Phase 3: Analyse evolved ── */
    printf("\n[3/4] Rendering and analysing evolved program...\n");
    float *full_evol=NULL; int nevol=0;
    shmc_world_render(best, &full_evol, &nevol, SR);
    write_wav("/mnt/user-data/outputs/evolved.wav", full_evol, nevol, SR);
    printf("  evolved.wav: %d samples (%.1fs)\n", nevol, (float)nevol/SR);
    free(full_evol);

    Analysis after; do_analysis(best, &wt, &after);
    print_analysis(&after, "EVOLVED (3s preview)");

    /* ── Phase 4: Diagnosis + history ── */
    printf("\n[4/4] Diagnosis and mutation history...\n");
    print_diagnosis(&before, &after);

    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  MUTATION HISTORY  (best-improving, %2d entries) ║\n", hist.n);
    printf("╚══════════════════════════════════════════════════╝\n");
    for (int i=0;i<hist.n;i++)
        printf("  %2d: [fit=%.4f] %s\n", i+1, hist.e[i].fit_after, hist.e[i].desc);

    /* ── Phase 5: Emit evolved DSL ── */
    char *edsl=(char*)malloc(131072);
    if (edsl) {
        int nb=shmc_world_to_dsl(best, edsl, 131072);
        if (nb>0) {
            FILE *f=fopen("/mnt/user-data/outputs/evolved.dsl","w");
            if (f){fwrite(edsl,1,(size_t)nb,f);fclose(f);}
            printf("\n  evolved.dsl: %d bytes\n", nb);
        }
        free(edsl);
    }

    /* Cleanup best — shmc_world_free the lib, then free the struct */
    shmc_world_free(best);
    free(best);
    free(dsl);

    printf("\n════════════════════════════════════════════════════\n");
    printf("  DONE — best fitness: %.4f\n", best_fit);
    printf("════════════════════════════════════════════════════\n\n");
    return 0;
}
