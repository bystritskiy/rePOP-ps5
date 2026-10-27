#include "platform/mmio_decode.h"
/* Host code is readable: decode in place. */
const uint8_t *resot_ps5_code_bytes(uint64_t rip) { return (const uint8_t *)(uintptr_t)rip; }
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t value;
static uint64_t rd(void *dev,uint32_t off,int size) { (void)dev;assert(off==8);return value; }
static void wr(void *dev,uint32_t off,uint64_t v,int size) { (void)dev;assert(off==8);value=v; }
static void check(const unsigned char *op,size_t n,RESOT_PS5_CONTEXT *c)
{
    c->Rip=(uintptr_t)op;
    assert(mmio_emulate(c,8,NULL,rd,wr));assert(c->Rip==(uintptr_t)op+n);
}
int main(void)
{
    RESOT_PS5_CONTEXT c={0};
    const unsigned char load32[]={0x8b,0x03};value=0x12345678;c.Rax=UINT64_MAX;
    check(load32,sizeof(load32),&c);assert(c.Rax==value);
    const unsigned char store64[]={0x4c,0x89,0x43,0x20};c.R8=0x123456789abcdef0;
    check(store64,sizeof(store64),&c);assert(value==c.R8);
    const unsigned char load8[]={0x0f,0xb6,0x43,0x04};value=0xff;c.Rax=UINT64_MAX;
    check(load8,sizeof(load8),&c);assert(c.Rax==255);
    const unsigned char test[]={0xf6,0x03,0x80};value=0x80;
    check(test,sizeof(test),&c);assert(!(c.EFlags&0x40)&&(c.EFlags&0x80));
    const unsigned char sib[]={0x8b,0x04,0x25,0x10,0x20,0x30,0x40};value=5;
    check(sib,sizeof(sib),&c);assert(c.Rax==5);
    const unsigned char sign64[]={0x48,0xc7,0x03,0xff,0xff,0xff,0xff};
    check(sign64,sizeof(sign64),&c);assert(value==UINT64_MAX);
    const unsigned char highbyte[]={0x88,0x23};c.Rax=0x1234;
    check(highbyte,sizeof(highbyte),&c);assert(value==0x12);
    const unsigned char highload[]={0x8a,0x23};value=0xab;c.Rax=0x11223344;
    check(highload,sizeof(highload),&c);assert(c.Rax==0x1122ab44);
    const unsigned char wordload[]={0x66,0x8b,0x03};value=0xcdef;c.Rax=0x1122334455667788;
    check(wordload,sizeof(wordload),&c);assert(c.Rax==UINT64_C(0x112233445566cdef));
    const unsigned char sx[]={0x48,0x63,0x03};value=0x80000000;
    check(sx,sizeof(sx),&c);assert(c.Rax==UINT64_C(0xffffffff80000000));
    const unsigned char cmp[]={0x83,0x3b,0xff};value=0xffffffff;
    check(cmp,sizeof(cmp),&c);assert(c.EFlags&0x40);
    const unsigned char cmp_overflow[]={0x39,0x03};c.Rax=1;value=0x80000000;
    check(cmp_overflow,sizeof(cmp_overflow),&c);assert(c.EFlags&0x800);
    const unsigned char regonly[]={0x8b,0xc3};c.Rip=(uintptr_t)regonly;
    assert(!mmio_emulate(&c,8,NULL,rd,wr));assert(c.Rip==(uintptr_t)regonly);
    const unsigned char unknown[]={0x0f,0x0b};c.Rip=(uintptr_t)unknown;
    assert(!mmio_emulate(&c,8,NULL,rd,wr));assert(c.Rip==(uintptr_t)unknown);
    puts("PS5 x86-64 MMIO: pass");
}
