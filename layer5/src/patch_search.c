/*
 * SHMC Layer 5 — Patch Search Engine
 *
 * Beam search over the discrete PatchProgram space to find patches
 * whose rendered audio matches a target audio clip's features.
 */
#include "../include/patch_search.h"
#include "../../layer0b/include/patch_meta.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* ================================================================
   RNG  (xorshift32)
   ================================================================ */
static inline uint32_t rng_next(uint32_t *s){
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}
static inline float rng_f01(uint32_t *s){
    return (float)(rng_next(s) >> 1) * (1.f / 2147483648.f);
}
static inline int rng_range(uint32_t *s, int lo, int hi){
    if(hi <= lo) return lo;
    return lo + (int)(rng_next(s) % (uint32_t)(hi - lo + 1));
}

/* ================================================================
   Feature extraction  (FFT-free)
   ================================================================ */

/*
 * Spectral centroid via Goertzel DFT at 4 frequencies.
 * Returned value is a Hz-scale centroid estimate.
 */
static float approx_centroid(const float *win, int n, float sr){
    /* Fix: apply Hann window before DFT bins to kill spectral leakage */
    static const float FREQS[4] = {220.f, 880.f, 3520.f, 7040.f};
    float mag[4], total = 0.f;
    float norm = (n > 1) ? (float)(n - 1) : 1.f;
    for(int b = 0; b < 4; b++){
        double w  = 2.0 * 3.14159265358979 * FREQS[b] / sr;
        double re = 0.0, im = 0.0;
        for(int i = 0; i < n; i++){
            /* Hann window: 0.5*(1 - cos(2*pi*i/(n-1))) */
            double hann = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / norm));
            double s    = (double)win[i] * hann;
            re += s * cos(w * i);
            im += s * sin(w * i);
        }
        mag[b] = (float)sqrt(re*re + im*im) + 1e-12f;
        total += mag[b];
    }
    float c = 0.f;
    for(int b = 0; b < 4; b++) c += FREQS[b] * (mag[b] / total);
    return c;
}

void feat_extract(const float *audio, int n, FeatureVec *out){
    memset(out, 0, sizeof(*out));
    int len = (n < FEAT_TOTAL_LEN) ? n : FEAT_TOTAL_LEN;

    float prev_centroid = 0.f;
    float flux_sum      = 0.f;
    float max_rms_jump  = 0.f;
    float prev_rms      = 0.f;

    for(int f = 0; f < FEAT_FRAMES; f++){
        int   off   = f * FEAT_WINDOW_SZ;
        int   avail = 0;
        if(off < len) avail = (off + FEAT_WINDOW_SZ <= len)
                              ? FEAT_WINDOW_SZ : (len - off);
        const float *win = audio + off;

        /* RMS */
        double ss = 0.0;
        for(int i = 0; i < avail; i++) ss += (double)win[i] * (double)win[i];
        out->rms[f] = (avail > 0) ? (float)sqrt(ss / avail) : 0.f;

        /* ZCR */
        int zc = 0;
        for(int i = 1; i < avail; i++)
            if((win[i-1] < 0.f) != (win[i] < 0.f)) zc++;
        out->zcr[f] = (avail > 1) ? (float)zc / (float)(avail - 1) : 0.f;

        /* Spectral centroid */
        out->centroid[f] = (avail >= 4)
            ? approx_centroid(win, avail, SEARCH_SR) : 0.f;

        /* Flux: |centroid change| */
        float dc = fabsf(out->centroid[f] - prev_centroid);
        flux_sum += dc;
        prev_centroid = out->centroid[f];

        /* Onset: max positive RMS jump across any frame boundary */
        float jump = out->rms[f] - prev_rms;
        if(jump > max_rms_jump) max_rms_jump = jump;
        prev_rms = out->rms[f];
    }

    out->flux  = (FEAT_FRAMES > 1) ? flux_sum / (float)(FEAT_FRAMES - 1) : 0.f;
    out->onset = max_rms_jump;
}

