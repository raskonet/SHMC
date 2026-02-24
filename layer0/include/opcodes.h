#pragma once
#include <stdint.h>

typedef uint64_t Instr;

#define INSTR_PACK(op,dst,a,b,hi,lo) \
    (((uint64_t)(uint8_t)(op)<<56)|((uint64_t)(uint8_t)(dst)<<48)| \
     ((uint64_t)(uint8_t)(a)<<40)|((uint64_t)(uint8_t)(b)<<32)|   \
     ((uint64_t)(uint16_t)(hi)<<16)|((uint64_t)(uint16_t)(lo)))

#define INSTR_OP(i)     ((uint8_t)((i)>>56))
#define INSTR_DST(i)    ((uint8_t)((i)>>48))
#define INSTR_SRC_A(i)  ((uint8_t)((i)>>40))
#define INSTR_SRC_B(i)  ((uint8_t)((i)>>32))
#define INSTR_IMM_HI(i) ((uint16_t)((i)>>16))
#define INSTR_IMM_LO(i) ((uint16_t)(i))

typedef enum {
    OP_CONST=0,OP_ADD,OP_SUB,OP_MUL,OP_DIV,OP_NEG,OP_ABS,
    OP_OSC,OP_SAW,OP_SQUARE,OP_TRI,OP_PHASE,
    OP_FM,OP_PM,OP_AM,OP_SYNC,
    OP_NOISE,OP_LP_NOISE,OP_RAND_STEP,
    OP_TANH,OP_CLIP,OP_FOLD,OP_SIGN,
    OP_LPF,OP_HPF,OP_BPF,OP_ONEPOLE,
    OP_ADSR,OP_RAMP,OP_EXP_DECAY,
    OP_MIN,OP_MAX,OP_MIXN,OP_OUT,
    OP_COUNT
} Opcode;

#define MAX_REGS    256
#define MAX_INSTRS  1024
/* FIX B3: 4 state slots per instruction, no modulo wrap */
#define MAX_STATE   (MAX_INSTRS * 4)
#define AUDIO_BLOCK 64
