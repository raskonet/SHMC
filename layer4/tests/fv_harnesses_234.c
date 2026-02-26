/*
 * Formal verification harnesses for layers 2, 3, 4.
 */
#include "../../layer2/include/motif.h"
#include "../../layer3/include/section.h"
#include "../../layer4/include/song.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void h_motif_define_bound(void){
    static MotifLibrary lib;
    motif_lib_init(&lib);
    VoiceProgram vp; memset(&vp, 0, sizeof(vp));
    for(int i = 0; i <= MOTIF_LIB_MAX + 1; i++){
        char name[MOTIF_NAME_LEN];
        snprintf(name, sizeof(name), "m%d", i);
        motif_define(&lib, name, &vp);
    }
    if(lib.n > MOTIF_LIB_MAX) __builtin_trap();
}

void h_motif_transpose_clamp(void){
    for(int semitones = -200; semitones <= 200; semitones++){
        VoiceProgram src, dst;
        memset(&src, 0, sizeof(src));
        src.n = 3;
        src.code[0] = VI_PACK(VI_NOTE, 0,   2, 4);
        src.code[1] = VI_PACK(VI_NOTE, 127, 2, 4);
        src.code[2] = VI_PACK(VI_NOTE, 64,  2, 4);
        motif_transpose(&src, &dst, semitones);
        for(int i = 0; i < dst.n; i++){
            if(VI_OP(dst.code[i]) == VI_NOTE || VI_OP(dst.code[i]) == VI_GLIDE){
                int p = (int)VI_PITCH(dst.code[i]);
                if(p < 0 || p > 127) __builtin_trap();
            }
        }
    }
}

void h_motif_compile_overflow(void){
    static MotifLibrary lib;
    motif_lib_init(&lib);
    VoiceProgram vp; memset(&vp, 0, sizeof(vp));
    vp.n = 2;
    vp.code[0] = VI_PACK(VI_NOTE, 60, 2, 4);

    motif_define(&lib, "m", &vp);
    MotifUse use = motif_use("m", 0.f, 10000, 0);
    use.time_scale = 1.0f; use.vel_scale = 1.0f;
    static EventStream out;
    char err[128];
    motif_compile_uses(&lib, &use, 1, &out, 44100.f, 120.f, err, sizeof(err));
    if(out.n > VOICE_MAX_EVENTS) __builtin_trap();
}

void h_section_bounds(void){
    static Section s;
    section_init(&s, "test", 16.f);
    static PatchProgram patch; memset(&patch, 0, sizeof(patch));
    MotifUse use = motif_use("m", 0.f, 1, 0);
    use.time_scale = 1.0f; use.vel_scale = 1.0f;
    for(int i = 0; i <= SECTION_MAX_TRACKS + 2; i++){
        char nm[SECTION_NAME_LEN];
        snprintf(nm, sizeof(nm), "t%d", i);
        section_add_track(&s, nm, &patch, &use, 1, 0.8f, 0.f);
    }
    if(s.n_tracks > SECTION_MAX_TRACKS) __builtin_trap();
}

void h_section_renderer_null_es(void){
    static SectionRenderer sr;
    memset(&sr, 0, sizeof(sr));
    sr.n_tracks = SECTION_MAX_TRACKS;
    for(int i = 0; i < SECTION_MAX_TRACKS; i++)
        sr.tracks[i].es = NULL;
    section_renderer_destroy(&sr);
}

void h_song_append_repeat(void){
    static Song song;
    song_init(&song, "test", 120.f, 44100.f);
    static Section sec; section_init(&sec, "s", 4.f);
    static MotifLibrary lib; motif_lib_init(&lib);
    song_append(&song, "e", &sec, &lib, 0, 0.f, 0.f, 0.f, 0.f);
    if(song.n_entries > 0 && song.entries[0].repeat < 1) __builtin_trap();
}

void h_song_bpm_monotone(void){
    static Song song;
    song_init(&song, "test", 120.f, 44100.f);
    song_add_bpm(&song, 4.f, 120.f);
    int r1 = song_add_bpm(&song, 4.f, 160.f);  /* same beat — must fail */
    int r2 = song_add_bpm(&song, 2.f, 130.f);  /* earlier beat — must fail */
    if(r1 == 0 || r2 == 0) __builtin_trap();
}

void h_song_overflow(void){
    static Song song;
    song_init(&song, "test", 120.f, 44100.f);
    static Section sec; section_init(&sec, "s", 4.f);
    static MotifLibrary lib; motif_lib_init(&lib);
    for(int i = 0; i <= SONG_MAX_ENTRIES + 1; i++)
        song_append(&song, "e", &sec, &lib, 1, 0.f, 0.f, 0.f, 0.f);
    if(song.n_entries > SONG_MAX_ENTRIES) __builtin_trap();
    for(int i = 0; i <= SONG_MAX_BPM_POINTS + 1; i++)
        song_add_bpm(&song, (float)i * 4.1f + 0.1f, 120.f + (float)i);
    if(song.n_bpm_points > SONG_MAX_BPM_POINTS) __builtin_trap();
}
