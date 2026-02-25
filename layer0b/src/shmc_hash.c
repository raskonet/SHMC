/*
 * SHMC — Unified structural hashing (T4-2)
 * FNV-1a with layer-specific mixing to prevent cross-layer collisions.
 */
#include "../include/shmc_hash.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* VEL8: quantize velocity [0,1] to 9 distinguishable buckets [0..8].
 * Uses roundf so all 8 VEL_TABLE entries map to DISTINCT integers:
 *   0.125→1, 0.250→2, 0.375→3, 0.500→4, 0.625→5, 0.750→6, 0.875→7, 1.000→8
 * The old floor+clamp formula (int(v*8), clamped to 7) collapsed
 * VEL_TABLE[6]=0.875 and VEL_TABLE[7]=1.0 both to 7 — a hash collision.
 * Result is used only as a uint64 hash input, never as an array index. */
#define VEL8(v) ((int)roundf((v)*8.0f))

/* ---- FNV-1a primitives ---- */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv_init(void) { return FNV_OFFSET; }

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t n){
    const unsigned char *p = (const unsigned char *)data;
    for(size_t i=0; i<n; i++){ h ^= (uint64_t)p[i]; h *= FNV_PRIME; }
    return h;
}

static uint64_t fnv_u64(uint64_t h, uint64_t v){
    return fnv_bytes(h, &v, 8);
}

static uint64_t fnv_f32(uint64_t h, float v){
    /* Treat ±0 as identical */
    if(v == 0.f) v = 0.f;
    return fnv_bytes(h, &v, 4);
}

static uint64_t fnv_str(uint64_t h, const char *s){
    while(*s){ h ^= (uint64_t)(unsigned char)*s++; h *= FNV_PRIME; }
    return h;
}

/* Layer tags — prevent cross-layer collisions */
#define TAG_L0  0xL0C0DEL0C0DE0000ULL
#define TAG_L1  0xL1C0DEL1C0DE0001ULL
#define TAG_L2  0xL2C0DEL2C0DE0002ULL
#define TAG_L3  0xL3C0DEL3C0DE0003ULL
#define TAG_L4  0xL4C0DEL4C0DE0004ULL

/* Can't use hex literals with chars — use decimal constants */
/* 0x4C30424445... — just use simple distinguishing seeds */
static const uint64_t SEED_L0 = 0x4C30434F44455300ULL;
static const uint64_t SEED_L1 = 0x4C31434F44455301ULL;
static const uint64_t SEED_L2 = 0x4C32434F44455302ULL;
static const uint64_t SEED_L3 = 0x4C33434F44455303ULL;
static const uint64_t SEED_L4 = 0x4C34434F44455304ULL;

/* ================================================================
   Layer 0: PatchProgram
   ================================================================ */
uint64_t hash_patch_prog(const PatchProgram *p){
    uint64_t h = fnv_u64(fnv_init(), SEED_L0);
    h = fnv_bytes(h, p->code, (size_t)p->n_instrs * sizeof(Instr));
    return h;
}

/* ================================================================
   Layer 1: VoiceProgram
   ================================================================ */
uint64_t hash_voice_prog(const VoiceProgram *v){
    uint64_t h = fnv_u64(fnv_init(), SEED_L1);
    h = fnv_bytes(h, v->code, (size_t)v->n * sizeof(VInstr));
    return h;
}

/* ================================================================
   Layer 2: MotifUse, Motif
   ================================================================ */
uint64_t hash_motif_use(const MotifUse *u){
    uint64_t h = fnv_u64(fnv_init(), SEED_L2);
    h = fnv_str(h, u->name);
    h = fnv_f32(h, u->start_beat);
    h = fnv_u64(h, (uint64_t)u->repeat);
    h = fnv_u64(h, (uint64_t)(uint32_t)u->transpose);
    return h;
}

uint64_t hash_motif(const Motif *m){
    uint64_t h = fnv_u64(fnv_init(), SEED_L2 ^ 0xFF);
    h = fnv_str(h, m->name);
    h = fnv_u64(h, hash_voice_prog(&m->vp));
    return h;
}

/* ================================================================
   Layer 3: SectionTrack, Section
   ================================================================ */
uint64_t hash_section_track(const SectionTrack *t){
    uint64_t h = fnv_u64(fnv_init(), SEED_L3);
    h = fnv_str(h, t->name);
    /* Hash patch program if set */
    if(t->patch) h = fnv_u64(h, hash_patch_prog(t->patch));
    else          h = fnv_u64(h, 0xDEAD0000DEAD0000ULL);
    h = fnv_f32(h, t->gain);
    h = fnv_f32(h, t->pan);
    /* Hash all motif uses */
    for(int i=0; i<t->n_uses; i++)
        h = fnv_u64(h, hash_motif_use(&t->uses[i]));
    return h;
}

uint64_t hash_section(const Section *s){
    uint64_t h = fnv_u64(fnv_init(), SEED_L3 ^ 0xFF);
    h = fnv_str(h, s->name);
    h = fnv_f32(h, s->length_beats);
    h = fnv_u64(h, (uint64_t)s->n_tracks);
    for(int i=0; i<s->n_tracks; i++)
        h = fnv_u64(h, hash_section_track(&s->tracks[i]));
    return h;
}

/* ================================================================
   Layer 4: BpmPoint, Song
   ================================================================ */
uint64_t hash_bpm_point(const BpmPoint *b){
    uint64_t h = fnv_u64(fnv_init(), SEED_L4);
    h = fnv_f32(h, b->beat);
    h = fnv_f32(h, b->bpm);
    h = fnv_u64(h, (uint64_t)b->interp);
    return h;
}