/* ================================================================
   Fitness:  0.0 = no match,  1.0 = perfect
   ================================================================ */

/* L2 distance squared, averaged per element */
static float vec_msd(const float *a, const float *b, int n){
    double s = 0.0;
    for(int i = 0; i < n; i++){
        double d = (double)a[i] - (double)b[i];
        s += d * d;
    }
    return (float)(s / (double)n);
}

float feat_fitness(const FitnessCtx *ctx, const FeatureVec *cand){
    const FeatureVec *t = &ctx->target_feat;

    /* Convert MSD to similarity via exp(-k * distance) */
    float s_rms = expf(-8.f  * vec_msd(t->rms,      cand->rms,      FEAT_FRAMES));
    float s_zcr = expf(-16.f * vec_msd(t->zcr,       cand->zcr,      FEAT_FRAMES));

    /* Fix: centroid in Hz — take sqrt(msd) first, then normalize.
     * Old: exp(-msd/8000^2) ≈ exp(-1e-7) ≈ 1.0 always (useless).
     * New: exp(-sqrt(msd)/2000) gives ~0 for 6000 Hz difference, ~1 for 0 Hz. */
    float s_cen = expf(-sqrtf(vec_msd(t->centroid, cand->centroid, FEAT_FRAMES)) / 2000.f);

    float d_flux = fabsf(t->flux  - cand->flux);
    float d_ons  = fabsf(t->onset - cand->onset);
    /* Fix: flux is Hz/frame, typical values ~20-200.
     * Old divisor 501 gave exp(-50/501)~0.90 for all candidates (no gradient).
     * New: exp(-d/50) gives 0.37 at 50 Hz/frame, 0.02 at 200 — strong signal. */
    float s_flux = expf(-d_flux / 50.f);
    float s_ons  = expf(-16.f * d_ons);

    float score = ctx->w_rms      * s_rms
                + ctx->w_centroid * s_cen
                + ctx->w_zcr      * s_zcr
                + ctx->w_flux     * s_flux
                + ctx->w_onset    * s_ons;

    if(score > 1.f) score = 1.f;
    if(score < 0.f) score = 0.f;
    return score;
}

void fitness_ctx_init(FitnessCtx *ctx,
                      const float *audio, int n,
                      int midi, float sr){
    memset(ctx, 0, sizeof(*ctx));
    ctx->sr   = sr;
    ctx->midi = midi;

    ctx->w_rms      = 0.40f;
    ctx->w_centroid = 0.25f;
    ctx->w_zcr      = 0.15f;
    ctx->w_flux     = 0.10f;
    ctx->w_onset    = 0.10f;

    /* Copy target audio, normalise to peak = 1 */
    int len = (n < FEAT_TOTAL_LEN) ? n : FEAT_TOTAL_LEN;
    float peak = 1e-12f;
    for(int i = 0; i < len; i++){
        float a = fabsf(audio[i]);
        if(a > peak) peak = a;
    }
    for(int i = 0; i < len; i++)
        ctx->target_audio[i] = audio[i] / peak;
    /* zero-pad */
    for(int i = len; i < FEAT_TOTAL_LEN; i++)
        ctx->target_audio[i] = 0.f;

    feat_extract(ctx->target_audio, FEAT_TOTAL_LEN, &ctx->target_feat);
}

/*
 * T4-7: fitness_score with shaping penalties.
 *
 * Penalties applied BEFORE feature similarity:
 *   - Silence / near-silence: score = 0
 *   - DC bias: multiplicative penalty (DC > 10% of peak)
 *   - Instability (NaN/Inf in output): score = 0
 *   - CPU cost: light penalty for patches with est_cpu > 32
 *
 * T4-8: Structural viability check via patch_meta.
 *   Non-viable patches (no oscillator, no envelope, or unstable) skip rendering.
 */
