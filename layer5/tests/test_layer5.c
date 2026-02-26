/*
 * SHMC Layer 5 — Patch Search Test
 *
 * Build from synth/ root:
 *   gcc -O2 \
 *     -Ilayer0/include -Ilayer5/include \
 *     layer5/tests/test_layer5.c \
 *     layer5/src/patch_search.c \
 *     layer0/src/patch_interp.c layer0/src/tables.c \
 *     -lm -o test_layer5 && ./test_layer5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "patch_search.h"
#include "../../layer0/include/patch_builder.h"

#define SR   44100

static void write_wav(const char *path, const float *buf, int n){
    FILE *f = fopen(path,"wb"); if(!f){perror(path);return;}
    int16_t *p = (int16_t*)malloc((size_t)n*2);
    for(int i=0;i<n;i++){float v=buf[i];if(v>1)v=1;if(v<-1)v=-1;p[i]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)n*2, rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=1;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*2;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=2,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n,f);
    fclose(f);free(p);
    printf("  wrote %s\n",path);
}

static void render_patch(const PatchProgram *prog, float *out, int midi){
    Patch p;
    patch_note_on(&p, prog, (float)SR, midi, 0.85f);
    patch_step(&p, out, FEAT_TOTAL_LEN);
}

static void normalise(float *buf, int n){
    float pk = 1e-12f;
    for(int i=0;i<n;i++){float a=fabsf(buf[i]);if(a>pk)pk=a;}
    for(int i=0;i<n;i++) buf[i]/=pk;
}

/* Simple local xorshift32 — avoids depending on internal search symbols */
static uint32_t my_rng(uint32_t *s){
    *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s;
}

/* ============================================================
   TEST 1: feat_extract — basic sanity
   ============================================================ */
