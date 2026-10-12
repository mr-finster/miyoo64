/* Big-endian -> native byte-order maps of the ROM's asset segments, generated
 * from the decomp's own build by tools/gen_swapmap.py (numbers only). */
#ifndef PORT_SWAPMAP_H
#define PORT_SWAPMAP_H

#include <PR/ultratypes.h>

typedef struct {
    u32 words; /* run length in 32-bit words */
    u32 perm;  /* index into gSwapPerms: native byte i = BE byte gSwapPerms[perm][i] */
} SwapRun;

typedef struct {
    const char* name;
    u32 rom;        /* ROM offset of the segment (of its MIO0 block if compressed) */
    u32 vram;       /* N64 VRAM (segmented address) */
    u32 size;       /* size of the (decompressed) image */
    u32 compressed; /* 1: stored as one MIO0 block; the map applies after decoding */
    u32 nruns;
    const SwapRun* runs;
} SwapSegment;

typedef struct {
    u32 rom;  /* ROM offset of an individually compressed texture's MIO0 block */
    u32 size; /* decoded size */
    u32 perm;
} SwapBlock;

extern const u8 gSwapPerms[24][4];
extern const SwapSegment gSwapSegments[];
extern const int gSwapSegmentCount;
extern const SwapBlock gSwapBlocks[];
extern const int gSwapBlockCount;

#endif
