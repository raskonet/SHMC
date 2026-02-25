/*
 * SHMC — TIGHT REGRESSION TESTS
 *
 * Philosophy: every test here was designed to FAIL on a specific bug
 * that existed before this fix. If all tests pass, the system is
 * demonstrably correct for the invariants they cover.
 *
 * Test domains:
 *   [A] hash_event_stream: insertion-order independence (BUG-1)
 *   [B] pb_finish auto-canonicalize (BUG-2)
 *   [C] patch_canonicalize semantics (idempotent, non-commutative preserved)
 *   [D] time_scale exact sample arithmetic
 *   [E] vel_scale exact values + clamping
 *   [F] hash_event_stream: stored stream NOT mutated
 *   [G] hash_event_stream: tiebreak NOTE_OFF-before-NOTE_ON at same sample
 *   [H] voice_compile_tempo exact sample positions (120 BPM, constant)
 *   [I] voice_compile_tempo vs tempo ramp: monotone earlier arrival
 *   [J] section_validate budget enforcement (T4-4)
 *   [K] section_validate length enforcement (T4-5)
 *   [L] repeat expansion limit enforced (T4-3)
 *   [M] TempoMap round-trip: beat→sample→beat within 1 sample
 *   [N] PatchMeta evaluative flags correctness
 *   [O] Fitness shaping: silence=0, DC penalty, instability=0
 *   [P] SectionRenderer per-instance buffers: two instances independent
 *   [Q] hash_patch_prog: same bytes → same hash; 1 bit change → different hash
 *   [R] hash_section / hash_song: structural sensitivity
 *   [S] motif time_scale with repeat: repeat boundary exactly at scaled position
 *   [T] patch_canonicalize: exactly which ops are commutative vs not
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "tempo_map.h"
#include "patch_meta.h"
#include "shmc_hash.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer0/include/opcodes.h"
#include "../../layer1/include/voice.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include "../../layer4/include/song.h"
#include "../../layer5/include/patch_search.h"

/* ---- Test infrastructure ---- */
static int g_pass = 0, g_fail = 0, g_total = 0;
static const char *g_suite = "";

#define SUITE(name) do{ g_suite = name; printf("\n[%s]\n", name); }while(0)

#define EXPECT(cond, fmt, ...) do{                                  \
    g_total++;                                                       \
    if(cond){ g_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__);\
    } else  { g_fail++; printf("  FAIL  " fmt "\n", ##__VA_ARGS__);\
    }                                                                \
}while(0)

#define NEAR(a,b,eps) (fabs((double)(a)-(double)(b)) < (double)(eps))

#define SR 44100.0
#define BPM 120.0f
/* At 120 BPM: 1 beat = 22050 samples. DUR_1_4 = 0.25 beats = 5512.5 → 5513 samples */
#define SAMPLES_PER_QUARTER 5513

/* ================================================================
   Helpers: build minimal EventStream manually
   ================================================================ */
static void es_push(EventStream *es, uint64_t smp, EvType type, uint8_t pitch, float vel){
    Event *e = &es->events[es->n++];
    e->sample   = smp;
    e->type     = type;
    e->pitch    = pitch;
    e->velocity = vel;
}

/* ================================================================
   [A] hash_event_stream: insertion-order independence (BUG-1)
   Before fix: hash depended on insertion order → FAIL.
   After fix: canonical sort → PASS.
   ================================================================ */
static void test_A_event_hash_order_independence(void){
    SUITE("A: hash_event_stream insertion-order independence");

    /* Two events at different samples, different pitches */
    EventStream es1, es2;
    memset(&es1, 0, sizeof(es1));
    memset(&es2, 0, sizeof(es2));

    /* es1: note-on(C4,t=0), note-off(C4,t=100), note-on(E4,t=200), note-off(E4,t=300) */
    es_push(&es1, 0,   EV_NOTE_ON,  60, 0.8f);
    es_push(&es1, 100, EV_NOTE_OFF, 60, 0.8f);
    es_push(&es1, 200, EV_NOTE_ON,  64, 0.6f);
    es_push(&es1, 300, EV_NOTE_OFF, 64, 0.6f);

    /* es2: same events but in reverse insertion order */
    es_push(&es2, 300, EV_NOTE_OFF, 64, 0.6f);
    es_push(&es2, 200, EV_NOTE_ON,  64, 0.6f);
    es_push(&es2, 100, EV_NOTE_OFF, 60, 0.8f);
    es_push(&es2, 0,   EV_NOTE_ON,  60, 0.8f);

    uint64_t h1 = hash_event_stream(&es1);
    uint64_t h2 = hash_event_stream(&es2);

    EXPECT(h1 == h2,
        "same events, reversed insertion order → same hash (0x%016llx)",
        (unsigned long long)h1);

    /* Verify stored order was NOT changed by hashing */
    EXPECT(es2.events[0].sample == 300,
        "hashing does not mutate stored EventStream (es2.events[0].sample=300)");
    EXPECT(es2.events[3].sample == 0,
        "hashing does not mutate stored EventStream (es2.events[3].sample=0)");

    /* Different events must still produce different hashes */
    EventStream es3;
    memset(&es3, 0, sizeof(es3));
    es_push(&es3, 0,   EV_NOTE_ON,  61, 0.8f); /* C#4 instead of C4 */
    es_push(&es3, 100, EV_NOTE_OFF, 61, 0.8f);
    es_push(&es3, 200, EV_NOTE_ON,  64, 0.6f);
    es_push(&es3, 300, EV_NOTE_OFF, 64, 0.6f);

    EXPECT(h1 != hash_event_stream(&es3),
        "different pitch → different hash");

    /* Empty stream */
    EventStream es_empty; memset(&es_empty, 0, sizeof(es_empty));
    uint64_t h_empty = hash_event_stream(&es_empty);
    EXPECT(h_empty != h1, "empty stream hashes differently from non-empty");

    /* Single event */
    EventStream es_single; memset(&es_single, 0, sizeof(es_single));
    es_push(&es_single, 100, EV_NOTE_ON, 60, 0.5f);
    uint64_t h_s1 = hash_event_stream(&es_single);
    uint64_t h_s2 = hash_event_stream(&es_single);
    EXPECT(h_s1 == h_s2, "single-event stream is deterministic");

    /* Quantized velocity: vel=0.501 and vel=0.499 → same 8-level bucket, same hash */
    EventStream es_va, es_vb;
    memset(&es_va, 0, sizeof(es_va));
    memset(&es_vb, 0, sizeof(es_vb));
    es_push(&es_va, 0, EV_NOTE_ON, 60, 0.501f);
    es_push(&es_vb, 0, EV_NOTE_ON, 60, 0.499f);
    /* 0.501 → vel8 = (int)(0.501*7.99+0.5) = (int)(4.504+0.5) = (int)5.004 = 5 */
    /* 0.499 → vel8 = (int)(0.499*7.99+0.5) = (int)(3.990+0.5) = (int)4.490 = 4 */
    /* Actually these ARE in different buckets: 0.5/7.99 ≈ 0.0626 per bucket.
       0.499 → bucket 3 (0.375), 0.501 → bucket 4 (0.5). Let's test nearby values. */
    EventStream es_vc; memset(&es_vc, 0, sizeof(es_vc));
    es_push(&es_vc, 0, EV_NOTE_ON, 60, 0.500f);  /* same bucket as 0.501 */
    EXPECT(hash_event_stream(&es_va) == hash_event_stream(&es_vc),
        "velocities in same 8-level bucket hash equally (0.501 and 0.500)");
}

