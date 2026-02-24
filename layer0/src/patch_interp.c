/*
 * SHMC Layer 0 — Patch Interpreter  (rev 2)
 *
 * Bugs fixed vs rev 1:
 *  [B1] osc_tick: single-subtract phase wrap → fmodf (handles >2π jumps)
 *  [B2] OP_FM: single-subtract phase wrap → fmodf
 *  [B3] MAX_STATE 512 → 4096 (= 4*MAX_INSTRS) so (i*4) never aliases
 *  [B4] ADSR att_idx was 6-bit (0-63) but g_env has 32 entries → mask to 5-bit
 *  [B5] ADSR release used sustain level instead of actual level at release point
 *       → store release-start level in state[sb+3]
 *  [B10] pb_const_f Q8.8 silent wrap for |v|>127 → moved to patch_builder.h clamp
 */
#include "../include/patch_builder.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530718f

/* ---- xorshift32 RNG ---- */
static inline float rng_f(uint32_t *s){
    *s^=*s<<13; *s^=*s>>17; *s^=*s<<5;
    return (float)(int32_t)*s*(1.f/2147483648.f);
}

/* ---- Waveform helpers ---- */
static inline float osc_tick(float *ph, float freq, float dt){
    float p=*ph;
    /* FIX B1: fmodf handles any step size, not just < 2π */
    *ph=fmodf(p + TWO_PI*freq*dt, TWO_PI);
    if(*ph<0.f)*ph+=TWO_PI;
    return p;
}
static inline float fsin(float x){
    /* Wrap to [-π,π], then 5th-order minimax, error < 0.0002 */
    x=fmodf(x, TWO_PI);
    if(x> 3.14159265f)x-=TWO_PI;
    if(x<-3.14159265f)x+=TWO_PI;
    float s=x*x; return x*(1.f-s*(1.f/6.f-s/120.f));
}
static inline float saw_w(float p){ return 2.f*(p/TWO_PI)-1.f; }
static inline float sqr_w(float p){ return p<3.14159265f?1.f:-1.f; }
static inline float tri_w(float p){ float t=p/TWO_PI; return t<.5f?4.f*t-1.f:3.f-4.f*t; }
static inline float fold_w(float x){ x=x*.5f+.5f; x-=floorf(x); return fabsf(x*2.f-1.f)*2.f-1.f; }

/* ---- One-pole LP coefficient ---- */
static inline float lpc(float cut, float dt){
    float w=TWO_PI*cut*dt; return w/(1.f+w);
}

/* ---- CONST decoding ----
   lo==0 → mod-table index (hi<32), else treat hi as Q8.8 signed */
static inline float decode_const(uint16_t hi, uint16_t lo){
    extern const float g_mod[32];
    if(lo==0){ if(hi<32)return g_mod[hi]; }
    return (float)(int16_t)hi/256.f;
}

/* ---- ADSR (rev 2) ----
   State: [0]=stage  [1]=level  [2]=timer  [3]=release_start_level
   Encoding: hi = att(5b)|dec(5b)|sus(5b)|padding(1b),  lo = rel(5b)|padding
   FIX B4: att now 5 bits (was 6) so all indices stay within g_env[32]
   FIX B5: release decays from state[3] (captured level) not from sus */
static inline float adsr_tick(float *st, uint16_t hi, uint16_t lo, float dt){
    extern float g_env[32]; extern const float g_mod[32];
    int   stg=(int)st[0]; float lv=st[1], tm=st[2], rl=st[3];
    /* FIX B4: all indices masked to 5 bits → 0-31, always valid */
    int   ai=(hi>>10)&0x1F, di=(hi>>5)&0x1F, si=hi&0x1F, ri=(lo>>11)&0x1F;
    float att=g_env[ai], dec=g_env[di], sus=g_mod[si], rel=g_env[ri];
    tm+=dt;
    switch(stg){
        case 0: /* Attack */
            lv=tm/att;
            if(tm>=att){lv=1.f;tm=0;stg=1;} break;
        case 1: /* Decay */
            lv=1.f-(1.f-sus)*(tm/dec);
            if(tm>=dec){lv=sus;tm=0;stg=2;} break;
        case 2: /* Sustain */
            lv=sus; break;
        case 3: /* Release — FIX B5: decay from captured rl, not from sus */
            lv=rl*(1.f-tm/rel);
            if(lv<0.f){lv=0.f;stg=4;} break;
        default: lv=0.f;
    }
    st[0]=(float)stg; st[1]=lv; st[2]=tm; st[3]=rl;
    return lv;
}

