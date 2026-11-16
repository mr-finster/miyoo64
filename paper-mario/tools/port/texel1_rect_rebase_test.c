/* Host-only pixel regression: actual gfx_soft 2D renderer, no ROM or device.
 * Build against the pre-change gfx_soft.c with -DBEFORE and that directory first
 * on the include path; the same oracle must then report mismatches. */
#include <gfx_soft.c>
#include <gfx_cc.c>
#include <stdarg.h>
#include <assert.h>
u32 gPortProfFrame[PROF_COUNT];
struct GfxDimensions gfx_current_dimensions = { 80, 56, 4.0f/3.0f };
int gfx_trace_frames;
void port_fb_tile_note(void *p, unsigned n) { (void)p; (void)n; }
void port_log(const char *f, ...) { (void)f; }
void port_log_always(const char *f, ...) { (void)f; }
u32 port_time_us(void) { return 0; }

static uint32_t tex0[16*8], tex1[16*8], reference[80*56];
static void render(int mode, int u, int v, int ds, int dt, int rs, int rt, int wrap, int k) {
    static const uint32_t cc[4][2] = {{0x121803,0xFFFFFF78},{0x317FFF,0x5FFEF438},{0x30FE01,0x5FFEF5F8},{0xFFFFFF,0xFFFD7438}};
    const uint8_t prim[4]={220,170,140,255}, env[4]={20,30,40,255};
    const int32_t map[6]={0,ds*65536,k,0,dt*65536,k};
    Vtx2D verts[2]={{0}};
    verts[0].u=u; verts[0].v=v; verts[0].x=0; verts[0].y=0;
    verts[1].u=u+75; verts[1].v=v+53; verts[1].x=75; verts[1].y=53;
    verts[1].r=verts[1].g=verts[1].b=verts[1].a=255;
    sTex[2].wrap_s=sTex[2].wrap_t=wrap;
    gfx_soft_pm_generic(true,cc[mode][0],cc[mode][1],true,prim,env,0,ds,dt,map);
    if (mode == 0) assert(sGen.t0 && !sGen.t1);
    if (mode == 1 || mode == 3) assert(!sGen.t0 && sGen.t1);
    if (mode == 2) assert(sGen.t0 && sGen.t1 && sGen.fast == 12);
    if (k) sGen.fast = 0; /* exercise the general two-texture transform */
#ifndef BEFORE
    if (sGen.t1) gfx_soft_pm_rect_rebase(rs,rt);
#else
    (void)rs; (void)rt;
#endif
    memset(sFB,0,80*56*4);
    gfx_scegu_draw_triangles_2d((float*)verts,0,1);
}
int main(void) {
    gfx_soft_set_render_res(80,56,80,56); gfx_soft_init();
    gfx_soft_set_scissor(0,0,80,56);
    for(int y=0;y<8;y++) for(int x=0;x<16;x++) {
        tex0[y*16+x]=0xff000000u|((x*13+20)<<16)|((y*25+20)<<8)|100;
        tex1[y*16+x]=0xff000000u|((x*13+20)<<16)|((y*25+20)<<8)|(x+y+30);
    }
    sTex[1]=(SoftTex){tex0,16,8,WRAP_REPEAT,WRAP_REPEAT,true,false};
    sTex[2]=(SoftTex){tex1,16,8,WRAP_CLAMP,WRAP_CLAMP,true,false};
    sBoundTex[0]=1; sBoundTex[1]=2;
    const int q[4][4]={{0,0,0,0},{75,0,-134,0},{0,53,0,-99},{75,53,-134,-99}};
    const int rebases[4][2]={{0,0},{64,0},{0,32},{64,32}};
    int failures=0, cases=0;
    for(int mode=0;mode<3;mode++) for(int wrap=0;wrap<2;wrap++) for(int quadrant=0;quadrant<4;quadrant++) {
        int u=q[quadrant][0],v=q[quadrant][1],ds=q[quadrant][2],dt=q[quadrant][3];
        int rs=rebases[quadrant][0],rt=rebases[quadrant][1];
        render(mode,u,v,ds,dt,0,0,wrap,0); memcpy(reference,sFB,sizeof reference);
        render(mode,u-rs,v-rt,ds,dt,rs,rt,wrap,0);
        cases++; if(memcmp(reference,sFB,sizeof reference)) failures++;
    }
    /* Independent direct-texel oracle, including every Tattle pixel centre. */
    for(int wrap=0;wrap<2;wrap++) for(int quadrant=0;quadrant<4;quadrant++) {
        int u=q[quadrant][0],v=q[quadrant][1],ds=q[quadrant][2],dt=q[quadrant][3];
        int rs=rebases[quadrant][0],rt=rebases[quadrant][1],bad=0;
        render(3,u-rs,v-rt,ds,dt,rs,rt,wrap,0);
        for(int y=0;y<53;y++) for(int x=0;x<75;x++) {
            int sx=u+x+ds, sy=v+y+dt;
            if(wrap==WRAP_REPEAT) { sx=(sx%16+16)%16; sy=(sy%8+8)%8; }
            else { sx=sx<0?0:sx>15?15:sx; sy=sy<0?0:sy>7?7:sy; }
            if(sFB[y*80+x]!=tex1[sy*16+sx]) bad++;
        }
        cases++; if(bad) failures++;
    }
    /* Negative UVs, signed rebases, animated tile-0 origin, and relative shifts.
       TEXEL0 repeats every 16x8 so both representations sample it identically. */
    const int cases2[][6]={{-17,-9,-16,-8,5,3},{-72,0,-512,0,72,0},{392,53,-512,32,120,-99},{75,53,64,32,-134,-99}};
    for(int mode=0;mode<3;mode++) for(int i=0;i<4;i++) for(int k=-1;k<=1;k++) {
        if(mode!=2 && k) continue; /* only both-texel general path uses gen_t1 */
        int u=cases2[i][0],v=cases2[i][1],rs=cases2[i][2],rt=cases2[i][3],ds=cases2[i][4],dt=cases2[i][5];
        /* fast 12 uses its legacy integer mapping; exercise general mapping for shifts. */
        render(mode,u,v,ds,dt,0,0,WRAP_CLAMP,k); memcpy(reference,sFB,sizeof reference);
        render(mode,u-rs,v-rt,ds,dt,rs,rt,WRAP_CLAMP,k);
        cases++; if(memcmp(reference,sFB,sizeof reference)) failures++;
    }
    printf("2D pixel cases=%d mismatches=%d\n",cases,failures);
    return failures!=0;
}
