#ifndef PORT_HUD_H
#define PORT_HUD_H

/* Display-list markers (G_NOOP with a tag) for the port's HUD placement (gfx_pc.c hud_fit_*):
 * the race HUD's pass, the draws in it that keep the 240-wide squeeze, and the named elements
 * laid out at 1:1 as one piece (SM64 80270dd / eb0a164's form; the lead's X27, 2026-09-28). The
 * N64 ignores G_NOOP, so only the port reads them. */
#define PORT_HUD_TAG_ON 0x48554431u       /* 'HUD1': the race HUD pass starts */
#define PORT_HUD_TAG_OFF 0x48554430u      /* 'HUD0': it ends */
#define PORT_HUD_TAG_CENTRE 0x48554432u   /* 'HUD2': menus (MK64's; not emitted here) */
#define PORT_HUD_TAG_KEEP 0x4855444Bu     /* 'HUDK': the draws up to _KEEP_END keep the squeeze */
#define PORT_HUD_TAG_KEEP_END 0x4855446Bu /* 'HUDk' */
#define PORT_HUD_TAG_GROUP 0x48554447u    /* 'HUDG': a named element's box, x0 | x1 << 12 */
#define PORT_HUD_TAG_GROUP2 0x48554467u   /* 'HUDg': y0 | y1 << 12 (y1 | PORT_HUD_KEEP_Y1: kept squeezed) */
#define PORT_HUD_KEEP_Y1 0x800u
#define PORT_FRAME_TAG_CROP 0x48554443u  /* 'HUDC': this frame's picture box, x0 | x1 << 12 (gfx_pc.c: fill the screen) */
#define PORT_FRAME_TAG_CROP2 0x48554463u /* 'HUDc': y0 | y1 << 12 */
#define PORT_TEXT_TAG 0x48554454u        /* 'HUDT': a font string's box, x0 | x1 << 12 (gfx_pc.c: menu text at 1:1) */
#define PORT_TEXT_TAG2 0x48554474u       /* 'HUDt': y0 | y1 << 12 */
#define PORT_LAYOUT_TAG_1TO1 0x4855444Fu /* 'HUDO': the 2D after it at 1:1, layout x0 | y0 << 12 at the top-left */
#define PORT_LAYOUT_TAG_END 0x4855446Fu  /* 'HUDo': back to the frame's own map */
/* or'd into y0 (y < 1024): line the element up with the one named just before it */
#define PORT_HUD_AT_LEFT 0x400u  /* its left edge on that element's left edge */
#define PORT_HUD_AT_RIGHT 0x800u /* its right edge on that element's right edge */

#if defined(TARGET_HANDHELD)
#define PORT_HUD_BEGIN(g) gDPNoOpTag((g)++, PORT_HUD_TAG_ON)
#define PORT_HUD_END(g) gDPNoOpTag((g)++, PORT_HUD_TAG_OFF)
#define PORT_HUD_KEEP_BEGIN(g) gDPNoOpTag((g)++, PORT_HUD_TAG_KEEP)
#define PORT_HUD_KEEP_END(g) gDPNoOpTag((g)++, PORT_HUD_TAG_KEEP_END)
/* A HUD element made of several draws (a label and its digits, each glyph its own texrect): its
 * box in the 320x240 layout, so the unsquish lays the whole element out at 1:1 around one centre
 * instead of widening each glyph in place. keep: the element stays squeezed (it would overlap). */
#define PORT_HUD_CLAMP12(v) ((u32) ((v) < 0 ? 0 : (v) > 0x7ff ? 0x7ff : (v)))
#define PORT_HUD_GROUP_AT(g, x0, x1, y0, y1, keep, at)                                                           \
    do {                                                                                                         \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(x0) | (PORT_HUD_CLAMP12(x1) << 12);              \
        (g)->words.w1 = PORT_HUD_TAG_GROUP;                                                                      \
        (g)++;                                                                                                   \
        (g)->words.w0 = ((u32) G_NOOP << 24) | (PORT_HUD_CLAMP12(y0) & 0x3ff) | (at) |                          \
                        ((PORT_HUD_CLAMP12(y1) | ((keep) ? PORT_HUD_KEEP_Y1 : 0)) << 12);                        \
        (g)->words.w1 = PORT_HUD_TAG_GROUP2;                                                                     \
        (g)++;                                                                                                   \
    } while (0)
#define PORT_HUD_GROUP(g, x0, x1, y0, y1, keep) PORT_HUD_GROUP_AT(g, x0, x1, y0, y1, keep, 0)
/* The frame's picture sits in this box of the 320x240 layout (a TV-safe margin around it): the port
 * stretches the box over the whole screen (the user, 2026-09-29: fill the screen). */
#define PORT_FRAME_CROP(g, x0, y0, x1, y1)                                                                       \
    do {                                                                                                         \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(x0) | (PORT_HUD_CLAMP12(x1) << 12);              \
        (g)->words.w1 = PORT_FRAME_TAG_CROP;                                                                     \
        (g)++;                                                                                                   \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(y0) | (PORT_HUD_CLAMP12(y1) << 12);              \
        (g)->words.w1 = PORT_FRAME_TAG_CROP2;                                                                    \
        (g)++;                                                                                                   \
    } while (0)
/* A font string's box (font.c), so the port can lay the string out at 1:1 as one piece on a filled
 * screen (FZX_MENU1TO1) */
#define PORT_TEXT_BOX(g, x0, y0, x1, y1)                                                                         \
    do {                                                                                                         \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(x0) | (PORT_HUD_CLAMP12(x1) << 12);              \
        (g)->words.w1 = PORT_TEXT_TAG;                                                                           \
        (g)++;                                                                                                   \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(y0) | (PORT_HUD_CLAMP12(y1) << 12);              \
        (g)->words.w1 = PORT_TEXT_TAG2;                                                                          \
        (g)++;                                                                                                   \
    } while (0)
/* The 2D up to PORT_LAYOUT_END drawn at 1:1 on a square screen, the layout's (x0, y0) at its top-left (gfx_pc.c
 * one_on: the results screens, laid out for 240 columns by menus.c when port_layout_1to1()) */
#define PORT_LAYOUT_1TO1(g, x0, y0)                                                                              \
    do {                                                                                                         \
        (g)->words.w0 = ((u32) G_NOOP << 24) | PORT_HUD_CLAMP12(x0) | (PORT_HUD_CLAMP12(y0) << 12);              \
        (g)->words.w1 = PORT_LAYOUT_TAG_1TO1;                                                                    \
        (g)++;                                                                                                   \
    } while (0)
#define PORT_LAYOUT_END(g) gDPNoOpTag((g)++, PORT_LAYOUT_TAG_END)
#else
#define PORT_HUD_BEGIN(g) ((void) 0)
#define PORT_HUD_END(g) ((void) 0)
#define PORT_HUD_KEEP_BEGIN(g) ((void) 0)
#define PORT_HUD_KEEP_END(g) ((void) 0)
#define PORT_HUD_GROUP(g, x0, x1, y0, y1, keep) ((void) 0)
#define PORT_HUD_GROUP_AT(g, x0, x1, y0, y1, keep, at) ((void) 0)
#define PORT_FRAME_CROP(g, x0, y0, x1, y1) ((void) 0)
#define PORT_TEXT_BOX(g, x0, y0, x1, y1) ((void) 0)
#define PORT_LAYOUT_1TO1(g, x0, y0) ((void) 0)
#define PORT_LAYOUT_END(g) ((void) 0)
#endif

#endif
