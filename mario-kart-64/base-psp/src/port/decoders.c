/**
 * C versions of the hand-written N64 assembly decoders (asm/mio0_decode.s,
 * asm/tkmk00_decode.s).  The data formats are big-endian and are read byte
 * by byte, so this is endian neutral.
 */
#include <ultra64.h>
#include <string.h>
#include <stdio.h>
#include "port.h"

static inline u32 read_be32(const u8* p) {
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------------- */
/* MIO0                                                                        */
/* ------------------------------------------------------------------------- */

void mio0decode(u8* in, u8* out) {
    u32 dest_size = read_be32(in + 4);
    u32 comp_offset = read_be32(in + 8);
    u32 uncomp_offset = read_be32(in + 12);
    const u8* bits = in + 16;
    u32 bit_idx = 0;
    u32 written = 0;

    if (in[0] != 'M' || in[1] != 'I' || in[2] != 'O' || in[3] != '0') {
        PORT_LOG("mio0decode: bad header at %p\n", in);
        return;
    }
    u32 next_pump = 16384;
    while (written < dest_size) {
        if (bits[bit_idx >> 3] & (0x80 >> (bit_idx & 7))) {
            out[written++] = in[uncomp_offset++];
        } else {
            const u8* v = in + comp_offset;
            u32 length = (v[0] >> 4) + 3;
            u32 idx = ((v[0] & 0xF) << 8) + v[1] + 1;
            u32 i;
            comp_offset += 2;
            for (i = 0; i < length; i++) {
                out[written] = out[written - idx];
                written++;
            }
        }
        bit_idx++;
        /* Keep audio fed during big decompresses (course load = the pre-race
         * "skip"). Throttled internally, so this is cheap; on N64 this loop was
         * a hidden asm decoder, so nothing here changes the game's timing. */
        if (written >= next_pump) { next_pump += 16384; port_audio_keepalive(); }
    }
}

/* asm/unused_mio0_decode.s: an older copy of the same decoder. */
void func_80040030(u8* in, u8* out) {
    mio0decode(in, out);
}

/* asm/mio0_decode.s: ghost-data helpers used by replays.c.  func_80040174
 * prepares the input for mio0encode; mio0encode returns the compressed size.
 * Ghost saving needs a Controller Pak, which the port does not emulate yet,
 * so these are inert for now. */
s32 func_80040174(void* src, s32 size, s32 dst) {
    (void) src;
    (void) size;
    (void) dst;
    return 0;
}

s32 mio0encode(s32 input, s32 size, s32 output) {
    (void) input;
    (void) size;
    (void) output;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* TKMK00 (menu backgrounds): the decomp ships a C decoder for its host tools  */
/* (tools/libtkmk00.c); it is plain C, so it is compiled in as-is.           */
/* ------------------------------------------------------------------------- */

void tkmk00_decode(uint8_t* tkmk, uint8_t* tmp_buf, uint8_t* rgba16, int32_t alpha_color);

void tkmk00decode(u8* tkmk, u8* tmp_buf, u8* rgba16, s32 alpha_color) {
    tkmk00_decode(tkmk, tmp_buf, rgba16, alpha_color);
#ifdef PORT_INPUT_SCRIPT
    {
        // Debug: keep input and output so the decode can be checked on the host.
        static int n;
        char name[64];
        FILE* fp;
        u32 w = ((u32) tkmk[8] << 8) | tkmk[9];
        u32 h = ((u32) tkmk[10] << 8) | tkmk[11];
        if (n < 16) {
            snprintf(name, sizeof(name), "%s" "tkmk%02d_%ux%u_a%d.in", port_save_dir(), n, (unsigned) w, (unsigned) h, (int) alpha_color);
            fp = fopen(name, "wb");
            if (fp) { fwrite(tkmk, 1, 0xCE00, fp); fclose(fp); }
            snprintf(name, sizeof(name), "%s" "tkmk%02d_%ux%u_a%d.out", port_save_dir(), n, (unsigned) w, (unsigned) h, (int) alpha_color);
            fp = fopen(name, "wb");
            if (fp) { fwrite(rgba16, 1, w * h * 2, fp); fclose(fp); }
            PORT_LOG("tkmk00 %d: %ux%u alpha %d in %p out %p tmp %p\n", n, (unsigned) w, (unsigned) h, (int) alpha_color, tkmk, rgba16, tmp_buf);
            n++;
        }
    }
#endif
}