/* ================================================================
   [B] pb_finish auto-canonicalize (BUG-2)
   Before fix: ADD(rA,rB) ≠ ADD(rB,rA) without manual call.
   After fix: pb_finish canonicalizes → equal hashes, no manual call needed.
   ================================================================ */
static void test_B_pb_finish_auto_canonicalize(void){
    SUITE("B: pb_finish auto-canonicalize");

    /* Build p1: osc → r2, const_1 → r3, ADD(r2, r3) */
    PatchProgram p1, p2;
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);      /* r2 */
        int c = pb_const_f(&b, 0.5f);     /* r3 */
        int s = pb_add(&b, o, c);         /* ADD(r2, r3) → r4 */
        int e = pb_adsr(&b, 0, 8, 20, 8); /* → r5 */
        pb_out(&b, pb_mul(&b, s, e));      /* MUL(r4, r5) → OUT */
        p1 = *pb_finish(&b);
    }
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.5f);
        int s = pb_add(&b, c, o);          /* ADD(r3, r2) — args SWAPPED */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        p2 = *pb_finish(&b);
    }

    /* After pb_finish (which now calls patch_canonicalize), bytes must match */
    uint64_t h1 = hash_patch_prog(&p1);
    uint64_t h2 = hash_patch_prog(&p2);
    EXPECT(h1 == h2,
        "pb_finish auto-canonicalize: ADD(osc,const) == ADD(const,osc) → hash 0x%016llx",
        (unsigned long long)h1);

    /* Program bytes must be identical (not just hash equal) */
    EXPECT(p1.n_instrs == p2.n_instrs,
        "instruction count equal (%d)", p1.n_instrs);
    int bytes_match = (memcmp(p1.code, p2.code, (size_t)p1.n_instrs * sizeof(Instr)) == 0);
    EXPECT(bytes_match, "instruction bytes identical after auto-canonicalize");

    /* MUL is also commutative — test */
    PatchProgram pm1, pm2;
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, o, e));   /* MUL(osc, adsr) */
        pm1 = *pb_finish(&b);
    }
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, e, o));   /* MUL(adsr, osc) — swapped */
        pm2 = *pb_finish(&b);
    }
    EXPECT(hash_patch_prog(&pm1) == hash_patch_prog(&pm2),
        "MUL(osc,adsr) == MUL(adsr,osc) after pb_finish");

    /* Canonicalize is idempotent: calling pb_finish twice equivalent to once */
    PatchProgram p1_copy = p1;
    extern int patch_canonicalize(PatchProgram *);
    patch_canonicalize(&p1_copy);  /* second call should change nothing */
    EXPECT(hash_patch_prog(&p1) == hash_patch_prog(&p1_copy),
        "patch_canonicalize idempotent: second call does not change hash");
}

/* ================================================================
   [C] patch_canonicalize: which ops are commutative, which are not
   ================================================================ */
static void test_C_canonicalize_semantics(void){
    SUITE("C: canonicalize — commutative vs non-commutative ops");

    extern int patch_canonicalize(PatchProgram *);

    /* SUB(a,b) ≠ SUB(b,a) — must NOT be swapped */
    PatchProgram ps1, ps2;
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.3f);
        int s = pb_sub(&b, o, c);         /* SUB(osc, const) */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        ps1 = *pb_finish(&b);
    }
    {
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.3f);
        int s = pb_sub(&b, c, o);          /* SUB(const, osc) — different semantics */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        ps2 = *pb_finish(&b);
    }
    EXPECT(hash_patch_prog(&ps1) != hash_patch_prog(&ps2),
        "SUB(a,b) != SUB(b,a): canonicalize must NOT reorder SUB");

    /* Verify SUB operands are preserved exactly */
    /* In ps1: SUB instruction should have src_a=osc_reg, src_b=const_reg */
    int sub_idx = -1;
    for(int i = 0; i < ps1.n_instrs; i++){
        if(INSTR_OP(ps1.code[i]) == OP_SUB){ sub_idx = i; break; }
    }
    EXPECT(sub_idx >= 0, "SUB instruction present in program");
    if(sub_idx >= 0){
        uint8_t a = INSTR_SRC_A(ps1.code[sub_idx]);
        uint8_t b = INSTR_SRC_B(ps1.code[sub_idx]);
        /* osc comes before const in build order → osc reg < const reg */
        EXPECT(a < b, "SUB(osc,const): osc register (%d) < const register (%d) (not swapped)", a, b);
    }

    /* ADD with same register both sides: ADD(r3, r3) → canonical, no-op swap */
    PatchProgram pa_same;
    {
        PatchBuilder b; pb_init(&b);
        int c = pb_const_f(&b, 0.5f);      /* r2 */
        int s = pb_add(&b, c, c);           /* ADD(r2, r2) */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        pa_same = *pb_finish(&b);
    }
    uint64_t h_same_before = hash_patch_prog(&pa_same);
    patch_canonicalize(&pa_same);
    EXPECT(hash_patch_prog(&pa_same) == h_same_before,
        "ADD(r,r): canonicalize of same-register operands is no-op");
}

/* ================================================================
   [D] time_scale exact sample arithmetic
   ================================================================ */
