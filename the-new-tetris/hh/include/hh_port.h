/* hh_port.h: small helpers for game code under #ifdef PORT (force-included into every
 * game TU through ge_libc.h). */
#ifndef HH_PORT_H
#define HH_PORT_H

/* Byte 0 of a 32-bit word as the big-endian N64 stores it: for structs whose bit-fields
 * are declared in reverse under PORT so each word keeps its N64 value (image_entry). */
#define HH_IMAGE_ENTRY_BYTE0(e) ((unsigned char)(*(unsigned int *)(e) >> 24))

/* Byte n (0-7) of a display-list command as the N64 stores it (two big-endian words),
 * for commands kept in host word order (lists from the ROM are converted at load,
 * hh_swap.c; runtime lists are built in host order): n ^ 3 on a little-endian host. */
#define HH_GFXB(p, n) (((unsigned char *)(p))[(n) ^ 3])
#define HH_GFXSB(p, n) (((signed char *)(p))[(n) ^ 3])
/* Halfword n (0-3) of a command as the N64 stores it (TRI4 index decoders). */
#define HH_GFXH(p, n) (((unsigned short *)(p))[(n) ^ 1])


/* u32 -> f32 as IDO compiles it: cvt.s.w of the value as a signed int, plus 2^32 (a
 * single-precision add) when it was negative. That rounds twice, and for some values of
 * 2^31 and up gives a different float than GCC's single rounding; GE turns every random
 * number into a fraction this way, and the attract demos check the RNG every frame. */
static inline float hh_u2f(unsigned int v)
{
    float f = (float)(int)v;
    if ((int)v < 0)
        f += 4294967296.0f;
    return f;
}
#define HH_U2F(v) hh_u2f(v)

void hh_idle_forever(void);
/* big-endian ROM data read as host values (PORT only; identity on the N64) */
#define HH_BE32(x) ((unsigned int)__builtin_bswap32((unsigned int)(x)))
#define HH_BE16S(x) ((short)__builtin_bswap16((unsigned short)(x)))
/* swap n 16/32-bit words in place */
void hh_swap16n(void *p, int n);
void hh_swap32n(void *p, int n);
void hh_swap_vtx(void *p, int n);
void hh_swap_bitmaps(void *p, int n);
void hh_swap_desc(void *p, const char *desc, int size);
#endif
