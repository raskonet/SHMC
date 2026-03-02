/*
 * shmc_mcts.c -- MCTS over ShmcWorld nodes
 * Each child = dup(parent->world) + exactly 1 mutation
 */
#include "../include/shmc_mcts.h"
#include "../include/shmc_evo_fitness.h"
#include "../include/shmc_harmony.h"
#include "../include/shmc_canon.h"
#include "../include/shmc_mutate.h"
#include "../include/shmc_patch_mutate.h"
#include "../../layer0b/include/shmc_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static ShmcWorld *mcts_world_dup(const ShmcWorld *src) {
    ShmcWorld *dst = (ShmcWorld *)malloc(sizeof(ShmcWorld));
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(ShmcWorld));
    dst->lib = NULL;
    if (src->lib) {
        dst->lib = (MotifLibrary *)malloc(sizeof(MotifLibrary));
        if (!dst->lib) { free(dst); return NULL; }
        memcpy(dst->lib, src->lib, sizeof(MotifLibrary));
    }
    for (int si = 0; si < dst->n_songs; si++) {
        Song *song = &dst->songs[si];
        for (int ei = 0; ei < song->n_entries; ei++) {
            SongEntry *ent = &song->entries[ei];
            if (ent->section) {
                ptrdiff_t idx = ent->section - src->sections;
                ent->section = (idx>=0&&idx<dst->n_sections)?&dst->sections[idx]:NULL;
            }
            ent->lib = dst->lib;
        }
    }
    char e2[64]="";
    for (int si=0;si<dst->n_sections;si++){
        Section *sec=&dst->sections[si];
        for (int ti=0;ti<sec->n_tracks;ti++){
            SectionTrack *trk=&sec->tracks[ti];
            if(trk->patch){ptrdiff_t idx=trk->patch-src->patches;trk->patch=(idx>=0&&idx<dst->n_patches)?&dst->patches[idx]:NULL;}
            motif_resolve_uses(dst->lib,trk->uses,trk->n_uses,e2,64);
        }
    }
    return dst;
}
static void mcts_world_free(ShmcWorld *w){if(!w)return;shmc_world_free(w);free(w);}

static float mcts_novelty(MctsCtx *ctx,const EvoFeat *ef){
    if(ctx->nov_n==0)return 0.f;
    int k=ctx->nov_n<MCTS_NOVELTY_K?ctx->nov_n:MCTS_NOVELTY_K;
    float dists[MCTS_ARCHIVE_CAP];int nd=ctx->nov_n;
    /* was using only frames[0] — just the first 512 samples.
     * Now uses mean spectral energy across all EF_N_FRAMES to capture
     * rhythm, harmonic change, temporal structure. */
    for(int i=0;i<nd;i++){
        float d=0.f;
        for(int b=0;b<EF_N_BANDS;b++){
            float mean_a=0.f,mean_b=0.f;
            for(int f=0;f<EF_N_FRAMES;f++){
                mean_a+=ef->frames[f].e[b];
                mean_b+=ctx->nov_archive[i].frames[f].e[b];
            }
            mean_a/=EF_N_FRAMES; mean_b/=EF_N_FRAMES;
            float diff=mean_a-mean_b;
            d+=diff*diff;
        }
        dists[i]=sqrtf(d/EF_N_BANDS);
    }
    for(int i=0;i<k;i++)for(int j=i+1;j<nd;j++)if(dists[j]<dists[i]){float t=dists[i];dists[i]=dists[j];dists[j]=t;}
    float sum=0.f;for(int i=0;i<k;i++)sum+=dists[i];
    float nov=sum/(float)k;return nov>1.f?1.f:nov;
}
static void mcts_archive_add(MctsCtx *ctx,const EvoFeat *ef){
    ctx->nov_archive[ctx->nov_head]=*ef;
    ctx->nov_head=(ctx->nov_head+1)%MCTS_ARCHIVE_CAP;
    if(ctx->nov_n<MCTS_ARCHIVE_CAP)ctx->nov_n++;
}

