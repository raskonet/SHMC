/*
 * SHMC Layer 4 — Song DSL Test
 *
 * Build from synth/ root:
 *   gcc -O2 \
 *     -Ilayer0/include -Ilayer1/include -Ilayer2/include \
 *     -Ilayer3/include -Ilayer4/include \
 *     layer4/tests/test_layer4.c \
 *     layer4/src/song.c \
 *     layer3/src/section.c layer2/src/motif.c \
 *     layer1/src/voice.c \
 *     layer0/src/patch_interp.c layer0/src/tables.c \
 *     -lm -o test_layer4
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "song.h"
#include "../../layer0/include/patch_builder.h"

#define SR   44100
#define BLK  512

/* ---- Stereo WAV ---- */
static void write_wav_stereo(const char *path, const float *lr, int n_frames){
    FILE *f = fopen(path, "wb"); if(!f){ perror(path); return; }
    int n = n_frames * 2;
    int16_t *p = (int16_t *)malloc((size_t)n * 2);
    for(int i = 0; i < n; i++){
        float v = lr[i]; if(v > 1) v = 1; if(v < -1) v = -1;
        p[i] = (int16_t)(v * 32767.f);
    }
    uint32_t ds = (uint32_t)n * 2, rs = 36 + ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=2;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*4;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=4,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n,f);
    fclose(f); free(p);
    printf("  wrote %s\n", path);
}

/* ---- Shared patches (static to avoid stack overflow) ---- */
static PatchProgram g_piano, g_bass, g_pad;

static void init_patches(void){
    { PatchBuilder b; pb_init(&b);
      int two=pb_const_f(&b,2.f); int mod=pb_osc(&b,two);
      int car=pb_fm(&b,REG_ONE,mod,15); int env=pb_adsr(&b,0,14,8,10);
      pb_out(&b,pb_mul(&b,car,env)); g_piano=*pb_finish(&b); }
    { PatchBuilder b; pb_init(&b);
      int saw=pb_saw(&b,REG_ONE); int flt=pb_lpf(&b,saw,26);
      int env=pb_adsr(&b,0,8,20,8);
      pb_out(&b,pb_mul(&b,flt,env)); g_bass=*pb_finish(&b); }
    { PatchBuilder b; pb_init(&b);
      int o1=pb_osc(&b,REG_ONE); int dt=pb_const_f(&b,1.007f);
      int o2=pb_osc(&b,dt); int mx=pb_mix(&b,o1,o2,15,15);
      int fl=pb_lpf(&b,mx,44); int en=pb_adsr(&b,16,4,28,22);
      pb_out(&b,pb_mul(&b,fl,en)); g_pad=*pb_finish(&b); }
}

/* ---- Build shared MotifLibrary + Sections ---- */
static MotifLibrary g_lib;
static Section      g_intro, g_verse, g_chorus;

