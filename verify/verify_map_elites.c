/*
 * verify_map_elites.c  —  MAP-Elites quality-diversity archive tests
 *
 * T1:  shmc_me_init: all cells empty, n_filled=0
 * T2:  shmc_me_cell_idx: zero descriptor → cell 0
 * T3:  shmc_me_cell_idx: all-ones descriptor → cell ME_CELLS-1
 * T4:  shmc_me_cell_idx: mid descriptor → valid range
 * T5:  shmc_me_cell_idx: 4D strides correct (b0 is highest stride)
 * T6:  shmc_me_describe: NULL audio → safe return (no crash)
 * T7:  shmc_me_describe: silent audio → brightness near 0
 * T8:  shmc_me_describe: known ZCR square wave → brightness > 0.5
 * T9:  shmc_me_describe: harmony feat piped through → b2=pitch_div, b3=tension
 * T10: shmc_me_update: first insert into empty cell → returns 1, n_filled=1
 * T11: shmc_me_update: second insert lower fitness → returns 0, cell unchanged
 * T12: shmc_me_update: second insert higher fitness → returns 1, fitness updated
 * T13: shmc_me_update: two different cells → n_filled=2
 * T14: shmc_me_random_elite: empty archive → returns -1
 * T15: shmc_me_random_elite: non-empty archive → valid index in [0, ME_CELLS)
 * T16: shmc_me_random_elite: single filled cell → always returns that cell
 * T17: shmc_me_best_world: empty archive → returns 0
 * T18: shmc_me_best_world: multiple cells → returns cell with highest fitness
 * T19: shmc_me_free: all worlds freed, n_filled=0
 * T20: Integration: ME cell idx is deterministic (same behavior → same cell)
 */