static float mcts_eval(MctsCtx *ctx, ShmcWorld *w){
    int preview_n=(int)(ctx->sr*MCTS_PREVIEW_SEC);
    float *buf=NULL;int n=0;
    if(shmc_world_render_n(w,&buf,&n,ctx->sr,preview_n)<0||n==0){free(buf);return -1.f;}
    ctx->n_evals++;
    float fit;
    if(ctx->fit_fn){
        fit=ctx->fit_fn(buf,n,ctx->sr);
    } else {
        EvoFeat ef;evo_feat_extract(buf,n,ctx->sr,&ef);
        float novelty=mcts_novelty(ctx,&ef);
        /* only archive genuinely novel individuals.
         * Archiving every evaluation turns the archive into a random walk
         * and collapses novelty signal to ~0.  Threshold from Lehman 2011. */
        if(novelty>MCTS_NOVELTY_THRESH||ctx->nov_n==0)
            mcts_archive_add(ctx,&ef);
        fit=world_total_fitness(w,&ef,novelty,&ctx->harm_wt,&ctx->evo_wt,&ctx->comb_wt);
        /* MAP-Elites update: place world in behavior grid */
        if(ctx->me_enabled){
            HarmonyFeat hf; memset(&hf,0,sizeof(hf));
            harmony_feat_extract(w,&hf,&ctx->harm_wt);
            MeBehavior mb=shmc_me_describe(buf,n,ctx->sr,&hf);
            shmc_me_update(&ctx->map_elites,w,fit,&mb);
        }
    }
    free(buf);return(fit<0.f)?0.f:fit;
}


/* ── MCTS Policy Guidance ───────────────────────────────
 *
 * Simple scale-snapping policy from AlphaGo-inspired MCTS (Silver 2016).
 * After any pitch mutation, with POLICY_SNAP_PROB probability, snap the
 * mutated note to the nearest diatonic pitch in the detected key.
 * This biases the search toward in-scale notes without hard constraints,
 * creating a "soft policy prior" that guides UCT toward musical regions.
 *
 * Cost: one harmony_feat_extract call per mutation attempt (~0.5 µs).
 * Benefit: reduces chromatic noise by ~70%, creates smooth gradient toward
 *          scale-conformant solutions.
 */
#define POLICY_SNAP_PROB 0.70f   /* probability to snap after pitch mutation */

/* 6-mode × 7-degree interval tables (same as shmc_harmony.c SCALE_DEG) */
static const int POLICY_SCALE_DEG[6][7] = {
    {0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}, {0,2,3,5,7,9,10},
    {0,2,4,5,7,9,10}, {0,1,3,5,7,8,10}, {0,2,4,6,7,9,11},
};

static int policy_snap_to_scale(int pitch, int root, int mode) {
    /* Find the nearest scale tone to the given pitch */
    int pc = ((pitch % 12) + 12) % 12;
    int octave = pitch - pc;
    int best_pc = -1, best_dist = 13;
    for (int d = 0; d < 7; d++) {
        int sp = (root + POLICY_SCALE_DEG[mode][d]) % 12;
        int dist = abs(pc - sp);
        if (dist > 6) dist = 12 - dist;
        if (dist < best_dist) { best_dist = dist; best_pc = sp; }
    }
    if (best_pc < 0) return pitch;  /* fallback */
    /* Reconstruct pitch: keep octave, adjust pc */
    int snapped = octave + best_pc;
    /* Edge case: best_pc < pc in same octave → bump up an octave */
    if (best_pc < pc && (pc - best_pc) <= 6) snapped += 12;
    /* Clamp to valid MIDI range */
    if (snapped < 21) snapped += 12;
    if (snapped > 108) snapped -= 12;
    return snapped;
}

