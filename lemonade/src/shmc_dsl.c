/*
 * SHMC DSL Compiler — shmc_dsl.c   v8
 * Improvements (each formally verified before being merged):
 *  v1: pb_mix/pb_square/pb_osc API fixes
 *  v2: post-compile motif resolve
 *  v8: Added rest, bpf, fold keywords
 *  v3: patch_canonicalize() after every PATCH   [verify_canon_after.c 3/3 ✓]
 *  v4: patch_ir DCE + graph canon               [verify_patch_ir.c   6/6 ✓]
 *  v5: LLM output validation limits             [test_dsl_limits.py 21/21 ✓]
 *  v6: $N reg fix: DSL_REG_ONE sentinel + dsl_regs[] map  [verify_reg_mapping.c 5/5 ✓]
 *  v7: lexer recursion → loop; float→int uses lroundf    [verify_dsl_v7.c 4/4 ✓]
 */
#include "../include/shmc_dsl.h"
#include "../include/shmc_dsl_limits.h"
#include "../../layer0/include/patch_builder.h"
#include "../../layer0b/include/shmc_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ── Lexer ───────────────────────────────────────────────────────── */
#define TOK_WORD   1
#define TOK_NUM    2
#define TOK_AT     3
#define TOK_LBRACE 4
#define TOK_RBRACE 5
#define TOK_SEMI   6
#define TOK_DOLLAR 7
#define TOK_EOF    0

typedef struct { const char *src; int pos,line,kind; char text[128]; double num; } Lex;

static void skip_ws(Lex *l){
    for(;;){
        char c=l->src[l->pos];
        if(!c)return;
        if(c=='\n'){l->line++;l->pos++;continue;}
        if(isspace((unsigned char)c)){l->pos++;continue;}
        if(c=='#'||(c=='/'&&l->src[l->pos+1]=='/')){
            while(l->src[l->pos]&&l->src[l->pos]!='\n')l->pos++;
            continue;}
        break;
    }
}
/* lex_next: iterative loop — no recursion, safe against any input length.
 * v7 fix: replaced tail-call "l->pos++; return lex_next(l)" with explicit loop.
 * Prevents stack overflow when input has many consecutive unknown characters. */
static int lex_next(Lex *l){
    for(;;){
        skip_ws(l); char c=l->src[l->pos];
        if(!c){l->kind=TOK_EOF;return 0;}
        if(c=='@'){l->pos++;l->kind=TOK_AT;    return TOK_AT;}
        if(c=='{'){l->pos++;l->kind=TOK_LBRACE;return TOK_LBRACE;}
        if(c=='}'){l->pos++;l->kind=TOK_RBRACE;return TOK_RBRACE;}
        if(c==';'){l->pos++;l->kind=TOK_SEMI;  return TOK_SEMI;}
        if(c=='$'){l->pos++;l->kind=TOK_DOLLAR;return TOK_DOLLAR;}
        if(isdigit((unsigned char)c)||(c=='-'&&isdigit((unsigned char)l->src[l->pos+1]))){
            char buf[64];int i=0;if(c=='-'){buf[i++]=c;l->pos++;}
            while((isdigit((unsigned char)l->src[l->pos])||l->src[l->pos]=='.')&&i<62)
                buf[i++]=l->src[l->pos++];
            buf[i]=0; l->num=atof(buf); l->kind=TOK_NUM; return TOK_NUM;
        }
        if(isalpha((unsigned char)c)||c=='_'){
            int i=0;
            while((isalnum((unsigned char)l->src[l->pos])||l->src[l->pos]=='_')&&i<126)
                l->text[i++]=l->src[l->pos++];
            l->text[i]=0; l->kind=TOK_WORD; return TOK_WORD;
        }
        l->pos++; /* skip unknown char, loop back — no recursion */
    }
}
static int ek(Lex*l,int k,char*e,int es){
    if(lex_next(l)!=k){snprintf(e,es,"line %d: expected token %d, got %d '%s'",l->line,k,l->kind,l->text);return -1;}return 0;}