static void build_content(void){
    motif_lib_init(&g_lib);

    /* Motifs */
    { VoiceBuilder v; vb_init(&v);
      vb_note(&v,60,DUR_1_4,VEL_P); vb_note(&v,64,DUR_1_4,VEL_P);
      vb_note(&v,67,DUR_1_2,VEL_P); motif_define(&g_lib,"Intro",&v.vp); }

    { VoiceBuilder v; vb_init(&v);
      vb_note(&v,64,DUR_1_8,VEL_MF); vb_note(&v,65,DUR_1_8,VEL_MF);
      vb_note(&v,67,DUR_1_4,VEL_F);  vb_note(&v,64,DUR_1_4,VEL_MF);
      motif_define(&g_lib,"MelA",&v.vp); }

    { VoiceBuilder v; vb_init(&v);
      vb_note(&v,67,DUR_1_4,VEL_F); vb_note(&v,65,DUR_1_4,VEL_MF);
      vb_note(&v,64,DUR_1_2,VEL_MP); motif_define(&g_lib,"MelB",&v.vp); }

    { VoiceBuilder v; vb_init(&v);
      int notes[]={60,64,67,72}; /* C major arpeggio */
      for(int i=0;i<4;i++) vb_note(&v,notes[i],DUR_1_4,VEL_FF);
      motif_define(&g_lib,"Chorus",&v.vp); }

    { VoiceBuilder v; vb_init(&v);
      vb_repeat_begin(&v);
        vb_note(&v,48,DUR_1_4,VEL_MF); vb_note(&v,55,DUR_1_4,VEL_MP);
      vb_repeat_end(&v,2);
      motif_define(&g_lib,"Bass",&v.vp); }

    { VoiceBuilder v; vb_init(&v);
      vb_note(&v,60,DUR_1,VEL_P); motif_define(&g_lib,"Pad",&v.vp); }

    /* Intro section: 4 beats */
    section_init(&g_intro, "Intro", 4.f);
    { MotifUse u[]={{"Intro",0.f,1,0}};
      section_add_track(&g_intro,"lead",&g_piano,u,1,0.7f,0.f); }
    { MotifUse u[]={{"Pad",0.f,1,0}};
      section_add_track(&g_intro,"pad",&g_pad,u,1,0.4f,0.f); }

    /* Verse section: 4 beats */
    section_init(&g_verse, "Verse", 4.f);
    { MotifUse u[]={{"MelA",0.f,1,0},{"MelB",2.f,1,0}};
      section_add_track(&g_verse,"lead",&g_piano,u,2,0.8f,0.f); }
    { MotifUse u[]={{"Bass",0.f,1,0}};
      section_add_track(&g_verse,"bass",&g_bass,u,1,0.9f,-0.2f); }
    { MotifUse u[]={{"Pad",0.f,1,0}};
      section_add_track(&g_verse,"pad",&g_pad,u,1,0.5f,0.1f); }

    /* Chorus section: 4 beats */
    section_init(&g_chorus, "Chorus", 4.f);
    { MotifUse u[]={{"Chorus",0.f,2,0}};
      section_add_track(&g_chorus,"lead",&g_piano,u,1,0.9f,0.f); }
    { MotifUse u[]={{"Bass",0.f,1,0}};
      section_add_track(&g_chorus,"bass",&g_bass,u,1,0.9f,-0.3f); }
    { MotifUse u[]={{"Pad",0.f,1,0}};
      section_add_track(&g_chorus,"pad",&g_pad,u,1,0.6f,0.2f); }
}

/* ===========================================================
   TEST 1: BPM automation
   =========================================================== */
static int test_bpm_automation(void){
    printf("[bpm_automation] BPM interpolation and beats_to_samples\n");
    Song song; song_init(&song, "T", 120.f, (float)SR);
    song_add_bpm(&song, 0.f,  120.f);
    song_add_bpm(&song, 4.f,  120.f);
    song_add_bpm(&song, 8.f,  240.f);  /* double tempo at beat 8 */

    /* At beat 0: 120 BPM */
    float b0 = song_bpm_at(&song, 0.f);
    if(fabsf(b0 - 120.f) > 0.1f){ printf("  FAIL bpm@0: %.1f\n",b0); return 0; }

    /* At beat 6: midway between 120 and 240 → 180 */
    float b6 = song_bpm_at(&song, 6.f);
    if(fabsf(b6 - 180.f) > 0.5f){ printf("  FAIL bpm@6: %.1f\n",b6); return 0; }

    /* At beat 8+: 240 */
    float b8 = song_bpm_at(&song, 10.f);
    if(fabsf(b8 - 240.f) > 0.1f){ printf("  FAIL bpm@10: %.1f\n",b8); return 0; }

    /* Beats to samples: 1 beat at 120 BPM = 22050 samples */
    uint64_t s1 = song_beats_to_samples(&song, 0.f, 1.f);
    if((int64_t)s1 - 22050 > 10){ printf("  FAIL samples: %llu\n",(unsigned long long)s1); return 0; }

    printf("  bpm@0=%.1f bpm@6=%.1f bpm@10=%.1f 1beat=%llu samples ✓\n",
        b0, b6, b8, (unsigned long long)s1);
    printf("  PASS\n\n"); return 1;
}