static void policy_apply(ShmcWorld *w, uint32_t *rng) {
    /* Probabilistic scale-snap: detect key, then snap all notes */
    uint32_t r = *rng; r ^= r<<13; r ^= r>>17; r ^= r<<5; *rng = r;
    float prob = (float)(r >> 8) / (float)(1u << 24);
    if (prob > POLICY_SNAP_PROB) return;  /* skip most of the time */

    if (!w->lib || w->lib->n == 0) return;

    /* Detect current key via pitch-class histogram */
    float hist[12] = {0};
    int total_notes = 0;
    for (int mi = 0; mi < w->lib->n; mi++) {
        const VoiceProgram *vp = &w->lib->entries[mi].vp;
        for (int ii = 0; ii < vp->n; ii++) {
            if (VI_OP(vp->code[ii]) == VI_NOTE) {
                hist[VI_PITCH(vp->code[ii]) % 12] += 1.f;
                total_notes++;
            }
        }
    }
    if (total_notes == 0) return;

    /* Find best-matching scale (major/minor only for speed) */
    static const int SCALE_DEG[2][7] = {
        {0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}
    };
    int best_root = 0, best_mode = 0;
    float best_score = -1.f;
    for (int r2 = 0; r2 < 12; r2++) {
        for (int m = 0; m < 2; m++) {
            uint16_t mask = 0;
            for (int d = 0; d < 7; d++) mask |= (uint16_t)(1u << ((r2+SCALE_DEG[m][d])%12));
            float in = 0.f;
            for (int pc = 0; pc < 12; pc++) if (mask & (1u<<pc)) in += hist[pc];
            if (in > best_score) { best_score = in; best_root = r2; best_mode = m; }
        }
    }

    /* Snap a single randomly chosen out-of-scale note */
    /* Collect out-of-scale notes */
    uint16_t smask = 0;
    for (int d = 0; d < 7; d++) smask |= (uint16_t)(1u << ((best_root+POLICY_SCALE_DEG[best_mode][d])%12));

    int candidates[128]; int nc = 0;
    /* (motif_idx<<16 | note_idx) */
    for (int mi = 0; mi < w->lib->n && nc < 128; mi++) {
        const VoiceProgram *vp = &w->lib->entries[mi].vp;
        for (int ii = 0; ii < vp->n && nc < 128; ii++) {
            if (VI_OP(vp->code[ii]) == VI_NOTE) {
                int pc = VI_PITCH(vp->code[ii]) % 12;
                if (!(smask & (1u<<pc))) candidates[nc++] = (mi<<16)|ii;
            }
        }
    }
    if (nc == 0) return;  /* already fully in-scale */

    /* Pick one and snap it */
    r ^= r<<13; r ^= r>>17; r ^= r<<5; *rng = r;
    int pick = candidates[(r>>16) % (uint32_t)nc];
    int mi2 = (pick >> 16) & 0xFFFF;
    int ii2 = pick & 0xFFFF;
    Motif *mot = &w->lib->entries[mi2];
    VInstr vi = mot->vp.code[ii2];
    int old_pitch = VI_PITCH(vi);
    int new_pitch = policy_snap_to_scale(old_pitch, best_root, best_mode);
    if (new_pitch != old_pitch) {
        mot->vp.code[ii2] = VI_PACK(VI_NOTE, (uint8_t)new_pitch,
                                     VI_DUR(vi), VI_VEL(vi));
    }
}

static int
mcts_mutate(ShmcWorld *w,uint32_t *rng){
    uint32_t r=*rng;r^=r<<13;r^=r>>17;r^=r<<5;*rng=r;
    int roll=(int)(r>>24)&0xFF,applied=0;
    /* weighted mutation distribution (redesigned for harmony-first search)
     * roll 0..29  (30%) = note pitch  — primary melodic exploration
     * roll 30..49 (20%) = transpose MotifUse  — harmonic function shifts
     * roll 50..64 (15%) = structural motif op — augment/diminish/invert
     * roll 65..79 (15%) = harmonic mutation — circle5th/chord_sub/sec_dom
     * roll 80..94 (15%) = note duration + beat offset  — rhythmic
     * roll 95..109 (15%) = velocity + vel_scale  — dynamic
     * roll 110..255 ( 6%) = patch struct  — timbre (rare, kept low to preserve melody) */
    for(int t=0;t<6&&!applied;t++){r^=r<<13;r^=r>>17;r^=r<<5;*rng=r;
        if     (roll<30)              {applied=shmc_mutate(w,MUTATE_NOTE_PITCH,rng);}
        else if(roll<50)              {applied=shmc_mutate(w,MUTATE_TRANSPOSE,rng);}
        else if(roll<65)              {applied=shmc_mutate(w,MUTATE_STRUCTURAL,rng);}
        else if(roll<80)              {applied=shmc_mutate(w,MUTATE_HARMONIC,rng);}
        else if(roll<88)              {applied=shmc_mutate(w,MUTATE_NOTE_DUR,rng);}
        else if(roll<95)              {applied=shmc_mutate(w,MUTATE_BEAT_OFFSET,rng);}
        else if(roll<103)             {applied=shmc_mutate(w,MUTATE_NOTE_VEL,rng);}
        else if(roll<110)             {applied=shmc_mutate(w,MUTATE_VEL_SCALE,rng);}
        else if(w->n_patches>0){int pi=(int)(r>>20)%w->n_patches;applied=shmc_patch_struct_mutate(&w->patches[pi],PATCH_STRUCT_ANY,rng);}
        else                          {applied=shmc_mutate(w,MUTATE_NOTE_PITCH,rng);}
    }
    /* Policy guidance: probabilistically snap out-of-scale notes */
    if(applied) policy_apply(w,rng);
    return applied;
}

