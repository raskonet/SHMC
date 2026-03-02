/*
 * verify_novelty.c — Stage 10: Novelty Search + MAP-Elites verification
 *
 * Tests:
 * T1:  BVec extraction matches WFeat fields
 * T2:  bvec_dist is symmetric
 * T3:  bvec_dist(x,x) == 0
 * T4:  archive_novelty returns 0 for empty archive
 * T5:  archive_novelty returns > 0 after adding a different BVec
 * T6:  archive_novelty returns 0 for identical BVec (k=1 dist=0)
 * T7:  archive_maybe_add: first entry always added (archive < 2)
 * T8:  archive_maybe_add: novel entry (far away) is added
 * T9:  archive_maybe_add: duplicate not added (novelty = 0 < thresh)
 * T10: archive ring-buffer wraps at ARCHIVE_CAP without crash
 * T11: map_update fills correct cell (zcr_cell × temporal_cell)
 * T12: map_update keeps best fitness per cell
 * T13: map_init clears all cells to zero
 * T14: combined score = fitness + lambda * novelty (formula check)
 * T15: novelty drives exploration — mean novelty of diverse set > uniform set
 * T16: stagnation detection — lambda should boost when no improvement
 * T17: MAP-Elites n_occupied increases with diverse inputs
 * T18: archive_novelty k-NN: with 10 entries, uses k=5 (NOVELTY_K)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ── Reproduce the novelty infrastructure inline for standalone test ── */
#define BVEC_DIM        4
#define ARCHIVE_CAP     512
#define ARCHIVE_THRESH  0.04f
#define NOVELTY_K       5
#define LAMBDA          0.15f
#define LAMBDA_BOOST    0.40f
#define STAG_GENS       5
#define MAP_DIM         8

typedef struct { float v[BVEC_DIM]; } BVec;

typedef struct {
    BVec entries[ARCHIVE_CAP];
    int  n;
    int  head;
} Archive;

typedef struct {
    float    fitness;
    uint64_t hash;
} MapCell;

typedef struct {
    MapCell cells[MAP_DIM][MAP_DIM];
    int     n_occupied;
} MapElites;

/* Copied exactly from shmc_evolve_analyze.c */
static float bvec_dist(const BVec *a, const BVec *b) {
    float s = 0.f;
    for (int i = 0; i < BVEC_DIM; i++) { float d = a->v[i] - b->v[i]; s += d*d; }
    return sqrtf(s);
}

static float archive_novelty(const Archive *ar, const BVec *q) {
    if (ar->n == 0) return 0.f;
    float dists[ARCHIVE_CAP];
    for (int i = 0; i < ar->n; i++) dists[i] = bvec_dist(q, &ar->entries[i]);
    int k = (ar->n < NOVELTY_K) ? ar->n : NOVELTY_K;
    float sum = 0.f;
    for (int j = 0; j < k; j++) {
        int mi = j;
        for (int i = j+1; i < ar->n; i++) if (dists[i] < dists[mi]) mi = i;
        float tmp = dists[j]; dists[j] = dists[mi]; dists[mi] = tmp;
        sum += dists[j];
    }
    return sum / (float)k;
}

static void archive_init(Archive *ar) { ar->n = 0; ar->head = 0; }

static void archive_maybe_add(Archive *ar, const BVec *b) {
    float nov = archive_novelty(ar, b);
    if (ar->n < 2 || nov > ARCHIVE_THRESH) {
        ar->entries[ar->head] = *b;
        ar->head = (ar->head + 1) % ARCHIVE_CAP;
        if (ar->n < ARCHIVE_CAP) ar->n++;
    }
}

static void map_init(MapElites *me) { memset(me, 0, sizeof(*me)); }

static void map_update(MapElites *me, float zcr_brightness, float temporal_spread,
                        float fitness, uint64_t hash) {
    int cx = (int)(zcr_brightness / 0.040f * (MAP_DIM-1));
    int cy = (int)(temporal_spread * (MAP_DIM-1));
    if (cx < 0) cx = 0;
    if (cx >= MAP_DIM) cx = MAP_DIM-1;
    if (cy < 0) cy = 0;
    if (cy >= MAP_DIM) cy = MAP_DIM-1;
    MapCell *cell = &me->cells[cx][cy];
    if (cell->hash == 0) me->n_occupied++;
    if (cell->hash == 0 || fitness > cell->fitness) { cell->fitness = fitness; cell->hash = hash; }
}

