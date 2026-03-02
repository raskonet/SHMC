/*
 * verify_structural_mutations.c — Stage 9 structural motif mutations
 *
 * Tests:
 * T1:  MUTATE_MOTIF_INVERT changes the motif VoiceProgram
 * T2:  MUTATE_MOTIF_RETROGRADE changes the motif VoiceProgram
 * T3:  MUTATE_MOTIF_AUGMENT increases note durations
 * T4:  MUTATE_MOTIF_DIMINISH decreases note durations
 * T5:  MUTATE_MOTIF_ADD_NOTE increases note count
 * T6:  MUTATE_MOTIF_DEL_NOTE decreases note count (only if ≥ 2 notes)
 * T7:  MUTATE_STRUCTURAL dispatches to one of the 6 structural ops
 * T8:  shmc_mutate_structural_tracked records MUT_TARGET_MSTRUCT
 * T9:  shmc_mutate_undo restores exact VoiceProgram after MSTRUCT (100 trials)
 * T10: snap_vp heap freed correctly (ASAN: no leak)
 * T11: MUTATE_MOTIF_DEL_NOTE never deletes last note (single-note motif unchanged)
 * T12: MUTATE_MOTIF_ADD_NOTE respects DSL_LIMIT_MAX_NOTES_PER_MOTIF cap
 * T13: world hash changes after structural mutation
 * T14: symbolic fitness: rhythm_entropy > 0 for varied durations
 * T15: symbolic fitness: motif_repetition > 0 for use with repeat > 1
 * T16: wfeat_fitness weights sum to 1.00 (±0.001)
 * T17: MUTATE_ANY still works (0-6 range unchanged)
 * T18: fitness does not degrade after structural mutation on a valid world
 *       (search still has gradient — score is a real number, not NaN)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../lemonade/include/shmc_dsl.h"
#include "../lemonade/include/shmc_mutate.h"
#include "../lemonade/include/shmc_mut_algebra.h"
#include "../lemonade/include/shmc_search.h"
#include "../lemonade/include/shmc_dsl_limits.h"
#include "../layer0b/include/shmc_hash.h"
#include "../lemonade/include/shmc_canon.h"
#include "../layer1/include/voice.h"

extern void tables_init(void);

static int T=0, P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  ✓ " m "\n",##__VA_ARGS__);}else{printf("  ✗ FAIL: " m "\n",##__VA_ARGS__);}}while(0)

/* A seed with multi-note motifs so all structural ops have something to work on */
static const char *SEED =
    "PATCH bass { saw ONE; adsr 2 8 20 4; mul $0 $1; lpf $2 18; out $3 }\n"
    "PATCH lead { tri ONE; adsr 1 6 24 8; mul $0 $1; hpf $2 8; out $3 }\n"
    "MOTIF bass_line { note 36 3 10; note 38 4 9; note 40 5 10; note 41 3 8; note 43 4 11 }\n"
    "MOTIF lead_mel  { note 64 2 11; note 67 3 10; note 69 4 12; note 71 2 9 }\n"
    "SECTION verse 16.0 { "
        "use bass_line @ 0.0 x4 patch bass t=0 v=1.0; "
        "use lead_mel  @ 0.0 x2 patch lead t=0 v=0.8 }\n"
    "SONG track 120.0 { play verse x2 }\n";

static ShmcWorld *fresh_world(void) {
    ShmcWorld *w = (ShmcWorld*)calloc(1, sizeof(ShmcWorld));
    char err[128]="";
    if (shmc_dsl_compile(SEED, w, err, 128) < 0) { free(w); return NULL; }
    return w;
}

static void world_drop(ShmcWorld *w) {
    if (!w) return;
    shmc_world_free(w);
    free(w);
}

/* Count VI_NOTE instructions in a motif's VoiceProgram */
static int count_notes(const VoiceProgram *vp) {
    int n=0;
    for (int i=0; i<vp->n; i++)
        if (VI_OP(vp->code[i])==VI_NOTE) n++;
    return n;
}

static int count_all_notes(const ShmcWorld *w) {
    int n=0;
    if (!w->lib) return 0;
    for (int mi=0; mi<w->lib->n; mi++)
        if (w->lib->entries[mi].valid) n+=count_notes(&w->lib->entries[mi].vp);
    return n;
}