static void test_D_time_scale_exact(void){
    SUITE("D: MotifUse time_scale exact sample positions");

    static MotifLibrary lib;
    motif_lib_init(&lib);

    /* Build motif: two quarter notes. At 120 BPM, DUR_1_4 = 5513 samples each */
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 7);   /* vel=1.0 */
    vb_note(&vb, 64, DUR_1_4, 7);
    motif_define(&lib, "two_quarters", &vb.vp);

    char err[128];

    /* Baseline: time_scale=1.0 */
    {
        MotifUse mu = motif_use("two_quarters", 0.f, 1, 0);
        EventStream es; memset(&es, 0, sizeof(es)); err[0]=0;
        int r = motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));
        EXPECT(r == 0, "baseline compile ok: %s", err);
        /* on[0]=0, off[0]=5513, on[1]=5513, off[1]=11026 (after sort) */
        EXPECT(es.n == 4, "baseline: 4 events (n=%d)", es.n);
        /* Find the two note-on events by scanning */
        int on_samples[2]={-1,-1}, off_samples[2]={-1,-1}, ni=0, fi=0;
        for(int i=0;i<es.n;i++){
            if(es.events[i].type==EV_NOTE_ON && ni<2)
                on_samples[ni++]=(int)es.events[i].sample;
            else if(es.events[i].type==EV_NOTE_OFF && fi<2)
                off_samples[fi++]=(int)es.events[i].sample;
        }
        EXPECT(on_samples[0] == 0,
            "baseline on[0] = 0 (got %d)", on_samples[0]);
        EXPECT(off_samples[0] == SAMPLES_PER_QUARTER,
            "baseline off[0] = %d (got %d)", SAMPLES_PER_QUARTER, off_samples[0]);
        EXPECT(on_samples[1] == SAMPLES_PER_QUARTER,
            "baseline on[1] = %d (got %d)", SAMPLES_PER_QUARTER, on_samples[1]);
    }

    /* time_scale=2.0: all sample positions exactly doubled */
    {
        MotifUse mu = motif_use("two_quarters", 0.f, 1, 0);
        mu.time_scale = 2.0f;
        EventStream es; memset(&es, 0, sizeof(es)); err[0]=0;
        int r = motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));
        EXPECT(r == 0, "time_scale=2.0 compile ok: %s", err);
        EXPECT(es.n == 4, "time_scale=2.0: 4 events");
        int on0=-1, off0=-1;
        for(int i=0;i<es.n;i++){
            if(es.events[i].type==EV_NOTE_ON  && on0<0)  on0=(int)es.events[i].sample;
            if(es.events[i].type==EV_NOTE_OFF && off0<0) off0=(int)es.events[i].sample;
        }
        /* off[0] must be exactly round(5512.5*2) = round(11025) = 11025 or 11026 */
        int expected_off = (int)(SAMPLES_PER_QUARTER * 2);
        EXPECT(abs(off0 - expected_off) <= 1,
            "time_scale=2.0: off[0]=%d ~= %d", off0, expected_off);
    }

    /* time_scale=0.5: all positions halved */
    {
        MotifUse mu = motif_use("two_quarters", 0.f, 1, 0);
        mu.time_scale = 0.5f;
        EventStream es; memset(&es, 0, sizeof(es)); err[0]=0;
        motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));
        int off0=-1;
        for(int i=0;i<es.n;i++)
            if(es.events[i].type==EV_NOTE_OFF && off0<0) off0=(int)es.events[i].sample;
        int expected = (int)(SAMPLES_PER_QUARTER * 0.5f + 0.5f);
        EXPECT(abs(off0 - expected) <= 1,
            "time_scale=0.5: off[0]=%d ~= %d", off0, expected);
    }

    /* time_scale with repeat: 2 repeats at time_scale=2.0
     * repeat-0: [0 .. 2*5513), repeat-1: [2*5513 .. 4*5513)
     * second note-on of repeat-1 must be at 2*5513 + 5513 (scaled) */
    {
        MotifUse mu = motif_use("two_quarters", 0.f, 2, 0);
        mu.time_scale = 2.0f;
        EventStream es; memset(&es, 0, sizeof(es)); err[0]=0;
        motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));
        EXPECT(es.n == 8, "repeat=2,time_scale=2: 8 events (got %d)", es.n);
        /* Find the note-on at the start of repeat-1 */
        /* scaled motif length = 2 * 2*5513 = 22052 samples (approx) */
        int scaled_len = (int)(2 * SAMPLES_PER_QUARTER * 2 + 0.5f);
        int repeat1_on = -1;
        for(int i=0;i<es.n;i++){
            if(es.events[i].type==EV_NOTE_ON && (int)es.events[i].sample >= scaled_len-2){
                repeat1_on = (int)es.events[i].sample; break;
            }
        }
        EXPECT(abs(repeat1_on - scaled_len) <= 2,
            "repeat-1 starts at scaled_len=%d (got %d)", scaled_len, repeat1_on);
    }

    /* start_beat offset: motif at beat 1.0, time_scale=1.0
     * At 120 BPM, beat 1 = 22050 samples. First note-on must be at 22050. */
    {
        MotifUse mu = motif_use("two_quarters", 1.0f, 1, 0);
        EventStream es; memset(&es, 0, sizeof(es)); err[0]=0;
        motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));
        int on0=-1;
        for(int i=0;i<es.n;i++)
            if(es.events[i].type==EV_NOTE_ON && on0<0) on0=(int)es.events[i].sample;
        EXPECT(on0 == 22050, "start_beat=1.0: first note-on at 22050 (got %d)", on0);
    }
}

/* ================================================================
   [E] vel_scale exact values and clamping
   ================================================================ */
static void test_E_vel_scale_exact(void){
    SUITE("E: MotifUse vel_scale exact values and clamping");

    static MotifLibrary lib2;
    motif_lib_init(&lib2);

    /* VEL_TABLE[7] = 1.0, VEL_TABLE[4] = 0.625 */
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 7);   /* vel index 7 → 1.0 */
    motif_define(&lib2, "loud", &vb.vp);

    VoiceBuilder vb2; vb_init(&vb2);
    vb_note(&vb2, 60, DUR_1_4, 4);  /* vel index 4 → 0.625 */
    motif_define(&lib2, "mid", &vb2.vp);

    char err[64];

    /* vel_scale=0.5 applied to vel=1.0 → 0.5 exactly */
    {
        MotifUse mu = motif_use("loud", 0.f, 1, 0); mu.vel_scale = 0.5f;
        EventStream es; memset(&es,0,sizeof(es));
        motif_compile_uses(&lib2, &mu, 1, &es, SR, BPM, err, sizeof(err));
        float got = es.events[0].velocity;
        EXPECT(fabsf(got - 0.5f) < 0.001f, "vel_scale=0.5 on vel=1.0 → 0.500 (got %.4f)", got);
    }

    /* vel_scale=0.625 applied to vel=0.625 → 0.390625, check exact */
    {
        MotifUse mu = motif_use("mid", 0.f, 1, 0); mu.vel_scale = 0.625f;
        EventStream es; memset(&es,0,sizeof(es));
        motif_compile_uses(&lib2, &mu, 1, &es, SR, BPM, err, sizeof(err));
        float expected = 0.625f * 0.625f;  /* 0.390625 */
        float got = es.events[0].velocity;
        EXPECT(fabsf(got - expected) < 0.001f,
            "vel_scale=0.625 on vel=0.625 → %.6f (got %.6f)", expected, got);
    }

    /* vel_scale=2.0 on vel=1.0 → clamped to 1.0, not 2.0 */
    {
        MotifUse mu = motif_use("loud", 0.f, 1, 0); mu.vel_scale = 2.0f;
        EventStream es; memset(&es,0,sizeof(es));
        motif_compile_uses(&lib2, &mu, 1, &es, SR, BPM, err, sizeof(err));
        float got = es.events[0].velocity;
        EXPECT(got <= 1.0f, "vel_scale=2.0 on vel=1.0 → clamped ≤ 1.0 (got %.4f)", got);
        EXPECT(got > 0.99f, "vel_scale=2.0 on vel=1.0 → clamped to ~1.0 (got %.4f)", got);
    }

    /* vel_scale=0.0 on vel=1.0 → 0.0 */
    {
        MotifUse mu = motif_use("loud", 0.f, 1, 0); mu.vel_scale = 0.0f;
        /* vel_scale <= 0 → treated as 1.0 (identity) per our clamping rule */
        EventStream es; memset(&es,0,sizeof(es));
        motif_compile_uses(&lib2, &mu, 1, &es, SR, BPM, err, sizeof(err));
        float got = es.events[0].velocity;
        /* vel_scale=0 is treated as identity (1.0) per motif.c line 105 */
        EXPECT(fabsf(got - 1.0f) < 0.001f,
            "vel_scale=0.0 → treated as identity (got %.4f)", got);
    }

    /* NOTE_OFF events carry same velocity as NOTE_ON — both scaled */
    {
        MotifUse mu = motif_use("loud", 0.f, 1, 0); mu.vel_scale = 0.25f;
        EventStream es; memset(&es,0,sizeof(es));
        motif_compile_uses(&lib2, &mu, 1, &es, SR, BPM, err, sizeof(err));
        EXPECT(es.n == 2, "one note → 2 events");
        float v0 = es.events[0].velocity;
        float v1 = es.events[1].velocity;
        EXPECT(fabsf(v0 - 0.25f) < 0.001f, "NOTE_ON vel=0.25 (got %.4f)", v0);
        EXPECT(fabsf(v1 - 0.25f) < 0.001f, "NOTE_OFF vel=0.25 (got %.4f)", v1);
    }
}

