/*
 * SHMC Beam Search v4 — shmc_search.c
 *
 * Root cause of all previous ASAN crashes:
 *   ShmcWorld (693KB) was stack-allocated inside shmc_search_run.
 *   After world_copy(), SectionTrack.patch still pointed into the
 *   old stack frame. After that frame exits → use-after-scope crash.
 *
 * Fix: Slot stores ShmcWorld* (heap pointer), never inline.
 *   world_alloc_copy() mallocs a fresh ShmcWorld and fixes ALL internal
 *   pointers: lib, song entries, SectionTrack.patch (re-indexed via
 *   ptr arithmetic into src->patches[]), motif_resolve_uses().
 *
 * Features:
 *   - Structural patch mutations (PATCH_STRUCT_ANY) 15% of mutations
 *   - World canonicalization before hash dedup
 *   - 8192-slot visited set (open addressing)
 *   - Preview render (3s) for fast fitness scoring
 *   - Seed reinject on beam exhaustion
 */
#include "../include/shmc_search.h"
#include "../include/shmc_canon.h"
#include "../include/shmc_patch_mutate.h"
#include "../include/shmc_mut_algebra.h"
#include "../../layer0b/include/shmc_hash.h"
#include "../../layer0/include/patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PREVIEW_SEC   3.0f
#define MAX_MUT_TRIES 8
#define STRUCT_PCT    15
#define VS_CAP        8192

/* ── World heap management ──────────────────────────────────────── */

/*
 * world_alloc_copy: malloc a new ShmcWorld, deep-copy src into it,
 * fix ALL internal pointers so no reference into src remains.
 * Returns NULL on OOM.
 */
static ShmcWorld *world_alloc_copy(const ShmcWorld *src) {
    ShmcWorld *dst = (ShmcWorld *)malloc(sizeof(ShmcWorld));
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(ShmcWorld));

    /* 1. Fresh lib copy */
    dst->lib = NULL;
    if (src->lib) {
        dst->lib = (MotifLibrary *)malloc(sizeof(MotifLibrary));
        if (!dst->lib) { free(dst); return NULL; }
        memcpy(dst->lib, src->lib, sizeof(MotifLibrary));
    }

    /* 2. Re-point song entries: lib + section pointers */
    for (int si = 0; si < dst->n_songs; si++) {
        for (int ei = 0; ei < dst->songs[si].n_entries; ei++) {
            SongEntry *se = &dst->songs[si].entries[ei];
            se->lib = dst->lib;
            /* Rebase section ptr: src->sections[] -> dst->sections[] */
            if (se->section) {
                ptrdiff_t sidx = se->section - src->sections;
                se->section = (sidx>=0 && sidx<src->n_sections)
                            ? &dst->sections[sidx] : NULL;
            }
        }
    }

    /* 3. Fix section track pointers */
    char e[64] = "";
    for (int si = 0; si < dst->n_sections; si++) {
        Section *sec = &dst->sections[si];
        for (int ti = 0; ti < sec->n_tracks; ti++) {
            SectionTrack *trk = &sec->tracks[ti];

            /* Re-base patch ptr: src->patches[] → dst->patches[] */
            if (trk->patch) {
                ptrdiff_t idx = trk->patch - src->patches;
                if (idx >= 0 && idx < src->n_patches)
                    trk->patch = &dst->patches[idx];
                else
                    trk->patch = NULL;
            }

            /* Re-resolve motif pointers in uses */
            motif_resolve_uses(dst->lib, trk->uses, trk->n_uses, e, 64);
        }
    }
    return dst;
}

/* world_alloc_compile: compile DSL into a fresh heap world. NULL on error. */
static ShmcWorld *world_alloc_compile(const char *dsl, char *err, int esz) {
    ShmcWorld *w = (ShmcWorld *)malloc(sizeof(ShmcWorld));
    if (!w) return NULL;
    if (shmc_dsl_compile(dsl, w, err, esz) < 0) { free(w); return NULL; }
    return w;
}

static void world_free_heap(ShmcWorld *w) {
    if (!w) return;
    shmc_world_free(w);  /* frees w->lib */
    free(w);
}

/* ── Slot — stores heap pointer, never inline world ────────────── */

typedef struct {
    ShmcWorld *w;   /* heap-allocated; NULL if invalid */
    WFeat      feat;
    float      fit;
    uint64_t   hash;
} Slot;

