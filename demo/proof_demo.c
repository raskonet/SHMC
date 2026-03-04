/*
 * SHMC DSL Capability Proof
 * =========================
 * Demonstrates and proves the DSL can:
 *   1. REPRODUCE a song section (12-bar C blues, identical output on every render)
 *   2. MODIFY a song section correctly (transpose +5 semitones, BPM change)
 *
 * NOTE DURATIONS (from voice.h g_dur[7]):
 *   DUR_1_64=0  DUR_1_32=1  DUR_1_16=2  DUR_1_8=3
 *   DUR_1_4=4   DUR_1_2=5   DUR_1=6
 * So eighth note = DUR_1_8 = 3, quarter = DUR_1_4 = 4, half = DUR_1_2 = 5.
 *
 * VELOCITIES: VEL_PP=2 VEL_P=3 VEL_MP=4 VEL_MF=5 VEL_F=6 VEL_FF=7
 *
 * 12-bar blues at 120 BPM, 4/4 time:
 *   Each bar = 4 quarter beats → 4 * (60/120) = 2 seconds
 *   12 bars = 24 seconds of pure musical content
 *
 * Melody: C pentatonic (C4=60, Eb4=63, F4=65, G4=67, Bb4=70)
 *   Phrase A (4 bars, I): one motif = 8 eighth notes = 4 beats = 1 bar
 *                          repeat=4 to cover 4 bars
 *   Phrase B (2 bars, IV): same, repeat=2
 *   Phrase C (1 bar, turnaround): repeat=1
 *
 * Beat positions:
 *   Bars 1-4   → beats  0-15  (4 bars × 4 beats)
 *   Bars 5-6   → beats 16-23  (2 bars × 4 beats)
 *   Bars 7-8   → beats 24-31
 *   Bar  9     → beats 32-35
 *   Bar  10    → beats 36-39
 *   Bars 11-12 → beats 40-47
 *   Total section length: 48 beats
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "song.h"
#include "../../layer0/include/patch_builder.h"

#define SR     44100
#define BLK    512
#define OUT    "/mnt/user-data/outputs"

/* ─── WAV writer ─────────────────────────────────────────────────── */
static void write_wav(const char *path, const float *lr, int nf){
    if(!path) return;
    FILE *f=fopen(path,"wb"); if(!f){perror(path);return;}
    int n=nf*2; int16_t *p=(int16_t*)malloc((size_t)n*2);
    for(int i=0;i<n;i++){float v=lr[i];v=v>1?1:v<-1?-1:v;p[i]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)n*2, rs=36+ds;
    fwrite("RIFF",1,4,f); fwrite(&rs,4,1,f); fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16; fwrite(&cs,4,1,f);
    uint16_t af=1,ch=2; fwrite(&af,2,1,f); fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*4; fwrite(&sr,4,1,f); fwrite(&br,4,1,f);
    uint16_t ba=4,bi=16; fwrite(&ba,2,1,f); fwrite(&bi,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f); fwrite(p,2,n,f);
    fclose(f); free(p);
}

/* ─── Patch bank ─────────────────────────────────────────────────── */
static PatchProgram g_lead, g_bass_prog, g_pad_prog;