/* ================================================================
   [F] hash_event_stream: stored stream NOT mutated by hashing
   ================================================================ */
static void test_F_hash_does_not_mutate(void){
    SUITE("F: hash_event_stream does not mutate stored EventStream");

    EventStream es; memset(&es, 0, sizeof(es));
    es_push(&es, 500, EV_NOTE_OFF, 72, 0.7f);
    es_push(&es, 100, EV_NOTE_ON,  60, 0.9f);
    es_push(&es, 300, EV_NOTE_ON,  64, 0.5f);

    /* Record stored order before hashing */
    uint64_t s0 = es.events[0].sample;
    uint64_t s1 = es.events[1].sample;
    uint64_t s2 = es.events[2].sample;
    uint8_t  p0 = es.events[0].pitch;
    EvType   t0 = es.events[0].type;

    /* Hash twice */
    uint64_t h1 = hash_event_stream(&es);
    uint64_t h2 = hash_event_stream(&es);

    EXPECT(h1 == h2, "hash is deterministic across two calls");
    EXPECT(es.events[0].sample == s0, "events[0].sample unchanged after hash (%llu)", (unsigned long long)s0);
    EXPECT(es.events[1].sample == s1, "events[1].sample unchanged after hash (%llu)", (unsigned long long)s1);
    EXPECT(es.events[2].sample == s2, "events[2].sample unchanged after hash (%llu)", (unsigned long long)s2);
    EXPECT(es.events[0].pitch  == p0, "events[0].pitch unchanged after hash (%d)", p0);
    EXPECT(es.events[0].type   == t0, "events[0].type unchanged after hash");
    EXPECT(es.n == 3, "events[0].n unchanged (n=%d)", es.n);
}

/* ================================================================
   [G] hash_event_stream: tiebreak NOTE_OFF before NOTE_ON at same sample
   Two streams with NOTE_ON and NOTE_OFF at same sample in different order
   must hash identically (canonical tiebreak resolves them the same way).
   ================================================================ */
static void test_G_hash_tiebreak(void){
    SUITE("G: hash_event_stream tiebreak at same sample");

    /* Same sample, different type order */
    EventStream es_on_first, es_off_first;
    memset(&es_on_first,  0, sizeof(es_on_first));
    memset(&es_off_first, 0, sizeof(es_off_first));

    /* es_on_first: NOTE_ON then NOTE_OFF at same sample */
    es_push(&es_on_first,  1000, EV_NOTE_ON,  60, 0.8f);
    es_push(&es_on_first,  1000, EV_NOTE_OFF, 60, 0.8f);

    /* es_off_first: NOTE_OFF then NOTE_ON at same sample */
    es_push(&es_off_first, 1000, EV_NOTE_OFF, 60, 0.8f);
    es_push(&es_off_first, 1000, EV_NOTE_ON,  60, 0.8f);

    EXPECT(hash_event_stream(&es_on_first) == hash_event_stream(&es_off_first),
        "NOTE_ON/NOTE_OFF at same sample: insertion order irrelevant to hash");

    /* Different pitches at same sample must remain distinguishable */
    EventStream es_ab, es_ba;
    memset(&es_ab, 0, sizeof(es_ab));
    memset(&es_ba, 0, sizeof(es_ba));
    es_push(&es_ab, 200, EV_NOTE_ON, 60, 0.5f);
    es_push(&es_ab, 200, EV_NOTE_ON, 64, 0.5f);
    es_push(&es_ba, 200, EV_NOTE_ON, 64, 0.5f);
    es_push(&es_ba, 200, EV_NOTE_ON, 60, 0.5f);
    EXPECT(hash_event_stream(&es_ab) == hash_event_stream(&es_ba),
        "chord: same pitches, different insertion order → same hash");

    /* But a chord with a different pitch must differ */
    EventStream es_diff; memset(&es_diff, 0, sizeof(es_diff));
    es_push(&es_diff, 200, EV_NOTE_ON, 60, 0.5f);
    es_push(&es_diff, 200, EV_NOTE_ON, 67, 0.5f);  /* G4 not E4 */
    EXPECT(hash_event_stream(&es_ab) != hash_event_stream(&es_diff),
        "chord with different pitch hashes differently");
}

/* ================================================================
   [H] voice_compile_tempo exact sample positions (constant 120 BPM)
   ================================================================ */
static void test_H_compile_tempo_exact(void){
    SUITE("H: voice_compile_tempo exact samples at 120 BPM constant");

    TempoMap tm; tempo_map_constant(&tm, 120.0, SR);

    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 7);  /* 0.25 beats */
    vb_note(&vb, 64, DUR_1_4, 7);  /* 0.25 beats */

    EventStream es; char err[64]="";
    int r = voice_compile_tempo(&vb.vp, &es, &tm, 0.0, err, sizeof(err));
    EXPECT(r == 0, "compile ok: %s", err);
    EXPECT(es.n == 4, "4 events for 2 notes (got %d)", es.n);

    /* After sort: on(60)@0, off(60)@5513, on(64)@5513, off(64)@11025 or 11026 */
    /* Find events by type+pitch */
    int on60=-1, off60=-1, on64=-1, off64=-1;
    for(int i=0;i<es.n;i++){
        if(es.events[i].pitch==60 && es.events[i].type==EV_NOTE_ON)  on60=(int)es.events[i].sample;
        if(es.events[i].pitch==60 && es.events[i].type==EV_NOTE_OFF) off60=(int)es.events[i].sample;
        if(es.events[i].pitch==64 && es.events[i].type==EV_NOTE_ON)  on64=(int)es.events[i].sample;
        if(es.events[i].pitch==64 && es.events[i].type==EV_NOTE_OFF) off64=(int)es.events[i].sample;
    }
    EXPECT(on60  == 0,    "on(60) at sample 0 (got %d)", on60);
    EXPECT(off60 == SAMPLES_PER_QUARTER,
           "off(60) at %d (got %d)", SAMPLES_PER_QUARTER, off60);
    EXPECT(on64  == SAMPLES_PER_QUARTER,
           "on(64) at %d (got %d)", SAMPLES_PER_QUARTER, on64);
    /* off(64): 2 × DUR_1_4 = 0.5 beats = 11025 samples exactly */
    int expected_off64 = (int)llround(0.5 * 22050.0);  /* 11025 */
    EXPECT(off64 == expected_off64,
           "off(64) at %d (got %d)", expected_off64, off64);

    /* total_samples must equal the last event sample */
    EXPECT((int)es.total_samples == expected_off64,
           "total_samples=%d == %d", (int)es.total_samples, expected_off64);
    /* total_beats must be 0.5 */
    EXPECT(fabsf(es.total_beats - 0.5f) < 0.01f,
           "total_beats=%.4f ~= 0.5", es.total_beats);

    /* start_beat offset: voice starting at beat 2 → all samples + 2*22050 */
    {
        EventStream es2; char err2[64]="";
        voice_compile_tempo(&vb.vp, &es2, &tm, 2.0, err2, sizeof(err2));
        int beat2_base = 2 * 22050;  /* exact for constant 120 BPM */
        int on60_at2 = -1;
        for(int i=0;i<es2.n;i++)
            if(es2.events[i].pitch==60 && es2.events[i].type==EV_NOTE_ON)
                on60_at2=(int)es2.events[i].sample;
        EXPECT(on60_at2 == beat2_base,
               "start_beat=2.0: on(60) at %d (got %d)", beat2_base, on60_at2);
    }
}

