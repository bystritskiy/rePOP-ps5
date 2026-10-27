/* reSOT PS5 host, MIT. Native statically recompiled Xbox title; no JIT. */
#include <SDL3/SDL.h>
#include "platform/win32_compat.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "guest_vmem.h"
#include "apu.h"
#include "target.h"
#include "../../android/src/sha256.h"
#include "../../android/src/host_files.h"
#include "../../windows/src/options.h"
#include "screen_ps5.h"
#include "input/xinput_xbox.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern MCPXAPUState *g_apu_state;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_OhciInit(void);
extern void xbox_InputInit(void);
extern void xbox_path_init(const char *,const char *);
extern void xbox_FramebufferHostSetWindow(void *);
extern void xbox_FramebufferSetWowProvider(void (*)(char *,size_t));
extern void resot_current_wow(char *,size_t);
extern int resot_ps5_install_fault_handler(void);
extern int resot_ps5_prepare_gpu(void);
extern int sceSystemServiceHideSplashScreen(void);
extern int sceKernelDebugOutText(int,const char *);
extern void resot_ps5_input_shutdown(void);
extern int sceKernelSendNotificationRequest(int,void *,size_t,int);
static char game_dir[1024]="/app0/game";
static const char *data_dir="/data/reSOT/PPSA99023";
static _Atomic int running;
static _Atomic int result=1;
const char *resot_ps5_data_dir(void) { return data_dir; }
static char crash_path[1100]="/data/reSOT/PPSA99023/logs/crash.txt";
const char *resot_ps5_crash_path(void) { return crash_path; }
static void defaults(const char *key,const char *value) { if(!getenv(key))setenv(key,value,1); }
/* Nothing on the console reads a title's standard error; boot messages also
 * go to klog, which survives a crash and needs no FTP to the log files. */
static void boot_log(const char *format,...)
{
    char line[1024];va_list args;va_start(args,format);
    vsnprintf(line,sizeof line,format,args);va_end(args);
    fputs(line,stderr);
    char record[1040];snprintf(record,sizeof record,"[reSOT] %s",line);
    sceKernelDebugOutText(0,record);
}
static int setup_failed(const char *step,const char *path)
{
    boot_log("[BOOT] data setup: %s %s: errno %d\n",step,path,errno);return 0;
}
/* A write to /data took 25-40 ms and up to 840 ms (observed), and every
 * kernel call logs two lines: the title ran at a few kernel calls a second,
 * waiting on its log. Streams go into a pipe instead, one write per line,
 * and a thread per file moves them to disk in large writes. Lines still in
 * the pipe when the process is killed are lost; boot lines also go to klog. */