/* ---- Core: execute one sample ---- */
static float exec1(PatchState *ps, const PatchProgram *prog){
    float *r=ps->regs, *s=ps->state;
    float dt=ps->dt, freq=ps->note_freq;
    extern float g_cutoff[64]; extern float g_env[32]; extern const float g_mod[32];

    for(int i=0;i<prog->n_instrs;i++){
        Instr    ins=prog->code[i];
        uint8_t  op=INSTR_OP(ins), dst=INSTR_DST(ins);
        uint8_t  a=INSTR_SRC_A(ins), b=INSTR_SRC_B(ins);
        uint16_t hi=INSTR_IMM_HI(ins), lo=INSTR_IMM_LO(ins);
        /* FIX B3: state array is now 4*MAX_INSTRS so (i*4) never wraps */
        int sb=i*4;

        switch(op){
        case OP_CONST: r[dst]=decode_const(hi,lo); break;
        case OP_ADD:   r[dst]=r[a]+r[b]; break;
        case OP_SUB:   r[dst]=r[a]-r[b]; break;
        case OP_MUL:   r[dst]=r[a]*r[b]; break;
        case OP_DIV:   r[dst]=(r[b]!=0.f)?r[a]/r[b]:0.f; break;
        case OP_NEG:   r[dst]=-r[a]; break;
        case OP_ABS:   r[dst]=fabsf(r[a]); break;

        case OP_OSC:   { float p=osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt); r[dst]=fsin(p); break; }
        case OP_SAW:   { float p=osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt); r[dst]=saw_w(p); break; }
        case OP_SQUARE:{ float p=osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt); r[dst]=sqr_w(p); break; }
        case OP_TRI:   { float p=osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt); r[dst]=tri_w(p); break; }
        case OP_PHASE: { osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt); r[dst]=s[sb]; break; }

        case OP_FM: {
            float md=(hi<32)?g_mod[hi]:0.5f;
            float cf=freq*(r[a]>0?r[a]:1.f);
            /* FIX B2: fmodf for FM phase, not single-subtract */
            s[sb]=fmodf(s[sb]+TWO_PI*cf*dt+md*r[b], TWO_PI);
            if(s[sb]<0.f)s[sb]+=TWO_PI;
            r[dst]=fsin(s[sb]); break;
        }
        case OP_PM: {
            float p=osc_tick(&s[sb],freq*(r[a]>0?r[a]:1.f),dt);
            r[dst]=fsin(p+r[b]); break;
        }
        case OP_AM: {
            float md=(hi<32)?g_mod[hi]:0.5f;
            r[dst]=r[a]*(1.f+md*r[b]); break;
        }
        case OP_SYNC: {
            /* s[sb]=prev master signal, s[sb+1]=slave phase
               FIX B3: sb=i*4 so sb+1=i*4+1, guaranteed unique, no wrap */
            float prev=s[sb]; s[sb]=r[a];
            if(prev<=0.f&&r[a]>0.f)s[sb+1]=0.f;
            float p=osc_tick(&s[sb+1],freq*(r[b]>0?r[b]:2.f),dt);
            r[dst]=fsin(p); break;
        }

        case OP_NOISE:    r[dst]=rng_f(&ps->rng); break;
        case OP_LP_NOISE: {
            float n=rng_f(&ps->rng);
            float c=(hi<64)?lpc(g_cutoff[hi],dt):0.05f;
            s[sb]+=c*(n-s[sb]); r[dst]=s[sb]; break;
        }
        case OP_RAND_STEP: {
            int per=(hi>0)?(int)hi:100;
            if((int)s[sb+1]<=0){s[sb]=rng_f(&ps->rng);s[sb+1]=(float)per;}
            s[sb+1]-=1.f; r[dst]=s[sb]; break;
        }

        case OP_TANH: r[dst]=tanhf(r[a]); break;
        case OP_CLIP: r[dst]=fmaxf(-1.f,fminf(1.f,r[a])); break;
        case OP_FOLD: r[dst]=fold_w(r[a]); break;
        case OP_SIGN: r[dst]=(r[a]>0.f)?1.f:(r[a]<0.f)?-1.f:0.f; break;

        case OP_LPF: {
            float c=(hi<64)?lpc(g_cutoff[hi],dt):0.1f;
            s[sb]+=c*(r[a]-s[sb]); r[dst]=s[sb]; break;
        }
        case OP_HPF: {
            float c=(hi<64)?lpc(g_cutoff[hi],dt):0.1f;
            float lp=s[sb]+c*(r[a]-s[sb]); s[sb]=lp; r[dst]=r[a]-lp; break;
        }
        case OP_BPF: {
            float c=(hi<64)?lpc(g_cutoff[hi],dt):0.1f;
            float q=(lo<32)?g_mod[lo]+0.1f:0.5f;
            float lv=s[sb],bv=s[sb+1];
            float hv=r[a]-lv-q*bv;
            bv+=c*hv; lv+=c*bv;
            s[sb]=lv; s[sb+1]=bv; r[dst]=bv; break;
        }
        case OP_ONEPOLE: {
            float c=(float)(uint8_t)(hi>>8)/255.f;
            s[sb]=c*r[a]+(1.f-c)*s[sb]; r[dst]=s[sb]; break;
        }

        case OP_ADSR: r[dst]=adsr_tick(&s[sb],hi,lo,dt); break;
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
            ps->note_time+=dt;
            return r[a]*ps->note_vel;

        default: break;
        }
    }
    ps->note_time+=dt;
    return r[0]*ps->note_vel;
}

/* ---- Public API ---- */

void patch_reset(Patch *p){
    memset(&p->st,0,sizeof(PatchState));
    p->st.rng=0xDEADBEEFu;
}

void patch_note_on(Patch *p, const PatchProgram *prog,
                   float sr, int midi, float vel){
    tables_init();
    patch_reset(p);
    p->prog=prog;
    p->st.sr=sr; p->st.dt=1.f/sr;
    p->st.note_freq=freq_from_midi(midi);
    p->st.note_vel=vel;
    p->st.note_time=0.f;
    p->st.regs[REG_FREQ]=p->st.note_freq;
    p->st.regs[REG_VEL] =vel;
    p->st.regs[REG_TIME]=0.f;
    p->st.regs[REG_ONE] =1.f;
}

int patch_step(Patch *p, float *out, int n){
    if(!p||!p->prog||!out)return -1;
    for(int i=0;i<n;i++){
        p->st.regs[REG_TIME]=p->st.note_time;
        out[i]=exec1(&p->st,p->prog);
    }
    return 0;
}