/* ================================================================
   [I] voice_compile_tempo vs tempo ramp: monotone earlier arrival
   ================================================================ */
static void test_I_compile_tempo_ramp(void){
    SUITE("I: voice_compile_tempo tempo ramp");

    /* At 120→240 BPM ramp, the quarter note at beat 0.25 arrives EARLIER
     * than at constant 120 BPM, because BPM accelerates through the note. */
    TempoPoint pts[2] = {{0.0, 120.0, TM_LINEAR_BPM}, {4.0, 240.0, TM_STEP}};
    TempoMap tm_ramp; tempo_map_build(&tm_ramp, pts, 2, SR);
    TempoMap tm_flat; tempo_map_constant(&tm_flat, 120.0, SR);

    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 7);

    EventStream es_ramp, es_flat; char err[64]="";
    voice_compile_tempo(&vb.vp, &es_ramp, &tm_ramp, 0.0, err, sizeof(err));
    voice_compile_tempo(&vb.vp, &es_flat, &tm_flat, 0.0, err, sizeof(err));

    int off_ramp=-1, off_flat=-1;
    for(int i=0;i<es_ramp.n;i++)
        if(es_ramp.events[i].type==EV_NOTE_OFF) off_ramp=(int)es_ramp.events[i].sample;
    for(int i=0;i<es_flat.n;i++)
        if(es_flat.events[i].type==EV_NOTE_OFF) off_flat=(int)es_flat.events[i].sample;

    EXPECT(off_ramp < off_flat,
        "ramp (off=%d) < flat (off=%d): note ends earlier under acceleration",
        off_ramp, off_flat);

    /* Continuity: two adjacent notes must have on[1].sample == off[0].sample */
    VoiceBuilder vb2; vb_init(&vb2);
    vb_note(&vb2, 60, DUR_1_4, 7);
    vb_note(&vb2, 64, DUR_1_4, 7);
    EventStream es2; char err2[64]="";
    voice_compile_tempo(&vb2.vp, &es2, &tm_ramp, 0.0, err2, sizeof(err2));

    int off0=-1, on1=-1;
    for(int i=0;i<es2.n;i++){
        if(es2.events[i].pitch==60 && es2.events[i].type==EV_NOTE_OFF && off0<0)
            off0=(int)es2.events[i].sample;
        if(es2.events[i].pitch==64 && es2.events[i].type==EV_NOTE_ON && on1<0)
            on1=(int)es2.events[i].sample;
    }
    EXPECT(off0 == on1,
        "sequential notes: off[0]=%d == on[1]=%d (no gap, no overlap)", off0, on1);
}

/* ================================================================
   [J] section_validate budget enforcement (T4-4)
   ================================================================ */
static void test_J_section_budget(void){
    SUITE("J: section_validate event budget T4-4");

    static MotifLibrary lib; motif_lib_init(&lib);
    /* Build a dense motif: 100 whole-beat notes */
    VoiceBuilder vb; vb_init(&vb);
    for(int i=0;i<100;i++) vb_note(&vb, 60+((i)%12), DUR_1, 5);
    motif_define(&lib, "dense", &vb.vp);

    PatchProgram pat;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); pat=*pb_finish(&b); }

    /* repeat=20 → ~4000 events; budget default = 2048 → should fail.
     * Section length=2001 beats so the 2000-beat motif fits (only budget blocks it). */
    static Section s; section_init(&s, "Dense", 2001.f);
    MotifUse mu = motif_use("dense", 0.f, 20, 0);
    mu.resolved_motif = motif_find(&lib, "dense");
    section_add_track(&s, "t", &pat, &mu, 1, 1.f, 0.f);

    char err[256]="";
    int r = section_validate(&s, &lib, SR, BPM, SLV_ERROR, 2048, err, sizeof(err));
    EXPECT(r < 0 && strstr(err, "T4-4"),
        "dense motif ×20 exceeds budget → T4-4 error (r=%d, err='%s')", r, err);

    /* budget=10000 → should pass for same section */
    char err2[256]="";
    int r2 = section_validate(&s, &lib, SR, BPM, SLV_ERROR, 10000, err2, sizeof(err2));
    EXPECT(r2 == 0, "same section with budget=10000 passes (r=%d, err='%s')", r2, err2);
}

/* ================================================================
   [K] section_validate length enforcement (T4-5)
   ================================================================ */
static void test_K_section_length(void){
    SUITE("K: section_validate length T4-5");

    static MotifLibrary lib2; motif_lib_init(&lib2);
    VoiceBuilder vb; vb_init(&vb);
    for(int i=0;i<8;i++) vb_note(&vb, 60, DUR_1, 5); /* 8 beats */
    motif_define(&lib2, "long8", &vb.vp);

    PatchProgram pat;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); pat=*pb_finish(&b); }

    /* Section is only 4 beats, motif is 8 beats → overflow */
    static Section s; section_init(&s, "Short", 4.f);
    MotifUse mu = motif_use("long8", 0.f, 1, 0);
    mu.resolved_motif = motif_find(&lib2, "long8");
    section_add_track(&s, "t", &pat, &mu, 1, 1.f, 0.f);

    char err[256]="";
    int r = section_validate(&s, &lib2, SR, BPM, SLV_ERROR, 0, err, sizeof(err));
    EXPECT(r < 0 && strstr(err, "T4-5"),
        "8-beat motif in 4-beat section → T4-5 error (r=%d, err='%s')", r, err);

    /* Motif that exactly fits (4 beats in 4-beat section) → should pass */
    VoiceBuilder vb2; vb_init(&vb2);
    for(int i=0;i<4;i++) vb_note(&vb2, 60, DUR_1, 5); /* exactly 4 beats */
    motif_define(&lib2, "exact4", &vb2.vp);

    static Section s2; section_init(&s2, "Exact", 4.f);
    MotifUse mu2 = motif_use("exact4", 0.f, 1, 0);
    mu2.resolved_motif = motif_find(&lib2, "exact4");
    section_add_track(&s2, "t", &pat, &mu2, 1, 1.f, 0.f);

    char err2[256]="";
    int r2 = section_validate(&s2, &lib2, SR, BPM, SLV_ERROR, 0, err2, sizeof(err2));
    EXPECT(r2 == 0, "4-beat motif in 4-beat section passes (r=%d)", r2);
}

/* ================================================================
   [L] repeat expansion limit enforced (T4-3)
   ================================================================ */
