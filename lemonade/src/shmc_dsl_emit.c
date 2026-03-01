/*
 * shmc_dsl_emit.c — World-to-DSL serializer   v2
 *
 * Implements the reverse of the DSL compiler: ShmcWorld → DSL text.
 *
 * Round-trip guarantee:
 *   DSL1 → world1 → DSL2 → world2
 *   hash(world1) == hash(world2)   [verified: verify_roundtrip.c]
 *
 * Design rules:
 *   1. Canonical ordering: PATCH → MOTIF → SECTION → SONG
 *   2. Deterministic float formatting: %.3f (3 decimal places always)
 *   3. SSA register reconstruction: tracks dst regs → $N indices
 *   4. Patch instruction decoding: reads Instr bits using INSTR_* macros
 *
 * v2 fix: OP_OSC emits "osc sin ONE" (subtype required by DSL compiler)
 */
#include "../include/shmc_dsl_emit.h"
#include "../include/shmc_dsl.h"
#include "../../layer0/include/patch.h"
#include "../../layer0/include/opcodes.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include "../../layer4/include/song.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── Buffer helper ──────────────────────────────────────────────── */
typedef struct { char *buf; int pos; int cap; int overflow; } Buf;

static void bprintf(Buf *b, const char *fmt, ...) {
    if (b->overflow) return;
    va_list ap; va_start(ap, fmt);
    int rem = b->cap - b->pos;
    int n = vsnprintf(b->buf + b->pos, rem > 0 ? rem : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= rem) { b->overflow = 1; return; }
    b->pos += n;
}

/* ── Patch serializer ───────────────────────────────────────────── */
/*
 * SSA register reconstruction:
 *   PatchProgram uses absolute register numbers (REG_FREE=4, 5, 6...).
 *   DSL uses $0=first instruction's output, $1=second, etc.
 *   We maintain a reverse map: reg_to_dsl[actual_reg] = DSL $N index.
 *   Special: REG_ONE (3) → "ONE"
 */
#define REG_MAP_SIZE 256
static int emit_patch(Buf *b, const PatchProgram *pp, const char *name) {
    bprintf(b, "PATCH %s {\n", name);

    int reg_to_dsl[REG_MAP_SIZE];
    memset(reg_to_dsl, -1, sizeof(reg_to_dsl));
    int n_dsl = 0;

    for (int i = 0; i < pp->n_instrs; i++) {
        Instr ins = pp->code[i];
        uint8_t op  = INSTR_OP(ins);
        uint8_t dst = INSTR_DST(ins);
        uint8_t sa  = INSTR_SRC_A(ins);
        uint8_t sb  = INSTR_SRC_B(ins);
        uint16_t hi = INSTR_IMM_HI(ins);
        uint16_t lo = INSTR_IMM_LO(ins);

        #define REGFMT(r, rbuf) do { \
            if ((r) == REG_ONE) snprintf(rbuf, 16, "ONE"); \
            else if (reg_to_dsl[(r)] >= 0) snprintf(rbuf, 16, "$%d", reg_to_dsl[(r)]); \
            else snprintf(rbuf, 16, "?%d", (r)); \
        } while(0)

        char ra[16], rb[16];

        switch (op) {
        case OP_OSC:
            REGFMT(sa, ra);
            bprintf(b, "  osc sin %s\n", ra);  /* v2: 'sin' subtype required by compiler */
            break;
        case OP_SAW:
            REGFMT(sa, ra);
            bprintf(b, "  saw %s\n", ra);
            break;
        case OP_SQUARE:
            REGFMT(sa, ra);
            bprintf(b, "  sqr %s\n", ra);
            break;
        case OP_TRI:
            REGFMT(sa, ra);
            bprintf(b, "  tri %s\n", ra);
            break;
        case OP_NOISE:
            bprintf(b, "  noise\n");
            break;
        case OP_FM: {
            REGFMT(sa, ra); REGFMT(sb, rb);
            int dep = (int)hi;
            bprintf(b, "  fm %s %s %d\n", ra, rb, dep);
            break;
        }
        case OP_LPF: {
            REGFMT(sa, ra);
            bprintf(b, "  lpf %s %d\n", ra, (int)hi);
            break;
        }
        case OP_HPF: {
            REGFMT(sa, ra);
            bprintf(b, "  hpf %s %d\n", ra, (int)hi);
            break;
        }
        case OP_BPF: {
            REGFMT(sa, ra);
            bprintf(b, "  bpf %s %d %d\n", ra, (int)hi, (int)lo);
            break;
        }
        case OP_TANH:
            REGFMT(sa, ra);
            bprintf(b, "  tanh %s\n", ra);
            break;
        case OP_CLIP:
            REGFMT(sa, ra);
            bprintf(b, "  clip %s\n", ra);
            break;
        case OP_FOLD:
            REGFMT(sa, ra);
            bprintf(b, "  fold %s\n", ra);
            break;
        case OP_ADSR: {
            int att = (hi >> 10) & 0x1F;
            int dec = (hi >>  5) & 0x1F;
            int sus = (hi      ) & 0x1F;
            int rel = (lo >> 11) & 0x1F;
            bprintf(b, "  adsr %d %d %d %d\n", att, dec, sus, rel);
            break;
        }
        case OP_MUL:
            REGFMT(sa, ra); REGFMT(sb, rb);
            bprintf(b, "  mul %s %s\n", ra, rb);
            break;
        case OP_ADD:
            REGFMT(sa, ra); REGFMT(sb, rb);
            bprintf(b, "  add %s %s\n", ra, rb);
            break;
        case OP_MIXN:
            REGFMT(sa, ra); REGFMT(sb, rb);
            bprintf(b, "  mix %s %s %d %d\n", ra, rb, (int)hi, (int)lo);
            break;
        case OP_OUT:
            REGFMT(sa, ra);
            bprintf(b, "  out %s\n", ra);
            continue;  /* OUT has no dst — skip registration */
        case OP_CONST:
            bprintf(b, "  # const (internal)\n");
            break;
        default:
            bprintf(b, "  # op%d (unknown)\n", op);
            break;
        }

        if (dst >= REG_FREE && dst < REG_MAP_SIZE)
            reg_to_dsl[dst] = n_dsl++;

        #undef REGFMT
    }
    bprintf(b, "}\n\n");
    return 0;
}

