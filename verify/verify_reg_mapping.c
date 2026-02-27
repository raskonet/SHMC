/* verify_reg_mapping.c — proves $N register fix (Stage 4, v6)
 * Tests: C4 and G4 produce different audio with ZCR ratio matching expected freq ratio.
 * Run: gcc verify_reg_mapping.c [src files] -lm -o v && ./v
 * Expected: 5/5 PASSED */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../lemonade/include/shmc_dsl.h"
#include "../layer0/include/patch.h"
static int T=0,P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  ✓ " m "\n",##__VA_ARGS__);}else printf("  ✗ FAIL: " m "\n",##__VA_ARGS__);}while(0)
static const char *DSL(int midi_note, char *buf, int sz){
    snprintf(buf,sz,
        "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 28; out $3 }\n"
        "MOTIF m { note %d 4 12 }\n"
        "SECTION s 2.0 { use m @ 0 patch p }\n"
        "SONG x 120.0 { play s }\n", midi_note);
    return buf;
}
static float zcr_window(const float *b, int start, int n){
    int zc=0; for(int i=start+1;i<start+n&&i<44100*4;i++) if((b[i-1]>=0)!=(b[i]>=0))zc++;
    return (float)zc;
}
int main(void){
    tables_init();
    printf("\n=== verify_reg_mapping ===\n");
    char d1[256],d2[256];
    ShmcWorld wC,wG; char err[128]="";
    shmc_dsl_compile(DSL(60,d1,256),&wC,err,128);
    shmc_dsl_compile(DSL(67,d2,256),&wG,err,128);
    float *bC=NULL,*bG=NULL; int nC=0,nG=0;
    shmc_world_render(&wC,&bC,&nC,44100.f);
    shmc_world_render(&wG,&bG,&nG,44100.f);
    int win=8000, start=4000;
    float zC=zcr_window(bC,start,win), zG=zcr_window(bG,start,win);
    double rms=0; for(int i=0;i<nC;i++) rms+=bC[i]*(double)bC[i]; rms=sqrt(rms/nC);
    printf("  C4: rms=%.4f zcr=%.0f\n  G4: zcr=%.0f\n  ratio=%.3f (expect ~1.498)\n",(float)rms,zC,zG,zG/zC);
    CHECK(rms>0.01f,"C4 audible (rms=%.4f)",(float)rms);
    CHECK(zC>5,"C4 oscillates (zcr=%.0f)",zC);
    CHECK(zG>5,"G4 oscillates (zcr=%.0f)",zG);
    float diff=0; int n=nC<nG?nC:nG; for(int i=0;i<n;i++){float d=bC[i]-bG[i];diff+=d*d;}
    CHECK(sqrtf(diff/n)>0.1f,"C4 and G4 differ (rms_diff=%.4f)",sqrtf(diff/n));
    CHECK(fabsf(zG/zC-1.498f)<0.05f,"ZCR ratio close to 1.498 (got %.3f)",zG/zC);
    free(bC); free(bG); shmc_world_free(&wC); shmc_world_free(&wG);
    printf("\n  RESULT: %d/%d PASSED\n",P,T);
    return P==T?0:1;
}
