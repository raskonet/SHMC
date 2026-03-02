/*
 * shmc_evo_fitness.c  —  Evolutionary / Diversity-Based Fitness
 *
 * Three measures, none requiring hardcoded musical targets.
 * See shmc_evo_fitness.h for full design rationale.
 */
#include "../include/shmc_evo_fitness.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── Bark scale ──────────────────────────────────────────────────── */

/*
 * bark_edges: compute EF_N_BANDS+1 frequency bin edges on the Bark scale.
 * Bark formula: z = 13*atan(0.00076*f) + 3.5*atan((f/7500)^2)
 * We space bands evenly in Bark units from 0 to bark(sr/2).
 */
void evo_bark_edges(float sr, float edges[EF_N_BANDS + 1]) {
    float f_max = sr * 0.5f;
    /* bark value at f_max */
    float b_max = 13.f * atanf(0.00076f * f_max)
                + 3.5f * atanf((f_max / 7500.f) * (f_max / 7500.f));
    for (int b = 0; b <= EF_N_BANDS; b++) {
        float bark_target = b_max * (float)b / (float)EF_N_BANDS;
        /* Inverse bark via bisection */
        float lo = 0.f, hi = f_max, mid = 0.f;
        for (int iter = 0; iter < 48; iter++) {
            mid = (lo + hi) * 0.5f;
            float bm = 13.f * atanf(0.00076f * mid)
                     + 3.5f * atanf((mid / 7500.f) * (mid / 7500.f));
            if (bm < bark_target) lo = mid; else hi = mid;
        }
        edges[b] = mid;
    }
    edges[0] = 0.f;
    edges[EF_N_BANDS] = f_max;
}

/* ── DFT for band energy (Goertzel per band) ─────────────────────── */

/*
 * frame_energy: compute RMS energy in each bark band for a frame of samples.
 * Uses a simple DFT magnitude sum — no FFT library dependency.
 * For EF_FRAME_SZ=512 and EF_N_BANDS=16, this is 512*16 = 8192 muls/adds.
 */
static void frame_energy(const float *samples, int n, float sr,
                          const float bark_edges[EF_N_BANDS + 1],
                          EFFrame *out) {
    /* Frequency resolution */
    float df = sr / (float)n;
    float total_e[EF_N_BANDS];
    memset(total_e, 0, sizeof(total_e));

    /* Compute DFT bins up to N/2 and accumulate into bark bands */
    for (int k = 1; k <= n / 2; k++) {
        float freq = (float)k * df;
        /* Which bark band? */
        int band = -1;
        for (int b = 0; b < EF_N_BANDS; b++) {
            if (freq >= bark_edges[b] && freq < bark_edges[b + 1]) {
                band = b; break;
            }
        }
        if (band < 0) continue;

        /* Goertzel: compute |X[k]|^2 */
        float w = 2.f * 3.14159265f * (float)k / (float)n;
        float cos_w = cosf(w), sin_w = sinf(w);
        float s0 = 0.f, s1 = 0.f, s2;
        float coeff = 2.f * cos_w;
        for (int i = 0; i < n; i++) {
            s2 = s1; s1 = s0;
            s0 = samples[i] + coeff * s1 - s2;
        }
        float re = s0 - s1 * cos_w;
        float im = s1 * sin_w;
        total_e[band] += re * re + im * im;
    }

    /* Normalise each band by sqrt(energy) → RMS-like, then scale to [0,1] */
    float max_e = 1e-12f;
    for (int b = 0; b < EF_N_BANDS; b++) {
        total_e[b] = sqrtf(total_e[b] / (float)n);
        if (total_e[b] > max_e) max_e = total_e[b];
    }
    for (int b = 0; b < EF_N_BANDS; b++)
        out->e[b] = total_e[b] / max_e;
}

/* ── Pairwise L2 distance ────────────────────────────────────────── */

static float frame_l2(const EFFrame *a, const EFFrame *b) {
    float s = 0.f;
    for (int i = 0; i < EF_N_BANDS; i++) {
        float d = a->e[i] - b->e[i]; s += d * d;
    }
    return sqrtf(s / (float)EF_N_BANDS);   /* normalised to [0, 1] range */
}

/* ── Main extraction ─────────────────────────────────────────────── */

