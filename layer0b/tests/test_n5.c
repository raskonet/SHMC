/*
 * SHMC — N5-* Integration Tests
 *
 * Tests:
 *   N5-1: patch_canonicalize() — commutative op normalization
 *   N5-2: MotifUse time_scale + vel_scale transforms
 *   N5-3: Thread-safety baseline — concurrent rendering produces no interference
 *   N5-4: hash_event_stream() — semantic equivalence hashing
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "tempo_map.h"
#include "patch_meta.h"
#include "shmc_hash.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer1/include/voice.h"
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include "../../layer4/include/song.h"

static int passed = 0, total = 0;
#define PASS(n)   do{ printf("  [PASS] %s\n",n); passed++; total++; }while(0)
#define FAIL(n,...) do{ printf("  [FAIL] %s: ",n); printf(__VA_ARGS__); printf("\n"); total++; }while(0)
#define NEAR(a,b,e) (fabs((a)-(b)) < (e))
#define SR 44100.f

/* ================================================================
   N5-1: patch_canonicalize
   ================================================================ */
static void test_canonicalize(void){
    printf("\n[patch_canonicalize] N5-1\n");

    /* Since pb_finish now canonicalizes automatically (BUG-2 fix),
       programs built with swapped commutative operands are ALREADY canonical
       at creation time. The "before/after" distinction no longer exists at
       the PatchBuilder level. We verify the post-fix invariant directly. */
    PatchProgram p1, p2;

    {   /* p1: ADD(osc, const) — lower-reg first after canonicalization */
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.5f);
        int s = pb_add(&b, o, c);      /* ADD(o, c) */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        p1 = *pb_finish(&b);           /* pb_finish auto-canonicalizes */
    }
    {   /* p2: ADD(const, osc) — args swapped, but pb_finish canonicalizes to same */
        PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.5f);
        int s = pb_add(&b, c, o);      /* ADD(c, o) — swapped */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        p2 = *pb_finish(&b);           /* pb_finish canonicalizes to same as p1 */
    }

    /* With pb_finish auto-canonicalize, BOTH programs are canonical at creation.
       ADD(r_lo, r_hi) and ADD(r_hi, r_lo) must have identical hashes immediately. */
    uint64_t h1_before = hash_patch_prog(&p1);
    uint64_t h2_before = hash_patch_prog(&p2);
    if(h1_before == h2_before)
        PASS("pb_finish auto-canonical: ADD(osc,const) == ADD(const,osc) at creation");
    else
        FAIL("auto-canonical", "hashes differ despite pb_finish canonicalization");

    /* Canonicalize both */
    int ch1 = patch_canonicalize(&p1);
    int ch2 = patch_canonicalize(&p2);
    (void)ch1; (void)ch2;

    uint64_t h1_after = hash_patch_prog(&p1);
    uint64_t h2_after = hash_patch_prog(&p2);
    if(h1_after == h2_after)
        PASS("manual canonicalize: still idempotent (already canonical from pb_finish)");
    else
        FAIL("after", "still different: h1=%llx h2=%llx",
             (unsigned long long)h1_after, (unsigned long long)h2_after);

    /* Idempotent: second canonicalize changes nothing */
    uint64_t h1_idem = hash_patch_prog(&p1);
    patch_canonicalize(&p1);
    uint64_t h1_idem2 = hash_patch_prog(&p1);
    if(h1_idem == h1_idem2) PASS("canonicalization is idempotent");
    else FAIL("idempotent", "hash changed on second call");

    /* Non-commutative ops (SUB, DIV) must NOT be reordered */
    PatchProgram p_sub;
    {   PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int c = pb_const_f(&b, 0.5f);
        int s = pb_sub(&b, o, c);      /* SUB(o,c) — not commutative, must keep order */
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, s, e));
        p_sub = *pb_finish(&b);
    }
    uint64_t h_sub_before = hash_patch_prog(&p_sub);
    patch_canonicalize(&p_sub);
    uint64_t h_sub_after = hash_patch_prog(&p_sub);
    if(h_sub_before == h_sub_after) PASS("SUB is not reordered by canonicalization");
    else FAIL("SUB preserved", "hash changed — SUB was incorrectly swapped");

    /* MUL is commutative: MUL r5 r4 and MUL r4 r5 should normalize to same */
    PatchProgram pa, pb_prog;
    {   PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int e = pb_adsr(&b, 0, 8, 20, 8);
        int m = pb_mul(&b, o, e);      /* MUL(o, e) where o < e (lower reg first) */
        pb_out(&b, m);
        pa = *pb_finish(&b);
    }
    {   PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE);
        int e = pb_adsr(&b, 0, 8, 20, 8);
        int m = pb_mul(&b, e, o);      /* MUL(e, o) — swapped */
        pb_out(&b, m);
        pb_prog = *pb_finish(&b);
    }
    patch_canonicalize(&pa);
    patch_canonicalize(&pb_prog);
    if(hash_patch_prog(&pa) == hash_patch_prog(&pb_prog))
        PASS("MUL(a,b) and MUL(b,a) canonicalize to same hash");
    else
        FAIL("MUL commutative", "still different after canonicalization");
}