static int ew(Lex*l,char*o,int os,char*e,int es){
    if(lex_next(l)!=TOK_WORD){snprintf(e,es,"line %d: expected word",l->line);return -1;}strncpy(o,l->text,os-1);o[os-1]=0;return 0;}
static int en(Lex*l,double*o,char*e,int es){
    if(lex_next(l)!=TOK_NUM){snprintf(e,es,"line %d: expected number",l->line);return -1;}*o=l->num;return 0;}

/* Sentinel: parse_reg returns DSL_REG_ONE for "ONE" keyword.
 * REG_ONE==3 collides with $3 index, so use a distinct large-negative sentinel.
 * DSL_REG macro maps DSL_REG_ONE → actual REG_ONE for pb_* calls. */
#define DSL_REG_ONE  (-100)

static int parse_reg(Lex*l,char*e,int es){
    int k=lex_next(l);
    if(k==TOK_DOLLAR){double n;if(en(l,&n,e,es)<0)return -1;return(int)n;}
    if(k==TOK_WORD){if(strcmp(l->text,"ONE")==0)return DSL_REG_ONE;
        snprintf(e,es,"line %d: unknown register '%s'",l->line,l->text);return -1;}
    snprintf(e,es,"line %d: expected register ($N or ONE)",l->line);return -1;
}

/* ── PATCH ───────────────────────────────────────────────────────── */
/*
 * Register mapping fix (v6):
 *   $N in DSL means "the N-th operation's output register".
 *   Previously parse_reg() returned N directly → raw reg N = REG_FREQ/VEL/TIME.
 *   Fix: maintain dsl_regs[N] = actual_reg_returned_by_pb_*
 *        translate $N through this map before passing to pb_*.
 *
 * Verified: verify_reg_mapping.c 5/5 PASSED, then regression 35/35.
 */
#define DSL_MAX_OPS_PER_PATCH 64
static int parse_patch(Lex*l,ShmcWorld*w,char*e,int es){
    if(w->n_patches>=DSL_MAX_PATCHES){snprintf(e,es,"too many patches (max %d)",DSL_MAX_PATCHES);return -1;}
    char name[32]; if(ew(l,name,32,e,es)<0)return -1;
    if(ek(l,TOK_LBRACE,e,es)<0)return -1;
    PatchBuilder b; pb_init(&b);
    /* dsl_regs[N] = actual PatchBuilder register assigned to DSL op $N */
    int dsl_regs[DSL_MAX_OPS_PER_PATCH];
    int n_dsl_regs=0;
    int n_ops=0;

#define DSL_REG(raw) ((raw)==DSL_REG_ONE ? REG_ONE : \
    ((raw)<0||(raw)>=n_dsl_regs ? \
        (snprintf(e,es,"line %d: $%d not yet defined (%d ops emitted so far)",\
                  l->line,(raw),n_dsl_regs),-1) \
        : dsl_regs[(raw)]))
