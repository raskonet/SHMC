/*
 * shmc_canon.c — Musical symmetry canonicalization   v2
 *
 * Two key capabilities:
 *
 * 1. shmc_world_hash() — complete world hash including MOTIF CONTENT.
 *    The existing hash_section() only hashes motif-use metadata (name, beat,
 *    repeat, transpose) — not the actual note pitches. shmc_world_hash() adds
 *    hash_motif() for every motif in the library, so two worlds with different
 *    note content always produce different hashes.
 *
 * 2. shmc_section_canonicalize() — sort motif-uses within each track by
 *    (start_beat, name) so that two sections with the same uses in different
 *    insertion order hash identically. Audio-safe: sorting MotifUse entries
 *    doesn't change what gets rendered, only the order in the struct array.
 *
 * NOTE on pitch normalization:
 *   We do NOT normalize motif pitches (pitch[i] -= pitch[0]).
 *   Although this would collapse transpositions, it requires updating every
 *   MotifUse.transpose that references this motif to compensate, which is
 *   complex and error-prone. The simpler, safer approach: hash motif content
 *   directly (different transpositions → different hashes, which is correct
 *   behavior for a search engine that tracks exact program identity).
 *
 * API:
 *   void shmc_section_canonicalize(Section *s)    — sort motif-uses
 *   void shmc_world_canonicalize(ShmcWorld *w)    — apply all canon passes
 *   uint64_t shmc_world_hash(const ShmcWorld *w)  — full hash incl. motifs
 *
 * Verified: verify_canon_struct.c N/N PASSED
 */
#include "../include/shmc_canon.h"
#include "../../layer0b/include/shmc_hash.h"
#include "../../layer1/include/voice.h"
#include <string.h>
#include <stdlib.h>

/* ── Section canonicalization ───────────────────────────────────── */

/*
 * Sort MotifUse entries within each track by (start_beat, name).
 * Audio-safe: the rendered output is identical regardless of the order
 * that MotifUse structs appear in the array — the scheduler reads all
 * of them and schedules by start_beat anyway.
 *
 * Effect: two sections that define uses in different order now hash the same.
 */
static int cmp_motif_use(const void *a, const void *b) {
    const MotifUse *ua = (const MotifUse *)a;
    const MotifUse *ub = (const MotifUse *)b;
    if (ua->start_beat < ub->start_beat) return -1;
    if (ua->start_beat > ub->start_beat) return  1;
    return strncmp(ua->name, ub->name, MOTIF_NAME_LEN);
}

void shmc_section_canonicalize(Section *s) {
    if (!s) return;
    for (int ti = 0; ti < s->n_tracks; ti++) {
        SectionTrack *trk = &s->tracks[ti];
        if (trk->n_uses > 1)
            qsort(trk->uses, (size_t)trk->n_uses, sizeof(MotifUse), cmp_motif_use);
    }
}

/* ── World canonicalization ─────────────────────────────────────── */

void shmc_world_canonicalize(ShmcWorld *w) {
    if (!w) return;
    /* Sections: sort motif-uses by beat position */
    for (int i = 0; i < w->n_sections; i++)
        shmc_section_canonicalize(&w->sections[i]);
    /* Patches: already canonicalized by patch_canonicalize() in compiler
     * and after every shmc_mutate() call. No additional work needed. */
}

/* ── World hash ─────────────────────────────────────────────────── */

/*
 * Complete structural hash of an ShmcWorld.
 *
 * Critically: includes hash_motif() for every motif in the library.
 * hash_motif() calls hash_voice_prog() which hashes all note pitches,
 * durations, and velocities. This means:
 *   - Two worlds with different note content → different hashes ✓
 *   - Two worlds with same content → same hash ✓
 *
 * The existing hash_section() / hash_motif_use() only hashes motif-use
 * metadata (name, beat, repeat, transpose) not note content — so we must
 * add the motif library hashes here.
 */
uint64_t shmc_world_hash(const ShmcWorld *w) {
    uint64_t h = 14695981039346656037ULL; /* FNV-1a offset basis */
    const uint64_t prime = 1099511628211ULL;

    /* Patches */
    for (int i = 0; i < w->n_patches; i++) {
        h ^= hash_patch_prog(&w->patches[i]);
        h *= prime;
    }
    /* Motifs — note content (pitches, durations, velocities) */
    if (w->lib) {
        for (int i = 0; i < w->lib->n; i++) {
            if (w->lib->entries[i].valid) {
                h ^= hash_motif(&w->lib->entries[i]);
                h *= prime;
            }
        }
    }
    /* Sections (motif-use metadata: beat positions, transpose, repeat) */
    for (int i = 0; i < w->n_sections; i++) {
        h ^= hash_section(&w->sections[i]);
        h *= prime;
    }
    /* Songs (BPM, section schedule) */
    for (int i = 0; i < w->n_songs; i++) {
        h ^= hash_song(&w->songs[i]);
        h *= prime;
    }
    return h ? h : 1; /* never return 0 */
}
