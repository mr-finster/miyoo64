/*
 * pms_convert: captured N64 frames (.pmc, from tools/cap) -> compact replay scene (.pms).
 *
 * Walks each frame's F3DEX2 display list from the gfx task root, with the RSP's own
 * segment rules, and keeps only the 4 KB RDRAM pages the frame touches (commands,
 * vertices, matrices, viewports, textures, palettes). Typed structures are converted
 * to host (little-endian) layout exactly as the port will hold them natively:
 *   Gfx commands, Mtx, forced matrices   -> 32-bit words swapped
 *   Vtx (ob, flag, tc), Vp_t              -> 16-bit halves swapped (cn bytes unchanged)
 *   Light_t, textures, palettes           -> left in N64 byte order (the kit reads them as bytes / be16)
 * Pointers inside commands stay N64 addresses; the bench resolves them against its RDRAM
 * buffer with the same segment rules (bench/kit/gfx_pc.c seg_addr, PM64_REPLAY).
 *
 * The output contains ROM-derived data: local test files only, never committed or shipped.
 *
 * .pms layout (little-endian): "PMS1" u32 nframes u32 0 u32 0,
 *   then per frame (one gfx task): u32 root, u32 nregions, u32 bytes, u32 poll (tasks with the same poll are one game frame), then per region u32 addr, u32 len, data[len].
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define RDRAM_SIZE (4u << 20)
#define PAGE 4096u
#define NPAGES (RDRAM_SIZE / PAGE)

enum { K_NONE, K_WORD, K_HALF, K_BYTES };

static uint8_t *m;             /* big-endian RDRAM image of one frame */
static uint8_t kind[RDRAM_SIZE / 2]; /* per halfword: how it is converted */
static uint8_t page_used[NPAGES];
static uint32_t seg[16], rdphalf1;
static uint32_t timg_addr, timg_siz, timg_width;
static unsigned op_count[256], n_cmds, n_vtx, n_tris, n_rects, n_bad, n_cimg_other;
static uint32_t main_cimg;
static int trace; /* PMS_TRACE=1: print every walked command of the first frame */

static uint32_t be32(uint32_t a) { return (uint32_t)m[a] << 24 | m[a + 1] << 16 | m[a + 2] << 8 | m[a + 3]; }
static uint32_t phys(uint32_t a) { return (seg[(a >> 24) & 0xF] + (a & 0x00FFFFFF)) & 0x00FFFFFF; }

static int use(uint32_t a, uint32_t len, int k) {
    if (len == 0) return 1;
    if (a >= RDRAM_SIZE || a + len > RDRAM_SIZE || len > RDRAM_SIZE) { n_bad++; return 0; }
    for (uint32_t p = a / PAGE; p <= (a + len - 1) / PAGE; p++) page_used[p] = 1;
    if (k != K_NONE)
        for (uint32_t h = a / 2; h < (a + len + 1) / 2; h++) if (!kind[h]) kind[h] = (uint8_t)k;
    return 1;
}