float fitness_score(const FitnessCtx *ctx, const PatchProgram *prog){
    /* T4-8: Structural viability — fast reject without rendering */
    {
        extern void patch_meta(const PatchProgram *, PatchMeta *);
        PatchMeta pm;
        patch_meta(prog, &pm);
        if(!pmeta_search_viable(&pm)){
            /* No oscillator or no envelope → guaranteed useless */
            if(!pm.has_oscillator || !pm.has_envelope) return 0.f;
            /* Unstable but otherwise valid: small penalty, still evaluate */
        }
        /* T4-7: CPU cost penalty */
        /* Fix: threshold raised 64→96; 0.01 → 0.0f so search
         * can distinguish true silence from expensive-but-rendered patches */
        if(pm.est_cpu > 96) return 0.f; /* truly too expensive: skip */
    }

    /* Render FEAT_TOTAL_LEN samples */
    Patch p;
    patch_note_on(&p, prog, ctx->sr, ctx->midi, 0.85f);

    /* Fix: was static (race-condition risk if parallelized).
     * Stack allocation is safe and GCC will optimize this fine. */
    float buf[FEAT_TOTAL_LEN];
    patch_step(&p, buf, FEAT_TOTAL_LEN);

    /* T4-7a: NaN/Inf check (instability) */
    for(int i = 0; i < FEAT_TOTAL_LEN; i++){
        if(!isfinite(buf[i])) return 0.f;
    }

    /* T4-7b: Silence penalty */
    float peak = 1e-12f;
    for(int i = 0; i < FEAT_TOTAL_LEN; i++){
        float a = fabsf(buf[i]);
        if(a > peak) peak = a;
    }
    if(peak < 1e-6f) return 0.f;   /* silence = useless */

    /* T4-7c: DC bias penalty — compute mean of normalised output */
    double dc_sum = 0.0;
    for(int i = 0; i < FEAT_TOTAL_LEN; i++) dc_sum += (double)buf[i];
    float dc_ratio = (float)(fabs(dc_sum) / (FEAT_TOTAL_LEN * peak));
    float dc_penalty = 1.f;
    if(dc_ratio > 0.1f){
        /* Smooth penalty: exp(-k * excess_DC) */
        dc_penalty = expf(-8.f * (dc_ratio - 0.1f));
        if(dc_penalty < 0.05f) dc_penalty = 0.05f;
    }

    /* Normalise for feature extraction */
    for(int i = 0; i < FEAT_TOTAL_LEN; i++) buf[i] /= peak;

    FeatureVec fv;
    feat_extract(buf, FEAT_TOTAL_LEN, &fv);

    float base = feat_fitness(ctx, &fv);
    return base * dc_penalty;
}

/* ================================================================
   Random patch generation using pb_* API
   ================================================================ */

/*
 * Oscillator instruction generator.
 * Returns the output register of the generated oscillator.
 */
static int emit_random_osc(PatchBuilder *b, uint32_t *rng){
    /* Fix: old distribution had 2/7 noise cases (~29%).
     * New: 6 tonal + 1 noise = ~14% noise, 86% pitched — much better for harmony search.
     * cases 0-4: tonal oscillators (sin/saw/sqr/tri/FM)
     * case 5:    FM variant with deeper modulation
     * case 6:    noise (only 1 slot, previously 2) */
    int kind = rng_range(rng, 0, 6);
    switch(kind){
    case 0: return pb_osc(b, REG_ONE);
    case 1: return pb_saw(b, REG_ONE);
    case 2: return pb_square(b, REG_ONE);
    case 3: return pb_tri(b, REG_ONE);
    case 4: {
        /* FM: carrier + sine modulator, moderate depth */
        int mod = pb_osc(b, REG_ONE);
        int di  = rng_range(rng, 0, 20);  /* shallower depth — more musical */
        return pb_fm(b, REG_ONE, mod, di);
    }
    case 5: {
        /* FM with deeper modulation — bright/metallic tonal */
        int mod = pb_tri(b, REG_ONE);
        int di  = rng_range(rng, 8, 31);
        return pb_fm(b, REG_ONE, mod, di);
    }
    case 6: return pb_noise(b);  /* noise: only 1/7 slots now */
    default: return pb_osc(b, REG_ONE);
    }
}

