/*
 * verify_roundtrip.c — Round-trip verification: DSL → world → DSL → world
 *
 * The critical property for the closed generative loop:
 *   compile(emit(compile(dsl))) produces same hash as compile(dsl)
 *
 * Tests:
 *   T1: emit produces non-empty output for each seed
 *   T2: emitted DSL compiles without error
 *   T3: re-compiled world renders audio (not silent)
 *   T4: hash(world1) == hash(world2) — structural identity preserved
 *   T5: rms(audio1) ≈ rms(audio2) — audio identity preserved
 *   T6: emitted DSL is human-readable (contains PATCH/MOTIF/SECTION/SONG)
 *   T7: after search mutation, mutated world is serializable + recompilable
 *
 * Expected: 7+/7+ PASSED (one set per seed DSL)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../lemonade/include/shmc_dsl.h"
#include "../lemonade/include/shmc_dsl_emit.h"
#include "../lemonade/include/shmc_mutate.h"
#include "../layer0b/include/shmc_hash.h"
#include "../layer0/include/patch.h"

static int T=0, P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  ✓ " m "\n",##__VA_ARGS__);}else{printf("  ✗ FAIL: " m "\n",##__VA_ARGS__);}}while(0)

static float buf_rms(const float *b, int n){
    double s=0; for(int i=0;i<n;i++) s+=b[i]*(double)b[i]; return sqrtf((float)(s/n));
}
static uint64_t world_hash(const ShmcWorld *w){
    uint64_t h=14695981039346656037ULL;
    for(int i=0;i<w->n_sections;i++){uint64_t sh=hash_section(&w->sections[i]);h^=sh;h*=1099511628211ULL;}
    for(int i=0;i<w->n_patches;i++){uint64_t ph=hash_patch_prog(&w->patches[i]);h^=ph;h*=1099511628211ULL;}
    return h;
}
#define DSL_BUF_SZ 32768

static void test_roundtrip(const char *label, const char *dsl) {
    printf("\n  [%s]\n", label);

    /* Compile original */
    ShmcWorld w1; char err[256]="";
    if(shmc_dsl_compile(dsl,&w1,err,256)<0){printf("  SKIP (compile failed: %s)\n",err);T+=7;return;}

    /* Emit */
    char *emitted = calloc(DSL_BUF_SZ,1);
    int n = shmc_world_to_dsl(&w1, emitted, DSL_BUF_SZ);
    printf("    emit: %d bytes\n", n);
    CHECK(n > 50, "emit non-empty (%d bytes)", n);

    /* Check structure */
    int has_patch  = strstr(emitted,"PATCH")!=NULL;
    int has_motif  = strstr(emitted,"MOTIF")!=NULL;
    int has_section= strstr(emitted,"SECTION")!=NULL;
    int has_song   = strstr(emitted,"SONG")!=NULL;
    CHECK(has_patch&&has_motif&&has_section&&has_song,
          "emitted DSL has all 4 blocks (PATCH=%d MOTIF=%d SECTION=%d SONG=%d)",
          has_patch,has_motif,has_section,has_song);

    /* Re-compile */
    ShmcWorld w2; err[0]=0;
    int rc2 = shmc_dsl_compile(emitted,&w2,err,256);
    if(rc2<0) printf("    emitted DSL:\n%s\n",emitted);
    CHECK(rc2==0, "emitted DSL recompiles (err='%s')", err);

    if(rc2==0){
        /* Render both and compare */
        float *a1=NULL,*a2=NULL; int n1=0,n2=0;
        shmc_world_render(&w1,&a1,&n1,44100.f);
        shmc_world_render(&w2,&a2,&n2,44100.f);
        float r1=buf_rms(a1,n1), r2=buf_rms(a2,n2);
        printf("    rms1=%.4f rms2=%.4f\n",r1,r2);
        CHECK(r2>0.005f,"round-trip audio audible (rms=%.4f)",r2);
        float rdiff=fabsf(r1-r2)/(r1>1e-6f?r1:1e-6f);
        CHECK(rdiff<0.05f,"audio RMS within 5%% (diff=%.1f%%)",rdiff*100.f);

        /* Hash comparison */
        uint64_t h1=world_hash(&w1), h2=world_hash(&w2);
        printf("    h1=%016llx h2=%016llx\n",(unsigned long long)h1,(unsigned long long)h2);
        CHECK(h1==h2,"structural hashes equal");

        free(a1); free(a2);
        shmc_world_free(&w2);
    } else {
        T+=3; /* mark 3 tests as fail */
    }

    shmc_world_free(&w1);
    free(emitted);
}