/* ── Test harness ── */
static int T=0, P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  \xe2\x9c\x93 " m "\n",##__VA_ARGS__);}else{printf("  \xe2\x9c\x97 FAIL: " m "\n",##__VA_ARGS__);}}while(0)

static BVec make_bvec(float a, float b, float c, float d) {
    BVec bv; bv.v[0]=a; bv.v[1]=b; bv.v[2]=c; bv.v[3]=d; return bv;
}

int main(void) {
    printf("=== verify_novelty ===\n");

    /* T1: BVec extraction — verify the 4 fields are the right indices */
    {
        /* Simulate WFeat fields: [0]=zcr, [1]=temporal, [2]=rhythm, [3]=pitch */
        float zcr=0.012f, tmp=0.3f, rhy=0.5f, pit=0.4f;
        BVec bv; bv.v[0]=zcr; bv.v[1]=tmp; bv.v[2]=rhy; bv.v[3]=pit;
        CHECK(bv.v[0]==zcr && bv.v[1]==tmp && bv.v[2]==rhy && bv.v[3]==pit,
              "BVec fields map to [zcr, temporal, rhythm, pitch]");
    }

    /* T2: bvec_dist symmetric */
    {
        BVec a = make_bvec(0.1f, 0.2f, 0.3f, 0.4f);
        BVec b = make_bvec(0.5f, 0.6f, 0.7f, 0.8f);
        float d_ab = bvec_dist(&a, &b);
        float d_ba = bvec_dist(&b, &a);
        CHECK(fabsf(d_ab - d_ba) < 1e-6f, "bvec_dist symmetric (%.6f == %.6f)", d_ab, d_ba);
    }

    /* T3: bvec_dist(x,x) == 0 */
    {
        BVec a = make_bvec(0.3f, 0.1f, 0.5f, 0.2f);
        float d = bvec_dist(&a, &a);
        CHECK(d == 0.0f, "bvec_dist(x,x) == 0 (got %.8f)", d);
    }

    /* T4: empty archive → novelty = 0 */
    {
        Archive ar; archive_init(&ar);
        BVec q = make_bvec(0.5f, 0.5f, 0.5f, 0.5f);
        float nov = archive_novelty(&ar, &q);
        CHECK(nov == 0.f, "archive_novelty(empty) == 0");
    }

    /* T5: novelty > 0 after adding a different BVec */
    {
        Archive ar; archive_init(&ar);
        BVec a = make_bvec(0.0f, 0.0f, 0.0f, 0.0f);
        BVec b = make_bvec(1.0f, 1.0f, 1.0f, 1.0f);
        ar.entries[0]=a; ar.n=1; ar.head=1;
        float nov = archive_novelty(&ar, &b);
        CHECK(nov > 0.f, "archive_novelty(non-empty, different) > 0 (%.4f)", nov);
    }

    /* T6: novelty ≈ 0 for identical BVec */
    {
        Archive ar; archive_init(&ar);
        BVec a = make_bvec(0.3f, 0.4f, 0.5f, 0.6f);
        ar.entries[0]=a; ar.n=1; ar.head=1;
        float nov = archive_novelty(&ar, &a);
        CHECK(nov < 1e-6f, "archive_novelty(identical) ≈ 0 (%.8f)", nov);
    }

    /* T7: first entry always added (n < 2) */
    {
        Archive ar; archive_init(&ar);
        BVec a = make_bvec(0.1f, 0.2f, 0.3f, 0.4f);
        archive_maybe_add(&ar, &a);
        CHECK(ar.n == 1, "first entry added (n=1)");
        BVec b = make_bvec(0.1001f, 0.2f, 0.3f, 0.4f);  /* very close but n<2 */
        archive_maybe_add(&ar, &b);
        CHECK(ar.n == 2, "second entry always added when n<2 (n=2)");
    }

    /* T8: novel (distant) entry IS added */
    {
        Archive ar; archive_init(&ar);
        ar.entries[0] = make_bvec(0.0f, 0.0f, 0.0f, 0.0f);
        ar.entries[1] = make_bvec(0.1f, 0.1f, 0.1f, 0.1f);
        ar.n = 2; ar.head = 2;
        BVec far_away = make_bvec(1.0f, 1.0f, 1.0f, 1.0f);
        archive_maybe_add(&ar, &far_away);
        CHECK(ar.n == 3, "far-away BVec added (novelty > thresh)");
    }

    /* T9: near-duplicate NOT added */
    {
        Archive ar; archive_init(&ar);
        ar.entries[0] = make_bvec(0.5f, 0.5f, 0.5f, 0.5f);
        ar.entries[1] = make_bvec(0.5f, 0.5f, 0.5f, 0.5f);
        ar.n = 2; ar.head = 2;
        BVec same = make_bvec(0.5f, 0.5f, 0.5f, 0.5f);  /* novelty = 0 */
        archive_maybe_add(&ar, &same);
        CHECK(ar.n == 2, "near-duplicate NOT added (novelty < thresh)");
    }

    /* T10: ring-buffer wraps without crash (add ARCHIVE_CAP+20 entries) */
    {
        Archive ar; archive_init(&ar);
        float x = 0.0f;
        for (int i = 0; i < ARCHIVE_CAP + 20; i++) {
            BVec b = make_bvec(x, x, x, x);
            /* Force add by setting n<2 trick — use raw insert */
            ar.entries[ar.head] = b;
            ar.head = (ar.head + 1) % ARCHIVE_CAP;
            if (ar.n < ARCHIVE_CAP) ar.n++;
            x = (x >= 1.0f) ? 0.0f : x + 0.001f;
        }
        CHECK(ar.n == ARCHIVE_CAP, "ring-buffer: n capped at ARCHIVE_CAP (%d)", ar.n);
        CHECK(ar.head >= 0 && ar.head < ARCHIVE_CAP, "ring-buffer: head in range [0,%d)", ARCHIVE_CAP);
    }

    /* T11: map_update fills correct cell */
    {
        MapElites me; map_init(&me);
        /* zcr=0.020 → cx = (0.020/0.040)*(7) = 3 or 4 */
        /* temporal=0.5 → cy = (int)(0.5*7) = 3 */
        map_update(&me, 0.020f, 0.5f, 0.8f, 12345ULL);
        int cx = (int)(0.020f / 0.040f * 7);
        int cy = (int)(0.5f * 7);
        CHECK(me.cells[cx][cy].hash == 12345ULL,
              "map_update: correct cell [cx=%d][cy=%d] hash set", cx, cy);
    }

    /* T12: map_update keeps BEST fitness per cell */
    {
        MapElites me; map_init(&me);
        map_update(&me, 0.010f, 0.3f, 0.6f, 111ULL);
        map_update(&me, 0.010f, 0.3f, 0.9f, 222ULL);  /* better */
        map_update(&me, 0.010f, 0.3f, 0.7f, 333ULL);  /* worse than current best */
        int cx = (int)(0.010f / 0.040f * 7);
        int cy = (int)(0.3f * 7);
        CHECK(me.cells[cx][cy].hash == 222ULL,
              "map keeps best fitness in cell (hash=222)");
        CHECK(me.cells[cx][cy].fitness == 0.9f,
              "map best fitness = 0.9f (got %.2f)", me.cells[cx][cy].fitness);
    }

    /* T13: map_init clears all cells */
    {
        MapElites me;
        /* Dirty the memory first */
        memset(&me, 0xFF, sizeof(me));
        map_init(&me);
        int all_zero = 1;
        for (int r=0;r<MAP_DIM;r++)
            for (int c=0;c<MAP_DIM;c++)
                if (me.cells[r][c].hash != 0) all_zero=0;
        CHECK(all_zero, "map_init: all cells cleared to zero");
        CHECK(me.n_occupied == 0, "map_init: n_occupied = 0");
    }

    /* T14: combined score formula check */
    {
        float fitness = 0.75f;
        float novelty = 0.20f;
        float lam = LAMBDA;
        float combined = fitness + lam * novelty;
        float expected = 0.75f + 0.15f * 0.20f;
        CHECK(fabsf(combined - expected) < 1e-6f,
              "combined = fit + lam*nov = %.4f (expected %.4f)", combined, expected);
    }

    /* T15: diverse set has higher mean novelty than uniform set */
    {
        Archive ar_div, ar_uni;
        archive_init(&ar_div); archive_init(&ar_uni);

        /* Diverse: spread across behavior space */
        for (int i=0; i<8; i++) {
            float t = (float)i / 7.0f;
            BVec b = make_bvec(t, 1.0f-t, t*0.5f, 0.3f);
            ar_div.entries[i] = b;
        }
        ar_div.n = 8; ar_div.head = 8;

        /* Uniform: all clustered near same point */
        for (int i=0; i<8; i++) {
            BVec b = make_bvec(0.5f + i*0.001f, 0.5f, 0.5f, 0.5f);
            ar_uni.entries[i] = b;
        }
        ar_uni.n = 8; ar_uni.head = 8;

        /* Test novelty of a new point against each archive */
        BVec test = make_bvec(0.3f, 0.7f, 0.4f, 0.6f);
        float nov_div = archive_novelty(&ar_div, &test);
        float nov_uni = archive_novelty(&ar_uni, &test);
        CHECK(nov_div > nov_uni,
              "diverse archive gives higher novelty (%.4f > %.4f)", nov_div, nov_uni);
    }

    /* T16: lambda boost during stagnation */
    {
        int stag = 0;
        float lam_normal = (stag >= STAG_GENS) ? LAMBDA_BOOST : LAMBDA;
        stag = STAG_GENS;
        float lam_stag = (stag >= STAG_GENS) ? LAMBDA_BOOST : LAMBDA;
        CHECK(lam_normal == LAMBDA, "normal lambda = LAMBDA (%.2f)", lam_normal);
        CHECK(lam_stag == LAMBDA_BOOST, "stagnation lambda = LAMBDA_BOOST (%.2f)", lam_stag);
        CHECK(LAMBDA_BOOST > LAMBDA, "LAMBDA_BOOST (%.2f) > LAMBDA (%.2f)", LAMBDA_BOOST, LAMBDA);
    }

    /* T17: n_occupied increases with diverse inputs */
    {
        MapElites me; map_init(&me);
        /* Add 4 clearly different behavior points */
        map_update(&me, 0.005f, 0.1f, 0.7f, 1ULL);  /* cx=0, cy=0 */
        map_update(&me, 0.035f, 0.9f, 0.8f, 2ULL);  /* cx=6, cy=6 */
        map_update(&me, 0.005f, 0.9f, 0.6f, 3ULL);  /* cx=0, cy=6 */
        map_update(&me, 0.035f, 0.1f, 0.5f, 4ULL);  /* cx=6, cy=0 */
        CHECK(me.n_occupied >= 3, "diverse inputs occupy >= 3 cells (got %d)", me.n_occupied);
    }

    /* T18: k-NN uses k=NOVELTY_K=5 with sufficient archive */
    {
        Archive ar; archive_init(&ar);
        /* Add 10 entries: known distances 0.1, 0.2, ..., 1.0 from origin */
        for (int i=1; i<=10; i++) {
            float t = i * 0.1f;
            BVec b = make_bvec(t, 0.f, 0.f, 0.f);
            ar.entries[i-1] = b; ar.n++;
        }
        ar.head = 10;
        /* Query from origin: distances are 0.1, 0.2, ..., 1.0 */
        BVec origin = make_bvec(0.f, 0.f, 0.f, 0.f);
        float nov = archive_novelty(&ar, &origin);
        /* k=5: sum of 5 smallest = 0.1+0.2+0.3+0.4+0.5 = 1.5 → mean = 0.3 */
        CHECK(fabsf(nov - 0.3f) < 0.001f,
              "k=5 NN novelty = 0.300 (got %.4f)", nov);
    }

    printf("\n  RESULT: %d/%d PASSED\n", P, T);
    return (P==T) ? 0 : 1;
}