/*
 * Emit a random modifier (filter / nonlinearity) that takes `r_in`
 * and returns a new register.
 */
static int emit_random_mod(PatchBuilder *b, uint32_t *rng, int r_in){
    int kind = rng_range(rng, 0, 5);
    int ci, qi;
    switch(kind){
    case 0:  ci = rng_range(rng, 0, 63); return pb_lpf(b, r_in, ci);
    case 1:  ci = rng_range(rng, 0, 63); return pb_hpf(b, r_in, ci);
    case 2:  ci = rng_range(rng, 16, 55); qi = rng_range(rng, 0, 31);
             return pb_bpf(b, r_in, ci, qi);
    case 3:  return pb_tanh(b, r_in);
    case 4:  return pb_fold(b, r_in);
    default: return pb_clip(b, r_in);
    }
}

PatchProgram patch_random(uint32_t *rng, int n_instrs){
    if(n_instrs < SEARCH_MIN_INSTRS) n_instrs = SEARCH_MIN_INSTRS;
    if(n_instrs > SEARCH_MAX_INSTRS) n_instrs = SEARCH_MAX_INSTRS;

    PatchBuilder b; pb_init(&b);

    /* 1. Core oscillator */
    int r_sig = emit_random_osc(&b, rng);

    /* 2. Zero or more modifier stages (each uses previous output as input) */
    int body = n_instrs - 3;   /* reserve slots: adsr, mul, out */
    if(body < 0) body = 0;
    for(int i = 0; i < body && b.ok == 0; i++)
        r_sig = emit_random_mod(&b, rng, r_sig);

    /* 3. ADSR envelope */
    int att = rng_range(rng, 0, 8);
    int dec = rng_range(rng, 4, 20);
    int sus = rng_range(rng, 10, 28);
    int rel = rng_range(rng, 5, 22);
    int r_env = pb_adsr(&b, att, dec, sus, rel);

    /* 4. Apply envelope */
    int r_out = pb_mul(&b, r_sig, r_env);

    /* 5. Output */
    pb_out(&b, r_out);

    /* If builder overflowed, return minimal valid patch */
    if(b.ok != 0){
        pb_init(&b);
        int r = pb_osc(&b, REG_ONE);
        int e = pb_adsr(&b, 0, 8, 20, 8);
        pb_out(&b, pb_mul(&b, r, e));
    }

    return *pb_finish(&b);
}

/* ================================================================
   Mutation — returns a modified copy of src
   ================================================================ */

/*
 * Identify instruction class for safe swap/replace decisions.
 */
typedef enum {
    ICLASS_OSC, ICLASS_MOD, ICLASS_ADSR, ICLASS_ARITH, ICLASS_OUT, ICLASS_OTHER
} InstrClass;

static InstrClass classify(uint8_t op){
    switch(op){
    case OP_OSC: case OP_SAW: case OP_SQUARE: case OP_TRI:
    case OP_FM:  case OP_PM:  case OP_NOISE:  case OP_LP_NOISE:
    case OP_RAND_STEP:
        return ICLASS_OSC;
    case OP_LPF: case OP_HPF: case OP_BPF: case OP_ONEPOLE:
    case OP_TANH: case OP_CLIP: case OP_FOLD: case OP_AM:
        return ICLASS_MOD;
    case OP_ADSR: case OP_RAMP: case OP_EXP_DECAY:
        return ICLASS_ADSR;
    case OP_MUL: case OP_ADD: case OP_SUB: case OP_MIXN:
        return ICLASS_ARITH;
    case OP_OUT:
        return ICLASS_OUT;
    default:
        return ICLASS_OTHER;
    }
}

