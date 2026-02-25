/*
 * SHMC Layer 0b — TempoMap + patch_meta test suite
 *
 * Tests from review 3 §10:
 *   1. Constant BPM sanity
 *   2. Piecewise-constant segment continuity
 *   3. Linear SPB ramp integral + round-trip
 *   4. Linear BPM ramp vs. numerical integration
 *   5. Round-trip precision (beat→time→beat within 1e-9)
 *   6. Scheduling reproducibility
 *
 * Plus:
 *   7. state_offset vs i*4 correctness (R1-1)
 *   8. hash_patch determinism + inequality (R1-2)
 *   9. PatchCost counts (R1-4)
 *  10. Grammar validation (R1-3)
 *  11. exec1_ex audio sanity (denormal protection + state_offset)
 *  12. Hash collision: mutated patches have different hashes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "tempo_map.h"
#include "patch_meta.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer0/include/patch.h"

#define SR 44100.0
#define PASS(name) do{ printf("  [PASS] %s\n", name); passed++; total++; }while(0)
#define FAIL(name, ...) do{ printf("  [FAIL] %s: ", name); printf(__VA_ARGS__); printf("\n"); total++; }while(0)
#define NEAR(a,b,eps) (fabs((a)-(b)) < (eps))

static int passed = 0, total = 0;

/* ----------------------------------------------------------------
   Numerical integration ground truth for beat_to_seconds tests
   ---------------------------------------------------------------- */
static double numerical_integrate(const TempoMap *m, double b0, double b1, int steps){
    double h = (b1 - b0) / steps;
    double sum = 0.0;
    for(int i = 0; i < steps; i++){
        double b = b0 + (i + 0.5) * h;
        double bpm = tempo_bpm_at(m, b);
        sum += 60.0 / bpm;
    }
    return sum * h;
}

/* ----------------------------------------------------------------
   TEST 1: Constant BPM sanity
   ---------------------------------------------------------------- */
static void test_constant_bpm(void){
    printf("\n[constant_bpm]\n");
    TempoMap m;
    tempo_map_constant(&m, 120.0, SR);

    double t4 = tempo_beat_to_seconds(&m, 4.0);
    /* 120 BPM: 4 beats = 4 * 0.5s = 2.0s */
    if(NEAR(t4, 2.0, 1e-9))
        PASS("4 beats @ 120 BPM = 2.000s");
    else
        FAIL("4 beats", "got %.10f, want 2.0", t4);

    /* Round-trip */
    double b_back = tempo_seconds_to_beat(&m, 2.0);
    if(NEAR(b_back, 4.0, 1e-9))
        PASS("round-trip 2.0s → 4 beats");
    else
        FAIL("round-trip", "got %.10f", b_back);

    /* Sample index */
    int64_t smp = tempo_beat_to_sample(&m, 4.0);
    int64_t expected = llround(2.0 * SR);
    if(smp == expected)
        PASS("beat_to_sample precision");
    else
        FAIL("beat_to_sample", "got %lld, want %lld", (long long)smp, (long long)expected);
}

/* ----------------------------------------------------------------
   TEST 2: Piecewise-constant segment continuity
   ---------------------------------------------------------------- */
static void test_piecewise_constant(void){
    printf("\n[piecewise_constant]\n");
    TempoPoint pts[2] = {
        {0.0, 120.0, TM_STEP},
        {4.0,  60.0, TM_STEP},
    };
    TempoMap m;
    int r = tempo_map_build(&m, pts, 2, SR);
    if(r != 0){ FAIL("build", "returned %d", r); return; }

    /* Segment 1: 0..4 beats @ 120 BPM → 2.0s */
    double t_at_4 = tempo_beat_to_seconds(&m, 4.0);
    if(NEAR(t_at_4, 2.0, 1e-9))
        PASS("t(4) = 2.000s");
    else
        FAIL("t(4)", "got %.10f", t_at_4);

    /* Continuity: t(4-ε) and t(4+ε) should straddle 2.0 */
    double t_before = tempo_beat_to_seconds(&m, 3.9999);
    double t_after  = tempo_beat_to_seconds(&m, 4.0001);
    if(t_before < t_at_4 && t_after > t_at_4)
        PASS("continuity at segment boundary");
    else
        FAIL("continuity", "before=%.8f at=%.8f after=%.8f", t_before, t_at_4, t_after);

    /* 8 beats past 4 @ 60 BPM = 2.0 + 8.0 = 10.0s */
    double t_at_12 = tempo_beat_to_seconds(&m, 12.0);
    if(NEAR(t_at_12, 10.0, 1e-9))
        PASS("t(12) = 10.000s (2s + 8s)");
    else
        FAIL("t(12)", "got %.10f", t_at_12);

    /* Round-trip */
    double b_back = tempo_seconds_to_beat(&m, 10.0);
    if(NEAR(b_back, 12.0, 1e-9))
        PASS("round-trip 10.0s → 12 beats");
    else
        FAIL("round-trip", "got %.10f", b_back);
}

