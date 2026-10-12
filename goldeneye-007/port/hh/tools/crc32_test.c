/* crc32_test.c: port/hh/hh_crc32.h (table driven) against the bit-at-a-time loop it replaced, over
 * the player's real ROM: the whole image, the packed data-segment range recipes.bin names (whose CRC
 * must also equal the header's crc_packed), every start offset 0..7 and the lengths around the
 * four-byte step, and chained calls. Host build:
 *   cc -O2 -Iport/hh -o crc32_test port/hh/tools/crc32_test.c && ./crc32_test <rom> <recipes.bin>
 * Reads the ROM only; prints counts and CRCs, no ROM bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hh_crc32.h"

static unsigned old_crc32(unsigned crc, const unsigned char *p, unsigned n)
{
    int k;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int main(int argc, char **argv)
{
    FILE *f, *r;
    unsigned char *rom, hdr[40];
    unsigned h[9], start, end, i, bad = 0, runs = 0, o, l;
    long n;
    if (argc < 3) return 2;
    f = fopen(argv[1], "rb");
    r = fopen(argv[2], "rb");
    if (!f || !r || fread(hdr, 1, 40, r) != 40) return 2;
    memcpy(h, hdr + 8, 32); /* count rom_start rom_end vaddr seg_size crc_packed crc_result total */
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    rom = malloc((size_t)n);
    if (fread(rom, 1, (size_t)n, f) != (size_t)n) return 2;
    if (rom[0] == 0x37) for (i = 0; i + 1 < n; i += 2) { unsigned char t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
    else if (rom[0] == 0x40) for (i = 0; i + 3 < n; i += 4) { unsigned char a = rom[i], b = rom[i + 1]; rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a; }
    start = h[1]; end = h[2];
    {
        unsigned a = old_crc32(0, rom + start, end - start), b = hh_crc32(0, rom + start, end - start);
        printf("packed range %#x..%#x (%u bytes): old %08x new %08x header %08x %s\n", start, end, end - start, a, b, h[5],
               a == b && b == h[5] ? "OK" : "MISMATCH");
        bad += !(a == b && b == h[5]);
        a = old_crc32(0, rom, (unsigned)n); b = hh_crc32(0, rom, (unsigned)n);
        printf("whole ROM (%ld bytes): old %08x new %08x %s\n", n, a, b, a == b ? "OK" : "MISMATCH");
        bad += a != b;
    }
    for (o = 0; o < 8; o++)
        for (l = 0; l < 40; l++) {
            unsigned a = old_crc32(0, rom + start + o, l), b = hh_crc32(0, rom + start + o, l);
            runs++; bad += a != b;
        }
    for (i = 0; i < 2000; i++) { /* random windows, chained from a random seed CRC */
        unsigned off = (unsigned)rand() % (unsigned)(n - 70000), len = (unsigned)rand() % 70000, seed = (unsigned)rand() * 2654435761u;
        unsigned a = old_crc32(seed, rom + off, len), b = hh_crc32(seed, rom + off, len);
        runs++; bad += a != b;
    }
    {
        unsigned a = 0, b = 0, off;
        for (off = 0; off + 777 < (unsigned)n; off += 777) { a = old_crc32(a, rom + off, 777); b = hh_crc32(b, rom + off, 777); }
        printf("chained 777-byte pieces over the ROM: old %08x new %08x %s\n", a, b, a == b ? "OK" : "MISMATCH");
        bad += a != b;
    }
    printf("%u small and random windows compared, %u differences\n", runs, bad);
    return bad != 0;
}
