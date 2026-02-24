#pragma once
#include "opcodes.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reserved register indices */
#define REG_FREQ  0   /* note frequency Hz */
#define REG_VEL   1   /* note velocity 0..1 */
#define REG_TIME  2   /* note_time seconds */
#define REG_ONE   3   /* constant 1.0 */
#define REG_FREE  4   /* first free register */

/* Discrete constant tables (runtime-initialized by tables_init()) */
extern float g_freq[128];    /* MIDI 0-127 -> Hz, equal temperament A4=440 */
extern float g_cutoff[64];   /* 20 Hz .. 20000 Hz, 64 log steps */
extern float g_env[32];      /* 1ms .. 4s,  32 log steps */
extern const float g_mod[32];/* 0.0 .. 1.0, 32 linear steps */
extern const float g_dur[7]; /* 1/64 1/32 1/16 1/8 1/4 1/2 1 beat */

/* Program: flat array of instructions */
typedef struct {
    Instr code[MAX_INSTRS];
    int   n_instrs;
    int   n_regs;
    int   n_state;
} PatchProgram;

/* Per-voice execution state */
typedef struct {
    float    regs[MAX_REGS];
    float    state[MAX_STATE]; /* persistent between step() calls */
    float    note_freq;
    float    note_vel;
    float    note_time;
    float    sr;
    float    dt;
    uint32_t rng;
} PatchState;

/* Patch = program + state */
typedef struct {
    const PatchProgram *prog;
    PatchState          st;
} Patch;

void  tables_init(void);
void  patch_note_on(Patch *p, const PatchProgram *prog,
                    float sr, int midi, float vel);
int   patch_step(Patch *p, float *out, int n);
void  patch_reset(Patch *p);
float freq_from_midi(int m);
float env_time(int i);
float cutoff_hz(int i);

#ifdef __cplusplus
}
#endif
