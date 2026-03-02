/*
 * shmc_map_elites.c  —  MAP-Elites Quality-Diversity Archive
 *
 * Mouret & Clune (2015) "Illuminating Search Spaces by Mapping Elites".
 *
 * Architecture:
 *   MCTS generates candidate worlds via mutation.
 *   After every eval, shmc_me_update() places the world in a 4D behavior
 *   grid cell.  Each cell keeps only the BEST world for its behavior bin.
 *   shmc_me_random_elite() returns a random non-empty cell for MCTS to
 *   use as next expansion root — guaranteeing behavioral diversity.
 *
 * Behavior dimensions (doc5, b1–b4):
 *   b1 = brightness     — mean zero-crossing rate, normalised
 *   b2 = rhythm_density — notes per beat, saturates at 4
 *   b3 = pitch_diversity — unique pitch classes / 12  (from EvoFeat)
 *   b4 = tonal_tension  — H6 Lerdahl tension from HarmonyFeat
 *
 * Grid: 6^4 = 1296 cells.
 * Memory: each cell stores one ShmcWorld pointer (~120 bytes) + fitness.
 * Worst-case: 1296 × sizeof(ShmcWorld) ≈ 160 KB for worlds, plus heap.
 */
#include "../include/shmc_map_elites.h"
#include "../../layer1/include/voice.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Internal helpers ─────────────────────────────────────────── */

static uint32_t me_xorshift(uint32_t *s){
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

/* Deep-copy a ShmcWorld (same logic as mcts_world_dup in shmc_mcts.c).
 * Returns heap-allocated copy, caller must free with shmc_world_free()+free(). */
static ShmcWorld *me_world_dup(const ShmcWorld *src) {
    ShmcWorld *dst = (ShmcWorld *)malloc(sizeof(ShmcWorld));
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(ShmcWorld));
    dst->lib = NULL;
    if (src->lib) {
        dst->lib = (MotifLibrary *)malloc(sizeof(MotifLibrary));
        if (!dst->lib) { free(dst); return NULL; }
        memcpy(dst->lib, src->lib, sizeof(MotifLibrary));
    }
    /* Rebase SongEntry section and lib pointers */
    for (int si = 0; si < dst->n_songs; si++) {
        Song *song = &dst->songs[si];
        for (int ei = 0; ei < song->n_entries; ei++) {
            SongEntry *ent = &song->entries[ei];
            if (ent->section) {
                ptrdiff_t idx = ent->section - src->sections;
                ent->section = (idx >= 0 && idx < dst->n_sections)
                               ? &dst->sections[idx] : NULL;
            }
            ent->lib = dst->lib;
        }
    }
    /* Rebase SectionTrack patch pointers + re-resolve motif uses */
    char e2[64] = "";
    for (int si = 0; si < dst->n_sections; si++) {
        Section *sec = &dst->sections[si];
        for (int ti = 0; ti < sec->n_tracks; ti++) {
            SectionTrack *trk = &sec->tracks[ti];
            if (trk->patch) {
                ptrdiff_t idx = trk->patch - src->patches;
                trk->patch = (idx >= 0 && idx < dst->n_patches)
                             ? &dst->patches[idx] : NULL;
            }
            motif_resolve_uses(dst->lib, trk->uses, trk->n_uses, e2, 64);
        }
    }
    return dst;
}

/* ── Public API ───────────────────────────────────────────────── */

void shmc_me_init(MeArchive *arc, uint32_t rng_seed) {
    memset(arc, 0, sizeof(MeArchive));
    arc->rng = rng_seed ? rng_seed : 0xDEADBEEFu;
    /* Mark all cells empty */
    for (int i = 0; i < ME_CELLS; i++) {
        arc->cells[i].fitness = -1.f;
        arc->cells[i].world   = NULL;
    }
}

void shmc_me_free(MeArchive *arc) {
    for (int i = 0; i < ME_CELLS; i++) {
        if (arc->cells[i].world) {
            shmc_world_free(arc->cells[i].world);
            free(arc->cells[i].world);
            arc->cells[i].world = NULL;
        }
        arc->cells[i].fitness = -1.f;
    }
    arc->n_filled = 0;
}

