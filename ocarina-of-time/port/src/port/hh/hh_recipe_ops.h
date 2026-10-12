/* First-start recipe ops (M3b), shared by the host deriver (tools/handheld/hh_recipes.c)
 * and the device converter (hh_convert.c) so both see the same tables.
 *
 * A unit (a native pack entry, or one symbol of the embedded region) of n bytes is
 * rebuilt from a source of the same length, the player's ROM file (or zeros):
 *   per 8-byte block, one byte: 0..55 one of the 56 byte-order permutations (every
 *     split of 8 into 8/4/2/1-byte fields, each field reversed; oot-PSP's OPZ4 set),
 *     56 zero, 57 two word ops follow;
 *   a tail of 4..7 bytes: 0x80 then one word op; then per remaining byte: 0..3 an
 *     index into the last partial word of the source, 0xFE zero, 0xFF + a literal byte;
 *   word op: 0..23 one of the 24 permutations of the source word's bytes, 24 zero,
 *     25 VROM (the source's big-endian word is a ROM file's start or end, rewritten to
 *     that file's place in the pack), 26 + a literal little-endian word. */
#ifndef HH_RECIPE_OPS_H
#define HH_RECIPE_OPS_H

#include <stdint.h>
#include <string.h>

#define HHR_NP8 56
#define HHR_ZERO 56
#define HHR_SPLIT 57
#define HHR_TAILWORD 0x80
#define HHR_W_ZERO 24
#define HHR_W_VROM 25
#define HHR_W_LIT 26

typedef struct {
    unsigned char p8[HHR_NP8][8];
    unsigned char p4[24][4];
} HhrPerms;

static void hhr_gen8(HhrPerms* t, int* n, int rem, int* parts, int np) {
    static const int ws[4] = { 8, 4, 2, 1 };
    int i;
    if (rem == 0) {
        int o = 0, k = 0, j;
        for (i = 0; i < np; i++) {
            for (j = parts[i] - 1; j >= 0; j--) t->p8[*n][k++] = (unsigned char)(o + j);
            o += parts[i];
        }
        (*n)++;
        return;
    }
    for (i = 0; i < 4; i++) {
        if (ws[i] <= rem) {
            parts[np] = ws[i];
            hhr_gen8(t, n, rem - ws[i], parts, np + 1);
        }
    }
}

static int hhr_perms(HhrPerms* t) {
    int parts[8], n = 0, a, b, c, d, m = 0;
    hhr_gen8(t, &n, 8, parts, 0);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++)
            for (c = 0; c < 4; c++)
                for (d = 0; d < 4; d++)
                    if (a != b && a != c && a != d && b != c && b != d && c != d) {
                        t->p4[m][0] = (unsigned char)a; t->p4[m][1] = (unsigned char)b;
                        t->p4[m][2] = (unsigned char)c; t->p4[m][3] = (unsigned char)d;
                        m++;
                    }
    return n == HHR_NP8 && m == 24;
}

static uint32_t hhr_be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t hhr_le32(const unsigned char* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

/* vrom(ctx, romWord, &out): 1 if romWord is a ROM file's start/end (out = its pack place) */
typedef int (*HhrVromFn)(void* ctx, uint32_t v, uint32_t* out);

/* Decode one unit: R (source, n bytes) -> N (n bytes). *c walks the op stream up to end.
 * Returns 0 on a malformed stream. */
static int hhr_decode(const HhrPerms* t, const unsigned char* R, unsigned char* N, uint32_t n,
                      const unsigned char** c, const unsigned char* end, HhrVromFn vrom, void* ctx) {
    const unsigned char* p = *c;
    uint32_t o = 0;
#define HHR_NEED(k) do { if ((size_t)(end - p) < (size_t)(k)) return 0; } while (0)
#define HHR_WORD(src, dst)                                                                  \
    do {                                                                                    \
        unsigned w;                                                                         \
        HHR_NEED(1);                                                                        \
        w = *p++;                                                                           \
        if (w < 24) {                                                                       \
            (dst)[0] = (src)[t->p4[w][0]]; (dst)[1] = (src)[t->p4[w][1]];                   \
            (dst)[2] = (src)[t->p4[w][2]]; (dst)[3] = (src)[t->p4[w][3]];                   \
        } else if (w == HHR_W_ZERO) {                                                       \
            memset((dst), 0, 4);                                                            \
        } else if (w == HHR_W_VROM) {                                                       \
            uint32_t m;                                                                     \
            if (!vrom(ctx, hhr_be32(src), &m)) return 0;                                    \
            (dst)[0] = (unsigned char)m; (dst)[1] = (unsigned char)(m >> 8);                \
            (dst)[2] = (unsigned char)(m >> 16); (dst)[3] = (unsigned char)(m >> 24);       \
        } else if (w == HHR_W_LIT) {                                                        \
            HHR_NEED(4);                                                                    \
            memcpy((dst), p, 4);                                                            \
            p += 4;                                                                         \
        } else {                                                                            \
            return 0;                                                                       \
        }                                                                                   \
    } while (0)
    for (; o + 8 <= n; o += 8) {
        unsigned s;
        HHR_NEED(1);
        s = *p++;
        if (s < HHR_NP8) {
            const unsigned char* m = t->p8[s];
            const unsigned char* r = R + o;
            unsigned char* d = N + o;
            d[0] = r[m[0]]; d[1] = r[m[1]]; d[2] = r[m[2]]; d[3] = r[m[3]];
            d[4] = r[m[4]]; d[5] = r[m[5]]; d[6] = r[m[6]]; d[7] = r[m[7]];
        } else if (s == HHR_ZERO) {
            memset(N + o, 0, 8);
        } else if (s == HHR_SPLIT) {
            HHR_WORD(R + o, N + o);
            HHR_WORD(R + o + 4, N + o + 4);
        } else {
            return 0;
        }
    }
    if (n - o >= 4) {
        HHR_NEED(1);
        if (*p++ != HHR_TAILWORD) return 0;
        HHR_WORD(R + o, N + o);
        o += 4;
    }
    for (; o < n; o++) {
        unsigned b;
        HHR_NEED(1);
        b = *p++;
        if (b < 4 && (n - n % 4) + b < n) {
            N[o] = R[(n - n % 4) + b];
        } else if (b == 0xFE) {
            N[o] = 0;
        } else if (b == 0xFF) {
            HHR_NEED(1);
            N[o] = *p++;
        } else {
            return 0;
        }
    }
#undef HHR_WORD
#undef HHR_NEED
    *c = p;
    return 1;
}

#endif