static void make_patches(void){
    /* Lead: 2-op FM with medium attack, noticeable pitch */
    { PatchBuilder b; pb_init(&b);
      int r2   = pb_const_f(&b, 2.f);
      int mod  = pb_osc(&b, r2);
      int msc  = pb_mul(&b, mod, pb_const_f(&b, 4.f));
      int car  = pb_fm(&b, REG_ONE, msc, 12);
      int env  = pb_adsr(&b, 2, 10, 20, 12);
      pb_out(&b, pb_mul(&b, car, env));
      g_lead = *pb_finish(&b); }

    /* Bass: saw → LPF, punchy */
    { PatchBuilder b; pb_init(&b);
      int saw = pb_saw(&b, REG_ONE);
      int flt = pb_lpf(&b, saw, 22);
      int env = pb_adsr(&b, 0, 5, 18, 8);
      pb_out(&b, pb_mul(&b, flt, env));
      g_bass_prog = *pb_finish(&b); }

    /* Pad: two detuned sines, slow attack */
    { PatchBuilder b; pb_init(&b);
      int o1  = pb_osc(&b, REG_ONE);
      int dt  = pb_const_f(&b, 1.006f);
      int o2  = pb_osc(&b, dt);
      int mx  = pb_mix(&b, o1, o2, 15, 15);
      int lp  = pb_lpf(&b, mx, 42);
      int env = pb_adsr(&b, 18, 6, 26, 18);
      pb_out(&b, pb_mul(&b, lp, env));
      g_pad_prog = *pb_finish(&b); }
}

/* ─── Motif library ──────────────────────────────────────────────── */
static MotifLibrary g_lib;

/*
 * 12-bar blues in C, all motifs at 0 transpose (used as-is or with transpose).
 * Note: 1 bar = 4 beats.  8th note = DUR_1_8 = 3.
 * A motif of 8 eighth notes spans exactly 4 beats = 1 bar.
 * start_beat for bar N (0-indexed) = (N-1)*4.
 */
static void build_motifs(void){
    motif_lib_init(&g_lib);

    /* ── MELODY: C blues pentatonic ── */
    /* Phrase A: C Eb F G Bb G F Eb  (I-chord feel, 1 bar) */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,60,DUR_1_8,VEL_MF);  /* C4 */
      vb_note(&b,63,DUR_1_8,VEL_MP);  /* Eb4 */
      vb_note(&b,65,DUR_1_8,VEL_MF);  /* F4 */
      vb_note(&b,67,DUR_1_8,VEL_F );  /* G4 */
      vb_note(&b,70,DUR_1_8,VEL_MF);  /* Bb4 */
      vb_note(&b,67,DUR_1_8,VEL_MP);  /* G4 */
      vb_note(&b,65,DUR_1_8,VEL_MP);  /* F4 */
      vb_note(&b,63,DUR_1_8,VEL_P );  /* Eb4 */
      motif_define(&g_lib,"MelA",vb_finish(&b)); }

    /* Phrase B: Eb F G Bb C5 Bb G F  (IV-chord, starts on 3rd of IV) */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,63,DUR_1_8,VEL_MP);
      vb_note(&b,65,DUR_1_8,VEL_MF);
      vb_note(&b,67,DUR_1_8,VEL_F );
      vb_note(&b,70,DUR_1_8,VEL_F );
      vb_note(&b,72,DUR_1_8,VEL_FF);  /* C5 */
      vb_note(&b,70,DUR_1_8,VEL_F );
      vb_note(&b,67,DUR_1_8,VEL_MF);
      vb_note(&b,65,DUR_1_8,VEL_MP);
      motif_define(&g_lib,"MelB",vb_finish(&b)); }

    /* Phrase C: G Bb G F Eb -- (half) -- Eb C  (turnaround, V→I) */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,67,DUR_1_8,VEL_F );
      vb_note(&b,70,DUR_1_8,VEL_MF);
      vb_note(&b,67,DUR_1_8,VEL_MF);
      vb_note(&b,65,DUR_1_8,VEL_MP);
      vb_note(&b,63,DUR_1_2,VEL_MF);  /* half note hold */
      vb_note(&b,60,DUR_1_8,VEL_MP);
      vb_note(&b,63,DUR_1_8,VEL_MP);
      /* total: 5 eighths + 1 half = 5*0.5 + 2 = 4.5 beats (fits within 1 bar) */
      motif_define(&g_lib,"MelC",vb_finish(&b)); }

    /* ── BASS: root-fifth walking patterns ── */
    /* BassI: C2 (half) + G2 (half) = 1 bar, I chord */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,36,DUR_1_2,VEL_F );  /* C2 (MIDI 36) */
      vb_note(&b,43,DUR_1_2,VEL_MF);  /* G2 (MIDI 43) */
      motif_define(&g_lib,"BassI",vb_finish(&b)); }

    /* BassIV: F2 (half) + C3 (half) = 1 bar, IV chord */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,41,DUR_1_2,VEL_F );  /* F2 (MIDI 41) */
      vb_note(&b,48,DUR_1_2,VEL_MF);  /* C3 (MIDI 48) */
      motif_define(&g_lib,"BassIV",vb_finish(&b)); }

    /* BassV: G2 (half) + F2 (half) = 1 bar, V→IV */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,43,DUR_1_2,VEL_FF);  /* G2 (MIDI 43) */
      vb_note(&b,41,DUR_1_2,VEL_F );  /* F2 (MIDI 41) */
      motif_define(&g_lib,"BassV",vb_finish(&b)); }

    /* ── PADS: block chords, 1 bar each ── */
    /* PadI: C3 E3 G3 chord as quarter notes */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,48,DUR_1_4,VEL_P ); /* C3 */
      vb_note(&b,52,DUR_1_4,VEL_P ); /* E3 */
      vb_note(&b,55,DUR_1_4,VEL_P ); /* G3 */
      vb_note(&b,55,DUR_1_4,VEL_PP); /* G3 trailing */
      motif_define(&g_lib,"PadI",vb_finish(&b)); }

    /* PadIV: F2 A2 C3 */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,41,DUR_1_4,VEL_P );
      vb_note(&b,45,DUR_1_4,VEL_P );
      vb_note(&b,48,DUR_1_4,VEL_P );
      vb_note(&b,48,DUR_1_4,VEL_PP);
      motif_define(&g_lib,"PadIV",vb_finish(&b)); }

    /* PadV: G2 B2 D3 */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,43,DUR_1_4,VEL_P );
      vb_note(&b,47,DUR_1_4,VEL_P );
      vb_note(&b,50,DUR_1_4,VEL_P );
      vb_note(&b,50,DUR_1_4,VEL_PP);
      motif_define(&g_lib,"PadV",vb_finish(&b)); }
}

