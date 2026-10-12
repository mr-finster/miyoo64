/* Port: the decomp's Gfx union carries its bitfield views (dma, tri, loadtile, ...) only on
 * big-endian builds (include/PR/gbi.h). These are the same fields as word arithmetic, valid
 * for native-order words (display lists we build, and ROM ones after the load-time swap).
 * F3D opcodes are signed (G_ENDDL = -72): the command byte reads back as `int cmd:8` did. */
#ifndef HH_GBI_LE_H
#define HH_GBI_LE_H
#define GFX_CMD(g)          ((s32)(s8)((u32)(g)->words.w0 >> 24))
#define GFX_DMA_PAR(g)      (((u32)(g)->words.w0 >> 16) & 0xFF)
#define GFX_DMA_ADDR(g)     ((u32)(g)->words.w1)
/* Tri: flag, v[0], v[1], v[2] = the bytes of w1, most significant first */
#define GFX_TRI_V(g, i)     (((u32)(g)->words.w1 >> (16 - 8 * (i))) & 0xFF)
/* Gloadtile.sl / .tl = w0 bits 23..12 / 11..0; stores truncate like IDO's cvt.w.s + insert */
#define GFX_SET_LOADTILE_SL(g, v) ((g)->words.w0 = ((u32)(g)->words.w0 & ~0x00FFF000u) | (((u32)(s32)(v) & 0xFFFu) << 12))
#define GFX_SET_LOADTILE_TL(g, v) ((g)->words.w0 = ((u32)(g)->words.w0 & ~0x00000FFFu) | ((u32)(s32)(v) & 0xFFFu))
#endif