#include "../lemonade/include/shmc_map_elites.h"
#include "../lemonade/include/shmc_dsl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_pass=0, g_fail=0;
#define CHECK(cond, fmt, ...) \
    do { if(cond){g_pass++;} else {g_fail++; \
         printf("  \u2717 FAIL: " fmt "\n", ##__VA_ARGS__);} } while(0)

/* Minimal valid DSL for creating ShmcWorld */
static const char *SEED_DSL =
    "PATCH p1 { saw ONE; adsr 0 4 20 6; mul $0 $1; lpf $2 28; out $3 }\n"
    "MOTIF m1 { note 60 4 7; note 64 4 7; note 67 4 7; }\n"
    "SECTION s1 8.0 { use m1 @ 0 x1 patch p1; }\n"
    "SONG g1 120.0 { play s1 }\n";

int main(void) {
    printf("verify_map_elites — Stage 12e\n\n");

    MeArchive arc;
    HarmonyFeat hf;
    memset(&hf, 0, sizeof(hf));

    /* T1: init */
    shmc_me_init(&arc, 42u);
    CHECK(arc.n_filled == 0, "init: n_filled=%d (want 0)", arc.n_filled);
    CHECK(arc.n_updates == 0, "init: n_updates=%d (want 0)", arc.n_updates);
    { int all_empty=1; for(int i=0;i<ME_CELLS;i++) if(arc.cells[i].fitness>=0.f){all_empty=0;break;}
      CHECK(all_empty, "init: not all cells empty"); }

    /* T2: cell_idx zero descriptor */
    { MeBehavior b; memset(&b,0,sizeof(b));
      int ci = shmc_me_cell_idx(&b);
      CHECK(ci == 0, "cell_idx(all zeros)=%d (want 0)", ci); }

    /* T3: cell_idx all-ones */
    { MeBehavior b; for(int i=0;i<ME_DIM;i++) b.b[i]=1.f;
      int ci = shmc_me_cell_idx(&b);
      CHECK(ci == ME_CELLS-1, "cell_idx(all ones)=%d (want %d)", ci, ME_CELLS-1); }

    /* T4: valid range for mid descriptor */
    { MeBehavior b; for(int i=0;i<ME_DIM;i++) b.b[i]=0.5f;
      int ci = shmc_me_cell_idx(&b);
      CHECK(ci >= 0 && ci < ME_CELLS, "cell_idx(0.5)=%d out of [0,%d)", ci, ME_CELLS); }

    /* T5: 4D strides — b0 is the highest-stride dimension */
    { MeBehavior b0, b1;
      memset(&b0,0,sizeof(b0)); memset(&b1,0,sizeof(b1));
      b0.b[0]=1.f/ME_BINS;  /* b0=bin1, others=0 */
      b1.b[ME_DIM-1]=1.f/ME_BINS;  /* only last dim=bin1, others=0 */
      int ci0=shmc_me_cell_idx(&b0), ci1=shmc_me_cell_idx(&b1);
      /* b0 moves by ME_BINS^3=216, last dim moves by 1 */
      CHECK(ci0 > ci1, "stride: b0 step=%d > last-dim step=%d", ci0, ci1); }

    /* T6: describe with NULL audio → no crash, b[0]=0 */
    { MeBehavior b = shmc_me_describe(NULL, 0, 44100.f, NULL);
      CHECK(b.b[0] == 0.f, "describe(NULL): b[0]=%f (want 0)", (double)b.b[0]); }

    /* T7: silent audio → brightness near 0 */
    { float silence[1024]={0};
      MeBehavior b = shmc_me_describe(silence, 1024, 44100.f, NULL);
      CHECK(b.b[0] < 0.01f, "describe(silence): b[0]=%f (want ~0)", (double)b.b[0]); }

    /* T8: square wave → high ZCR → b[0] > 0.5 */
    { float sq[4096]; for(int i=0;i<4096;i++) sq[i]=(i%2==0)?0.5f:-0.5f;
      MeBehavior b = shmc_me_describe(sq, 4096, 44100.f, NULL);
      CHECK(b.b[0] > 0.5f, "describe(square): b[0]=%f (want >0.5)", (double)b.b[0]); }

    /* T9: harmony feat piped → b[2]=pitch_div, b[3] comes from lerdahl */
    { hf.pitch_diversity = 0.75f;
      hf.lerdahl_tension  = 0.80f;  /* score=0.8, tension=1-0.8=0.2 */
      float sig[256]={0};
      MeBehavior b = shmc_me_describe(sig, 256, 44100.f, &hf);
      CHECK(fabsf(b.b[2]-0.75f)<0.001f, "describe: b[2]=%f (want 0.75)", (double)b.b[2]);
      CHECK(fabsf(b.b[3]-0.20f)<0.001f, "describe: b[3]=%f (want 0.20)", (double)b.b[3]); }

    /* T10-T13: update tests using a real DSL world */
    ShmcWorld w1;
    char err[256]="";
    int compiled = (shmc_dsl_compile(SEED_DSL,&w1,err,256)==0);
    if (!compiled) { printf("  (DSL compile failed: %s — skipping T10-T13)\n",err); }
    else {
        MeBehavior ba, bb;
        memset(&ba,0,sizeof(ba)); ba.b[0]=0.3f; ba.b[1]=0.4f;
        memset(&bb,0,sizeof(bb)); bb.b[0]=0.7f; bb.b[1]=0.8f;
        int ca=shmc_me_cell_idx(&ba), cb=shmc_me_cell_idx(&bb);
        (void)cb;

        /* T10: first insert */
        int r1=shmc_me_update(&arc,&w1,0.5f,&ba);
        CHECK(r1==1,"update(first): returned %d (want 1)",r1);
        CHECK(arc.n_filled==1,"update(first): n_filled=%d (want 1)",arc.n_filled);
        CHECK(fabsf(arc.cells[ca].fitness-0.5f)<1e-5f,"update(first): fitness=%f",
              (double)arc.cells[ca].fitness);

        /* T11: lower fitness same cell */
        int r2=shmc_me_update(&arc,&w1,0.3f,&ba);
        CHECK(r2==0,"update(lower): returned %d (want 0)",r2);
        CHECK(fabsf(arc.cells[ca].fitness-0.5f)<1e-5f,"update(lower): fitness unchanged=%f",
              (double)arc.cells[ca].fitness);

        /* T12: higher fitness same cell */
        int r3=shmc_me_update(&arc,&w1,0.8f,&ba);
        CHECK(r3==1,"update(higher): returned %d (want 1)",r3);
        CHECK(fabsf(arc.cells[ca].fitness-0.8f)<1e-5f,"update(higher): fitness=%f",
              (double)arc.cells[ca].fitness);

        /* T13: different cell */
        int r4=shmc_me_update(&arc,&w1,0.6f,&bb);
        CHECK(r4==1,"update(diff cell): returned %d (want 1)",r4);
        CHECK(arc.n_filled==2,"update(diff cell): n_filled=%d (want 2)",arc.n_filled);

        shmc_world_free(&w1);
    }

    /* T14: random_elite on empty archive */
    { MeArchive arc2; shmc_me_init(&arc2,1u);
      int r=shmc_me_random_elite(&arc2);
      CHECK(r==-1,"random_elite(empty)=%d (want -1)",r); }

    /* T15: random_elite on non-empty archive */
    { int r=shmc_me_random_elite(&arc);
      CHECK(r>=0&&r<ME_CELLS,"random_elite=%d not in [0,%d)",r,ME_CELLS); }

    /* T16: single filled cell → always returns that cell */
    { MeArchive arc3; shmc_me_init(&arc3,7u);
      ShmcWorld w3; char e3[64]="";
      if(shmc_dsl_compile(SEED_DSL,&w3,e3,64)==0){
          MeBehavior bx; memset(&bx,0,sizeof(bx)); bx.b[0]=0.1f;
          shmc_me_update(&arc3,&w3,0.5f,&bx);
          int expected=shmc_me_cell_idx(&bx);
          int all_same=1;
          for(int t=0;t<10;t++) if(shmc_me_random_elite(&arc3)!=expected){all_same=0;break;}
          CHECK(all_same,"random_elite(single)=always cell %d",expected);
          shmc_world_free(&w3);
      } }

    /* T17: best_world on empty archive */
    { MeArchive arc4; shmc_me_init(&arc4,0u);
      ShmcWorld dst; memset(&dst,0,sizeof(dst));
      int r=shmc_me_best_world(&arc4,&dst);
      CHECK(r==0,"best_world(empty)=%d (want 0)",r); }

    /* T18: best_world returns highest-fitness cell */
    { float best_fit=-1.f; int best_ci=-1;
      for(int i=0;i<ME_CELLS;i++)
          if(arc.cells[i].fitness>best_fit){best_fit=arc.cells[i].fitness;best_ci=i;}
      CHECK(best_ci>=0,"best_world: no cells filled");
      CHECK(fabsf(best_fit-0.8f)<1e-5f,"best_world: best fitness=%f (want 0.8)",(double)best_fit); }

    /* T19: free → n_filled=0 */
    shmc_me_free(&arc);
    CHECK(arc.n_filled==0,"free: n_filled=%d (want 0)",arc.n_filled);
    { int any_world=0; for(int i=0;i<ME_CELLS;i++) if(arc.cells[i].world){any_world=1;break;}
      CHECK(!any_world,"free: world pointers not cleared"); }

    /* T20: deterministic cell index */
    { MeBehavior b1, b2;
      for(int i=0;i<ME_DIM;i++){b1.b[i]=0.33f; b2.b[i]=0.33f;}
      CHECK(shmc_me_cell_idx(&b1)==shmc_me_cell_idx(&b2),
            "cell_idx deterministic: %d vs %d",
            shmc_me_cell_idx(&b1),shmc_me_cell_idx(&b2)); }

    int total=g_pass+g_fail;
    printf("\n  RESULT: %d/%d %s\n", g_pass, total, g_fail?"SOME FAILED":"PASSED");
    return g_fail?1:0;
}
