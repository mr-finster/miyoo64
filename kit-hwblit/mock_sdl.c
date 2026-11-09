/* mock_sdl.c: LD_PRELOAD under qemu with mock_mi.c (kit-hwblit/qemu_check.sh): SDL_SetVideoMode's surface gets its
 * pixels' address in unused1, as some Miyoo SDL builds keep the physical one (the mock's "physical" = virtual), so a
 * game's whole hwblit path runs against the mock engine. Never shipped. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <SDL/SDL.h>
SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags) {
    static SDL_Surface *(*real)(int, int, int, Uint32);
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "SDL_SetVideoMode");
    SDL_Surface *s = real(w, h, bpp, flags);
    if (s) s->unused1 = (Uint32)(uintptr_t)s->pixels;
    return s;
}
