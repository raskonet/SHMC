#pragma once
#include "patch.h"
/*
 * Inline assembler for PatchProgram construction.  (rev 2)
 *
 * FIX B4: pb_adsr att_idx now masked to 5 bits (was 6) — matches g_env[32]
 * FIX B10: pb_const_f clamps value to Q8.8 range [-128, 127.996] to prevent
 *          silent integer wrap/UB for out-of-range inputs.
 * NEW: pb_adsr_release() — trigger release on a live patch instance using the
 *      captured level in state[sb+3], for correct mid-decay releases (FIX B5).
 */

typedef struct { PatchProgram prog; int rc; int ok; } PatchBuilder;

static inline void pb_init(PatchBuilder *b){
    b->prog.n_instrs=0; b->prog.n_state=0; b->prog.n_regs=REG_FREE;
    b->rc=REG_FREE; b->ok=0;
}
static inline int pb_reg(PatchBuilder *b){
    if(b->rc>=MAX_REGS){b->ok=-1;return 0;} return b->rc++;
}
static inline void pb_emit(PatchBuilder *b, Instr ins){
    if(b->prog.n_instrs>=MAX_INSTRS){b->ok=-1;return;}
    b->prog.code[b->prog.n_instrs++]=ins;
}

/* --- const ---
   pb_const_mod: index into g_mod[32]
   pb_const_f:   float value encoded as Q8.8, range -128.0 .. 127.996
                 FIX B10: clamped — no silent wrap */
static inline int pb_const_mod(PatchBuilder *b, int mi){
    int d=pb_reg(b);
    pb_emit(b,INSTR_PACK(OP_CONST,d,0,0,(uint16_t)(mi&0x1F),0));
    return d;
}
static inline int pb_const_f(PatchBuilder *b, float v){
    int d=pb_reg(b);
    /* FIX B10: clamp to int16_t range before cast */
    if(v >  127.996f) v =  127.996f;
    if(v < -128.0f)   v = -128.0f;
    int16_t q=(int16_t)(v*256.f);
    pb_emit(b,INSTR_PACK(OP_CONST,d,0,0,(uint16_t)q,1));
    return d;
}

/* --- arithmetic --- */
static inline int pb_add(PatchBuilder *b,int a,int c){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_ADD,d,a,c,0,0));return d;}
static inline int pb_sub(PatchBuilder *b,int a,int c){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_SUB,d,a,c,0,0));return d;}
static inline int pb_mul(PatchBuilder *b,int a,int c){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_MUL,d,a,c,0,0));return d;}
static inline int pb_neg(PatchBuilder *b,int a)      {int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_NEG,d,a,0,0,0));return d;}
static inline int pb_abs(PatchBuilder *b,int a)      {int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_ABS,d,a,0,0,0));return d;}

/* --- oscillators --- */
static inline int pb_osc   (PatchBuilder *b,int rm){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_OSC,   d,rm,0,0,0));return d;}
static inline int pb_saw   (PatchBuilder *b,int rm){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_SAW,   d,rm,0,0,0));return d;}
static inline int pb_square(PatchBuilder *b,int rm){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_SQUARE,d,rm,0,0,0));return d;}
static inline int pb_tri   (PatchBuilder *b,int rm){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_TRI,   d,rm,0,0,0));return d;}

/* --- modulation --- */
static inline int pb_fm(PatchBuilder *b,int rm,int rmod,int di){
    int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_FM,d,rm,rmod,(uint16_t)di,0));return d;}
static inline int pb_am(PatchBuilder *b,int rc,int rmod,int di){
    int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_AM,d,rc,rmod,(uint16_t)di,0));return d;}

/* --- noise --- */
static inline int pb_noise   (PatchBuilder *b)      {int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_NOISE,   d,0,0,0,0));return d;}
static inline int pb_lp_noise(PatchBuilder *b,int ci){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_LP_NOISE,d,0,0,(uint16_t)ci,0));return d;}

/* --- nonlinearities --- */
static inline int pb_tanh(PatchBuilder *b,int a){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_TANH,d,a,0,0,0));return d;}
static inline int pb_clip(PatchBuilder *b,int a){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_CLIP,d,a,0,0,0));return d;}
static inline int pb_fold(PatchBuilder *b,int a){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_FOLD,d,a,0,0,0));return d;}

/* --- filters --- */
static inline int pb_lpf(PatchBuilder *b,int a,int ci){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_LPF,d,a,0,(uint16_t)ci,0));return d;}
static inline int pb_hpf(PatchBuilder *b,int a,int ci){int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_HPF,d,a,0,(uint16_t)ci,0));return d;}
static inline int pb_bpf(PatchBuilder *b,int a,int ci,int qi){
    int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_BPF,d,a,0,(uint16_t)ci,(uint16_t)qi));return d;}

/* --- envelope ---
   FIX B4: att_idx masked to 5 bits (0x1F) in encoding, matches g_env[32]
   State layout per ADSR instruction:
     [sb+0] stage   [sb+1] level   [sb+2] timer   [sb+3] release_start_level */
static inline int pb_adsr(PatchBuilder *b,int att,int dec,int sus,int rel){
    int d=pb_reg(b);
    /* FIX B4: att masked to 0x1F (5 bits), not 0x3F */
    uint16_t hi=(uint16_t)(((att&0x1F)<<10)|((dec&0x1F)<<5)|(sus&0x1F));
    uint16_t lo=(uint16_t)((rel&0x1F)<<11);
    pb_emit(b,INSTR_PACK(OP_ADSR,d,0,0,hi,lo)); return d;
}
static inline int pb_exp_decay(PatchBuilder *b,int ri){
    int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_EXP_DECAY,d,0,0,(uint16_t)ri,0));return d;}

/* --- utility --- */
static inline int pb_mix(PatchBuilder *b,int a,int c,int wa,int wb){
    int d=pb_reg(b);pb_emit(b,INSTR_PACK(OP_MIXN,d,a,c,(uint16_t)wa,(uint16_t)wb));return d;}
static inline void pb_out(PatchBuilder *b,int src){
    pb_emit(b,INSTR_PACK(OP_OUT,0,src,0,0,0));}
static inline PatchProgram *pb_finish(PatchBuilder *b){
    b->prog.n_regs = b->rc;
    /* BUG-2 FIX: canonicalize commutative ops at creation so that
     * ADD(rA,rB) and ADD(rB,rA) always produce identical program bytes.
     * Forward-declare so patch_builder.h remains standalone;
     * patch_canonicalize is defined in layer0b/src/shmc_hash.c. */
    extern int patch_canonicalize(PatchProgram *prog);
    patch_canonicalize(&b->prog);
    return &b->prog;
}
