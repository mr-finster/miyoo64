/* Exhaustive K4 input sweep. Output is checked against Angrylion RDP Plus. */
#include "gfx_soft.c"
#include <assert.h>
u32 gPortProfFrame[PROF_COUNT];
u32 port_time_us(void){return 0;}
static uint32_t tex(int v){return (uint32_t)v*0x01010101u;}
static uint32_t go(int which,int k,int t0,int t1) {
    uint32_t w0=0x251a47,w1=0x77fcaa3e;
    const uint8_t pf[4]={0,0,0,120},ef[4]={0,0,0,164},pq[4]={75,75,75,0},eq[4]={255,32,0,0};
    const int32_t map[6]={0};
    if(which==1) w1=(w1&~0x0f000000u)|0x08000000u; /* K4 only cycle 0 */
    if(which==2) w1=(w1&~0xf0000000u)|0x80000000u; /* K4 only cycle 1 */
    if(which==3){w0=0x137fe0;w1=0x63fef378;}
    gfx_soft_pm_k4(k);
    gfx_soft_pm_generic(true,w0,w1,true,which==3?pq:pf,which==3?eq:ef,0,0,0,map);
    if(which!=3) assert(sGen.k4_used && sGen.fast==0);
    if(which==3) assert(!sGen.k4_used && sGen.rgb[0][1]==S_ZERO && sGen.rgb[0][2]==S_ZERO);
    return pm_gen_eval(tex(t0),tex(t1),255,255,255,255);
}
int main(void){
    const int vals[][2]={{20,80},{100,80},{200,40},{255,255}};
    { const uint8_t c[4]={0}; const int32_t map[6]={0};
      gfx_soft_pm_k4(92); gfx_soft_pm_generic(true,0x251a5f,0x77fcaa3e,true,c,c,0,0,0,map);
      /* Cycle 1 has RGB C=31 (zero); cycle 0 still selects K4. */
      assert(sGen.rgb[1][1]==S_K4 && sGen.rgb[1][2]==S_ZERO && sGen.k4_used);
      gfx_soft_pm_generic(true,0x251a5f | (31u << 15),0x77fcaa3e,true,c,c,0,0,0,map);
      assert(sGen.rgb[0][2]==S_ZERO && !sGen.k4_used);
    }
    { const uint8_t prim[4]={0,0,0,120}, env[4]={0,0,0,164}; const int32_t map[6]={0};
      gfx_soft_pm_k4(92); gfx_soft_pm_generic(true,0x251a47,0x77fcaa3e,false,prim,env,0,0,0,map);
      assert(sGen.k4_used && pm_gen_eval(tex(100),tex(80),255,255,255,255)==0xa4686868u);
    }
    assert(cc_ext9(0x0ff)==255 && cc_ext9(0x100)==256 && cc_ext9(0x17f)==383 && cc_ext9(0x180)==-128 && cc_ext9(0x1ff)==-1);
    for(int k=0;k<512;k++)for(int c=0;c<4;c++)for(int i=0;i<4;i++){
        uint32_t p=go(c,k,vals[i][0],vals[i][1]);fwrite(&p,4,1,stdout);
    }
}