/* ----------------------------------------------------------------
   TEST 3: Linear SPB ramp
   ---------------------------------------------------------------- */
static void test_linear_spb(void){
    printf("\n[linear_spb]\n");
    /* SPB from 0.5 (120 BPM) to 1.0 (60 BPM) across 4 beats */
    /* Closed-form: t(4) = S0*4 + 0.5*k*16 = 0.5*4 + 0.5*(0.5/4)*16 = 2.0 + 1.0 = 3.0s */
    TempoPoint pts[2] = {
        {0.0, 120.0, TM_LINEAR_SPB},
        {4.0,  60.0, TM_STEP},
    };
    TempoMap m;
    tempo_map_build(&m, pts, 2, SR);

    double t4 = tempo_beat_to_seconds(&m, 4.0);
    /* Numerical ground truth */
    double t4_num = numerical_integrate(&m, 0.0, 4.0, 100000);
    if(NEAR(t4, 3.0, 1e-6))
        PASS("t(4) linear SPB = 3.000s (closed-form)");
    else
        FAIL("t(4) closed-form", "got %.10f", t4);

    if(NEAR(t4, t4_num, 1e-5))
        PASS("closed-form matches numerical integration");
    else
        FAIL("vs numerical", "closed=%.8f num=%.8f diff=%.2e", t4, t4_num, fabs(t4-t4_num));

    /* Round-trip */
    double b_back = tempo_seconds_to_beat(&m, t4);
    if(NEAR(b_back, 4.0, 1e-8))
        PASS("round-trip t(4) → 4.0 beats");
    else
        FAIL("round-trip", "got %.10f", b_back);

    /* Mid-ramp */
    double t2 = tempo_beat_to_seconds(&m, 2.0);
    double t2_num = numerical_integrate(&m, 0.0, 2.0, 100000);
    if(NEAR(t2, t2_num, 1e-5))
        PASS("mid-ramp t(2) matches numerical");
    else
        FAIL("mid-ramp", "closed=%.8f num=%.8f", t2, t2_num);
}

/* ----------------------------------------------------------------
   TEST 4: Linear BPM ramp vs. numerical integration
   ---------------------------------------------------------------- */
static void test_linear_bpm(void){
    printf("\n[linear_bpm]\n");
    /* BPM 60 → 120 over 4 beats */
    TempoPoint pts[2] = {
        {0.0,  60.0, TM_LINEAR_BPM},
        {4.0, 120.0, TM_STEP},
    };
    TempoMap m;
    tempo_map_build(&m, pts, 2, SR);

    double t4      = tempo_beat_to_seconds(&m, 4.0);
    double t4_num  = numerical_integrate(&m, 0.0, 4.0, 1000000);

    if(NEAR(t4, t4_num, 1e-5))
        PASS("linear BPM closed-form matches numerical (1M steps)");
    else
        FAIL("linear BPM", "closed=%.8f num=%.8f diff=%.2e", t4, t4_num, fabs(t4-t4_num));

    /* Round-trip */
    double b_back = tempo_seconds_to_beat(&m, t4);
    if(NEAR(b_back, 4.0, 1e-8))
        PASS("round-trip linear BPM");
    else
        FAIL("round-trip", "got %.10f", b_back);

    /* BPM at known positions */
    double bpm0 = tempo_bpm_at(&m, 0.0);
    double bpm4 = tempo_bpm_at(&m, 4.0);
    if(NEAR(bpm0,  60.0, 1e-9)) PASS("BPM(0) = 60");
    else FAIL("BPM(0)", "got %.4f", bpm0);
    if(NEAR(bpm4, 120.0, 1e-9)) PASS("BPM(4) = 120 (held after map)");
    else FAIL("BPM(4)", "got %.4f", bpm4);
}

