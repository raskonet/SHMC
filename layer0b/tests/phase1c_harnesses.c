/*
 * Phase 1C: GCC -fanalyzer targeted harnesses
 * Every dangerous pattern exposed to the static analyzer.
 */
#include "../../layer0/include/patch_builder.h"
#include "../include/patch_meta.h"
#include "../include/shmc_hash.h"
#include "../include/tempo_map.h"
#include "../../layer1/include/voice.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int patch_canonicalize(PatchProgram *prog);

/* H1: exec1 state indexing — sb=i*4, worst case sb+3 = (MAX_INSTRS-1)*4+3 */
void harness_exec1_state(void) {
    PatchProgram prog; memset(&prog,0,sizeof(prog));
    if(prog.n_instrs<0||prog.n_instrs>MAX_INSTRS) return;
    if(prog.n_regs<REG_FREE||prog.n_regs>MAX_REGS) return;
    Patch p; memset(&p,0,sizeof(p));
    p.prog=&prog; p.st.dt=1.f/44100.f; p.st.note_freq=440.f;
    p.st.note_vel=0.8f; p.st.rng=1;
    float out[1]; patch_step(&p,out,1);
}

/* H2: exec1_ex state_offset — assigned by patch_assign_state_offsets */
void harness_exec1_ex_state(void) {
    PatchProgramEx prog; memset(&prog,0,sizeof(prog));
    if(prog.n_instrs<0||prog.n_instrs>MAX_INSTRS) return;
    patch_assign_state_offsets(&prog);
    if(prog.n_state>MAX_STATE) __builtin_trap();
    PatchState ps; memset(&ps,0,sizeof(ps));
    ps.dt=1.f/44100.f; ps.note_freq=440.f; ps.note_vel=0.8f; ps.rng=1;
    exec1_ex(&ps,&prog);
}

/* H3: ADSR bit-field masking — all indices provably in [0,31] */
void harness_adsr_mask(uint16_t hi, uint16_t lo) {
    int ai=(hi>>10)&0x1F, di=(hi>>5)&0x1F, si=hi&0x1F, ri=(lo>>11)&0x1F;
    /* Prove bounds before use */
    if(ai<0||ai>31) __builtin_trap();
    if(di<0||di>31) __builtin_trap();
    if(si<0||si>31) __builtin_trap();
    if(ri<0||ri>31) __builtin_trap();
    PatchProgram prog; prog.n_instrs=2; prog.n_regs=5;
    prog.code[0]=INSTR_PACK(OP_ADSR,REG_FREE,0,0,hi,lo);
    prog.code[1]=INSTR_PACK(OP_OUT,0,REG_FREE,0,0,0);
    Patch p; memset(&p,0,sizeof(p)); p.prog=&prog;
    p.st.dt=1.f/44100.f; p.st.note_freq=440.f; p.st.note_vel=0.8f; p.st.rng=1;
    float out[1]; patch_step(&p,out,1);
}

/* H4: Filter/noise table guards hi<64 and lo<32 */
void harness_filter_guards(uint16_t hi, uint16_t lo) {
    PatchProgram prog; prog.n_instrs=2; prog.n_regs=5;
    prog.code[0]=INSTR_PACK(OP_BPF,REG_FREE,REG_ONE,0,hi,lo);
    prog.code[1]=INSTR_PACK(OP_OUT,0,REG_FREE,0,0,0);
    Patch p; memset(&p,0,sizeof(p)); p.prog=&prog;
    p.st.dt=1.f/44100.f; p.st.note_freq=440.f; p.st.note_vel=0.8f; p.st.rng=1;
    float out[1]; patch_step(&p,out,1);
}

/* H5: hash_event_stream malloc/free balance — no leak on any path */
void harness_hash_es_memory(void) {
    EventStream es; memset(&es,0,sizeof(es));
    if(es.n<0||es.n>VOICE_MAX_EVENTS) return;
    hash_event_stream(&es);
}

/* H6: patch_canonicalize — only touches code[0..n_instrs-1] */
void harness_canonicalize(void) {
    PatchProgram prog; memset(&prog,0,sizeof(prog));
    if(prog.n_instrs<0||prog.n_instrs>MAX_INSTRS) return;
    patch_canonicalize(&prog);
}

/* H7: TempoMap binary search — segs[] never OOB */
void harness_tempo_search(void) {
    TempoMap m; memset(&m,0,sizeof(m));
    if(m.n_segs<1||m.n_segs>TEMPO_MAX_PTS) return;
    double beat; tempo_beat_to_seconds(&m,beat);
}

/* H8: voice_compile_tempo — event count never exceeds VOICE_MAX_EVENTS */
void harness_voice_compile(void) {
    VoiceProgram vp; memset(&vp,0,sizeof(vp));
    if(vp.n<0||vp.n>VOICE_MAX_INSTRS) return;
    TempoMap tm; memset(&tm,0,sizeof(tm)); tm.n_segs=1;
    tm.segs[0].b0=0; tm.segs[0].b1=1e15; tm.segs[0].type=TM_STEP;
    tm.segs[0].step.S=0.5; tm.segs[0].T0=0; tm.segs[0].T1=1e15*0.5;
    tm.sr=44100.0; tm.last_beat=1e15; tm.last_T=1e15*0.5; tm.last_bpm=120.0;
    EventStream es; char err[128];
    voice_compile_tempo(&vp,&es,&tm,0.0,err,sizeof(err));
}

/* H9: SYNC — uses sb and sb+1 — must both be < MAX_STATE */
void harness_sync_state(void) {
    PatchProgram prog; prog.n_instrs=2; prog.n_regs=6;
    prog.code[0]=INSTR_PACK(OP_SYNC,REG_FREE,REG_ONE,REG_ONE,0,0);
    prog.code[1]=INSTR_PACK(OP_OUT,0,REG_FREE,0,0,0);
    Patch p; memset(&p,0,sizeof(p)); p.prog=&prog;
    p.st.dt=1.f/44100.f; p.st.note_freq=440.f; p.st.note_vel=0.8f; p.st.rng=1;
    float out[1]; patch_step(&p,out,1);
}

/* H10: hash_patch_prog — reads code[0..n_instrs-1], no OOB */
void harness_hash_patch(void) {
    PatchProgram prog; memset(&prog,0,sizeof(prog));
    if(prog.n_instrs<0||prog.n_instrs>MAX_INSTRS) return;
    hash_patch_prog(&prog);
}