static int test_feat_extract(void){
    printf("[feat_extract] feature extraction sanity\n");

    /* Silent buffer: RMS and ZCR must be exactly 0 */
    static float silence[FEAT_TOTAL_LEN];
    memset(silence,0,sizeof(silence));
    FeatureVec fv0; feat_extract(silence, FEAT_TOTAL_LEN, &fv0);
    for(int i=0;i<FEAT_FRAMES;i++){
        if(fv0.rms[i]!=0.f||fv0.zcr[i]!=0.f){
            printf("  FAIL: silence rms[%d]=%g zcr[%d]=%g\n",
                   i,fv0.rms[i],i,fv0.zcr[i]); return 0;}
    }
    printf("  silence rms/zcr all-zero ✓\n");

    /* 440 Hz sine: RMS ~0.707 */
    static float sine[FEAT_TOTAL_LEN];
    float ph=0.f, dp=2.f*3.14159265f*440.f/(float)SR;
    for(int i=0;i<FEAT_TOTAL_LEN;i++){sine[i]=sinf(ph);ph+=dp;}
    FeatureVec fvs; feat_extract(sine, FEAT_TOTAL_LEN, &fvs);

    float rms_mean=0.f;
    for(int i=0;i<FEAT_FRAMES;i++) rms_mean+=fvs.rms[i];
    rms_mean/=FEAT_FRAMES;
    int ok_rms = fabsf(rms_mean-0.707f)<0.05f;
    printf("  sine rms_mean=%.3f (want ~0.707) %s\n", rms_mean, ok_rms?"✓":"✗");

    /* ZCR for 440 Hz sine should be ~0.02 crossings/sample */
    float zcr_mean=0.f;
    for(int i=0;i<FEAT_FRAMES;i++) zcr_mean+=fvs.zcr[i];
    zcr_mean/=FEAT_FRAMES;
    int ok_zcr = (zcr_mean>0.01f && zcr_mean<0.05f);
    printf("  sine zcr_mean=%.4f (want 0.01-0.05) %s\n", zcr_mean, ok_zcr?"✓":"✗");

    int pass = ok_rms && ok_zcr;
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

/* ============================================================
   TEST 2: feat_fitness — identity=1.0, noise<same
   ============================================================ */
static int test_feat_fitness(void){
    printf("[feat_fitness] identity=1.0, noise ranks lower\n");
    static float sine[FEAT_TOTAL_LEN];
    float ph=0.f,dp=2.f*3.14159265f*440.f/(float)SR;
    for(int i=0;i<FEAT_TOTAL_LEN;i++){sine[i]=sinf(ph);ph+=dp;}

    FitnessCtx ctx;
    fitness_ctx_init(&ctx, sine, FEAT_TOTAL_LEN, 60, (float)SR);

    /* Same signal (normalised copy) */
    FeatureVec fv_same; feat_extract(ctx.target_audio, FEAT_TOTAL_LEN, &fv_same);
    float f_same = feat_fitness(&ctx, &fv_same);

    /* White noise */
    static float noise[FEAT_TOTAL_LEN];
    uint32_t rng=0xABCD1234;
    for(int i=0;i<FEAT_TOTAL_LEN;i++){
        my_rng(&rng); noise[i]=(float)(int32_t)rng*(1.f/2147483648.f);
    }
    FeatureVec fv_noise; feat_extract(noise, FEAT_TOTAL_LEN, &fv_noise);
    float f_noise = feat_fitness(&ctx, &fv_noise);

    printf("  f(same)=%.4f (want>0.95) %s\n", f_same, f_same>0.95f?"✓":"✗");
    printf("  f(noise)=%.4f < f(same) %s\n", f_noise, f_noise<f_same?"✓":"✗");
    int pass = (f_same>0.95f) && (f_noise<f_same);
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

/* ============================================================
   TEST 3: patch_random — valid and renderable
   ============================================================ */
static int test_patch_random(void){
    printf("[patch_random] 20 random patches valid and renderable\n");
    uint32_t rng=0x12345678;
    int ok=1; float peak_sum=0.f;
    for(int i=0;i<20;i++){
        my_rng(&rng);
        int n = SEARCH_MIN_INSTRS + (int)(rng%(SEARCH_MAX_INSTRS-SEARCH_MIN_INSTRS+1));
        PatchProgram p = patch_random(&rng, n);

        if(p.n_instrs < SEARCH_MIN_INSTRS){
            printf("  FAIL: n_instrs=%d\n",p.n_instrs); ok=0; continue;}
        if(INSTR_OP(p.code[p.n_instrs-1])!=OP_OUT){
            printf("  FAIL: last op not OP_OUT\n"); ok=0; continue;}

        static float buf[FEAT_TOTAL_LEN];
        render_patch(&p, buf, 60);
        float pk=0.f; int nans=0;
        for(int s=0;s<FEAT_TOTAL_LEN;s++){
            if(!isfinite(buf[s])) nans++;
            float a=fabsf(buf[s]); if(a>pk) pk=a;
        }
        if(nans){printf("  FAIL: NaN in patch %d\n",i); ok=0;}
        peak_sum+=pk;
    }
    printf("  all valid ✓  mean_peak=%.3f\n", peak_sum/20.f);
    printf("  %s\n\n", ok?"PASS":"FAIL"); return ok;
}

/* ============================================================
   TEST 4: patch_mutate — produces different, valid programs
   ============================================================ */
static int test_patch_mutate(void){
    printf("[patch_mutate] 50 mutations: valid and differ from source\n");
    uint32_t rng=0xDEAD0001;
    int diffs=0, valid=0;
    for(int i=0;i<50;i++){
        PatchProgram src = patch_random(&rng, 6);
        PatchProgram mut = patch_mutate(&src, &rng);

        if(mut.n_instrs<1 || INSTR_OP(mut.code[mut.n_instrs-1])!=OP_OUT) continue;
        static float buf[FEAT_TOTAL_LEN];
        render_patch(&mut, buf, 60);
        int nan=0;
        for(int s=0;s<FEAT_TOTAL_LEN;s++) if(!isfinite(buf[s])){nan=1;break;}
        if(nan) continue;
        valid++;

        int same=(src.n_instrs==mut.n_instrs);
        if(same) for(int k=0;k<src.n_instrs;k++)
            if(src.code[k]!=mut.code[k]){same=0;break;}
        if(!same) diffs++;
    }
    printf("  valid=%d diffs=%d (of 50)\n", valid, diffs);
    int pass=(valid>=40 && diffs>=30);
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

/* ============================================================
   TEST 5: fitness_score ranking
   ============================================================ */
static int test_fitness_ranking(void){
    printf("[fitness_ranking] sine target: sine patch > noise patch\n");
    PatchProgram sine_prog;
    { PatchBuilder b; pb_init(&b);
      int e=pb_adsr(&b,0,4,28,6);
      int o=pb_osc(&b,REG_ONE);
      pb_out(&b,pb_mul(&b,o,e));
      sine_prog=*pb_finish(&b); }

    static float tgt[FEAT_TOTAL_LEN];
    render_patch(&sine_prog, tgt, 60);
    FitnessCtx ctx;
    fitness_ctx_init(&ctx, tgt, FEAT_TOTAL_LEN, 60, (float)SR);

    float f_sine  = fitness_score(&ctx, &sine_prog);

    PatchProgram noise_prog;
    { PatchBuilder b; pb_init(&b);
      int n=pb_noise(&b);
      int hf=pb_hpf(&b,n,50);
      int e=pb_exp_decay(&b,24);
      pb_out(&b,pb_mul(&b,hf,e));
      noise_prog=*pb_finish(&b); }
    float f_noise = fitness_score(&ctx, &noise_prog);

    printf("  sine vs target:  %.4f\n", f_sine);
    printf("  noise vs target: %.4f\n", f_noise);
    int pass=(f_sine>f_noise && f_sine>0.5f);
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

/* ============================================================
   TEST 6: search on sine target — must beat random baseline
   ============================================================ */
static int progress_cb(int gen, float best, void *ud){
    if(gen%40==0) printf("    gen %3d: %.4f\n", gen, best);
    (void)ud; return 0;
}

static int test_search_sine(void){
    printf("[search_sine] beam search on sine target\n");
    PatchProgram tgt_prog;
    { PatchBuilder b; pb_init(&b);
      int e=pb_adsr(&b,0,8,24,10);
      int o=pb_osc(&b,REG_ONE);
      pb_out(&b,pb_mul(&b,o,e));
      tgt_prog=*pb_finish(&b); }

    static float tgt[FEAT_TOTAL_LEN];
    render_patch(&tgt_prog, tgt, 60);
    FitnessCtx ctx;
    fitness_ctx_init(&ctx, tgt, FEAT_TOTAL_LEN, 60, (float)SR);

    uint32_t rng=0xCAFEBABE; float baseline=0.f;
    for(int i=0;i<8;i++){
        PatchProgram p=patch_random(&rng,6);
        baseline+=fitness_score(&ctx,&p);
    }
    baseline/=8.f;
    printf("  random baseline: %.4f\n", baseline);

    SearchResult result;
    patch_search(&ctx, 0x1337C0DE, &result, progress_cb, NULL);
    printf("  best: %.4f  gen=%d  evals=%d  t=%.2fs\n",
           result.best.fitness, result.n_generations,
           result.n_evaluations, result.time_seconds);

    static float found[FEAT_TOTAL_LEN];
    render_patch(&result.best.prog, found, 60);
    normalise(found, FEAT_TOTAL_LEN);
    normalise(tgt, FEAT_TOTAL_LEN);
    write_wav("/mnt/user-data/outputs/l5_sine_target.wav", tgt, FEAT_TOTAL_LEN);
    write_wav("/mnt/user-data/outputs/l5_sine_found.wav", found, FEAT_TOTAL_LEN);

    int pass=(result.best.fitness>baseline && result.best.fitness>0.5f);
    printf("  beat baseline: %s (%.4f>%.4f)\n",
           pass?"✓":"✗", result.best.fitness, baseline);
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

/* ============================================================
   TEST 7: search on saw target
   ============================================================ */
static int test_search_saw(void){
    printf("[search_saw] beam search on saw+lpf target\n");
    PatchProgram tgt_prog;
    { PatchBuilder b; pb_init(&b);
      int e=pb_adsr(&b,0,6,20,12);
      int s=pb_saw(&b,REG_ONE);
      int lf=pb_lpf(&b,s,30);
      pb_out(&b,pb_mul(&b,lf,e));
      tgt_prog=*pb_finish(&b); }

    static float tgt[FEAT_TOTAL_LEN];
    render_patch(&tgt_prog, tgt, 60);
    FitnessCtx ctx;
    fitness_ctx_init(&ctx, tgt, FEAT_TOTAL_LEN, 60, (float)SR);

    uint32_t rng=0xFACE0001; float baseline=0.f;
    for(int i=0;i<8;i++){
        PatchProgram p=patch_random(&rng,6);
        baseline+=fitness_score(&ctx,&p);
    }
    baseline/=8.f;

    SearchResult result;
    patch_search(&ctx, 0xABCDEF01, &result, NULL, NULL);

    static float found[FEAT_TOTAL_LEN];
    render_patch(&result.best.prog, found, 60);
    normalise(found, FEAT_TOTAL_LEN);
    normalise(tgt, FEAT_TOTAL_LEN);
    write_wav("/mnt/user-data/outputs/l5_saw_target.wav", tgt, FEAT_TOTAL_LEN);
    write_wav("/mnt/user-data/outputs/l5_saw_found.wav", found, FEAT_TOTAL_LEN);

    printf("  baseline=%.4f  found=%.4f  evals=%d\n",
           baseline, result.best.fitness, result.n_evaluations);
    int pass=(result.best.fitness>baseline);
    printf("  %s\n\n", pass?"PASS":"FAIL"); return pass;
}

int main(void){
    tables_init();
    printf("=== SHMC Layer 5 — Patch Search Test ===\n\n");
    int pass=0,total=0;
    #define RUN(fn) do{total++;if(fn())pass++;}while(0)
    RUN(test_feat_extract);
    RUN(test_feat_fitness);
    RUN(test_patch_random);
    RUN(test_patch_mutate);
    RUN(test_fitness_ranking);
    RUN(test_search_sine);
    RUN(test_search_saw);
    printf("=== %d / %d passed ===\n", pass, total);
    return (pass==total)?0:1;
}
