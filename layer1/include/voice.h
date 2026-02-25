#pragma once
/*
 * SHMC Layer 1 — Voice DSL  (rev 2)
 *
 * Changes from rev 1:
 *  - OP_ADSR opcode no longer hardcoded; use symbolic OP_ADSR from opcodes.h
 *  - VoiceRenderer tracks a release-tail patch alongside the active patch
 *    so note transitions are glitch-free (old note finishes its release)
 *  - Event scheduling uses integer sample indices, not accumulated floats
 *  - GLIDE cancels the pending note-off of the previous note before
 *    firing the new note-on (true legato behaviour)
 *  - TIE tracks the most-recent NOTE_OFF index explicitly
 *  - voice_compile() returns a human-readable error string on failure
 */

#include <stdint.h>
#include "../../layer0/include/patch.h"
#include "../../layer0/include/opcodes.h"   /* OP_ADSR symbol */

/* ---- Limits ---- */
#define VOICE_MAX_INSTRS  4096
#define VOICE_MAX_EVENTS  8192
#define VOICE_MAX_REPEAT   8

/* ---- Duration indices (into g_dur[7]) ---- */
#define DUR_1_64  0
#define DUR_1_32  1
#define DUR_1_16  2
#define DUR_1_8   3
#define DUR_1_4   4
#define DUR_1_2   5
#define DUR_1     6

/* ---- Velocity indices ---- */
#define VEL_PPPP  0
#define VEL_PPP   1
#define VEL_PP    2
#define VEL_P     3
#define VEL_MP    4
#define VEL_MF    5
#define VEL_F     6
#define VEL_FF    7

extern const float VEL_TABLE[16];

/* ---- VoiceInstr opcodes ---- */
typedef enum {
    VI_NOTE  = 0,
    VI_REST,
    VI_TIE,          /* extend duration of last note-off */
    VI_GLIDE,        /* legato pitch change */
    VI_REPEAT_BEGIN,
    VI_REPEAT_END,
    VI_COUNT
} VIOp;

/* ---- Packed 32-bit instruction ----
   [31:24] opcode   [23:16] pitch   [15:8] dur_idx   [7:0] vel_idx / count */
typedef uint32_t VInstr;
#define VI_PACK(op,pitch,dur,vel) \
    (((uint32_t)(uint8_t)(op)<<24)|((uint32_t)(uint8_t)(pitch)<<16)| \
     ((uint32_t)(uint8_t)(dur)<<8)|((uint32_t)(uint8_t)(vel)))
#define VI_OP(i)    ((uint8_t)((i)>>24))
#define VI_PITCH(i) ((uint8_t)((i)>>16))
#define VI_DUR(i)   ((uint8_t)((i)>> 8))
#define VI_VEL(i)   ((uint8_t)(i))

typedef struct {
    VInstr code[VOICE_MAX_INSTRS];
    int    n;
} VoiceProgram;

/* ---- Event ---- */
typedef enum { EV_NOTE_ON=0, EV_NOTE_OFF, EV_GLIDE } EvType;

typedef struct {
    uint64_t sample;     /* absolute sample index (integer, no float drift) */
    EvType   type;
    uint8_t  pitch;
    float    velocity;
} Event;

typedef struct {
    Event  events[VOICE_MAX_EVENTS];
    int    n;
    uint64_t total_samples;  /* total duration in samples at a given sr/bpm */
    float    total_beats;
} EventStream;

/* ---- VoiceRenderer ---- */
typedef struct {
    const EventStream  *es;
    const PatchProgram *patch_prog;
    float               sr;
    uint64_t            sample_pos;
    int                 ev_cursor;

    /* Active patch (currently playing note) */
    Patch   active;
    int     has_active;

    /* Release tail: the previous note finishing its release */
    Patch   tail;
    int     has_tail;

    /* Amplitude tracker for tail silence detection */
    float   tail_env;

    int     done;
} VoiceRenderer;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile VoiceProgram → EventStream.
 * sr, bpm: needed to convert beats → integer sample indices.
 * Returns 0 on success, -1 on error (err_out filled if non-NULL).
 */
/* T4-3: Hard limit on total events after REPEAT expansion */
#define VOICE_MAX_EVENTS_EXPANDED  8192   /* total events generated (not instrs) */
#define VOICE_MAX_REPEAT_COUNT       64   /* max repeat count per REPEAT block */

int voice_compile(const VoiceProgram *vp, EventStream *es,
                  float sr, float bpm, char *err_out, int err_sz);

/*
 * T4-1: TempoMap-aware voice compile.
 * Converts each beat position to a sample index via tempo_map exactly,
 * using the closed-form integrals rather than a fixed samples-per-beat.
 * start_beat: beat offset within the tempo map where this voice begins.
 */
#ifndef TEMPO_MAP_H_INCLUDED
#define TEMPO_MAP_H_INCLUDED
#include "../../layer0b/include/tempo_map.h"
#endif
int voice_compile_tempo(const VoiceProgram *vp, EventStream *es,
                        const TempoMap *tm, double start_beat,
                        char *err_out, int err_sz);

/*
 * T4-2: Structural hash of a VoiceProgram (FNV-1a over packed instructions).
 */
uint64_t hash_voice(const VoiceProgram *vp);

void voice_renderer_init(VoiceRenderer *vr,
                         const EventStream  *es,
                         const PatchProgram *patch,
                         float sr);

/* Returns 0 while playing, 1 when done. */
int voice_render_block(VoiceRenderer *vr, float *out, int n);

/* Trigger ADSR release on a patch without restarting it */
void patch_release(Patch *p, const PatchProgram *prog);

/* ---- Builder ---- */
typedef struct {
    VoiceProgram vp;
    int          repeat_stack[VOICE_MAX_REPEAT];
    int          rsp;
    int          ok;
    int          last_note_off_idx;  /* tracks most recent NOTE_OFF event index */
} VoiceBuilder;

static inline void vb_init(VoiceBuilder *b){
    b->vp.n=0; b->rsp=0; b->ok=0; b->last_note_off_idx=-1;
}
static inline void vb_emit(VoiceBuilder *b, VInstr vi){
    if(b->vp.n>=VOICE_MAX_INSTRS){b->ok=-1;return;}
    b->vp.code[b->vp.n++]=vi;
}
static inline void vb_note(VoiceBuilder *b, int pitch, int dur, int vel){
    vb_emit(b,VI_PACK(VI_NOTE,pitch,dur,vel));
}
static inline void vb_rest(VoiceBuilder *b, int dur){
    vb_emit(b,VI_PACK(VI_REST,0,dur,0));
}
static inline void vb_tie(VoiceBuilder *b, int dur){
    vb_emit(b,VI_PACK(VI_TIE,0,dur,0));
}
static inline void vb_glide(VoiceBuilder *b, int pitch, int dur, int vel){
    vb_emit(b,VI_PACK(VI_GLIDE,pitch,dur,vel));
}
static inline void vb_repeat_begin(VoiceBuilder *b){
    if(b->rsp>=VOICE_MAX_REPEAT){b->ok=-1;return;}
    b->repeat_stack[b->rsp++]=b->vp.n;
    vb_emit(b,VI_PACK(VI_REPEAT_BEGIN,0,0,0));
}
static inline void vb_repeat_end(VoiceBuilder *b, int n){
    if(b->rsp<=0){b->ok=-1;return;}
    b->rsp--;
    vb_emit(b,VI_PACK(VI_REPEAT_END,0,0,(uint8_t)n));
}
static inline VoiceProgram *vb_finish(VoiceBuilder *b){ return &b->vp; }

#ifdef __cplusplus
}
#endif