MeBehavior shmc_me_describe(const float *audio, int n, float sr,
                             const HarmonyFeat *hf) {
    MeBehavior b;
    b.b[0] = b.b[1] = b.b[2] = b.b[3] = 0.f;
    if (!audio || n < 2) return b;

    /* b1: brightness — mean zero-crossing rate normalised.
     *     Typical speech ZCR ≈ 0.01–0.10.  Cap at 0.15 for normalisation. */
    int zcr = 0;
    for (int i = 1; i < n; i++)
        if ((audio[i] >= 0.f) != (audio[i-1] >= 0.f)) zcr++;
    float zcr_rate = (float)zcr / (float)n;
    b.b[0] = zcr_rate / 0.15f;
    if (b.b[0] > 1.f) b.b[0] = 1.f;

    /* b2: rhythm density — rough note-onset rate, normalised to [0,1].
     *     Detect onsets as positive-going RMS peaks.  Saturates at 4/beat.
     *     Assume 120 BPM (our default) for beat duration at given sr. */
    {
        int frame = (int)(sr * 0.025f);   /* 25 ms frames */
        if (frame < 1) frame = 1;
        int n_frames = n / frame;
        float prev_rms = 0.f;
        int onsets = 0;
        float threshold = 0.02f;
        for (int f = 0; f < n_frames; f++) {
            float rms = 0.f;
            int start = f * frame;
            int end = start + frame; if (end > n) end = n;
            for (int i = start; i < end; i++) rms += audio[i]*audio[i];
            rms = sqrtf(rms / (float)(end - start));
            if (rms > prev_rms + threshold) onsets++;
            prev_rms = rms;
        }
        float beats = (float)n / sr * 2.0f;   /* at 120 BPM */
        float density = (beats > 0.f) ? (float)onsets / beats : 0.f;
        b.b[1] = density / 4.f;  /* saturate at 4 notes/beat */
        if (b.b[1] > 1.f) b.b[1] = 1.f;
    }

    /* b3: pitch diversity (from HarmonyFeat if available, else 0.5) */
    if (hf) {
        b.b[2] = hf->pitch_diversity;
    } else {
        b.b[2] = 0.5f;
    }

    /* b4: tonal tension — H6 Lerdahl (from HarmonyFeat, else 0.5) */
    if (hf) {
        /* lerdahl_tension is already a SCORE (1=stable, 0=tense).
         * For behavior we want the raw tension [0,1] (lower=more stable). */
        b.b[3] = 1.f - hf->lerdahl_tension;
    } else {
        b.b[3] = 0.5f;
    }

    return b;
}

int shmc_me_cell_idx(const MeBehavior *b) {
    /* 4D index: idx = b0*6^3 + b1*6^2 + b2*6 + b3 */
    int idx = 0;
    int stride = 1;
    for (int d = ME_DIM - 1; d >= 0; d--) {
        float v = b->b[d];
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        int bin = (int)(v * ME_BINS);
        if (bin >= ME_BINS) bin = ME_BINS - 1;
        idx += bin * stride;
        stride *= ME_BINS;
    }
    return idx;   /* in [0, ME_CELLS) */
}

int shmc_me_update(MeArchive *arc, const ShmcWorld *world,
                    float fitness, const MeBehavior *b) {
    arc->n_updates++;
    int ci = shmc_me_cell_idx(b);
    MeCell *cell = &arc->cells[ci];

    if (cell->fitness >= 0.f && fitness <= cell->fitness)
        return 0;   /* not an improvement */

    /* Accept: deep-copy world into cell */
    ShmcWorld *w_copy = me_world_dup(world);
    if (!w_copy) return 0;

    if (cell->world) {
        shmc_world_free(cell->world);
        free(cell->world);
    } else {
        arc->n_filled++;   /* new cell filled */
    }

    cell->world   = w_copy;
    cell->fitness = fitness;
    arc->n_improves++;
    return 1;
}

int shmc_me_random_elite(MeArchive *arc) {
    if (arc->n_filled == 0) return -1;
    /* Reservoir sampling: scan cells, pick uniformly among filled ones */
    int chosen = -1, seen = 0;
    for (int i = 0; i < ME_CELLS; i++) {
        if (arc->cells[i].fitness >= 0.f) {
            seen++;
            /* Reservoir: replace with probability 1/seen */
            uint32_t r = me_xorshift(&arc->rng);
            if ((r % (uint32_t)seen) == 0) chosen = i;
        }
    }
    return chosen;
}

void shmc_me_print_map(const MeArchive *arc) {
    /* 2D projection: b1 (brightness, rows) × b2 (rhythm, cols) */
    printf("\nMAP-Elites 2D projection (brightness × rhythm density)\n");
    printf("   ");
    for (int c = 0; c < ME_BINS; c++) printf(" %d", c);
    printf("   rhythm →\n");
    for (int r = 0; r < ME_BINS; r++) {
        printf(" %d [", r);
        for (int c = 0; c < ME_BINS; c++) {
            /* Count any cell with b0≈r/6, b1≈c/6 (any b2,b3) */
            int filled = 0;
            for (int b2 = 0; b2 < ME_BINS; b2++) {
                for (int b3 = 0; b3 < ME_BINS; b3++) {
                    int idx = r*(ME_BINS*ME_BINS*ME_BINS)
                            + c*(ME_BINS*ME_BINS)
                            + b2*ME_BINS + b3;
                    if (idx < ME_CELLS && arc->cells[idx].fitness >= 0.f)
                        filled = 1;
                }
            }
            printf(filled ? " █" : " ░");
        }
        printf(" ]\n");
    }
    printf("  brightness ↓    filled=%d/%d  updates=%d  improves=%d\n",
           arc->n_filled, ME_CELLS, arc->n_updates, arc->n_improves);
}

int shmc_me_best_world(const MeArchive *arc, ShmcWorld *dst) {
    float best_fit = -1.f;
    const ShmcWorld *best = NULL;
    for (int i = 0; i < ME_CELLS; i++) {
        if (arc->cells[i].fitness > best_fit) {
            best_fit = arc->cells[i].fitness;
            best = arc->cells[i].world;
        }
    }
    if (!best) return 0;
    memcpy(dst, best, sizeof(ShmcWorld));
    return 1;
}
