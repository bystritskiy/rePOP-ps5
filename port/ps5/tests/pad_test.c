#include "pad_ps5.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    ResotPadData p = {.connected=1,.lx=128,.ly=128,.rx=128,.ry=128};
    XBOX_GAMEPAD g;
    resot_ps5_translate_pad(&p,&g);
    XBOX_GAMEPAD zero = {0}; assert(memcmp(&g,&zero,sizeof(g))==0);
    const uint32_t bits[] = {0x4000,0x2000,0x8000,0x1000,0x800,0x400};
    for(unsigned i=0;i<6;i++) {
        p.buttons=bits[i]; resot_ps5_translate_pad(&p,&g);
        for(unsigned j=0;j<8;j++) assert(g.bAnalogButtons[j]==(i==j?255:0));
    }
    p.buttons=0x10|0x40|0x80|0x20|0x08|0x100000|0x02|0x04;
    p.l2=123; p.r2=231; p.lx=0;p.ly=0;p.rx=255;p.ry=255;
    resot_ps5_translate_pad(&p,&g);
    assert(g.wButtons==255 && g.bAnalogButtons[6]==123 && g.bAnalogButtons[7]==231);
    assert(g.sThumbLX==-32768 && g.sThumbLY==32767 && g.sThumbRX==32767 && g.sThumbRY==-32767);
    p.connected=0;resot_ps5_translate_pad(&p,&g);assert(memcmp(&g,&zero,sizeof(g))==0);
    for(unsigned i=120;i<=136;i++) {
        p.connected=1;p.lx=p.ly=p.rx=p.ry=(uint8_t)i;
        resot_ps5_translate_pad(&p,&g);
        assert(!g.sThumbLX&&!g.sThumbLY&&!g.sThumbRX&&!g.sThumbRY);
    }
    puts("DualSense mapping: pass");
}