/* ===========================================================
   TEST 2: Song construction and total_beats/seconds
   =========================================================== */
static int test_song_structure(void){
    printf("[song_structure] construction, total_beats, total_seconds\n");
    Song song; song_init(&song, "TestSong", 120.f, (float)SR);
    song.master_gain = 0.8f;

    /* Append 3 entries: no crossfade, no gap */
    song_append(&song, "Intro",  &g_intro,  &g_lib, 1, 1.f, 1.f, 0.f, 0.f);
    song_append(&song, "Verse",  &g_verse,  &g_lib, 2, 0.f, 0.f, 0.f, 0.f);
    song_append(&song, "Chorus", &g_chorus, &g_lib, 1, 0.f, 1.f, 0.f, 0.f);

    float tb = song_total_beats(&song);
    /* Intro: 4 beats × 1 repeat = 4
       Verse:  4 beats × 2 repeats = 8
       Chorus: 4 beats × 1 repeat = 4
       Total = 16 beats (no xfade, no gap) */
    if(fabsf(tb - 16.f) > 0.1f){ printf("  FAIL total_beats=%.2f\n",tb); return 0; }

    float ts = song_total_seconds(&song);
    float expected = 16.f * 60.f / 120.f;  /* 8 seconds at 120 BPM */
    if(fabsf(ts - expected) > 0.1f){ printf("  FAIL total_seconds=%.2f\n",ts); return 0; }

    printf("  n_entries=%d  total_beats=%.1f  total_seconds=%.2f ✓\n",
        song.n_entries, tb, ts);
    printf("  PASS\n\n"); return 1;
}

/* ===========================================================
   TEST 3: Full song render — Intro → Verse x2 → Chorus
   =========================================================== */
static int test_song_render(void){
    printf("[song_render] full song: Intro → Verse×2 → Chorus\n");
    Song song; song_init(&song, "Demo", 120.f, (float)SR);
    song.master_gain = 0.85f;
    song_append(&song, "Intro",  &g_intro,  &g_lib, 1, 2.f, 0.f, 0.f, 0.f);
    song_append(&song, "Verse",  &g_verse,  &g_lib, 2, 0.f, 0.f, 0.f, 0.f);
    song_append(&song, "Chorus", &g_chorus, &g_lib, 1, 0.f, 2.f, 0.f, 0.f);

    float total_sec = song_total_seconds(&song);
    int cap = (int)(total_sec * SR) + SR * 4;  /* +4s tail */
    float *buf = (float *)calloc((size_t)cap * 2, sizeof(float));

    SongRenderer *sr = song_renderer_new(&song);
    if(!sr){ printf("  FAIL: OOM\n"); free(buf); return 0; }

    float blk[BLK * 2]; int pos = 0;
    while(!sr->done && pos < cap){
        int c = cap - pos < BLK ? cap - pos : BLK;
        song_render_block(sr, blk, c);
        memcpy(buf + pos * 2, blk, (size_t)c * 2 * sizeof(float));
        pos += c;
    }
    song_renderer_free(sr);

    float pk = 0; int nans = 0;
    for(int i = 0; i < pos * 2; i++){
        if(!isfinite(buf[i])) nans++;
        float a = fabsf(buf[i]); if(a > pk) pk = a;
    }
    write_wav_stereo("/mnt/user-data/outputs/l4_song.wav", buf, pos);
    free(buf);

    if(nans || pk < 1e-5f){ printf("  FAIL peak=%g nans=%d\n",pk,nans); return 0; }
    printf("  total_sec=%.2f frames=%d peak=%.4f ✓\n", total_sec, pos, pk);
    printf("  PASS\n\n"); return 1;
}

/* ===========================================================
   TEST 4: Crossfade — two sections overlap
   =========================================================== */
