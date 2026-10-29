/* reSOT PS5, MIT. A static text screen through public VideoOut exports.
 * This is CPU scanout, not the game's NV2A/Vulkan renderer. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
extern int sceVideoOutSetFlipRate(int,int);
extern int sceKernelDebugOutText(int,const char *);
#define WIDTH 1920
#define HEIGHT 1080
/* The buffers are those PS5_Mesa's VideoOut WSI registers in this app
 * (src/vulkan/wsi/wsi_common_videoout.c): five of one direct-memory
 * allocation (CPU type 12), B8G8R8A8 SDR, tiling 0, which is 64 KiB tiles of
 * 128x128 pixels (PS5_Vulkan src/demo_renderer.cpp): 15x9 tiles, 8,847,360
 * bytes for this frame, 10 MiB at the 2 MiB alignment. Only the first is
 * shown. One buffer was refused with 0x80290001 by
 * sceVideoOutRegisterBuffers2, as an 8 MiB, an R8G8B8A8 sRGB and a type 3
 * one were (observed 2026-10-29). */
#define BUFFER_BYTES 0xa00000
#define BUFFERS 5
#define BYTES (BUFFER_BYTES*BUFFERS)
#define PIXEL_FORMAT_B8G8R8A8_SDR UINT64_C(0x8000000000000000)
#define DIRECT_TYPE_CPU 12
static size_t tiled_offset(unsigned x,unsigned y)
{
    uint32_t inside=((y<<4)&0x70u)^((y<<5)&0xf00u)^((y<<9)&0x1000u)^((y<<8)&0x4000u)^
        ((x<<2)&0xcu)^((x<<5)&0x380u)^((x<<4)&0x400u)^((x<<6)&0x800u)^((x<<9)&0xa000u);
    return ((size_t)((y>>7)*((WIDTH+127)/128)+(x>>7))<<16)+inside;
}
static int failed(const char *step,int rc)
{
    char line[160];snprintf(line,sizeof line,"[reSOT] [BOOT] text screen: %s %#x\n",step,(unsigned)rc);
    sceKernelDebugOutText(0,line);fputs(line+8,stderr);
    return rc;
}
int resot_ps5_text_screen(const ResotScreenLine *lines,unsigned count)
{
    /* Drawn row by row, then stored tiled; 0xAARRGGBB in memory is the
     * format's bytes B, G, R, A. */
    uint32_t *rows=malloc((size_t)WIDTH*HEIGHT*4);
    if(!rows)return failed("no memory for the picture",-1);
    for(unsigned i=0;i<WIDTH*HEIGHT;i++)rows[i]=0xff201912;
    for(unsigned i=0;i<count;i++)
        resot_host_draw_text_scaled(rows,WIDTH,HEIGHT,120,(int)lines[i].y,lines[i].text,lines[i].color,(int)lines[i].scale);
    sceSystemServiceHideSplashScreen();
    int video=sceVideoOutOpen(255,0,0,NULL);if(video<0){free(rows);return failed("sceVideoOutOpen",video);}
    sceVideoOutSetFlipRate(video,0);
    int64_t physical=0;uint8_t *pixels=NULL;
    int rc=sceKernelAllocateDirectMemory(0,(int64_t)sceKernelGetDirectMemorySize(),BYTES,0x200000,DIRECT_TYPE_CPU,&physical);
    if(rc<0){free(rows);sceVideoOutClose(video);return failed("sceKernelAllocateDirectMemory",rc);}
    rc=sceKernelMapDirectMemory((void **)&pixels,BYTES,0x33,0,physical,0x200000);
    if(rc<0){free(rows);sceKernelReleaseDirectMemory(physical,BYTES);sceVideoOutClose(video);return failed("sceKernelMapDirectMemory",rc);}
    memset(pixels,0,BYTES);
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++)
        *(uint32_t *)(pixels+tiled_offset(x,y))=rows[y*WIDTH+x];
    free(rows);
    for(size_t i=0;i<BYTES;i+=64)__asm__ volatile("clflush (%0)"::"r"(pixels+i):"memory");
    __asm__ volatile("mfence":::"memory");
    ResotVideoBuffer buffers[BUFFERS]={{0}};ResotVideoAttribute attribute={0};
    for(unsigned i=0;i<BUFFERS;i++)buffers[i].address=pixels+(size_t)i*BUFFER_BYTES;
    sceVideoOutSetBufferAttribute2(&attribute,PIXEL_FORMAT_B8G8R8A8_SDR,0,WIDTH,HEIGHT,0,0,0);
    rc=sceVideoOutRegisterBuffers2(video,0,0,buffers,BUFFERS,&attribute,0,NULL);
    if(rc<0)return failed("sceVideoOutRegisterBuffers2",rc); /* allocations remain owned until process exit */
    rc=sceVideoOutSubmitFlip(video,0,1,1);
    if(rc<0)return failed("sceVideoOutSubmitFlip",rc);
    sceVideoOutWaitVblank(video);
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