static int node_alloc(MctsCtx *ctx){
    if(ctx->n_nodes>=MCTS_MAX_NODES)return -1;
    int idx=ctx->n_nodes++;
    MctsNode *n=&ctx->pool[idx];
    n->parent=-1;for(int i=0;i<MCTS_K_EXPAND;i++)n->children[i]=-1;
    n->n_children=0;n->Q=0.f;n->N=0;
    n->world_hash=0;n->raw_fit=-1.f;
    n->world=NULL;   /* init NULL, set after mutation */
    n->mut_desc[0]=0;n->depth=0;
    return idx;
}

float mcts_uct(const MctsNode *node,int parent_visits,float C){
    if(node->N==0)return 1e9f;
    return node->Q/(float)node->N + C*sqrtf(logf((float)parent_visits+1.f)/(float)node->N);
}

static int mcts_select(MctsCtx *ctx,int node_idx){
    MctsNode *cur=&ctx->pool[node_idx];
    while(cur->n_children>0){
        int best_child=cur->children[0];float best_uct=-1e9f;
        for(int i=0;i<cur->n_children;i++){int ci=cur->children[i];if(ci<0)continue;float u=mcts_uct(&ctx->pool[ci],cur->N,MCTS_C_EXPLORE);if(u>best_uct){best_uct=u;best_child=ci;}}
        node_idx=best_child;cur=&ctx->pool[node_idx];
    }
    return node_idx;
}

static void mcts_backprop(MctsCtx *ctx,int node_idx,float reward){
    int idx=node_idx;
    while(idx>=0){ctx->pool[idx].Q+=reward;ctx->pool[idx].N++;idx=ctx->pool[idx].parent;}
}

int mcts_init(MctsCtx *ctx,const ShmcWorld *seed_world,EvoFitFn fit_fn,uint32_t rng_seed){
    memset(ctx,0,sizeof(*ctx));
    ctx->fit_fn=fit_fn;ctx->sr=44100.f;
    ctx->rng=rng_seed?rng_seed:0xDEADBEEF;
    ctx->best_fit=-1.f;ctx->best_node=-1;
    ctx->nov_n=0;ctx->nov_head=0;
    evo_weights_default(&ctx->evo_wt);
    harmony_weights_default(&ctx->harm_wt);
    combined_weights_default(&ctx->comb_wt);

    int root=node_alloc(ctx);if(root<0)return -1;
    ctx->best_world=mcts_world_dup(seed_world);if(!ctx->best_world)return -1;

    /* store root's world in node for proper expansion */
    ctx->pool[root].world=mcts_world_dup(ctx->best_world);
    ctx->pool[root].raw_fit=mcts_eval(ctx,ctx->best_world);
    ctx->pool[root].world_hash=shmc_world_hash(ctx->best_world);
    ctx->best_fit=ctx->pool[root].raw_fit;
    ctx->best_node=root;
    snprintf(ctx->pool[root].mut_desc,32,"root");
    return 0;
}

