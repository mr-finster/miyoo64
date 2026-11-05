/* hh_be.h: big-endian (ROM order) helpers for the load-time conversions (hh_swap.c). */
#ifndef HH_BE_H
#define HH_BE_H

#include <ultra64.h>

static inline u16 hh_bswap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 hh_bswap32(u32 v) { return __builtin_bswap32(v); }

/* In-place swaps of a field at byte offset o in a buffer p (alignment-safe). */
static inline void hh_sw16(void *p, u32 o)
{
    u8 *b = (u8 *)p + o, t = b[0];
    b[0] = b[1];
    b[1] = t;
}
static inline void hh_sw32(void *p, u32 o)
{
    u8 *b = (u8 *)p + o, t;
    t = b[0]; b[0] = b[3]; b[3] = t;
    t = b[1]; b[1] = b[2]; b[2] = t;
}

/* Read big-endian values from ROM-order bytes. */
static inline u16 hh_be16(const void *p)
{
    const u8 *b = p;
    return (u16)((b[0] << 8) | b[1]);
}
static inline u32 hh_be32(const void *p)
{
    const u8 *b = p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

#endif
