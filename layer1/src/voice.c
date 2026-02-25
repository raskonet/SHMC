#include "../include/voice.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

const float VEL_TABLE[16] = {
    /* indices 0-15 → linear 0.0625..1.0, covering full DSL vel range */
    0.0625f, 0.125f, 0.1875f, 0.250f,
    0.3125f, 0.375f, 0.4375f, 0.500f,
    0.5625f, 0.625f, 0.6875f, 0.750f,
    0.8125f, 0.875f, 0.9375f, 1.000f
};

/* ============================================================
   patch_release  — trigger ADSR release without restarting note
   Uses the symbolic OP_ADSR constant (no magic numbers).
   ============================================================ */
void patch_release(Patch *p, const PatchProgram *prog){
    for(int k=0;k<prog->n_instrs;k++){
        if(INSTR_OP(prog->code[k])==OP_ADSR){
            int sb=(k*4)%MAX_STATE;
            p->st.state[sb+0]=3.0f; /* stage = release */
            p->st.state[sb+2]=0.0f; /* reset timer */
        }
    }
}

/* ============================================================
   Compilation
   ============================================================ */

static int ev_push(EventStream *es, uint64_t sample, EvType type,
                   uint8_t pitch, float vel){
    if(es->n>=VOICE_MAX_EVENTS) return -1;
    Event *e=&es->events[es->n++];
    e->sample   =sample;
    e->type     =type;
    e->pitch    =pitch;
    e->velocity =vel;
    return 0;
}

typedef struct {
    const VInstr *code;
    EventStream  *es;
    float         spb;       /* samples per beat */
    uint64_t     *sample;    /* current position, in samples */
    int           last_off;  /* index of most recent NOTE_OFF in es->events */
    char         *err;
    int           err_sz;
} CompileCtx;

static int compile_range(CompileCtx *ctx, int lo, int hi);

static int compile_range(CompileCtx *ctx, int lo, int hi){
    extern const float g_dur[7];
    int i=lo;
    while(i<hi){
        VInstr  vi    = ctx->code[i];
        uint8_t op    = VI_OP(vi);
        uint8_t pitch = VI_PITCH(vi);
        uint8_t di    = VI_DUR(vi);
        uint8_t veli  = VI_VEL(vi);

        uint64_t dur_samp = (uint64_t)((di<7?g_dur[di]:g_dur[4])*ctx->spb + 0.5f);
        float    vel      = (veli<16)?VEL_TABLE[veli]:0.75f;

        switch(op){
        case VI_NOTE:
            if(ev_push(ctx->es,*ctx->sample,EV_NOTE_ON, pitch,vel)<0)
                {snprintf(ctx->err,ctx->err_sz,"event overflow");return -1;}
            ctx->last_off=ctx->es->n;
            if(ev_push(ctx->es,*ctx->sample+dur_samp,EV_NOTE_OFF,pitch,vel)<0)
                {snprintf(ctx->err,ctx->err_sz,"event overflow");return -1;}
            *ctx->sample+=dur_samp;
            break;

        case VI_REST:
            *ctx->sample+=dur_samp;
            break;

        case VI_TIE:
            /* Extend the most-recently emitted NOTE_OFF */
            if(ctx->last_off>=0 && ctx->last_off<ctx->es->n){
                ctx->es->events[ctx->last_off].sample+=dur_samp;
            }
            *ctx->sample+=dur_samp;
            break;

        case VI_GLIDE: {
            /* Cancel previous note-off, fire new note-on immediately (legato),
               schedule new note-off at end of this duration. */
            if(ctx->last_off>=0 && ctx->last_off<ctx->es->n){
                /* Push old NOTE_OFF to now so the renderer triggers release
                   right before the new note-on at the same sample. */
                ctx->es->events[ctx->last_off].sample = *ctx->sample;
                ctx->es->events[ctx->last_off].type   = EV_GLIDE; /* special */
            }
            if(ev_push(ctx->es,*ctx->sample,EV_NOTE_ON,pitch,vel)<0)
                {snprintf(ctx->err,ctx->err_sz,"event overflow");return -1;}
            ctx->last_off=ctx->es->n;
            if(ev_push(ctx->es,*ctx->sample+dur_samp,EV_NOTE_OFF,pitch,vel)<0)
                {snprintf(ctx->err,ctx->err_sz,"event overflow");return -1;}
            *ctx->sample+=dur_samp;
            break;
        }

        case VI_REPEAT_BEGIN: {
            int depth=1, end_i=-1;
            for(int j=i+1;j<hi;j++){
                uint8_t jop=VI_OP(ctx->code[j]);
                if(jop==VI_REPEAT_BEGIN) depth++;
                else if(jop==VI_REPEAT_END){depth--;if(!depth){end_i=j;break;}}
            }
            if(end_i<0){snprintf(ctx->err,ctx->err_sz,"unmatched REPEAT_BEGIN at %d",i);return -1;}
            int count=(int)VI_VEL(ctx->code[end_i]);
            if(count<1) count=1;
            for(int r=0;r<count;r++)
                if(compile_range(ctx,i+1,end_i)<0) return -1;
            i=end_i;
            break;
        }
        case VI_REPEAT_END: break;
        default: break;
        }
        i++;
    }
    return 0;
}

