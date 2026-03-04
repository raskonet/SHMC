/*
 * SHMC DSL Capability Demo — "C Blues" song section
 * Proves the DSL can reproduce and modify a song section.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "song.h"
#include "../../layer0/include/patch_builder.h"


#define SR    44100
#define BLK   512

static void write_wav(const char *path, const float *lr, int nframes){
    FILE *f=fopen(path,"wb"); if(!f){perror(path);return;}
    int n=nframes*2;
    int16_t *p=(int16_t*)malloc((size_t)n*2);
    for(int i=0;i<n;i++){float v=lr[i];if(v>1)v=1;if(v<-1)v=-1;p[i]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)n*2,rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=2;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*4;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=4,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n,f);
    fclose(f);free(p);
    printf("  wrote %s  (%d frames = %.2fs)\n",path,nframes,(float)nframes/SR);
}

static PatchProgram g_lead, g_bass_patch, g_pad_patch;

static void make_patches(void){
    { PatchBuilder b; pb_init(&b);
      int m=pb_osc(&b,pb_const_f(&b,2.f));
      int ms=pb_mul(&b,m,pb_const_f(&b,6.f));
      int c=pb_fm(&b,REG_ONE,ms,15);
      int e=pb_adsr(&b,1,12,18,14);
      pb_out(&b,pb_mul(&b,c,e));
      g_lead=*pb_finish(&b); }
    { PatchBuilder b; pb_init(&b);
      int s=pb_saw(&b,REG_ONE);
      int f=pb_lpf(&b,s,20);
      int e=pb_adsr(&b,0,6,15,8);
      pb_out(&b,pb_mul(&b,f,e));
      g_bass_patch=*pb_finish(&b); }
    { PatchBuilder b; pb_init(&b);
      int o1=pb_osc(&b,REG_ONE);
      int o2=pb_osc(&b,pb_const_f(&b,1.008f));
      int mx=pb_mix(&b,o1,o2,15,15);
      int lp=pb_lpf(&b,mx,40);
      int e=pb_adsr(&b,20,8,24,20);
      pb_out(&b,pb_mul(&b,lp,e));
      g_pad_patch=*pb_finish(&b); }
}

static MotifLibrary g_lib;

/* Each motif phrase = 4 beats (1 bar at 4/4)
   Notes: dur=1 = eighth note (0.5 beat), dur=2 = quarter (1 beat), dur=4 = half (2 beat) */
static void build_motifs(void){
    motif_lib_init(&g_lib);

    /* MELODY — C blues pentatonic */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,60,1,6); vb_note(&b,63,1,5); vb_note(&b,65,1,6); vb_note(&b,67,1,7);
      vb_note(&b,70,1,6); vb_note(&b,67,1,5); vb_note(&b,65,1,5); vb_note(&b,63,1,4);
      motif_define(&g_lib,"MelA",vb_finish(&b)); }  /* C Eb F G Bb G F Eb */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,63,1,5); vb_note(&b,65,1,6); vb_note(&b,67,1,7); vb_note(&b,70,1,7);
      vb_note(&b,72,1,7); vb_note(&b,70,1,6); vb_note(&b,67,1,5); vb_note(&b,65,1,5);
      motif_define(&g_lib,"MelB",vb_finish(&b)); }  /* Eb F G Bb C Bb G F */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,67,1,7); vb_note(&b,70,1,6); vb_note(&b,67,1,6); vb_note(&b,65,1,5);
      vb_note(&b,63,1,5); vb_note(&b,60,2,6); vb_note(&b,63,1,5); vb_note(&b,60,1,7);
      motif_define(&g_lib,"MelC",vb_finish(&b)); }  /* turnaround G Bb G F Eb C.. */

    /* BASS — root+fifth patterns */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,36,2,7); vb_note(&b,43,2,6);  /* C2 G2 — I chord */
      motif_define(&g_lib,"BassI",vb_finish(&b)); }
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,41,2,7); vb_note(&b,48,2,6);  /* F2 C3 — IV chord */
      motif_define(&g_lib,"BassIV",vb_finish(&b)); }
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b,43,2,7); vb_note(&b,41,2,6);  /* G2 F2 — V-IV */
      motif_define(&g_lib,"BassV",vb_finish(&b)); }

    /* PADS — chord tones (root voice, each bar held) */
    { VoiceBuilder b; vb_init(&b); vb_note(&b,48,4,4); vb_note(&b,52,4,3); vb_note(&b,55,4,3);
      motif_define(&g_lib,"PadI",vb_finish(&b)); }    /* C E G */
    { VoiceBuilder b; vb_init(&b); vb_note(&b,41,4,4); vb_note(&b,45,4,3); vb_note(&b,48,4,3);
      motif_define(&g_lib,"PadIV",vb_finish(&b)); }   /* F A C */
    { VoiceBuilder b; vb_init(&b); vb_note(&b,43,4,4); vb_note(&b,47,4,3); vb_note(&b,50,4,3);
      motif_define(&g_lib,"PadV",vb_finish(&b)); }    /* G B D */
}

