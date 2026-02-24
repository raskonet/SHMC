/*
 * SHMC Layer 0 — Integration test + WAV output
 * Build: gcc -O2 tests/test_layer0.c src/patch_interp.c src/tables.c -Iinclude -lm -o test_layer0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "patch_builder.h"

#define SR    44100
#define NDUR  44100   /* 1 second */

/* --- Minimal WAV writer --- */
static void write_wav(const char *path, const float *b, int n){
    FILE *f=fopen(path,"wb"); if(!f){perror(path);return;}
    int16_t *p=(int16_t*)malloc(n*2);
    for(int i=0;i<n;i++){float v=b[i];if(v>1)v=1;if(v<-1)v=-1;p[i]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)n*2, rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=1;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*2;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=2,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n,f);
    fclose(f);free(p);
}

static float *render(const PatchProgram *pr, int midi, float vel, int n){
    float *buf=(float*)calloc(n,sizeof(float));
    Patch pa; patch_note_on(&pa,pr,(float)SR,midi,vel);
    float blk[AUDIO_BLOCK]; int i=0;
    while(i<n){
        int c=n-i<AUDIO_BLOCK?n-i:AUDIO_BLOCK;
        patch_step(&pa,blk,c); memcpy(buf+i,blk,c*sizeof(float)); i+=c;
    }
    return buf;
}

/* ===== Patch definitions ===== */

static PatchProgram p_sine_adsr(void){
    PatchBuilder b; pb_init(&b);
    int env=pb_adsr(&b,3,10,22,18);
    int osc=pb_osc(&b,REG_ONE);
    pb_out(&b,pb_mul(&b,osc,env));
    return *pb_finish(&b);
}
static PatchProgram p_saw_lpf(void){
    PatchBuilder b; pb_init(&b);
    int env=pb_adsr(&b,2,8,20,15);
    int saw=pb_saw(&b,REG_ONE);
    int flt=pb_lpf(&b,saw,30);
    pb_out(&b,pb_mul(&b,flt,env));
    return *pb_finish(&b);
}
static PatchProgram p_fm_2op(void){
    PatchBuilder b; pb_init(&b);
    int two=pb_const_f(&b,2.0f);
    int mod=pb_osc(&b,two);
    int car=pb_fm(&b,REG_ONE,mod,20);
    int env=pb_adsr(&b,2,12,18,14);
    pb_out(&b,pb_mul(&b,car,env));
    return *pb_finish(&b);
}
static PatchProgram p_fm_fold(void){
    PatchBuilder b; pb_init(&b);
    int three=pb_const_f(&b,3.0f);
    int mod=pb_osc(&b,three);
    int car=pb_fm(&b,REG_ONE,mod,25);
    int fld=pb_fold(&b,car);
    int flt=pb_lpf(&b,fld,38);
    int env=pb_adsr(&b,1,8,16,12);
    pb_out(&b,pb_mul(&b,flt,env));
    return *pb_finish(&b);
}
static PatchProgram p_noise_bpf(void){
    PatchBuilder b; pb_init(&b);
    int n=pb_noise(&b);
    int f=pb_bpf(&b,n,35,25);
    int e=pb_exp_decay(&b,18);
    pb_out(&b,pb_mul(&b,f,e));
    return *pb_finish(&b);
}
static PatchProgram p_pad(void){
    PatchBuilder b; pb_init(&b);
    int o1=pb_osc(&b,REG_ONE);
    int dt=pb_const_f(&b,1.008f);
    int o2=pb_osc(&b,dt);
    int mx=pb_mix(&b,o1,o2,15,15);
    int lf=pb_const_f(&b,0.03f);
    int lfo=pb_osc(&b,lf);
    int am=pb_am(&b,mx,lfo,8);
    int fl=pb_lpf(&b,am,40);
    int en=pb_adsr(&b,15,5,28,20);
    pb_out(&b,pb_mul(&b,fl,en));
    return *pb_finish(&b);
}
static PatchProgram p_square_hpf(void){
    PatchBuilder b; pb_init(&b);
    int sq=pb_square(&b,REG_ONE);
    int hp=pb_hpf(&b,sq,15);
    int en=pb_adsr(&b,0,8,18,12);
    pb_out(&b,pb_mul(&b,hp,en));
    return *pb_finish(&b);
}
static PatchProgram p_tri_tanh(void){
    PatchBuilder b; pb_init(&b);
    int tr=pb_tri(&b,REG_ONE);
    int gn=pb_const_f(&b,4.0f);
    int dr=pb_mul(&b,tr,gn);
    int st=pb_tanh(&b,dr);
    int en=pb_adsr(&b,2,10,20,15);
    pb_out(&b,pb_mul(&b,st,en));
    return *pb_finish(&b);
}

/* ===== Main ===== */
int main(void){
    tables_init();
    printf("=== SHMC Layer 0  —  Patch Interpreter Test ===\n\n");

    struct { const char *name, *desc; PatchProgram prog; int note; } T[]={
        {"sine_adsr",  "Sine + ADSR",             p_sine_adsr(),   69},
        {"saw_lpf",    "Sawtooth + LPF",           p_saw_lpf(),     60},
        {"fm_2op",     "FM 2-operator",             p_fm_2op(),      60},
        {"fm_fold",    "FM + wavefold + LPF",       p_fm_fold(),     60},
        {"noise_bpf",  "Noise + BPF (snare)",       p_noise_bpf(),   60},
        {"pad",        "Detuned OSC + AM LFO + LPF",p_pad(),         60},
        {"square_hpf", "Square + HPF (buzz)",       p_square_hpf(),  60},
        {"tri_tanh",   "Triangle + tanh saturation",p_tri_tanh(),    60},
    };
    int nt=sizeof(T)/sizeof(T[0]), pass=0, fail=0;

    for(int t=0;t<nt;t++){
        printf("[%s]  %s\n", T[t].name, T[t].desc);
        float *buf=render(&T[t].prog, T[t].note, 0.8f, NDUR);
        float pk=0; int nans=0;
        for(int i=0;i<NDUR;i++){
            if(!isfinite(buf[i]))nans++;
            float av=fabsf(buf[i]); if(av>pk)pk=av;
        }
        char path[256];
        snprintf(path,sizeof(path),"/mnt/user-data/outputs/%s.wav",T[t].name);
        write_wav(path,buf,NDUR);
        if(nans==0 && pk>1e-5f){ printf("  PASS  peak=%.4f\n\n",pk); pass++; }
        else                    { printf("  FAIL  peak=%g  nans=%d\n\n",pk,nans); fail++; }
        free(buf);
    }
    printf("=== %d / %d passed ===\n", pass, nt);
    return fail ? 1 : 0;
}
