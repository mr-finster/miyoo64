#ifndef _SPECTRUM_H_
#define _SPECTRUM_H_
#include <ultra64.h>

typedef enum SPECTRUMROMS { HH_ENUM_INT(src_game_spectrum_h_5)
    ROM_SABRE,
    ROM_ATIC,
    ROM_JETPAC,
    ROM_JETMAN,
    ROM_ALIEN8,
    ROM_GUNFRIGHT,
    ROM_UNDER,
    ROM_KNIGHTLORE,
    ROM_PSSST,
    ROM_COOKIE
} SPECTRUMROMS;

extern void init_spectrum_game(s32 game);
extern void run_spectrum_game(void);
extern Gfx* spectrum_draw_screen(Gfx* DL);

#endif