void evo_weights_default(EvoWeights *w) {
    /* weights: includes harmonicity + roughness */
    w->w_spectral    = 0.30f;
    w->w_dissim      = 0.20f;
    w->w_temporal    = 0.15f;
    w->w_harmonicity = 0.25f;  /* Helmholtz integer-ratio alignment */
    w->w_roughness   = 0.10f;  /* Sethares dissonance beauty */
}


/* ── Harmonicity metrics ────────────────────────────────
 *
 * H1: Helmholtz integer-ratio score
 *   For each pair of spectral peaks (f_i, f_j), score how close their
 *   ratio is to a small integer ratio p:q (p,q <= 8).
 *   S_ij = max_{p,q<=8} exp(-((fi/fj - p/q)^2) / sigma^2)
 *   H = mean over all pairs.
 *   H~1 → harmonic chord.  H~0 → noise / inharmonic.
 *   (Helmholtz 1863, "On the Sensations of Tone")
 *
 * H2: Virtual pitch / Terhardt harmonicity
 *   For candidate fundamental F, score how well spectral peaks align
 *   with integer multiples k*F.
 *   H(F) = sum_i exp(-((fi - ki*F)^2) / sigma^2)
 *   Score = max_F H(F)  normalized to [0,1].
 *   (Terhardt 1974, "Pitch, consonance and harmony")
 *
 * H3: Sethares roughness beauty  B = exp(-D)
 *   D = sum_{i<j} ai*aj * (exp(-b1*df) - exp(-b2*df))
 *   df = |fi - fj|  for partials within ~1.5 critical bands.
 *   (Sethares 1993, "Local consonance and the relationship between
 *    timbre and scale")
 * ----------------------------------------------------------------- */

#define HARM_SIGMA2  (0.02f * 0.02f)  /* tolerance for ratio matching */
#define HARM_MAX_PQ  8                 /* max numerator/denominator */
#define HARM_NPEAKS  8                 /* spectral peaks to track */
#define HARM_N_FUND  48                /* candidate fundamentals to try */
#define HARM_B1      3.5f              /* Sethares roughness decay fast */
#define HARM_B2      5.75f             /* Sethares roughness decay slow */

/* Simple parabolic peak picker on a power spectrum */
static int pick_peaks(const float *mag, int n, float sr,
                      float *peak_hz, float *peak_amp, int maxp) {
    int np = 0;
    float hz_per_bin = sr / (float)(2 * n);
    for (int i = 1; i < n - 1 && np < maxp; i++) {
        if (mag[i] > mag[i-1] && mag[i] > mag[i+1] && mag[i] > 0.01f) {
            /* parabolic interpolation for sub-bin accuracy */
            float alpha = mag[i-1], beta = mag[i], gamma = mag[i+1];
            float p = 0.5f * (alpha - gamma) / (alpha - 2.f*beta + gamma + 1e-12f);
            peak_hz[np]  = ((float)i + p) * hz_per_bin;
            peak_amp[np] = beta;
            np++;
        }
    }
    return np;
}

/* Compute a small power spectrum from a block using Goertzel on log-spaced freqs */
static void block_spectrum(const float *audio, int n, float sr,
                             float *mag_out, int n_bins) {
    /* Log-spaced from 80 Hz to sr/2 */
    float lo = 80.f, hi = sr * 0.5f;
    for (int b = 0; b < n_bins; b++) {
        float t  = (float)b / (float)(n_bins - 1);
        float fr = lo * powf(hi / lo, t);   /* exponential spacing */
        double w  = 2.0 * 3.14159265358979 * fr / sr;
        double re = 0.0, im = 0.0;
        float norm = (n > 1) ? (float)(n - 1) : 1.f;
        for (int i = 0; i < n; i++) {
            /* Hann window */
            double hann = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / norm));
            double s    = (double)audio[i] * hann;
            re += s * cos(w * i);
            im += s * sin(w * i);
        }
        mag_out[b] = (float)sqrt(re*re + im*im);
    }
}

