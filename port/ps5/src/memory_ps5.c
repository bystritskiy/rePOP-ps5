/* reSOT PS5 memory bridge, MIT. Direct-memory API: PS5 Platform, GPL-3.0+.
 * Host pages are 16 KiB; guest code is data, with no native EXEC permission.
 * Only recorded reservations may be replaced by fixed views. */
#include "platform/win32_compat.h"
#include <ps5platform/shm.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef __PROSPERO__
/* PayloadSDK's libc.a defines mmap and mprotect as raw syscalls; a title may
 * not issue one (SYSTEM_ILLEGAL_FUNCTION_CALL: "directly issued a syscall 74"
 * from VirtualProtect, then 477 from Mesa's disk cache, observed on the
 * console). Defined here, they keep libc.a's mman.o out of the link and go
 * through libkernel, whose errors are 0x8002xxxx with the errno low. */
extern int sceKernelMprotect(const void *,size_t,int);
extern int sceKernelMmap(void *,size_t,int,int,int,off_t,void **);
void *mmap(void *at,size_t size,int prot,int flags,int fd,off_t offset)
{
    void *mapped=NULL;int e=sceKernelMmap(at,size,prot,flags,fd,offset,&mapped);
    if(e){errno=e&0xffff;return MAP_FAILED;}
    return mapped;
}
int mprotect(const void *at,size_t size,int prot)
{
    int e=sceKernelMprotect(at,size,prot);
    if(e){errno=e&0xffff;return -1;}
    return 0;
}
#endif
#define PAGE_SIZE_PS5 ((size_t)0x4000)
#define RECORDS 1024
struct backing { struct ps5_shm shm; size_t requested; unsigned refs; };
struct region { void *base; size_t size; struct backing *object; DWORD protect; DWORD *pages; };
static struct region ranges[RECORDS], views[RECORDS];
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t rounded(size_t n) { return n && n <= SIZE_MAX-PAGE_SIZE_PS5+1 ? (n+PAGE_SIZE_PS5-1)&~(PAGE_SIZE_PS5-1) : 0; }
static int contains(const struct region *r, const void *p, size_t n)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)r->base;
    return b && a>=b && a-b<r->size && n<=r->size-(a-b);
}
static struct region *find(struct region *rs,const void *p,size_t n)
{
    for(unsigned i=0;i<RECORDS;i++) if(contains(rs+i,p,n)) return rs+i;
    return NULL;
}
static struct region *empty(struct region *rs)
{
    for(unsigned i=0;i<RECORDS;i++) if(!rs[i].base) return rs+i;
    return NULL;
}
static int overlap(const void *p,size_t n)
{
    uintptr_t a=(uintptr_t)p;
    if(n>UINTPTR_MAX-a)return 1;
    for(unsigned i=0;i<RECORDS;i++) if(views[i].base) {
        uintptr_t b=(uintptr_t)views[i].base;
        if(a<b+views[i].size && b<a+n)return 1;
    }
    return 0;
}
static int protection(DWORD p)
{
    switch(p&255) {
    case PAGE_NOACCESS:return 0;
    case PAGE_READONLY:case PAGE_EXECUTE:case PAGE_EXECUTE_READ:return PS5_SHM_READ;
    default:return PS5_SHM_READ|PS5_SHM_WRITE;
    }
}
static struct backing *new_backing(size_t n)
{
    struct backing *b=calloc(1,sizeof(*b));
    if(!b)return NULL;
    if(ps5_shm_create(n,&b->shm)){free(b);return NULL;}
    /* Direct allocations can be recycled by the kernel. Give Win32 callers
     * zero-initialized storage before exposing any alias. */
    void *zero=NULL;
    if(ps5_shm_map(&b->shm,0,n,NULL,PS5_SHM_READ|PS5_SHM_WRITE,0,&zero)) {
        ps5_shm_destroy(&b->shm);free(b);return NULL;
    }
    memset(zero,0,n);
    if(ps5_shm_unmap(zero,n,0)){ps5_shm_destroy(&b->shm);free(b);return NULL;}
    b->requested=n;b->refs=1;return b;
}
static void release_locked(struct backing *b)
{
    if(--b->refs==0){ps5_shm_destroy(&b->shm);free(b);}
}
static void release_handle(void *p)
{
    pthread_mutex_lock(&vm_lock);release_locked(p);pthread_mutex_unlock(&vm_lock);
}
static void *map_locked(struct backing *b,size_t off,size_t n,void *at,DWORD protect)
{
    struct region *slot=empty(views);
    if(!slot || !n || off%PAGE_SIZE_PS5 || n%PAGE_SIZE_PS5 || off>b->requested || n>b->requested-off) return NULL;
    /* Outside a recorded reservation the platform may only use a hint.
     * An unexpected placement is unmapped and reported as a refusal. */
    unsigned flags=at && find(ranges,at,n) ? PS5_SHM_FIXED : 0;
    if(at && (overlap(at,n) || (uintptr_t)at%PAGE_SIZE_PS5))return NULL;
    void *view=NULL;
    if(ps5_shm_map(&b->shm,off,n,at,protection(protect),flags,&view))return NULL;
    if(at && view!=at){ps5_shm_unmap(view,n,0);return NULL;}
    DWORD *pages=malloc((n/PAGE_SIZE_PS5)*sizeof(*pages));
    if(!pages){ps5_shm_unmap(view,n,flags?PS5_SHM_KEEP_RESERVED:0);return NULL;}
    for(size_t i=0;i<n/PAGE_SIZE_PS5;i++)pages[i]=protect;
    *slot=(struct region){view,n,b,protect,pages};b->refs++;
    return view;
}
static int unmap_locked(struct region *v)
{
    unsigned flags=find(ranges,v->base,v->size)?PS5_SHM_KEEP_RESERVED:0;
    if(ps5_shm_unmap(v->base,v->size,flags))return 0;
    release_locked(v->object);free(v->pages);memset(v,0,sizeof(*v));return 1;
}
HANDLE CreateFileMappingA(HANDLE file,LPSECURITY_ATTRIBUTES sa,DWORD protect,DWORD high,DWORD low,LPCSTR name)
{
    (void)sa;(void)protect;(void)name;
    size_t n=((size_t)high<<32)|low;
    if(file!=INVALID_HANDLE_VALUE || !n || n%PAGE_SIZE_PS5){SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
    struct backing *b=new_backing(n);
    if(!b){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    return w32_direct_mapping_handle(b,release_handle);
}
HANDLE CreateFileMappingW(HANDLE f,LPSECURITY_ATTRIBUTES s,DWORD p,DWORD h,DWORD l,LPCWSTR n)
{ (void)n;return CreateFileMappingA(f,s,p,h,l,NULL); }
LPVOID MapViewOfFileEx(HANDLE handle,DWORD access,DWORD high,DWORD low,SIZE_T count,LPVOID at)
{
    pthread_mutex_lock(&vm_lock);
    struct backing *b=w32_direct_mapping_payload(handle);
    size_t off=((size_t)high<<32)|low;
    void *result=NULL;
    if(b && off<=b->requested) result=map_locked(b,off,count?count:b->requested-off,at,access==FILE_MAP_READ?PAGE_READONLY:PAGE_READWRITE);
    pthread_mutex_unlock(&vm_lock);
    if(!result)SetLastError(b?ERROR_INVALID_ADDRESS:ERROR_INVALID_HANDLE);
    return result;
}
LPVOID MapViewOfFile(HANDLE h,DWORD a,DWORD hi,DWORD lo,SIZE_T n)
{ return MapViewOfFileEx(h,a,hi,lo,n,NULL); }
BOOL UnmapViewOfFile(LPCVOID p)
{
    pthread_mutex_lock(&vm_lock);
    struct region *v=find(views,p,1);
    int ok=v && v->base==p && unmap_locked(v);
    pthread_mutex_unlock(&vm_lock);return ok;
}
/* A write to a committed contiguous page faulted as read-only (Xbox VA
 * 0x808E8014, DSOUND, observed) with no protection change in the log. A
 * change that takes write away from more than was asked (the request rounded
 * out to 16 KiB) goes to klog: the request, the span, and the caller; it found
 * MmSetAddressProtect. klog and snprintf only: the
 * AC97 trap changes protection inside the signal handler. */
#ifdef __PROSPERO__
extern int sceKernelDebugOutText(int,const char *);
static void note_readonly(const char *what,const void *at,size_t size,DWORD protect,uintptr_t start,size_t n,const void *caller)
{
    static _Atomic unsigned seen;
    if(!n || (protection(protect)&PS5_SHM_WRITE) || (start==(uintptr_t)at && n==size) || atomic_fetch_add(&seen,1)>=64)return;
    char line[256];
    snprintf(line,sizeof line,"[reSOT] [MEM] %s %p+%#zx protect %#x covers %p+%#zx from %p\n",
             what,at,size,(unsigned)protect,(void *)start,n,caller);
    sceKernelDebugOutText(0,line);
}
#else
static void note_readonly(const char *w,const void *a,size_t s,DWORD p,uintptr_t st,size_t n,const void *c)
{ (void)w;(void)a;(void)s;(void)p;(void)st;(void)n;(void)c; }
#endif
/* Host page protection is recorded per 16 KiB, including subrange changes. */
static int protect_locked(struct region *v,uintptr_t start,size_t n,DWORD protect,PDWORD old)
{
    if(!contains(v,(void *)start,n))return 0;
    if(mprotect((void *)start,n,protection(protect)))return 0;
    size_t first=(start-(uintptr_t)v->base)/PAGE_SIZE_PS5;
    if(old)*old=v->pages[first];
    for(size_t i=first;i<first+n/PAGE_SIZE_PS5;i++)v->pages[i]=protect;
    return 1;
}
LPVOID VirtualAlloc(LPVOID at,SIZE_T size,DWORD type,DWORD protect)
{
    size_t n=rounded(size);void *p=NULL;
    if(!n || (at && (uintptr_t)at%PAGE_SIZE_PS5 && !(type==MEM_COMMIT))) {SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
    pthread_mutex_lock(&vm_lock);
    struct region *existing=at?find(views,at,size):NULL;
    if(existing && (type&MEM_COMMIT) && !(type&MEM_RESERVE)) {
        uintptr_t start=(uintptr_t)at&~(PAGE_SIZE_PS5-1);
        size_t span=rounded((uintptr_t)at-start+size);
        if(protect_locked(existing,start,span,protect,NULL)) {
            p=at;note_readonly("VirtualAlloc commit",at,size,protect,start,span,__builtin_return_address(0));
        }
    } else if((type&MEM_RESERVE) && !(type&MEM_COMMIT)) {
        struct region *r=empty(ranges);
        if(r && !ps5_vrange_reserve(n,at,0x10000,&p)) {
            if(at && p!=at){ps5_vrange_release(p,n);p=NULL;}
            else *r=(struct region){p,n,NULL,PAGE_NOACCESS};
        }
    } else if(type&MEM_COMMIT) {
        struct backing *b=new_backing(n);
        if(b){p=map_locked(b,0,n,at,protect);release_locked(b);}
    }
    pthread_mutex_unlock(&vm_lock);
    if(!p)SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
BOOL VirtualFree(LPVOID at,SIZE_T size,DWORD type)
{
    int ok=0;pthread_mutex_lock(&vm_lock);
    struct region *v=find(views,at,size?size:1),*r=find(ranges,at,size?size:1);
    if(type&MEM_DECOMMIT) {
        if(v) {
            uintptr_t start=(uintptr_t)at&~(PAGE_SIZE_PS5-1);
            size_t span=rounded((uintptr_t)at-start+size);
            ok=span && protect_locked(v,start,span,PAGE_NOACCESS,NULL);
        }
    } else if(type&MEM_RELEASE) {
        if(v && at==v->base && (!size || size==v->size))ok=unmap_locked(v);
        else if(r && at==r->base && (!size || size==r->size)) {
            /* A reservation can only be released once its views are gone. */
            if(!overlap(r->base,r->size) && !ps5_vrange_release(r->base,r->size)){memset(r,0,sizeof(*r));ok=1;}
        } else if(r && size && !overlap(at,size))ok=1; /* hole remains owned until replaced */
    }
    pthread_mutex_unlock(&vm_lock);
    if(!ok)SetLastError(ERROR_INVALID_ADDRESS);
    return ok;
}
BOOL VirtualProtect(LPVOID at,SIZE_T size,DWORD protect,PDWORD old)
{
    int ok=0;pthread_mutex_lock(&vm_lock);
    uintptr_t start=(uintptr_t)at&~(PAGE_SIZE_PS5-1);
    size_t n=rounded((uintptr_t)at-start+size);
    struct region *v=n?find(views,(void *)start,n):NULL;
    if(v)ok=protect_locked(v,start,n,protect,old);
    pthread_mutex_unlock(&vm_lock);
    if(!ok)SetLastError(ERROR_INVALID_ADDRESS);
    else note_readonly("VirtualProtect",at,size,protect,start,n,__builtin_return_address(0));
    return ok;
}
SIZE_T VirtualQuery(LPCVOID at,PMEMORY_BASIC_INFORMATION out,SIZE_T size)
{
    if(!out || size<sizeof(*out))return 0;
    memset(out,0,sizeof(*out));pthread_mutex_lock(&vm_lock);
    struct region *v=find(views,at,1),*r=v?v:find(ranges,at,1);
    out->BaseAddress=(void *)at;out->Protect=PAGE_NOACCESS;out->State=MEM_FREE;
    if(r) {
        out->AllocationBase=r->base;out->AllocationProtect=r->protect;
        out->RegionSize=r->size-((uintptr_t)at-(uintptr_t)r->base);
        out->State=v?MEM_COMMIT:MEM_RESERVE;out->Protect=PAGE_NOACCESS;
        if(v) {
            size_t first=((uintptr_t)at-(uintptr_t)v->base)/PAGE_SIZE_PS5;
            size_t last=first+1;out->Protect=v->pages[first];
            while(last<v->size/PAGE_SIZE_PS5 && v->pages[last]==out->Protect)last++;
            out->RegionSize=last*PAGE_SIZE_PS5-((uintptr_t)at-(uintptr_t)v->base);
        }
        out->Type=v?MEM_PRIVATE:0;
    }
    pthread_mutex_unlock(&vm_lock);return sizeof(*out);
}