#define EMIT_REG(rv) do{ if((rv)<0)return -1; \
    if(n_dsl_regs<DSL_MAX_OPS_PER_PATCH) dsl_regs[n_dsl_regs++]=(rv); }while(0)

    while(1){
        int k=lex_next(l); if(k==TOK_RBRACE)break;
        if(k!=TOK_WORD){snprintf(e,es,"line %d: expected patch op",l->line);return -1;}
        if(++n_ops>DSL_LIMIT_MAX_PATCH_OPS){
            snprintf(e,es,"PATCH '%s': too many ops (max %d)",name,DSL_LIMIT_MAX_PATCH_OPS);return -1;}
        char op[32]; strncpy(op,l->text,31); op[31]=0; int r=-1;

        if(strcmp(op,"osc")==0||strcmp(op,"sin")==0){
            skip_ws(l); char typ[16]="sin";
            if(isalpha((unsigned char)l->src[l->pos])){if(ew(l,typ,16,e,es)<0)return -1;}
            int raw=parse_reg(l,e,es);if(raw==-1)return -1;
            int fr=DSL_REG(raw);if(fr<0)return -1;
            if(strcmp(typ,"saw")==0)     r=pb_saw(&b,fr);
            else if(strcmp(typ,"sqr")==0||strcmp(typ,"square")==0) r=pb_square(&b,fr);
            else if(strcmp(typ,"tri")==0) r=pb_tri(&b,fr);
            else if(strcmp(typ,"noise")==0){(void)fr;r=pb_noise(&b);}
            else                          r=pb_osc(&b,fr);
            EMIT_REG(r);
        }else if(strcmp(op,"saw")==0){
            int raw=parse_reg(l,e,es);if(raw==-1)return -1;
            int fr=DSL_REG(raw);if(fr<0)return -1;
            r=pb_saw(&b,fr); EMIT_REG(r);
        }else if(strcmp(op,"tri")==0){
            int raw=parse_reg(l,e,es);if(raw==-1)return -1;
            int fr=DSL_REG(raw);if(fr<0)return -1;
            r=pb_tri(&b,fr); EMIT_REG(r);
        }else if(strcmp(op,"noise")==0){
            r=pb_noise(&b); EMIT_REG(r);
        }else if(strcmp(op,"fm")==0){
            int rawc=parse_reg(l,e,es);if(rawc==-1)return -1;
            int rawm=parse_reg(l,e,es);if(rawm==-1)return -1;
            int car=DSL_REG(rawc);if(car<0)return -1;
            int mod=DSL_REG(rawm);if(mod<0)return -1;
            double dep;if(en(l,&dep,e,es)<0)return -1;
            r=pb_fm(&b,car,mod,(int)lroundf((float)dep)); EMIT_REG(r); /* v7: round */
        }else if(strcmp(op,"lpf")==0){
            int raws=parse_reg(l,e,es);if(raws==-1)return -1;
            int s2=DSL_REG(raws);if(s2<0)return -1;
            double ci;if(en(l,&ci,e,es)<0)return -1;
            r=pb_lpf(&b,s2,(int)lroundf((float)ci)); EMIT_REG(r); /* v7: round not truncate */
        }else if(strcmp(op,"hpf")==0){
            int raws=parse_reg(l,e,es);if(raws==-1)return -1;
            int s2=DSL_REG(raws);if(s2<0)return -1;
            double ci;if(en(l,&ci,e,es)<0)return -1;
            r=pb_hpf(&b,s2,(int)lroundf((float)ci)); EMIT_REG(r);
        }else if(strcmp(op,"bpf")==0){
            /* v8: band-pass filter — signal, cutoff, q */
            int raws=parse_reg(l,e,es);if(raws==-1)return -1;
            int s2=DSL_REG(raws);if(s2<0)return -1;
            double ci,qi;if(en(l,&ci,e,es)<0||en(l,&qi,e,es)<0)return -1;
            r=pb_bpf(&b,s2,(int)lroundf((float)ci),(int)lroundf((float)qi)); EMIT_REG(r);
        }else if(strcmp(op,"fold")==0){
            /* v8: wavefolder — adds timbral complexity / harmonic saturation */
            int rawa=parse_reg(l,e,es);if(rawa==-1)return -1;
            int a2=DSL_REG(rawa);if(a2<0)return -1;
            r=pb_fold(&b,a2); EMIT_REG(r);
        }else if(strcmp(op,"adsr")==0){
            double a,d,s,rv;
            if(en(l,&a,e,es)<0||en(l,&d,e,es)<0||en(l,&s,e,es)<0||en(l,&rv,e,es)<0)return -1;
            r=pb_adsr(&b,(int)lroundf((float)a),(int)lroundf((float)d),
                          (int)lroundf((float)s),(int)lroundf((float)rv)); EMIT_REG(r); /* v7 */
        }else if(strcmp(op,"mul")==0){
            int rawa=parse_reg(l,e,es);if(rawa==-1)return -1;
            int rawb=parse_reg(l,e,es);if(rawb==-1)return -1;
            int a2=DSL_REG(rawa);if(a2<0)return -1;
            int bv=DSL_REG(rawb);if(bv<0)return -1;
            r=pb_mul(&b,a2,bv); EMIT_REG(r);
        }else if(strcmp(op,"add")==0){
            int rawa=parse_reg(l,e,es);if(rawa==-1)return -1;
            int rawb=parse_reg(l,e,es);if(rawb==-1)return -1;
            int a2=DSL_REG(rawa);if(a2<0)return -1;
            int bv=DSL_REG(rawb);if(bv<0)return -1;
            r=pb_mix(&b,a2,bv,REG_ONE,REG_ONE); EMIT_REG(r);
        }else if(strcmp(op,"tanh")==0){
            int rawa=parse_reg(l,e,es);if(rawa==-1)return -1;
            int a2=DSL_REG(rawa);if(a2<0)return -1;
            r=pb_tanh(&b,a2); EMIT_REG(r);
        }else if(strcmp(op,"clip")==0){
            int rawa=parse_reg(l,e,es);if(rawa==-1)return -1;
            int a2=DSL_REG(rawa);if(a2<0)return -1;
            r=pb_clip(&b,a2); EMIT_REG(r);
        }else if(strcmp(op,"out")==0){
            int raws=parse_reg(l,e,es);if(raws==-1)return -1;
            int s2=DSL_REG(raws);if(s2<0)return -1;
            pb_out(&b,s2); r=0;
            /* 'out' does not produce a new register — no EMIT_REG */
        }else{snprintf(e,es,"line %d: unknown patch op '%s'",l->line,op);return -1;}
        (void)r;
        skip_ws(l); if(l->src[l->pos]==';')l->pos++;
    }