static void test_L_repeat_limit(void){
    SUITE("L: repeat expansion limit T4-3");

    TempoMap tm; tempo_map_constant(&tm, 120.0, SR);

    /* repeat count 65 (> VOICE_MAX_REPEAT_COUNT=64) → must reject */
    VoiceProgram vp;
    vp.n = 3;
    vp.code[0] = VI_PACK(VI_REPEAT_BEGIN, 0, 0, 0);
    vp.code[1] = VI_PACK(VI_NOTE, 60, DUR_1_8, 5);
    vp.code[2] = VI_PACK(VI_REPEAT_END, 0, 0, 65);

    EventStream es; char err[128]="";
    int r = voice_compile_tempo(&vp, &es, &tm, 0.0, err, sizeof(err));
    EXPECT(r < 0, "repeat=65 rejected (r=%d err='%s')", r, err);

    /* repeat count 64 (at limit) → must succeed with exactly 128 events */
    VoiceProgram vp2;
    vp2.n = 3;
    vp2.code[0] = VI_PACK(VI_REPEAT_BEGIN, 0, 0, 0);
    vp2.code[1] = VI_PACK(VI_NOTE, 60, DUR_1_8, 5);
    vp2.code[2] = VI_PACK(VI_REPEAT_END, 0, 0, 64);

    EventStream es2; char err2[128]="";
    int r2 = voice_compile_tempo(&vp2, &es2, &tm, 0.0, err2, sizeof(err2));
    EXPECT(r2 == 0, "repeat=64 accepted (r=%d)", r2);
    EXPECT(es2.n == 128, "repeat=64: 64*(on+off)=128 events (got %d)", es2.n);

    /* Verify events are in order: each pair should advance by DUR_1_8 samples */
    /* DUR_1_8 = 0.125 beats; at 120 BPM = 0.125*22050 = 2756.25 → 2756 or 2757 */
    int first_off = -1;
    for(int i=0;i<es2.n;i++)
        if(es2.events[i].type==EV_NOTE_OFF && first_off<0)
            first_off=(int)es2.events[i].sample;
    EXPECT(first_off >= 2756 && first_off <= 2757,
        "first NOTE_OFF at ~2756 samples (got %d)", first_off);
}

/* ================================================================
   [M] TempoMap round-trip: beat→sample→beat within 1 sample
   ================================================================ */
static void test_M_tempo_roundtrip(void){
    SUITE("M: TempoMap round-trip precision");

    TempoPoint pts[3] = {
        {0.0, 80.0,  TM_LINEAR_BPM},
        {4.0, 160.0, TM_LINEAR_SPB},
        {8.0, 120.0, TM_STEP}
    };
    TempoMap tm; tempo_map_build(&tm, pts, 3, SR);

    /* Test 200 evenly-spaced beats, verify round-trip */
    int failures = 0;
    double max_err_beats = 0.0;
    for(int i=0; i<=200; i++){
        double beat = i * 0.05;  /* 0.0 to 10.0 in 0.05 steps */
        int64_t  smp       = tempo_beat_to_sample(&tm, beat);
        double   beat_back = tempo_seconds_to_beat(&tm, (double)smp / SR);
        double   err       = fabs(beat - beat_back);
        if(err > max_err_beats) max_err_beats = err;
        if(err > 2.0/SR) failures++;  /* 2 samples = acceptable quantization error */
    }
    EXPECT(failures == 0,
        "200 beat round-trips: 0 failures with > 2-sample error (max_err=%.2e beats)",
        max_err_beats);

    /* Segment boundary: time at seg boundary is continuous (no jump) */
    double t_just_before = tempo_beat_to_seconds(&tm, 3.9999);
    double t_at_boundary = tempo_beat_to_seconds(&tm, 4.0000);
    double t_just_after  = tempo_beat_to_seconds(&tm, 4.0001);
    EXPECT(t_just_before < t_at_boundary && t_at_boundary < t_just_after,
        "tempo boundary at beat 4: monotone (%.6f < %.6f < %.6f)",
        t_just_before, t_at_boundary, t_just_after);

    /* beat_to_seconds is strictly monotone */
    double prev = -1.0;
    int mono_fail = 0;
    for(int i=0; i<=100; i++){
        double t = tempo_beat_to_seconds(&tm, i * 0.1);
        if(t <= prev) mono_fail++;
        prev = t;
    }
    EXPECT(mono_fail == 0, "beat_to_seconds is strictly monotone (%d failures)", mono_fail);
}

/* ================================================================
   [N] PatchMeta evaluative flags correctness
   ================================================================ */
static void test_N_patch_meta(void){
    SUITE("N: PatchMeta evaluative flags");

    /* Full patch: osc + lpf + adsr */
    PatchProgram p_full;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int f=pb_lpf(&b,o,20);
      int e=pb_adsr(&b,0,8,20,8); pb_out(&b,pb_mul(&b,f,e));
      p_full=*pb_finish(&b); }
    PatchMeta m; patch_meta(&p_full, &m);

    EXPECT(m.has_oscillator, "full: has_oscillator=1");
    EXPECT(m.has_envelope,   "full: has_envelope=1");
    EXPECT(m.has_filter,     "full: has_filter=1");
    EXPECT(m.is_stable,      "full: is_stable=1 (lpf, moderate params)");
    EXPECT(pmeta_search_viable(&m), "full: search_viable=1");
    EXPECT(m.n_oscillators == 1, "full: n_oscillators=1 (got %d)", m.n_oscillators);
    EXPECT(m.n_envelopes == 1,   "full: n_envelopes=1 (got %d)", m.n_envelopes);
    EXPECT(m.n_filters == 1,     "full: n_filters=1 (got %d)", m.n_filters);

    /* No envelope → not viable */
    PatchProgram p_no_env;
    { PatchBuilder b; pb_init(&b); int o=pb_saw(&b,REG_ONE); pb_out(&b,o); p_no_env=*pb_finish(&b); }
    PatchMeta m2; patch_meta(&p_no_env, &m2);
    EXPECT(m2.has_oscillator, "no_env: has_oscillator=1");
    EXPECT(!m2.has_envelope,  "no_env: has_envelope=0");
    EXPECT(!pmeta_search_viable(&m2), "no_env: not viable");

    /* No oscillator → not viable */
    PatchProgram p_no_osc;
    { PatchBuilder b; pb_init(&b);
      int c=pb_const_f(&b,0.5f); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,c,e)); p_no_osc=*pb_finish(&b); }
    PatchMeta m3; patch_meta(&p_no_osc, &m3);
    EXPECT(!m3.has_oscillator, "no_osc: has_oscillator=0");
    EXPECT(!pmeta_search_viable(&m3), "no_osc: not viable");

    /* Noise source counts as oscillator */
    PatchProgram p_noise;
    { PatchBuilder b; pb_init(&b);
      int n=pb_noise(&b); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,n,e)); p_noise=*pb_finish(&b); }
    PatchMeta m4; patch_meta(&p_noise, &m4);
    EXPECT(m4.has_oscillator, "noise: has_oscillator=1 (noise counted as osc)");
    EXPECT(m4.has_noise_source, "noise: has_noise_source=1");
    EXPECT(pmeta_search_viable(&m4), "noise+adsr: viable");

    /* est_cpu: stateful instructions cost more */
    EXPECT(m.est_cpu > 0, "full patch est_cpu > 0 (got %d)", m.est_cpu);
    PatchMeta m_no_env; patch_meta(&p_no_env, &m_no_env);
    EXPECT(m_no_env.est_cpu <= m.est_cpu,
        "no_env patch cpu (%d) <= full patch cpu (%d)", m_no_env.est_cpu, m.est_cpu);
}

