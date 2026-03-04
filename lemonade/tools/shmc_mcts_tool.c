#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "../include/shmc_dsl.h"
#include "../include/shmc_mcts.h"
#include "../include/shmc_evo_fitness.h"
#include "../include/shmc_harmony.h"
#include "../include/shmc_mutate.h"
#include "../include/shmc_patch_mutate.h"
#include "../include/shmc_mut_algebra.h"
#include "../include/shmc_canon.h"
#include "../../layer0b/include/shmc_hash.h"
#include "../include/shmc_dsl_emit.h"

#define SR       44100.f
#define PREVIEW_S 3.0f
#define PREVIEW_N ((int)(PREVIEW_S * SR))

extern void tables_init(void);

static void write_wav(const char *path, const float *buf, int n, float sr) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int data_size = n * 2, hdr_size = 44, file_size = hdr_size + data_size - 8;
    unsigned char hdr[44] = {
        82,73,70,70, file_size&0xFF,(file_size>>8)&0xFF,(file_size>>16)&0xFF,(file_size>>24)&0xFF,
        87,65,86,69, 102,109,116,32, 16,0,0,0, 1,0, 1,0,
        (int)sr&0xFF,((int)sr>>8)&0xFF,0,0, (int)sr*2&0xFF,((int)sr*2>>8)&0xFF,0,0,
        2,0, 16,0, 100,97,116,97, data_size&0xFF,(data_size>>8)&0xFF,(data_size>>16)&0xFF,(data_size>>24)&0xFF
    };
    fwrite(hdr, 1, 44, f);
    for (int i = 0; i < n; i++) {
        float s = buf[i]; if (s > 1.f) s = 1.f; if (s < -1.f) s = -1.f;
        short si = (short)(s * 32767.f); fwrite(&si, 2, 1, f);
    }
    fclose(f);
}

static ShmcWorld *world_new(const char *dsl, char *err, int err_sz) {
    ShmcWorld *w = (ShmcWorld *)calloc(1, sizeof(ShmcWorld));
    if (shmc_dsl_compile(dsl, w, err, err_sz) < 0) { free(w); return NULL; }
    return w;
}

static void world_free(ShmcWorld *w) { if (w) { shmc_world_free(w); free(w); } }

static ShmcWorld *world_dup(const ShmcWorld *src) {
    ShmcWorld *dst = (ShmcWorld *)malloc(sizeof(ShmcWorld));
    memcpy(dst, src, sizeof(ShmcWorld));
    dst->lib = NULL;
    if (src->lib) {
        dst->lib = (MotifLibrary *)malloc(sizeof(MotifLibrary));
        memcpy(dst->lib, src->lib, sizeof(MotifLibrary));
    }
    for (int si = 0; si < dst->n_songs; si++) {
        for (int ei = 0; ei < dst->songs[si].n_entries; ei++) {
            SongEntry *ent = &dst->songs[si].entries[ei];
            if (ent->section) ent->section = &dst->sections[ent->section - src->sections];
            ent->lib = dst->lib;
        }
    }
    char e[64]="";
    for (int si = 0; si < dst->n_sections; si++) {
        for (int ti = 0; ti < dst->sections[si].n_tracks; ti++) {
            SectionTrack *trk = &dst->sections[si].tracks[ti];
            if (trk->patch) trk->patch = &dst->patches[trk->patch - src->patches];
            motif_resolve_uses(dst->lib, trk->uses, trk->n_uses, e, 64);
        }
    }
    return dst;
}

static float full_score_ext(ShmcWorld *w, EvoFeat *ef_out) {
    float *buf = NULL; int n = 0;
    if (shmc_world_render_n(w, &buf, &n, SR, PREVIEW_N) < 0 || n == 0) {
        if(buf) free(buf);
        return -1.f;
    }
    evo_feat_extract(buf, n, SR, ef_out);
    free(buf);
    EvoWeights ew; evo_weights_default(&ew);
    HarmonyWeights hw; harmony_weights_default(&hw);
    CombinedWeights cw; combined_weights_default(&cw);
    return world_total_fitness(w, ef_out, 0.f, &hw, &ew, &cw);
}

