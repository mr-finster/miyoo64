/**
 * RSP segment table and the few globals the MK64-derived renderer expects.
 *
 * DKR addresses nearly everything in its display lists with KSEG0 pointers,
 * which the port turns into host pointers (PR/os_convert.h, #ifdef PORT); the
 * binary is linked above 0x10000000 so those never collide with a segmented
 * address (0x0S000000 + offset). Segments the game sets with gSPSegment
 * (framebuffer, z-buffer, segment 0 = base 0) resolve through this table.
 */
#include <ultra64.h>
#include "port.h"

uintptr_t gSegmentTable[16];

void port_set_segment(int seg, uintptr_t base) {
    if (seg >= 0 && seg < 16) gSegmentTable[seg] = base;
}

void *port_seg_to_ptr(uintptr_t addr) {
    if (port_is_real_ptr(addr)) return (void *) addr;
    return (void *) (gSegmentTable[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF));
}

/* gfx_pc.c's gfx_sp_reset (the matrix stack is unused under F3DDKR). */
float identity_matrix[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

/* No FPS overlay yet (M3). */
void port_gfx_overlay(void) { }

/* DKR keeps no game data in the executable: the "static memory" the texture
 * cache may skip hashing is only .text/.rodata (gfx_pc.c
 * gfx_is_static_memory), and the MK64 asset region is empty. */
char __assets_start[1], __assets_end[1];
