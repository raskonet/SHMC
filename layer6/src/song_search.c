/*
 * SHMC Layer 6 — DSL Song Search Engine    song_search.c
 *
 * Beam search over ShmcWorld space using shmc_mutate() operators.
 * Reuses FitnessCtx / feat_extract / feat_fitness from layer5.
 *
 * Verified: verify_song_search.c (all tests pass)
 */
#include "../include/song_search.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* ── RNG (same xorshift32 as layer5) ────────────────────────────── */
static uint32_t xr32(uint32_t *s){
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s;
}

/* ── Render world → feature vector ──────────────────────────────── */
/* Returns fitness in [0,1], or -1.0f on failure */
float song_fitness_score(const SongSearchCtx *ctx, ShmcWorld *w){
    float *buf = NULL;
    int nf = 0;
    if(shmc_world_render(w, &buf, &nf, SONG_SR) < 0) return -1.0f;
    if(!buf || nf <= 0){ free(buf); return -1.0f; }

    /* Pad or trim to SONG_AUDIO_LEN for consistent feature windows */
    int use = nf < SONG_AUDIO_LEN ? nf : SONG_AUDIO_LEN;
    float padded[SONG_AUDIO_LEN];
    memset(padded, 0, sizeof(padded));
    memcpy(padded, buf, (size_t)use * sizeof(float));
    free(buf);

    FeatureVec fv;
    feat_extract(padded, SONG_AUDIO_LEN, &fv);
    return feat_fitness(&ctx->fit_ctx, &fv);
}

/* ── Beam management ─────────────────────────────────────────────── */
static void candidate_free(SongCandidate *c){
    if(c->valid) shmc_world_free(&c->world);
    c->valid = 0;
}

/* Copy a ShmcWorld by re-compiling from DSL.
 * We don't have a deep-copy API, so we compile fresh.
 * The seed_dsl field in ctx provides the baseline. */
static int world_clone(ShmcWorld *dst, const ShmcWorld *src,
                       const char *orig_dsl){
    /* We can't deep-copy ShmcWorld directly (has lib pointer, section ptrs).
     * Strategy: compile a fresh copy from orig_dsl, then apply the deltas
     * stored in src relative to orig_dsl.
     *
     * SIMPLER: since mutations operate in-place, we re-compile from dsl
     * each time we need a clean copy, which is only at beam initialisation.
     * During search, candidates are individually held; mutation clones by
     * recompiling from dsl and replaying mutations — but that's expensive.
     *
     * ACTUAL strategy: we hold N beam candidates as independent worlds.
     * To "copy" a world for mutation, we serialize it to DSL and recompile.
     * Since we don't have a serializer yet, we recompile from the seed_dsl.
     * This means mutations accumulate per-beam-member across generations
     * (the world IS mutated in place; we just need a separate copy to mutate).
     *
     * Implementation: use shmc_dsl_compile(orig_dsl, dst) as a starting point,
     * then accept that beam members diverge only via the mutations applied to them.
     * This is correct: each beam member starts from orig_dsl and gets independently
     * mutated over generations.
 */
    (void)src;
    char err[256] = "";
    return shmc_dsl_compile(orig_dsl, dst, err, 256);
}