/* Build section: 12 bars = 48 beats */
static void build_section(Section *s, int transpose, float bass_vel){
    section_init(s, "12BarBlues", 48.f);

    /* Melody track */
    { MotifUse m[6]; memset(m,0,sizeof(m));
      m[0]=motif_use("MelA", 0.f, 4,transpose);    /* bars 1-4 */
      m[1]=motif_use("MelB",16.f, 2,transpose);    /* bars 5-6 */
      m[2]=motif_use("MelA",24.f, 2,transpose);    /* bars 7-8 */
      m[3]=motif_use("MelC",32.f, 1,transpose);    /* bar 9 */
      m[4]=motif_use("MelB",36.f, 1,transpose-2);  /* bar 10 IV */
      m[5]=motif_use("MelA",40.f, 2,transpose);    /* bars 11-12*/
      section_add_track(s,"melody",&g_lead,m,6,0.85f,0.f); }

    /* Bass track */
    { MotifUse b[7]; memset(b,0,sizeof(b));
      b[0]=motif_use("BassI",  0.f, 4,transpose);
      b[1]=motif_use("BassIV",16.f, 2,transpose);
      b[2]=motif_use("BassI", 24.f, 2,transpose);
      b[3]=motif_use("BassV", 32.f, 1,transpose);
      b[4]=motif_use("BassIV",36.f, 1,transpose);
      b[5]=motif_use("BassI", 40.f, 1,transpose);
      b[6]=motif_use("BassV", 44.f, 1,transpose);
      for(int i=0;i<7;i++) b[i].vel_scale=bass_vel;
      section_add_track(s,"bass",&g_bass_patch,b,7,0.75f,-0.2f); }

    /* Pad track */
    { MotifUse p[5]; memset(p,0,sizeof(p));
      p[0]=motif_use("PadI",  0.f, 4,transpose);
      p[1]=motif_use("PadIV",16.f, 2,transpose);
      p[2]=motif_use("PadI", 24.f, 2,transpose);
      p[3]=motif_use("PadV", 32.f, 1,transpose);
      p[4]=motif_use("PadIV",36.f, 3,transpose);
      section_add_track(s,"pad",&g_pad_patch,p,5,0.50f,0.15f); }
}

static int render_section_to_wav(Section *sec, float bpm, float master_gain,
                                  const char *wav_path, const char *meta_path,
                                  float **out_buf, int *out_frames){
    Song song; song_init(&song,"Demo",bpm,(float)SR);
    song.master_gain=master_gain;
    song_append(&song,"S",sec,&g_lib,1,0.f,0.f,0.f,0.f);

    float tsec=song_total_seconds(&song);
    int cap=(int)(tsec*(float)SR)+(int)(SR*2);
    float *buf=(float*)calloc((size_t)cap*2,sizeof(float));
    if(!buf) return 0;

    SongRenderer *sr=song_renderer_new(&song);
    if(!sr){free(buf);return 0;}

    float blk[BLK*2]; int pos=0;
    while(!sr->done && pos<cap){
        int c=cap-pos<BLK?cap-pos:BLK;
        song_render_block(sr,blk,c);
        memcpy(buf+pos*2,blk,(size_t)c*2*sizeof(float));
        pos+=c;
    }
    song_renderer_free(sr);

    float pk=0; int nans=0;
    for(int i=0;i<pos*2;i++){
        if(!isfinite(buf[i]))nans++;
        float a=fabsf(buf[i]);if(a>pk)pk=a;
    }
    printf("  bpm=%.0f  frames=%d  dur=%.2fs  peak=%.4f  nans=%d\n",
           bpm,(int)pos,(float)pos/SR,pk,nans);

    if(wav_path) write_wav(wav_path,buf,pos);

    if(meta_path){
        FILE *mf=fopen(meta_path,"w");
        if(mf){
            fprintf(mf,"frames=%d\nsample_rate=%d\nduration_sec=%.6f\npeak=%.6f\nnans=%d\nbpm=%.2f\ntranspose=%d\n",
                    pos,SR,(float)pos/SR,pk,nans,bpm,0);
            fclose(mf);
        }
    }

    if(out_buf){ *out_buf=buf; *out_frames=pos; }
    else free(buf);

    return (nans==0 && pk>1e-4f) ? 1 : 0;
}

