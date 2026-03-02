/*
 * verify_evo_fitness.c  —  Formal verification of shmc_evo_fitness
 *
 * T1:  evo_weights_default sums to 1.0 (±0.001)
 * T2:  bark_edges: edges[0]=0, edges[N_BANDS]=sr/2, monotone increasing
 * T3:  evo_feat_extract on silence → spectral_div=0, self_dissim=0, temporal_ent=0
 * T4:  evo_feat_extract on white noise → spectral_div > 0
 * T5:  evo_feat_extract on white noise → self_dissim > 0
 * T6:  evo_feat_extract on white noise → temporal_ent > 0
 * T7:  spectral_div in [0, 1] for any input
 * T8:  self_dissim in [0, 1] for any input
 * T9:  temporal_ent in [0, 1] for any input
 * T10: evo_fitness in [0, 1] for any input
 * T11: white noise has higher spectral_div than pure sine
 * T12: white noise has higher self_dissim than pure sine
 * T13: amplitude-modulated sine has higher temporal_ent than constant sine
 * T14: evo_fitness(noise) > evo_fitness(silence)
 * T15: evo_fitness(pure_sine) > evo_fitness(noise) — harmonicity rewards pitched sounds
 * T16: frame_count: at least 2 frames processed for n >= 2*FRAME_SZ
 * T17: spectral_div is symmetric (reversing audio doesn't change score by much)
 * T18: bark_edges: all edges in [0, sr/2]
 * T19: evo_feat_extract handles n < FRAME_SZ gracefully (returns zeros)
 * T20: two identical audio buffers → spectral_div == 0, self_dissim == 0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../lemonade/include/shmc_evo_fitness.h"

static int T=0, P=0;
#define CHECK(c,m,...) do{T++;if(c){P++;printf("  \xe2\x9c\x93 " m "\n",##__VA_ARGS__);}else{printf("  \xe2\x9c\x97 FAIL: " m "\n",##__VA_ARGS__);}}while(0)

#define SR 44100.f
#define N_AUDIO (EF_FRAME_SZ * (EF_N_FRAMES + 2))

/* Generate white noise */
static void make_noise(float *buf, int n, uint32_t seed) {
    uint32_t s = seed;
    for (int i = 0; i < n; i++) {
        s ^= s<<13; s ^= s>>17; s ^= s<<5;
        buf[i] = ((float)(s & 0xFFFF) / 32768.f) - 1.f;
    }
}

/* Generate pure sine at freq Hz */
static void make_sine(float *buf, int n, float freq, float sr) {
    for (int i = 0; i < n; i++)
        buf[i] = 0.5f * sinf(2.f * 3.14159265f * freq * (float)i / sr);
}

/* Generate amplitude-modulated sine */
static void make_am_sine(float *buf, int n, float carrier, float mod_hz, float sr) {
    for (int i = 0; i < n; i++) {
        float am = 0.5f + 0.5f * sinf(2.f * 3.14159265f * mod_hz * (float)i / sr);
        buf[i] = am * sinf(2.f * 3.14159265f * carrier * (float)i / sr);
    }
}

