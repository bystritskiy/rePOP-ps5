/* reSOT PS5, MIT. A static text screen through public VideoOut exports.
 * This is CPU scanout, not the game's NV2A/Vulkan renderer. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "screen_ps5.h"
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t,int64_t,size_t,size_t,int,int64_t *);
extern int sceKernelMapDirectMemory(void **,size_t,int,int,int64_t,size_t);
extern int sceKernelReleaseDirectMemory(int64_t,size_t);
extern int sceVideoOutOpen(int,int,int,const void *);
extern int sceVideoOutClose(int);
extern int sceVideoOutWaitVblank(int);
extern int sceVideoOutSubmitFlip(int,int,uint32_t,int64_t);
extern int sceSystemServiceHideSplashScreen(void);
extern void resot_host_draw_text_scaled(uint32_t *,int,int,int,int,const char *,uint32_t,int);
typedef struct { void *address,*metadata,*reserved[2]; } ResotVideoBuffer;
typedef struct { uint8_t reserved[80]; } ResotVideoAttribute;
extern void sceVideoOutSetBufferAttribute2(ResotVideoAttribute *,uint64_t,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t);
extern int sceVideoOutRegisterBuffers2(int,int,int,ResotVideoBuffer *,int,ResotVideoAttribute *,int,void *);
#define WIDTH 1920
#define HEIGHT 1080
#define BYTES 0x800000
int resot_ps5_text_screen(const ResotScreenLine *lines,unsigned count)
{
    int video=sceVideoOutOpen(255,0,0,NULL);if(video<0)return video;
    int64_t physical=0;uint32_t *pixels=NULL;
    int rc=sceKernelAllocateDirectMemory(0,(int64_t)sceKernelGetDirectMemorySize(),BYTES,0x200000,3,&physical);
    if(rc<0){sceVideoOutClose(video);return rc;}
    rc=sceKernelMapDirectMemory((void **)&pixels,BYTES,0x33,0,physical,0x200000);
    if(rc<0){sceKernelReleaseDirectMemory(physical,BYTES);sceVideoOutClose(video);return rc;}
    for(unsigned i=0;i<WIDTH*HEIGHT;i++)pixels[i]=0xff201912;
    for(unsigned i=0;i<count;i++)
        resot_host_draw_text_scaled(pixels,WIDTH,HEIGHT,120,(int)lines[i].y,lines[i].text,lines[i].color,(int)lines[i].scale);
    for(size_t i=0;i<BYTES;i+=64)__asm__ volatile("clflush (%0)"::"r"((uint8_t *)pixels+i):"memory");
    __asm__ volatile("mfence":::"memory");
    ResotVideoBuffer buffer={.address=pixels};ResotVideoAttribute attribute={0};
    sceVideoOutSetBufferAttribute2(&attribute,UINT64_C(0x8000000022000000),0,WIDTH,HEIGHT,0,0,0);
    rc=sceVideoOutRegisterBuffers2(video,0,0,&buffer,1,&attribute,0,NULL);
    if(rc<0)return rc; /* allocations remain owned until process exit */
    rc=sceVideoOutSubmitFlip(video,0,1,1);
    if(rc>=0){sceVideoOutWaitVblank(video);sceSystemServiceHideSplashScreen();}
    return rc;
}
int resot_ps5_diagnostic_screen(int memory_ok,int events_ok)
{
    const ResotScreenLine lines[]={
        {130,12,0xffffffff,"RESOT PS5"},
        {260,7,0xff8ed6f0,"PORT DIAGNOSTICS"},
        {380,5,0xffffffff,memory_ok?"GUEST MEMORY ALIASES PASS":"GUEST MEMORY ALIASES FAIL"},
        {450,5,0xffffffff,events_ok?"HOST EVENTS PASS":"HOST EVENTS FAIL"},
        {560,5,0xffffffff,"PRESS CROSS FOR AUDIO TEST"},
        {650,5,0xffffffff,"CONTROLLER CHANGES ARE LOGGED"},
        {780,5,0xff8ed6f0,"GAME RENDERER IS NOT LINKED"},
        {850,5,0xff8ed6f0,"NO GAME DATA INCLUDED"}};
    return resot_ps5_text_screen(lines,sizeof lines/sizeof lines[0]);
}
