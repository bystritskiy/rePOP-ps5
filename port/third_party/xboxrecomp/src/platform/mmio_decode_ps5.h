/* RE-SoT: PS5 x86-64 MMIO decoder. MIT.
 * Services scalar operations emitted by Clang for guest register access.
 * Unknown instructions stop the guest; they never fabricate a device value. */
#ifndef RESOT_MMIO_DECODE_PS5_H
#define RESOT_MMIO_DECODE_PS5_H
#include "win32_compat.h"
#include <string.h>
typedef uint64_t (*mmio_read_fn)(void *, uint32_t, int);
typedef void (*mmio_write_fn)(void *, uint32_t, uint64_t, int);
static inline uint64_t *mmio_ctx_reg(PCONTEXT c, unsigned r)
{
    switch(r) {
#define R(n,name) case n:return &c->name;
    R(0,Rax) R(1,Rcx) R(2,Rdx) R(3,Rbx) R(4,Rsp) R(5,Rbp) R(6,Rsi) R(7,Rdi)
    R(8,R8) R(9,R9) R(10,R10) R(11,R11) R(12,R12) R(13,R13) R(14,R14) R(15,R15)
#undef R
    default:return NULL;
    }
}
static inline uint64_t mmio_mask(int size)
{ return size==8 ? UINT64_MAX : (UINT64_C(1)<<(size*8))-1; }
static inline uint64_t mmio_reg_read(PCONTEXT c,unsigned r,int size,int rex)
{
    if(size==1&&!rex&&r>=4&&r<8)return (*mmio_ctx_reg(c,r-4)>>8)&255;
    return *mmio_ctx_reg(c,r)&mmio_mask(size);
}
static inline void mmio_reg_write(PCONTEXT c,unsigned r,int size,int rex,uint64_t v)
{
    v &= mmio_mask(size);
    if(size==1&&!rex&&r>=4&&r<8) {
        uint64_t *dst=mmio_ctx_reg(c,r-4);*dst=(*dst&~UINT64_C(0xff00))|(v<<8);
    } else {
        uint64_t *dst=mmio_ctx_reg(c,r);
        *dst=size>=4 ? v : (*dst&~mmio_mask(size))|v;
    }
}
static inline void mmio_flags(PCONTEXT c,uint64_t a,uint64_t b,uint64_t result,int size,int subtract)
{
    uint64_t mask=mmio_mask(size),sign=UINT64_C(1)<<(size*8-1);
    a&=mask;b&=mask;result&=mask;
    c->EFlags &= ~UINT64_C(0x8d5); /* OF SF ZF AF PF CF */
    if(!result)c->EFlags|=0x40;
    if(result&sign)c->EFlags|=0x80;
    unsigned byte=(unsigned)result&255;
    if((__builtin_popcount(byte)&1)==0)c->EFlags|=4;
    if(subtract) {
        if(a<b)c->EFlags|=1;
        if((a^b^result)&16)c->EFlags|=16;
        if(((a^b)&(a^result))&sign)c->EFlags|=0x800;
    }
}
/* Skip only addressing bytes: the signal supplies the actual device address. */
static inline int mmio_address_len(const uint8_t *p)
{
    unsigned mod=*p>>6,rm=*p&7;int len=1;
    if(mod==3)return 0;
    if(rm==4) { unsigned base=p[1]&7;len++;if(mod==0&&base==5)len+=4; }
    else if(mod==0&&rm==5)len+=4;
    if(mod==1)len++;else if(mod==2)len+=4;
    return len;
}
static inline uint64_t mmio_immediate(const uint8_t *p,int size)
{ uint64_t v=0;memcpy(&v,p,(size_t)size);return v; }
/* RE-SoT: the console maps eboot code execute-only, and reading the
 * instruction at Rip ended the title with SYSTEM_XO_VIOLATION (observed). The
 * host returns where the same bytes can be read: a copy of the code segment
 * from /app0/eboot.bin, or Rip itself outside it. */