int main(void){
    tables_init();
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  verify_roundtrip (Stage 6 serializer) ║\n");
    printf("╚══════════════════════════════════════╝\n");

    /* Test 1: Simple bass */
    test_roundtrip("simple_bass",
        "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 28; out $3 }\n"
        "MOTIF m { note 60 4 12; note 64 4 10 }\n"
        "SECTION s 4.0 { use m @ 0 x2 patch p }\n"
        "SONG x 120.0 { play s }\n");

    /* Test 2: Two patches + transpose */
    test_roundtrip("two_patches",
        "PATCH lead { saw ONE; adsr 0 2 20 8; mul $0 $1; lpf $2 30; out $3 }\n"
        "PATCH pad  { tri ONE; adsr 4 6 28 10; mul $0 $1; hpf $2 5; out $3 }\n"
        "MOTIF mel { note 60 4 12; note 64 4 10; note 67 4 11 }\n"
        "MOTIF bass { note 48 4 14; note 52 4 12 }\n"
        "SECTION s 8.0 { use mel @ 0 x2 patch lead; use bass @ 0 x4 patch pad t=3 v=0.800 }\n"
        "SONG x 110.0 { play s x2 }\n");

    /* Test 3: FM patch */
    test_roundtrip("fm_patch",
        "PATCH fm { saw ONE; saw ONE; adsr 8 4 28 12; fm $0 $1 3; mul $3 $2; lpf $4 20; out $5 }\n"
        "MOTIF m { note 72 4 8; note 76 4 7; note 79 4 9 }\n"
        "SECTION s 4.0 { use m @ 0 patch fm }\n"
        "SONG x 90.0 { play s }\n");

    /* Test 4: After mutation, still serializable */
    printf("\n  [after_mutation]\n");
    {
        const char *dsl =
            "PATCH p { saw ONE; adsr 0 4 20 6; mul $0 $1; out $2 }\n"
            "MOTIF m { note 60 4 12; note 64 4 10 }\n"
            "SECTION s 4.0 { use m @ 0 x2 patch p }\n"
            "SONG x 120.0 { play s }\n";
        ShmcWorld w; char err[256]="";
        shmc_dsl_compile(dsl,&w,err,256);
        uint32_t rng=0xDEADBEEF;
        shmc_mutate(&w,MUTATE_ANY,&rng);
        char *emitted=calloc(DSL_BUF_SZ,1);
        int n=shmc_world_to_dsl(&w,emitted,DSL_BUF_SZ);
        CHECK(n>50,"mutated world serializes (%d bytes)",n);
        ShmcWorld w2; err[0]=0;
        int rc=shmc_dsl_compile(emitted,&w2,err,256);
        CHECK(rc==0,"mutated→DSL→world compiles (err='%s')",err);
        if(rc==0){
            float *a=NULL; int nf=0;
            shmc_world_render(&w2,&a,&nf,44100.f);
            CHECK(nf>0&&buf_rms(a,nf)>0.001f,"mutated round-trip renders audio");
            free(a); shmc_world_free(&w2);
        } else T++;
        free(emitted); shmc_world_free(&w);
    }

    printf("\n══════════════════════════════════════════\n");
    printf("  RESULT: %d/%d PASSED\n", P, T);
    printf("══════════════════════════════════════════\n");
    return P==T ? 0 : 1;
}
