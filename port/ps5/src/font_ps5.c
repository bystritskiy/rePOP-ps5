/* reSOT PS5, MIT. Small CPU font for the shared FPS and log panels and the
 * host's own text screens. */
#include <stdint.h>
#include <stddef.h>
static const uint8_t letters[26][7]={
{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
{7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
/* Lower case for paths, which are case-sensitive on the console. */
static const uint8_t small[26][7]={
{0,0,14,1,15,17,15},{16,16,22,25,17,17,30},{0,0,14,16,16,17,14},
{1,1,13,19,17,17,15},{0,0,14,17,31,16,14},{6,9,8,28,8,8,8},
{0,15,17,17,15,1,14},{16,16,22,25,17,17,17},{4,0,12,4,4,4,14},
{2,0,6,2,2,18,12},{16,16,18,20,24,20,18},{12,4,4,4,4,4,14},
{0,0,26,21,21,17,17},{0,0,22,25,17,17,17},{0,0,14,17,17,17,14},
{0,0,30,17,30,16,16},{0,0,13,19,15,1,1},{0,0,22,25,16,16,16},
{0,0,14,16,14,1,30},{8,8,28,8,8,9,6},{0,0,17,17,17,19,13},
{0,0,17,17,17,10,4},{0,0,17,17,21,21,10},{0,0,17,10,4,10,17},
{0,0,17,17,15,1,14},{0,0,31,2,4,8,31}};
static const uint8_t numbers[10][7]={
{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};

static int glyph_on(unsigned char c,int row,int col)
{
    const uint8_t *g=c>='A'&&c<='Z'?letters[c-'A']:c>='a'&&c<='z'?small[c-'a']:
        c>='0'&&c<='9'?numbers[c-'0']:NULL;
    if(g)return (g[row]&(16>>col))!=0;
    switch(c) {
    case '.':return row==6 && col==2;
    case ',':return (row==5 && col==2)||(row==6 && col==1);
    case ':':return (row==2||row==5)&&col==2;
    case '-':return row==3;
    case '_':return row==6;
    case '/':return col==4-row*4/6;
    case '|':return col==2;
    case '(':return (col==2 && (row==0||row==6))||(col==1 && row>0 && row<6);
    case ')':return (col==2 && (row==0||row==6))||(col==3 && row>0 && row<6);
    case '\'':return col==2 && row<2;
    default:return 0;
    }
}
void resot_host_draw_text_scaled(uint32_t *bits,int w,int h,int x,int y,const char *text,uint32_t argb,int scale)
{
    for(;*text;text++,x+=6*scale)
        for(int row=0;row<7;row++)for(int col=0;col<5;col++) {
            if(!glyph_on((unsigned char)*text,row,col))continue;
            for(int dy=0;dy<scale;dy++)for(int dx=0;dx<scale;dx++) {
                int px=x+col*scale+dx,py=y+row*scale+dy;
                if(px>=0&&py>=0&&px<w&&py<h)bits[py*w+px]=argb;
            }
        }
}
/* The panels are upper case, as they always were. */
void resot_host_draw_text(uint32_t *bits,int w,int h,int x,int y,const char *text,uint32_t argb)
{
    char upper[256];size_t n=0;
    for(;text[n] && n<sizeof upper-1;n++)upper[n]=text[n]>='a'&&text[n]<='z'?(char)(text[n]-32):text[n];
    upper[n]=0;
    resot_host_draw_text_scaled(bits,w,h,x,y,upper,argb,2);
}
