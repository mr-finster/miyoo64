/* Exercise the release's exact M4.1 state predicate. */
#include "gfx_pc.c"
#include <assert.h>
int main(void) {
    const uint32_t edge = G_RM_AA_ZB_TEX_EDGE | G_RM_AA_ZB_TEX_EDGE2;
    assert(pm_m41_edge(G_CYC_1CYCLE, edge));
    assert(!pm_m41_edge(G_CYC_2CYCLE, edge));
    assert(!pm_m41_edge(G_CYC_1CYCLE, edge ^ 1));
    puts("M4.1 exact one-cycle texture-edge predicate: PASS");
    return 0;
}