int main(void){
    tables_init();
    make_patches();
    build_motifs();

    printf("=== SHMC DSL Capability Demo: 12-Bar C Blues ===\n\n");
    int pass=0, total=0;

    /* ── TEST 1: REPRODUCE original section ── */
    printf("[TEST 1] Reproduce: 12-bar C blues at 120 BPM\n");
    total++;
    { static Section sec; build_section(&sec,0,1.0f);
      int ok=render_section_to_wav(&sec,120.f,0.80f,
          "/mnt/user-data/outputs/blues_original.wav",
          "/mnt/user-data/outputs/blues_original.meta.txt",
          NULL,NULL);
      printf("  %s\n\n",ok?"PASS":"FAIL");
      pass+=ok; }

    /* ── TEST 2: MODIFY — transpose +5, faster tempo, louder bass ── */
    printf("[TEST 2] Modify: +5 semitones (C→F), 140 BPM, bass vel_scale=1.3\n");
    total++;
    { static Section sec; build_section(&sec,5,1.3f);
      int ok=render_section_to_wav(&sec,140.f,0.80f,
          "/mnt/user-data/outputs/blues_modified.wav",
          "/mnt/user-data/outputs/blues_modified.meta.txt",
          NULL,NULL);
      printf("  %s\n\n",ok?"PASS":"FAIL");
      pass+=ok; }

    /* ── TEST 3: EXACT REPRODUCTION — determinism check ── */
    printf("[TEST 3] Determinism: render original twice, compare sample-by-sample\n");
    total++;
    {
        static Section secA, secB;
        build_section(&secA,0,1.0f);
        build_section(&secB,0,1.0f);

        float *bufA=NULL,*bufB=NULL; int nA=0,nB=0;
        printf("  render A: "); render_section_to_wav(&secA,120.f,0.80f,"/mnt/user-data/outputs/blues_repro_A.wav",NULL,&bufA,&nA);
        printf("  render B: "); render_section_to_wav(&secB,120.f,0.80f,"/mnt/user-data/outputs/blues_repro_B.wav",NULL,&bufB,&nB);

        int diff=0; double max_err=0; int cmp=nA<nB?nA:nB;
        for(int i=0;i<cmp*2;i++){
            double e=fabs((double)bufA[i]-(double)bufB[i]);
            if(e>0)diff++;
            if(e>max_err)max_err=e;
        }
        /* write repro metadata */
        FILE *mf=fopen("/mnt/user-data/outputs/blues_repro.meta.txt","w");
        if(mf){ fprintf(mf,"frames_A=%d\nframes_B=%d\ndiff_samples=%d\nmax_err=%.2e\n",nA,nB,diff,max_err); fclose(mf); }

        free(bufA); free(bufB);
        int ok=(diff==0);
        printf("  nA=%d nB=%d differing_samples=%d max_err=%.2e\n",nA,nB,diff,max_err);
        printf("  %s\n\n",ok?"PASS: bit-identical":"FAIL: non-deterministic");
        pass+=ok;
    }

    /* ── TEST 4: MODIFICATION CORRECTNESS — duration proportional to tempo ── */
    printf("[TEST 4] Modification correctness: 140 BPM duration = 120/140 * original\n");
    total++;
    {
        static Section sA, sB;
        build_section(&sA,0,1.0f);
        build_section(&sB,0,1.0f);

        float *bufA=NULL,*bufB=NULL; int nA=0,nB=0;
        printf("  120 BPM: "); render_section_to_wav(&sA,120.f,0.80f,NULL,NULL,&bufA,&nA);
        printf("  140 BPM: "); render_section_to_wav(&sB,140.f,0.80f,NULL,NULL,&bufB,&nB);

        /* Expected: nB/nA ≈ 120/140 = 0.857 */
        float ratio=(float)nB/(float)nA;
        float expected=120.f/140.f;
        float err=fabsf(ratio-expected);
        int ok=(err<0.025f);  /* allow 2.5% (ADSR release tail is BPM-independent, shifts ratio) tolerance for integer rounding */

        FILE *mf=fopen("/mnt/user-data/outputs/blues_tempo_ratio.meta.txt","w");
        if(mf){ fprintf(mf,"frames_120bpm=%d\nframes_140bpm=%d\nratio=%.6f\nexpected=%.6f\nerr=%.6f\n",nA,nB,ratio,expected,err); fclose(mf); }

        free(bufA); free(bufB);
        printf("  frames_120=%d  frames_140=%d  ratio=%.4f  expected=%.4f  err=%.4f\n",nA,nB,ratio,expected,err);
        printf("  %s\n\n",ok?"PASS":"FAIL");
        pass+=ok;
    }

    printf("=== %d / %d passed ===\n",pass,total);
    return (pass==total)?0:1;
}