static void walk(uint32_t addr, int depth) {
    if (depth > 18) { n_bad++; return; }
    for (unsigned guard = 0; guard < 200000; guard++, addr += 8) {
        if (!use(addr, 8, K_WORD)) return;
        uint32_t w0 = be32(addr), w1 = be32(addr + 4);
        uint8_t op = w0 >> 24;
        op_count[op]++; n_cmds++;
        if (trace) fprintf(stderr, "%*s%06X: %08X %08X\n", depth * 2, "", addr, w0, w1);
        switch (op) {
        case 0x01: { /* G_VTX */
            uint32_t n = (w0 >> 12) & 0xFF, a = phys(w1);
            n_vtx += n;
            if (use(a, n * 16, K_NONE))
                for (uint32_t i = 0; i < n; i++) use(a + i * 16, 12, K_HALF); /* cn[4] stays bytes */
            break;
        }
        case 0x05: n_tris++; break;
        case 0x06: n_tris += 2; break;
        case 0x07: n_tris += 2; break;
        case 0xDA: use(phys(w1), 64, K_WORD); break; /* G_MTX */
        case 0xDC: { /* G_MOVEMEM */
            uint32_t len = ((w0 >> 19) & 0x1F) * 8 + 8, idx = w0 & 0xFF, a = phys(w1);
            use(a, len, idx == 8 ? K_HALF : idx == 10 ? K_BYTES : K_WORD); /* viewport s16 / lights bytes / matrices */
            break;
        }
        case 0xDB: /* G_MOVEWORD */
            if (((w0 >> 16) & 0xFF) == 6) seg[((w0 & 0xFFFF) >> 2) & 0xF] = w1 & 0x00FFFFFF;
            break;
        case 0xE1: rdphalf1 = w1; break;
        case 0x04: walk(phys(rdphalf1), depth + 1); break; /* G_BRANCH_Z: conservatively walk the target */
        case 0xDE: /* G_DL */
            if ((w0 >> 16) & 1) { addr = phys(w1) - 8; break; } /* branch, no return */
            walk(phys(w1), depth + 1);
            break;
        case 0xDF: return; /* G_ENDDL */
        case 0xFD: timg_addr = phys(w1); timg_siz = (w0 >> 19) & 3; timg_width = (w0 & 0xFFF) + 1; break;
        case 0xF3: { /* G_LOADBLOCK */
            uint32_t lrs = (w1 >> 12) & 0xFFF, bytes = timg_siz == 0 ? (lrs + 2) / 2 : (lrs + 1) << (timg_siz - 1);
            use(timg_addr, bytes, K_NONE);
            break;
        }
        case 0xF4: { /* G_LOADTILE */
            uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, lrt = w1 & 0xFFF;
            uint32_t bpp2 = timg_siz == 0 ? 1 : 2u << (timg_siz - 1); /* bytes per texel x2 */
            uint32_t row = timg_width * bpp2 / 2;
            uint32_t s = timg_addr + (ult >> 2) * row + (uls >> 2) * bpp2 / 2;
            uint32_t e = timg_addr + (lrt >> 2) * row + ((lrs >> 2) + 1) * bpp2 / 2;
            if (e > s) use(s, e - s, K_NONE);
            break;
        }
        case 0xF0: use(timg_addr, (((w1 >> 14) & 0x3FF) + 1) * 2, K_NONE); break; /* G_LOADTLUT */
        case 0xE4: case 0xE5: n_rects++; break;
        case 0xFF: /* G_SETCIMG */
            if (!main_cimg) main_cimg = phys(w1);
            else if (phys(w1) != main_cimg) n_cimg_other++;
            break;
        default: break;
        }
    }
    n_bad++;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: pms_convert out.pms frame.pmc...\n"); return 1; }
    FILE *out = fopen(argv[1], "wb");
    if (!out) { perror(argv[1]); return 1; }
    uint32_t hdr[4] = { 0x31534D50 /* "PMS1" */, (uint32_t)(argc - 2), 0, 0 };
    fwrite(hdr, sizeof hdr, 1, out);
    m = malloc(RDRAM_SIZE);
    uint8_t *conv = malloc(RDRAM_SIZE);
    unsigned long total = 0;
    for (int f = 2; f < argc; f++) {
        FILE *in = fopen(argv[f], "rb");
        uint32_t ph[8];
        if (!in || fread(ph, sizeof ph, 1, in) != 1 || ph[0] != 0x31434D50 || fread(m, RDRAM_SIZE, 1, in) != 1) {
            fprintf(stderr, "bad capture %s\n", argv[f]); return 1;
        }
        fclose(in);
        memset(kind, 0, sizeof kind); memset(page_used, 0, sizeof page_used); memset(seg, 0, sizeof seg);
        memset(op_count, 0, sizeof op_count);
        n_cmds = n_vtx = n_tris = n_rects = n_bad = n_cimg_other = 0; main_cimg = 0;
        uint32_t root = ph[1] & 0x00FFFFFF;
        trace = f == 2 && getenv("PMS_TRACE");
        walk(root, 0);
        /* convert: copy, then swap typed ranges */
        memcpy(conv, m, RDRAM_SIZE);
        for (uint32_t h = 0; h < RDRAM_SIZE / 2; h++) {
            if (kind[h] == K_HALF) { uint8_t t = conv[h * 2]; conv[h * 2] = conv[h * 2 + 1]; conv[h * 2 + 1] = t; }
            else if (kind[h] == K_WORD && !(h & 1)) {
                uint32_t v; memcpy(&v, conv + h * 2, 4); v = __builtin_bswap32(v); memcpy(conv + h * 2, &v, 4);
            }
        }
        /* regions = runs of used pages */
        uint32_t nreg = 0, bytes = 0;
        for (uint32_t p = 0; p < NPAGES; p++) if (page_used[p] && (p == 0 || !page_used[p - 1])) nreg++;
        for (uint32_t p = 0; p < NPAGES; p++) if (page_used[p]) bytes += PAGE;
        uint32_t fh[4] = { root, nreg, bytes, ph[5] };
        fwrite(fh, sizeof fh, 1, out);
        for (uint32_t p = 0; p < NPAGES; p++) {
            if (!page_used[p] || (p > 0 && page_used[p - 1])) continue;
            uint32_t q = p; while (q < NPAGES && page_used[q]) q++;
            uint32_t rh[2] = { p * PAGE, (q - p) * PAGE };
            fwrite(rh, sizeof rh, 1, out); fwrite(conv + p * PAGE, (q - p) * PAGE, 1, out);
        }
        total += bytes;
        fprintf(stderr, "%s: root %06X cmds %u vtx %u tris %u rects %u pages %u KB %u otherCIMG %u bad %u\n",
                argv[f], root, n_cmds, n_vtx, n_tris, n_rects, nreg, bytes / 1024, n_cimg_other, n_bad);
        if (f == 2) {
            fprintf(stderr, "  opcodes:");
            for (int i = 0; i < 256; i++) if (op_count[i]) fprintf(stderr, " %02X:%u", i, op_count[i]);
            fprintf(stderr, "\n");
        }
    }
    fclose(out);
    fprintf(stderr, "total %lu KB for %d frames\n", total / 1024, argc - 2);
    return 0;
}
