/* verify_mutate.c — proves all 7 mutation operators produce audible change
 * Run: gcc verify_mutate.c [src files] -lm -o v && ./v
 * Expected: 24/24 PASSED */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../lemonade/include/shmc_dsl.h"
#include "../lemonade/include/shmc_mutate.h"
#include "../layer0/include/patch.h"
static int T=0,P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  ✓ " m "\n",##__VA_ARGS__);}else printf("  ✗ FAIL: " m "\n",##__VA_ARGS__);}while(0)
static const char *DSL2=
    "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 28; out $3 }\n"
    "MOTIF a { note 60 4 12; note 64 4 10 }\n"
    "MOTIF b { note 67 4 11; note 69 4 9  }\n"
    "SECTION s 8.0 { use a @ 0 x2 patch p; use b @ 4 patch p t=3 }\n"
    "SONG x 120.0 { play s }\n";
static float rms_diff(const float*a,const float*b,int n){
    float d=0; for(int i=0;i<n;i++){float x=a[i]-b[i];d+=x*x;} return sqrtf(d/n);}
static float rms(const float*b,int n){
    float d=0; for(int i=0;i<n;i++) d+=b[i]*b[i]; return sqrtf(d/n);}
static void test_mut(const char*name, MutateType mt, float min_diff){
    printf("\n  [%s]\n",name);
    ShmcWorld w1,w2; char e[64]="";
    shmc_dsl_compile(DSL2,&w1,e,64); shmc_dsl_compile(DSL2,&w2,e,64);
    float *b1=NULL,*b2=NULL; int n1=0,n2=0;
    shmc_world_render(&w1,&b1,&n1,44100.f);
    uint32_t rng=0xCAFEBABE; int applied=0;
    for(int i=0;i<200&&!applied;i++) applied=shmc_mutate(&w2,mt,&rng);
    shmc_world_render(&w2,&b2,&n2,44100.f);
    int n=n1<n2?n1:n2;
    float rd=rms_diff(b1,b2,n), r1=rms(b1,n1);
    printf("    applied=%d rms=%.4f diff=%.5f min=%.5f\n",applied,r1,rd,min_diff);
    CHECK(applied>0,"mutation applied");
    CHECK(r1>0.01f,"baseline audible (rms=%.4f)",r1);
    CHECK(rd>=min_diff,"audio changed (diff=%.5f >= %.5f)",rd,min_diff);
    free(b1); free(b2); shmc_world_free(&w1); shmc_world_free(&w2);
}
int main(void){
    tables_init();
    printf("\n=== verify_mutate (7 mutation operators) ===\n");
    test_mut("NOTE_PITCH",  MUTATE_NOTE_PITCH,  0.001f);
    test_mut("NOTE_VEL",    MUTATE_NOTE_VEL,    0.0001f);
    test_mut("NOTE_DUR",    MUTATE_NOTE_DUR,    0.001f);
    test_mut("TRANSPOSE",   MUTATE_TRANSPOSE,   0.001f);
    test_mut("VEL_SCALE",   MUTATE_VEL_SCALE,   0.0001f);
    test_mut("BEAT_OFFSET", MUTATE_BEAT_OFFSET, 0.001f);
    test_mut("PATCH_PARAM", MUTATE_PATCH,       0.0001f);
    /* MUTATE_ANY bulk */
    printf("\n  [MUTATE_ANY bulk: 100]\n");
    int napp=0,nok=0; uint32_t rng=0x12345678;
    for(int i=0;i<100;i++){
        ShmcWorld w; char e[32]=""; shmc_dsl_compile(DSL2,&w,e,32);
        if(shmc_mutate(&w,MUTATE_ANY,&rng)) napp++;
        float *buf=NULL; int nf=0;
        if(shmc_world_render(&w,&buf,&nf,44100.f)==0&&nf>0) nok++;
        free(buf); shmc_world_free(&w);
    }
    printf("    applied=%d/100 renderable=%d/100\n",napp,nok);
    CHECK(napp>=85,">=85%% applied (%d/100)",napp);
    CHECK(nok==100,"all 100 render cleanly");
    /* Empty world safety */
    printf("\n  [Empty world safety]\n"); int safe=1;
    for(int t=0;t<7;t++){ShmcWorld w; memset(&w,0,sizeof(w));uint32_t r=t;if(shmc_mutate(&w,(MutateType)t,&r)!=0)safe=0;}
    CHECK(safe,"all types return 0 on empty world");
    printf("\n  RESULT: %d/%d PASSED\n",P,T);
    return P==T?0:1;
}