#undef DSL_REG
#undef EMIT_REG

    const PatchProgram*pp=pb_finish(&b);
    if(!pp){snprintf(e,es,"PATCH '%s': missing 'out' instruction",name);return -1;}
    int idx=w->n_patches++;
    w->patches[idx]=*pp;
    patch_canonicalize(&w->patches[idx]);
    snprintf(w->patch_names[idx],32,"%s",name);
    return 0;
}

/* ── MOTIF ───────────────────────────────────────────────────────── */
static int parse_motif(Lex*l,ShmcWorld*w,char*e,int es){
    char name[MOTIF_NAME_LEN]; if(ew(l,name,MOTIF_NAME_LEN,e,es)<0)return -1;
    if(ek(l,TOK_LBRACE,e,es)<0)return -1;
    VoiceBuilder vb; vb_init(&vb); int nn=0;
    while(1){
        int k=lex_next(l); if(k==TOK_RBRACE)break;
        if(k!=TOK_WORD){
            snprintf(e,es,"line %d: expected 'note' or 'rest'",l->line);return -1;}
        if(nn>=DSL_LIMIT_MAX_NOTES_PER_MOTIF){
            snprintf(e,es,"MOTIF '%s': too many notes (max %d)",name,DSL_LIMIT_MAX_NOTES_PER_MOTIF);return -1;}
        if(strcmp(l->text,"rest")==0){
            /* v8: rest — advances time without a note */
            double d; if(en(l,&d,e,es)<0)return -1;
            if((int)d<0||(int)d>DSL_LIMIT_DUR_MAX){
                snprintf(e,es,"MOTIF '%s': rest duration %d out of range [0,%d]",
                         name,(int)d,DSL_LIMIT_DUR_MAX);return -1;}
            vb_rest(&vb,(int)d); nn++;
        } else if(strcmp(l->text,"note")==0){
            double p,d,v;
            if(en(l,&p,e,es)<0||en(l,&d,e,es)<0||en(l,&v,e,es)<0)return -1;
            if((int)p<DSL_LIMIT_MIDI_MIN||(int)p>DSL_LIMIT_MIDI_MAX){
                snprintf(e,es,"MOTIF '%s': MIDI pitch %d out of range [%d,%d]",
                         name,(int)p,DSL_LIMIT_MIDI_MIN,DSL_LIMIT_MIDI_MAX);return -1;}
            if((int)d<0||(int)d>DSL_LIMIT_DUR_MAX){
                snprintf(e,es,"MOTIF '%s': duration index %d out of range [0,%d]",
                         name,(int)d,DSL_LIMIT_DUR_MAX);return -1;}
            if((int)v<0||(int)v>DSL_LIMIT_VEL_MAX){
                snprintf(e,es,"MOTIF '%s': velocity %d out of range [0,%d]",
                         name,(int)v,DSL_LIMIT_VEL_MAX);return -1;}
            vb_note(&vb,(int)p,(int)d,(int)v); nn++;
        } else {
            snprintf(e,es,"line %d: expected 'note' or 'rest', got '%s'",l->line,l->text);return -1;
        }
        skip_ws(l); if(l->src[l->pos]==';')l->pos++;
    }
    if(nn==0){snprintf(e,es,"MOTIF '%s': no notes defined",name);return -1;}
    const VoiceProgram*vp=vb_finish(&vb);
    if(!vp){snprintf(e,es,"vb_finish failed for MOTIF '%s'",name);return -1;}
    if(motif_define(w->lib,name,vp)<0){
        snprintf(e,es,"motif_define failed for '%s' (duplicate?)",name);return -1;}
    return 0;
}