static void slot_drop(Slot *s) {
    world_free_heap(s->w);
    s->w = NULL;
}

/* ── Visited set ────────────────────────────────────────────────── */

typedef struct { uint64_t keys[VS_CAP]; int count; } VS;
static void vs_init(VS *v) { memset(v, 0, sizeof(*v)); }
static int vs_has(const VS *v, uint64_t h) {
    if (!h) h = 1;
    uint32_t idx = (uint32_t)(h & (VS_CAP-1));
    for (int i = 0; i < VS_CAP; i++) {
        uint32_t s = (idx+i) & (VS_CAP-1);
        if (!v->keys[s]) return 0;
        if (v->keys[s] == h) return 1;
    }
    return 0;
}
static void vs_add(VS *v, uint64_t h) {
    if (!h) h = 1;
    if (v->count >= VS_CAP*3/4) return;
    uint32_t idx = (uint32_t)(h & (VS_CAP-1));
    for (int i = 0; i < VS_CAP; i++) {
        uint32_t s = (idx+i) & (VS_CAP-1);
        if (!v->keys[s] || v->keys[s]==h) { v->keys[s]=h; v->count++; return; }
    }
}

/* ── Feature extraction ─────────────────────────────────────────── */

void wfeat_extract(const float *audio, int n, float sr,
                   const ShmcWorld *w, WFeat *out) {
    memset(out, 0, sizeof(*out));
    if (n <= 0 || !audio) return;

    double s = 0;
    for (int i = 0; i < n; i++) s += (double)audio[i]*audio[i];
    out->rms = (float)sqrt(s / n);

    int wsz = n / SEARCH_FEAT_WIN; if (wsz < 2) wsz = 2;
    for (int wi = 0; wi < SEARCH_FEAT_WIN; wi++) {
        int a = wi*wsz, b = a+wsz; if (b>n) b=n; if (a>=b) { out->rms_env[wi]=0; continue; }
        double ws = 0;
        for (int i = a; i < b; i++) ws += (double)audio[i]*audio[i];
        out->rms_env[wi] = (float)sqrt(ws/(b-a));
    }

    { int win=256, tzc=0, nw=0;
      for (int off=0; off+win<=n; off+=win/2) {
          int zc=0;
          for (int i=off+1; i<off+win; i++)
              if ((audio[i-1]>=0.f) != (audio[i]>=0.f)) zc++;
          tzc+=zc; nw++;
      }
      out->zcr_brightness = nw>0 ? fminf((float)tzc/nw/((float)win/2.f),1.f) : 0.f;
    }

    float pk = 0;
    for (int i = 0; i < n; i++) if (fabsf(audio[i])>pk) pk=fabsf(audio[i]);
    out->dynamic_range = out->rms>1e-6f ? pk/out->rms : 1.f;

    if (w && w->lib) {
        int seen[128]={0}, total=0;
        for (int mi=0; mi<w->lib->n; mi++) {
            if (!w->lib->entries[mi].valid) continue;
            const VoiceProgram *vp = &w->lib->entries[mi].vp;
            for (int ni=0; ni<vp->n; ni++) {
                VInstr vi = vp->code[ni];
                if (VI_OP(vi)==VI_NOTE) { seen[VI_PITCH(vi)&127]=1; total++; }
            }
        }
        int uniq=0; for(int i=0;i<128;i++) if(seen[i]) uniq++;
        out->pitch_diversity = total>0 ? (float)uniq/total : 0.f;
    }

    if (w && w->n_sections>0) {
        const Section *sec = &w->sections[0];
        float len = sec->length_beats>0 ? sec->length_beats : 1.f;
        int grid[64]={0};
        for (int ti=0; ti<sec->n_tracks; ti++) {
            const SectionTrack *trk = &sec->tracks[ti];
            for (int ui=0; ui<trk->n_uses; ui++) {
                int sl = (int)(trk->uses[ui].start_beat/len*64.f);
                if (sl>=0 && sl<64) grid[sl]=1;
            }
        }
        int f2=0; for(int i=0;i<64;i++) if(grid[i]) f2++;
        out->temporal_spread = (float)f2/64.f;

        /* motif_repetition — how much reuse happens across uses.
         * Count total uses and uses where the same motif name appears >1 time. */
        int total_uses = 0, repeated_uses = 0;
        for (int ti=0; ti<sec->n_tracks; ti++) {
            const SectionTrack *trk = &sec->tracks[ti];
            for (int ui=0; ui<trk->n_uses; ui++) {
                total_uses++;
                if (trk->uses[ui].repeat > 1)
                    repeated_uses += trk->uses[ui].repeat - 1;
            }
        }
        out->motif_repetition = total_uses>0
            ? fminf((float)repeated_uses / (float)(total_uses + repeated_uses), 1.f)
            : 0.f;
    }

    /* rhythm_entropy — Shannon entropy of dur distribution in motifs.
     * High entropy = many different durations used = more rhythmic variety. */
    if (w && w->lib) {
        int dur_counts[7] = {0}; int total_dur = 0;
        for (int mi=0; mi<w->lib->n; mi++) {
            if (!w->lib->entries[mi].valid) continue;
            const VoiceProgram *vp = &w->lib->entries[mi].vp;
            for (int ni=0; ni<vp->n; ni++) {
                VInstr vi = vp->code[ni];
                if (VI_OP(vi)==VI_NOTE) {
                    int d = (int)VI_DUR(vi);
                    if (d>=0 && d<7) { dur_counts[d]++; total_dur++; }
                }
            }
        }
        float entropy = 0.f;
        if (total_dur > 0) {
            for (int i=0; i<7; i++) {
                if (dur_counts[i] > 0) {
                    float p = (float)dur_counts[i] / (float)total_dur;
                    entropy -= p * logf(p);
                }
            }
            entropy /= logf(7.f);  /* normalize to [0,1] */
        }
        out->rhythm_entropy = entropy;
    }
}

