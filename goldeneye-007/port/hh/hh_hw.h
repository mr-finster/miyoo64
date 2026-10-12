/* hh_hw.h: state shared between hh_hw.c (the emulated N64 devices) and the platform. */
#ifndef HH_HW_H
#define HH_HW_H

#include <ultra64.h>

extern const u8 *hh_rom;  /* the cartridge, big-endian (z64 order) */
extern u32 hh_rom_size;
void hh_rom_read(void *dst, u32 romaddr, u32 len);

extern u16 hh_pad_buttons; /* controller 1, N64 button bits */
extern s8 hh_pad_stick_x, hh_pad_stick_y;
void hh_input_poll(void); /* hh_input.c: keys -> hh_pad_* (the layout lives there) */

void hh_eeprom_load(const char *path);
extern int hh_eeprom_write_failed;

extern OSViMode *hh_vi_mode;
extern int hh_vi_black;
extern f32 hh_vi_xscale, hh_vi_yscale;
extern u32 hh_vi_features;

#endif