/* ── SECTION ─────────────────────────────────────────────────────── */
typedef struct{char motif[MOTIF_NAME_LEN],patch[32];float beat,vel_scale,time_scale;int repeat,transpose;}UL;

static int find_patch(const ShmcWorld*w,const char*n){
    for(int i=0;i<w->n_patches;i++) if(strcmp(w->patch_names[i],n)==0) return i;
    return -1;
}

static int parse_section(Lex*l,ShmcWorld*w,char*e,int es){
    if(w->n_sections>=DSL_MAX_SECTIONS){snprintf(e,es,"too many sections (max %d)",DSL_MAX_SECTIONS);return -1;}
    char name[SECTION_NAME_LEN]; if(ew(l,name,SECTION_NAME_LEN,e,es)<0)return -1;
    double len; if(en(l,&len,e,es)<0)return -1;
    if((float)len<=0.f||(float)len>DSL_LIMIT_MAX_SECTION_BEATS){
        snprintf(e,es,"SECTION '%s': length %.1f out of range (0,%.0f]",
                 name,len,(double)DSL_LIMIT_MAX_SECTION_BEATS);return -1;}
    if(ek(l,TOK_LBRACE,e,es)<0)return -1;
    int idx=w->n_sections++;
    section_init(&w->sections[idx],name,(float)len);
    snprintf(w->section_names[idx],SECTION_NAME_LEN,"%s",name);

    UL uses[DSL_LIMIT_MAX_USES_PER_SECTION]; int nu=0;
    while(1){
        int k=lex_next(l); if(k==TOK_RBRACE)break;
        if(k!=TOK_WORD||strcmp(l->text,"use")!=0){
            snprintf(e,es,"line %d: expected 'use'",l->line);return -1;}
        if(nu>=DSL_LIMIT_MAX_USES_PER_SECTION){
            snprintf(e,es,"SECTION '%s': too many use lines (max %d)",
                     name,DSL_LIMIT_MAX_USES_PER_SECTION);return -1;}
        UL*u=&uses[nu++]; memset(u,0,sizeof(*u));
        u->repeat=1; u->vel_scale=1.f; u->time_scale=1.f;
        snprintf(u->patch,32,"%s",w->n_patches>0?w->patch_names[0]:"");
        if(ew(l,u->motif,MOTIF_NAME_LEN,e,es)<0)return -1;
        if(ek(l,TOK_AT,e,es)<0)return -1;
        double beat; if(en(l,&beat,e,es)<0)return -1; u->beat=(float)beat;
        while(1){
            skip_ws(l); char c=l->src[l->pos]; if(c==';'||c=='}'||!c)break;
            if(c=='x'){
                l->pos++; double n; if(en(l,&n,e,es)<0)return -1;
                if((int)n<1||(int)n>DSL_LIMIT_MAX_REPEAT){
                    snprintf(e,es,"line %d: repeat x%d out of range [1,%d]",
                             l->line,(int)n,DSL_LIMIT_MAX_REPEAT);return -1;}
                u->repeat=(int)n; continue;
            }
            int k2=lex_next(l); if(k2!=TOK_WORD)break;
            char mod[16]; strncpy(mod,l->text,15); mod[15]=0;
            if(strcmp(mod,"patch")==0){if(ew(l,u->patch,32,e,es)<0)return -1;continue;}
            if(strcmp(mod,"t")!=0&&strcmp(mod,"v")!=0&&strcmp(mod,"ts")!=0){
                l->pos-=(int)strlen(mod); break;}
            skip_ws(l); if(l->src[l->pos]=='=')l->pos++;
            double val; if(en(l,&val,e,es)<0)return -1;
            if(strcmp(mod,"t")==0){
                if((int)val<DSL_LIMIT_TRANSPOSE_MIN||(int)val>DSL_LIMIT_TRANSPOSE_MAX){
                    snprintf(e,es,"line %d: transpose t=%d out of range [%d,%d]",
                             l->line,(int)val,DSL_LIMIT_TRANSPOSE_MIN,DSL_LIMIT_TRANSPOSE_MAX);return -1;}
                u->transpose=(int)val;
            }else if(strcmp(mod,"v")==0){
                if((float)val<DSL_LIMIT_VEL_SCALE_MIN||(float)val>DSL_LIMIT_VEL_SCALE_MAX){
                    snprintf(e,es,"line %d: vel_scale v=%.2f out of range [%.1f,%.1f]",
                             l->line,val,(double)DSL_LIMIT_VEL_SCALE_MIN,(double)DSL_LIMIT_VEL_SCALE_MAX);return -1;}
                u->vel_scale=(float)val;
            }else if(strcmp(mod,"ts")==0){
                if((float)val<DSL_LIMIT_TIME_SCALE_MIN||(float)val>DSL_LIMIT_TIME_SCALE_MAX){
                    snprintf(e,es,"line %d: time_scale ts=%.2f out of range [%.2f,%.2f]",
                             l->line,val,(double)DSL_LIMIT_TIME_SCALE_MIN,(double)DSL_LIMIT_TIME_SCALE_MAX);return -1;}
                u->time_scale=(float)val;
            }
        }
        skip_ws(l); if(l->src[l->pos]==';')l->pos++;
    }
    char seen[SECTION_MAX_TRACKS][32]; int ns=0;
    for(int i=0;i<nu;i++){
        int found=0;for(int j=0;j<ns;j++)if(strcmp(seen[j],uses[i].patch)==0){found=1;break;}
        if(!found&&ns<SECTION_MAX_TRACKS)snprintf(seen[ns++],32,"%s",uses[i].patch);}
    for(int pi=0;pi<ns;pi++){
        int pidx=find_patch(w,seen[pi]);
        if(pidx<0){snprintf(e,es,"SECTION '%s': patch '%s' not defined",name,seen[pi]);return -1;}
        MotifUse mus[SECTION_MAX_USES]; int nm=0;
        for(int i=0;i<nu&&nm<SECTION_MAX_USES;i++){
            if(strcmp(uses[i].patch,seen[pi])!=0)continue;
            UL*u=&uses[i];
            MotifUse mu=motif_use(u->motif,u->beat,u->repeat,u->transpose);
            mu.vel_scale=u->vel_scale; mu.time_scale=u->time_scale; mus[nm++]=mu;}
        char tname[32]; snprintf(tname,32,"trk%d",pi);
        if(section_add_track(&w->sections[idx],tname,&w->patches[pidx],mus,nm,1.f,0.f)<0){
            snprintf(e,es,"section_add_track failed (patch '%s')",seen[pi]);return -1;}}
    return 0;
}

