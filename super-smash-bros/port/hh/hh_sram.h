/* hh_sram.h -- the cartridge's 32 KiB SRAM, backed by <save dir>/sram.bin.
 * Every write goes to disk at once and atomically (tmp, fsync, rename): a
 * power cut leaves the previous file or the new one, never a torn one. The
 * game writes its backup data in host byte order (the port's own format).
 * A failed write shows GAME SAVE FAILED and is tried again, whole, at the
 * game's next write; a sram.bin that exists but cannot be read at the start
 * is never replaced that session. */
#ifndef HH_SRAM_H
#define HH_SRAM_H
#include <stdint.h>
#define HH_SRAM_SIZE 0x8000u
int hh_sram_read(uint32_t offset, void *dst, uint32_t size);
int hh_sram_write(uint32_t offset, const void *src, uint32_t size);
/* A .sra/.srm/.sav dropped into the data folder (or a cartridge save renamed sram.bin) converted into
 * sram.bin, once, at start-up (sram.c says how). */
void hh_sram_import(void);
#endif
