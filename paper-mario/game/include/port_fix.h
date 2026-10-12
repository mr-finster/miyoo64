/* The native port's fixups for what the game reads from the ROM (M2). Data in the ROM is big-endian and its pointers
 * are N64 addresses where the N64 loads it (a map's shapes at 0x80210000, a background at 0x80200000); the patched
 * load sites in the decomp (game/patches/decomp.patch) call these right after the read. Game side. */
#ifndef PORT_FIX_H
#define PORT_FIX_H
#ifdef PM64_PORT

#define PORT_SWAP32(x) ((x) = (__typeof__(x)) __builtin_bswap32((u32) (x)))
#define PORT_SWAP16(x) ((x) = (__typeof__(x)) __builtin_bswap16((u16) (x)))

/* an N64 address inside a buffer the N64 keeps at a fixed address -> the same place in the host's buffer */
#define PORT_RELOC(ptr, n64base, host) \
    ((ptr) = (__typeof__(ptr)) ((ptr) ? (u32) (ptr) - (u32) (n64base) + (u32) (host) : 0))

void port_log(const char *fmt, ...);                  /* the port's log (port_host.c) */
void port_fix_words(void *data, u32 bytes);           /* every 32-bit word */
void port_fix_asset_table(void *table, u32 bytes);    /* mapfs entries: name[16], offset, compressed, decompressed */
void port_fix_background(void *bg);                   /* a BackgroundHeader loaded into gBackgroundImage */
void port_fix_sprite(void *data, u32 size);           /* a sprite after decode_yay0 */
void port_fix_shape(void *data, u32 size);            /* a map's shape file, decoded into gMapShapeData */
#define PORT_BE32(x) ((u32) __builtin_bswap32((u32) (x)))

/* the panel's stretch over the scene's black frame (port_main.c): the frame the background drew, the paused scene
 * keeping its own, and how far the status bar and the speech bubble move down to stay inside the stretch */
void port_letterbox_set(s32 x0, s32 y0, s32 x1, s32 y1);
void port_letterbox_keep(void);
s32 port_hud_shift(s32 px);

/* the sound files (port_fix_audio.c) */
void port_fix_audio_words(void *data, u32 bytes);
void port_fix_audio_halves(void *data, u32 bytes);
void port_fix_init_header(void *data);
void port_fix_bank_list(void *data, u32 bytes);
void port_fix_drums(void *data, u32 bytes);
void port_fix_instruments(void *data, u32 bytes);
void port_fix_bgm(void *data, u32 size);
void port_fix_mseq(void *data);
void port_fix_sef(void *data, u32 size);
void port_fix_bk_header(void *data);
void port_fix_bk(void *data, u32 size);

#endif
#endif
