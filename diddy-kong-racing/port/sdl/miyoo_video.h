/* The screen calls the SDL 1.2 backend, the menu and the first-start splash make, per device.
 *
 * Miyoo Mini (MIYOO, miyoo_video.c): everything draws into a 320x240 RGB565 surface, as it would
 * on a 320x240 handheld, and each flip scales it by the largest whole factor that fits the panel
 * (2x on the 640x480 Mini / Mini Plus), centred with black borders on a bigger one (752x560 Mini
 * v4). The device's SDL keys: A = SPACE, B = LCTRL, X = LSHIFT, Y = LALT, L1 = e, R1 = t,
 * L2 = TAB, R2 = BACKSPACE, SELECT = RCTRL, START = RETURN, MENU = ESCAPE. */
#pragma once
#include <SDL/SDL.h>

#if defined(MIYOO)
SDL_Surface *miyoo_set_video_mode(int w, int h, int bpp, Uint32 flags);
int miyoo_flip(SDL_Surface *s);
/* A 640x480 32 bpp software surface (the menu, miyoo_menu.c) straight onto the panel, turned
 * as the game is, then flipped. */
void miyoo_show_full(SDL_Surface *s);
/* miyoo_menu.c: the menus' 8x8 bitmap font, each pixel k x k (16 or 32 bpp surfaces), and the
 * current notification drawn on a game frame. */
void miyoo_text(SDL_Surface *s, int x, int y, int k, const char *t, Uint32 argb);
int miyoo_text_w(const char *t, int k);
void miyoo_menu_overlay(SDL_Surface *screen);
/* The game's frames (gfx_sdl12.c), on the present thread: a 32-bit 0xXXRRGGBB frame at the
 * render size straight onto a 32 bpp panel, doubled and turned (NEON), with the notice drawn on
 * it first -- no RGB565 stage. miyoo_can_draw32 says whether this panel takes it (else the frame
 * goes the 565 way, through miyoo_flip); miyoo_draw32 runs under the present lock, then
 * miyoo_flip_panel outside it. */
int miyoo_can_draw32(int w, int h);
void miyoo_draw32(uint32_t *src, int w, int h);
void miyoo_flip_panel(void);
#define HH_SetVideoMode miyoo_set_video_mode
#define HH_Flip         miyoo_flip
#define HH_KEY_MENU     SDLK_ESCAPE
#define HH_KEY_A        SDLK_SPACE
#define HH_KEY_B        SDLK_LCTRL
#else /* other SDL 1.2 builds: plain SDL, desktop keys */
#define HH_SetVideoMode SDL_SetVideoMode
#define HH_Flip         SDL_Flip
#define HH_KEY_MENU     SDLK_q
#define HH_KEY_A        SDLK_a
#define HH_KEY_B        SDLK_b
#endif
