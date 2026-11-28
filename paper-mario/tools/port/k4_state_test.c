#include "gfx_pc.c"
#include <assert.h>
#include <stdarg.h>
u32 gPortProfFrame[PROF_COUNT];
float identity_matrix[4][4];
void port_log(const char *fmt, ...){(void)fmt;}
void port_log_always(const char *fmt, ...){(void)fmt;}
uint32_t port_time_us(void){return 0;}
extern uint32_t test_eval(void);
extern int test_fast(void),test_k4(void),test_k4_used(void);
static int flushes;static uint32_t flushed_pixel;
static void draw_tri(float *p,size_t n,size_t tris){(void)p;(void)n;(void)tris;flushes++;flushed_pixel=test_eval();}
static struct GfxRenderingAPI api={.draw_triangles=draw_tri};
static void setup(void){pm_gen_setup(true,false,false);}
int main(void){
    gfx_rapi=&api;rdp.pm_cc_w0=0x251a47;rdp.pm_cc_w1=0x77fcaa3e;rdp.other_mode_h=G_CYC_2CYCLE;
    rdp.prim_color.a=120;rdp.env_color.a=164;
    gfx_dp_set_convert(0);setup();assert(test_eval()==0xffcfcfcfu);
    bool gen,t0,t1;pm_draw_combine(&gen,&t0,&t1);assert(gen && t0 && t1);
    tri_state.valid=true;buf_vbo_len=1;buf_vbo_num_tris=1;
    gfx_dp_set_convert(92u<<9);assert(rdp.pm_k4==92);
    assert(flushes==1);assert(!tri_state.valid);
    assert(flushed_pixel==0xffcfcfcfu);
    setup();assert(test_eval()==0xff646464u);
    assert(test_k4_used() && test_k4()==92 && test_fast()==0);
    tri_state.valid=true;buf_vbo_len=1;buf_vbo_num_tris=1;
    int prior=flushes;gfx_dp_set_convert(0);assert(flushes==prior+1);
    assert(flushed_pixel==0xff646464u);
    setup();assert(test_eval()==0xffcfcfcfu);
    tri_state.valid=true;prior=flushes;gfx_dp_set_convert(0);assert(tri_state.valid && flushes==prior);
    gfx_dp_set_convert(511u<<9);assert(rdp.pm_k4==511);setup();
    assert(test_k4_used() && test_k4()==511);
    Gfx commands[3]={0};commands[0].words.w0=0xec000000u;commands[0].words.w1=92u<<9;
    commands[1].words.w0=0xec000000u;commands[1].words.w1=0x180u<<9;
    commands[2].words.w0=0xdf000000u;gfx_cmd_budget=0;gfx_run_dl(commands);
    assert(rdp.pm_k4==0x180);setup();assert(test_k4()==0x180);
    puts("SetConvert ordering, flush, invalidation, generic rebuild and K4 state: PASS");
}
