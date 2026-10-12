/* vtxstore_test.c: GoldenEye's vertex store (src/game/vtxstore.c, the real file, built with the game's flags) on the
 * host (2026-10-01, the Dam's exploded crates). objDeform asks it for a copy of a destroyed object's vertices
 * (type 0x0B0B: 1500 vertices in 40 batches in 1-player missions but Cradle/Statue; chr.c's 0xCCCC: 500 in 20).
 *
 * The bug: vtxstore_allocate read the batch count as ((s16 *)&count)[1] -- the low halfword on the big-endian N64,
 * the HIGH halfword (0) on the little-endian port. So only batch 0 was searched and it was never split: the first
 * destroyed object took the whole store and every later one failed (objDeform's fallback: no blackening, no
 * crumpling, the object squashed by 0.85 and moved by 15% of its height). With the store's free count at 0,
 * vtxstore_tick (sub_GAME_7F09BBBC, every frame) found it under a quarter and called sub_GAME_7F056690, which frees
 * a deformed object outright -- the first exploded crate vanished the frame after it deformed.
 *
 * Checks: N allocations of typical object sizes succeed while they fit, each in its own batch, free count right,
 * no eviction request; freeing returns everything (merged back to one batch). Run: vtxstore_test.sh. */
#include <ultra64.h>
#include "bondtypes.h"
#include "vtxstore.h"
int printf(const char *, ...); /* (the game's headers clash with the host's stdio/stdlib) */
void *calloc(unsigned int, unsigned int);

/* the game's side of vtxstore.c */
void *mempAllocBytesInBank(u32 n, u8 bank) { (void)bank; return calloc(1, n); }
s32 getPlayerCount(void) { return 1; }
s32 lvlGetCurrentStageToLoad(void) { return 33; /* the Dam */ }
void *chrpropGetActiveTail(void) { return NULL; }
void *modelGetNodeRwData(void *m, void *n) { (void)m; (void)n; return NULL; }
static int sEvict;
void sub_GAME_7F056690(void) { sEvict++; }

extern s16 word_CODE_bss_8007A0F2;
extern s32 dword_CODE_bss_8007A0D8;

int main(void) {
    static const int sizes[] = { 24, 36, 24, 60, 24, 100, 48, 24, 36, 24 }; /* crates, boxes, desks (vertices) */
    Vertex *got[10];
    int fail = 0, ok = 0, i;
    sub_GAME_7F09B820();
    printf("store: %d vertices, free %d\n", (int)dword_CODE_bss_8007A0D8, (int)word_CODE_bss_8007A0F2);
    for (i = 0; i < 10; i++) {
        got[i] = (Vertex *)vtxstore_allocate(sizes[i], 0x0B0B, 0x1000 + i, 1);
        sub_GAME_7F09BBBC(); /* the frame's tick */
        printf("  alloc %3d -> %s, free %d%s\n", sizes[i], got[i] ? "ok" : "FAILED", (int)word_CODE_bss_8007A0F2,
               sEvict ? " (eviction requested)" : "");
        if (got[i]) ok++;
        if (!got[i] || sEvict) fail = 1;
        if (i > 0 && got[i] && got[i - 1] && got[i] != got[i - 1] + sizes[i - 1]) { printf("    not packed\n"); fail = 1; }
    }
    for (i = 0; i < 10; i++) if (got[i]) sub_GAME_7F09C044(got[i]);
    printf("after freeing: free %d\n", (int)word_CODE_bss_8007A0F2);
    if (word_CODE_bss_8007A0F2 != (s16)dword_CODE_bss_8007A0D8) fail = 1;
    Vertex *all = (Vertex *)vtxstore_allocate((s32)dword_CODE_bss_8007A0D8, 0x0B0B, 1, 1);
    printf("whole store again: %s\n", all ? "ok" : "FAILED");
    if (!all) fail = 1;
    printf("%d of 10 allocations, %s\n", ok, fail ? "vtxstore_test: FAIL" : "vtxstore_test: PASS");
    return fail;
}
