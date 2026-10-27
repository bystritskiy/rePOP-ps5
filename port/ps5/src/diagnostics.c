/* reSOT PS5, MIT. A real native host probe; it never calls the game's entry. */
#include "platform/win32_compat.h"
#include "apu/apu_xaudio2.h"
#include "pad_ps5.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
extern int resot_ps5_diagnostic_screen(int,int);
static int log_fd=-1;
static void record(const char *format,...)
{
    char line[1024];va_list ap;va_start(ap,format);
    int n=vsnprintf(line,sizeof(line),format,ap);va_end(ap);
    if(n<=0)return;size_t len=(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1;
    if(log_fd>=0){(void)write(log_fd,line,len);fsync(log_fd);}
}
static int memory_test(void)
{
    const size_t span_size=(size_t)UINT64_C(0x100000000),ram=64*1024*1024;
    void *span=VirtualAlloc(NULL,span_size,MEM_RESERVE,PAGE_NOACCESS);
    record("reserve_4gb=%p error=%u\n",span,(unsigned)GetLastError());
    if(!span)return 0;
    w32_compat_own_range(span,span_size);
    HANDLE backing=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)ram,NULL);
    volatile uint32_t *first=NULL,*mirror=NULL;int ok=0;
    if(backing) {
        first=MapViewOfFileEx(backing,FILE_MAP_ALL_ACCESS,0,0,ram,span);
        mirror=MapViewOfFileEx(backing,FILE_MAP_ALL_ACCESS,0,0,ram,(char *)span+ram);
        if(first&&mirror){first[1024]=0x534f5435;ok=mirror[1024]==0x534f5435;
            mirror[2048]=0x78563412;ok=ok&&first[2048]==0x78563412;}
    }
    record("shared_ram=%p mirror=%p alias_test=%d error=%u\n",(void *)first,(void *)mirror,ok,(unsigned)GetLastError());
    if(mirror)UnmapViewOfFile((const void *)mirror);
    if(first)UnmapViewOfFile((const void *)first);
    if(backing)CloseHandle(backing);
    w32_compat_own_range(NULL,0);
    int released=VirtualFree(span,span_size,MEM_RELEASE);
    record("reservation_released=%d\n",released);
    return ok&&released;
}
static int events_test(void)
{
    HANDLE event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event)return 0;
    int ok=WaitForSingleObject(event,0)==WAIT_TIMEOUT;
    ok=ok&&SetEvent(event)&&WaitForSingleObject(event,0)==WAIT_OBJECT_0;
    ok=ok&&WaitForSingleObject(event,0)==WAIT_TIMEOUT;
    CloseHandle(event);record("events_auto_reset=%d\n",ok);return ok;
}
static void beep(void)
{
    if(!xa2_init()){record("audio_open=FAIL\n");return;}
    int16_t pcm[512];
    for(int frame=0;frame<94;frame++) {
        for(int i=0;i<256;i++){int16_t v=((frame*256+i)%48)<24?2500:-2500;pcm[i*2]=pcm[i*2+1]=v;}
        while(xa2_is_active()&&!xa2_submit_samples(pcm,256))usleep(5000);
    }
    usleep(150000);
    record("audio_test_submitted active=%d audible_result=USER_CHECK_REQUIRED\n",xa2_is_active());
    xa2_shutdown();
}
int main(int argc,char **argv)
{
    (void)argc;(void)argv;
    mkdir("/data/reSOT",0755);mkdir("/data/reSOT/PPSA99022",0755);
    log_fd=open("/data/reSOT/PPSA99022/diagnostics.log",O_CREAT|O_APPEND|O_WRONLY|O_NOFOLLOW,0600);
    if(log_fd<0)log_fd=open("/download0/resot-ps5-diagnostics.log",O_CREAT|O_APPEND|O_WRONLY|O_NOFOLLOW,0600);
    record("\nreSOT PS5 diagnostics 1 pid=%d page=%ld console_tested=THIS_RUN_ONLY\n",getpid(),sysconf(_SC_PAGESIZE));
    int memory=memory_test(),events=events_test();
    int video=resot_ps5_diagnostic_screen(memory,events);record("video_submit=%x\n",(unsigned)video);
    xbox_InputInit();uint32_t previous=0;int disconnected=0;
    for(;;) {
        XBOX_INPUT_STATE state;
        if(xbox_InputGetState(0,&state)==ERROR_SUCCESS) {
            if(disconnected)record("pad_connected\n");disconnected=0;
            uint32_t buttons=state.Gamepad.wButtons|((uint32_t)(state.Gamepad.bAnalogButtons[0]>30)<<16);
            if(buttons!=previous){record("pad_buttons=%x lx=%d ly=%d rx=%d ry=%d\n",buttons,state.Gamepad.sThumbLX,state.Gamepad.sThumbLY,state.Gamepad.sThumbRX,state.Gamepad.sThumbRY);
                if((buttons&0x10000)&&!(previous&0x10000))beep();previous=buttons;}
        } else if(!disconnected){record("pad_disconnected\n");disconnected=1;previous=0;}
        usleep(50000);
    }
}