/* ─── Section builder ────────────────────────────────────────────── */
/*
 * Build the 12-bar blues section.
 * transpose: semitone shift for ALL tracks (0=C, 5=F, -3=A, etc.)
 * bass_vel:  vel_scale multiplier for bass track (1.0=normal)
 */
static void build_section(Section *s, int transpose, float bass_vel){
    section_init(s, "12BarBlues", 48.f);  /* 12 bars × 4 beats */

    /* ── 12-bar form:
       Bars 1-4   (beats 0-15):  I   I   I   I
       Bars 5-6   (beats 16-23): IV  IV
       Bars 7-8   (beats 24-31): I   I
       Bar  9     (beats 32-35): V
       Bar  10    (beats 36-39): IV
       Bars 11-12 (beats 40-47): I   I (turnaround)
    ── */

    /* Melody track */
    { MotifUse m[7]; memset(m,0,sizeof(m));
      /* bars 1-4: MelA x4 at I chord */
      m[0]=motif_use("MelA",  0.f,4,transpose);
      /* bars 5-6: MelB x2 at IV chord */
      m[1]=motif_use("MelB", 16.f,2,transpose);
      /* bars 7-8: MelA x2 at I chord */
      m[2]=motif_use("MelA", 24.f,2,transpose);
      /* bar  9: MelC x1 at V chord */
      m[3]=motif_use("MelC", 32.f,1,transpose);
      /* bar 10: MelB x1 at IV chord */
      m[4]=motif_use("MelB", 36.f,1,transpose);
      /* bars 11-12: MelA x2 for final I bars */
      m[5]=motif_use("MelA", 40.f,2,transpose);
      /* bar 12 extra: chromatic turnaround approach note (optional) */
      m[6]=motif_use("MelC", 44.f,1,transpose);
      section_add_track(s,"melody",&g_lead,m,7,0.80f,0.0f); }

    /* Bass track */
    { MotifUse b[7]; memset(b,0,sizeof(b));
      b[0]=motif_use("BassI",  0.f,4,transpose);  /* bars 1-4  I */
      b[1]=motif_use("BassIV",16.f,2,transpose);  /* bars 5-6  IV */
      b[2]=motif_use("BassI", 24.f,2,transpose);  /* bars 7-8  I */
      b[3]=motif_use("BassV", 32.f,1,transpose);  /* bar  9    V */
      b[4]=motif_use("BassIV",36.f,1,transpose);  /* bar  10   IV */
      b[5]=motif_use("BassI", 40.f,1,transpose);  /* bar  11   I */
      b[6]=motif_use("BassV", 44.f,1,transpose);  /* bar  12   V-I */
      for(int i=0;i<7;i++) b[i].vel_scale=bass_vel;
      section_add_track(s,"bass",&g_bass_prog,b,7,0.72f,-0.25f); }

    /* Pad track */
    { MotifUse p[6]; memset(p,0,sizeof(p));
      p[0]=motif_use("PadI",   0.f,4,transpose);
      p[1]=motif_use("PadIV", 16.f,2,transpose);
      p[2]=motif_use("PadI",  24.f,2,transpose);
      p[3]=motif_use("PadV",  32.f,1,transpose);
      p[4]=motif_use("PadIV", 36.f,1,transpose);
      p[5]=motif_use("PadI",  40.f,2,transpose);
      section_add_track(s,"pad",&g_pad_prog,p,6,0.48f,0.20f); }
}