/* ================================================================
   N5-2: MotifUse time_scale + vel_scale
   ================================================================ */
static void test_motif_transforms(void){
    printf("\n[motif_transforms] N5-2\n");

    static MotifLibrary lib;
    motif_lib_init(&lib);

    /* Build a simple one-note motif: quarter note C4 at full velocity */
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 7); /* vel index 7 = 1.0 */
    motif_define(&lib, "note", &vb.vp);

    /* Baseline: no transforms */
    {
        MotifUse mu = motif_use("note", 0.f, 1, 0);
        EventStream es; char err[64]="";
        int r = motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        if(r < 0){ FAIL("baseline compile", "%s", err); return; }
        /* DUR_1_4 = 0.25 beats; at 120 BPM: 5512.5 → 5513 samples */
        if(es.n == 2 && (int64_t)es.events[1].sample == 5513)
            PASS("baseline: 1 quarter note = 5513 samples at 120 BPM");
        else
            FAIL("baseline", "n=%d end_sample=%lld", es.n, (long long)es.events[1].sample);
    }

    /* time_scale = 2.0: duration doubled */
    {
        MotifUse mu = motif_use("note", 0.f, 1, 0);
        mu.time_scale = 2.f;
        EventStream es; char err[64]="";
        motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        /* note-off should be at 2 × 5513 = 11026 samples */
        int64_t expected_off = 11026;
        int64_t got_off = (int64_t)es.events[1].sample;
        if(NEAR(got_off, expected_off, 2))
            PASS("time_scale=2.0: duration doubled");
        else
            FAIL("time_scale=2", "note-off at %lld, want ~%lld", (long long)got_off, (long long)expected_off);
    }

    /* time_scale = 0.5: duration halved */
    {
        MotifUse mu = motif_use("note", 0.f, 1, 0);
        mu.time_scale = 0.5f;
        EventStream es; char err[64]="";
        motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        /* note-off should be at 0.5 × 5513 = 2756 samples */
        int64_t expected_off = 2756;
        int64_t got_off = (int64_t)es.events[1].sample;
        if(NEAR(got_off, expected_off, 2))
            PASS("time_scale=0.5: duration halved");
        else
            FAIL("time_scale=0.5", "note-off at %lld, want ~%lld", (long long)got_off, (long long)expected_off);
    }

    /* vel_scale = 0.5: velocity halved */
    {
        MotifUse mu = motif_use("note", 0.f, 1, 0);
        mu.vel_scale = 0.5f;
        EventStream es; char err[64]="";
        motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        /* vel index 7 = 1.0, scaled by 0.5 → 0.5 */
        float got_vel = es.events[0].velocity;
        if(NEAR(got_vel, 0.5f, 0.01f))
            PASS("vel_scale=0.5: velocity halved to 0.5");
        else
            FAIL("vel_scale=0.5", "got vel=%.4f, want 0.5", got_vel);
    }

    /* vel_scale = 2.0: velocity clamped to 1.0 */
    {
        MotifUse mu = motif_use("note", 0.f, 1, 0);
        mu.vel_scale = 2.f;
        EventStream es; char err[64]="";
        motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        float got_vel = es.events[0].velocity;
        if(got_vel <= 1.0f && got_vel > 0.9f)
            PASS("vel_scale=2.0: velocity clamped to 1.0");
        else
            FAIL("vel_scale=2.0", "got vel=%.4f, want <=1.0", got_vel);
    }

    /* time_scale affects repeat spacing: 2 repeats at time_scale=2 */
    {
        MotifUse mu = motif_use("note", 0.f, 2, 0);
        mu.time_scale = 2.f;
        EventStream es; char err[64]="";
        motif_compile_uses(&lib, &mu, 1, &es, SR, 120.f, err, sizeof(err));
        /* repeat 1: on=0, off=11026; repeat 2: on=11026, off=22052 */
        if(es.n == 4 && NEAR((int64_t)es.events[2].sample, 11026, 5))
            PASS("time_scale=2.0 with repeat=2: second repeat at correct position");
        else
            FAIL("repeat+time_scale", "n=%d events[2].sample=%lld", es.n, (long long)es.events[2].sample);
    }
}