int voice_compile(const VoiceProgram *vp, EventStream *es,
                  float sr, float bpm, char *err_out, int err_sz){
    if(!err_out||err_sz<=0){ static char _eb[64]; err_out=_eb; err_sz=64; }
    err_out[0]='\0';
    memset(es,0,sizeof(*es));

    extern const float g_dur[7];
    float spb = sr * 60.0f / bpm;  /* samples per beat */
    uint64_t sample=0;

    CompileCtx ctx={vp->code, es, spb, &sample, -1, err_out, err_sz};
    int r=compile_range(&ctx,0,vp->n);
    es->total_samples=sample;
    es->total_beats  =(float)sample/spb;
    return r;
}

/* ============================================================
   VoiceRenderer
   ============================================================ */

void voice_renderer_init(VoiceRenderer *vr,
                         const EventStream  *es,
                         const PatchProgram *patch,
                         float sr){
    memset(vr,0,sizeof(*vr));
    vr->es          =es;
    vr->patch_prog  =patch;
    vr->sr          =sr;
    vr->sample_pos  =0;
    vr->ev_cursor   =0;
    vr->has_active  =0;
    vr->has_tail    =0;
    vr->tail_env    =0.0f;
    vr->done        =0;
}

int voice_render_block(VoiceRenderer *vr, float *out, int n){
    if(vr->done){ memset(out,0,n*sizeof(float)); return 1; }

    memset(out,0,n*sizeof(float));

    for(int s=0;s<n;s++){
        uint64_t cur=vr->sample_pos;

        /* Dispatch all events whose sample <= cur */
        while(vr->ev_cursor<vr->es->n){
            const Event *ev=&vr->es->events[vr->ev_cursor];
            if(ev->sample>cur) break;

            if(ev->type==EV_NOTE_ON){
                /* Move active → tail (to let it finish its release) */
                if(vr->has_active){
                    vr->tail      =vr->active;
                    vr->has_tail  =1;
                    vr->tail_env  =1.0f;
                    patch_release(&vr->tail,vr->patch_prog);
                }
                patch_note_on(&vr->active,vr->patch_prog,
                              vr->sr,(int)ev->pitch,ev->velocity);
                vr->has_active=1;
            } else if(ev->type==EV_NOTE_OFF){
                if(vr->has_active){
                    patch_release(&vr->active,vr->patch_prog);
                }
            } else if(ev->type==EV_GLIDE){
                /* Glide: move active to tail, new note-on fires next */
                if(vr->has_active){
                    vr->tail     =vr->active;
                    vr->has_tail =1;
                    vr->tail_env =1.0f;
                    patch_release(&vr->tail,vr->patch_prog);
                    vr->has_active=0;
                }
            }
            vr->ev_cursor++;
        }

        /* Mix active + tail */
        float samp=0.0f;

        if(vr->has_tail){
            float tsamp=0.0f;
            patch_step(&vr->tail,&tsamp,1);
            /* Leaky envelope tracker for silence detection */
            float av=fabsf(tsamp);
            vr->tail_env=vr->tail_env*0.9999f+(av>vr->tail_env?av-vr->tail_env:0.f);
            if(vr->tail_env<1e-5f) vr->has_tail=0;
            else samp+=tsamp;
        }
        if(vr->has_active){
            float asamp=0.0f;
            patch_step(&vr->active,&asamp,1);
            samp+=asamp;
        }

        out[s]=samp;
        vr->sample_pos++;
    }

    /* Done when all events consumed, no active, no tail */
    if(vr->ev_cursor>=vr->es->n && !vr->has_tail){
        if(!vr->has_active){ vr->done=1; return 1; }
        /* Check if active has gone silent (post-release) */
        float probe=0.0f;
        patch_step(&vr->active,&probe,1);
        vr->sample_pos++;
        if(fabsf(probe)<1e-5f){ vr->done=1; return 1; }
    }
    return 0;
}

