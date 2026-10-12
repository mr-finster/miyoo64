#include <ultra64.h>


/**
 * Decode RLE-compressed 8-bit image data into a raw byte buffer.
 */
void rle_expand_8bit(u8 *src, u8 *dst)
{
    u8 *in;
    s32 remaining;
    s32 count;
    s32 more;
    u16 w, h;
    u8 value;
#ifdef PORT /* the header is big-endian in the ROM data */
    w = (u16)((src[0] << 8) | src[1]);
    h = (u16)((src[2] << 8) | src[3]);
#else
    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
#endif
    remaining = w * h;
    in = &src[10];

    do {
        count = *in++;
        value = *in++;
        remaining -= count;
        count--;

        do {
            more = 0 < count;
            count--;
            *dst++ = value;
        } while (more);
    } while (remaining > 0);
}


/**
 * Decode RLE-compressed RGB data into RGBA5551 pixels.
 * 
 * Unreferenced
 */
void rle_expand_rgb_to_u16_5551(u8 *src, u16 *dst)
{
    u8 *in;
    s32 remaining;
    s32 count;
    u8 r, g, b;
    u16 w, h;
    s32 pixel;
#ifdef PORT /* the header is big-endian in the ROM data */
    w = (u16)((src[0] << 8) | src[1]);
    h = (u16)((src[2] << 8) | src[3]);
#else
    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
#endif
    remaining = w * h;
    in = &src[10];

    do {
        count = in[0];
        r = in[1];
        g = in[2];
        b = in[3];
        pixel = ((b >> 3) << 11) | ((g >> 3) << 6) | ((r >> 3) << 1) | 1;
        remaining -= count;
        in += 4;
        count--;

        do {
#ifdef PORT /* texels in N64 byte order */
            *dst++ = (u16)((pixel >> 8) | (pixel << 8));
#else
            *dst++ = pixel;
#endif
        } while (count-- > 0);
    } while (remaining > 0);
}


/**
 * Decode RLE-compressed RGB data into RGBA32 pixels.
 * 
 * Unreferenced
 */
void rle_expand_rgb_to_rgba32(u8 *src, u8 *dst)
{
    u8 *in;
    u8 *out;
    s32 remaining;
    s32 count;
    u8 r, g, b;
    u16 w, h;
    
    out = dst;
#ifdef PORT /* the header is big-endian in the ROM data */
    w = (u16)((src[0] << 8) | src[1]);
    h = (u16)((src[2] << 8) | src[3]);
#else
    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
#endif
    remaining = w * h;
    in = &src[10]; // Start of RLE stream.

    do {
        count = in[0];
        r = in[1];
        g = in[2];
        remaining -= count;
        b = in[3];
        in += 4;
        count--;
        do {
            out[0] = b;
            out[1] = g;
            out[2] = r;
            out[3] = 0xFF; 
            out += 4;
        } while (count-- > 0);
    } while (remaining > 0);
}
