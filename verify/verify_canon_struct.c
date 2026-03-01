/*
 * verify_canon_struct.c — Stages 6+7 verification v2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../lemonade/include/shmc_dsl.h"
#include "../lemonade/include/shmc_canon.h"
#include "../lemonade/include/shmc_patch_mutate.h"
#include "../lemonade/include/shmc_dsl_limits.h"
#include "../layer0b/include/shmc_hash.h"
#include "../layer0/include/patch.h"

static int T=0,P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  ✓ " m "\n",##__VA_ARGS__);}else{printf("  ✗ FAIL: " m "\n",##__VA_ARGS__);}}while(0)

static const char *DSL_C =
    "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 24; out $3 }\n"
    "MOTIF m { note 60 4 12; note 64 4 10; note 67 4 11 }\n"
    "SECTION s 4.0 { use m @ 0 x2 patch p }\n"
    "SONG x 120.0 { play s }\n";
static const char *DSL_G =
    "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 24; out $3 }\n"
    "MOTIF m { note 67 4 12; note 71 4 10; note 74 4 11 }\n"
    "SECTION s 4.0 { use m @ 0 x2 patch p }\n"
    "SONG x 120.0 { play s }\n";
static const char *DSL_P =
    "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 24; out $3 }\n"
    "MOTIF m { note 60 4 12; note 64 4 10 }\n"
    "SECTION s 4.0 { use m @ 0 x2 patch p }\n"
    "SONG x 120.0 { play s }\n";

static float rms_f(const float *b,int n){double s=0;for(int i=0;i<n;i++)s+=b[i]*(double)b[i];return (float)sqrt(s/n);}
static float rms_diff(const float *a,const float *b,int n){double s=0;for(int i=0;i<n;i++){float d=a[i]-b[i];s+=d*(double)d;}return (float)sqrt(s/n);}

int main(void){
    tables_init();
    printf("\n=== verify_canon_struct v2 ===\n\n");
    printf("-- World hash --\n");
    ShmcWorld wC,wG,wC2; char err[128]="";
    shmc_dsl_compile(DSL_C,&wC,err,128);
    shmc_dsl_compile(DSL_G,&wG,err,128);
    shmc_dsl_compile(DSL_C,&wC2,err,128);
    uint64_t hC=shmc_world_hash(&wC), hG=shmc_world_hash(&wG), hC2=shmc_world_hash(&wC2);
    printf("  C=%016llx G=%016llx C2=%016llx\n",(unsigned long long)hC,(unsigned long long)hG,(unsigned long long)hC2);
    CHECK(hC!=hG,"T1: C and G hash different (motif content included)");
    CHECK(hC==hC2,"T2: identical worlds hash equal");

    /* T3: idempotent */
    ShmcWorld wt; shmc_dsl_compile(DSL_C,&wt,err,128);
    shmc_world_canonicalize(&wt); uint64_t h1=shmc_world_hash(&wt);
    shmc_world_canonicalize(&wt); uint64_t h2=shmc_world_hash(&wt);
    CHECK(h1==h2,"T3: canonicalize idempotent");

    /* T4: audio-safe */
    ShmcWorld wo,wc; shmc_dsl_compile(DSL_C,&wo,err,128); shmc_dsl_compile(DSL_C,&wc,err,128);
    shmc_world_canonicalize(&wc);
    float *a1=NULL,*a2=NULL; int n1=0,n2=0;
    shmc_world_render(&wo,&a1,&n1,44100.f); shmc_world_render(&wc,&a2,&n2,44100.f);
    float rd=rms_diff(a1,a2,n1<n2?n1:n2);
    printf("  audio rms_diff=%.8f\n",rd);
    CHECK(rd<1e-5f,"T4: canonicalize is audio-safe (rms_diff=%.8f)",rd);
    free(a1);free(a2);

    /* T5: hash changes after pitch mutation */
    ShmcWorld wm; shmc_dsl_compile(DSL_C,&wm,err,128);
    uint64_t hb=shmc_world_hash(&wm);
    if(wm.lib&&wm.lib->n>0&&wm.lib->entries[0].valid){
        VoiceProgram *vp=&wm.lib->entries[0].vp;
        for(int i=0;i<vp->n;i++) if(VI_OP(vp->code[i])==VI_NOTE){
            uint8_t p=VI_PITCH(vp->code[i])+1,d=VI_DUR(vp->code[i]),v=VI_VEL(vp->code[i]);
            vp->code[i]=VI_PACK(VI_NOTE,p,d,v); break;
        }
    }
    uint64_t ha=shmc_world_hash(&wm);
    CHECK(hb!=ha,"T5: hash changes after pitch mutation");
    shmc_world_free(&wC);shmc_world_free(&wG);shmc_world_free(&wC2);
    shmc_world_free(&wt);shmc_world_free(&wo);shmc_world_free(&wc);shmc_world_free(&wm);

    printf("\n-- Structural patch mutation --\n");
    ShmcWorld wb; shmc_dsl_compile(DSL_P,&wb,err,128);
    float *ab=NULL; int nb=0; shmc_world_render(&wb,&ab,&nb,44100.f);
    printf("  baseline: rms=%.4f n_instrs=%d\n",rms_f(ab,nb),wb.patches[0].n_instrs);

    /* T6: SUBSTITUTE */
    {ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128);
     uint64_t hb2=hash_patch_prog(&w.patches[0]); uint32_t rng=0xABCD1234; int ok=0;
     for(int t=0;t<16&&!ok;t++) ok=shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_SUBSTITUTE,&rng);
     uint64_t ha2=hash_patch_prog(&w.patches[0]);
     printf("  SUBSTITUTE: applied=%d hash_changed=%d\n",ok,hb2!=ha2);
     CHECK(ok,"T6: SUBSTITUTE applied"); CHECK(hb2!=ha2,"T6: changes patch hash"); shmc_world_free(&w);}

    /* T7: INSERT_DIST */
    {ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128); int nb2=w.patches[0].n_instrs;
     uint32_t rng=0xDEADBEEF; int ok=shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_INSERT_DIST,&rng);
     float *a=NULL;int na=0;shmc_world_render(&w,&a,&na,44100.f);
     printf("  INSERT_DIST: applied=%d n %d->%d rms=%.4f\n",ok,nb2,w.patches[0].n_instrs,rms_f(a,na));
     CHECK(ok&&w.patches[0].n_instrs>nb2,"T7: INSERT_DIST increases n_instrs");
     CHECK(rms_f(a,na)>0.01f,"T7: audible"); free(a);shmc_world_free(&w);}

    /* T8: INSERT_FILT */
    {ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128); int nb2=w.patches[0].n_instrs;
     uint32_t rng=0xCAFEBABE; int ok=shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_INSERT_FILT,&rng);
     float *a=NULL;int na=0;shmc_world_render(&w,&a,&na,44100.f);
     printf("  INSERT_FILT: applied=%d n %d->%d rms=%.4f\n",ok,nb2,w.patches[0].n_instrs,rms_f(a,na));
     CHECK(ok&&w.patches[0].n_instrs>nb2,"T8: INSERT_FILT increases n_instrs");
     CHECK(rms_f(a,na)>0.01f,"T8: audible"); free(a);shmc_world_free(&w);}

    /* T9: REMOVE */
    {ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128); int nb2=w.patches[0].n_instrs;
     uint32_t rng=0x11223344; int ok=0;
     for(int t=0;t<8&&!ok;t++) ok=shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_REMOVE,&rng);
     float *a=NULL;int na=0; int rok=shmc_world_render(&w,&a,&na,44100.f)==0&&na>0;
     printf("  REMOVE: applied=%d n %d->%d renderable=%d\n",ok,nb2,w.patches[0].n_instrs,rok);
     CHECK(ok&&w.patches[0].n_instrs<nb2,"T9: REMOVE decreases n_instrs");
     CHECK(rok,"T9: still renders"); free(a);shmc_world_free(&w);}

    /* T10: ANY bulk */
    {int na2=0,nok=0;
     for(int i=0;i<100;i++){ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128);
      uint32_t rng=0x5A5A0000+i;
      if(shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_ANY,&rng)) na2++;
      float *a=NULL;int n=0;
      if(shmc_world_render(&w,&a,&n,44100.f)==0&&n>0&&rms_f(a,n)>0.001f) nok++;
      free(a);shmc_world_free(&w);}
     printf("  ANY bulk: applied=%d/100 renderable=%d/100\n",na2,nok);
     CHECK(na2>=85,"T10: >=85%% applied"); CHECK(nok>=95,"T10: >=95%% renderable");}

    /* T11: bounds */
    {int ok=1;
     for(int i=0;i<200;i++){ShmcWorld w;shmc_dsl_compile(DSL_P,&w,err,128);
      uint32_t rng=0x9A9B0000+i;
      shmc_patch_struct_mutate(&w.patches[0],PATCH_STRUCT_ANY,&rng);
      int ni=w.patches[0].n_instrs; if(ni<3||ni>DSL_LIMIT_MAX_PATCH_OPS) ok=0;
      shmc_world_free(&w);}
     CHECK(ok,"T11: n_instrs in [3,%d] for 200 trials",DSL_LIMIT_MAX_PATCH_OPS);}

    free(ab);shmc_world_free(&wb);
    printf("\n══════════════════════════════════════════\n");
    printf("  RESULT: %d/%d PASSED\n",P,T);
    printf("══════════════════════════════════════════\n");
    return P==T?0:1;
}
