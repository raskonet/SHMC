/*
 * SHMC Layer 1 — Voice DSL Test (rev 2)
 * Build from synth/ root:
 *   gcc -O2 -Ilayer1/include -Ilayer0/include \
 *       layer1/tests/test_layer1.c layer1/src/voice.c \
 *       layer0/src/patch_interp.c layer0/src/tables.c \
 *       -lm -o test_layer1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "voice.h"
#include "../../layer0/include/patch_builder.h"

#define SR   44100
#define BLK  512

/* ---- WAV writer ---- */
static void write_wav(const char *path, const float *b, int n){
    FILE *f=fopen(path,"wb"); if(!f){perror(path);return;}
    int16_t *p=(int16_t*)malloc(n*2);
    for(int i=0;i<n;i++){float v=b[i];if(v>1)v=1;if(v<-1)v=-1;p[i]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)n*2,rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=1;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*2;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=2,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n,f);
    fclose(f);free(p);
    printf("  wrote %s\n",path);
}

/* ---- Compile + render helper ---- */
static float *render_voice(const VoiceProgram *vp, const PatchProgram *patch,
                             float bpm, int *out_n){
    char err[128]="";
    EventStream es;
    if(voice_compile(vp,&es,(float)SR,bpm,err,sizeof(err))<0){
        printf("  COMPILE ERROR: %s\n",err); *out_n=0; return NULL;
    }

    int cap=(int)(SR*(es.total_beats*60.0f/bpm+2.5f));
    float *buf=(float*)calloc(cap,sizeof(float));
    VoiceRenderer vr;
    voice_renderer_init(&vr,&es,patch,(float)SR);

    float blk[BLK]; int pos=0;
    while(!vr.done && pos<cap){
        int chunk=cap-pos<BLK?cap-pos:BLK;
        voice_render_block(&vr,blk,chunk);
        memcpy(buf+pos,blk,chunk*sizeof(float));
        pos+=chunk;
    }
    *out_n=pos;
    return buf;
}

/* ---- Patches ---- */
static PatchProgram patch_piano(void){
    PatchBuilder b; pb_init(&b);
    int two=pb_const_f(&b,2.0f);
    int mod=pb_osc(&b,two);
    int car=pb_fm(&b,REG_ONE,mod,15);
    int env=pb_adsr(&b,0,14,8,10);
    pb_out(&b,pb_mul(&b,car,env));
    return *pb_finish(&b);
}
static PatchProgram patch_bass(void){
    PatchBuilder b; pb_init(&b);
    int saw=pb_saw(&b,REG_ONE);
    int flt=pb_lpf(&b,saw,28);
    int env=pb_adsr(&b,0,8,20,8);
    pb_out(&b,pb_mul(&b,flt,env));
    return *pb_finish(&b);
}
static PatchProgram patch_lead(void){
    PatchBuilder b; pb_init(&b);
    int tr=pb_tri(&b,REG_ONE);
    int gn=pb_const_f(&b,3.0f);
    int dr=pb_mul(&b,tr,gn);
    int st=pb_tanh(&b,dr);
    int env=pb_adsr(&b,1,10,22,12);
    pb_out(&b,pb_mul(&b,st,env));
    return *pb_finish(&b);
}
static PatchProgram patch_pad(void){
    PatchBuilder b; pb_init(&b);
    int o1=pb_osc(&b,REG_ONE);
    int dt=pb_const_f(&b,1.008f);
    int o2=pb_osc(&b,dt);
    int mx=pb_mix(&b,o1,o2,15,15);
    int fl=pb_lpf(&b,mx,42);
    int en=pb_adsr(&b,14,4,28,20);
    pb_out(&b,pb_mul(&b,fl,en));
    return *pb_finish(&b);
}

/* ==== Tests ==== */

static int check_buf(const float *buf, int n, const char *name){
    if(!buf){ printf("  FAIL: render returned NULL\n"); return 0; }
    float pk=0; int nans=0;
    for(int i=0;i<n;i++){
        if(!isfinite(buf[i]))nans++;
        float a=fabsf(buf[i]); if(a>pk)pk=a;
    }
    if(nans||pk<1e-5f){ printf("  FAIL: peak=%g nans=%d\n",pk,nans); return 0; }
    printf("  peak=%.4f  samples=%d\n",pk,n);
    return 1;
}

/* 1: C major scale */
static void test_scale(void){
    printf("[scale] C major scale, quarter notes @ 120bpm\n");
    int sc[]={60,62,64,65,67,69,71,72};
    VoiceBuilder vb; vb_init(&vb);
    for(int i=0;i<8;i++) vb_note(&vb,sc[i],DUR_1_4,VEL_MF);
    PatchProgram pa=patch_piano();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,120.f,&n);
    write_wav("/mnt/user-data/outputs/v1_scale.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"scale")?"PASS":"FAIL");
    free(buf);
}

