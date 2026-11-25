#include "gfx_soft.c"
void gfx_cc_get_features(uint32_t id,struct CCFeatures *cc){(void)id;memset(cc,0,sizeof(*cc));}
uint32_t test_eval(void){return pm_gen_eval(0x64646464u,0x50505050u,255,255,255,255);}
int test_fast(void){return sGen.fast;}
int test_k4(void){return sGenConst[S_K4][0];}
int test_k4_used(void){return sGen.k4_used;}
