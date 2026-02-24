#include "../include/patch.h"
#include <math.h>

float g_freq[128];
float g_cutoff[64];
float g_env[32];

const float g_mod[32] = {
    0.000f,0.032f,0.065f,0.097f,0.129f,0.161f,0.194f,0.226f,
    0.258f,0.290f,0.323f,0.355f,0.387f,0.419f,0.452f,0.484f,
    0.516f,0.548f,0.581f,0.613f,0.645f,0.677f,0.710f,0.742f,
    0.774f,0.806f,0.839f,0.871f,0.903f,0.935f,0.968f,1.000f
};
const float g_dur[7]={1.f/64,1.f/32,1.f/16,1.f/8,1.f/4,1.f/2,1.f};

static int s_init=0;
void tables_init(void){
    if(s_init)return;
    for(int i=0;i<128;i++) g_freq[i]=440.f*powf(2.f,(i-69)/12.f);
    for(int i=0;i<64;i++)  g_cutoff[i]=20.f*powf(1000.f,(float)i/63.f);
    for(int i=0;i<32;i++)  g_env[i]=0.001f*powf(4000.f,(float)i/31.f);
    s_init=1;
}
float freq_from_midi(int m){if(m<0)m=0;if(m>127)m=127;return g_freq[m];}
float env_time(int i)      {if(i<0)i=0;if(i>31) i=31;  return g_env[i];}
float cutoff_hz(int i)     {if(i<0)i=0;if(i>63) i=63;  return g_cutoff[i];}