static void print_analysis(const char *title, ShmcWorld *w, const float *audio, int n, float sr) {
    EvoFeat ef; evo_feat_extract(audio, n, sr, &ef);
    HarmonyFeat hf; HarmonyWeights hw; harmony_weights_default(&hw);
    harmony_feat_extract(w, &hf, &hw);

    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  FULL STAGE 12 ANALYSIS: %-31s ║\n", title);
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  AUDIO DIVERSITY (EvoFeat)                               ║\n");
    printf("║    Spectral Diversity:     %6.4f                        ║\n", ef.spectral_div);
    printf("║    Self-Dissimilarity:     %6.4f                        ║\n", ef.self_dissim);
    printf("║    Temporal Dynamics:      %6.4f                        ║\n", ef.temporal_ent);
    printf("║    Acoustic Harmonicity:   %6.4f                        ║\n", ef.harmonicity);
    printf("║    Sethares Roughness:     %6.4f                        ║\n", ef.roughness);
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  SYMBOLIC HARMONY (HarmonyFeat)                          ║\n");
    printf("║    Scale Consistency:      %6.4f  (Root: %2d Mode: %d)  ║\n", hf.scale_consistency, hf.best_root, hf.best_scale_type);
    printf("║    Chord Consonance:       %6.4f                        ║\n", hf.consonance);
    printf("║    Voice Leading (Smooth): %6.4f                        ║\n", hf.voice_leading);
    printf("║    Tension Arc (MorpheuS): %6.4f                        ║\n", hf.tension_arc);
    printf("║    Root Cadence (Piston):  %6.4f                        ║\n", hf.cadence);
    printf("║    Lerdahl Tonal Tension:  %6.4f                        ║\n", hf.lerdahl_tension);
    printf("║    Chord Progression:      %6.4f                        ║\n", hf.chord_progression);
    printf("║    Pitch Diversity:        %6.4f                        ║\n", hf.pitch_diversity);
    printf("║    Rhythm Entropy:         %6.4f                        ║\n", hf.rhythm_entropy);
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

typedef struct { ShmcWorld *world; float fit; uint64_t hash; int valid; } BSlot;
static void bslot_free(BSlot *s) { if (s->valid && s->world) { world_free(s->world); s->world=NULL; s->valid=0; } }

static float run_beam(const char *dsl, int max_gen, ShmcWorld *best_out, int *n_evals_out) {
    int BW=8, BM=4, pool_sz=40, evals=0;
    BSlot *beam = (BSlot*)calloc(BW, sizeof(BSlot)), *pool = (BSlot*)calloc(pool_sz, sizeof(BSlot));
    uint32_t rng = (uint32_t)time(NULL) ^ 0xBEEF1234;

    char err[128]=""; ShmcWorld *seed = world_new(dsl, err, 128);
    if (!seed) { free(beam); free(pool); return -1.f; }

    EvoFeat dummy; float seed_fit = full_score_ext(seed, &dummy); evals++;
    printf("[beam] seed fit = %.4f\n", seed_fit);

    for (int i=0; i<BW; i++) {
        beam[i].world = world_dup(seed); beam[i].fit = seed_fit; beam[i].valid = 1; beam[i].hash = shmc_world_hash(seed);
    }
    world_free(seed);

    float best_fit = seed_fit;
    for (int gen=0; gen<max_gen; gen++) {
        int pn=0;
        for (int bi=0; bi<BW && pn<pool_sz; bi++) {
            if (!beam[bi].valid) continue;
            pool[pn].world = world_dup(beam[bi].world); pool[pn].fit = beam[bi].fit; pool[pn].valid = 1; pool[pn].hash = beam[bi].hash; pn++;
            for (int m=0; m<BM && pn<pool_sz; m++) {
                BSlot *s = &pool[pn]; s->world = world_dup(beam[bi].world);
                rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
                int roll = (int)(rng>>24) & 0xFF, ok = 0;
                for (int t=0; t<6 && !ok; t++) {
                    if (roll < 38 && s->world->n_patches>0) ok = shmc_patch_struct_mutate(&s->world->patches[(rng>>20)%s->world->n_patches], PATCH_STRUCT_ANY, &rng);
                    else if (roll < 64) ok = shmc_mutate(s->world, MUTATE_STRUCTURAL, &rng);
                    else ok = shmc_mutate(s->world, MUTATE_ANY, &rng);
                }
                if (!ok) { world_free(s->world); s->world=NULL; continue; }
                shmc_world_canonicalize(s->world); s->hash = shmc_world_hash(s->world);
                float mfit = full_score_ext(s->world, &dummy); evals++;
                if (mfit < 0.f) { world_free(s->world); s->world=NULL; continue; }
                s->fit = mfit; s->valid = 1; if (mfit > best_fit) best_fit = mfit; pn++;
            }
        }
        for (int i=0; i<BW && i<pn; i++) {
            int bst=i; for (int j=i+1; j<pn; j++) if (pool[j].fit > pool[bst].fit) bst=j;
            if (bst!=i) { BSlot tmp = pool[i]; pool[i] = pool[bst]; pool[bst] = tmp; }
        }
        printf("  [beam] gen %2d: fit=%.4f  evals=%d\n", gen+1, best_fit, evals);
        for (int i=0; i<BW; i++) bslot_free(&beam[i]);
        for (int i=0; i<BW && i<pn; i++) { beam[i] = pool[i]; pool[i].valid=0; pool[i].world=NULL; }
        for (int i=BW; i<pn; i++) bslot_free(&pool[i]);
        if (best_fit >= 0.95f) break;
    }

    if (best_out && beam[0].valid && beam[0].world) {
        ShmcWorld *src = beam[0].world; memcpy(best_out, src, sizeof(ShmcWorld)); best_out->lib = NULL;
        if (src->lib) { best_out->lib = (MotifLibrary*)malloc(sizeof(MotifLibrary)); memcpy(best_out->lib, src->lib, sizeof(MotifLibrary)); }
        char e2[64]="";
        for (int si=0; si<best_out->n_sections; si++)
            for (int ti=0; ti<best_out->sections[si].n_tracks; ti++) {
                if (best_out->sections[si].tracks[ti].patch) best_out->sections[si].tracks[ti].patch = &best_out->patches[best_out->sections[si].tracks[ti].patch - src->patches];
                motif_resolve_uses(best_out->lib, best_out->sections[si].tracks[ti].uses, best_out->sections[si].tracks[ti].n_uses, e2, 64);
            }
        for (int si=0; si<best_out->n_songs; si++)
            for (int ei=0; ei<best_out->songs[si].n_entries; ei++) {
                if (best_out->songs[si].entries[ei].section) best_out->songs[si].entries[ei].section = &best_out->sections[best_out->songs[si].entries[ei].section - src->sections];
                best_out->songs[si].entries[ei].lib = best_out->lib;
            }
    }
    for (int i=0; i<BW; i++) bslot_free(&beam[i]);
    free(beam); free(pool);
    if (n_evals_out) *n_evals_out = evals;
    return best_fit;
}

int main(int argc, char **argv) {
    const char *dsl_path = argc > 1 ? argv[1] : NULL;
    int iters = argc > 2 ? atoi(argv[2]) : 60;
    int beam_gen = argc > 3 ? atoi(argv[3]) : 10;
    if (!dsl_path) return 1;

    tables_init();

    FILE *f = fopen(dsl_path, "r");
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *dsl = (char*)malloc(sz + 1);
    size_t got = fread(dsl, 1, sz, f); (void)got; dsl[sz] = 0; fclose(f);

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  SHMC Stage 12: MCTS vs Beam (Harmony+Evo+Novelty)   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* ── Beam ── */
    printf("[1/2] BEAM SEARCH...\n");
    ShmcWorld *beam_best = (ShmcWorld*)calloc(1, sizeof(ShmcWorld));
    int beam_evals = 0; clock_t beam_t0 = clock();
    float beam_fit = run_beam(dsl, beam_gen, beam_best, &beam_evals);
    double beam_sec = (double)(clock()-beam_t0)/CLOCKS_PER_SEC;

    if (beam_fit >= 0.f) {
        float *beam_audio = NULL; int beam_n = 0;
        shmc_world_render(beam_best, &beam_audio, &beam_n, SR);
        write_wav("beam_evolved.wav", beam_audio, beam_n, SR);

        EvoFeat beam_ef; float beam_full = full_score_ext(beam_best, &beam_ef);
        printf("\n  BEAM results:\n    fitness (preview): %.4f\n    fitness (full):    %.4f\n    evals: %d    time: %.1fs\n", beam_fit, beam_full, beam_evals, beam_sec);
        print_analysis("Beam Evolved Output", beam_best, beam_audio, beam_n, SR);
        free(beam_audio);
    } else {
        printf("[beam] search failed\n");
    }

    /* ── MCTS ── */
    printf("\n[2/2] MCTS SEARCH...\n");
    char err[128]=""; ShmcWorld *seed = world_new(dsl, err, 128);
    if (!seed) { fprintf(stderr, "MCTS Seed Error: %s\n", err); return 1; }
    MctsCtx ctx; mcts_init(&ctx, seed, NULL, (uint32_t)time(NULL)^0xC0FFEE); world_free(seed);

    clock_t mcts_t0 = clock();
    for (int i = 0; i < iters; i++) {
        mcts_run(&ctx, 1);
        if ((i+1) % 10 == 0) printf("  [mcts] iter %3d/%3d: fit=%.4f  nodes=%d  evals=%d\n", i+1, iters, ctx.best_fit, ctx.n_nodes, ctx.n_evals);
    }
    double mcts_sec = (double)(clock()-mcts_t0)/CLOCKS_PER_SEC;

    ShmcWorld *mcts_best = (ShmcWorld*)calloc(1, sizeof(ShmcWorld));
    mcts_best_world(&ctx, mcts_best);

    float *mcts_audio = NULL; int mcts_n = 0;
    shmc_world_render(mcts_best, &mcts_audio, &mcts_n, SR);
    write_wav("mcts_evolved.wav", mcts_audio, mcts_n, SR);

    EvoFeat mcts_ef; float mcts_full = full_score_ext(mcts_best, &mcts_ef);

    printf("\n  MCTS results:\n    fitness (preview): %.4f\n    fitness (full):    %.4f\n    evals: %d    time: %.1fs\n", ctx.best_fit, mcts_full, ctx.n_evals, mcts_sec);
    print_analysis("MCTS Evolved Output", mcts_best, mcts_audio, mcts_n, SR);


    char *edsl = (char*)malloc(131072);
    if (edsl) {
        int nb = shmc_world_to_dsl(mcts_best, edsl, 131072);
        if (nb > 0) {
            FILE *f = fopen("mcts_evolved.shmc", "w");
            if (f) { fwrite(edsl, 1, nb, f); fclose(f); }
            printf("    evolved dsl:       mcts_evolved.shmc\n");
        }
        free(edsl);
    }

    printf("\n╔══════════════════════════════════════════╗\n");

    printf("║  Winner: %-31s ║\n", mcts_full > (beam_fit >= 0.f ? beam_fit : 0.f) ? "MCTS" : "BEAM");
    printf("╚══════════════════════════════════════════╝\n\n");

    free(mcts_audio);
    mcts_free(&ctx); shmc_world_free(beam_best); free(beam_best); shmc_world_free(mcts_best); free(mcts_best); free(dsl);
    return 0;
}