PatchProgram patch_mutate(const PatchProgram *src, uint32_t *rng){
    PatchProgram dst = *src;
    int n = dst.n_instrs;
    if(n < 1) return dst;

    /* Pick mutation type */
    int mutation = rng_range(rng, 0, 5);

    switch(mutation){

    case 0: {
        /* Perturb an ADSR immediate field by ±1 step */
        for(int i = 0; i < n; i++){
            if(INSTR_OP(dst.code[i]) == OP_ADSR){
                uint16_t hi = INSTR_IMM_HI(dst.code[i]);
                uint16_t lo = INSTR_IMM_LO(dst.code[i]);
                int field = rng_range(rng, 0, 3);
                int delta = (rng_next(rng) & 1) ? 1 : -1;
                /* ADSR encoding: hi = [att:5][dec:5][sus:5], lo = [rel:5][...] */
                switch(field){
                case 0: { int v = (int)((hi >> 10) & 0x1F) + delta;
                          v = v < 0 ? 0 : v > 31 ? 31 : v;
                          hi = (uint16_t)((hi & ~(0x1F << 10)) | (v << 10)); break; }
                case 1: { int v = (int)((hi >>  5) & 0x1F) + delta;
                          v = v < 0 ? 0 : v > 31 ? 31 : v;
                          hi = (uint16_t)((hi & ~(0x1F << 5)) | (v << 5)); break; }
                case 2: { int v = (int)(hi & 0x1F) + delta;
                          v = v < 0 ? 0 : v > 31 ? 31 : v;
                          hi = (uint16_t)((hi & ~0x1F) | v); break; }
                case 3: { int v = (int)((lo >> 11) & 0x1F) + delta;
                          v = v < 0 ? 0 : v > 31 ? 31 : v;
                          lo = (uint16_t)((lo & ~(0x1F << 11)) | (v << 11)); break; }
                }
                dst.code[i] = INSTR_PACK(OP_ADSR,
                    INSTR_DST(dst.code[i]), 0, 0, hi, lo);
                break;
            }
        }
        break;
    }

    case 1: {
        /* Perturb a filter cutoff index by ±1..±3 steps */
        for(int i = n - 1; i >= 0; i--){
            uint8_t op = INSTR_OP(dst.code[i]);
            if(op == OP_LPF || op == OP_HPF || op == OP_BPF || op == OP_ONEPOLE){
                uint16_t hi = INSTR_IMM_HI(dst.code[i]);
                int delta = rng_range(rng, -3, 3);
                int v = (int)hi + delta;
                /* LPF/HPF use 0..63 range */
                v = v < 0 ? 0 : v > 63 ? 63 : v;
                dst.code[i] = INSTR_PACK(op,
                    INSTR_DST(dst.code[i]),
                    INSTR_SRC_A(dst.code[i]),
                    INSTR_SRC_B(dst.code[i]),
                    (uint16_t)v, INSTR_IMM_LO(dst.code[i]));
                break;
            }
        }
        break;
    }

    case 2: {
        /* Replace oscillator with a different type, preserving output register */
        for(int i = 0; i < n; i++){
            if(classify(INSTR_OP(dst.code[i])) == ICLASS_OSC){
                uint8_t dst_reg = INSTR_DST(dst.code[i]);
                int kind = rng_range(rng, 0, 4);
                Opcode new_op;
                switch(kind){
                case 0: new_op = OP_OSC;    break;
                case 1: new_op = OP_SAW;    break;
                case 2: new_op = OP_SQUARE; break;
                case 3: new_op = OP_TRI;    break;
                default:new_op = OP_NOISE;  break;
                }
                /* Simple non-FM oscillators take REG_ONE as freq multiplier */
                if(new_op != OP_FM && new_op != OP_PM)
                    dst.code[i] = INSTR_PACK(new_op, dst_reg, REG_ONE, 0, 0, 0);
                /* FM stays as-is to avoid register invalidation */
                break;
            }
        }
        break;
    }

    case 3: {
        /* Replace a modifier (filter/nonlinearity) with a different one */
        for(int i = n - 1; i >= 0; i--){
            uint8_t op = INSTR_OP(dst.code[i]);
            if(classify(op) == ICLASS_MOD){
                uint8_t dr = INSTR_DST(dst.code[i]);
                uint8_t sa = INSTR_SRC_A(dst.code[i]);
                int kind = rng_range(rng, 0, 4);
                int ci = rng_range(rng, 16, 48);
                switch(kind){
                case 0: dst.code[i] = INSTR_PACK(OP_LPF,  dr, sa, 0, (uint16_t)ci, 0); break;
                case 1: dst.code[i] = INSTR_PACK(OP_HPF,  dr, sa, 0, (uint16_t)ci, 0); break;
                case 2: dst.code[i] = INSTR_PACK(OP_TANH, dr, sa, 0, 0, 0);            break;
                case 3: dst.code[i] = INSTR_PACK(OP_FOLD, dr, sa, 0, 0, 0);            break;
                default:dst.code[i] = INSTR_PACK(OP_CLIP, dr, sa, 0, 0, 0);            break;
                }
                break;
            }
        }
        break;
    }

    case 4: {
        /*
         * Insert a filter instruction before OP_OUT (if room).
         * Find the penultimate instruction (the MUL before OP_OUT) and
         * insert a filter that takes its output register as input.
 */
        if(n < MAX_INSTRS - 1 && n >= 2){
            /* Find OP_OUT position */
            int out_pos = n - 1;
            while(out_pos > 0 && INSTR_OP(dst.code[out_pos]) != OP_OUT)
                out_pos--;
            if(out_pos > 0 && out_pos < MAX_INSTRS - 1){
                /* Register feeding OUT */
                uint8_t r_feed = INSTR_SRC_A(dst.code[out_pos]);
                /* Allocate a new register (use n_regs) */
                if(dst.n_regs < MAX_REGS - 1){
                    uint8_t r_new = (uint8_t)dst.n_regs++;
                    int ci = rng_range(rng, 16, 50);
                    Instr new_ins = INSTR_PACK(OP_LPF, r_new, r_feed, 0, (uint16_t)ci, 0);
                    /* Shift instructions from out_pos onward */
                    for(int k = n; k > out_pos; k--)
                        dst.code[k] = dst.code[k-1];
                    dst.code[out_pos] = new_ins;
                    /* Rewire OUT to read from r_new */
                    dst.code[out_pos + 1] = INSTR_PACK(OP_OUT, 0, r_new, 0, 0, 0);
                    dst.n_instrs++;
                }
            }
        }
        break;
    }

    case 5: {
        /*
         * Delete a modifier instruction if there's more than SEARCH_MIN_INSTRS.
         * Find a modifier and remove it, patching up the register chain.
 */
        if(n > SEARCH_MIN_INSTRS + 1){
            for(int i = 1; i < n - 2; i++){
                if(classify(INSTR_OP(dst.code[i])) == ICLASS_MOD){
                    uint8_t removed_out  = INSTR_DST(dst.code[i]);
                    uint8_t removed_in   = INSTR_SRC_A(dst.code[i]);
                    /* Patch next instruction to read from removed_in instead */
                    for(int k = i + 1; k < n; k++){
                        uint8_t op  = INSTR_OP(dst.code[k]);
                        uint8_t dr  = INSTR_DST(dst.code[k]);
                        uint8_t sa  = INSTR_SRC_A(dst.code[k]);
                        uint8_t sb  = INSTR_SRC_B(dst.code[k]);
                        if(sa == removed_out) sa = removed_in;
                        if(sb == removed_out) sb = removed_in;
                        dst.code[k] = INSTR_PACK(op, dr, sa, sb,
                            INSTR_IMM_HI(dst.code[k]),
                            INSTR_IMM_LO(dst.code[k]));
                    }
                    /* Shift instructions down */
                    for(int k = i; k < n - 1; k++)
                        dst.code[k] = dst.code[k+1];
                    dst.n_instrs--;
                    break;
                }
            }
        }
        break;
    }
    }

    return dst;
}

