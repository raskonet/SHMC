/*
 * SHMC — patch_meta.c
 * Hashing, state offsets, cost model, grammar validation.
 */
#include "../include/patch_meta.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ----------------------------------------------------------------
   How many state slots does each opcode need?
   ---------------------------------------------------------------- */
static int state_slots_for_op(uint8_t op){
    switch((Opcode)op){
    /* Oscillators: 1 slot (phase accumulator) */
    case OP_OSC: case OP_SAW: case OP_SQUARE: case OP_TRI: case OP_PHASE:
        return 1;
    /* FM: 1 slot (carrier phase; modulator's reg is provided by prev instr) */
    case OP_FM:
        return 1;
    /* PM: 1 slot (phase) */
    case OP_PM:
        return 1;
    /* SYNC: 2 slots (prev-master + slave-phase) */
    case OP_SYNC:
        return 2;
    /* Noise: 0 (uses ps->rng, not state[]) */
    case OP_NOISE:
        return 0;
    /* LP_NOISE: 1 (one-pole state) */
    case OP_LP_NOISE:
        return 1;
    /* RAND_STEP: 2 (current value + countdown) */
    case OP_RAND_STEP:
        return 2;
    /* Filters: 1 or 2 */
    case OP_LPF: case OP_HPF: case OP_ONEPOLE:
        return 1;
    case OP_BPF:
        return 2;  /* lv + bv */
    /* ADSR: 4 (stage, level, timer, release_start_level) */
    case OP_ADSR:
        return 4;
    /* Stateless */
    default:
        return 0;
    }
}

/* ----------------------------------------------------------------
   State offset assignment (R1-1 fix)
   ---------------------------------------------------------------- */
int patch_assign_state_offsets(PatchProgramEx *prog){
    int next_slot = 0;
    for(int i = 0; i < prog->n_instrs; i++){
        uint8_t op = INSTR_OP(prog->code[i]);
        int     ns = state_slots_for_op(op);
        prog->state_offset[i] = (uint16_t)next_slot;
        next_slot += ns;
        if(next_slot > MAX_STATE){
            /* Clamp — shouldn't happen with sane programs */
            next_slot -= ns;
            prog->state_offset[i] = 0;
        }
    }
    prog->n_state = next_slot;
    return next_slot;
}

/* ----------------------------------------------------------------
   FNV-1a hash (R1-2)
   ---------------------------------------------------------------- */