/* ----------------------------------------------------------------
   TEST 5: Round-trip precision over many random beats
   ---------------------------------------------------------------- */
static void test_roundtrip_precision(void){
    printf("\n[roundtrip_precision]\n");
    /* Complex 4-segment map */
    TempoPoint pts[] = {
        { 0.0,  90.0, TM_STEP},
        { 4.0, 120.0, TM_LINEAR_SPB},
        { 8.0, 180.0, TM_LINEAR_BPM},
        {16.0,  60.0, TM_STEP},
        {20.0, 100.0, TM_STEP},
    };
    TempoMap m;
    tempo_map_build(&m, pts, 5, SR);

    double max_err = 0.0;
    uint32_t rng = 0xABCDEF01u;
    int n_tests = 10000;
    for(int i = 0; i < n_tests; i++){
        rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
        double b = (double)(rng >> 1) / (double)(1u<<31) * 24.0; /* 0..24 beats */
        double t = tempo_beat_to_seconds(&m, b);
        double b2 = tempo_seconds_to_beat(&m, t);
        double err = fabs(b2 - b);
        if(err > max_err) max_err = err;
    }
    if(max_err < 1e-9)
        PASS("10000 random b→t→b round-trips within 1e-9");
    else
        FAIL("round-trip", "max error = %.2e (want < 1e-9)", max_err);

    /* Sample-domain round-trip */
    double max_smp_err = 0.0;
    rng = 0xDEADBEEFu;
    for(int i = 0; i < 10000; i++){
        rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
        double b = (double)(rng >> 1) / (double)(1u<<31) * 20.0;
        int64_t smp  = tempo_beat_to_sample(&m, b);
        double  b2   = tempo_sample_to_beat(&m, smp);
        /* Allow ±1 sample of error */
        double err = fabs(b2 - b);
        if(err > max_smp_err) max_smp_err = err;
    }
    /* Tolerance: 1 sample at slowest BPM (60 BPM = 44100 samples/beat → 2.27e-5 beats/sample) */
    if(max_smp_err < 5e-5)
        PASS("10000 beat→sample→beat round-trips within 1 sample");
    else
        FAIL("sample round-trip", "max_err=%.2e beats", max_smp_err);
}

/* ----------------------------------------------------------------
   TEST 6: Scheduling reproducibility
   ---------------------------------------------------------------- */
static void test_scheduling_reproducibility(void){
    printf("\n[scheduling_reproducibility]\n");
    TempoMap m;
    tempo_map_constant(&m, 120.0, SR);

    /* Beat positions for 4 quarter notes */
    double beats[4] = {0.0, 1.0, 2.0, 3.0};
    int64_t samples[4];
    for(int i = 0; i < 4; i++)
        samples[i] = tempo_beat_to_sample(&m, beats[i]);

    /* Rebuild map and recompute — must be identical */
    TempoMap m2;
    tempo_map_constant(&m2, 120.0, SR);
    int ok = 1;
    for(int i = 0; i < 4; i++){
        int64_t s2 = tempo_beat_to_sample(&m2, beats[i]);
        if(s2 != samples[i]){ ok = 0; break; }
    }
    if(ok)
        PASS("sample indices reproducible across builds");
    else
        FAIL("reproducibility", "indices differ between builds");

    /* Verify expected values at 120 BPM */
    int64_t expected[4] = {0, 22050, 44100, 66150};
    int match = 1;
    for(int i = 0; i < 4; i++)
        if(samples[i] != expected[i]){ match = 0; break; }
    if(match)
        PASS("sample indices at 120 BPM: 0, 22050, 44100, 66150");
    else{
        printf("  got: ");
        for(int i=0;i<4;i++) printf("%lld ", (long long)samples[i]);
        FAIL("values", "wrong sample indices");
    }
}