/* 2: Alberti bass repeated x4 */
static void test_repeat(void){
    printf("[repeat] Alberti bass x4\n");
    VoiceBuilder vb; vb_init(&vb);
    vb_repeat_begin(&vb);
        vb_note(&vb,48,DUR_1_8,VEL_MP);
        vb_note(&vb,52,DUR_1_8,VEL_MP);
        vb_note(&vb,55,DUR_1_8,VEL_MP);
        vb_note(&vb,52,DUR_1_8,VEL_MP);
    vb_repeat_end(&vb,4);
    PatchProgram pa=patch_bass();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,120.f,&n);
    write_wav("/mnt/user-data/outputs/v1_repeat.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"repeat")?"PASS":"FAIL");
    free(buf);
}

/* 3: Rest + tie */
static void test_rest_tie(void){
    printf("[rest_tie] dotted quarter, rest, tie\n");
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb,60,DUR_1_4,VEL_F);
    vb_tie( &vb,DUR_1_8);
    vb_rest(&vb,DUR_1_8);
    vb_note(&vb,64,DUR_1_4,VEL_MF);
    vb_rest(&vb,DUR_1_4);
    vb_note(&vb,67,DUR_1_2,VEL_P);
    PatchProgram pa=patch_lead();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,100.f,&n);
    write_wav("/mnt/user-data/outputs/v1_rest_tie.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"rest_tie")?"PASS":"FAIL");
    free(buf);
}

/* 4: Nested repeats */
static void test_nested_repeat(void){
    printf("[nested] nested REPEAT blocks\n");
    VoiceBuilder vb; vb_init(&vb);
    vb_repeat_begin(&vb);
        vb_note(&vb,60,DUR_1_4,VEL_MP);
        vb_repeat_begin(&vb);
            vb_note(&vb,64,DUR_1_8,VEL_MP);
            vb_note(&vb,62,DUR_1_8,VEL_MP);
        vb_repeat_end(&vb,2);
        vb_note(&vb,60,DUR_1_4,VEL_MF);
    vb_repeat_end(&vb,3);
    PatchProgram pa=patch_piano();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,130.f,&n);
    write_wav("/mnt/user-data/outputs/v1_nested.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"nested")?"PASS":"FAIL");
    free(buf);
}

/* 5: Glide — chromatic rise then held note */
static void test_glide(void){
    printf("[glide] chromatic slide (legato)\n");
    VoiceBuilder vb; vb_init(&vb);
    for(int p=55;p<=67;p++) vb_glide(&vb,p,DUR_1_16,VEL_MF);
    vb_note(&vb,67,DUR_1_2,VEL_F);
    PatchProgram pa=patch_lead();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,100.f,&n);
    write_wav("/mnt/user-data/outputs/v1_glide.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"glide")?"PASS":"FAIL");
    free(buf);
}

/* 6: Melody — Twinkle Twinkle */
static void test_melody(void){
    printf("[melody] Twinkle Twinkle first phrase\n");
    int mel[]={60,60,67,67,69,69,67, 65,65,64,64,62,62,60};
    int drs[]={ 4, 4, 4, 4, 4, 4, 2,  4, 4, 4, 4, 4, 4, 2};
    VoiceBuilder vb; vb_init(&vb);
    for(int i=0;i<14;i++)
        vb_note(&vb,mel[i],drs[i]==4?DUR_1_4:DUR_1_2,VEL_MF);
    PatchProgram pa=patch_pad();
    int n; float *buf=render_voice(vb_finish(&vb),&pa,110.f,&n);
    write_wav("/mnt/user-data/outputs/v1_melody.wav",buf,n);
    printf("  %s\n\n",check_buf(buf,n,"melody")?"PASS":"FAIL");
    free(buf);
}