const uint8_t *resot_ps5_code_bytes(uint64_t rip);
static inline int mmio_emulate(PCONTEXT c,uint32_t off,void *dev,mmio_read_fn rd,mmio_write_fn wr)
{
    if(!c||!rd||!wr)return 0;
    const uint8_t *start=resot_ps5_code_bytes(c->Rip),*p=start;
    int word=0,rex=0;
    while(p-start<4) {
        if(*p==0x66){word=1;p++;}
        else if(*p>=0x40&&*p<=0x4f){rex=*p++;}
        else break;
    }
    unsigned op=*p++;
    int size=(rex&8)?8:(word?2:4),n;
    unsigned r;
    uint64_t a,b,v;
    if(op==0x0f) {
        unsigned second=*p++;
        if(second!=0xb6&&second!=0xb7&&second!=0xbe&&second!=0xbf)return 0;
        n=mmio_address_len(p);if(!n)return 0;
        r=((*p>>3)&7)|((rex&4)?8:0);p+=n;
        int width=(second==0xb6||second==0xbe)?1:2;
        v=rd(dev,off,width)&mmio_mask(width);
        if(second==0xbe)v=(uint64_t)(int64_t)(int8_t)v;
        if(second==0xbf)v=(uint64_t)(int64_t)(int16_t)v;
        mmio_reg_write(c,r,size,rex,v);
    } else {
        n=mmio_address_len(p);if(!n)return 0;
        unsigned group=(*p>>3)&7;r=group|((rex&4)?8:0);p+=n;
        switch(op) {
        case 0x88:case 0x89:
            if(op==0x88)size=1;
            wr(dev,off,mmio_reg_read(c,r,size,rex),size);break;
        case 0x8a:case 0x8b:
            if(op==0x8a)size=1;
            mmio_reg_write(c,r,size,rex,rd(dev,off,size));break;
        case 0x63:
            if(!(rex&8))return 0;
            v=(uint64_t)(int64_t)(int32_t)rd(dev,off,4);
            mmio_reg_write(c,r,8,rex,v);break;
        case 0xc6:case 0xc7:
            if(group!=0)return 0;
            if(op==0xc6)size=1;
            v=mmio_immediate(p,size==8?4:size);p+=size==8?4:size;
            if(size==8)v=(uint64_t)(int64_t)(int32_t)v;
            wr(dev,off,v,size);break;
        case 0x84:case 0x85:case 0x38:case 0x39:case 0x3a:case 0x3b:
        case 0x08:case 0x09:case 0x20:case 0x21:case 0x30:case 0x31:
            if(!(op&1))size=1;
            a=rd(dev,off,size)&mmio_mask(size);b=mmio_reg_read(c,r,size,rex);
            if(op==0x3a||op==0x3b){v=a;a=b;b=v;}
            if(op>=0x38&&op<=0x3b)mmio_flags(c,a,b,a-b,size,1);
            else {
                if(op==0x08||op==0x09)v=a|b;
                else if(op==0x30||op==0x31)v=a^b;
                else v=a&b;
                if(op!=0x84&&op!=0x85)wr(dev,off,v&mmio_mask(size),size);
                mmio_flags(c,a,b,v,size,0);
            }break;
        case 0xf6:case 0xf7:case 0x80:case 0x81:case 0x83:
            if(op==0xf6||op==0x80)size=1;
            if(op==0xf6||op==0xf7){if(group!=0)return 0;}
            else if(group!=1&&group!=4&&group!=6&&group!=7)return 0;
            int imm=(op==0x83)?1:(size==8?4:size);
            b=mmio_immediate(p,imm);p+=imm;
            if(op==0x83)b=(uint64_t)(int64_t)(int8_t)b;
            else if(size==8)b=(uint64_t)(int64_t)(int32_t)b;
            a=rd(dev,off,size)&mmio_mask(size);b&=mmio_mask(size);
            if(group==7&&op!=0xf6&&op!=0xf7)mmio_flags(c,a,b,a-b,size,1);
            else {
                v=group==1 ? a|b : (group==6 ? a^b : a&b);
                if(op!=0xf6&&op!=0xf7)wr(dev,off,v&mmio_mask(size),size);
                mmio_flags(c,a,b,v,size,0);
            }break;
        default:return 0;
        }
    }
    c->Rip+=(uint64_t)(p-start);return 1;
}
#endif