/* ================================================================
   T4-1: TempoMap-aware compilation
   T4-3: Repeat expansion hard limit
   T4-2: Structural hash
   ================================================================ */

#include "../../layer0b/include/tempo_map.h"

/* FNV-1a helpers (same as patch_meta.c — self-contained) */
#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME  1099511628211ULL
static uint64_t fnv1a_v1(const void *data, size_t n){
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = FNV64_OFFSET;
    for(size_t i=0;i<n;i++){ h^=(uint64_t)p[i]; h*=FNV64_PRIME; }
    return h;
}

uint64_t hash_voice(const VoiceProgram *vp){
    return fnv1a_v1(vp->code, (size_t)vp->n * sizeof(VInstr));
}

/* ---- Tempo-aware compile context ---- */
typedef struct {
    const VInstr  *code;
    EventStream   *es;
    const TempoMap *tm;
    double         cur_beat;   /* current beat position */
    double         start_beat; /* start beat of this voice in the global map */
    int            last_off;
    char          *err;
    int            err_sz;
    int            total_events; /* T4-3: guard against explosion */
} TCompileCtx;

static int tcompile_range(TCompileCtx *ctx, int lo, int hi);

static int tcompile_range(TCompileCtx *ctx, int lo, int hi){
    extern const float g_dur[7];
    int i = lo;
    while(i < hi){
        VInstr  vi    = ctx->code[i];
        uint8_t op    = VI_OP(vi);
        uint8_t pitch = VI_PITCH(vi);
        uint8_t di    = VI_DUR(vi);
        uint8_t veli  = VI_VEL(vi);

        double dur_beats = (double)(di < 7 ? g_dur[di] : g_dur[4]);
        float  vel       = (veli < 16) ? VEL_TABLE[veli] : 0.75f;

        /* Convert beat positions → sample indices via TempoMap */
        int64_t smp_now  = tempo_beat_to_sample(ctx->tm, ctx->start_beat + ctx->cur_beat);
        int64_t smp_end  = tempo_beat_to_sample(ctx->tm, ctx->start_beat + ctx->cur_beat + dur_beats);

        switch(op){
        case VI_NOTE:
            /* T4-3: check event budget */
            if(ctx->total_events + 2 > VOICE_MAX_EVENTS_EXPANDED){
                snprintf(ctx->err, ctx->err_sz, "event budget exceeded (T4-3)"); return -1;
            }
            if(ev_push(ctx->es, (uint64_t)smp_now,  EV_NOTE_ON,  pitch, vel) < 0 ||
               ev_push(ctx->es, (uint64_t)smp_end,  EV_NOTE_OFF, pitch, vel) < 0){
                snprintf(ctx->err, ctx->err_sz, "event overflow"); return -1;
            }
            ctx->last_off = ctx->es->n - 1;
            ctx->total_events += 2;
            ctx->cur_beat += dur_beats;
            break;

        case VI_REST:
            ctx->cur_beat += dur_beats;
            break;

        case VI_TIE:
            if(ctx->last_off >= 0 && ctx->last_off < ctx->es->n){
                /* Extend the most recent NOTE_OFF to the new end position */
                int64_t new_end = tempo_beat_to_sample(
                    ctx->tm,
                    ctx->start_beat + ctx->cur_beat + dur_beats);
                ctx->es->events[ctx->last_off].sample = (uint64_t)new_end;
            }
            ctx->cur_beat += dur_beats;
            break;

        case VI_GLIDE: {
            if(ctx->last_off >= 0 && ctx->last_off < ctx->es->n){
                ctx->es->events[ctx->last_off].sample = (uint64_t)smp_now;
                ctx->es->events[ctx->last_off].type   = EV_GLIDE;
            }
            if(ctx->total_events + 2 > VOICE_MAX_EVENTS_EXPANDED){
                snprintf(ctx->err, ctx->err_sz, "event budget exceeded"); return -1;
            }
            if(ev_push(ctx->es, (uint64_t)smp_now, EV_NOTE_ON,  pitch, vel) < 0 ||
               ev_push(ctx->es, (uint64_t)smp_end, EV_NOTE_OFF, pitch, vel) < 0){
                snprintf(ctx->err, ctx->err_sz, "event overflow"); return -1;
            }
            ctx->last_off = ctx->es->n - 1;
            ctx->total_events += 2;
            ctx->cur_beat += dur_beats;
            break;
        }

        case VI_REPEAT_BEGIN: {
            int depth = 1, end_i = -1;
            for(int j = i+1; j < hi; j++){
                uint8_t jop = VI_OP(ctx->code[j]);
                if(jop == VI_REPEAT_BEGIN) depth++;
                else if(jop == VI_REPEAT_END){ depth--; if(!depth){ end_i=j; break; } }
            }
            if(end_i < 0){
                snprintf(ctx->err, ctx->err_sz, "unmatched REPEAT_BEGIN at %d", i);
                return -1;
            }
            int count = (int)VI_VEL(ctx->code[end_i]);
            if(count < 1) count = 1;
            /* T4-3: clamp repeat count */
            if(count > VOICE_MAX_REPEAT_COUNT){
                snprintf(ctx->err, ctx->err_sz,
                         "repeat count %d exceeds max %d (T4-3)", count, VOICE_MAX_REPEAT_COUNT);
                return -1;
            }
            for(int rr = 0; rr < count; rr++)
                if(tcompile_range(ctx, i+1, end_i) < 0) return -1;
            i = end_i;
            break;
        }
        case VI_REPEAT_END: break;
        default: break;
        }
        i++;
    }
    return 0;
}

int voice_compile_tempo(const VoiceProgram *vp, EventStream *es,
                        const TempoMap *tm, double start_beat,
                        char *err_out, int err_sz){
    if(!err_out || err_sz <= 0){ static char _eb[64]; err_out=_eb; err_sz=64; }
    err_out[0] = '\0';
    memset(es, 0, sizeof(*es));

    TCompileCtx ctx = {
        .code        = vp->code,
        .es          = es,
        .tm          = tm,
        .cur_beat    = 0.0,
        .start_beat  = start_beat,
        .last_off    = -1,
        .err         = err_out,
        .err_sz      = err_sz,
        .total_events = 0,
    };

    int r = tcompile_range(&ctx, 0, vp->n);

    /* Compute total duration from TempoMap */
    if(ctx.cur_beat > 0.0){
        double t0 = tempo_beat_to_seconds(tm, start_beat);
        double t1 = tempo_beat_to_seconds(tm, start_beat + ctx.cur_beat);
        es->total_samples = (uint64_t)llround((t1 - t0) * tm->sr);
    }
    es->total_beats = (float)ctx.cur_beat;
    return r;
}