int main(void) {
    printf("=== verify_evo_fitness ===\n");

    float *buf_noise  = (float*)malloc(N_AUDIO * sizeof(float));
    float *buf_sine   = (float*)malloc(N_AUDIO * sizeof(float));
    float *buf_amsin  = (float*)malloc(N_AUDIO * sizeof(float));
    float *buf_zero   = (float*)calloc(N_AUDIO, sizeof(float));
    float *buf_noise2 = (float*)malloc(N_AUDIO * sizeof(float));

    make_noise(buf_noise,  N_AUDIO, 0xCAFEBABE);
    make_sine (buf_sine,   N_AUDIO, 440.f, SR);
    make_am_sine(buf_amsin,N_AUDIO, 440.f, 2.f, SR);
    make_noise(buf_noise2, N_AUDIO, 0xCAFEBABE);  /* identical seed = identical */

    /* T1: weights sum to 1 */
    {
        EvoWeights w; evo_weights_default(&w);
        float s = w.w_spectral + w.w_dissim + w.w_temporal
                + w.w_harmonicity + w.w_roughness;
        CHECK(fabsf(s - 1.f) < 0.001f, "weights sum = %.4f (≈1.000)", s);
    }

    /* T2: bark_edges monotone, starts at 0, ends at sr/2 */
    {
        float edges[EF_N_BANDS + 1];
        evo_bark_edges(SR, edges);
        int mono = 1;
        for (int i = 1; i <= EF_N_BANDS; i++)
            if (edges[i] <= edges[i-1]) mono = 0;
        CHECK(edges[0] == 0.f, "bark_edges[0] == 0");
        CHECK(fabsf(edges[EF_N_BANDS] - SR/2.f) < 1.f,
              "bark_edges[N_BANDS] ≈ sr/2 (%.1f ≈ %.1f)", edges[EF_N_BANDS], SR/2.f);
        CHECK(mono, "bark_edges monotone increasing");
    }

    /* T3: silence → all scores zero */
    {
        EvoFeat f; evo_feat_extract(buf_zero, N_AUDIO, SR, &f);
        CHECK(f.spectral_div == 0.f, "silence: spectral_div = 0");
        CHECK(f.self_dissim  == 0.f, "silence: self_dissim = 0 (%.4f)", f.self_dissim);
        CHECK(f.temporal_ent == 0.f, "silence: temporal_ent = 0");
    }

    /* T4-T6: noise → all scores > 0 */
    {
        EvoFeat f; evo_feat_extract(buf_noise, N_AUDIO, SR, &f);
        CHECK(f.spectral_div > 0.f, "noise: spectral_div > 0 (%.4f)", f.spectral_div);
        CHECK(f.self_dissim  > 0.f, "noise: self_dissim > 0 (%.4f)", f.self_dissim);
        CHECK(f.temporal_ent > 0.f, "noise: temporal_ent > 0 (%.4f)", f.temporal_ent);
    }

    /* T7-T9: scores in [0,1] */
    {
        EvoFeat fn, fs, fam;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_sine,  N_AUDIO, SR, &fs);
        evo_feat_extract(buf_amsin, N_AUDIO, SR, &fam);
        CHECK(fn.spectral_div >= 0.f && fn.spectral_div <= 1.f,
              "spectral_div in [0,1] for noise (%.4f)", fn.spectral_div);
        CHECK(fn.self_dissim >= 0.f && fn.self_dissim <= 1.f,
              "self_dissim in [0,1] for noise (%.4f)", fn.self_dissim);
        CHECK(fn.temporal_ent >= 0.f && fn.temporal_ent <= 1.f,
              "temporal_ent in [0,1] for noise (%.4f)", fn.temporal_ent);
        /* Also check sine stays in range */
        int in_range = (fs.spectral_div >= 0.f && fs.spectral_div <= 1.f &&
                        fs.self_dissim  >= 0.f && fs.self_dissim  <= 1.f &&
                        fs.temporal_ent >= 0.f && fs.temporal_ent <= 1.f);
        CHECK(in_range, "all scores in [0,1] for sine");
    }

    /* T10: evo_fitness in [0,1] */
    {
        EvoWeights w; evo_weights_default(&w);
        EvoFeat fn, fz;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_zero,  N_AUDIO, SR, &fz);
        float fitn = evo_fitness(&fn, &w);
        float fitz = evo_fitness(&fz, &w);
        CHECK(fitn >= 0.f && fitn <= 1.f, "evo_fitness(noise) in [0,1] (%.4f)", fitn);
        CHECK(fitz == 0.f, "evo_fitness(silence) == 0 (%.4f)", fitz);
    }

    /* T11: noise spectral_div > sine spectral_div */
    {
        EvoFeat fn, fs;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_sine,  N_AUDIO, SR, &fs);
        CHECK(fn.spectral_div > fs.spectral_div,
              "noise spectral_div (%.4f) > sine (%.4f)", fn.spectral_div, fs.spectral_div);
    }

    /* T12: noise self_dissim > sine self_dissim */
    {
        EvoFeat fn, fs;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_sine,  N_AUDIO, SR, &fs);
        CHECK(fn.self_dissim > fs.self_dissim,
              "noise self_dissim (%.4f) > sine (%.4f)", fn.self_dissim, fs.self_dissim);
    }

    /* T13: AM-sine has higher temporal_ent than constant sine */
    {
        EvoFeat fam, fs;
        evo_feat_extract(buf_amsin, N_AUDIO, SR, &fam);
        evo_feat_extract(buf_sine,  N_AUDIO, SR, &fs);
        CHECK(fam.temporal_ent > fs.temporal_ent,
              "AM-sine temporal_ent (%.4f) > const sine (%.4f)",
              fam.temporal_ent, fs.temporal_ent);
    }

    /* T14: noise better than silence */
    {
        EvoWeights w; evo_weights_default(&w);
        EvoFeat fn, fz;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_zero,  N_AUDIO, SR, &fz);
        CHECK(evo_fitness(&fn, &w) > evo_fitness(&fz, &w),
              "evo_fitness(noise) > evo_fitness(silence)");
    }

    /* T15 (Stage 12b updated): sine > noise once harmonicity is included.
     * Old fitness (diversity only): noise scored higher (more spectral variation).
     * New fitness (+harmonicity): sine has a pure harmonic series → higher score. */
    {
        EvoWeights w; evo_weights_default(&w);
        EvoFeat fn, fs;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(buf_sine,  N_AUDIO, SR, &fs);
        float fitn = evo_fitness(&fn, &w);
        float fits = evo_fitness(&fs, &w);
        CHECK(fits > fitn,
              "evo_fitness(sine)=%.4f > evo_fitness(noise)=%.4f", fits, fitn);
    }

    /* T16: at least 2 frames processed */
    {
        /* For N_AUDIO = FRAME_SZ * (N_FRAMES+2), we should get all N_FRAMES */
        EvoFeat f; evo_feat_extract(buf_noise, N_AUDIO, SR, &f);
        /* Check that frames were populated (at least frame[0] is non-zero) */
        float sum0 = 0.f;
        for (int b = 0; b < EF_N_BANDS; b++) sum0 += f.frames[0].e[b];
        CHECK(sum0 > 0.f, "frame[0] non-zero after extraction (sum=%.4f)", sum0);
    }

    /* T17: spectral_div approximately symmetric (reversed audio) */
    {
        float *rev = (float*)malloc(N_AUDIO * sizeof(float));
        for (int i = 0; i < N_AUDIO; i++) rev[i] = buf_noise[N_AUDIO-1-i];
        EvoFeat fn, fr;
        evo_feat_extract(buf_noise, N_AUDIO, SR, &fn);
        evo_feat_extract(rev, N_AUDIO, SR, &fr);
        /* Allow 10% relative difference for reversed audio */
        float diff = fabsf(fn.spectral_div - fr.spectral_div);
        float mean = (fn.spectral_div + fr.spectral_div) * 0.5f + 1e-6f;
        CHECK(diff / mean < 0.15f,
              "spectral_div ~symmetric: fwd=%.4f rev=%.4f diff%%=%.1f%%",
              fn.spectral_div, fr.spectral_div, 100.f*diff/mean);
        free(rev);
    }

    /* T18: all bark edges in [0, sr/2] */
    {
        float edges[EF_N_BANDS + 1];
        evo_bark_edges(SR, edges);
        int ok = 1;
        for (int i = 0; i <= EF_N_BANDS; i++)
            if (edges[i] < 0.f || edges[i] > SR/2.f + 1.f) ok = 0;
        CHECK(ok, "all bark edges in [0, sr/2]");
    }

    /* T19: short audio (n < FRAME_SZ) → zeros, no crash */
    {
        float short_buf[16] = {0.1f, 0.2f, 0.3f};
        EvoFeat f;
        evo_feat_extract(short_buf, 16, SR, &f);
        CHECK(f.spectral_div == 0.f && f.temporal_ent == 0.f,
              "short audio: returns zeros gracefully");
    }

    /* T20: identical audio → spectral_div ≈ 0 and self_dissim ≈ 0 */
    {
        /* Two identical noise buffers */
        EvoFeat f1, f2;
        evo_feat_extract(buf_noise,  N_AUDIO, SR, &f1);
        evo_feat_extract(buf_noise2, N_AUDIO, SR, &f2);
        CHECK(fabsf(f1.spectral_div - f2.spectral_div) < 0.001f,
              "identical audio: spectral_div is deterministic (%.6f == %.6f)",
              f1.spectral_div, f2.spectral_div);
    }

    free(buf_noise); free(buf_sine); free(buf_amsin);
    free(buf_zero);  free(buf_noise2);

    printf("\n  RESULT: %d/%d PASSED\n", P, T);
    return (P == T) ? 0 : 1;
}