/* ── SONG ────────────────────────────────────────────────────────── */
static int find_sec(const ShmcWorld*w,const char*n){
    for(int i=0;i<w->n_sections;i++) if(strcmp(w->section_names[i],n)==0) return i;
    return -1;
}

static int parse_song(Lex*l,ShmcWorld*w,char*e,int es){
    if(w->n_songs>=DSL_MAX_SONGS){snprintf(e,es,"too many songs (max %d)",DSL_MAX_SONGS);return -1;}
    char name[SONG_NAME_LEN]; if(ew(l,name,SONG_NAME_LEN,e,es)<0)return -1;
    double bpm; if(en(l,&bpm,e,es)<0)return -1;
    if((float)bpm<DSL_LIMIT_BPM_MIN||(float)bpm>DSL_LIMIT_BPM_MAX){
        snprintf(e,es,"SONG '%s': BPM %.1f out of range [%.0f,%.0f]",
                 name,bpm,(double)DSL_LIMIT_BPM_MIN,(double)DSL_LIMIT_BPM_MAX);return -1;}
    if(ek(l,TOK_LBRACE,e,es)<0)return -1;
    int idx=w->n_songs++; song_init(&w->songs[idx],name,(float)bpm,44100.f);
    int n_plays=0;
    while(1){
        int k=lex_next(l); if(k==TOK_RBRACE)break;
        if(k!=TOK_WORD||strcmp(l->text,"play")!=0){
            snprintf(e,es,"line %d: expected 'play'",l->line);return -1;}
        if(++n_plays>DSL_LIMIT_MAX_PLAYS_PER_SONG){
            snprintf(e,es,"SONG '%s': too many play lines (max %d)",name,DSL_LIMIT_MAX_PLAYS_PER_SONG);return -1;}
        char sname[SECTION_NAME_LEN]; if(ew(l,sname,SECTION_NAME_LEN,e,es)<0)return -1;
        int si=find_sec(w,sname);
        if(si<0){snprintf(e,es,"SONG '%s': section '%s' not defined",name,sname);return -1;}
        int repeat=1; float fi=0,fo=0,xf=0,gap=0;
        while(1){
            skip_ws(l); char c=l->src[l->pos]; if(c==';'||c=='}'||!c)break;
            if(c=='x'){
                l->pos++; double n; if(en(l,&n,e,es)<0)return -1;
                if((int)n<1||(int)n>DSL_LIMIT_MAX_SONG_REPEAT){
                    snprintf(e,es,"line %d: song repeat x%d out of range [1,%d]",
                             l->line,(int)n,DSL_LIMIT_MAX_SONG_REPEAT);return -1;}
                repeat=(int)n; continue;}
            int k2=lex_next(l); if(k2!=TOK_WORD)break;
            /* Unknown word = next 'play' line. Push back by rewinding pos. */
            if(strcmp(l->text,"fi")!=0 && strcmp(l->text,"fo")!=0 &&
               strcmp(l->text,"xf")!=0 && strcmp(l->text,"gap")!=0){
                l->pos -= (int)strlen(l->text); break;}
            skip_ws(l); if(l->src[l->pos]=='=')l->pos++;
            double val; if(en(l,&val,e,es)<0)return -1;
            if(strcmp(l->text,"fi")==0)fi=(float)val;
            else if(strcmp(l->text,"fo")==0)fo=(float)val;
            else if(strcmp(l->text,"xf")==0)xf=(float)val;
            else if(strcmp(l->text,"gap")==0)gap=(float)val;
        }
        skip_ws(l); if(l->src[l->pos]==';')l->pos++;
        song_append(&w->songs[idx],sname,&w->sections[si],w->lib,repeat,fi,fo,xf,gap);
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */
int shmc_dsl_compile(const char*src,ShmcWorld*w,char*e,int es){
    memset(w,0,sizeof(*w));
    w->lib=(MotifLibrary*)calloc(1,sizeof(MotifLibrary));
    if(!w->lib){snprintf(e,es,"OOM: MotifLibrary");return -1;}
    motif_lib_init(w->lib);
    Lex l; memset(&l,0,sizeof(l)); l.src=src; l.line=1;
    while(1){
        int k=lex_next(&l); if(k==TOK_EOF)break;
        if(k!=TOK_WORD){snprintf(e,es,"line %d: expected keyword",l.line);return -1;}
        if(strcmp(l.text,"PATCH")==0){if(parse_patch(&l,w,e,es)<0)return -1;}
        else if(strcmp(l.text,"MOTIF")==0){if(parse_motif(&l,w,e,es)<0)return -1;}
        else if(strcmp(l.text,"SECTION")==0){if(parse_section(&l,w,e,es)<0)return -1;}
        else if(strcmp(l.text,"SONG")==0){if(parse_song(&l,w,e,es)<0)return -1;}
        else{snprintf(e,es,"line %d: unknown keyword '%s'",l.line,l.text);return -1;}
    }
    if(w->n_songs==0){snprintf(e,es,"no SONG defined");return -1;}
    /* Post-compile motif resolve: catch dangling names before render */
    for(int si=0;si<w->n_sections;si++){
        Section*sec=&w->sections[si];
        for(int ti=0;ti<sec->n_tracks;ti++){
            SectionTrack*trk=&sec->tracks[ti];
            if(motif_resolve_uses(w->lib,trk->uses,trk->n_uses,e,es)<0)return -1;}}
    return 0;
}
void shmc_world_free(ShmcWorld*w){if(w->lib){free(w->lib);w->lib=NULL;}}

int shmc_world_render(ShmcWorld*w,float**out,int*n_frames,float sr){
    if(!w->n_songs)return -1;
    Song*s=&w->songs[0]; s->sr=sr;
    float tot=song_total_seconds(s);
    int cap=(int)(tot*sr)+(int)(2*sr)+1;
    float*buf=(float*)calloc((size_t)cap*2,sizeof(float)); if(!buf)return -1;
    SongRenderer*rend=song_renderer_new(s); if(!rend){free(buf);return -1;}
    float blk[1024]; int pos=0;
    while(!rend->done&&pos<cap){
        int ch=512; if(pos+ch>cap)ch=cap-pos;
        song_render_block(rend,blk,ch);
        for(int i=0;i<ch;i++)buf[pos+i]=(blk[i*2]+blk[i*2+1])*.5f;
        pos+=ch;}
    song_renderer_free(rend); *out=buf; *n_frames=pos; return 0;
}
int shmc_world_render_n(ShmcWorld*w,float**out,int*n_frames,float sr,int max_frames){
    if(!w->n_songs)return -1;
    if(max_frames<=0) return shmc_world_render(w,out,n_frames,sr);
    Song*s=&w->songs[0]; s->sr=sr;
    int cap=max_frames;
    float*buf=(float*)calloc((size_t)cap*2,sizeof(float)); if(!buf)return -1;
    SongRenderer*rend=song_renderer_new(s); if(!rend){free(buf);return -1;}
    float blk[1024]; int pos=0;
    while(!rend->done&&pos<cap){
        int ch=512; if(pos+ch>cap)ch=cap-pos;
        song_render_block(rend,blk,ch);
        for(int i=0;i<ch;i++)buf[pos+i]=(blk[i*2]+blk[i*2+1])*.5f;
        pos+=ch;}
    song_renderer_free(rend); *out=buf; *n_frames=pos; return 0;
}
int shmc_write_wav(const char*path,const float*buf,int nf,int nch,float sr){
    FILE*f=fopen(path,"wb"); if(!f)return -1;
    int16_t*s16=(int16_t*)malloc((size_t)nf*(size_t)nch*2); if(!s16){fclose(f);return -1;}
    for(int i=0;i<nf;i++)for(int c=0;c<nch;c++){
        float v=buf[i]; if(v>1.f)v=1.f; if(v<-1.f)v=-1.f;
        s16[i*nch+c]=(int16_t)(v*32767.f);}
    uint32_t ds=(uint32_t)nf*(uint32_t)nch*2,rs=36+ds;
    fwrite("RIFF",1,4,f);fwrite(&rs,4,1,f);fwrite("WAVEfmt ",1,8,f);
    uint32_t cs=16;fwrite(&cs,4,1,f);
    uint16_t af=1,ch2=(uint16_t)nch;fwrite(&af,2,1,f);fwrite(&ch2,2,1,f);
    uint32_t sri=(uint32_t)sr,br=(uint32_t)(sr*(float)nch*2.f);fwrite(&sri,4,1,f);fwrite(&br,4,1,f);
    uint16_t ba=(uint16_t)(nch*2),bi=16;fwrite(&ba,2,1,f);fwrite(&bi,2,1,f);
    fwrite("data",1,4,f);fwrite(&ds,4,1,f);fwrite(s16,2,(size_t)nf*(size_t)nch,f);
    free(s16);fclose(f);return 0;
}