/* ================================================================
   [O] Fitness shaping: silence=0, DC penalty, instability=0
   ================================================================ */
static void test_O_fitness_shaping(void){
    SUITE("O: fitness shaping penalties");

    /* Target: clean sine with envelope */
    PatchProgram target;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); target=*pb_finish(&b); }

    static float target_audio[FEAT_TOTAL_LEN];
    Patch p; patch_note_on(&p,&target,(float)SR,60,0.85f);
    patch_step(&p,target_audio,FEAT_TOTAL_LEN);

    FitnessCtx ctx; fitness_ctx_init(&ctx,target_audio,FEAT_TOTAL_LEN,60,(float)SR);

    /* Self-score: target against itself must be high */
    float self = fitness_score(&ctx, &target);
    EXPECT(self > 0.95f, "target vs self: fitness=%.4f > 0.95", self);

    /* No oscillator → viability fast-reject → score 0 */
    PatchProgram p_no_osc;
    { PatchBuilder b; pb_init(&b);
      int c=pb_const_f(&b,0.5f); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,c,e)); p_no_osc=*pb_finish(&b); }
    float f_no_osc = fitness_score(&ctx, &p_no_osc);
    EXPECT(f_no_osc < 0.1f, "no oscillator: fitness=%.4f < 0.1", f_no_osc);

    /* DC-heavy patch: constant output → DC penalty must reduce score below self */
    PatchProgram p_dc;
    { PatchBuilder b; pb_init(&b);
      int c=pb_const_f(&b,0.9f); int e=pb_adsr(&b,0,28,28,28);
      int o=pb_osc(&b,REG_ONE); /* add osc so it passes viability check */
      int mix=pb_add(&b,c,o);
      pb_out(&b,pb_mul(&b,mix,e)); p_dc=*pb_finish(&b); }
    float f_dc = fitness_score(&ctx, &p_dc);
    EXPECT(f_dc < self, "DC-heavy patch: fitness=%.4f < self=%.4f", f_dc, self);

    /* Silence check: if rendering produces near-zero output → 0 */
    /* Build a patch that produces silence by multiplying by zero */
    PatchProgram p_silent;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE);
      int z=pb_const_f(&b,0.0f); /* zero constant */
      int e=pb_adsr(&b,0,8,20,8);
      /* NOTE: pb_const_f(0.0f) hits special case returning REG_ZERO,
         MUL by REG_ZERO → silence */
      pb_out(&b,pb_mul(&b,pb_mul(&b,o,z),e)); p_silent=*pb_finish(&b); }
    float f_silent = fitness_score(&ctx, &p_silent);
    EXPECT(f_silent < 0.01f, "silent patch: fitness=%.4f < 0.01", f_silent);
}

/* ================================================================
   [P] SectionRenderer per-instance buffer independence
   ================================================================ */
static void test_P_renderer_isolation(void){
    SUITE("P: SectionRenderer per-instance buffer independence");

    static MotifLibrary lib; motif_lib_init(&lib);
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 5); vb_note(&vb, 64, DUR_1_4, 5);
    motif_define(&lib, "mel", &vb.vp);

    PatchProgram pat;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); pat=*pb_finish(&b); }

    static Section sec; section_init(&sec, "S", 4.f);
    MotifUse mu = motif_use("mel", 0.f, 1, 0);
    mu.resolved_motif = motif_find(&lib, "mel");
    section_add_track(&sec, "t", &pat, &mu, 1, 1.f, 0.f);

    static SectionRenderer sr1, sr2;
    char err[128]="";
    section_renderer_init(&sr1, &sec, &lib, SR, BPM, err, sizeof(err));
    section_renderer_init(&sr2, &sec, &lib, SR, BPM, err, sizeof(err));

    /* Verify pointers are distinct (not aliased) */
    EXPECT(sr1.scratch_mono != sr2.scratch_mono, "scratch_mono pointers distinct");
    EXPECT(sr1.scratch_l    != sr2.scratch_l,    "scratch_l pointers distinct");
    EXPECT(sr1.scratch_r    != sr2.scratch_r,    "scratch_r pointers distinct");

    /* Render same content from both — must produce identical output */
    float l1[256]={}, r1[256]={}, l2[256]={}, r2[256]={};
    section_render_block(&sr1, l1, r1, 256);
    section_render_block(&sr2, l2, r2, 256);

    int identical = 1;
    for(int i=0;i<256;i++)
        if(fabsf(l1[i]-l2[i]) > 1e-7f || fabsf(r1[i]-r2[i]) > 1e-7f){ identical=0; break; }
    EXPECT(identical, "two independent renderers produce bit-identical output");

    /* Corruption test: poison sr1's scratch in the MIDDLE of render,
     * then re-render sr2 from scratch (reset) — sr2 must produce same output as before.
     * This verifies that sr1's scratch is fully isolated from sr2's scratch. */
    {
        static SectionRenderer sr1b, sr2b;
        char err2[128]="";
        section_renderer_init(&sr1b, &sec, &lib, SR, BPM, err2, sizeof(err2));
        section_renderer_init(&sr2b, &sec, &lib, SR, BPM, err2, sizeof(err2));

        /* Poison sr1b's scratch completely with garbage before any render */
        memset(sr1b.scratch_mono, 0x7F, sizeof(sr1b.scratch_mono));
        memset(sr1b.scratch_l,    0x7F, sizeof(sr1b.scratch_l));
        memset(sr1b.scratch_r,    0x7F, sizeof(sr1b.scratch_r));

        /* Now render sr2b — it must produce same output as sr2 (same initial state) */
        float l2b[256]={}, r2b[256]={};
        section_render_block(&sr2b, l2b, r2b, 256);

        int still_match = 1;
        for(int i=0;i<256;i++)
            if(fabsf(l2[i]-l2b[i]) > 1e-7f || fabsf(r2[i]-r2b[i]) > 1e-7f){ still_match=0; break; }
        EXPECT(still_match,
            "poisoning sr1b.scratch does not affect sr2b render output (isolation confirmed)");

        section_renderer_destroy(&sr1b);
        section_renderer_destroy(&sr2b);
    }

    section_renderer_destroy(&sr1);
    section_renderer_destroy(&sr2);
}

/* ================================================================
   [Q] hash_patch_prog: sensitivity
   ================================================================ */
static void test_Q_hash_sensitivity(void){
    SUITE("Q: hash_patch_prog sensitivity");

    PatchProgram p1;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); p1=*pb_finish(&b); }

    /* Identical build → same hash */
    PatchProgram p2;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,o,e)); p2=*pb_finish(&b); }
    EXPECT(hash_patch_prog(&p1) == hash_patch_prog(&p2), "identical programs hash equally");

    /* 1-bit flip in one instruction byte → different hash */
    PatchProgram p3 = p1;
    p3.code[0] ^= 1ULL;  /* flip bit 0 of first instruction */
    EXPECT(hash_patch_prog(&p1) != hash_patch_prog(&p3), "1-bit flip → different hash");

    /* Different oscillator type → different hash */
    PatchProgram p4;
    { PatchBuilder b; pb_init(&b);
      int o=pb_saw(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8); /* SAW not OSC */
      pb_out(&b,pb_mul(&b,o,e)); p4=*pb_finish(&b); }
    EXPECT(hash_patch_prog(&p1) != hash_patch_prog(&p4), "OSC vs SAW → different hash");

    /* Different n_instrs → different hash (even if code bytes happen to overlap) */
    PatchProgram p5;
    { PatchBuilder b; pb_init(&b);
      int o=pb_osc(&b,REG_ONE); int e=pb_adsr(&b,0,8,20,8);
      int f=pb_lpf(&b,o,20);  /* extra instruction */
      pb_out(&b,pb_mul(&b,f,e)); p5=*pb_finish(&b); }
    EXPECT(hash_patch_prog(&p1) != hash_patch_prog(&p5), "different n_instrs → different hash");
}