/* ================================================================
   N5-3: Thread-safety baseline — two SectionRenderers render independently
   ================================================================ */
static void test_thread_safety_baseline(void){
    printf("\n[thread_safety_baseline] N5-3\n");

    /* Without actual threading, verify the key property:
       two SectionRenderer instances render identical output independently.
       If static buffers existed, the second render would corrupt the first's scratch.
       With per-instance buffers, each has its own scratch — results must be identical. */

    PatchProgram pat;
    {   PatchBuilder b; pb_init(&b);
        int o = pb_osc(&b, REG_ONE); int e = pb_adsr(&b,0,8,20,8);
        pb_out(&b, pb_mul(&b,o,e)); pat = *pb_finish(&b);
    }

    static MotifLibrary lib;
    motif_lib_init(&lib);
    VoiceBuilder vb; vb_init(&vb);
    vb_note(&vb, 60, DUR_1_4, 5);
    vb_note(&vb, 64, DUR_1_4, 5);
    motif_define(&lib, "mel", &vb.vp);

    static Section sec;
    section_init(&sec, "S", 4.f);
    MotifUse mu = motif_use("mel", 0.f, 1, 0);
    mu.resolved_motif = motif_find(&lib, "mel");
    section_add_track(&sec, "lead", &pat, &mu, 1, 0.8f, 0.f);

    /* Two independent renderers */
    static SectionRenderer sr1, sr2;
    char err[128]="";
    if(section_renderer_init(&sr1, &sec, &lib, SR, 120.f, err, sizeof(err)) < 0){
        FAIL("sr1 init", "%s", err); return;
    }
    if(section_renderer_init(&sr2, &sec, &lib, SR, 120.f, err, sizeof(err)) < 0){
        FAIL("sr2 init", "%s", err); return;
    }

    /* Render 256 frames from each — if scratch buffers are per-instance, results match */
    float l1[256]={}, r1[256]={};
    float l2[256]={}, r2[256]={};
    section_render_block(&sr1, l1, r1, 256);
    section_render_block(&sr2, l2, r2, 256);

    int identical = 1;
    for(int i=0; i<256; i++){
        if(fabsf(l1[i]-l2[i]) > 1e-7f || fabsf(r1[i]-r2[i]) > 1e-7f){
            identical = 0; break;
        }
    }
    if(identical) PASS("two SectionRenderers produce identical output (per-instance buffers)");
    else FAIL("isolation", "outputs differ — shared buffer contamination");

    /* Verify scratch_l/r are not aliases of each other between instances */
    if(sr1.scratch_l != sr2.scratch_l)
        PASS("sr1.scratch_l != sr2.scratch_l (separate instances)");
    else
        FAIL("buffer isolation", "both renderers share the same scratch buffer!");

    section_renderer_destroy(&sr1);
    section_renderer_destroy(&sr2);
}