/* ================================================================
   Beam search
   ================================================================ */

static int cand_cmp_desc(const void *a, const void *b){
    float fa = ((const Candidate *)a)->fitness;
    float fb = ((const Candidate *)b)->fitness;
    if(fb > fa) return  1;
    if(fb < fa) return -1;
    return 0;
}

void patch_search(const FitnessCtx *ctx,
                  uint32_t seed,
                  SearchResult *result,
                  SearchProgressFn progress_cb,
                  void *userdata){
    tables_init();

    uint32_t rng = seed ? seed : 0x5EED1234u;

    int pool_cap = SEARCH_BEAM_SIZE * (1 + SEARCH_MUTATIONS);
    Candidate *beam = (Candidate *)malloc((size_t)SEARCH_BEAM_SIZE * sizeof(Candidate));
    Candidate *pool = (Candidate *)malloc((size_t)pool_cap         * sizeof(Candidate));
    if(!beam || !pool){ free(beam); free(pool); memset(result, 0, sizeof(*result)); return; }

    clock_t t0 = clock();
    int n_evals = 0;

    /* ---- Seed beam with random patches ---- */
    for(int i = 0; i < SEARCH_BEAM_SIZE; i++){
        int ni = rng_range(&rng, SEARCH_MIN_INSTRS, SEARCH_MAX_INSTRS);
        beam[i].prog    = patch_random(&rng, ni);
        beam[i].fitness = fitness_score(ctx, &beam[i].prog);
        n_evals++;
    }
    qsort(beam, (size_t)SEARCH_BEAM_SIZE, sizeof(Candidate), cand_cmp_desc);

    int gen;
    for(gen = 0; gen < SEARCH_MAX_GEN; gen++){
        if(beam[0].fitness >= SEARCH_FITNESS_THRESH) break;

        /* ---- Random restart: replace bottom half with fresh randoms ---- */
        if(gen > 0 && gen % SEARCH_RESTART_EVERY == 0){
            int half = SEARCH_BEAM_SIZE / 2;
            for(int i = half; i < SEARCH_BEAM_SIZE; i++){
                int ni = rng_range(&rng, SEARCH_MIN_INSTRS, SEARCH_MAX_INSTRS);
                beam[i].prog    = patch_random(&rng, ni);
                beam[i].fitness = fitness_score(ctx, &beam[i].prog);
                n_evals++;
            }
        }

        /* ---- Build pool: beam members + mutations ---- */
        int pool_sz = 0;
        for(int i = 0; i < SEARCH_BEAM_SIZE; i++){
            pool[pool_sz++] = beam[i];
            for(int m = 0; m < SEARCH_MUTATIONS; m++){
                Candidate c;
                c.prog    = patch_mutate(&beam[i].prog, &rng);
                c.fitness = fitness_score(ctx, &c.prog);
                pool[pool_sz++] = c;
                n_evals++;
            }
        }

        /* ---- Select top-K from pool ---- */
        qsort(pool, (size_t)pool_sz, sizeof(Candidate), cand_cmp_desc);
        int keep = pool_sz < SEARCH_BEAM_SIZE ? pool_sz : SEARCH_BEAM_SIZE;
        memcpy(beam, pool, (size_t)keep * sizeof(Candidate));

        if(progress_cb && progress_cb(gen, beam[0].fitness, userdata) != 0) break;
    }

    clock_t t1 = clock();

    result->best          = beam[0];
    result->n_generations = gen;
    result->n_evaluations = n_evals;
    result->time_seconds  = (double)(t1 - t0) / CLOCKS_PER_SEC;

    free(beam);
    free(pool);
}