static _Atomic unsigned log_write_max_us;
static _Atomic unsigned long log_bytes;
struct log_sink { int from,to; };
static void *log_writer(void *arg)
{
    struct log_sink *sink=arg;static char buffers[2][65536];
    char *buffer=buffers[sink->to&1];
    for(;;) {
        ssize_t n=read(sink->from,buffer,sizeof buffers[0]);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
        for(ssize_t done=0;done<n;) {
            ssize_t w=write(sink->to,buffer+done,(size_t)(n-done));
            if(w<0 && errno==EINTR)continue;
            if(w<=0)break;
            done+=w;
        }
        clock_gettime(CLOCK_MONOTONIC,&b);
        unsigned us=(unsigned)((b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000),seen=atomic_load(&log_write_max_us);
        while(us>seen && !atomic_compare_exchange_weak(&log_write_max_us,&seen,us)){}
        atomic_fetch_add(&log_bytes,(unsigned long)n);
    }
    return NULL;
}
static FILE *open_log(const char *path)
{
    static struct log_sink sinks[2];static unsigned used;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644),ends[2];
    if(fd<0)return NULL;
    if(used<2 && pipe(ends)==0) {
        struct log_sink *sink=&sinks[used];sink->from=ends[0];sink->to=fd;
        pthread_t thread;FILE *stream;
        if(!pthread_create(&thread,NULL,log_writer,sink)) {
            used++;pthread_detach(thread);
            if((stream=fdopen(ends[1],"w"))){setvbuf(stream,NULL,_IOLBF,0);return stream;}
        }
        close(ends[1]);
    }
    FILE *direct=fdopen(fd,"w");if(direct)setvbuf(direct,NULL,_IONBF,0);
    return direct;
}
static int setup_data(void)
{
    const char *dirs[]={"/data/reSOT",data_dir};
    for(unsigned i=0;i<2;i++)if(mkdir(dirs[i],0755) && errno!=EEXIST)return setup_failed("mkdir",dirs[i]);
    /* The app's user owns what it creates; ftpsrv runs as another and was
     * refused (550) a folder in a 0755 /data/reSOT (observed 2026-10-27).
     * Leave the game folder writable for copying the disc over FTP. */
    char path[1024];
    snprintf(path,sizeof path,"/data/reSOT/" RESOT_TARGET_ID);mkdir(path,0777);
    chmod("/data/reSOT",0777);chmod(path,0777);
    snprintf(path,sizeof path,"%s/logs",data_dir);if(mkdir(path,0755) && errno!=EEXIST)return setup_failed("mkdir",path);
    /* A title may not dup2 (EPERM), which freopen of stderr/stdout relies on:
     * on the console the files were created and stayed empty. The streams are
     * replaced instead, as PS5 Platform's klog capture does. */
    const char *streams[]={"err","out"};
    for(unsigned i=0;i<2;i++) {
        snprintf(path,sizeof path,"%s/logs/" RESOT_TARGET_ID "-%s.txt",data_dir,streams[i]);
        char old[1100];snprintf(old,sizeof old,"%s.previous",path);rename(path,old);
        FILE *file=open_log(path);if(!file)return setup_failed("open",path);
        if(i==0)stderr=file;else stdout=file;
    }
    snprintf(crash_path,sizeof crash_path,"%s/logs/" RESOT_TARGET_ID "-crash.txt",data_dir);remove(crash_path);
    snprintf(path,sizeof path,"%s/saves",data_dir);if(mkdir(path,0755) && errno!=EEXIST)return setup_failed("mkdir",path);
    snprintf(path,sizeof path,"%s/saves/" RESOT_TARGET_ID,data_dir);if(mkdir(path,0755) && errno!=EEXIST)return setup_failed("mkdir",path);
    /* The console refuses chdir (EPERM, observed) and faults in getcwd, so
     * paths are absolute; like Android, the refusal is only reported. */
    if(chdir(data_dir))boot_log("[BOOT] chdir %s: errno %d\n",data_dir,errno);
    return 1;
}
/* The game is the user's own disc, so the app need not carry it: a package
 * with game/ inside wins, then a folder on internal storage, USB drives and
 * extended storage. The first folder with every resource and the supported
 * XBE is used; why each other folder was refused goes on the missing-files
 * screen and in the log. */
static void *read_xbe(const char *dir,size_t *size,char *why,size_t why_size)
{
    const char *required[]={"default.xbe","prince.bf","video","sound","shaders.bin"};
    unsigned count=strcmp(RESOT_TARGET_ID,"xbox-retail")==0?5:4;
    char path[1200];
    for(unsigned i=0;i<count;i++)if(resot_path_kind(dir,required[i],path,sizeof path)!=(i==2||i==3?2:1)) {
        snprintf(why,why_size,"%s: no %s",dir,required[i]);return NULL;
    }
    if(!resot_path_kind(dir,"default.xbe",path,sizeof path))return NULL;
    FILE *f=fopen(path,"rb");if(!f){snprintf(why,why_size,"%s: default.xbe unreadable",dir);return NULL;}
    long length=-1;
    if(!fseek(f,0,SEEK_END))length=ftell(f);
    void *data=length>0 && length<=32*1024*1024 && !fseek(f,0,SEEK_SET)?malloc((size_t)length):NULL;
    if(!data || fread(data,1,(size_t)length,f)!=(size_t)length) {
        free(data);fclose(f);snprintf(why,why_size,"%s: default.xbe unreadable",dir);return NULL;
    }
    fclose(f);
    ResotSha256 sha;char hex[65];resot_sha256_init(&sha);
    resot_sha256_update(&sha,data,(size_t)length);resot_sha256_hex(&sha,hex);
    if(strcmp(hex,RESOT_XBE_SHA256)) {
        boot_log("[BOOT] %s/default.xbe: unsupported SHA-256 %s (expected %s)\n",dir,hex,RESOT_XBE_SHA256);
        snprintf(why,why_size,"%s: default.xbe is another version",dir);free(data);return NULL;
    }
    *size=(size_t)length;return data;
}
static void *find_game(size_t *size,char refused[][160],unsigned *refused_count)
{
    char candidates[11][64];unsigned n=0;
    snprintf(candidates[n++],64,"/app0/game");
    snprintf(candidates[n++],64,"/data/reSOT/" RESOT_TARGET_ID);
    for(unsigned i=0;i<8;i++)snprintf(candidates[n++],64,"/mnt/usb%u/reSOT/" RESOT_TARGET_ID,i);
    for(unsigned i=0;i<2 && n<11;i++)snprintf(candidates[n++],64,"/mnt/ext%u/reSOT/" RESOT_TARGET_ID,i);
    *refused_count=0;
    for(unsigned i=0;i<n;i++) {
        struct stat st;if(stat(candidates[i],&st) || !S_ISDIR(st.st_mode))continue;
        char why[160]="";void *xbe=read_xbe(candidates[i],size,why,sizeof why);
        if(xbe){snprintf(game_dir,sizeof game_dir,"%s",candidates[i]);boot_log("[BOOT] game files: %s\n",game_dir);return xbe;}
        boot_log("[BOOT] game folder refused: %s\n",why);
        if(*refused_count<3)snprintf(refused[(*refused_count)++],160,"%s",why);
    }
    return NULL;
}
/* No game: say where to put it, on a CPU screen (the GPU is not set up yet)
 * and as a notification, then wait for Cross to close. */