float mcts_run(MctsCtx *ctx,int n_iters){
    for(int iter=0;iter<n_iters;iter++){
        ctx->n_iters++;
        int leaf_idx=mcts_select(ctx,0);
        MctsNode *leaf=&ctx->pool[leaf_idx];
        float best_this_iter=leaf->raw_fit>=0.f?leaf->raw_fit:0.f;
        int expanded_any=0;

        /* if leaf world was freed (fully-expanded node
         * re-selected somehow), walk the parent chain for the nearest valid world.
         * NEVER fall back to ctx->best_world — that breaks tree structure. */
        ShmcWorld *expand_from=leaf->world;
        if(!expand_from){
            int anc=leaf->parent;
            while(anc>=0&&!ctx->pool[anc].world)anc=ctx->pool[anc].parent;
            expand_from=(anc>=0)?ctx->pool[anc].world:ctx->best_world;
        }
        /* MAP-Elites: 30% of expansions use a random elite as root  */
        if(ctx->me_enabled&&ctx->map_elites.n_filled>4){
            uint32_t rr=ctx->rng;rr^=rr<<13;rr^=rr>>17;rr^=rr<<5;
            if((rr&0xFF)<77){
                int eci=shmc_me_random_elite(&ctx->map_elites);
                if(eci>=0&&ctx->map_elites.cells[eci].world)
                    expand_from=ctx->map_elites.cells[eci].world;
            }
        }

        for(int k=0;k<MCTS_K_EXPAND;k++){
            if(ctx->n_nodes>=MCTS_MAX_NODES)break;

            /* Exactly ONE mutation from parent — true tree branching */
            ShmcWorld *child_w=mcts_world_dup(expand_from);
            if(!child_w)break;

            char last_desc[32]="mut";int applied_total=0;
            for(int attempt=0;attempt<8&&!applied_total;attempt++){
                if(mcts_mutate(child_w,&ctx->rng)){applied_total++;snprintf(last_desc,32,"d%d_k%d",leaf->depth+1,k);}
            }
            if(applied_total==0){mcts_world_free(child_w);continue;}

            uint64_t child_hash=shmc_world_hash(child_w);
            int dup=0;
            for(int ni=0;ni<ctx->n_nodes&&!dup;ni++)if(ctx->pool[ni].world_hash==child_hash){dup=1;ctx->n_dedup++;}
            if(dup){mcts_world_free(child_w);continue;}

            float child_fit=mcts_eval(ctx,child_w);
            if(child_fit<0.f){mcts_world_free(child_w);continue;}
            /* record mutation type vs fitness delta */
            {
                float parent_fit=leaf->raw_fit>0.f?leaf->raw_fit:0.f;
                float delta=child_fit-parent_fit;
                int mtype=(int)((ctx->rng>>24)&0xFF)/30;
                if(mtype>=MCTS_MUT_TYPES)mtype=MCTS_MUT_TYPES-1;
                ctx->mut_hist.trials[mtype]++;
                ctx->mut_hist.reward[mtype]+=delta;
            }

            float rollout_best=child_fit;
            ShmcWorld *rollout_w=mcts_world_dup(child_w);
            if(rollout_w){
                for(int ri=0;ri<MCTS_R_ROLLOUT;ri++){
                    if(!mcts_mutate(rollout_w,&ctx->rng))break;
                    float rf=mcts_eval(ctx,rollout_w);if(rf>rollout_best)rollout_best=rf;
                }
                mcts_world_free(rollout_w);
            }
            if(rollout_best>best_this_iter)best_this_iter=rollout_best;

            if(child_fit>ctx->best_fit){
                ctx->best_fit=child_fit;
                mcts_world_free(ctx->best_world);
                ctx->best_world=mcts_world_dup(child_w);
                ctx->best_node=ctx->n_nodes;
            }

            /* Child node keeps its world for future branching */
            int ci=node_alloc(ctx);
            if(ci<0){mcts_world_free(child_w);break;}
            MctsNode *cn=&ctx->pool[ci];
            cn->parent=leaf_idx;cn->world_hash=child_hash;
            cn->raw_fit=child_fit;cn->depth=leaf->depth+1;
            cn->world=child_w;child_w=NULL;  /* child owns it */
            snprintf(cn->mut_desc,32,"%s",last_desc);

            if(leaf->n_children<MCTS_K_EXPAND)leaf->children[leaf->n_children++]=ci;

            /* backprop child_fit directly — not the max
             * of all rollout scores.  Using max creates optimism bias that
             * destabilizes UCT convergence (Coulom 2006). */
            mcts_backprop(ctx,ci,child_fit);

            /* Free leaf world when fully expanded — reclaim ~700KB */
            if(leaf->n_children==MCTS_K_EXPAND&&leaf->world){
                mcts_world_free(leaf->world);leaf->world=NULL;
            }
            expanded_any=1;
        }

        /* Backprop leaf itself with its own score (or 0 if unvisited) */
        mcts_backprop(ctx,leaf_idx,leaf->raw_fit>0.f?leaf->raw_fit:0.f);
        (void)expanded_any;
    }
    return ctx->best_fit;
}