/* ----------------------------------------------------------------
   TEST 7: state_offset vs i*4 equivalence for standard patches
   ---------------------------------------------------------------- */
static void test_state_offsets(void){
    printf("\n[state_offsets]\n");

    /* Build a patch with known stateful ops at known positions:
       instr 0: OP_CONST (0 state slots)   -> offset = 0
       instr 1: OP_OSC   (1 slot: phase)   -> offset = 0
       instr 2: OP_LPF   (1 slot)          -> offset = 1
       instr 3: OP_BPF   (2 slots: lv+bv)  -> offset = 2
       instr 4: OP_ADSR  (4 slots)         -> offset = 4
       instr 5: OP_MUL   (0 slots)         -> offset = 8
       instr 6: OP_OUT   (0 slots)         -> offset = 8
       total state = 8
 */
    PatchBuilder b; pb_init(&b);
    int rc = pb_const_mod(&b, 10);         /* 0: CONST   offset 0, 0 slots */
    int ro = pb_osc(&b, REG_ONE);          /* 1: OSC     offset 0, 1 slot */
    int rf = pb_lpf(&b, ro, 20);           /* 2: LPF     offset 1, 1 slot */
    int rbp= pb_bpf(&b, rf, 25, 5);        /* 3: BPF     offset 2, 2 slots */
    int re = pb_adsr(&b, 0, 8, 20, 8);    /* 4: ADSR    offset 4, 4 slots */
    int rm = pb_mul(&b, rbp, re);          /* 5: MUL     offset 8, 0 slots */
    pb_out(&b, rm);                         /* 6: OUT     offset 8, 0 slots */
    (void)rc;
    PatchProgram *prog = pb_finish(&b);

    PatchProgramEx ex;
    patch_to_ex(prog, &ex);

    /* Expected offsets */
    int exp_off[] = {0, 0, 1, 2, 4, 8, 8};
    int ok = 1;
    for(int i = 0; i < ex.n_instrs; i++){
        if(ex.state_offset[i] != (uint16_t)exp_off[i]){
            printf("  instr %d: expected offset %d, got %d (op=%d)\n",
                   i, exp_off[i], ex.state_offset[i], INSTR_OP(ex.code[i]));
            ok = 0;
        }
    }
    if(ok) PASS("state_offset[] matches expected per-instr layout");
    else   FAIL("state_offset", "mismatch (see above)");

    int exp_n_state = 8;
    if(ex.n_state == exp_n_state)
        PASS("n_state = 8 (correct total)");
    else
        FAIL("n_state", "got %d, want %d", ex.n_state, exp_n_state);

    /* Render with exec1_ex and verify it produces finite audio */
    tables_init();
    PatchState ps; memset(&ps,0,sizeof(ps));
    ps.sr = 44100.f; ps.dt = 1.f/44100.f;
    ps.note_freq = 440.f; ps.note_vel = 0.8f;
    ps.rng = 0x12345u;
    ps.regs[REG_FREQ] = 440.f; ps.regs[REG_VEL] = 0.8f;
    ps.regs[REG_ONE] = 1.f;

    int finite_count = 0;
    for(int i = 0; i < 4096; i++){
        ps.regs[REG_TIME] = ps.note_time;
        float s = exec1_ex(&ps, &ex);
        if(isfinite(s)) finite_count++;
    }
    if(finite_count == 4096)
        PASS("exec1_ex produces 4096 finite samples");
    else
        FAIL("exec1_ex", "only %d/4096 finite samples", finite_count);
}

/* ----------------------------------------------------------------
   TEST 8: hash_patch determinism + inequality
   ---------------------------------------------------------------- */
