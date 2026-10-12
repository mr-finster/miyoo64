/* Forced include for every handheld/proxy compile unit (Makefile.handheld -include).
 * The handheld counterpart of oot_psp_compat.h: what game code expects from the
 * port without the PSP hardware parts. */
#ifndef HH_COMPAT_H
#define HH_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/* <math.h> first: in port files (-iquote game dirs) it is the system header and
 * must be declared before the game math.h defines fabs/fabsf/sqrtf as macros; in
 * game files both names resolve to include/libc/math.h (guarded). */
#include <math.h>
#include "assert.h"
#include "math.h"

/* Real pointers are >= this (the binary is linked at 0x40000000, above the asset handles at 0x20000000+); anything
 * below with a segment number 0x01-0x0F in the top byte is an N64 segmented
 * address. Replaces oot-PSP's PSP RAM window checks. */
#define PORT_REAL_PTR_MIN 0x40000000u

#define OOT_PSP_SYSTEM_HEAP_SIZE (4U * 1024U * 1024U)
extern unsigned char gOotPspSystemHeap[OOT_PSP_SYSTEM_HEAP_SIZE];

static inline int OotPsp_IsSystemHeapRange(const void* ptr, size_t size) {
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t heapStart = (uintptr_t)gOotPspSystemHeap;

    if ((ptr == NULL) || (size == 0) || (start > UINTPTR_MAX - size)) {
        return 0;
    }
    return (start >= heapStart) && (start + size <= heapStart + OOT_PSP_SYSTEM_HEAP_SIZE);
}

/* "Is this a writable runtime address (heap/bss/stack) rather than a segmented
 * address or read-only data?" oot-PSP answers from its address map. */
static inline int OotPsp_IsRuntimeByteRange(const void* ptr, size_t size) {
    return (ptr != NULL) && (size != 0) && ((uintptr_t)ptr >= PORT_REAL_PTR_MIN);
}


/* Display-list tags (G_NOOP) the handheld game code emits for the renderer:
 * Skybox_Draw brackets the sky so gfx_soft draws it at half resolution. */
#define HH_TAG_SKY_BEGIN 0x534B5942u /* 'SKYB' */
#define HH_TAG_SKY_END 0x534B5945u   /* 'SKYE' */
/* HUD / pause screen / file select: the UI's perspective views map like the 2D
 * HUD (uniform 4:3 squeeze, no world crop) and are never bilinear-filtered. */
#define HH_TAG_UI_BEGIN 0x55494942u /* 'UIIB' */
#define HH_TAG_UI_END 0x55494945u   /* 'UIIE' */
/* OOT_HUD preview (gfx_pc, off unless OOT_HUD is set): a HUD element's texrects between HH_HUD_EL and
 * HH_HUD_EL_END are drawn 1:1 around the centre the 320->240 squeeze gives the element, cx in the
 * 320-wide layout. id: 1 hearts+magic, 2 rupees+keys, 3 B/C buttons, 4 minimap, 5 message box, 6 title,
 * 7 the ammo counts on B/C, 8 the action labels (B's texrect; A's perspective quad takes HH_HUD_KX); 15 (no element) C-left's centre for the button group. */
#define HH_HUD_EL_TAG(id, cx) (0x48560000u | (((u32)(id) & 0xFu) << 12) | ((u32)(cx) & 0xFFFu))
#define HH_HUD_EL(pkt, id, cx) gDPNoOpTag((pkt), HH_HUD_EL_TAG((id), (cx)))
#define HH_HUD_EL_END(pkt) gDPNoOpTag((pkt), 0x4856FFFFu)
/* Message box (id 5) flags in cx (OOT_MSGBOX1TO1): a box with an item icon or the ocarina staff (COMPACT), the
 * credits (CREDITS); neither = the text at the N64's size. */
#define HH_HUD_MSG_COMPACT 0x800
#define HH_HUD_MSG_CREDITS 0x400
/* x scale that draws element id's model-space quads 1:1 on this output (4/3 on a square output, 1 at 4:3 or
 * with the element off): the A button's label, a perspective quad the texrect rule can't reach. */
float gfx_hud_el_kx(int id);
#define HH_HUD_KX(id) gfx_hud_el_kx(id)
/* Prerendered room about to draw its background image (z_room.c): the rest of
 * the frame's 3D is squashed like the image -- starting with the room's own
 * depth-only occluders, which the room draws BEFORE the image. */
#define HH_TAG_PRERENDER 0x50524552u /* 'PRER' */

#endif