/* ── Main search ─────────────────────────────────────────────────── */
void song_search(const SongSearchCtx *ctx, uint32_t seed,
                 SongSearchResult *result,
                 SongProgressFn progress_cb, void *userdata){
    memset(result, 0, sizeof(*result));

    uint32_t rng = seed ? seed : 1;

    /* ── Initialise beam with SONG_BEAM_SIZE independent copies ── */
    /* Allocate beam on heap — ShmcWorld is ~280KB each,
     * SONG_BEAM_SIZE * SongCandidate would overflow the stack */
    SongCandidate *beam = (SongCandidate*)calloc(
        SONG_BEAM_SIZE, sizeof(SongCandidate));
    if(!beam){ result->n_generations=0; return; }

    for(int i = 0; i < SONG_BEAM_SIZE; i++){
        char err[256] = "";
        if(shmc_dsl_compile(ctx->seed_dsl, &beam[i].world, err, 256) < 0){
            beam[i].valid = 0;
            beam[i].fitness = -1.0f;
            continue;
        }
        beam[i].valid = 1;
        /* Apply a few random mutations to diversify the initial beam */
        int n_init_mutations = (int)(xr32(&rng) % 4) + 1;
        for(int m = 0; m < n_init_mutations; m++){
            shmc_mutate(&beam[i].world, MUTATE_ANY, &rng);
        }
        beam[i].fitness = song_fitness_score(ctx, &beam[i].world);
    }

    /* Track global best */
    float best_fitness = -1.0f;
    int   best_idx     = -1;
    for(int i = 0; i < SONG_BEAM_SIZE; i++){
        if(beam[i].valid && beam[i].fitness > best_fitness){
            best_fitness = beam[i].fitness;
            best_idx = i;
        }
    }

    int n_evals = SONG_BEAM_SIZE;

    /* ── Generational loop ───────────────────────────────────────── */
    for(int gen = 0; gen < SONG_MAX_GEN; gen++){

        if(progress_cb){
            if(progress_cb(gen, best_fitness, n_evals, userdata)) break;
        }

        /* Early stop */
        if(best_fitness >= SONG_FITNESS_THRESH) break;

        /* Generate children: mutate each beam member SONG_MUTATIONS times
         * with MUTATE_ANY, producing a pool of candidates */
        int pool_size = SONG_BEAM_SIZE * SONG_MUTATIONS;
        SongCandidate *pool = (SongCandidate*)calloc(
            (size_t)pool_size, sizeof(SongCandidate));
        if(!pool) break;

        for(int bi = 0; bi < SONG_BEAM_SIZE; bi++){
            if(!beam[bi].valid) continue;
            for(int mi = 0; mi < SONG_MUTATIONS; mi++){
                int pi = bi * SONG_MUTATIONS + mi;
                char err[256] = "";
                /* Start from seed DSL */
                if(shmc_dsl_compile(ctx->seed_dsl, &pool[pi].world, err, 256) < 0){
                    pool[pi].valid = 0; pool[pi].fitness = -1.0f; continue;
                }
                pool[pi].valid = 1;
                /* Apply beam parent's accumulated mutations by copying it
                 * (we can't deep-copy, so we copy the motif VoicePrograms
                 *  and patch instructions from beam[bi] into pool[pi]) */
                ShmcWorld *src = &beam[bi].world;
                ShmcWorld *dst = &pool[pi].world;
                /* Copy motif library entries */
                if(src->lib && dst->lib){
                    dst->lib->n = src->lib->n;
                    for(int ei = 0; ei < src->lib->n; ei++)
                        dst->lib->entries[ei] = src->lib->entries[ei];
                }
                /* Copy patch programs */
                for(int pi2 = 0; pi2 < src->n_patches && pi2 < dst->n_patches; pi2++)
                    dst->patches[pi2] = src->patches[pi2];
                /* Copy MotifUse parameters (transpose, vel_scale, beat) */
                for(int si = 0; si < src->n_sections && si < dst->n_sections; si++){
                    Section *ss = &src->sections[si], *ds = &dst->sections[si];
                    for(int ti = 0; ti < ss->n_tracks && ti < ds->n_tracks; ti++){
                        SectionTrack *st = &ss->tracks[ti], *dt = &ds->tracks[ti];
                        for(int ui = 0; ui < st->n_uses && ui < dt->n_uses; ui++)
                            dt->uses[ui] = st->uses[ui];
                    }
                }
                /* Now apply one fresh mutation */
                shmc_mutate(&pool[pi].world, MUTATE_ANY, &rng);
                pool[pi].fitness = song_fitness_score(ctx, &pool[pi].world);
                n_evals++;
            }
        }

        /* Select new beam: merge beam + pool, keep top SONG_BEAM_SIZE */
        int all_size = SONG_BEAM_SIZE + pool_size;
        SongCandidate *all = (SongCandidate*)calloc(
            (size_t)all_size, sizeof(SongCandidate));
        if(!all){ /* OOM: free pool and stop */
            for(int i=0;i<pool_size;i++) candidate_free(&pool[i]);
            free(pool); break;
        }
        int n_all = 0;
        /* Copy beam into all[] */
        for(int i = 0; i < SONG_BEAM_SIZE; i++){
            if(beam[i].valid){
                all[n_all++] = beam[i];
                beam[i].valid = 0; /* ownership transferred */
            }
        }
        /* Copy pool into all[] */
        for(int i = 0; i < pool_size; i++){
            if(pool[i].valid){
                all[n_all++] = pool[i];
                pool[i].valid = 0;
            }
        }
        free(pool);

        /* Selection sort top SONG_BEAM_SIZE */
        for(int i = 0; i < n_all && i < SONG_BEAM_SIZE; i++){
            int best = i;
            for(int j = i+1; j < n_all; j++)
                if(all[j].fitness > all[best].fitness) best = j;
            if(best != i){ SongCandidate tmp=all[i]; all[i]=all[best]; all[best]=tmp; }
        }

        /* Update beam: take top SONG_BEAM_SIZE, free the rest */
        int new_beam_sz = n_all < SONG_BEAM_SIZE ? n_all : SONG_BEAM_SIZE;
        for(int i = 0; i < new_beam_sz; i++) beam[i] = all[i];
        for(int i = new_beam_sz; i < n_all; i++) candidate_free(&all[i]);
        for(int i = new_beam_sz; i < SONG_BEAM_SIZE; i++){
            beam[i].valid = 0; beam[i].fitness = -1.0f;
        }
        free(all);

        /* Update global best */
        if(beam[0].valid && beam[0].fitness > best_fitness){
            best_fitness = beam[0].fitness;
            best_idx = 0;
        }

        result->n_generations = gen + 1;
        result->n_evaluations = n_evals;
    }

    /* Fill result */
    result->best_fitness  = best_fitness;
    result->n_evaluations = n_evals;
    result->found         = best_fitness >= SONG_FITNESS_THRESH;

    /* Deep-copy best world: recompile from seed + copy deltas */
    if(best_idx >= 0 && beam[best_idx].valid){
        char err[256] = "";
        shmc_dsl_compile(ctx->seed_dsl, &result->best_world, err, 256);
        ShmcWorld *src = &beam[best_idx].world;
        ShmcWorld *dst = &result->best_world;
        if(src->lib && dst->lib){
            dst->lib->n = src->lib->n;
            for(int i = 0; i < src->lib->n; i++)
                dst->lib->entries[i] = src->lib->entries[i];
        }
        for(int i = 0; i < src->n_patches && i < dst->n_patches; i++)
            dst->patches[i] = src->patches[i];
        for(int si = 0; si < src->n_sections && si < dst->n_sections; si++){
            Section *ss = &src->sections[si], *ds = &dst->sections[si];
            for(int ti = 0; ti < ss->n_tracks && ti < ds->n_tracks; ti++){
                SectionTrack *st = &ss->tracks[ti], *dt = &ds->tracks[ti];
                for(int ui = 0; ui < st->n_uses && ui < dt->n_uses; ui++)
                    dt->uses[ui] = st->uses[ui];
            }
        }
    }

    /* Free beam */
    for(int i = 0; i < SONG_BEAM_SIZE; i++) candidate_free(&beam[i]);
    free(beam);
}

/* ── Convenience init ────────────────────────────────────────────── */
void song_search_ctx_init(SongSearchCtx *ctx,
                          const float *target_audio, int target_n,
                          const char *seed_dsl, float sr){
    memset(ctx, 0, sizeof(*ctx));
    fitness_ctx_init(&ctx->fit_ctx, target_audio, target_n,
                     60 /* MIDI C4 */, sr);
    ctx->seed_dsl = seed_dsl;
}