static void test_hashing(void){
    printf("\n[hashing]\n");

    /* Two identical patches */
    PatchProgram p1, p2;
    { PatchBuilder b; pb_init(&b);
      int o = pb_osc(&b, REG_ONE); int e = pb_adsr(&b,0,8,20,8);
      pb_out(&b, pb_mul(&b,o,e)); p1 = *pb_finish(&b); }
    { PatchBuilder b; pb_init(&b);
      int o = pb_osc(&b, REG_ONE); int e = pb_adsr(&b,0,8,20,8);
      pb_out(&b, pb_mul(&b,o,e)); p2 = *pb_finish(&b); }

    uint64_t h1a = hash_patch_raw(&p1);
    uint64_t h1b = hash_patch_raw(&p1);
    uint64_t h2  = hash_patch_raw(&p2);

    if(h1a == h1b) PASS("hash is deterministic");
    else FAIL("determinism", "h1a=%llx h1b=%llx", (unsigned long long)h1a, (unsigned long long)h1b);

    if(h1a == h2) PASS("identical programs hash equally");
    else FAIL("equality", "h1=%llx h2=%llx", (unsigned long long)h1a, (unsigned long long)h2);

    /* Different patch must have different hash */
    PatchProgram p3;
    { PatchBuilder b; pb_init(&b);
      int o = pb_saw(&b, REG_ONE); int e = pb_adsr(&b,2,10,15,6);
      pb_out(&b, pb_mul(&b,o,e)); p3 = *pb_finish(&b); }
    uint64_t h3 = hash_patch_raw(&p3);

    if(h1a != h3) PASS("different programs hash differently");
    else FAIL("inequality", "both hash to %llx", (unsigned long long)h1a);

    /* PatchProgramEx hash */
    PatchProgramEx ex; patch_to_ex(&p1, &ex);
    uint64_t hex = hash_patch(&ex);
    if(hex == h1a) PASS("PatchProgramEx hash matches raw hash");
    else FAIL("ex hash", "ex=%llx raw=%llx", (unsigned long long)hex, (unsigned long long)h1a);
}

/* ----------------------------------------------------------------
   TEST 9: PatchCost counts
   ---------------------------------------------------------------- */
static void test_patch_cost(void){
    printf("\n[patch_cost]\n");

    /* osc(1 state) + lpf(1 state) + adsr(4 state) + mul(0) + out(0) = 6 state slots */
    PatchProgram p;
    { PatchBuilder b; pb_init(&b);
      int o = pb_osc(&b, REG_ONE);
      int f = pb_lpf(&b, o, 20);
      int e = pb_adsr(&b, 0, 8, 20, 8);
      pb_out(&b, pb_mul(&b, f, e)); p = *pb_finish(&b); }

    PatchCost c; patch_cost(&p, &c);

    if(c.n_instrs == p.n_instrs)
        PASS("n_instrs matches");
    else
        FAIL("n_instrs", "got %d, want %d", c.n_instrs, p.n_instrs);

    /* stateful: osc(1) + lpf(1) + adsr(1) = 3 */
    if(c.n_stateful == 3)
        PASS("n_stateful = 3 (osc, lpf, adsr)");
    else
        FAIL("n_stateful", "got %d, want 3", c.n_stateful);

    /* state slots: 1 + 1 + 4 = 6 */
    if(c.n_state_slots == 6)
        PASS("n_state_slots = 6");
    else
        FAIL("n_state_slots", "got %d, want 6", c.n_state_slots);

    /* est_cpu: osc(2) + lpf(2) + adsr(2) + mul(1) + out(1) = 8 */
    if(c.est_cpu == 8)
        PASS("est_cpu = 8");
    else
        FAIL("est_cpu", "got %d, want 8", c.est_cpu);
}

/* ----------------------------------------------------------------
   TEST 10: Grammar validation
   ---------------------------------------------------------------- */
static void test_grammar_validation(void){
    printf("\n[grammar_validation]\n");

    PatchProgram p;
    { PatchBuilder b; pb_init(&b);
      int o = pb_osc(&b, REG_ONE); int e = pb_adsr(&b,0,8,20,8);
      pb_out(&b, pb_mul(&b,o,e)); p = *pb_finish(&b); }

    char err[128];
    /* Valid program with generous limits */
    int r = patch_program_valid(&p, 32, 8, 64, err, sizeof(err));
    if(r == 0) PASS("valid program passes validation");
    else FAIL("valid", "rejected: %s", err);

    /* Reject: too many instrs */
    r = patch_program_valid(&p, 2, 8, 64, err, sizeof(err));
    if(r != 0) PASS("rejects program exceeding max_instrs");
    else FAIL("max_instrs", "should have rejected (n=%d)", p.n_instrs);

    /* Reject: too many stateful ops */
    r = patch_program_valid(&p, 32, 1, 64, err, sizeof(err));
    if(r != 0) PASS("rejects program exceeding max_stateful");
    else FAIL("max_stateful", "should have rejected");
}