static int missing_game(char refused[][160],unsigned refused_count)
{
    ResotScreenLine lines[16]={
        {110,8,0xffffffff,"reSOT - The Sands of Time"},
        {210,5,0xff8ed6f0,"Game files not found"},
        {320,3,0xffffffff,"This app runs your own copy of the Xbox retail game. Extract your disc"},
        {360,3,0xffffffff,"and copy all its files and folders into one of these folders:"},
        {430,3,0xff8ed6f0,"/data/reSOT/" RESOT_TARGET_ID},
        {470,3,0xff8ed6f0,"/mnt/usb0/reSOT/" RESOT_TARGET_ID "   (usb0 to usb7, ext0, ext1)"},
        {540,3,0xffffffff,"default.xbe must be the supported retail version, SHA-256"},
        {580,3,0xff8ed6f0,RESOT_XBE_SHA256},
        {620,3,0xffffffff,"Then start the app again."}};
    unsigned count=9;
    static char notes[3][200];
    if(refused_count) {
        lines[count++]=(ResotScreenLine){700,3,0xffffffff,"Folders found but not used:"};
        for(unsigned i=0;i<refused_count;i++) {
            snprintf(notes[i],sizeof notes[i],"%s",refused[i]);
            lines[count++]=(ResotScreenLine){740+40*i,3,0xff8ed6f0,notes[i]};
        }
    }
    lines[count++]=(ResotScreenLine){960,4,0xffffffff,"Press Cross to close."};
    struct { char unused[45]; char message[3075]; } note;memset(&note,0,sizeof note);
    snprintf(note.message,sizeof note.message,"reSOT: game files not found. Copy your disc to /data/reSOT/" RESOT_TARGET_ID);
    sceKernelSendNotificationRequest(0,&note,sizeof note,0);
    int rc=resot_ps5_text_screen(lines,count);
    boot_log("[BOOT] no game files; missing-files screen %#x\n",rc);
    xbox_InputInit();
    for(;;) {
        XBOX_INPUT_STATE state;
        if(xbox_InputGetState(0,&state)==ERROR_SUCCESS && state.Gamepad.bAnalogButtons[0]>30)break;
        usleep(50000);
    }
    boot_log("[SESSION] closed from the missing-files screen\n");
    return 1;
}
/* Guest memory exists before the GPU: hw_init starts the presentation
 * thread, which reads NV2A registers in guest memory at once. Initialized
 * on the game thread, it faulted there at offset 0 (0xFD008700, observed). */
static int setup_guest_memory(void *xbe,size_t size)
{
    if(!xbox_MemoryLayoutInit(xbe,size)){boot_log("[BOOT] guest memory initialization failed\n");free(xbe);return 0;}
    free(xbe);g_xbox_mem_offset=xbox_GetMemoryOffset();
#if defined(RESOT_DEFAULT_EXT_VMA) && RESOT_DEFAULT_EXT_VMA
    if(getenv("RECOMP_EXT_VMA") && strcmp(getenv("RECOMP_EXT_VMA"),"0") && !guest_vmem_active()) {
        boot_log("[BOOT] required retail extended virtual memory unavailable\n");return 0;
    }
#endif
    boot_log("[BOOT] guest memory offset=%p\n",(void *)g_xbox_mem_offset);
    return 1;
}
/* Every 5 s: trapped device accesses per second, the process CPU clock
 * against wall time (it read exactly 100% throughout on the console, so it
 * is not a load measure there), the log rate and its slowest disk write. */