static int test_crossfade(void){
    printf("[crossfade] section crossfade produces audio throughout transition\n");
    Song song; song_init(&song, "XFade", 120.f, (float)SR);
    song.master_gain = 1.f;
    /* Verse then Chorus with 1-beat crossfade */
    song_append(&song, "Verse",  &g_verse,  &g_lib, 1, 0.f, 1.f, 0.f, 0.f);
    song_append(&song, "Chorus", &g_chorus, &g_lib, 1, 1.f, 0.f, 1.f, 0.f);

    int cap = SR * 10;
    float *buf = (float *)calloc((size_t)cap * 2, sizeof(float));
    SongRenderer *sr = song_renderer_new(&song);
    if(!sr){ free(buf); return 0; }

    float blk[BLK*2]; int pos = 0;
    while(!sr->done && pos < cap){
        int c = cap - pos < BLK ? cap - pos : BLK;
        song_render_block(sr, blk, c);
        memcpy(buf + pos*2, blk, (size_t)c*2*sizeof(float));
        pos += c;
    }
    song_renderer_free(sr);

    float pk = 0; int nans = 0;
    for(int i = 0; i < pos*2; i++){
        if(!isfinite(buf[i])) nans++;
        float a = fabsf(buf[i]); if(a > pk) pk = a;
    }
    write_wav_stereo("/mnt/user-data/outputs/l4_crossfade.wav", buf, pos);
    free(buf);
    if(nans || pk < 1e-5f){ printf("  FAIL peak=%g nans=%d\n",pk,nans); return 0; }
    printf("  frames=%d  peak=%.4f  no NaN ✓\n", pos, pk);
    printf("  PASS\n\n"); return 1;
}

/* ===========================================================
   TEST 5: BPM change between sections
   =========================================================== */
static int test_bpm_change(void){
    printf("[bpm_change] song with BPM automation\n");
    Song song; song_init(&song, "Accel", 100.f, (float)SR);
    song.master_gain = 0.8f;
    /* Intro at 100 BPM, chorus at 140 BPM */
    song_add_bpm(&song, 0.f, 100.f);
    song_add_bpm(&song, 4.f, 140.f);
    song_append(&song, "Intro",  &g_intro,  &g_lib, 1, 0.f, 0.f, 0.f, 0.f);
    song_append(&song, "Chorus", &g_chorus, &g_lib, 1, 0.f, 0.f, 0.f, 0.f);

    int cap = SR * 10;
    float *buf = (float *)calloc((size_t)cap * 2, sizeof(float));
    SongRenderer *sr = song_renderer_new(&song);
    if(!sr){ free(buf); return 0; }
    float blk[BLK*2]; int pos = 0;
    while(!sr->done && pos < cap){
        int c = cap-pos<BLK?cap-pos:BLK;
        song_render_block(sr,blk,c);
        memcpy(buf+pos*2,blk,(size_t)c*2*sizeof(float));
        pos+=c;
    }
    song_renderer_free(sr);
    float pk=0; int nans=0;
    for(int i=0;i<pos*2;i++){ if(!isfinite(buf[i]))nans++; float a=fabsf(buf[i]);if(a>pk)pk=a;}
    write_wav_stereo("/mnt/user-data/outputs/l4_bpm_change.wav",buf,pos);
    free(buf);
    if(nans||pk<1e-5f){ printf("  FAIL\n"); return 0; }
    printf("  frames=%d  peak=%.4f ✓\n",pos,pk);
    printf("  PASS\n\n"); return 1;
}

/* ===========================================================
   Main
   =========================================================== */
int main(void){
    tables_init();
    init_patches();
    build_content();

    printf("=== SHMC Layer 4 — Song DSL Test ===\n\n");
    int pass=0, total=0;
    #define RUN(fn) do{ total++; if(fn()) pass++; }while(0)

    RUN(test_bpm_automation);
    RUN(test_song_structure);
    RUN(test_song_render);
    RUN(test_crossfade);
    RUN(test_bpm_change);

    printf("=== %d / %d passed ===\n", pass, total);
    return (pass == total) ? 0 : 1;
}
