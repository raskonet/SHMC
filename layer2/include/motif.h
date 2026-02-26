#pragma once
/*
 * SHMC Layer 2 — Motif DSL  (rev 2)
 *
 * A Motif is a named, reusable VoiceProgram fragment.
 * MotifLibrary stores up to MOTIF_LIB_MAX named motifs.
 *
 * IMPORTANT — Stack size:
 *   sizeof(MotifLibrary) ≈ 1 MB.  Declare as static or allocate on the heap.
 *   Never declare MotifLibrary as a local variable inside a function.
 *
 * Fixes vs rev 1:
 *   L2-2: ev_cmp tiebreak was backwards — NOTE_ON sorted before NOTE_OFF at the
 *          same sample, causing a click/pop on every motif repeat boundary.
 *          Fixed: NOTE_OFF now correctly precedes NOTE_ON at identical timestamps.
 */

#include "../../layer1/include/voice.h"
#include <string.h>  /* for memset in motif_use() */

#define MOTIF_NAME_LEN  32
#define MOTIF_LIB_MAX   64

/* ---- A named motif ---- */
typedef struct Motif_s {
    char         name[MOTIF_NAME_LEN];
    VoiceProgram vp;
    int          valid;
} Motif;

/*
 * MotifLibrary — ≈ 1 MB.
 * Must be declared static or heap-allocated, never as a plain local:
 *   static MotifLibrary lib;            // OK: BSS/data segment
 *   MotifLibrary *lib = malloc(sizeof); // OK: heap
 *   MotifLibrary lib;                   // DANGER: stack overflow
 */
typedef struct {
    Motif entries[MOTIF_LIB_MAX];
    int   n;
} MotifLibrary;

/* Forward declaration for resolved_motif pointer */
typedef struct Motif_s Motif;

/* ---- One scheduled use of a motif ---- */
typedef struct {
    char          name[MOTIF_NAME_LEN];
    float         start_beat;       /* beat offset within section */
    int           repeat;           /* number of times to repeat */
    int           transpose;        /* semitone shift, -127..+127 */

    /* N5-2: Transform algebra — applied at compile time */
    float time_scale;    /* duration multiplier: 1.0=normal, 2.0=double len,
                            0.5=half speed. Applied to every note duration.
                            0.0 or negative → treated as 1.0 */
    float vel_scale;     /* velocity multiplier: 1.0=normal, 0.5=half vel.
                            Clamped to [0.0, 2.0] then to [0.0, 1.0] per note */

    /* R2-4: pre-resolved at section build time — render path uses this,
       never does a string lookup during playback. NULL until resolved. */
    const Motif  *resolved_motif;
} MotifUse;

/* Convenience initializer for MotifUse with default transforms */
static inline MotifUse motif_use(const char *name, float start_beat,
                                 int repeat, int transpose){
    MotifUse u;
    memset(&u, 0, sizeof(u));
    unsigned i=0;
    while(i < MOTIF_NAME_LEN-1 && name[i]){ u.name[i]=name[i]; i++; }
    u.name[i]='\0';
    u.start_beat = start_beat;
    u.repeat     = repeat;
    u.transpose  = transpose;
    u.time_scale = 1.0f;   /* identity */
    u.vel_scale  = 1.0f;   /* identity */
    u.resolved_motif = NULL;
    return u;
}

#ifdef __cplusplus
extern "C" {
#endif

void         motif_lib_init(MotifLibrary *lib);
int          motif_define(MotifLibrary *lib, const char *name, const VoiceProgram *vp);
const Motif *motif_find(const MotifLibrary *lib, const char *name);
void         motif_transpose(const VoiceProgram *src, VoiceProgram *dst, int semitones);

/*
 * R2-4: Pre-resolve name strings in an array of MotifUse entries.
 * Sets uses[i].resolved_motif = motif_find(lib, uses[i].name).
 * Returns 0 if all resolved, -1 if any name is not found (sets err).
 * Call once at section build time; render path then uses resolved_motif.
 */
int motif_resolve_uses(const MotifLibrary *lib,
                       MotifUse *uses, int n_uses,
                       char *err, int err_sz);

/*
 * Compile a list of MotifUse entries into a single EventStream.
 * Events from all uses are merged and sorted by sample position.
 * Tiebreak: NOTE_OFF before NOTE_ON at identical sample (prevents retrigger click).
 * Returns 0 on success, -1 on error.
 */
int motif_compile_uses(const MotifLibrary *lib,
                       const MotifUse *uses, int n_uses,
                       EventStream *out,
                       float sr, float bpm,
                       char *err, int err_sz);

#ifdef __cplusplus
}
#endif