extern volatile unsigned long resot_ps5_mmio_faults[3];
static double seconds(clockid_t clock)
{
    struct timespec t;clock_gettime(clock,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static void *perf_main(void *unused)
{
    (void)unused;unsigned long last[3]={0};
    double wall=seconds(CLOCK_MONOTONIC),cpu=seconds(CLOCK_PROCESS_CPUTIME_ID);
    for(;;) {
        sleep(5);
        double now=seconds(CLOCK_MONOTONIC),used=seconds(CLOCK_PROCESS_CPUTIME_ID),span=now-wall;
        unsigned long f[3];
        for(int i=0;i<3;i++){f[i]=__atomic_load_n(&resot_ps5_mmio_faults[i],__ATOMIC_RELAXED);}
        static unsigned long logged;unsigned long bytes=atomic_load(&log_bytes);
        double write_ms=atomic_exchange(&log_write_max_us,0)/1000.0;
        boot_log("[PERF] mmio/s apu=%.0f ohci=%.0f ac97=%.0f process CPU clock %.0f%%, log %.0f B/s, slowest disk write %.1f ms\n",
            (f[0]-last[0])/span,(f[1]-last[1])/span,(f[2]-last[2])/span,100*(used-cpu)/span,(bytes-logged)/span,write_ms);
        logged=bytes;
        memcpy(last,f,sizeof last);wall=now;cpu=used;
    }
    return NULL;
}
/* Records the main thread's CPUs and priority, which threads created later
 * inherit, and keeps them on the 13 CPUs a game thread may use (kGameCpus of
 * github.com/elripalda/gta4-ps5). The console already gave 0x1fff and
 * priority 700 (observed); the log line shows if a launcher gives fewer. */
extern int scePthreadSetaffinity(pthread_t,uint64_t);
extern int scePthreadGetaffinity(pthread_t,uint64_t *);
extern int scePthreadGetprio(pthread_t,int *);
static void spread_threads(void)
{
    uint64_t before=0;int prio=0;
    int got=scePthreadGetaffinity(pthread_self(),&before),gp=scePthreadGetprio(pthread_self(),&prio);
    int set=scePthreadSetaffinity(pthread_self(),0x1FFF);
    uint64_t after=0;scePthreadGetaffinity(pthread_self(),&after);
    boot_log("[BOOT] main thread CPUs %#llx (%#x) -> %#llx (%#x), priority %d (%#x)\n",
        (unsigned long long)before,got,(unsigned long long)after,set,prio,gp);
}
static void *game_main(void *unused)
{
    (void)unused;
    /* sigaltstack is per thread: the MMIO handler's stack belongs to the
     * thread that runs the guest. It decodes APU/OHCI/AC97, not NV2A. */
    if(!resot_ps5_install_fault_handler()){boot_log("[BOOT] fault handler installation failed\n");goto done;}
    g_apu_state=mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    if(!g_apu_state){boot_log("[BOOT] APU initialization failed\n");goto done;}
    xbox_kernel_init();
    char saves[1200];snprintf(saves,sizeof saves,"%s/saves/" RESOT_TARGET_ID,data_dir);
    xbox_path_init(game_dir,saves);xbox_kernel_bridge_init();
#ifdef RESOT_D3D_DEVICE_GLOBAL
    xbox_Nv2aMirrorFence(RESOT_D3D_DEVICE_GLOBAL,RESOT_D3D_FENCE_ISSUED,RESOT_D3D_FENCE_GPU_PTR);
#endif
#ifdef RESOT_D3D_NOTIFY_EVENT
    /* Same as the Windows and Android hosts. Missing here, D3D's BlockOnFence
     * (caller 0x0037029B) waited on 0x0037D41C, failed at once and retried
     * 800,000 times a second after the menu data loaded (observed). */
    xbox_Nv2aNotifyEvent(RESOT_D3D_DEVICE_GLOBAL,RESOT_D3D_NOTIFY_EVENT,RESOT_D3D_FENCE_STATUS_SHIFT);
#endif
#ifdef RESOT_DSOUND_CODE_BEGIN
    xbox_SetApuPhysicalCallers(RESOT_DSOUND_CODE_BEGIN,RESOT_DSOUND_CODE_END);
#endif
    xbox_OhciInit();g_esp=XBOX_STACK_TOP;
    if(!recomp_dispatch_init())boot_log("[BOOT] dispatch uses binary search\n");
    xbox_WatchdogStart();
    boot_log("[BOOT] entering " RESOT_TARGET_ID "\n");
    xbe_entry_point();atomic_store(&result,0);
done:
    atomic_store(&running,0);return NULL;
}
int main(int argc,char **argv)
{
    (void)argc;(void)argv;
    if(!setup_data())return 1;
    spread_threads();
    boot_log("[SESSION] reSOT PS5 " RESOT_TARGET_ID " xbe=" RESOT_XBE_SHA256 "\n");
    boot_log("[SESSION] upstream=" RESOT_UPSTREAM_RELEASE " commit=" RESOT_UPSTREAM_COMMIT "\n");
    char refused[3][160];unsigned refused_count=0;size_t xbe_size=0;
    void *xbe=find_game(&xbe_size,refused,&refused_count);
    if(!xbe)return missing_game(refused,refused_count);
    defaults("RECOMP_PB_EXEC","1");defaults("RECOMP_FB_WINDOW","1");defaults("RECOMP_VBLANK","1");
    defaults("RECOMP_HW","1");defaults("RECOMP_AC97_READY","1");defaults("RECOMP_APU_DSP_ACK","gp+0x810");
    defaults("RECOMP_HEAP_RECLAIM","1");defaults("RESOT_RENDERER","vulkan");defaults("RECOMP_HW_ASPECT","16:9");
    defaults("RECOMP_HW_VSYNC","1");defaults("RECOMP_KEYBOARD","0");
    /* RADV's cache defaults to /app0/radv-shader-cache, read-only in an image
     * ("Read-only filesystem---disabling", observed). */
    char radv_cache[1100];snprintf(radv_cache,sizeof radv_cache,"%s/radv-shader-cache",data_dir);
    defaults("MESA_SHADER_CACHE_DIR",radv_cache);
    /* The database form is the one PS5_Vulkan runs on the console; with only
     * a directory Mesa chose the multi-file cache and its mmap'd index. */
    defaults("MESA_DISK_CACHE_DATABASE","1");
    /* SDL reports why it refuses a Vulkan device only as GPU warnings. */
    SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU,SDL_LOG_PRIORITY_WARN);
#ifdef RESOT_DEFAULT_EXT_VMA
#if RESOT_DEFAULT_EXT_VMA
    defaults("RECOMP_EXT_VMA","1");
#endif
#endif
#ifdef RESOT_PROJECTED_UI
    defaults("RECOMP_HW_PROJECTED_UI","1");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"offscreen");
    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)){boot_log("[BOOT] SDL: %s\n",SDL_GetError());return 1;}
    SDL_Window *window=SDL_CreateWindow("reSOT - " RESOT_TARGET_NAME,1920,1080,SDL_WINDOW_VULKAN);
    if(!window){boot_log("[BOOT] window: %s\n",SDL_GetError());return 1;}
    xbox_FramebufferHostSetWindow(window);xbox_FramebufferSetWowProvider(resot_current_wow);
    resot_options_init();xbox_InputInit();
    if(!setup_guest_memory(xbe,xbe_size))return 1;
    /* GPU setup stays on the presentation thread. A real Vulkan device is
     * required before entering lifted code; failure cannot select a stub. */
    if(!resot_ps5_prepare_gpu()){boot_log("[BOOT] Vulkan initialization failed: %s\n",SDL_GetError());return 1;}
    sceSystemServiceHideSplashScreen();
    pthread_attr_t attr;pthread_attr_init(&attr);
    int rc=pthread_attr_setstacksize(&attr,64u<<20);pthread_t thread;
    atomic_store(&running,1);
    if(!rc)rc=pthread_create(&thread,&attr,game_main,NULL);
    pthread_attr_destroy(&attr);
    if(rc){boot_log("[BOOT] game thread: %d\n",rc);return 1;}
    pthread_t perf;if(!pthread_create(&perf,NULL,perf_main,NULL))pthread_detach(perf);
    while(atomic_load(&running)) {
        SDL_Event event;
        while(SDL_PollEvent(&event))if(event.type==SDL_EVENT_QUIT) {
            resot_ps5_input_shutdown();boot_log("[SESSION] close requested\n");_exit(0);
        }
        SDL_Delay(8);
    }
    pthread_join(thread,NULL);resot_ps5_input_shutdown();
    boot_log("[SESSION] title returned, status=%d\n",atomic_load(&result));
    /* Guest devices own worker threads until process teardown. */
    return atomic_load(&result);
}