/* ================================================================
   [R] hash_section / hash_song: structural sensitivity
   ================================================================ */
static void test_R_structural_hashes(void){
    SUITE("R: hash_section and hash_song structural sensitivity");

    static Section s1, s2, s3;
    section_init(&s1, "Verse",  8.f);
    section_init(&s2, "Verse",  8.f);   /* identical to s1 */
    section_init(&s3, "Chorus", 8.f);   /* different name */

    EXPECT(hash_section(&s1) == hash_section(&s2), "identical sections: equal hash");
    EXPECT(hash_section(&s1) != hash_section(&s3), "different name: different hash");

    /* Different length → different hash */
    static Section s4; section_init(&s4, "Verse", 16.f);
    EXPECT(hash_section(&s1) != hash_section(&s4), "different length: different hash");

    /* Song hashing */
    Song song1, song2, song3;
    song_init(&song1, "A", 120.f, SR);
    song_init(&song2, "A", 120.f, SR);
    song_init(&song3, "B", 120.f, SR);
    EXPECT(hash_song(&song1) == hash_song(&song2), "identical songs: equal hash");
    EXPECT(hash_song(&song1) != hash_song(&song3), "different name: different song hash");

    /* Adding a BPM point changes song hash */
    uint64_t h_before = hash_song(&song1);
    song_add_bpm(&song1, 4.f, 180.f);
    uint64_t h_after = hash_song(&song1);
    EXPECT(h_before != h_after, "adding BPM point changes song hash");
}

/* ================================================================
   [S] motif time_scale with repeat: boundary exact
   (Catches the bug where scaled_repeat_dur wasn't computed for repeat offset)
   ================================================================ */
static void test_S_time_scale_repeat_boundary(void){
    SUITE("S: time_scale with repeat: exact repeat boundary");

    static MotifLibrary lib; motif_lib_init(&lib);
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 5);  /* one quarter note = 5513 samples */
    motif_define(&lib, "q", &vb.vp);

    char err[64];

    /* time_scale=3.0, repeat=3.
     * Each repeat has duration = 3*5513 = 16539 samples.
     * repeat-0: note-on at 0
     * repeat-1: note-on at 16539
     * repeat-2: note-on at 33078 */
    MotifUse mu = motif_use("q", 0.f, 3, 0);
    mu.time_scale = 3.0f;
    EventStream es; memset(&es,0,sizeof(es));
    motif_compile_uses(&lib, &mu, 1, &es, SR, BPM, err, sizeof(err));

    EXPECT(es.n == 6, "3 repeats × 2 events = 6 (got %d)", es.n);

    /* Find note-on events in sorted order */
    int on_samps[3] = {-1,-1,-1}; int ni=0;
    for(int i=0;i<es.n && ni<3;i++)
        if(es.events[i].type==EV_NOTE_ON) on_samps[ni++]=(int)es.events[i].sample;

    int expected_len = (int)(5513 * 3.0f + 0.5f);  /* 16539 */
    EXPECT(on_samps[0] == 0, "repeat-0 on at 0 (got %d)", on_samps[0]);
    EXPECT(abs(on_samps[1] - expected_len) <= 1,
        "repeat-1 on at %d (got %d)", expected_len, on_samps[1]);
    EXPECT(abs(on_samps[2] - 2*expected_len) <= 2,
        "repeat-2 on at %d (got %d)", 2*expected_len, on_samps[2]);
}

/* ================================================================
   [T] patch_canonicalize: complete opcode table check
   ================================================================ */
static void test_T_canonicalize_opcode_table(void){
    SUITE("T: patch_canonicalize opcode coverage");

    extern int patch_canonicalize(PatchProgram *);

    /* For each commutative op: verify that high_reg,low_reg becomes low_reg,high_reg */
    /* We construct a raw instruction and check the swap */

    struct { Opcode op; const char *name; int commutative; } ops[] = {
        {OP_ADD, "ADD", 1},
        {OP_MUL, "MUL", 1},
        {OP_MIN, "MIN", 1},
        {OP_MAX, "MAX", 1},
        {OP_SUB, "SUB", 0},  /* not commutative */
        {OP_DIV, "DIV", 0},  /* not commutative */
    };

    for(int i=0; i<(int)(sizeof(ops)/sizeof(ops[0])); i++){
        /* Build instruction with src_a=10, src_b=5 (a > b) */
        PatchProgram prog;
        prog.n_instrs = 1;
        prog.code[0] = INSTR_PACK(ops[i].op, 11, 10, 5, 0, 0);
        prog.n_regs  = 12;

        patch_canonicalize(&prog);

        uint8_t a = INSTR_SRC_A(prog.code[0]);
        uint8_t b = INSTR_SRC_B(prog.code[0]);

        if(ops[i].commutative){
            EXPECT(a == 5 && b == 10,
                "%s commutative: (10,5) → (%d,%d) expect (5,10)", ops[i].name, a, b);
        } else {
            EXPECT(a == 10 && b == 5,
                "%s NOT commutative: (10,5) stays (%d,%d) expect (10,5)", ops[i].name, a, b);
        }
    }

    /* Already-canonical: low,high stays low,high (no change) */
    PatchProgram prog2; prog2.n_instrs=1; prog2.n_regs=12;
    prog2.code[0] = INSTR_PACK(OP_ADD, 11, 3, 7, 0, 0);  /* a=3 < b=7 → already canonical */
    patch_canonicalize(&prog2);
    EXPECT(INSTR_SRC_A(prog2.code[0])==3 && INSTR_SRC_B(prog2.code[0])==7,
        "already-canonical ADD(3,7): stays (3,7)");
}

/* ================================================================
   Main
   ================================================================ */
int main(void){
    printf("=== SHMC TIGHT REGRESSION TESTS ===\n");
    tables_init();

    test_A_event_hash_order_independence();
    test_B_pb_finish_auto_canonicalize();
    test_C_canonicalize_semantics();
    test_D_time_scale_exact();
    test_E_vel_scale_exact();
    test_F_hash_does_not_mutate();
    test_G_hash_tiebreak();
    test_H_compile_tempo_exact();
    test_I_compile_tempo_ramp();
    test_J_section_budget();
    test_K_section_length();
    test_L_repeat_limit();
    test_M_tempo_roundtrip();
    test_N_patch_meta();
    test_O_fitness_shaping();
    test_P_renderer_isolation();
    test_Q_hash_sensitivity();
    test_R_structural_hashes();
    test_S_time_scale_repeat_boundary();
    test_T_canonicalize_opcode_table();

    printf("\n=== %d / %d passed  (%d failed) ===\n",
           g_pass, g_total, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
