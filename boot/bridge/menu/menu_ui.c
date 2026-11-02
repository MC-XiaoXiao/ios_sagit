// SPDX-License-Identifier: BSD-2-Clause
#include "menu_ui.h"
#include "assets/menu_font.h"

struct canvas { struct bridge_menu_surface *s; int unit, ox, oy; };
// EFI redraws are synchronous. Keep the bounded glyph cache off the firmware
// stack; Windows-target ARM64 otherwise emits a CRT stack-probe dependency.
static unsigned char glyph_mask[MENU_GLYPH_SCRATCH];
static int scaled(const struct canvas *c, int n) { return n * c->unit / 540; }
static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }
static uint32_t mix(uint32_t background, uint32_t color, unsigned alpha) {
    unsigned inv = 255 - alpha;
    return (((((color >> 16) & 255) * alpha + ((background >> 16) & 255) * inv + 127) / 255) << 16) |
           (((((color >> 8) & 255) * alpha + ((background >> 8) & 255) * inv + 127) / 255) << 8) |
           (((color & 255) * alpha + (background & 255) * inv + 127) / 255);
}
static void pixel(struct canvas *c, int x, int y, uint32_t color, unsigned alpha) {
    if ((unsigned)x >= c->s->width || (unsigned)y >= c->s->height || !alpha) return;
    uint32_t *p = &c->s->pixels[(unsigned)y * c->s->stride + (unsigned)x];
    *p = alpha == 255 ? color : mix(*p, color, alpha);
}
// Round-capped selection marker with 4x4 edge coverage.
static void line(struct canvas *c, int ax, int ay, int bx, int by, int thickness, uint32_t color) {
    ax=(c->ox+scaled(c,ax))*8; ay=(c->oy+scaled(c,ay))*8;
    bx=(c->ox+scaled(c,bx))*8; by=(c->oy+scaled(c,by))*8;
    int r=maximum(4,scaled(c,thickness)*4), vx=bx-ax, vy=by-ay;
    int64_t length=(int64_t)vx*vx+(int64_t)vy*vy;
    for(int y=maximum(0,(minimum(ay,by)-r)/8); y<=minimum((int)c->s->height-1,(maximum(ay,by)+r)/8); y++)
        for(int x=maximum(0,(minimum(ax,bx)-r)/8); x<=minimum((int)c->s->width-1,(maximum(ax,bx)+r)/8); x++) {
            unsigned hits=0;
            for(int sy=1;sy<8;sy+=2) for(int sx=1;sx<8;sx+=2) {
                int px=x*8+sx, py=y*8+sy;
                int64_t dot=(int64_t)(px-ax)*vx+(int64_t)(py-ay)*vy;
                int qx=ax, qy=ay;
                if(length && dot>0) {
                    if(dot>=length) { qx=bx; qy=by; }
                    else { qx+=(int)(dot*vx/length); qy+=(int)(dot*vy/length); }
                }
                int dx=px-qx,dy=py-qy;
                hits += (int64_t)dx*dx+(int64_t)dy*dy <= (int64_t)r*r;
            }
            pixel(c,x,y,color,(hits*255+8)/16);
        }
}
static const struct menu_glyph *glyph(unsigned code) {
    unsigned first=0,last=sizeof(menu_glyphs)/sizeof(menu_glyphs[0]);
    while(first<last) {
        unsigned middle=first+(last-first)/2;
        if(menu_glyphs[middle].code<code) first=middle+1; else last=middle;
    }
    if(first<sizeof(menu_glyphs)/sizeof(menu_glyphs[0]) && menu_glyphs[first].code==code) return &menu_glyphs[first];
    return &menu_glyphs['?'-32];
}
static unsigned sample(const unsigned char *mask, const struct menu_glyph *g, int x, int y) {
    int ix=x>>8, iy=y>>8;
    if(ix<0 || iy<0 || ix+1>=(int)g->width || iy+1>=(int)g->height) return 0;
    unsigned fx=(unsigned)x&255, fy=(unsigned)y&255;
    unsigned a=mask[iy*g->width+ix],b=mask[iy*g->width+ix+1];
    unsigned d=mask[(iy+1)*g->width+ix],e=mask[(iy+1)*g->width+ix+1];
    return ((a*(256-fx)+b*fx)*(256-fy)+(d*(256-fx)+e*fx)*fy+32768)>>16;
}
static void text(struct canvas *c, int lx, int baseline, int size, const uint16_t *string,
                 uint32_t color, int max_width) {
    if(!string) return;
    unsigned char *mask=glyph_mask;
    int x=c->ox+scaled(c,lx), y=c->oy+scaled(c,baseline), em=maximum(1,scaled(c,size));
    int limit=x+scaled(c,max_width);
    for(unsigned n=0; string[n] && n<128; n++) {
        const struct menu_glyph *g=glyph(string[n]);
        int advance=(g->advance*em+32)/64;
        if(x+advance>limit) break;
        unsigned decoded=0;
        for(unsigned i=0;i<g->bytes;i+=2) {
            unsigned count=menu_glyph_data[g->offset+i],value=menu_glyph_data[g->offset+i+1];
            if(count>MENU_GLYPH_SCRATCH-decoded) return;
            while(count--) mask[decoded++]=(unsigned char)value;
        }
        if(decoded!=g->width*g->height) return;
        int left=x+g->left*em/64, top=y+g->top*em/64;
        int w=(g->width*em+63)/64, h=(g->height*em+63)/64;
        for(int py=0;py<h;py++) for(int px=0;px<w;px++) {
            // Nine bilinear samples integrate the glyph mask when scaling down.
            unsigned alpha=0;
            for(int sy=1;sy<6;sy+=2) for(int sx=1;sx<6;sx+=2)
                alpha+=sample(mask,g,((px*6+sx)*64*256)/(6*em)-128,
                                    ((py*6+sy)*64*256)/(6*em)-128);
            pixel(c,left+px,top+py,color,(alpha+4)/9);
        }
        x+=advance;
    }
}
int bridge_menu_render(struct bridge_menu_surface *surface, const struct bridge_menu_view *view) {
    if(!surface || !surface->pixels || !view || !view->items || !view->count ||
       view->count>BRIDGE_MENU_MAX_ITEMS || view->selected>=view->count ||
       surface->width<320 || surface->height<320 || surface->width>4096 || surface->height>4096 ||
       surface->stride<surface->width || surface->stride>8192) return -1;
    // A single column fits either orientation without changing the GOP mode.
    struct canvas c={surface,0,0,0};
    int layout_height=surface->width>surface->height && view->count<=4 ? 720 : 960;
    c.unit=minimum((int)surface->width,(int)surface->height*540/layout_height);
    c.ox=((int)surface->width-c.unit)/2;
    c.oy=((int)surface->height-scaled(&c,layout_height))/2;
    for(unsigned y=0;y<surface->height;y++)
        for(unsigned x=0;x<surface->width;x++) surface->pixels[y*surface->stride+x]=0x101010;

    int row=76;
    int top=(layout_height-(100+(int)view->count*row))/2-20;
    text(&c,60,top+40,40,(const uint16_t *)L"启动",0xf0f0f0,420);
    int list_top=top+100;
    for(unsigned i=0;i<view->count;i++) {
        int baseline=list_top+(int)i*row+44;
        int selected=i==view->selected;
        if(selected) line(&c,61,baseline-9,72,baseline-9,2,0xededed);
        text(&c,92,baseline,24,view->items[i].title,selected?0xf0f0f0:0x858585,330);
        if(selected && view->remaining>=0) {
            int seconds=minimum(view->remaining,99);
            uint16_t number[4]={(uint16_t)('0'+seconds/10),(uint16_t)('0'+seconds%10),'s',0};
            text(&c,450,baseline-2,14,number+(seconds<10),0x858585,36);
        }
    }
    text(&c,60,layout_height-76,14,(const uint16_t *)L"音量键 选择    电源键 确认",0x737373,420);
    return 0;
}