/* ----------------------------------------------------------------
   TEST 11: exec1_ex denormal protection
   ---------------------------------------------------------------- */
static void test_denormal_protection(void){
    printf("\n[denormal_protection]\n");

    /* LP-filtered noise decays to near-zero — should not produce denormals */
    PatchProgram p;
    { PatchBuilder b; pb_init(&b);
      int n = pb_lp_noise(&b, 0);   /* very low cutoff → near-zero output */
      int e = pb_adsr(&b, 0, 1, 0, 1);  /* tiny sustain */
      pb_out(&b, pb_mul(&b, n, e)); p = *pb_finish(&b); }

    PatchProgramEx ex; patch_to_ex(&p, &ex);
    tables_init();

    PatchState ps; memset(&ps, 0, sizeof(ps));
    ps.sr = 44100.f; ps.dt = 1.f/44100.f;
    ps.note_freq = 200.f; ps.note_vel = 0.5f;
    ps.rng = 0xABCDu;
    ps.regs[REG_ONE] = 1.f; ps.regs[REG_FREQ] = 200.f; ps.regs[REG_VEL] = 0.5f;

    int denormal_count = 0;
    /* Run 44100 samples — envelope decays to sustain 0 (stage 2) */
    for(int i = 0; i < 44100; i++){
        ps.regs[REG_TIME] = ps.note_time;
        float s = exec1_ex(&ps, &ex);
        /* A denormal is nonzero but < FLT_MIN (1.175e-38) */
        float abs_s = fabsf(s);
        if(abs_s > 0.f && abs_s < 1.175494351e-38f) denormal_count++;
    }
    if(denormal_count == 0)
        PASS("no denormal values produced (FLUSH macro active)");
    else
        FAIL("denormals", "%d denormal samples out of 44100", denormal_count);
}

/* ----------------------------------------------------------------
   TEST 12: Hash collision check after mutation
   ---------------------------------------------------------------- */
static void test_hash_after_mutation(void){
    printf("\n[hash_after_mutation]\n");

    PatchProgram base;
    { PatchBuilder b; pb_init(&b);
      int o = pb_saw(&b, REG_ONE); int e = pb_adsr(&b,2,10,18,8);
      pb_out(&b, pb_mul(&b,o,e)); base = *pb_finish(&b); }

    uint64_t h_base = hash_patch_raw(&base);

    /* Modify ADSR parameters — hash must differ */
    PatchProgram mod = base;
    /* Perturb ADSR hi field: increase attack by 1 step */
    uint16_t hi = INSTR_IMM_HI(mod.code[1]); /* assuming instr 1 is ADSR */
    int att = ((hi>>10) & 0x1F);
    att = att < 31 ? att + 1 : att - 1;
    hi = (uint16_t)((hi & ~(0x1F<<10)) | (att<<10));
    mod.code[1] = INSTR_PACK(OP_ADSR, INSTR_DST(mod.code[1]),
                              0, 0, hi, INSTR_IMM_LO(mod.code[1]));

    uint64_t h_mod = hash_patch_raw(&mod);

    if(h_base != h_mod)
        PASS("mutated patch has different hash");
    else
        FAIL("hash_inequality", "base and mutated hash to same value %llx", (unsigned long long)h_base);
}

/* ----------------------------------------------------------------
   Main
   ---------------------------------------------------------------- */
int main(void){
    printf("=== SHMC Layer 0b — TempoMap + patch_meta tests ===\n");
    tables_init();

    test_constant_bpm();
    test_piecewise_constant();
    test_linear_spb();
    test_linear_bpm();
    test_roundtrip_precision();
    test_scheduling_reproducibility();
    test_state_offsets();
    test_hashing();
    test_patch_cost();
    test_grammar_validation();
    test_denormal_protection();
    test_hash_after_mutation();

    printf("\n=== %d / %d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