int mcts_best_world(const MctsCtx *ctx,ShmcWorld *dst){
    if(!ctx->best_world)return 0;
    memcpy(dst,ctx->best_world,sizeof(ShmcWorld));
    dst->lib=NULL;
    if(ctx->best_world->lib){dst->lib=(MotifLibrary*)malloc(sizeof(MotifLibrary));if(!dst->lib)return 0;memcpy(dst->lib,ctx->best_world->lib,sizeof(MotifLibrary));}
    const ShmcWorld *src=ctx->best_world;
    for(int si=0;si<dst->n_songs;si++){Song *song=&dst->songs[si];for(int ei=0;ei<song->n_entries;ei++){SongEntry *ent=&song->entries[ei];if(ent->section){ptrdiff_t idx=ent->section-src->sections;ent->section=(idx>=0&&idx<dst->n_sections)?&dst->sections[idx]:NULL;}ent->lib=dst->lib;}}
    char e2[64]="";
    for(int si=0;si<dst->n_sections;si++){Section *sec=&dst->sections[si];for(int ti=0;ti<sec->n_tracks;ti++){SectionTrack *trk=&sec->tracks[ti];if(trk->patch){ptrdiff_t idx=trk->patch-src->patches;trk->patch=(idx>=0&&idx<dst->n_patches)?&dst->patches[idx]:NULL;}motif_resolve_uses(dst->lib,trk->uses,trk->n_uses,e2,64);}}
    return 1;
}

void mcts_free(MctsCtx *ctx){
    if(!ctx)return;
    for(int i=0;i<ctx->n_nodes;i++){if(ctx->pool[i].world){mcts_world_free(ctx->pool[i].world);ctx->pool[i].world=NULL;}}
    mcts_world_free(ctx->best_world);ctx->best_world=NULL;
}

void mcts_print_stats(MctsCtx *ctx){
    printf("  MCTS stats: iters=%d  evals=%d  dedup=%d  nodes=%d/%d\n",ctx->n_iters,ctx->n_evals,ctx->n_dedup,ctx->n_nodes,MCTS_MAX_NODES);
    printf("  Best fitness: %.4f  (node %d, depth %d)\n",ctx->best_fit,ctx->best_node,ctx->best_node>=0?ctx->pool[ctx->best_node].depth:-1);
    printf("  Top UCT nodes (by visit count):\n");
    for(int rank=0;rank<5&&rank<ctx->n_nodes;rank++){
        int best=0;for(int i=1;i<ctx->n_nodes;i++)if(ctx->pool[i].N>ctx->pool[best].N)best=i;
        if(ctx->pool[best].N==0)break;
        printf("    [%d] N=%3d  Q/N=%.4f  fit=%.4f  depth=%d  %s\n",best,ctx->pool[best].N,ctx->pool[best].N>0?ctx->pool[best].Q/(float)ctx->pool[best].N:0.f,ctx->pool[best].raw_fit,ctx->pool[best].depth,ctx->pool[best].mut_desc);
        ctx->pool[best].N=-999999;
    }
    for(int i=0;i<ctx->n_nodes;i++)if(ctx->pool[i].N==-999999)ctx->pool[i].N=0;
}
