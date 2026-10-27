#include "apu/apu_xaudio2.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int entered, credits, release_all, fail_output, closed;
static int16_t frames[8][512];
int sceAudioOutInit(void) { return 0; }
int sceAudioOutOpen(int user,int type,int index,unsigned grain,unsigned rate,unsigned format)
{
    assert(user==255 && type==0 && index==0 && grain==256 && rate==48000 && format==1);
    return 9;
}
int sceAudioOutClose(int handle) { assert(handle==9); closed++;return 0; }
int sceAudioOutOutput(int handle,const void *pcm)
{
    assert(handle==9); if(!pcm)return 0;
    pthread_mutex_lock(&gate);
    if(entered<8)memcpy(frames[entered],pcm,sizeof(frames[0]));
    entered++; pthread_cond_broadcast(&changed);
    while(!credits&&!release_all)pthread_cond_wait(&changed,&gate);
    if(credits)credits--;
    int rc=fail_output?-1:0;
    pthread_mutex_unlock(&gate);return rc;
}
static void wait_calls(int n)
{
    pthread_mutex_lock(&gate);
    while(entered<n)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void advance(void)
{
    pthread_mutex_lock(&gate);credits++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
}
int main(void)
{
    assert(xa2_init());wait_calls(1);
    int16_t samples[512];for(unsigned i=0;i<512;i++)samples[i]=(int16_t)(i-256);
    assert(!xa2_submit_samples(NULL,256));assert(!xa2_submit_samples(samples,257));
    assert(!xa2_submit_samples(samples,0));assert(xa2_submit_samples(samples,256));
    for(int i=0;i<4;i++){advance();wait_calls(i+2);}
    /* Within 0.8% of the source rate the ramp survives interpolation; the
     * first stereo sample may blend with the preceding silence. */
    for(int j=0;j<2;j++)assert(frames[4][j]<=0&&frames[4][j]>=samples[j]);
    for(int j=2;j<512;j++)assert(abs(frames[4][j]-samples[j])<=2);
    for(int i=0;i<4;i++)for(int j=0;j<512;j++)assert(frames[i][j]==0);
    for(int i=0;i<24;i++)assert(xa2_submit_samples(samples,256));
    assert(!xa2_submit_samples(samples,256));
    int queued,dropped;xa2_get_stats(&queued,&dropped,NULL);assert(queued==24&&dropped==1);
    /* A queue held above TARGET speeds playback up: within 0.8% the device
     * consumes more than one source frame per output frame. */
    for(int i=0;i<40;i++){advance();wait_calls(6+i);assert(xa2_submit_samples(samples,256));}
    int dropped_after;xa2_get_stats(&queued,&dropped_after,NULL);
    assert(dropped_after==dropped&&queued<24);
    pthread_mutex_lock(&gate);release_all=1;fail_output=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
    xa2_shutdown();assert(!xa2_is_active()&&closed==1);
    assert(!xa2_submit_samples(samples,256));
    puts("Audio PCM, backpressure and shutdown: pass (mock device)");
}