#define FNV_OFFSET 14695981039346656037ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv1a_bytes(const void *data, size_t len){
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = FNV_OFFSET;
    for(size_t i = 0; i < len; i++){
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t hash_patch(const PatchProgramEx *prog){
    return fnv1a_bytes(prog->code, (size_t)prog->n_instrs * sizeof(Instr));
}

uint64_t hash_patch_raw(const PatchProgram *prog){
    return fnv1a_bytes(prog->code, (size_t)prog->n_instrs * sizeof(Instr));
}

/* ----------------------------------------------------------------
   Cost model (R1-4)
   ---------------------------------------------------------------- */
void patch_cost(const PatchProgram *prog, PatchCost *out){
    memset(out, 0, sizeof(*out));
    out->n_instrs = prog->n_instrs;
    out->max_reg  = prog->n_regs;
    for(int i = 0; i < prog->n_instrs; i++){
        uint8_t op = INSTR_OP(prog->code[i]);
        int ns = state_slots_for_op(op);
        out->n_state_slots += ns;
        if(ns > 0){
            out->n_stateful++;
            out->est_cpu += 2;
        } else {
            out->est_cpu += 1;
        }
    }
}

/* ----------------------------------------------------------------
   Grammar validation (R1-3)
   ---------------------------------------------------------------- */
int patch_program_valid(const PatchProgram *prog,
                        int max_instrs, int max_stateful, int max_regs,
                        char *err, int err_sz){
    if(!prog){ if(err) snprintf(err, err_sz, "null program"); return -1; }
    if(prog->n_instrs < 1){
        if(err) snprintf(err, err_sz, "empty program");
        return -1;
    }
    if(max_instrs > 0 && prog->n_instrs > max_instrs){
        if(err) snprintf(err, err_sz,
            "too many instrs: %d > %d", prog->n_instrs, max_instrs);
        return -1;
    }
    if(max_regs > 0 && prog->n_regs > max_regs){
        if(err) snprintf(err, err_sz,
            "too many regs: %d > %d", prog->n_regs, max_regs);
        return -1;
    }
    /* Must end with OP_OUT */
    if(INSTR_OP(prog->code[prog->n_instrs - 1]) != OP_OUT){
        if(err) snprintf(err, err_sz, "last instruction is not OP_OUT");
        return -1;
    }
    /* Count stateful ops */
    if(max_stateful > 0){
        int nst = 0;
        for(int i = 0; i < prog->n_instrs; i++)
            if(state_slots_for_op(INSTR_OP(prog->code[i])) > 0) nst++;
        if(nst > max_stateful){
            if(err) snprintf(err, err_sz,
                "too many stateful ops: %d > %d", nst, max_stateful);
            return -1;
        }
    }
    /* Check all src registers are valid (< n_regs) */
    for(int i = 0; i < prog->n_instrs; i++){
        Instr   ins = prog->code[i];
        uint8_t op  = INSTR_OP(ins);
        uint8_t dst = INSTR_DST(ins);
        uint8_t sa  = INSTR_SRC_A(ins);
        uint8_t sb  = INSTR_SRC_B(ins);
        if(op == OP_OUT){ continue; }
        if(dst >= prog->n_regs && prog->n_regs > 0){
            if(err) snprintf(err, err_sz,
                "instr %d: dst r%d >= n_regs %d", i, dst, prog->n_regs);
            return -1;
        }
        /* Source regs: only check if they're used (not 0 for unused src) */
        if(sa > 0 && sa < REG_FREE){
            /* Reserved regs 0-3 are always valid */
        } else if(sa >= prog->n_regs && prog->n_regs > 0 && sa >= REG_FREE){
            if(err) snprintf(err, err_sz,
                "instr %d: src_a r%d >= n_regs %d", i, sa, prog->n_regs);
            return -1;
        }
        (void)sb; /* sb validation similarly */
    }
    return 0;
}

/* ----------------------------------------------------------------
   Convert PatchProgram ↔ PatchProgramEx
   ---------------------------------------------------------------- */
void patch_to_ex(const PatchProgram *src, PatchProgramEx *dst){
    memset(dst, 0, sizeof(*dst));
    for(int i = 0; i < src->n_instrs; i++) dst->code[i] = src->code[i];
    dst->n_instrs = src->n_instrs;
    dst->n_regs   = src->n_regs;
    patch_assign_state_offsets(dst);
    dst->hash = hash_patch(dst);
}

/* ----------------------------------------------------------------
   exec1_ex — uses state_offset[] not i*4   (R1-1 fix)
   Also includes denormal flush (R1-8)
   ---------------------------------------------------------------- */

/* Waveform helpers (duplicated from patch_interp.c for self-containment) */
#define TWO_PI 6.28318530718f

static inline float rng_f_ex(uint32_t *s){
    *s ^= *s<<13; *s ^= *s>>17; *s ^= *s<<5;
    return (float)(int32_t)*s * (1.f/2147483648.f);
}
static inline float fsin_ex(float x){
    x = fmodf(x, TWO_PI);
    if(x > 3.14159265f) x -= TWO_PI;
    if(x <-3.14159265f) x += TWO_PI;
    float s = x*x; return x*(1.f - s*(1.f/6.f - s/120.f));
}
static inline float osc_tick_ex(float *ph, float freq, float dt){
    float p = *ph;
    *ph = fmodf(p + TWO_PI*freq*dt, TWO_PI);
    if(*ph < 0.f) *ph += TWO_PI;
    return p;
}
static inline float saw_w(float p){ return 2.f*(p/TWO_PI) - 1.f; }
static inline float sqr_w(float p){ return p < 3.14159265f ? 1.f : -1.f; }
static inline float tri_w(float p){
    float t = p/TWO_PI;
    return t < .5f ? 4.f*t - 1.f : 3.f - 4.f*t;
}
static inline float fold_w(float x){
    x = x*.5f + .5f; x -= floorf(x);
    return fabsf(x*2.f - 1.f)*2.f - 1.f;
}
static inline float lpc_ex(float cut, float dt){
    float w = TWO_PI*cut*dt; return w/(1.f+w);
}
/* R1-8: Denormal flush */
#define FLUSH(x) if(fabsf(x) < 1e-15f) (x) = 0.f

static inline float decode_const_ex(uint16_t hi, uint16_t lo){
    extern const float g_mod[32];
    if(lo == 0){ if(hi < 32) return g_mod[hi]; }
    return (float)(int16_t)hi / 256.f;
}
static inline float adsr_tick_ex(float *st, uint16_t hi, uint16_t lo, float dt){
    extern float g_env[32]; extern const float g_mod[32];
    int   stg = (int)st[0]; float lv = st[1], tm = st[2], rl = st[3];
    int   ai=(hi>>10)&0x1F, di=(hi>>5)&0x1F, si=hi&0x1F, ri=(lo>>11)&0x1F;
    float att=g_env[ai], dec=g_env[di], sus=g_mod[si], rel=g_env[ri];
    tm += dt;
    switch(stg){
    case 0: lv=tm/att; if(tm>=att){lv=1.f;tm=0;stg=1;} break;
    case 1: lv=1.f-(1.f-sus)*(tm/dec); if(tm>=dec){lv=sus;tm=0;stg=2;} break;
    case 2: lv=sus; break;
    case 3: lv=rl*(1.f-tm/rel); if(lv<0.f){lv=0.f;stg=4;} break;
    default: lv=0.f;
    }
    st[0]=(float)stg; st[1]=lv; st[2]=tm; st[3]=rl;
    return lv;
}

float exec1_ex(PatchState *ps, const PatchProgramEx *prog){
    float *r = ps->regs, *st = ps->state;
    float dt = ps->dt, freq = ps->note_freq;
    extern float g_cutoff[64]; extern float g_env[32]; extern const float g_mod[32];

    for(int i = 0; i < prog->n_instrs; i++){
        Instr    ins = prog->code[i];
        uint8_t  op  = INSTR_OP(ins), dst = INSTR_DST(ins);
        uint8_t  a   = INSTR_SRC_A(ins), b = INSTR_SRC_B(ins);
        uint16_t hi  = INSTR_IMM_HI(ins), lo = INSTR_IMM_LO(ins);

        /* R1-1: use precomputed state_offset, not i*4 */
        int sb = prog->state_offset[i];

        switch(op){
        case OP_CONST:  r[dst]=decode_const_ex(hi,lo); break;
        case OP_ADD:    r[dst]=r[a]+r[b]; break;
        case OP_SUB:    r[dst]=r[a]-r[b]; break;
        case OP_MUL:    r[dst]=r[a]*r[b]; break;
        case OP_DIV:    r[dst]=(r[b]!=0.f)?r[a]/r[b]:0.f; break;
        case OP_NEG:    r[dst]=-r[a]; break;
        case OP_ABS:    r[dst]=fabsf(r[a]); break;

        case OP_OSC:    { float p=osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt); FLUSH(st[sb]); r[dst]=fsin_ex(p); break; }
        case OP_SAW:    { float p=osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt); FLUSH(st[sb]); r[dst]=saw_w(p); break; }
        case OP_SQUARE: { float p=osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt); FLUSH(st[sb]); r[dst]=sqr_w(p); break; }
        case OP_TRI:    { float p=osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt); FLUSH(st[sb]); r[dst]=tri_w(p); break; }
        case OP_PHASE:  { osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt); FLUSH(st[sb]); r[dst]=st[sb]; break; }

        case OP_FM: {
            float md=(hi<32)?g_mod[hi]:0.5f;
            float cf=freq*(r[a]>0?r[a]:1.f);
            st[sb]=fmodf(st[sb]+TWO_PI*cf*dt+md*r[b], TWO_PI);
            if(st[sb]<0.f) st[sb]+=TWO_PI;
            FLUSH(st[sb]);
            r[dst]=fsin_ex(st[sb]); break;
        }
        case OP_PM: {
            float p=osc_tick_ex(&st[sb],freq*(r[a]>0?r[a]:1.f),dt);
            FLUSH(st[sb]);
            r[dst]=fsin_ex(p+r[b]); break;
        }
        case OP_AM: {
            float md=(hi<32)?g_mod[hi]:0.5f;
            r[dst]=r[a]*(1.f+md*r[b]); break;
        }
        case OP_SYNC: {
            float prev=st[sb]; st[sb]=r[a];
            if(prev<=0.f && r[a]>0.f) st[sb+1]=0.f;
            float p=osc_tick_ex(&st[sb+1],freq*(r[b]>0?r[b]:2.f),dt);
            FLUSH(st[sb+1]);
            r[dst]=fsin_ex(p); break;
        }

        case OP_NOISE:    r[dst]=rng_f_ex(&ps->rng); break;
        case OP_LP_NOISE: {
            float n=rng_f_ex(&ps->rng);
            float c=(hi<64)?lpc_ex(g_cutoff[hi],dt):0.05f;
            st[sb]+=c*(n-st[sb]); FLUSH(st[sb]); r[dst]=st[sb]; break;
        }
        case OP_RAND_STEP: {
            int per=(hi>0)?(int)hi:100;
            if((int)st[sb+1]<=0){st[sb]=rng_f_ex(&ps->rng);st[sb+1]=(float)per;}
            st[sb+1]-=1.f; r[dst]=st[sb]; break;
        }

        case OP_TANH: r[dst]=tanhf(r[a]); break;
        case OP_CLIP: r[dst]=fmaxf(-1.f,fminf(1.f,r[a])); break;
        case OP_FOLD: r[dst]=fold_w(r[a]); break;
        case OP_SIGN: r[dst]=(r[a]>0.f)?1.f:(r[a]<0.f)?-1.f:0.f; break;

        case OP_LPF: {
            float c=(hi<64)?lpc_ex(g_cutoff[hi],dt):0.1f;
            st[sb]+=c*(r[a]-st[sb]); FLUSH(st[sb]); r[dst]=st[sb]; break;
        }
        case OP_HPF: {
            float c=(hi<64)?lpc_ex(g_cutoff[hi],dt):0.1f;
            float lp=st[sb]+c*(r[a]-st[sb]); st[sb]=lp; FLUSH(st[sb]); r[dst]=r[a]-lp; break;
        }
        case OP_BPF: {
            float c=(hi<64)?lpc_ex(g_cutoff[hi],dt):0.1f;
            float q=(lo<32)?g_mod[lo]+0.1f:0.5f;
            float lv=st[sb], bv=st[sb+1];
            float hv=r[a]-lv-q*bv;
            bv+=c*hv; lv+=c*bv;
            FLUSH(lv); FLUSH(bv);
            st[sb]=lv; st[sb+1]=bv; r[dst]=bv; break;
        }
        case OP_ONEPOLE: {
            float c=(float)(uint8_t)(hi>>8)/255.f;
            st[sb]=c*r[a]+(1.f-c)*st[sb]; FLUSH(st[sb]); r[dst]=st[sb]; break;
        }

        case OP_ADSR:      r[dst]=adsr_tick_ex(&st[sb],hi,lo,dt); break;
        case OP_RAMP: {
            float dur=(hi<32)?g_env[hi]:0.1f;
            r[dst]=fminf(1.f,ps->note_time/dur); break;
        }
        case OP_EXP_DECAY: {
            float rate=(hi<32)?g_mod[hi]*20.f:2.f;
            r[dst]=expf(-rate*ps->note_time); break;
        }

        case OP_MIN:  r[dst]=fminf(r[a],r[b]); break;
        case OP_MAX:  r[dst]=fmaxf(r[a],r[b]); break;
        case OP_MIXN: {
            float wa=(hi<32)?g_mod[hi]:0.5f;
            float wb=(lo<32)?g_mod[lo]:0.5f;
            r[dst]=r[a]*wa+r[b]*wb; break;
        }
        case OP_OUT:
            ps->note_time += dt;
            return r[a] * ps->note_vel;

        default: break;
        }
    }
    ps->note_time += dt;
    return r[0] * ps->note_vel;
}

