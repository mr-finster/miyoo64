/* The output size (handheld-kit K8, from MK64 f760b73): 320x240 on the Miyoo Mini. Elsewhere (the
 * desktop proxy) the square size, unless debug.cfg asks for 320x240 with a line of its own:
 *     screen=320x240
 * (hh_main.c's loader turns it into OOT_SCREEN): the 3D renders at the N64's own 320x240 and is
 * presented 1:1, the menu and first-start screens are centred.
 * Read once, before the first SDL_SetVideoMode, and fixed for the run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sW, sH; /* 0 = not read yet; a setting, never game state (.bss is outside the save states) */

static void screen_read(void) {
    const char* e;
    int w = 0, h = 0;
    if (sW) return;
#if defined(MIYOO)
    sW = 320; sH = 240; /* the Miyoo Mini: the N64's 320x240, doubled onto its 640x480 panel (gfx_sdl12.c) */
    return;
#endif
    sW = 240; sH = 240;
    e = getenv("OOT_SCREEN");
    if (e == NULL || e[0] == '\0') return;
    if (sscanf(e, "%dx%d", &w, &h) == 2 && w == 320 && h == 240) {
        sW = 320;
        fprintf(stderr, "screen: 320x240 (screen=320x240, experimental)\n");
    } else if (!(w == 240 && h == 240)) {
        fprintf(stderr, "screen: unknown screen=%s, using the square size\n", e);
    }
}

int HhScreen_W(void) { screen_read(); return sW; }
int HhScreen_H(void) { screen_read(); return sH; }

/* After SDL_SetVideoMode: the size the video surface really came up at wins (a panel that can't do
 * 320x240 falls back to the square size, and the render, present and menu follow it). */
void HhScreen_Confirm(int w, int h) {
    screen_read();
    if (w == sW && h == sH) return;
    fprintf(stderr, "screen: asked for %dx%d, the display came up at %dx%d; using the square size\n", sW, sH, w, h);
    sW = 240; sH = 240;
}
