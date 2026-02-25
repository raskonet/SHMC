#pragma once
/*
 * SHMC — Unified Structural Hashing  (T4-2)
 *
 * FNV-1a hashes for every IR type across all layers.
 * Enables MCTS transposition tables.
 *
 * Design rules:
 *  - Pure functions; no allocation.
 *  - Hash covers only the canonical symbolic content of an IR node,
 *    not runtime state (sample counts, pointers, BPM snapshots).
 *  - Commutative ops are NOT canonicalized here (T4-9 deferred).
 *  - All functions return uint64_t.
 *
 * Layer coverage:
 *   L0  : hash_patch_prog()    — instruction bytes of PatchProgram
 *   L1  : hash_voice_prog()    — instruction bytes of VoiceProgram
 *   L2  : hash_motif_use()     — name + start_beat + repeat + transpose
 *          hash_motif()        — name + VoiceProgram content
 *   L3  : hash_section_track() — patch hash + all motif-use hashes + gain + pan
 *          hash_section()      — name + length + all track hashes
 *   L4  : hash_bpm_point()     — beat + bpm + interp
 *          hash_song()         — name + bpm points + all entry hashes
 */

#include <stdint.h>
#include <string.h>

/* Forward declarations — include only what you need */
#include "../../layer0/include/patch.h"
#include "../../layer1/include/voice.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include "../../layer4/include/song.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t hash_patch_prog   (const PatchProgram   *p);
uint64_t hash_voice_prog   (const VoiceProgram   *v);
uint64_t hash_motif_use    (const MotifUse       *u);
uint64_t hash_motif        (const Motif          *m);
uint64_t hash_section_track(const SectionTrack   *t);
uint64_t hash_section      (const Section        *s);
uint64_t hash_bpm_point    (const BpmPoint       *b);
uint64_t hash_song         (const Song           *s);

#ifdef __cplusplus
}
#endif

/*
 * N5-4: EventStream canonical hash.
 * Hashes the compiled event sequence (sample, type, pitch, velocity) —
 * two VoicePrograms with different DSL but identical scheduled events
 * will have identical EventStream hashes.
 * This is the hash to use for MCTS transposition tables at voice level.
 */
uint64_t hash_event_stream(const EventStream *es);

/*
 * N5-1: Canonical patch normalization.
 * Reorders commutative operands (ADD, MUL, MIN, MAX, MIXN) so that
 * the source register with the smaller index always appears first.
 * This eliminates symmetric duplicates:
 *   ADD r4 r3  →  ADD r3 r4 (canonical)
 *   ADD r3 r4  →  ADD r3 r4 (already canonical)
 * Modifies prog in place. Returns number of instructions changed.
 * Safe to call multiple times (idempotent).
 */
int patch_canonicalize(PatchProgram *prog);