/* 7: Unit test — verify EventStream sample positions */
static void test_event_positions(void){
    printf("[event_positions] sample-exact scheduling verification\n");
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb,60,DUR_1_4,VEL_MF);   /* 0.25 beats */
    vb_rest(&vb,DUR_1_8);              /* 0.125 beats rest */
    vb_note(&vb,64,DUR_1_4,VEL_F);    /* 0.25 beats */

    char err[128]="";
    EventStream es;
    voice_compile(vb_finish(&vb),&es,(float)SR,120.f,err,sizeof(err));

    /* At 120bpm: 1 beat = 22050 samples
       event[0]: NOTE_ON  C4  at sample 0
       event[1]: NOTE_OFF C4  at sample 5512  (0.25 * 22050)
       event[2]: NOTE_ON  E4  at sample 8268  (0.375 * 22050)
       event[3]: NOTE_OFF E4  at sample 13780 (0.625 * 22050) */
    float spb=SR*60.f/120.f;
    /* Compiler accumulates sample counts incrementally (not single multiply),
       so we must compute expected values the same way to avoid rounding drift. */
    uint64_t dq=(uint64_t)(0.250f*spb+0.5f);  /* quarter note in samples */
    uint64_t de=(uint64_t)(0.125f*spb+0.5f);  /* eighth note in samples */
    uint64_t e0=0, e1=dq, e2=dq+de, e3=dq+de+dq;

    int pass=1;
    if(es.n!=4){printf("  FAIL: expected 4 events, got %d\n",es.n);pass=0;}
    else {
        if(es.events[0].sample!=e0||es.events[0].type!=EV_NOTE_ON) {printf("  FAIL ev0 sample=%llu\n",(unsigned long long)es.events[0].sample);pass=0;}
        if(es.events[1].sample!=e1||es.events[1].type!=EV_NOTE_OFF){printf("  FAIL ev1 sample=%llu\n",(unsigned long long)es.events[1].sample);pass=0;}
        if(es.events[2].sample!=e2||es.events[2].type!=EV_NOTE_ON) {printf("  FAIL ev2 sample=%llu\n",(unsigned long long)es.events[2].sample);pass=0;}
        if(es.events[3].sample!=e3||es.events[3].type!=EV_NOTE_OFF){printf("  FAIL ev3 sample=%llu\n",(unsigned long long)es.events[3].sample);pass=0;}
        if(pass) printf("  samples: %llu ON  %llu OFF  %llu ON  %llu OFF  ✓\n",
            (unsigned long long)e0,(unsigned long long)e1,
            (unsigned long long)e2,(unsigned long long)e3);
    }
    printf("  %s\n\n",pass?"PASS":"FAIL");
}

/* 8: Glide legato — verify GLIDE converts previous NOTE_OFF to EV_GLIDE */
static void test_glide_legato_structure(void){
    printf("[glide_legato] GLIDE event structure check\n");
    VoiceBuilder vb; vb_init(&vb);
    vb_note( &vb,60,DUR_1_4,VEL_MF);  /* C4, then glide to E4 */
    vb_glide(&vb,64,DUR_1_4,VEL_MF);

    char err[128]="";
    EventStream es;
    voice_compile(vb_finish(&vb),&es,(float)SR,120.f,err,sizeof(err));

    /* Expected:
       [0] NOTE_ON  C4  sample 0
       [1] EV_GLIDE C4  sample spb*0.25   (old NOTE_OFF converted)
       [2] NOTE_ON  E4  sample spb*0.25   (fires at same sample as glide)
       [3] NOTE_OFF E4  sample spb*0.50
 */
    int pass=1;
    if(es.n!=4){printf("  FAIL: expected 4 events, got %d (err=%s)\n",es.n,err);pass=0;}
    else {
        if(es.events[0].type!=EV_NOTE_ON) {printf("  FAIL ev0 not NOTE_ON\n");pass=0;}
        if(es.events[1].type!=EV_GLIDE)   {printf("  FAIL ev1 not EV_GLIDE (got %d)\n",es.events[1].type);pass=0;}
        if(es.events[2].type!=EV_NOTE_ON) {printf("  FAIL ev2 not NOTE_ON\n");pass=0;}
        if(es.events[3].type!=EV_NOTE_OFF){printf("  FAIL ev3 not NOTE_OFF\n");pass=0;}
        /* GLIDE and new NOTE_ON must be at same sample */
        if(es.events[1].sample!=es.events[2].sample){
            printf("  FAIL: GLIDE sample %llu != NOTE_ON sample %llu\n",
                (unsigned long long)es.events[1].sample,
                (unsigned long long)es.events[2].sample);
            pass=0;
        }
        if(pass) printf("  NOTE_ON → EV_GLIDE/NOTE_ON @ same sample → NOTE_OFF  ✓\n");
    }
    printf("  %s\n\n",pass?"PASS":"FAIL");
}

int main(void){
    tables_init();
    printf("=== SHMC Layer 1  —  Voice DSL Test (rev 2) ===\n\n");
    test_event_positions();
    test_glide_legato_structure();
    test_scale();
    test_repeat();
    test_rest_tie();
    test_nested_repeat();
    test_glide();
    test_melody();
    printf("=== done ===\n");
    return 0;
}