/* ── Weights / fitness ──────────────────────────────────────────── */

void wweights_default(WWeights *out) {
    out->w_audibility  = 0.25f;   /* -0.05 to make room for symbolic terms */
    out->w_env_variety = 0.20f;
    out->w_brightness  = 0.15f;
    out->w_temporal    = 0.15f;
    out->w_pitch_div   = 0.10f;
    out->w_dynamics    = 0.10f;
    out->w_motif_rep   = 0.03f;   /* motif repetition bonus */
    out->w_rhythm_ent  = 0.02f;   /* rhythm entropy bonus */
}

float wfeat_fitness(const WFeat *f, const WWeights *w) {
    float au = 1.f/(1.f+expf(-50.f*(f->rms-0.02f)));
    float mean=0;
    for (int i=0; i<SEARCH_FEAT_WIN; i++) mean+=f->rms_env[i];
      mean/=SEARCH_FEAT_WIN;
    float var=0;
    for (int i=0; i<SEARCH_FEAT_WIN; i++) { float d=f->rms_env[i]-mean; var+=d*d; } var/=SEARCH_FEAT_WIN;
    float ev = 1.f - expf(-500.f*var);
    float zcr = f->zcr_brightness;
    float br = expf(-0.5f*((zcr-0.012f)/0.010f)*((zcr-0.012f)/0.010f));
    float pi = fminf(f->pitch_diversity*2.f,1.f);
    float dr = f->dynamic_range;
    float dy = expf(-0.5f*((dr-5.f)/3.f)*((dr-5.f)/3.f));
    return w->w_audibility*au + w->w_env_variety*ev + w->w_brightness*br
         + w->w_temporal*f->temporal_spread + w->w_pitch_div*pi + w->w_dynamics*dy
         + w->w_motif_rep*f->motif_repetition + w->w_rhythm_ent*f->rhythm_entropy;
}

void search_cfg_default(ShmcSearchCfg *cfg, const char *seed_dsl, uint32_t seed) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->seed_dsl = seed_dsl;
    cfg->beam_width = SEARCH_BEAM_W;
    cfg->max_generations = SEARCH_MAX_GEN;
    cfg->muts_per_cand = SEARCH_MUTS_PER_CAND;
    cfg->sr = SEARCH_SR;
    cfg->seed = seed;
    wweights_default(&cfg->weights);
}

/* ── Scoring ────────────────────────────────────────────────────── */

static int render_preview(ShmcWorld *w, float sr, float **ao, int *no) {
    int pn = (int)(PREVIEW_SEC*sr);
    float *buf=NULL; int nout=0;
    if (shmc_world_render_n(w,&buf,&nout,sr,pn)<0 || nout==0) { free(buf); return -1; }
    *ao=buf; *no=nout; return 0;
}