static float compute_harmonicity(const float *audio, int n, float sr) {
    if (n < 64) return 0.5f;

    /* Build spectrum */
    enum { N_BINS = 64 };
    float mag[N_BINS];
    block_spectrum(audio, (n < 2048 ? n : 2048), sr, mag, N_BINS);

    /* Pick peaks */
    float ph[HARM_NPEAKS], pa[HARM_NPEAKS];
    int np = pick_peaks(mag, N_BINS, sr, ph, pa, HARM_NPEAKS);
    if (np < 2) return 0.5f;

    /* H1: Helmholtz integer-ratio score ─────────────────────────── */
    float h1 = 0.f;
    int npairs = 0;
    for (int i = 0; i < np; i++) {
        for (int j = 0; j < np; j++) {
            if (i == j) continue;
            float ratio = ph[i] / (ph[j] + 1e-6f);
            float best  = 0.f;
            for (int p = 1; p <= HARM_MAX_PQ; p++) {
                for (int q = 1; q <= HARM_MAX_PQ; q++) {
                    float target = (float)p / (float)q;
                    float d = ratio - target;
                    float s = expf(-(d * d) / HARM_SIGMA2);
                    if (s > best) best = s;
                }
            }
            h1 += best;
            npairs++;
        }
    }
    h1 = (npairs > 0) ? h1 / (float)npairs : 0.f;

    /* H2: Virtual pitch / Terhardt harmonicity ──────────────────── */
    /* Candidate fundamentals: 55 Hz to sr/4, log-spaced */
    float f_lo = 55.f, f_hi = sr * 0.25f;
    float best_h2 = 0.f;
    float h2_sigma2 = 10.f * 10.f;  /* ±10 Hz tolerance per partial */
    for (int fi = 0; fi < HARM_N_FUND; fi++) {
        float t = (float)fi / (float)(HARM_N_FUND - 1);
        float F = f_lo * powf(f_hi / f_lo, t);
        float score = 0.f;
        for (int i = 0; i < np; i++) {
            int k = (int)(ph[i] / F + 0.5f);
            if (k < 1) k = 1;
            float diff = ph[i] - (float)k * F;
            score += pa[i] * expf(-(diff * diff) / h2_sigma2);
        }
        if (score > best_h2) best_h2 = score;
    }
    /* Normalize: perfect alignment on np peaks each with amplitude 1 → np */
    float peak_sum = 0.f;
    for (int i = 0; i < np; i++) peak_sum += pa[i];
    float h2 = (peak_sum > 0.f) ? (best_h2 / peak_sum) : 0.f;
    if (h2 > 1.f) h2 = 1.f;

    /* Combine H1 and H2 — equal weight */
    return 0.5f * h1 + 0.5f * h2;
}

static float compute_roughness(const float *audio, int n, float sr) {
    /* Sethares 1993 roughness model — B = exp(-D) */
    if (n < 64) return 0.5f;

    enum { N_BINS = 32 };
    float mag[N_BINS];
    block_spectrum(audio, (n < 1024 ? n : 1024), sr, mag, N_BINS);

    float ph[HARM_NPEAKS], pa[HARM_NPEAKS];
    int np = pick_peaks(mag, N_BINS, sr, ph, pa, HARM_NPEAKS);
    if (np < 2) return 0.8f;  /* monophonic → no roughness */

    /* Normalize amplitudes */
    float amax = 1e-12f;
    for (int i = 0; i < np; i++) if (pa[i] > amax) amax = pa[i];
    for (int i = 0; i < np; i++) pa[i] /= amax;

    float D = 0.f;
    for (int i = 0; i < np; i++) {
        for (int j = i + 1; j < np; j++) {
            float df  = fabsf(ph[i] - ph[j]);
            /* Only partials within ~1.5 critical bands matter */
            float cbw = 1.72f * powf(0.5f*(ph[i]+ph[j]), 0.65f);  /* Bark CBW */
            if (df > 1.5f * cbw) continue;
            float dij = pa[i] * pa[j] * (expf(-HARM_B1 * df / cbw)
                                        - expf(-HARM_B2 * df / cbw));
            if (dij > 0.f) D += dij;
        }
    }
    float B = expf(-D);
    if (B < 0.f) B = 0.f;
    if (B > 1.f) B = 1.f;
    return B;
}

