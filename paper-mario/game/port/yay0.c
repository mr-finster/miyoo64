/* Yay0 decompression in C for the native port (M2); the decomp keeps it as MIPS assembly (src/os/decode_yay0.s).
 * The stream is big-endian as it sits in the ROM: "Yay0", the decompressed size, the offset of the back-reference
 * table (16-bit entries) and of the literal/length bytes, then 32-bit mask words, one bit per step (1 = copy a
 * literal byte, 0 = copy from the output: a 12-bit distance minus 1 and a 4-bit length minus 2, 0 meaning a length
 * byte + 18 follows). The same format as MK64's MIO0 with a separate table. */
#include "common.h"

static inline u32 yay0_be32(const u8 *p) { return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]; }

void decode_yay0(void *src, void *dst) {
    const u8 *s = src;
    u8 *d = dst;
    const u32 size = yay0_be32(s + 4);
    u32 link = yay0_be32(s + 8), chunk = yay0_be32(s + 12), maskAt = 16, mask = 0;
    s32 bits = 0;
    u8 *const end = d + size;

    while (d < end) {
        if (bits == 0) {
            mask = yay0_be32(s + maskAt);
            maskAt += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            *d++ = s[chunk++];
        } else {
            const u32 pair = (u32) s[link] << 8 | s[link + 1];
            const u8 *from = d - (pair & 0xFFF) - 1;
            u32 len = pair >> 12;
            link += 2;
            len = len == 0 ? (u32) s[chunk++] + 18 : len + 2;
            while (len-- && d < end)
                *d++ = *from++;
        }
        mask <<= 1;
        bits--;
    }
}