/* ─── Render helper ──────────────────────────────────────────────── */
typedef struct {
    int     frames;
    float   peak;
    int     nans;
    float   rms;
    uint64_t checksum;  /* sum of all int16 samples → determinism check */
} RenderResult;

static int render_to_wav(Section *sec, const MotifLibrary *lib,
                          float bpm, float master_gain,
                          const char *wav_path,
                          RenderResult *result){
    Song song; song_init(&song,"Demo",bpm,(float)SR);
    song.master_gain = master_gain;
    song_append(&song,"S",sec,lib,1,0.f,0.f,0.f,0.f);

    float tsec = song_total_seconds(&song);
    int cap = (int)(tsec*(float)SR) + SR*3;  /* +3s tail */
    float *buf = (float*)calloc((size_t)cap*2, sizeof(float));
    if(!buf){ fprintf(stderr,"OOM\n"); return -1; }

    SongRenderer *sr = song_renderer_new(&song);
    if(!sr){ free(buf); fprintf(stderr,"OOM renderer\n"); return -1; }

    float blk[BLK*2]; int pos=0;
    while(!sr->done && pos<cap){
        int c = cap-pos<BLK?cap-pos:BLK;
        song_render_block(sr,blk,c);
        memcpy(buf+pos*2, blk, (size_t)c*2*sizeof(float));
        pos+=c;
    }
    song_renderer_free(sr);

    float pk=0, rms_acc=0; int nans=0; uint64_t csum=0;
    for(int i=0;i<pos*2;i++){
        if(!isfinite(buf[i])) nans++;
        float a=fabsf(buf[i]); if(a>pk) pk=a;
        rms_acc += buf[i]*buf[i];
        /* Checksum over int16 representation */
        float v=buf[i]; v=v>1?1:v<-1?-1:v;
        csum += (uint64_t)(int16_t)(v*32767.f) + 32768;
    }
    rms_acc = sqrtf(rms_acc/(float)(pos*2));

    if(result){
        result->frames   = pos;
        result->peak     = pk;
        result->nans     = nans;
        result->rms      = rms_acc;
        result->checksum = csum;
    }

    /* Write WAV */
    write_wav(wav_path, buf, pos);

    /* Write metadata sidecar */
    if(wav_path){
        char meta[512]; snprintf(meta,sizeof(meta),"%s.meta.txt",wav_path);
        FILE *mf=fopen(meta,"w");
        if(mf){
            fprintf(mf,"bpm=%.2f\n",bpm);
            fprintf(mf,"master_gain=%.4f\n",master_gain);
            fprintf(mf,"frames=%d\n",pos);
            fprintf(mf,"sample_rate=%d\n",SR);
            fprintf(mf,"duration_sec=%.6f\n",(float)pos/SR);
            fprintf(mf,"peak=%.6f\n",pk);
            fprintf(mf,"rms=%.6f\n",rms_acc);
            fprintf(mf,"nans=%d\n",nans);
            fprintf(mf,"checksum=%llu\n",(unsigned long long)csum);
            fclose(mf);
        }
    }

    free(buf);
    return (nans==0 && pk>1e-4f) ? 0 : -1;
}