void evo_feat_extract(const float *audio, int n, float sr, EvoFeat *out) {
    memset(out, 0, sizeof(*out));
    if (!audio || n < EF_FRAME_SZ) return;

    float bark_edges[EF_N_BANDS + 1];
    evo_bark_edges(sr, bark_edges);

    /* Sample EF_N_FRAMES evenly across the audio */
    int stride = (n - EF_FRAME_SZ) / (EF_N_FRAMES - 1);
    if (stride < 1) stride = 1;

    int actual_frames = 0;
    for (int fi = 0; fi < EF_N_FRAMES; fi++) {
        int offset = fi * stride;
        if (offset + EF_FRAME_SZ > n) break;
        frame_energy(audio + offset, EF_FRAME_SZ, sr, bark_edges, &out->frames[fi]);
        actual_frames++;
    }
    if (actual_frames < 2) { out->spectral_div = 0.f; out->self_dissim = 0.f; return; }

    /* 1. SpectralDiversity: mean pairwise L2 between all frame pairs */
    {
        float sum = 0.f; int npairs = 0;
        for (int i = 0; i < actual_frames - 1; i++) {
            for (int j = i + 1; j < actual_frames; j++) {
                sum += frame_l2(&out->frames[i], &out->frames[j]);
                npairs++;
            }
        }
        out->spectral_div = npairs > 0 ? sum / (float)npairs : 0.f;
        /* Clamp to [0,1] — theoretical max of normalised L2 is 1 */
        if (out->spectral_div > 1.f) out->spectral_div = 1.f;
    }

    /* 2. SelfDissimilarity: 1 - mean(off_diag / (diag_i * diag_j)) */
    {
        /* Self-similarity: dot product of normalised frame vectors */
        float diag[EF_N_FRAMES];
        for (int i = 0; i < actual_frames; i++) {
            float d = 0.f;
            for (int b = 0; b < EF_N_BANDS; b++)
                d += out->frames[i].e[b] * out->frames[i].e[b];
            diag[i] = sqrtf(d) + 1e-10f;
        }
        float off_sum = 0.f; int n_off = 0;
        for (int i = 0; i < actual_frames - 1; i++) {
            for (int j = i + 1; j < actual_frames; j++) {
                /* If both frames are silence (near-zero energy), treat as identical */
                if (diag[i] < 0.01f && diag[j] < 0.01f) {
                    off_sum += 1.f; n_off++; continue;
                }
                float dot = 0.f;
                for (int b = 0; b < EF_N_BANDS; b++)
                    dot += out->frames[i].e[b] * out->frames[j].e[b];
                float sim = dot / (diag[i] * diag[j]);
                if (sim < 0.f) sim = 0.f;
                if (sim > 1.f) sim = 1.f;
                off_sum += sim; n_off++;
            }
        }
        float mean_sim = n_off > 0 ? off_sum / (float)n_off : 1.f;
        out->self_dissim = 1.f - mean_sim;
        if (out->self_dissim < 0.f) out->self_dissim = 0.f;
    }

    /* 3. TemporalDynamics: normalised coefficient of variation of block RMS.
     *    CV = std(rms) / mean(rms), clamped to [0,1].
     *    Constant amplitude → CV=0.  Dynamic music → CV>0.
     *    No hardcoded target: any deviation from flat envelope is rewarded.
 */
    {
        float block_rms[EF_N_FRAMES];
        float sum = 0.f;
        for (int fi = 0; fi < actual_frames; fi++) {
            int offset = fi * stride;
            float s = 0.f;
            for (int i = 0; i < EF_FRAME_SZ && offset+i < n; i++)
                s += audio[offset+i] * audio[offset+i];
            block_rms[fi] = sqrtf(s / (float)EF_FRAME_SZ);
            sum += block_rms[fi];
        }
        float mean_rms = actual_frames > 0 ? sum / (float)actual_frames : 0.f;
        if (mean_rms < 1e-10f) { out->temporal_ent = 0.f; return; }
        float var = 0.f;
        for (int fi = 0; fi < actual_frames; fi++) {
            float d = block_rms[fi] - mean_rms; var += d * d;
        }
        float std_rms = sqrtf(var / (float)actual_frames);
        float cv = std_rms / mean_rms;  /* coefficient of variation */
        /* Typical CV for musical audio: 0.1-0.5.
         * Normalise via tanh so values in [0, inf) map to [0, 1). */
        out->temporal_ent = tanhf(cv * 3.f);  /* cv=0.33 → ~0.7 */
        if (out->temporal_ent < 0.f) out->temporal_ent = 0.f;
        if (out->temporal_ent > 1.f) out->temporal_ent = 1.f;
    }

    /* harmonicity + roughness from the full audio buffer */
    out->harmonicity = compute_harmonicity(audio, n, sr);
    out->roughness   = compute_roughness(audio, n, sr);
}

float evo_fitness(const EvoFeat *f, const EvoWeights *w) {
    return w->w_spectral    * f->spectral_div
         + w->w_dissim      * f->self_dissim
         + w->w_temporal    * f->temporal_ent
         + w->w_harmonicity * f->harmonicity   /* Helmholtz+Terhardt */
         + w->w_roughness   * f->roughness;    /* Sethares beauty */
}