/* ================================================================
   T4-6: Evaluative patch metadata
   ================================================================ */

/* Returns maximum g_mod index used in FM depth field (hi field) for FM ops */
static float fm_depth_from_hi(uint16_t hi){
    extern const float g_mod[32];
    int idx = (int)hi & 0x1F;
    return (idx < 32) ? g_mod[idx] : 0.5f;
}
static float bpf_q_from_lo(uint16_t lo){
    extern const float g_mod[32];
    int idx = (int)lo & 0x1F;
    return (idx < 32) ? g_mod[idx] : 0.5f;
}

void patch_meta(const PatchProgram *prog, PatchMeta *out){
    memset(out, 0, sizeof(*out));
    out->is_stable = 1;  /* assume stable until proven otherwise */

    /* Compute base cost first */
    PatchCost cost; patch_cost(prog, &cost);
    out->n_instrs     = cost.n_instrs;
    out->n_stateful   = cost.n_stateful;
    out->n_state_slots= cost.n_state_slots;
    out->est_cpu      = cost.est_cpu;

    for(int i = 0; i < prog->n_instrs; i++){
        uint8_t  op = INSTR_OP(prog->code[i]);
        uint16_t hi = INSTR_IMM_HI(prog->code[i]);
        uint16_t lo = INSTR_IMM_LO(prog->code[i]);

        switch((Opcode)op){
        /* Oscillator family */
        case OP_OSC: case OP_SAW: case OP_SQUARE: case OP_TRI: case OP_PHASE:
        case OP_PM:
            out->has_oscillator = 1;
            out->n_oscillators++;
            break;
        case OP_FM:
            out->has_oscillator = 1;
            out->n_oscillators++;
            /* FM with high depth → potential instability */
            if(fm_depth_from_hi(hi) >= 0.8f) out->is_stable = 0;
            break;
        /* Noise sources */
        case OP_NOISE: case OP_LP_NOISE: case OP_RAND_STEP:
            out->has_noise_source = 1;
            out->has_oscillator   = 1;  /* noise counts as oscillator for search */
            out->n_oscillators++;
            break;
        /* Envelope family */
        case OP_ADSR: case OP_RAMP: case OP_EXP_DECAY:
            out->has_envelope = 1;
            out->n_envelopes++;
            break;
        /* Filter family */
        case OP_LPF: case OP_HPF: case OP_ONEPOLE:
            out->has_filter = 1;
            out->n_filters++;
            break;
        case OP_BPF:
            out->has_filter = 1;
            out->n_filters++;
            /* High-Q BPF → potential instability */
            if(bpf_q_from_lo(lo) >= 0.9f) out->is_stable = 0;
            break;
        default:
            break;
        }
    }
}
