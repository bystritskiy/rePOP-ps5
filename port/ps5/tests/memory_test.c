/* reSOT PS5 memory backend contract with real shared mappings on the host. */
#include "platform/win32_compat.h"
#include <ps5platform/shm.h>
#include "kernel/guest_vmem.h"
#include "../../../port/game/guest_probe.h"
#include <pthread.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned objects,views,ranges;
static DWORD error;
size_t g_xbox_total_ram=0x4000000, g_xbox_map_size;
void SetLastError(DWORD e){error=e;}
DWORD GetLastError(void){return error;}
static pthread_mutex_t guest_lock=PTHREAD_MUTEX_INITIALIZER;
void AcquireSRWLockShared(PSRWLOCK p){(void)p;pthread_mutex_lock(&guest_lock);}
void ReleaseSRWLockShared(PSRWLOCK p){(void)p;pthread_mutex_unlock(&guest_lock);}
void AcquireSRWLockExclusive(PSRWLOCK p){(void)p;pthread_mutex_lock(&guest_lock);}
void ReleaseSRWLockExclusive(PSRWLOCK p){(void)p;pthread_mutex_unlock(&guest_lock);}
void xbox_ApuDmaUnmap(uint32_t a,uint32_t n){(void)a;(void)n;}
struct handle {void *payload;void (*release)(void *);};
HANDLE w32_direct_mapping_handle(void *p,void (*r)(void *))
{ struct handle *h=malloc(sizeof(*h));*h=(struct handle){p,r};return h; }
void *w32_direct_mapping_payload(HANDLE h) {return h?((struct handle *)h)->payload:NULL;}
static void close_mapping(HANDLE p){struct handle *h=p;h->release(h->payload);free(h);}
int ps5_shm_create(size_t n,struct ps5_shm *s)
{
    char name[]="/tmp/resot-shm-XXXXXX";int fd=mkstemp(name);if(fd<0)return -1;unlink(name);
    if(ftruncate(fd,n)){close(fd);return -1;}*s=(struct ps5_shm){fd,n};objects++;return 0;
}
void ps5_shm_destroy(struct ps5_shm *s){assert(objects);close((int)s->direct_start);objects--;}
int ps5_vrange_reserve(size_t n,void *hint,size_t align,void **out)
{
    (void)align;void *p=mmap(hint,n,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(p==MAP_FAILED)return -1;*out=p;ranges++;return 0;
}
int ps5_vrange_release(void *p,size_t n){if(munmap(p,n))return -1;ranges--;return 0;}
int ps5_shm_map(const struct ps5_shm *s,size_t off,size_t n,void *at,int prot,unsigned flags,void **out)
{
    void *p=mmap(at,n,prot,MAP_SHARED|((flags&PS5_SHM_FIXED)?MAP_FIXED:0),(int)s->direct_start,off);
    if(p==MAP_FAILED)return -1;*out=p;views++;return 0;
}
int ps5_shm_unmap(void *p,size_t n,unsigned flags)
{
    if(munmap(p,n))return -1;views--;
    if(flags&PS5_SHM_KEEP_RESERVED)assert(mmap(p,n,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==p);
    return 0;
}
int main(void)
{
    const size_t page=0x4000;
    unsigned char *arena=VirtualAlloc(NULL,8*page,MEM_RESERVE,PAGE_NOACCESS);assert(arena);
    MEMORY_BASIC_INFORMATION info;assert(VirtualQuery(arena,&info,sizeof info));assert(info.State==MEM_RESERVE);
    assert(VirtualFree(arena,2*page,MEM_RELEASE)); /* slice remains owned */
    HANDLE h=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,2*page,NULL);assert(h);
    unsigned char *a=MapViewOfFileEx(h,FILE_MAP_ALL_ACCESS,0,0,2*page,arena);assert(a==arena);
    unsigned char *b=MapViewOfFileEx(h,FILE_MAP_ALL_ACCESS,0,0,2*page,arena+4*page);assert(b==arena+4*page);
    a[page+37]=0x7b;assert(b[page+37]==0x7b);b[12]=0x19;assert(a[12]==0x19);
    assert(!MapViewOfFileEx(h,FILE_MAP_ALL_ACCESS,0,0,page,a)); /* cannot replace live view */
    assert(!MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,3*page,page)); /* outside object */
    close_mapping(h);assert(objects==1 && views==2); /* views survive CloseHandle */
    DWORD old;assert(VirtualProtect(a+page,page,PAGE_READONLY,&old));assert(old==PAGE_READWRITE);
    assert(VirtualQuery(a+page,&info,sizeof info));assert(info.Protect==PAGE_READONLY && info.RegionSize==page);
    assert(VirtualAlloc(a+page,4096,MEM_COMMIT,PAGE_READWRITE)==a+page);a[page+4]=2;assert(b[page+4]==2);
    assert(!VirtualFree(arena,8*page,MEM_RELEASE)); /* live views keep reservation */
    assert(UnmapViewOfFile(a));assert(objects==1);assert(UnmapViewOfFile(b));assert(objects==0);
    assert(VirtualFree(arena,8*page,MEM_RELEASE));assert(!ranges && !views);
    void *private=VirtualAlloc(NULL,page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(private);
    memset(private,0x35,page);assert(VirtualFree(private,0,MEM_RELEASE));assert(!objects);
    assert(!UnmapViewOfFile(private));assert(error);
    /* Exercise the real retail tracker inside an already owned arena, with
     * adjacent 4 KiB commits sharing one PS5 16 KiB page. */
    arena=VirtualAlloc(NULL,8*page,MEM_RESERVE,PAGE_NOACCESS);assert(arena);
    const uint32_t top=GUEST_VMEM_TOP-8*page;
    setenv("RECOMP_EXT_VMA","1",1);
    ptrdiff_t offset=(ptrdiff_t)(uintptr_t)arena-top;
    assert(guest_vmem_init(offset,0x4000000,top));assert(guest_vmem_active());
    uint32_t base=0,amount=8192,status=1;
    assert(guest_vmem_allocate(&base,&amount,MEM_RESERVE,PAGE_READWRITE,&status) && !status && base==top);
    uint32_t first=base,one=4096;
    assert(guest_vmem_allocate(&first,&one,MEM_COMMIT,PAGE_READWRITE,&status) && !status);
    uint32_t second=base+4096;one=4096;
    assert(guest_vmem_allocate(&second,&one,MEM_COMMIT,PAGE_READWRITE,&status) && !status);
    arena[3]=0xaa;arena[4099]=0xbb;
    uint32_t probe=0,expected=0x12345678;
    memcpy(arena+16,&expected,4);
    assert(resot_guest_read_word(offset,base+16,&probe) && probe==expected);
    assert(!resot_guest_read_word(offset,base+8192,&probe)); /* reserved only */
    assert(!resot_guest_read_word(offset,UINT32_MAX-1u,&probe));
    assert(!resot_guest_read_word(offset,0,NULL));
    assert(guest_vmem_accessible(base,8192));
    first=base;one=4096;
    assert(guest_vmem_free(&first,&one,MEM_DECOMMIT,&status) && !status);
    assert(arena[3]==0 && arena[4099]==0xbb && !guest_vmem_accessible(base,4096));
    assert(!resot_guest_read_word(offset,base+16,&probe)); /* decommitted 4 KiB */
    assert(resot_guest_read_word(offset,base+4096,&probe)); /* same native page */
    first=base;one=4096;
    assert(guest_vmem_allocate(&first,&one,MEM_COMMIT,PAGE_READWRITE,&status) && !status);
    assert(arena[3]==0 && arena[4099]==0xbb && guest_vmem_accessible(base,8192));
    uint32_t query[7];assert(guest_vmem_query(base,query) && query[4]==MEM_COMMIT);
    guest_vmem_shutdown();assert(!guest_vmem_active());
    assert(!resot_guest_read_word(offset,base+16,&probe));
    assert(VirtualFree(arena,8*page,MEM_RELEASE));assert(!objects && !ranges && !views);
    puts("PS5 retail extended VMA: reserve, adjacent 4 KiB commits, decommit/recommit and shutdown PASS");
    puts("PS5 direct-memory contract: aliases, protection, lifetime, bounds and cleanup PASS");
    puts("PS5 guest metadata probe: committed, reserved, decommitted, adjacent and invalid ranges PASS");
}