/* ── Motif serializer ───────────────────────────────────────────── */
static int emit_motif(Buf *b, const Motif *m) {
    bprintf(b, "MOTIF %s {\n", m->name);
    const VoiceProgram *vp = &m->vp;
    for (int i = 0; i < vp->n; i++) {
        VInstr vi = vp->code[i];
        if (VI_OP(vi) == VI_NOTE) {
            bprintf(b, "  note %d %d %d\n",
                    VI_PITCH(vi), VI_DUR(vi), VI_VEL(vi));
        } else if (VI_OP(vi) == VI_REST) {
            bprintf(b, "  rest %d\n", VI_DUR(vi));
        }
    }
    bprintf(b, "}\n\n");
    return 0;
}

/* ── Section serializer ─────────────────────────────────────────── */
static const char *find_patch_name(const ShmcWorld *w, const PatchProgram *pp) {
    for (int i = 0; i < w->n_patches; i++)
        if (&w->patches[i] == pp) return w->patch_names[i];
    return "unknown";
}

static int emit_section(Buf *b, const ShmcWorld *w, const Section *sec,
                        const char *name) {
    bprintf(b, "SECTION %s %.3f {\n", name, (double)sec->length_beats);
    for (int ti = 0; ti < sec->n_tracks; ti++) {
        const SectionTrack *trk = &sec->tracks[ti];
        const char *pname = find_patch_name(w, trk->patch);
        for (int ui = 0; ui < trk->n_uses; ui++) {
            const MotifUse *u = &trk->uses[ui];
            bprintf(b, "  use %s @ %.3f", u->name, (double)u->start_beat);
            if (u->repeat > 1)
                bprintf(b, " x%d", u->repeat);
            bprintf(b, " patch %s", pname);
            if (u->transpose != 0)
                bprintf(b, " t=%d", u->transpose);
            if (fabsf(u->vel_scale - 1.0f) > 0.001f)
                bprintf(b, " v=%.3f", (double)u->vel_scale);
            if (fabsf(u->time_scale - 1.0f) > 0.001f)
                bprintf(b, " ts=%.3f", (double)u->time_scale);
            bprintf(b, "\n");
        }
    }
    bprintf(b, "}\n\n");
    return 0;
}

/* ── Song serializer ────────────────────────────────────────────── */
static int emit_song(Buf *b, const ShmcWorld *w, int song_idx) {
    const Song *s = &w->songs[song_idx];
    bprintf(b, "SONG %s %.3f {\n", s->name, (double)s->default_bpm);
    for (int ei = 0; ei < s->n_entries; ei++) {
        const SongEntry *e = &s->entries[ei];
        bprintf(b, "  play %s", e->name);
        if (e->repeat > 1)
            bprintf(b, " x%d", e->repeat);
        if (fabsf(e->fade_in_beats)  > 0.001f)
            bprintf(b, " fi=%.3f",  (double)e->fade_in_beats);
        if (fabsf(e->fade_out_beats) > 0.001f)
            bprintf(b, " fo=%.3f",  (double)e->fade_out_beats);
        if (fabsf(e->xfade_beats)    > 0.001f)
            bprintf(b, " xf=%.3f",  (double)e->xfade_beats);
        if (fabsf(e->gap_beats)      > 0.001f)
            bprintf(b, " gap=%.3f", (double)e->gap_beats);
        bprintf(b, "\n");
    }
    bprintf(b, "}\n");
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────── */
int shmc_world_to_dsl(const ShmcWorld *w, char *out, int cap) {
    Buf b = { out, 0, cap, 0 };
    for (int i = 0; i < w->n_patches;  i++)
        emit_patch(&b, &w->patches[i], w->patch_names[i]);
    if (w->lib) {
        for (int i = 0; i < w->lib->n; i++)
            if (w->lib->entries[i].valid)
                emit_motif(&b, &w->lib->entries[i]);
    }
    for (int i = 0; i < w->n_sections; i++)
        emit_section(&b, w, &w->sections[i], w->section_names[i]);
    for (int i = 0; i < w->n_songs;    i++)
        emit_song(&b, w, i);
    if (b.overflow) return -1;
    return b.pos;
}