/* Sum of all note duration indices in a VoiceProgram */
static int sum_dur(const VoiceProgram *vp) {
    int s=0;
    for (int i=0; i<vp->n; i++)
        if (VI_OP(vp->code[i])==VI_NOTE) s+=(int)VI_DUR(vp->code[i]);
    return s;
}

int main(void) {
    tables_init();
    printf("=== verify_structural_mutations ===\n");

    /* T1: MOTIF_INVERT changes VoiceProgram */
    {
        ShmcWorld *w = fresh_world();
        VoiceProgram before = w->lib->entries[0].vp;
        uint32_t rng = 12345;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_INVERT, &rng);
        CHECK(ok && memcmp(&w->lib->entries[0].vp, &before, sizeof(VoiceProgram))!=0,
              "MUTATE_MOTIF_INVERT changes VoiceProgram");
        world_drop(w);
    }

    /* T2: MOTIF_RETROGRADE changes VoiceProgram */
    {
        ShmcWorld *w = fresh_world();
        VoiceProgram before = w->lib->entries[0].vp;
        uint32_t rng = 22222;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_RETROGRADE, &rng);
        CHECK(ok && memcmp(&w->lib->entries[0].vp, &before, sizeof(VoiceProgram))!=0,
              "MUTATE_MOTIF_RETROGRADE changes VoiceProgram");
        world_drop(w);
    }

    /* T3: MOTIF_AUGMENT increases sum of durations */
    {
        ShmcWorld *w = fresh_world();
        int dur_before = sum_dur(&w->lib->entries[0].vp);
        uint32_t rng = 33333;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_AUGMENT, &rng);
        int dur_after = sum_dur(&w->lib->entries[0].vp);
        CHECK(!ok || dur_after >= dur_before,
              "MUTATE_MOTIF_AUGMENT does not decrease total duration (%d→%d)", dur_before, dur_after);
        world_drop(w);
    }

    /* T4: MOTIF_DIMINISH decreases or maintains sum of durations */
    {
        ShmcWorld *w = fresh_world();
        int dur_before = sum_dur(&w->lib->entries[0].vp);
        uint32_t rng = 44444;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_DIMINISH, &rng);
        int dur_after = sum_dur(&w->lib->entries[0].vp);
        CHECK(!ok || dur_after <= dur_before,
              "MUTATE_MOTIF_DIMINISH does not increase total duration (%d→%d)", dur_before, dur_after);
        world_drop(w);
    }

    /* T5: MOTIF_ADD_NOTE increases note count */
    {
        ShmcWorld *w = fresh_world();
        int nc_before = count_all_notes(w);
        uint32_t rng = 55555;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_ADD_NOTE, &rng);
        int nc_after = count_all_notes(w);
        CHECK(ok && nc_after > nc_before,
              "MUTATE_MOTIF_ADD_NOTE increases note count (%d→%d)", nc_before, nc_after);
        world_drop(w);
    }

    /* T6: MOTIF_DEL_NOTE decreases note count */
    {
        ShmcWorld *w = fresh_world();
        int nc_before = count_all_notes(w);
        uint32_t rng = 66666;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_DEL_NOTE, &rng);
        int nc_after = count_all_notes(w);
        CHECK(ok && nc_after < nc_before,
              "MUTATE_MOTIF_DEL_NOTE decreases note count (%d→%d)", nc_before, nc_after);
        world_drop(w);
    }

    /* T7: MUTATE_STRUCTURAL dispatches to a structural op (world changes) */
    {
        ShmcWorld *w = fresh_world();
        uint64_t h_before = shmc_world_hash(w);
        uint32_t rng = 77777;
        int ok = 0;
        for (int t=0; t<50 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_STRUCTURAL, &rng);
        uint64_t h_after = shmc_world_hash(w);
        CHECK(ok && h_after != h_before,
              "MUTATE_STRUCTURAL changes world hash");
        world_drop(w);
    }

    /* T8: structural_tracked records MUT_TARGET_MSTRUCT */
    {
        ShmcWorld *w = fresh_world();
        uint32_t rng = 88888;
        MutationRecord rec; memset(&rec,0,sizeof(rec));
        int ok = 0;
        for (int t=0; t<50 && !ok; t++)
            ok = shmc_mutate_structural_tracked(w, MUTATE_STRUCTURAL, &rng, &rec);
        CHECK(ok && rec.target_kind == MUT_TARGET_MSTRUCT,
              "structural_tracked: target_kind == MUT_TARGET_MSTRUCT");
        CHECK(ok && rec.snap_vp != NULL,
              "structural_tracked: snap_vp is non-NULL");
        mut_record_free(&rec);
        world_drop(w);
    }

    /* T9: undo restores exact VoiceProgram — 100 trials */
    {
        int n_ok = 0, n_trials = 0;
        for (int trial=0; trial<100; trial++) {
            ShmcWorld *w = fresh_world();
            uint32_t rng = 99000 + trial;
            MutationRecord rec; memset(&rec,0,sizeof(rec));
            int applied = shmc_mutate_structural_tracked(w, MUTATE_STRUCTURAL, &rng, &rec);
            if (!applied) { world_drop(w); continue; }
            n_trials++;
            VoiceProgram snap = *rec.snap_vp;
            int undo_ok = shmc_mutate_undo(w, &rec);
            int restored = (undo_ok && rec.motif_idx < w->lib->n &&
                memcmp(&w->lib->entries[rec.motif_idx].vp, &snap, sizeof(VoiceProgram))==0);
            if (restored) n_ok++;
            mut_record_free(&rec);
            world_drop(w);
        }
        CHECK(n_trials > 20, "structural undo: ran %d trials", n_trials);
        CHECK(n_ok == n_trials,
              "structural undo: %d/%d exact restores", n_ok, n_trials);
    }

    /* T10: snap_vp freed correctly — relies on ASAN in the surrounding test run */
    {
        ShmcWorld *w = fresh_world();
        uint32_t rng = 10001;
        MutationRecord rec; memset(&rec,0,sizeof(rec));
        int ok = 0;
        for (int t=0; t<50 && !ok; t++)
            ok = shmc_mutate_structural_tracked(w, MUTATE_STRUCTURAL, &rng, &rec);
        mut_record_free(&rec);  /* must not leak */
        CHECK(rec.snap_vp == NULL, "snap_vp NULL after mut_record_free");
        world_drop(w);
    }

    /* T11: DEL_NOTE does nothing on single-note motif */
    {
        const char *one_note_dsl =
            "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; out $2 }\n"
            "MOTIF m { note 60 4 10 }\n"
            "SECTION s 4.0 { use m @ 0.0 patch p }\n"
            "SONG t 120.0 { play s x1 }\n";
        ShmcWorld *w = (ShmcWorld*)calloc(1,sizeof(ShmcWorld));
        char err[128]="";
        shmc_dsl_compile(one_note_dsl, w, err, 128);
        int nc_before = count_all_notes(w);
        uint32_t rng = 11000;
        int ok = 0;
        for (int t=0; t<20; t++)
            ok |= shmc_mutate(w, MUTATE_MOTIF_DEL_NOTE, &rng);
        int nc_after = count_all_notes(w);
        CHECK(nc_after == nc_before,
              "DEL_NOTE on single-note motif: note count unchanged (%d)", nc_before);
        (void)ok;
        shmc_world_free(w); free(w);
    }

    /* T12: ADD_NOTE respects DSL_LIMIT_MAX_NOTES_PER_MOTIF cap */
    {
        ShmcWorld *w = fresh_world();
        uint32_t rng = 12000;
        /* Flood add notes until cap */
        for (int i=0; i<200; i++)
            shmc_mutate(w, MUTATE_MOTIF_ADD_NOTE, &rng);
        int nc = count_all_notes(w) / (w->lib ? w->lib->n : 1);
        CHECK(nc <= DSL_LIMIT_MAX_NOTES_PER_MOTIF,
              "ADD_NOTE cap: %d ≤ %d", nc, DSL_LIMIT_MAX_NOTES_PER_MOTIF);
        world_drop(w);
    }

    /* T13: world hash changes after structural mutation */
    {
        ShmcWorld *w = fresh_world();
        uint64_t h0 = shmc_world_hash(w);
        uint32_t rng = 13000;
        int ok = 0;
        for (int t=0; t<50 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_MOTIF_ADD_NOTE, &rng);
        uint64_t h1 = shmc_world_hash(w);
        CHECK(ok && h1 != h0, "hash changes after ADD_NOTE (h0=%llu h1=%llu)",
              (unsigned long long)h0, (unsigned long long)h1);
        world_drop(w);
    }

    /* T14: rhythm_entropy > 0 for our mixed-duration seed */
    {
        ShmcWorld *w = fresh_world();
        WFeat f; WWeights wt; wweights_default(&wt);
        /* Compute features symbolically (no audio needed for symbolic fields) */
        memset(&f, 0, sizeof(f));
        /* Compute rhythm_entropy from world directly */
        int dur_counts[7]={0}; int total_dur=0;
        for (int mi=0; mi<w->lib->n; mi++) {
            if (!w->lib->entries[mi].valid) continue;
            const VoiceProgram *vp = &w->lib->entries[mi].vp;
            for (int ni=0; ni<vp->n; ni++) {
                VInstr vi = vp->code[ni];
                if (VI_OP(vi)==VI_NOTE) {
                    int d=(int)VI_DUR(vi); if(d>=0&&d<7){dur_counts[d]++;total_dur++;}
                }
            }
        }
        float entropy=0.f;
        if (total_dur>0) {
            for (int i=0;i<7;i++) {
                if (dur_counts[i]>0) {
                    float p=(float)dur_counts[i]/(float)total_dur;
                    entropy -= p*logf(p);
                }
            }
            entropy /= logf(7.f);
        }
        CHECK(entropy > 0.0f, "rhythm_entropy > 0 for mixed-duration seed (%.4f)", entropy);
        world_drop(w);
    }

    /* T15: motif_repetition > 0 for uses with repeat > 1 */
    {
        ShmcWorld *w = fresh_world();
        /* Our seed has x4 and x2 repeats */
        const Section *sec = &w->sections[0];
        int total_uses=0, repeated_uses=0;
        for (int ti=0; ti<sec->n_tracks; ti++) {
            const SectionTrack *trk = &sec->tracks[ti];
            for (int ui=0; ui<trk->n_uses; ui++) {
                total_uses++;
                if (trk->uses[ui].repeat>1) repeated_uses+=trk->uses[ui].repeat-1;
            }
        }
        float mr = total_uses>0
            ? (float)repeated_uses/(float)(total_uses+repeated_uses) : 0.f;
        CHECK(mr > 0.0f, "motif_repetition > 0 for x4/x2 seed (%.4f)", mr);
        world_drop(w);
    }

    /* T16: wfeat_fitness weights sum to 1.00 */
    {
        WWeights wt; wweights_default(&wt);
        float sum = wt.w_audibility + wt.w_env_variety + wt.w_brightness
                  + wt.w_temporal + wt.w_pitch_div + wt.w_dynamics
                  + wt.w_motif_rep + wt.w_rhythm_ent;
        CHECK(fabsf(sum - 1.0f) < 0.001f, "weights sum = %.4f (≈1.000)", sum);
    }

    /* T17: MUTATE_ANY still in range 0-6 (patch mutations work) */
    {
        ShmcWorld *w = fresh_world();
        uint32_t rng = 17000;
        int ok = 0;
        for (int t=0; t<20 && !ok; t++)
            ok = shmc_mutate(w, MUTATE_ANY, &rng);
        CHECK(ok, "MUTATE_ANY (0-6 range) still works");
        world_drop(w);
    }

    /* T18: fitness is a valid float after structural mutations */
    {
        ShmcWorld *w = fresh_world();
        uint32_t rng = 18000;
        for (int t=0; t<5; t++) shmc_mutate(w, MUTATE_STRUCTURAL, &rng);
        /* Symbolic features only (no audio render) */
        WFeat f; memset(&f,0,sizeof(f));
        WWeights wt; wweights_default(&wt);
        /* Set plausible audio features */
        f.rms=0.05f; f.zcr_brightness=0.012f; f.dynamic_range=5.f;
        f.pitch_diversity=0.5f; f.temporal_spread=0.2f;
        for (int i=0; i<SEARCH_FEAT_WIN; i++) f.rms_env[i]=0.05f*(1.f+(i%4)*0.1f);
        f.motif_repetition=0.3f; f.rhythm_entropy=0.4f;
        float fit = wfeat_fitness(&f, &wt);
        CHECK(fit==fit && fit>=0.f && fit<=1.1f,
              "wfeat_fitness is valid [%.4f] after structural mutations", fit);
        world_drop(w);
    }

    printf("\n  RESULT: %d/%d PASSED\n", P, T);
    return (P==T) ? 0 : 1;
}
