/*
 * Single-note isolation test: render ONE note at C2, ONE at F2,
 * and ONE at C2+5semitones via motif_use transpose.
 * This directly proves whether transpose=5 produces F2 or not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "song.h"
#include "../../layer0/include/patch_builder.h"

#define SR 44100
#define BLK 512

static void write_wav_mono_as_stereo(const char *path, const float *L, int n){
    FILE *f=fopen(path,"wb"); if(!f){perror(path);return;}
    int16_t *p=(int16_t*)malloc((size_t)n*4);
    for(int i=0;i<n;i++){
        float v=L[i]; if(v>1)v=1; if(v<-1)v=-1;
        int16_t s=(int16_t)(v*32767.f);
        p[i*2]=s; p[i*2+1]=s;
    }
    uint32_t ds=(uint32_t)n*4, rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch=2;fwrite(&af,2,1,f);fwrite(&ch,2,1,f);
    uint32_t sr=SR,br=SR*4;fwrite(&sr,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=4,bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(p,2,n*2,f);
    fclose(f);free(p);
    printf("  wrote %s\n",path);
}

static PatchProgram g_bass;

static void render_single_note(int midi, int transpose, const char *path){
    MotifLibrary lib; motif_lib_init(&lib);
    /* Define a motif: single quarter note */
    { VoiceBuilder b; vb_init(&b);
      vb_note(&b, midi, 6, 7);  /* dur=6 = g_dur[6] = 1 beat */
      motif_define(&lib,"single",vb_finish(&b)); }

    Section sec; section_init(&sec,"S",4.f);
    { MotifUse u; memset(&u,0,sizeof(u));
      u=motif_use("single",0.f,1,transpose);
      section_add_track(&sec,"t",&g_bass,&u,1,1.f,0.f); }

    Song song; song_init(&song,"T",120.f,(float)SR);
    song_append(&song,"S",&sec,&lib,1,0.f,0.f,0.f,0.f);

    float tsec=song_total_seconds(&song);
    int cap=(int)(tsec*(float)SR)+SR;
    float *buf=(float*)calloc((size_t)cap*2,sizeof(float));
    SongRenderer *sr=song_renderer_new(&song);
    float blk[BLK*2]; int pos=0;
    while(!sr->done&&pos<cap){
        int c=cap-pos<BLK?cap-pos:BLK;
        song_render_block(sr,blk,c);
        for(int i=0;i<c;i++) buf[pos+i]=(blk[i*2]+blk[i*2+1])*0.5f;
        pos+=c;
    }
    song_renderer_free(sr);

    /* Print FFT peak */
    int N=SR; /* 1 second = fine resolution */
    double *re=(double*)calloc((size_t)N,sizeof(double));
    double *im=(double*)calloc((size_t)N,sizeof(double));
    for(int i=0;i<N&&i<pos;i++) re[i]=buf[i];
    /* Simple DFT for key frequencies */
    printf("  midi=%d+%d=MIDI%d  expected %.2fHz\n",
           midi, transpose, midi+transpose,
           440.0*pow(2.0,(midi+transpose-69)/12.0));
    /* Check energy at expected freq ±2Hz using Goertzel */
    double exp_f = 440.0*pow(2.0,((midi+transpose)-69.0)/12.0);
    /* Goertzel at expected freq */
    double w=2.0*M_PI*exp_f/SR;
    double coeff=2.0*cos(w);
    double s1=0,s2=0;
    int Ng=SR/2; /* 0.5s */
    for(int i=0;i<Ng&&i<pos;i++){
        double s=buf[i]+coeff*s1-s2;
        s2=s1; s1=s;
    }
    double pow_exp=s1*s1+s2*s2-coeff*s1*s2;

    /* Goertzel at actual dominant (scan 40-200Hz) */
    double best_pow=0; double best_f=0;
    for(int fi=40;fi<=500;fi++){
        double fw=2.0*M_PI*fi/SR;
        double fc=2.0*cos(fw);
        double q1=0,q2=0;
        for(int i=0;i<Ng&&i<pos;i++){
            double q=buf[i]+fc*q1-q2; q2=q1; q1=q;
        }
        double p=q1*q1+q2*q2-fc*q1*q2;
        if(p>best_pow){best_pow=p;best_f=fi;}
    }
    printf("    Expected f=%.2fHz  Goertzel_pow=%.1f\n",exp_f,pow_exp);
    printf("    Dominant f=%.0fHz  power=%.1f\n",best_f,best_pow);
    printf("    Semitone error: %.2f\n",
           12.0*log2(best_f/exp_f));

    if(path) write_wav_mono_as_stereo(path,buf,pos);
    free(buf);free(re);free(im);
}

int main(void){
    tables_init();
    { PatchBuilder b; pb_init(&b);
      int s=pb_saw(&b,REG_ONE);
      int fl=pb_lpf(&b,s,20);
      int e=pb_adsr(&b,0,6,15,8);
      pb_out(&b,pb_mul(&b,fl,e));
      g_bass=*pb_finish(&b); }

    printf("=== Single-note transpose isolation test ===\n\n");

    printf("Test A: Direct C2 (MIDI 36, transpose=0)\n");
    render_single_note(36,0,"/mnt/user-data/outputs/iso_C2_direct.wav");

    printf("\nTest B: Direct F2 (MIDI 41, transpose=0)\n");
    render_single_note(41,0,"/mnt/user-data/outputs/iso_F2_direct.wav");

    printf("\nTest C: C2 + transpose=5 (should produce F2)\n");
    render_single_note(36,5,"/mnt/user-data/outputs/iso_C2_t5.wav");

    printf("\nTest D: C2 + transpose=2 (should produce D2)\n");
    render_single_note(36,2,"/mnt/user-data/outputs/iso_C2_t2.wav");

    printf("\nTest E: C2 + transpose=7 (should produce G2)\n");
    render_single_note(36,7,"/mnt/user-data/outputs/iso_C2_t7.wav");

    return 0;
}
