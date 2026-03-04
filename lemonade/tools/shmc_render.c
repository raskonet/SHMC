/*
 * shmc_render — CLI renderer for SHMC DSL text files
 *
 * Usage:  shmc_render <input.shmc> <output.wav> [--sr 44100] [--gain 0.9]
 *
 * Reads a SHMC DSL text file, compiles it, renders the first SONG to a
 * 16-bit mono WAV.  Exit 0 on success, 1 on any error.
 *
 * This is the binary called by lemonade_bridge.py after the LLM produces
 * a .shmc file.
 */
#include "../include/shmc_dsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── simple file reader ─────────────────────────────────────────── */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "shmc_render: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    const char *in_path  = NULL;
    const char *out_path = NULL;
    float       sr       = 44100.f;
    float       gain     = 0.35f;  /* conservative: multi-track mixes sum hot */
    int         normalize = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sr") == 0 && i+1 < argc) {
            sr = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--gain") == 0 && i+1 < argc) {
            gain = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--normalize") == 0) {
            normalize = 1;
        } else if (!in_path) {
            in_path = argv[i];
        } else if (!out_path) {
            out_path = argv[i];
        }
    }

    if (!in_path || !out_path) {
        fprintf(stderr,
            "Usage: shmc_render <input.shmc> <output.wav> [--sr 44100] [--gain 0.9]\n");
        return 1;
    }

    /* Read source */
    char *src = read_file(in_path);
    if (!src) return 1;

    /* Compile */
    ShmcWorld world;
    char err[DSL_ERR_SZ];
    fprintf(stderr, "[shmc_render] Compiling %s …\n", in_path);
    int rc = shmc_dsl_compile(src, &world, err, sizeof(err));
    free(src);
    if (rc != 0) {
        fprintf(stderr, "[shmc_render] COMPILE ERROR: %s\n", err);
        return 1;
    }
    fprintf(stderr, "[shmc_render] Compiled: %d patch(es), %d section(s), %d song(s)\n",
            world.n_patches, world.n_sections, world.n_songs);

    /* Render */
    float *buf = NULL;
    int    n_frames = 0;
    fprintf(stderr, "[shmc_render] Rendering at %.0f Hz …\n", sr);
    rc = shmc_world_render(&world, &buf, &n_frames, sr);
    if (rc != 0 || !buf) {
        fprintf(stderr, "[shmc_render] RENDER ERROR\n");
        shmc_world_free(&world);
        return 1;
    }
    fprintf(stderr, "[shmc_render] Rendered %d frames (%.2f s)\n",
            n_frames, (float)n_frames / sr);

    /* Apply gain; optionally normalize to 0 dBFS (-0.1 dB headroom) */
    if (normalize) {
        float peak = 0.f;
        for (int i = 0; i < n_frames; i++)
            if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
        if (peak > 1e-6f) gain = 0.891f / peak;  /* -1 dBFS */
    }
    for (int i = 0; i < n_frames; i++) {
        buf[i] *= gain;
        if (buf[i] >  1.f) buf[i] =  1.f;
        if (buf[i] < -1.f) buf[i] = -1.f;
    }

    /* Write WAV (mono) */
    rc = shmc_write_wav(out_path, buf, n_frames, 1, sr);
    free(buf);
    shmc_world_free(&world);

    if (rc != 0) {
        fprintf(stderr, "[shmc_render] WAV WRITE ERROR: %s\n", out_path);
        return 1;
    }
    fprintf(stderr, "[shmc_render] Written: %s\n", out_path);
    return 0;
}