static float score_world(ShmcWorld *w, const WWeights *wt, WFeat *fo, float sr) {
    float *a=NULL; int n=0;
    if (render_preview(w,sr,&a,&n)<0) return -1.f;
    WFeat f; wfeat_extract(a,n,sr,w,&f); free(a);
    if (fo) *fo=f;
    return wfeat_fitness(&f,wt);
}

/* ── Selection sort top-K ───────────────────────────────────────── */

static void top_k(Slot *pool, int n, int k) {
    for (int i=0; i<k&&i<n; i++) {
        int best=i;
        for (int j=i+1; j<n; j++) if (pool[j].fit>pool[best].fit) best=j;
        if (best!=i) { Slot tmp=pool[i]; pool[i]=pool[best]; pool[best]=tmp; }
    }
}

/* ── Main search ────────────────────────────────────────────────── */

int shmc_search_run(const ShmcSearchCfg *cfg, ShmcSearchResult *result,
                    char *err, int err_sz) {
    memset(result, 0, sizeof(*result));
    const int   BW   = cfg->beam_width>0      ? cfg->beam_width      : SEARCH_BEAM_W;
    const int   MAXG = cfg->max_generations>0 ? cfg->max_generations : SEARCH_MAX_GEN;
    const int   MUTS = cfg->muts_per_cand>0   ? cfg->muts_per_cand   : SEARCH_MUTS_PER_CAND;
    const float SR   = cfg->sr>0              ? cfg->sr              : SEARCH_SR;

    int pool_sz = BW + BW*MUTS;
    Slot *beam = (Slot *)calloc((size_t)BW,      sizeof(Slot));
    Slot *pool = (Slot *)calloc((size_t)pool_sz, sizeof(Slot));
    VS   *vs   = (VS *)  calloc(1, sizeof(VS));
    if (!beam||!pool||!vs) {
        free(beam); free(pool); free(vs);
        snprintf(err,err_sz,"OOM"); return -1;
    }
    vs_init(vs);
    uint32_t rng = cfg->seed;

    /* ── Seed beam from DSL ── */
    {
        char ce[256]="";
        ShmcWorld *seed = world_alloc_compile(cfg->seed_dsl, ce, 256);
        if (!seed) {
            snprintf(err,err_sz,"seed compile: %s",ce);
            free(beam); free(pool); free(vs); return -1;
        }
        WFeat sf; float sfit = score_world(seed,&cfg->weights,&sf,SR);
        result->total_renders++;
        shmc_world_canonicalize(seed);
        uint64_t sh = shmc_world_hash(seed);
        vs_add(vs, sh);

        for (int i=0; i<BW; i++) {
            beam[i].w = world_alloc_copy(seed);
            if (!beam[i].w) {
                snprintf(err,err_sz,"OOM seeding beam");
                world_free_heap(seed);
                for(int j=0;j<i;j++) slot_drop(&beam[j]);
                free(beam); free(pool); free(vs); return -1;
            }
            beam[i].feat=sf; beam[i].fit=sfit; beam[i].hash=sh;
        }
        result->best_fitness = sfit;
        world_free_heap(seed);
    }

    /* ── Main evolution loop ── */
    for (int gen=0; gen<MAXG; gen++) {
        int pn=0, n_dedup=0;

        for (int bi=0; bi<BW && pn<pool_sz; bi++) {
            if (!beam[bi].w) continue;

            /* Carry parent forward unchanged */
            pool[pn].w = world_alloc_copy(beam[bi].w);
            if (!pool[pn].w) break;
            pool[pn].feat=beam[bi].feat; pool[pn].fit=beam[bi].fit;
            pool[pn].hash=beam[bi].hash; pn++;

            /* Generate MUTS mutated children */
            for (int m=0; m<MUTS && pn<pool_sz; m++) {
                Slot *s = &pool[pn];
                s->w = world_alloc_copy(beam[bi].w);
                if (!s->w) break;

                int applied = 0;
                for (int t=0; t<MAX_MUT_TRIES && !applied; t++) {
                    rng = rng*6364136223846793005ULL + 1442695040888963407ULL;
                    int roll = (int)((rng>>24)&0xFF);
                    if (roll < (255*STRUCT_PCT/100) && s->w->n_patches>0) {
                        /* 15%: structural patch mutation */
                        int pi = (int)(rng % (uint32_t)s->w->n_patches);
                        applied = shmc_patch_struct_mutate(&s->w->patches[pi], PATCH_STRUCT_ANY, &rng);
                    } else if (roll < (255*(STRUCT_PCT+10)/100) && s->w->lib && s->w->lib->n>0) {
                        /* 10%: structural motif mutation (invert/retrograde/augment/diminish/add/del) */
                        applied = shmc_mutate(s->w, MUTATE_STRUCTURAL, &rng);
                    } else {
                        /* 75%: parameter mutations */
                        int nm = 1 + (int)((rng>>31)&1);
                        rng = rng*6364136223846793005ULL + 1442695040888963407ULL;
                        for (int k=0; k<nm; k++) { int r=shmc_mutate(s->w,MUTATE_ANY,&rng); if(r) applied=1; }
                    }
                }
                if (!applied) { slot_drop(s); continue; }
                result->total_mutations++;

                shmc_world_canonicalize(s->w);
                uint64_t mh = shmc_world_hash(s->w);
                if (vs_has(vs,mh)) { slot_drop(s); n_dedup++; continue; }
                vs_add(vs, mh);

                WFeat mf; float mfit = score_world(s->w,&cfg->weights,&mf,SR);
                result->total_renders++;
                if (mfit < 0.f) { slot_drop(s); continue; }
                s->feat=mf; s->fit=mfit; s->hash=mh; pn++;
            }
        }

        top_k(pool, pn, BW);
        if (pn>0 && pool[0].fit > result->best_fitness)
            result->best_fitness = pool[0].fit;

        /* Replace beam with top-BW from pool */
        for (int i=0; i<BW; i++) slot_drop(&beam[i]);
        for (int i=0; i<BW; i++) {
            if (i<pn && pool[i].w) {
                beam[i] = pool[i]; pool[i].w = NULL;
            } else {
                /* Reinject seed for diversity */
                char ce2[64]="";
                beam[i].w = world_alloc_compile(cfg->seed_dsl, ce2, 64);
                beam[i].fit = 0.f;
            }
        }
        for (int i=BW; i<pn; i++) slot_drop(&pool[i]);

        result->generations_run = gen+1;
        result->n_dedup_skipped += n_dedup;
        if (cfg->progress_cb)
            cfg->progress_cb(gen+1, result->best_fitness, cfg->userdata);
        if (result->best_fitness >= 0.95f) break;
    }

    /* Return best world */
    if (BW>0 && beam[0].w) {
        result->best_fitness = beam[0].fit;
        /* world_copy_resolved into the inline result field */
        memcpy(&result->best_world, beam[0].w, sizeof(ShmcWorld));
        /* We need a fresh lib copy since beam[0].w->lib will be freed */
        result->best_world.lib = NULL;
        if (beam[0].w->lib) {
            result->best_world.lib = (MotifLibrary*)malloc(sizeof(MotifLibrary));
            if (result->best_world.lib) {
                memcpy(result->best_world.lib, beam[0].w->lib, sizeof(MotifLibrary));
                /* Fix song entry lib pointers */
                for (int si=0; si<result->best_world.n_songs; si++)
                    for (int ei=0; ei<result->best_world.songs[si].n_entries; ei++)
                        result->best_world.songs[si].entries[ei].lib = result->best_world.lib;
                /* Fix section track patch + motif pointers */
                char e[64]="";
                for (int si=0; si<result->best_world.n_sections; si++) {
                    Section *sec = &result->best_world.sections[si];
                    for (int ti=0; ti<sec->n_tracks; ti++) {
                        SectionTrack *trk = &sec->tracks[ti];
                        if (trk->patch) {
                            ptrdiff_t idx = trk->patch - beam[0].w->patches;
                            trk->patch = (idx>=0&&idx<result->best_world.n_patches)
                                       ? &result->best_world.patches[idx] : NULL;
                        }
                        motif_resolve_uses(result->best_world.lib, trk->uses, trk->n_uses, e, 64);
                    }
                }
            }
        }
        result->best_world_valid = 1;
    }

    for (int i=0; i<BW; i++) slot_drop(&beam[i]);
    free(beam); free(pool); free(vs);
    return 0;
}
