/* The N64 JPEG microcode task (njpgdsp) on the CPU.
 *
 * Prerendered rooms (the Market, the Temple of Time's exterior, shops and
 * houses) are JPEGs the game decodes when a room loads: z_jpeg.c's Jpeg_Decode
 * does the Huffman pass on the CPU, four macroblocks at a time, and hands each
 * batch to the RSP to dequantise, inverse-DCT and convert to RGBA5551. The
 * scheduler used to drop that task, so those backgrounds were never decoded.
 *
 * Data layout (JpegWork, include/jpeg.h): data[mb] holds one macroblock of
 * 64-coefficient blocks in zigzag order -- mode 0: Y0 Y1 U V (16x8, chroma
 * halved horizontally); mode 2: Y0 Y1 Y2 Y3 U V (16x16, chroma halved both
 * ways). The quantisation tables are in the same zigzag order. The decoded
 * 16-pixel-wide tile (host-endian RGBA5551, alpha set) goes back into data[mb],
 * which Jpeg_CopyToZbuffer then copies out. Standard baseline JPEG maths:
 * dequantise, reorder, 8x8 inverse DCT, level shift, JFIF YCbCr -> RGB.
 */
#include <math.h>
#include <string.h>

#include "ultra64.h"
#include "jpeg.h"

/* Zigzag position of each natural-order (row-major) coefficient. */
static const u8 sZigzagIndex[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
    41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
    46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

/* sCos[x][u] = C(u)/2 * cos((2x + 1) u pi / 16), C(0) = 1/sqrt(2), else 1. */
static float sCos[8][8];
static int sCosReady;

static void HhJpeg_InitCos(void) {
    int x, u;

    for (x = 0; x < 8; x++) {
        for (u = 0; u < 8; u++) {
            float c = (u == 0) ? (float)M_SQRT1_2 : 1.0f;
            sCos[x][u] = 0.5f * c * cosf((float)((2 * x + 1) * u) * (float)M_PI / 16.0f);
        }
    }
    sCosReady = 1;
}

/* Zigzag coefficients + zigzag quantiser -> 64 spatial samples, level-shifted
 * for luma (+128) or left centred on 0 for chroma. */
static void HhJpeg_DecodeBlock(const s16* zz, const u16* q, float out[64], float shift) {
    float coef[64], tmp[64];
    int i, x, y, u;

    for (i = 0; i < 64; i++) {
        coef[i] = (float)((s32)zz[sZigzagIndex[i]] * (s32)q[sZigzagIndex[i]]);
    }
    /* rows: tmp[y][x] = sum_u coef[y][u] * cos(x, u) */
    for (y = 0; y < 8; y++) {
        const float* r = &coef[y * 8];
        for (x = 0; x < 8; x++) {
            float s = 0.0f;
            for (u = 0; u < 8; u++) {
                s += r[u] * sCos[x][u];
            }
            tmp[y * 8 + x] = s;
        }
    }
    /* columns */
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            float s = 0.0f;
            for (u = 0; u < 8; u++) {
                s += tmp[u * 8 + x] * sCos[y][u];
            }
            out[y * 8 + x] = s + shift;
        }
    }
}

static u16 HhJpeg_Rgba5551(float y, float cb, float cr) {
    int r = (int)(y + 1.402f * cr + 0.5f);
    int g = (int)(y - 0.344136f * cb - 0.714136f * cr + 0.5f);
    int b = (int)(y + 1.772f * cb + 0.5f);

    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
}

void HhJpeg_RunTask(OSTask* task) {
    JpegTaskData* td = (JpegTaskData*)task->t.data_ptr;
    /* the game passes OS_K0_TO_PHYSICAL(ptr) = ptr - 0x80000000; undo it */
    u16(*data)[0x180] = (u16(*)[0x180])(uintptr_t)(td->address + 0x80000000u);
    const u16* qY = (const u16*)(uintptr_t)(td->qTableYPtr + 0x80000000u);
    const u16* qU = (const u16*)(uintptr_t)(td->qTableUPtr + 0x80000000u);
    const u16* qV = (const u16*)(uintptr_t)(td->qTableVPtr + 0x80000000u);
    u32 mode = td->mode, mb;

    if (!sCosReady) {
        HhJpeg_InitCos();
    }
    for (mb = 0; mb < td->mbCount && mb < 4; mb++) {
        const s16* in = (const s16*)data[mb];
        float Y[4][64], U[64], V[64];
        u16 tile[16 * 16];
        int nY = (mode == 0) ? 2 : 4, b, x, y;

        for (b = 0; b < nY; b++) {
            HhJpeg_DecodeBlock(in + b * 64, qY, Y[b], 128.0f);
        }
        HhJpeg_DecodeBlock(in + nY * 64, qU, U, 0.0f);
        HhJpeg_DecodeBlock(in + (nY + 1) * 64, qV, V, 0.0f);

        if (mode == 0) { /* 16x8: Y0 | Y1, chroma halved horizontally */
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 16; x++) {
                    const float* yb = Y[x >> 3];
                    tile[y * 16 + x] = HhJpeg_Rgba5551(yb[y * 8 + (x & 7)], U[y * 8 + (x >> 1)], V[y * 8 + (x >> 1)]);
                }
            }
            memcpy(data[mb], tile, 16 * 8 * sizeof(u16));
        } else { /* 16x16: Y0 Y1 / Y2 Y3, chroma halved both ways */
            for (y = 0; y < 16; y++) {
                for (x = 0; x < 16; x++) {
                    const float* yb = Y[((y >> 3) << 1) | (x >> 3)];
                    const int c = (y >> 1) * 8 + (x >> 1);
                    tile[y * 16 + x] = HhJpeg_Rgba5551(yb[(y & 7) * 8 + (x & 7)], U[c], V[c]);
                }
            }
            memcpy(data[mb], tile, sizeof(tile));
        }
    }
}