uint64_t hash_song(const Song *s){
    uint64_t h = fnv_u64(fnv_init(), SEED_L4 ^ 0xFF);
    h = fnv_str(h, s->name);
    h = fnv_f32(h, s->master_gain);
    h = fnv_f32(h, s->default_bpm);
    /* BPM points */
    for(int i=0; i<s->n_bpm_points; i++)
        h = fnv_u64(h, hash_bpm_point(&s->bpm_points[i]));
    /* Entries: hash section content + structural params */
    for(int i=0; i<s->n_entries; i++){
        const SongEntry *e = &s->entries[i];
        h = fnv_str(h, e->name);
        if(e->section) h = fnv_u64(h, hash_section(e->section));
        h = fnv_u64(h, (uint64_t)e->repeat);
        h = fnv_f32(h, e->fade_in_beats);
        h = fnv_f32(h, e->fade_out_beats);
        h = fnv_f32(h, e->xfade_beats);
        h = fnv_f32(h, e->gap_beats);
    }
    return h;
}

/* ================================================================
   N5-4: EventStream canonical hash
   N5-1: Commutative op normalization
   ================================================================ */

/* ---- Comparator for canonical EventStream hashing (N5-4) ----
 * Sort order: sample ASC, then NOTE_OFF before NOTE_ON at same sample
 * (matches existing tiebreak in motif_compile_uses), then pitch ASC,
 * then quantized velocity ASC.
 * This ensures: two EventStreams with identical event multisets
 * but different insertion orders produce the same hash. */
static int ev_cmp_for_hash(const void *ap, const void *bp){
    const Event *a = (const Event *)ap;
    const Event *b = (const Event *)bp;
    if(a->sample < b->sample) return -1;
    if(a->sample > b->sample) return  1;
    /* Same sample: NOTE_OFF before NOTE_ON before EV_GLIDE */
    if(a->type != b->type){
        /* Lower enum value = earlier in canonical order */
        /* EV_NOTE_ON=0, EV_NOTE_OFF=1, EV_GLIDE=2 — but we want OFF first */
        int ta = (a->type == EV_NOTE_OFF) ? 0 : (a->type == EV_NOTE_ON) ? 1 : 2;
        int tb = (b->type == EV_NOTE_OFF) ? 0 : (b->type == EV_NOTE_ON) ? 1 : 2;
        return ta - tb;
    }
    if(a->pitch < b->pitch) return -1;
    if(a->pitch > b->pitch) return  1;
    int va = VEL8(a->velocity);
    int vb = VEL8(b->velocity);
    return va - vb;
}

uint64_t hash_event_stream(const EventStream *es){
    uint64_t h = fnv_u64(fnv_init(), SEED_L1 ^ 0xEE);
    if(!es || es->n == 0) return fnv_u64(h, 0ULL);

    /* Small-buffer optimization: avoid malloc for the common case (≤128 events).
     * EventStreams from typical motifs have <100 events.
     * For larger streams, heap-allocate a sort buffer.
     * The stored EventStream is NEVER modified — sorting is purely local. */
    enum { SMALL = 128 };
    Event small_buf[SMALL];
    Event *buf = (es->n <= SMALL) ? small_buf
                                  : (Event *)malloc((size_t)es->n * sizeof(Event));
    if(!buf){
        /* Allocation failure fallback: hash in stored order (still useful, not canonical) */
        for(int i = 0; i < es->n; i++){
            const Event *e = &es->events[i];
            h = fnv_u64(h, e->sample);
            h = fnv_u64(h, (uint64_t)e->type);
            h = fnv_u64(h, (uint64_t)e->pitch);
            h = fnv_u64(h, (uint64_t)VEL8(e->velocity));
        }
        return fnv_u64(h, (uint64_t)es->n);
    }

    /* Copy into sort buffer, sort, hash, free (if heap) */
    for(int i = 0; i < es->n; i++) buf[i] = es->events[i];
    qsort(buf, (size_t)es->n, sizeof(Event), ev_cmp_for_hash);

    for(int i = 0; i < es->n; i++){
        h = fnv_u64(h, buf[i].sample);
        h = fnv_u64(h, (uint64_t)buf[i].type);
        h = fnv_u64(h, (uint64_t)buf[i].pitch);
        h = fnv_u64(h, (uint64_t)VEL8(buf[i].velocity));
    }
    h = fnv_u64(h, (uint64_t)es->n);

    if(buf != small_buf) free(buf);
    return h;
}

/*
 * N5-1: Canonicalize commutative ops in a PatchProgram.
 * For ADD, MUL, MIN, MAX: swap src_a and src_b if src_a > src_b.
 * For MIXN: also sort src_a/src_b by register index.
 *
 * This makes ADD r4 r3 and ADD r3 r4 produce identical hashes,
 * cutting symmetric duplicates from the search space roughly in half.
 */
int patch_canonicalize(PatchProgram *prog){
    int changed = 0;
    for(int i = 0; i < prog->n_instrs; i++){
        Instr   ins = prog->code[i];
        uint8_t op  = INSTR_OP(ins);
        uint8_t a   = INSTR_SRC_A(ins);
        uint8_t b   = INSTR_SRC_B(ins);
        int     swap = 0;

        switch((Opcode)op){
        case OP_ADD: case OP_MUL: case OP_MIN: case OP_MAX:
            swap = (a > b);
            break;
        case OP_MIXN:
            /* Only swap if weights would remain symmetric; here we always sort
               by register index regardless of weights — conservative but safe */
            swap = (a > b);
            break;
        default:
            break;
        }

        if(swap){
            /* Swap src_a and src_b, preserve all other fields */
            prog->code[i] = INSTR_PACK(op, INSTR_DST(ins), b, a,
                                       INSTR_IMM_HI(ins), INSTR_IMM_LO(ins));
            changed++;
        }
    }
    return changed;
}