/* ================================================================
   N5-4: hash_event_stream — semantic equivalence
   ================================================================ */
static void test_event_stream_hash(void){
    printf("\n[event_stream_hash] N5-4\n");

    /* Two different VoicePrograms that produce identical EventStreams:
       (1) two notes directly
       (2) same two notes but with a REPEAT block of count=1 wrapping the second */

    /* Build a TempoMap (constant 120 BPM) */
    TempoMap tm; tempo_map_constant(&tm, 120.0, 44100.0);

    /* Voice 1: note(60), note(64) */
    VoiceBuilder vb1; vb_init(&vb1);
    vb_note(&vb1, 60, DUR_1_4, 5);
    vb_note(&vb1, 64, DUR_1_4, 5);

    /* Voice 2: note(60), then REPEAT(1){note(64)} — same musical result */
    VoiceProgram vp2;
    vp2.n = 4;
    vp2.code[0] = VI_PACK(VI_NOTE, 60, DUR_1_4, 5);
    vp2.code[1] = VI_PACK(VI_REPEAT_BEGIN, 0, 0, 0);
    vp2.code[2] = VI_PACK(VI_NOTE, 64, DUR_1_4, 5);
    vp2.code[3] = VI_PACK(VI_REPEAT_END, 0, 0, 1); /* count=1 */

    /* DSL hashes must differ (different programs) */
    uint64_t dsl1 = hash_voice_prog(&vb1.vp);
    uint64_t dsl2 = hash_voice_prog(&vp2);
    if(dsl1 != dsl2) PASS("DSL hashes differ (different programs)");
    else FAIL("DSL diff", "same DSL hash — premise wrong");

    /* Compile both */
    EventStream es1, es2;
    char err[64]="";
    voice_compile_tempo(&vb1.vp, &es1, &tm, 0.0, err, sizeof(err));
    voice_compile_tempo(&vp2,    &es2, &tm, 0.0, err, sizeof(err));

    /* EventStream hashes must be equal (same events) */
    uint64_t eh1 = hash_event_stream(&es1);
    uint64_t eh2 = hash_event_stream(&es2);
    if(eh1 == eh2) PASS("EventStream hashes equal for semantically identical programs");
    else FAIL("ES hash equality", "es1=%llx es2=%llx", (unsigned long long)eh1, (unsigned long long)eh2);

    /* Different pitches → different ES hash */
    VoiceBuilder vb3; vb_init(&vb3);
    vb_note(&vb3, 60, DUR_1_4, 5);
    vb_note(&vb3, 67, DUR_1_4, 5); /* G4 instead of E4 */
    EventStream es3;
    voice_compile_tempo(&vb3.vp, &es3, &tm, 0.0, err, sizeof(err));
    uint64_t eh3 = hash_event_stream(&es3);
    if(eh1 != eh3) PASS("different pitches → different ES hash");
    else FAIL("ES hash diff", "same hash despite different pitches");

    /* Deterministic: hash same stream twice */
    uint64_t eh1b = hash_event_stream(&es1);
    if(eh1 == eh1b) PASS("hash_event_stream is deterministic");
    else FAIL("deterministic", "got different values on second call");
}

/* ================================================================
   Main
   ================================================================ */
int main(void){
    printf("=== SHMC N5-* Tests ===\n");
    tables_init();

    test_canonicalize();
    test_motif_transforms();
    test_thread_safety_baseline();
    test_event_stream_hash();

    printf("\n=== %d / %d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