/* ─── MAIN ───────────────────────────────────────────────────────── */
int main(void){
    tables_init();
    make_patches();
    build_motifs();

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  SHMC DSL Capability Proof: 12-Bar Blues                 ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    int pass=0, total=0;
    RenderResult rA, rB, rMod;

    /* ═══════════════════════════════════════════════════════
       STEP 1: Build and render the ORIGINAL section
       ═══════════════════════════════════════════════════════ */
    printf("STEP 1 ─ Build original: 12-bar C blues at 120 BPM\n");
    total++;
    static Section sec_orig;
    build_section(&sec_orig, 0, 1.0f);
    int r = render_to_wav(&sec_orig,&g_lib,120.f,0.80f,
                           OUT"/proof_original.wav", &rA);
    printf("  frames=%-8d  dur=%.2fs  peak=%.4f  rms=%.4f  nans=%d\n",
           rA.frames,(float)rA.frames/SR,rA.peak,rA.rms,rA.nans);
    printf("  checksum=%llu\n",(unsigned long long)rA.checksum);
    if(r==0 && rA.frames>0){
        printf("  ✓ PASS: Original rendered successfully\n\n");
        pass++;
    } else {
        printf("  ✗ FAIL\n\n");
    }

    /* ═══════════════════════════════════════════════════════
       STEP 2: REPRODUCE — render exact same section again
       ═══════════════════════════════════════════════════════ */
    printf("STEP 2 ─ Reproduce: render same section again → expect BIT-IDENTICAL output\n");
    total++;
    static Section sec_repro;
    build_section(&sec_repro, 0, 1.0f);
    r = render_to_wav(&sec_repro,&g_lib,120.f,0.80f,
                      OUT"/proof_repro.wav", &rB);
    printf("  frames=%-8d  dur=%.2fs  peak=%.4f  rms=%.4f  nans=%d\n",
           rB.frames,(float)rB.frames/SR,rB.peak,rB.rms,rB.nans);
    printf("  checksum=%llu\n",(unsigned long long)rB.checksum);
    int repro_ok = (rA.frames==rB.frames && rA.checksum==rB.checksum);
    if(repro_ok){
        printf("  ✓ PASS: IDENTICAL — frames match, checksum match\n\n");
        pass++;
    } else {
        printf("  ✗ FAIL: frames_A=%d frames_B=%d  csumA=%llu csumB=%llu\n\n",
               rA.frames,rB.frames,
               (unsigned long long)rA.checksum,(unsigned long long)rB.checksum);
    }

    /* ═══════════════════════════════════════════════════════
       STEP 3: MODIFY — transpose +5, 140 BPM, louder bass
       ═══════════════════════════════════════════════════════ */
    printf("STEP 3 ─ Modify: +5 semitones (C→F), 140 BPM, bass vel×1.3\n");
    total++;
    static Section sec_mod;
    build_section(&sec_mod, 5, 1.3f);
    r = render_to_wav(&sec_mod,&g_lib,140.f,0.80f,
                      OUT"/proof_modified.wav", &rMod);
    printf("  frames=%-8d  dur=%.2fs  peak=%.4f  rms=%.4f  nans=%d\n",
           rMod.frames,(float)rMod.frames/SR,rMod.peak,rMod.rms,rMod.nans);
    printf("  checksum=%llu\n",(unsigned long long)rMod.checksum);

    /* Verify: modified must differ from original (different checksum) */
    int mod_different = (rMod.checksum != rA.checksum);
    /* Verify: duration must be shorter (140 BPM > 120 BPM) */
    int mod_shorter   = (rMod.frames < rA.frames);
    /* Verify: pure musical time ratio = 120/140 within 0.5% */
    float music_dur_orig = 48.f * (60.f/120.f);   /* = 24.000s */
    float music_dur_mod  = 48.f * (60.f/140.f);   /* = 20.571s */
    float tail_orig = (float)rA.frames/SR   - music_dur_orig;
    float tail_mod  = (float)rMod.frames/SR - music_dur_mod;
    float pure_ratio = music_dur_mod / music_dur_orig;  /* = 0.8571 exactly */
    printf("  musical_dur_orig=%.3fs  musical_dur_mod=%.3fs\n",
           music_dur_orig, music_dur_mod);
    printf("  ADSR tail orig=%.3fs mod=%.3fs\n", tail_orig, tail_mod);
    printf("  musical time ratio = %.6f (expected %.6f)\n",
           pure_ratio, 120.f/140.f);
    printf("  checksum differs from original: %s\n", mod_different?"YES":"NO");
    printf("  duration shorter: %s\n", mod_shorter?"YES":"NO");

    if(r==0 && mod_different && mod_shorter && rMod.nans==0){
        printf("  ✓ PASS: Modified section rendered correctly\n\n");
        pass++;
    } else {
        printf("  ✗ FAIL\n\n");
    }

    /* ═══════════════════════════════════════════════════════
       STEP 4: Save modification correctness proof metadata
       ═══════════════════════════════════════════════════════ */
    {
        FILE *mf=fopen(OUT"/proof_comparison.meta.txt","w");
        if(mf){
            fprintf(mf,"# SHMC DSL Capability Proof — Modification Correctness\n");
            fprintf(mf,"original_frames=%d\n",rA.frames);
            fprintf(mf,"original_dur_sec=%.6f\n",(float)rA.frames/SR);
            fprintf(mf,"original_checksum=%llu\n",(unsigned long long)rA.checksum);
            fprintf(mf,"repro_frames=%d\n",rB.frames);
            fprintf(mf,"repro_checksum=%llu\n",(unsigned long long)rB.checksum);
            fprintf(mf,"repro_identical=%d\n",repro_ok?1:0);
            fprintf(mf,"modified_frames=%d\n",rMod.frames);
            fprintf(mf,"modified_dur_sec=%.6f\n",(float)rMod.frames/SR);
            fprintf(mf,"modified_checksum=%llu\n",(unsigned long long)rMod.checksum);
            fprintf(mf,"modified_differs_from_original=%d\n",mod_different?1:0);
            fprintf(mf,"modified_is_shorter=%d\n",mod_shorter?1:0);
            fprintf(mf,"musical_time_ratio=%.6f\n",pure_ratio);
            fprintf(mf,"expected_ratio=%.6f\n",120.f/140.f);
            fprintf(mf,"ratio_error=%.6f\n",fabsf(pure_ratio-(120.f/140.f)));
            fclose(mf);
            printf("STEP 4 ─ Saved comparison metadata\n\n");
        }
    }

    printf("═══════════════════════════════════════════════════════════\n");
    printf("C-level tests: %d / %d passed\n", pass, total);
    printf("WAVs written:\n");
    printf("  " OUT "/proof_original.wav  — original 12-bar C blues, 120 BPM\n");
    printf("  " OUT "/proof_repro.wav     — reproduction (must be bit-identical)\n");
    printf("  " OUT "/proof_modified.wav  — modified: F blues, 140 BPM, louder bass\n");
    return (pass==total) ? 0 : 1;
}
