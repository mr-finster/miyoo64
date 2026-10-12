/* hh_input.h -- handheld keys (or desktop keyboard) -> one N64 pad with a
 * virtual stick. The stick schemes of notes/controls.md live in input.c;
 * until one is picked (M4) the default is scheme 1 (native + R half stick). */
#ifndef HH_INPUT_H
#define HH_INPUT_H
#include <stdint.h>
#include <stdbool.h>

typedef struct HhPad { uint16_t button; int8_t stick_x, stick_y; } HhPad;

void hh_input_read(HhPad *pad);           /* the game's controller poll */
void controller_sdl_key(int sdl2_scancode, bool down);   /* desktop */
void controller_handheld_key(int sym, int down);           /* handheldDL 1.2 */
#endif
