#pragma once
/*
 * shmc_evo_fitness.h  —  Evolutionary / Diversity-Based Fitness
 *
 * Problem with the previous fitness function:
 *   wfeat_fitness() uses hardcoded Gaussian targets (ZCR center=0.012,
 *   sigma=0.010; dynamic_range target=5 dB) and fixed weights.  These
 *   embed one aesthetic preference ("bright, dynamic, rhythmically varied")
 *   and cause premature saturation once a program hits those specific values.
 *   They are literally "one shoe fits all".
 *
 * This module implements THREE complementary evolutionary fitness measures
 * that require NO hardcoded musical targets:
 *
 * 1. SpectralDiversity (EF_SPECTRAL_DIV)
 *    Embed each audio frame as a fixed-length spectral energy vector
 *    (mel-like bark bands).  Fitness = mean pairwise L2 distance between
 *    all frame embeddings.  A program that is "sonically interesting" has
 *    audio that changes over time — frames are far from each other.
 *    Silence/noise/drone all score LOW because frames are identical.
 *
 * 2. SelfDissimilarity (EF_SELF_DISSIM)
 *    Compute the self-similarity matrix of a short-time spectral sequence.
 *    Fitness = 1 − mean(off_diagonal / diagonal).  A monochromatic drone
 *    has a flat self-similarity matrix (everything similar) → score near 0.
 *    Varied music has large off-diagonal variance → score near 1.
 *
 * 3. TemporalEntropy (EF_TEMPORAL_ENT)
 *    Divide audio into blocks, compute RMS per block, treat as probability
 *    distribution, compute Shannon entropy.  Flat envelope (constant volume)
 *    → low entropy.  Music with dynamics → high entropy.  No target level
 *    required — any non-flat envelope is rewarded.
 *
 * Combined fitness:
 *   evo_fitness = w1*spectral_div + w2*self_dissim + w3*temporal_ent
 *   Defaults: w1=0.45  w2=0.35  w3=0.20
 *
 * All three measures share the property:
 *   - Score of 0 → trivially bad (silence, pure drone, constant envelope)
 *   - Score of 1 → maximally varied audio
 *   - NO "ideal" value exists — more variation is always better*
 *     (* within the sensible range for music)
 *
 * This means the fitness landscape has a true gradient up to program
 * complexity, rather than plateauing when acoustic statistics match a target.
 *
 * N_BANDS: number of bark-scale frequency bands for spectral embedding.
 * N_FRAMES: number of frames to sample from the audio for diversity calc.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EF_N_BANDS    16    /* bark-scale bands for spectral embedding */
#define EF_N_FRAMES   32    /* frames sampled from audio */
#define EF_FRAME_SZ   512   /* samples per frame */

/* Spectral embedding of one audio frame: bark-band RMS energy vector */
typedef struct {
    float e[EF_N_BANDS];   /* energy per band, normalised to [0,1] */
} EFFrame;

/* Full feature set computed from an audio buffer */
typedef struct {
    EFFrame frames[EF_N_FRAMES];  /* spectral embeddings */
    float   spectral_div;         /* mean pairwise L2 between frames */
    float   self_dissim;          /* 1 - mean similarity ratio */
    float   temporal_ent;         /* Shannon entropy of RMS envelope */
    /* Harmonicity (Helmholtz + Terhardt virtual pitch) */
    float   harmonicity;          /* 0=inharmonic/noise  1=pure harmonic series */
    float   roughness;            /* 1 - Sethares dissonance score (beauty) */
} EvoFeat;

/* Weights for the combined score */
typedef struct {
    float w_spectral;     /* default 0.30 */
    float w_dissim;       /* default 0.20 */
    float w_temporal;     /* default 0.15 */
    float w_harmonicity;  /* default 0.25 — Helmholtz integer-ratio score */
    float w_roughness;    /* default 0.10 — Sethares dissonance beauty */
} EvoWeights;

/* Fill EvoWeights with defaults */
void evo_weights_default(EvoWeights *w);

/*
 * Compute all three evolutionary fitness measures from a raw audio buffer.
 * audio: float PCM, range [-1,1]
 * n:     number of samples
 * sr:    sample rate (for band boundary calculation)
 * out:   filled with embeddings + three scores
 */
void evo_feat_extract(const float *audio, int n, float sr, EvoFeat *out);

/*
 * Combined evolutionary fitness score in [0, 1].
 * Higher = more sonically varied = more "interesting" by any aesthetic.
 */
float evo_fitness(const EvoFeat *f, const EvoWeights *w);

/*
 * Compute bark-scale band boundaries for a given sample rate.
 * Returns EF_N_BANDS+1 values in Hz: edges[0]=0, edges[N_BANDS]=sr/2.
 */
void evo_bark_edges(float sr, float edges[EF_N_BANDS + 1]);

#ifdef __cplusplus
}
#endif
