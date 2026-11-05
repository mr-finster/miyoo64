/* hh_crc32.h: zlib's CRC-32 (tools/romtables.py uses zlib.crc32), table driven, four bytes a step.
 * Values are identical to the bit-at-a-time loop it replaced (tools/crc32_test.c checks them over
 * the real ROM range). The tables are built on the first call. */
#ifndef HH_CRC32_H
#define HH_CRC32_H

static unsigned hh_crc32(unsigned crc, const unsigned char *p, unsigned n)
{
    static unsigned t[4][256];
    static int built;
    unsigned i;
    if (!built) {
        for (i = 0; i < 256; i++) {
            unsigned c = i;
            int k;
            for (k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            t[0][i] = c;
        }
        for (i = 0; i < 256; i++) {
            t[1][i] = (t[0][i] >> 8) ^ t[0][t[0][i] & 0xFF];
            t[2][i] = (t[1][i] >> 8) ^ t[0][t[1][i] & 0xFF];
            t[3][i] = (t[2][i] >> 8) ^ t[0][t[2][i] & 0xFF];
        }
        built = 1;
    }
    crc = ~crc;
    while (n >= 4) { /* byte loads: any alignment, any host order */
        crc ^= (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
        crc = t[3][crc & 0xFF] ^ t[2][(crc >> 8) & 0xFF] ^ t[1][(crc >> 16) & 0xFF] ^ t[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n--)
        crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}

#endif
