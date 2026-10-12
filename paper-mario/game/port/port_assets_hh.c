/* The asset region for the handheld build (M2): the ROM-derived data rebuilt from the player's ROM at first start.
 *
 * The shipped executable carries none of the game's ROM-derived data (textures, vertices and display lists the
 * decomp's split generates from the ROM, the font, the image-effect keyframes): they live in a NOLOAD region
 * (__assets_start..__assets_end, tools/port/gen_build.py's assets_*.ld) that is filled at start
 *   1. from assets.bin, a cache in the save dir written on the first run, or
 *   2. from the player's own Paper Mario (USA) ROM (rom.z64 next to the game, else on the SD card), by executing the
 *      recipe table recipes.bin that ships in the zip (numbers only: ROM offsets, byte-swap patterns, sizes --
 *      tools/assets/derive_recipes.py, emit_recipes.py), then cached as 1.
 *
 * From super-mario-64's port/src/hh/assets_handheld.c (after the MK64 port and beckerd's PSP asset split): the
 * same recipe format and cache, a Paper Mario ROM check, and the ROM finder exported (port_find_rom), because the
 * game streams from the ROM the whole time it runs (port_rom.c). No MIO0: the region is raw ROM data. The first-start
 * and error screens are SDL 1.2 and the OS's OpenSans font; the desktop proxy (PM64_ASSETS_LINKED) links the data in. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "port.h"
#include "port_assets.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define SAVESTATE_EXCLUDE /* no save states yet (the kit's savestate.h when they come) */

extern u8 __assets_start[], __assets_end[];

/* MIO0 (the N64 SDK's LZ scheme): header "MIO0", decompressed size, offsets of
 * the compressed (back-reference) and uncompressed (literal) streams; a layout
 * bitstream picks one of the two per output byte / run. Big-endian fields.
 * Returns 0 when a stream runs outside the block or a reference reaches before
 * the output's start (a bad dump): reported, never read out of bounds. */
static u32 be32(const u8* p) { return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]; }
static int mio0decode(const u8* in, u32 in_len, u8* out, u32 out_len) {
    const u8* const end = in + in_len;
    const u8 *layout, *comp, *raw;
    u32 size, n = 0, bits = 0;
    int nbits = 0;
    if (in_len < 16) return 0;
    size = be32(in + 4);
    if (size > out_len || be32(in + 8) > in_len || be32(in + 12) > in_len) return 0;
    layout = in + 16; comp = in + be32(in + 8); raw = in + be32(in + 12);
    while (n < size) {
        if (nbits == 0) {
            if (end - layout < 4) return 0;
            bits = be32(layout); layout += 4; nbits = 32;
        }
        if (bits & 0x80000000u) {
            if (raw >= end) return 0;
            out[n++] = *raw++;
        } else {
            u32 v, len, dist;
            if (end - comp < 2) return 0;
            v = (u32) comp[0] << 8 | comp[1];
            len = (v >> 12) + 3;
            dist = (v & 0xFFF) + 1;
            comp += 2;
            if (dist > n) return 0;
            while (len-- && n < size) { out[n] = out[n - dist]; n++; }
        }
        bits <<= 1; nbits--;
    }
    return 1;
}
extern const uint32_t port_build_stamp; /* build_stamp.c: this link's random id (tools/port/miyoo.sh) */

#define CACHE_NAME  "assets.bin"
#define RECIPE_NAME "recipes.bin"

/* ------------------------------------------------------------------ ROM */
static FILE* sRom SAVESTATE_EXCLUDE;
static int sRomMode; /* 0 = z64 (big-endian), 1 = v64 (byte-swapped pairs), 2 = n64 (little-endian words) */

static void fix_order(u8* b, u32 n) {
    u32 i;
    if (sRomMode == 1) {
        for (i = 0; i + 1 < n; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; }
    } else if (sRomMode == 2) {
        for (i = 0; i + 3 < n; i += 4) { u8 t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; }
    }
}

/* Read `len` bytes at ROM offset `off` (any alignment) in big-endian order. */
static int rom_read(u32 off, u8* dst, u32 len) {
    u32 a = off & ~3u, e = (off + len + 3) & ~3u;
    static u8 tmp[4096];
    u32 pos = a;
    while (pos < e) {
        u32 n = e - pos; if (n > sizeof(tmp)) n = sizeof(tmp);
        if (fseek(sRom, pos, SEEK_SET) != 0 || fread(tmp, 1, n, sRom) != n) return 0;
        fix_order(tmp, n);
        {
            u32 s = pos < off ? off - pos : 0;
            u32 c = pos + n > off + len ? off + len - (pos + s) : n - s;
            memcpy(dst + (pos + s - off), tmp + s, c);
        }
        pos += n;
    }
    return 1;
}

static void rom_close(void) { if (sRom) { fclose(sRom); sRom = NULL; } }

/* Open + validate: an N64 ROM in any byte order whose header says PAPER MARIO with the US game code (NMQE). The CRC
 * of the generated region is the real test. */
static int rom_open(const char* path, char* err, u32 errlen) {
    u8 hdr[0x40];
    rom_close();
    sRom = fopen(path, "rb");
    if (sRom == NULL) { snprintf(err, errlen, "cannot open"); return 0; }
    setvbuf(sRom, NULL, _IOFBF, 256 * 1024); /* SD card: fewer, larger reads */
    sRomMode = 0;
    if (fread(hdr, 1, 4, sRom) != 4) { snprintf(err, errlen, "unreadable"); rom_close(); return 0; }
    if (hdr[0] == 0x80 && hdr[1] == 0x37) sRomMode = 0;
    else if (hdr[0] == 0x37 && hdr[1] == 0x80) sRomMode = 1;
    else if (hdr[0] == 0x40 && hdr[1] == 0x12) sRomMode = 2;
    else { snprintf(err, errlen, "not an N64 ROM"); rom_close(); return 0; }
    if (!rom_read(0, hdr, sizeof(hdr))) { snprintf(err, errlen, "unreadable"); rom_close(); return 0; }
    if (memcmp(hdr + 0x20, "PAPER MARIO", 11) != 0) { snprintf(err, errlen, "not Paper Mario"); rom_close(); return 0; }
    if (memcmp(hdr + 0x3B, "NMQE", 4) != 0) { snprintf(err, errlen, "not the US (NTSC) version (code %.4s)", (const char*) hdr + 0x3B); rom_close(); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ transforms */
static const PortAssetPattern* sPatterns;
static const u16* sRuns;
static void xf_sw16(u8* b, u32 n) { u32 i; for (i = 0; i + 1 < n; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; } }
static void xf_sw32(u8* b, u32 n) { u32 i; for (i = 0; i + 3 < n; i += 4) { u8 t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; } }
static void xf_words(u8* q, u32 nw, u32 shape) {
    u8 t;
    for (; nw; nw--, q += 4) {
        switch (shape) {
            case PA_SHAPE_4:   t = q[0]; q[0] = q[3]; q[3] = t; t = q[1]; q[1] = q[2]; q[2] = t; break;
            case PA_SHAPE_22:  t = q[0]; q[0] = q[1]; q[1] = t; t = q[2]; q[2] = q[3]; q[3] = t; break;
            case PA_SHAPE_211: t = q[0]; q[0] = q[1]; q[1] = t; break;
            case PA_SHAPE_112: t = q[2]; q[2] = q[3]; q[3] = t; break;
            default: return; /* 1111: bytes stay */
        }
    }
}
/* n bytes (whole words) that start `phase` words into the pattern's cycle. */
static void xf_pattern(u8* b, u32 n, const PortAssetPattern* p, u32 phase) {
    const u16* run = sRuns + p->first;
    u32 r = 0, left, w = 0, nw = n / 4;
    phase %= p->nwords;
    while (phase >= (u32) (run[r] & 0x1FFF)) { phase -= run[r] & 0x1FFF; r++; }
    left = (run[r] & 0x1FFF) - phase;
    while (w < nw) {
        const u32 k = left < nw - w ? left : nw - w;
        xf_words(b + w * 4, k, run[r] >> 13);
        w += k; left -= k;
        if (left == 0) { if (++r == p->run_count) r = 0; left = run[r] & 0x1FFF; }
    }
}
static void apply_xform(u8* b, u32 n, u16 xf, u32 phase) {
    if (xf == PA_XF_SW16) xf_sw16(b, n);
    else if (xf == PA_XF_SW32) xf_sw32(b, n);
    else if (xf >= PA_XF_PATTERN) xf_pattern(b, n, &sPatterns[xf - PA_XF_PATTERN], phase);
}
/* The recipes were matched against whole transformed streams, so a symbol
 * whose size is not a multiple of the transform's word gets its last partial
 * word from the transformed neighbourhood: transform a rounded-up range in a
 * temp buffer and keep `size` bytes. `phase`: the pattern word it starts at. */
static u8* sXfTmp SAVESTATE_EXCLUDE;  /* sXfTmpSize bytes: the largest MIO0 block or 64 KB, whichever is bigger */
static u32 sXfTmpSize;
static int emit_xformed(u8* dst, const u8* src, u32 size, u16 xf, u32 phase) {
    u32 n = (size + 3) & ~3u;
    if (xf == PA_XF_ID) { memcpy(dst, src, size); return 1; }
    if (n > sXfTmpSize) return 0; /* a recipe larger than any source it can come from: corrupt blob */
    memcpy(sXfTmp, src, n);
    apply_xform(sXfTmp, n, xf, phase);
    memcpy(dst, sXfTmp, size);
    return 1;
}

/* ------------------------------------------------------------------ crc */
/* Standard CRC-32 (zlib's crc32(), incrementally from 0), slice-by-8: eight
 * table lookups per 8 bytes instead of a byte-serial dependency chain. Same
 * result (checked against the byte-wise loop on random data, lengths and
 * alignments); every launch and resume checks the whole
 * 10 MB cache (on MK64's 13 MB the byte loop cost ~70-80 ms on the device). */
static u32 crc_table[8][256] SAVESTATE_EXCLUDE; /* 8 KB: keep it out of the save states */
static int crc_ready SAVESTATE_EXCLUDE;
static void crc_init(void) {
    u32 i, j, k;
    if (crc_ready) return;
    crc_ready = 1;
    for (i = 0; i < 256; i++) { u32 c = i; for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[0][i] = c; }
    for (i = 0; i < 256; i++) for (k = 1; k < 8; k++) crc_table[k][i] = (crc_table[k - 1][i] >> 8) ^ crc_table[0][crc_table[k - 1][i] & 0xFF];
}
static u32 crc_update(u32 crc, const u8* b, u32 n) {
    crc = ~crc;
    for (; n && ((uintptr_t) b & 3); n--) crc = crc_table[0][(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    for (; n >= 8; n -= 8, b += 8) {
        u32 lo, hi;
        memcpy(&lo, b, 4); memcpy(&hi, b + 4, 4); /* little-endian (ARM, x86) */
        lo ^= crc;
        crc = crc_table[7][lo & 0xFF] ^ crc_table[6][(lo >> 8) & 0xFF] ^ crc_table[5][(lo >> 16) & 0xFF] ^ crc_table[4][lo >> 24] ^
              crc_table[3][hi & 0xFF] ^ crc_table[2][(hi >> 8) & 0xFF] ^ crc_table[1][(hi >> 16) & 0xFF] ^ crc_table[0][hi >> 24];
    }
    for (; n; n--) crc = crc_table[0][(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static u32 port_crc32(u32 crc, const void* b, u32 n) { crc_init(); return crc_update(crc, (const u8*) b, n); }

/* ------------------------------------------------------------------ recipe blob */
static const PortAssetBlobHeader* sH;
static const PortAssetRecipe* sRecipes;
static const u8* sLiterals;
static const PortAssetBlock* sBlocks;
static const PortAssetReloc* sRelocs;
static u32 sBlobStamp;   /* "STMP" trailer: the build stamp of the executable the table was emitted for */
static char sBlobPath[512];

static u32 read_le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

/* recipes.bin next to the executable (the game folder), else in the data dirs. */
static int blob_locate(const char* exe_dir) {
    const char* dirs[3]; int i, n = 0;
    if (exe_dir && exe_dir[0]) dirs[n++] = exe_dir;
    dirs[n++] = port_eboot_dir();
    dirs[n++] = port_save_dir();
    for (i = 0; i < n; i++) {
        FILE* f;
        snprintf(sBlobPath, sizeof(sBlobPath), "%s%s", dirs[i], RECIPE_NAME);
        f = fopen(sBlobPath, "rb");
        if (f) {
            u8 t[16]; long len;
            fseek(f, 0, SEEK_END); len = ftell(f);
            sBlobStamp = 0;
            /* trailer: "BLID" cache-id "STMP" build-stamp (16 bytes; a PSP-style table has only the first 8 or none) */
            if (len >= (long) sizeof(PortAssetBlobHeader) + 16 && fseek(f, len - 16, SEEK_SET) == 0 && fread(t, 1, 16, f) == 16
                && memcmp(t, "BLID", 4) == 0 && memcmp(t + 8, "STMP", 4) == 0) sBlobStamp = read_le32(t + 12);
            fclose(f);
            return 1;
        }
    }
    sBlobPath[0] = 0;
    return 0;
}

static u8* blob_load(u32* out_len, char* err, u32 errlen) {
    FILE* f; long len; u8* buf;
#if defined(MIYOO)
    if (!sBlobPath[0]) { snprintf(err, errlen, "recipes.bin is missing next to the game"); return NULL; }
#else
    if (!sBlobPath[0]) { snprintf(err, errlen, "recipes.bin is missing from the game folder"); return NULL; }
#endif
    f = fopen(sBlobPath, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", sBlobPath); return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < (long) sizeof(PortAssetBlobHeader) || len > 64 * 1024 * 1024) { snprintf(err, errlen, "recipes.bin has a bad size"); fclose(f); return NULL; }
    buf = malloc((size_t) len);
    if (!buf || fread(buf, 1, (size_t) len, f) != (size_t) len) { snprintf(err, errlen, "cannot read recipes.bin"); fclose(f); free(buf); return NULL; }
    fclose(f);
    if (memcmp(buf, "PM64RCP1", 8) != 0) { snprintf(err, errlen, "recipes.bin is not a recipe table (this build cannot extract from a ROM)"); free(buf); return NULL; }
    *out_len = (u32) len;
    return buf;
}

static int blob_parse(u8* buf, u32 len) {
    const u8* p = buf;
    uint64_t need;
    u32 i;
    sH = (const PortAssetBlobHeader*) p; p += sizeof(*sH);
    need = sizeof(*sH) + (uint64_t) sH->recipe_count * sizeof(PortAssetRecipe) + (((uint64_t) sH->literal_size + 3) & ~3u)
         + (uint64_t) sH->pattern_count * sizeof(PortAssetPattern) + (((uint64_t) sH->run_count * 2 + 3) & ~3u)
         + (uint64_t) sH->block_count * sizeof(PortAssetBlock) + (uint64_t) sH->reloc_count * sizeof(PortAssetReloc);
    if (need > len) return 0;
    if (sH->max_block > 64u << 20 || sH->region_size > 256u << 20) return 0;
    sRecipes = (const PortAssetRecipe*) p; p += sH->recipe_count * sizeof(PortAssetRecipe);
    sLiterals = p; p += (sH->literal_size + 3) & ~3u;
    sPatterns = (const PortAssetPattern*) p; p += sH->pattern_count * sizeof(PortAssetPattern);
    sRuns = (const u16*) p; p += (sH->run_count * 2 + 3) & ~3u;
    sBlocks = (const PortAssetBlock*) p; p += sH->block_count * sizeof(PortAssetBlock);
    sRelocs = (const PortAssetReloc*) p; p += sH->reloc_count * sizeof(PortAssetReloc);
    (void) p;
    /* every pattern index a recipe names must exist; every pattern's runs must
     * lie in the table, be valid shapes and add up to its length */
    for (i = 0; i < sH->recipe_count; i++)
        if (sRecipes[i].xform >= PA_XF_PATTERN && sRecipes[i].xform - PA_XF_PATTERN >= sH->pattern_count) return 0;
    for (i = 0; i < sH->pattern_count; i++) {
        const PortAssetPattern* pt = &sPatterns[i];
        u32 r, words = 0;
        if (pt->nwords == 0 || pt->run_count == 0 || pt->first > sH->run_count || pt->run_count > sH->run_count - pt->first) return 0;
        for (r = 0; r < pt->run_count; r++) {
            const u16 v = sRuns[pt->first + r];
            if ((v >> 13) > PA_SHAPE_1111 || (v & 0x1FFF) == 0) return 0;
            words += v & 0x1FFF;
        }
        if (words != pt->nwords) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ generate */
static u8* sBlockBuf SAVESTATE_EXCLUDE;  /* decompressed mio0 block */
static u32 sBlockRom = 0xFFFFFFFF;
static u8* sCompBuf SAVESTATE_EXCLUDE;   /* compressed block read from the ROM */

static const PortAssetBlock* find_block(u32 rom_off) {
    u32 i;
    for (i = 0; i < sH->block_count; i++) if (sBlocks[i].rom_off == rom_off) return &sBlocks[i];
    return NULL;
}

static int load_block(u32 rom_off, char* err, u32 errlen) {
    const PortAssetBlock* bk;
    if (sBlockRom == rom_off) return 1;
    bk = find_block(rom_off);
    if (bk == NULL) { snprintf(err, errlen, "unknown mio0 block %08X", (unsigned) rom_off); return 0; }
    if (bk->rom_len < 16 || bk->rom_len > sH->max_block || bk->decomp_len > sH->max_block) { snprintf(err, errlen, "bad mio0 block entry %08X", (unsigned) rom_off); return 0; }
    if (!rom_read(rom_off, sCompBuf, bk->rom_len)) { snprintf(err, errlen, "ROM read failed at %08X", (unsigned) rom_off); return 0; }
    if (memcmp(sCompBuf, "MIO0", 4) != 0) { snprintf(err, errlen, "no MIO0 block at %08X (wrong ROM?)", (unsigned) rom_off); return 0; }
    /* the header's decompressed size must be what the recipes were derived against */
    if (((u32) sCompBuf[4] << 24 | (u32) sCompBuf[5] << 16 | (u32) sCompBuf[6] << 8 | sCompBuf[7]) != bk->decomp_len) { snprintf(err, errlen, "mio0 block %08X has another size (wrong ROM?)", (unsigned) rom_off); return 0; }
    if (!mio0decode(sCompBuf, bk->rom_len, sBlockBuf, bk->decomp_len)) { snprintf(err, errlen, "mio0 block %08X is corrupt (a bad dump?)", (unsigned) rom_off); return 0; }
    /* a block's last symbol may be transformed as a word-rounded range that
     * runs up to 3 bytes past the block: the recipes were derived against a
     * zero-padded block, so the tail must be zero, not stale heap */
    memset(sBlockBuf + bk->decomp_len, 0, 16);
    sBlockRom = rom_off;
    return 1;
}

/* A crash inside the generator would otherwise be a silent exit on the
 * device: say which recipe was running, then die. Installed for the recipe
 * loop only, and the handlers it replaced (port_host.c's crash report, set in
 * main before the assets load) go back afterwards: it used to reset them to
 * the default, so after a first start a crash of the game left nothing in
 * console.log (and on the device SDL 1.2's parachute, which SDL_Init installs
 * wherever the handler is the default, took SIGSEGV to stderr only). */
static volatile u32 sCurRecipe = 0xFFFFFFFF;
static const int kCrashSigs[4] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };
static struct sigaction sCrashPrev[4];
static void crash_handler(int sig);
static void crash_handlers(int install) {
    int i;
    for (i = 0; i < 4; i++) {
        if (install) {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = crash_handler;
            sigemptyset(&sa.sa_mask);
            sigaction(kCrashSigs[i], &sa, &sCrashPrev[i]);
        } else {
            sigaction(kCrashSigs[i], &sCrashPrev[i], NULL);
        }
    }
}
static void crash_handler(int sig) {
    char buf[160];
    int n;
    if (sCurRecipe != 0xFFFFFFFF && sRecipes) {
        const PortAssetRecipe* r = &sRecipes[sCurRecipe];
        n = snprintf(buf, sizeof(buf), "assets: CRASH signal %d at recipe %u (kind %u xform %u dst %u size %u src %u extra %u)\n",
                     sig, (unsigned) sCurRecipe, (unsigned) r->kind, (unsigned) r->xform, (unsigned) r->dst, (unsigned) r->size, (unsigned) r->src, (unsigned) r->extra);
    } else {
        n = snprintf(buf, sizeof(buf), "assets: CRASH signal %d outside the recipe loop\n", sig);
    }
    if (n > 0) { (void) !write(2, buf, (size_t) n); { FILE* f = fopen(port_save_path("console.log"), "a"); if (f) { fputs(buf, f); fclose(f); } } }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void scratch_free(void) {
    free(sBlockBuf); free(sCompBuf); free(sXfTmp);
    sBlockBuf = sCompBuf = sXfTmp = NULL;
}

static int generate(const char* rom_path, void (*progress)(const char*, int), char* err, u32 errlen) {
    u32 i, region_size = (u32) (__assets_end - __assets_start), blk;
    u8* region = __assets_start;
    u32 t0 = port_time_us();

    if (region_size != sH->region_size || sBlobStamp != port_build_stamp) { snprintf(err, errlen, "recipes.bin is for another build of the game"); return 0; }
    if (!rom_open(rom_path, err, errlen)) return 0;
    blk = (sH->max_block + 63) & ~63u;
    sBlockBuf = malloc(blk + 16);                 /* + the zeroed tail, see load_block */
    sCompBuf = malloc(blk > 64 * 1024 ? blk : 64 * 1024); /* compressed <= decompressed; RAW recipes go through it in 64 KB pieces */
    sXfTmpSize = (blk > 64 * 1024 ? blk : 64 * 1024) + 16; /* MIO0 recipes are block slices, RAW is chunked */
    sXfTmp = malloc(sXfTmpSize);
    if (!sBlockBuf || !sCompBuf || !sXfTmp) { snprintf(err, errlen, "out of memory"); scratch_free(); rom_close(); return 0; }
    sBlockRom = 0xFFFFFFFF;
    memset(region, 0, region_size);
    crash_handlers(1);

    for (i = 0; i < sH->recipe_count; i++) {
        const PortAssetRecipe* r = &sRecipes[i];
        u8* dst = region + r->dst;
        sCurRecipe = i;
        if (r->size > region_size || r->dst > region_size - r->size) { snprintf(err, errlen, "recipe %u outside the region", (unsigned) i); goto fail; }
        switch (r->kind) {
            case PA_RAW:
                if (r->xform == PA_XF_ID) {
                    if (!rom_read(r->src, dst, r->size)) { snprintf(err, errlen, "ROM read failed at %08X", (unsigned) r->src); goto fail; }
                } else {
                    /* through the temp buffer in 64 KB pieces (whole words), each
                     * continuing the pattern where the last one stopped */
                    u32 done = 0;
                    while (done < r->size) {
                        u32 n = r->size - done; if (n > 64 * 1024) n = 64 * 1024;
                        if (!rom_read(r->src + done, sCompBuf, (n + 3) & ~3u)) { snprintf(err, errlen, "ROM read failed at %08X", (unsigned) r->src); goto fail; }
                        if (!emit_xformed(dst + done, sCompBuf, n, r->xform, done / 4)) { snprintf(err, errlen, "recipe %u too large", (unsigned) i); goto fail; }
                        done += n;
                    }
                }
                break;
            case PA_MIO0: {
                const PortAssetBlock* bk;
                if (!load_block(r->src, err, errlen)) goto fail;
                bk = find_block(r->src);
                if (r->extra > bk->decomp_len || r->size > bk->decomp_len - r->extra) { snprintf(err, errlen, "recipe %u outside its block", (unsigned) i); goto fail; }
                if (!emit_xformed(dst, sBlockBuf + r->extra, r->size, r->xform, 0)) { snprintf(err, errlen, "recipe %u too large", (unsigned) i); goto fail; }
                break;
            }
            case PA_LITERAL:
                if (r->src > sH->literal_size || r->size > sH->literal_size - r->src) { snprintf(err, errlen, "recipe %u outside the literals", (unsigned) i); goto fail; }
                memcpy(dst, sLiterals + r->src, r->size);
                break;
            case PA_RELOCS:
                break;
            default:
                snprintf(err, errlen, "recipe %u has an unknown kind %u", (unsigned) i, (unsigned) r->kind); goto fail;
        }
        if (progress) progress("Extracting", (int) (i * 70 / sH->recipe_count)); /* the screen draws when pct or stage changed */
    }

    rom_close();
    scratch_free();
    sCurRecipe = 0xFFFFFFFF;
    crash_handlers(0);

    /* Verify before the pointers go in: crc of the region with pointer words
     * zero (literal recipes carry their link-time pointer values: clear them). */
    if (progress) progress("Checking the data", 70);
    for (i = 0; i < sH->reloc_count; i++) {
        u32 o = sRelocs[i].off & 0x7FFFFFFFu;
        if (region_size < 4 || o > region_size - 4) { snprintf(err, errlen, "reloc %u outside the region", (unsigned) i); return 0; }
        *(u32*) (region + o) = 0;
    }
    {
        u32 crc = 0, at = 0;
        while (at < region_size) { /* 1 MB steps, the bar 70..85 */
            u32 n = region_size - at; if (n > 1024 * 1024) n = 1024 * 1024;
            crc = port_crc32(crc, region + at, n);
            at += n;
            if (progress) progress("Checking the data", 70 + (int) ((uint64_t) at * 15 / region_size));
        }
        if (crc != sH->data_crc) {
            snprintf(err, errlen, "the data built from this ROM does not match (crc %08X, expected %08X): a different dump?", (unsigned) crc, (unsigned) sH->data_crc);
            return 0;
        }
    }
    /* Absolute pointers: this ELF runs at its link address, so a module-
     * relative target (flag) is the final value and a region-relative one is
     * region + target. */
    for (i = 0; i < sH->reloc_count; i++) {
        const PortAssetReloc* rl = &sRelocs[i];
        u32 v = (rl->off & 0x80000000u) ? rl->target : (u32) (uintptr_t) region + rl->target;
        *(u32*) (region + (rl->off & 0x7FFFFFFFu)) = v;
        if (progress && (i & 4095) == 0) progress("Linking the data", 85 + (int) ((uint64_t) i * 5 / sH->reloc_count));
    }
    port_log_always("assets: generated from %s in %u ms\n", rom_path, (unsigned) ((port_time_us() - t0) / 1000));
    return 1;
fail:
    rom_close();
    scratch_free();
    sCurRecipe = 0xFFFFFFFF;
    crash_handlers(0);
    return 0;
}

/* ------------------------------------------------------------------ cache */
/* assets.bin (tools/psp/make_assets.py's format): header | reloc offsets |
 * (no symbol table when written here) | data at `data_off`. Pointer words
 * hold their link-time = runtime values (fixed-address link), so the data
 * is the region verbatim; data_crc covers it. build_id is recipes.bin's cache
 * id: it changes with any code change (pointer values), so a cache from
 * another build is regenerated, never loaded. */
struct AssetHeader {
    char magic[8];
    u32 version, region_addr, region_size, reloc_count, sym_count, names_size, build_id, data_crc, data_off;
};
/* 1 once assets.bin is known to hold this run's region (loaded with its crc checked, or written from it): the save
 * states' pristine pages come from it (port_state.c), and an old cache left after a failed write must not serve */
int port_assets_cache_current SAVESTATE_EXCLUDE;

static void screen_progress(const char* what, int pct);
static int write_cache(const char* path, char* err, u32 errlen) {
    u32 region_size = (u32) (__assets_end - __assets_start);
    struct AssetHeader h; u32 i, off = 0, hdrlen, crc = 0;
    FILE* f;
    char tmp[512];
    /* private temp + rename: a reader never sees a partial file, and several
     * instances extracting at once (the desktop sweep) all end with a whole one */
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int) getpid());
    f = fopen(tmp, "wb");
    if (f == NULL) { snprintf(err, errlen, "cannot create %s", tmp); return 0; }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "PM64ASST", 8);
    h.version = 1; h.region_addr = (u32) (uintptr_t) __assets_start; h.region_size = region_size;
    h.reloc_count = sH->reloc_count; h.build_id = port_build_stamp;
    hdrlen = (u32) sizeof(h) + sH->reloc_count * 4;
    h.data_off = (hdrlen + 4095) & ~4095u;
    fwrite(&h, 1, sizeof(h), f);
    for (i = 0; i < sH->reloc_count; i++) { u32 o = sRelocs[i].off & 0x7FFFFFFFu; fwrite(&o, 4, 1, f); }
    while (hdrlen < h.data_off) { fputc(0, f); hdrlen++; }
    while (off < region_size) {
        u32 n = region_size - off; if (n > 256 * 1024) n = 256 * 1024;
        crc = port_crc32(crc, __assets_start + off, n);
        if (fwrite(__assets_start + off, 1, n, f) != n) { snprintf(err, errlen, "write failed (SD card full?)"); fclose(f); remove(tmp); return 0; }
        off += n;
        screen_progress("Saving", 90 + (int) ((uint64_t) off * 6 / region_size));
    }
    /* data_crc last: a file that has it is complete */
    if (fseek(f, (long) ((char*) &h.data_crc - (char*) &h), SEEK_SET) != 0 || fwrite(&crc, 4, 1, f) != 1) {
        snprintf(err, errlen, "write failed (SD card full?)"); fclose(f); remove(tmp); return 0;
    }
    /* On the card before the rename, then the renamed file synced again (still open), then the folder. Without the
     * syncs the 11 MB, its FAT chain and its directory slot reached the card in whatever order writeback chose, up to
     * ~30 s later: a hard power cut in that window could leave the FAT inconsistent (lost clusters, or a slot pointing
     * at clusters the FAT does not hold). The next start's crc rebuilt the cache, not the FAT. The second fsync is the
     * vfat one (Linux 4.14 vfat_rename only marks the moved directory slot dirty; the folder fsync does not write it;
     * pm64.sav 20d93fc, diddy-kong-racing 9c91d40 for its cache). Once, at the first start, under the Saving screen. A
     * failure after the rename is only logged: the file is whole. */
    screen_progress("Finishing the copy", 97); /* the fsyncs and the rename: no megabyte, but time */
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { snprintf(err, errlen, "write failed (SD card full?)"); fclose(f); remove(tmp); return 0; }
    if (rename(tmp, path) != 0) { snprintf(err, errlen, "cannot rename %s", tmp); fclose(f); remove(tmp); return 0; }
    if (fsync(fileno(f)) != 0) port_log_always("assets: fsync after the rename: %s (the cache is written)\n", strerror(errno));
    fclose(f);
    screen_progress("Finishing the copy", 98);
    {
        const int d = open(port_save_dir(), O_RDONLY);
        if (d < 0 || fsync(d) != 0) port_log_always("assets: folder fsync %s: %s (the cache is written)\n", port_save_dir(), strerror(errno));
        if (d >= 0) close(d);
    }
    port_assets_cache_current = 1;
    return 1;
}

static int try_load(const char* path, char* err, u32 errlen) {
    struct AssetHeader h;
    u32 done, region_size = (u32) (__assets_end - __assets_start), crc = 0;
    FILE* f = fopen(path, "rb");
    if (f == NULL) { snprintf(err, errlen, "not found"); return 0; }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "PM64ASST", 8) != 0 || h.version != 1) {
        snprintf(err, errlen, "not a Paper Mario asset cache"); fclose(f); return 0;
    }
    if (h.region_addr != (u32) (uintptr_t) __assets_start || h.region_size != region_size) {
        snprintf(err, errlen, "from another version of the game (region %08X+%u, this build %08X+%u)", (unsigned) h.region_addr, (unsigned) h.region_size, (unsigned) (uintptr_t) __assets_start, (unsigned) region_size);
        fclose(f); return 0;
    }
    if (h.build_id != port_build_stamp) {
        snprintf(err, errlen, "from another version of the game (build %08X, this build %08X)", (unsigned) h.build_id, (unsigned) port_build_stamp);
        fclose(f); return 0;
    }
    if (h.data_crc == 0) { snprintf(err, errlen, "incomplete"); fclose(f); return 0; }
    if (h.data_off == 0) h.data_off = (sizeof(h) + h.reloc_count * 4 + h.sym_count * 16 + h.names_size + 15) & ~15u; /* legacy pad */
    if (fseek(f, h.data_off, SEEK_SET) != 0) { snprintf(err, errlen, "truncated"); fclose(f); return 0; }
    for (done = 0; done < region_size;) {
        u32 want = region_size - done;
        size_t got;
        if (want > 512 * 1024) want = 512 * 1024;
        got = fread(__assets_start + done, 1, want, f);
        if (got == 0) break;
        crc = port_crc32(crc, __assets_start + done, (u32) got);
        done += (u32) got;
    }
    fclose(f);
    if (done != region_size) { snprintf(err, errlen, "truncated (%u of %u bytes)", (unsigned) done, (unsigned) region_size); return 0; }
    if (crc != h.data_crc) { snprintf(err, errlen, "corrupt (crc %08X, expected %08X)", (unsigned) crc, (unsigned) h.data_crc); return 0; }
    port_log_always("assets: %s loaded (%u bytes, build %08X)\n", path, (unsigned) region_size, (unsigned) h.build_id);
    port_assets_cache_current = 1;
    return 1;
}

/* ------------------------------------------------------------------ ROM search */
/* The dump's byte order is auto-detected from its header (z64/v64/n64), so the
 * extension is only a filter to avoid header-probing every file on the card --
 * the header check in rom_open is the real gate. Accept the byte-order
 * extensions and the generic cart-image ones every N64 tool uses. */
static int rom_ext_ok(const char* n) {
    size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') return 0;
    e[0] = (char) (n[l - 3] | 0x20); e[1] = (char) (n[l - 2] | 0x20); e[2] = (char) (n[l - 1] | 0x20); e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") ||   /* the three byte orders */
           !strcmp(e, "rom") || !strcmp(e, "bin") || !strcmp(e, "u64") ||   /* generic cart images */
           !strcmp(e, "usa") || !strcmp(e, "u1");                           /* region-suffixed dumps */
}

/* rom.z64 next to the executable once port_pkg_rom has refused it, and why: a start asks up to three times (main, its
 * port_find_rom, the region's build), and the executable's folder is also the first one scan_dir reads */
static char sPkgRefused[512], sPkgWhy[128];

/* The first file in `dir` (case-insensitive extension) that opens as a US
 * Paper Mario ROM. The first valid one wins; `why` gets the last rejection. */
static int scan_dir(const char* dir, char* out, u32 outlen, char* why, u32 whylen) {
    DIR* d = opendir(dir);
    struct dirent* ent;
    int found = 0;
    if (d == NULL) return 0;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        char path[512];
        char err[96];
        if (n[0] == '.') continue; /* hidden, "._foo.z64" resource forks */
        if (!rom_ext_ok(n)) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", n);
        if (strcmp(path, sPkgRefused) == 0) continue; /* said once already */
        if (rom_open(path, err, sizeof(err))) {
            rom_close();
            snprintf(out, outlen, "%s", path);
            found = 1;
            break;
        }
        /* Only a file that is an N64 ROM (or can't be read) says why it was passed over. The rest are no ROMs at all
         * -- the game's own recipes.bin, the cache assets.bin, other systems' .bin BIOS files -- and recipes.bin, scanned
         * first, made every card with no ROM say "No usable Paper Mario ROM. It must be the US (NTSC) cart." instead of
         * "No Paper Mario ROM found." */
        if (strcmp(err, "not an N64 ROM") != 0) snprintf(why, whylen, "%s: %s", n, err);
        port_log_always("assets: skipping %s: %s\n", path, err);
    }
    closedir(d);
    return found;
}

/* `root`, then every folder under it down to `depth` levels. */
static int scan_tree(const char* root, int depth, char* out, u32 outlen, char* why, u32 whylen) {
    DIR* d;
    struct dirent* ent;
    if (scan_dir(root, out, outlen, why, whylen)) return 1;
    if (depth <= 0) return 0;
    d = opendir(root);
    if (d == NULL) return 0;
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;
        size_t rl = strlen(root);
        if (ent->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s%s%s/", root, (rl && root[rl - 1] == '/') ? "" : "/", ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (scan_tree(sub, depth - 1, out, outlen, why, whylen)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}

/* rom.z64 next to the executable (App/PaperMario/rom.z64: the auto-ROM a personal zip carries, the whole dump in .z64
 * order). The launcher may exec the bare name, so the file is found from /proc/self/exe's folder, not argv[0]. It is only
 * read, as a card ROM is: the same header check here (rom_open), port_rom_open's when the game opens it to stream,
 * generate()'s data CRC at a first start; the cache still goes to the save dir. Not there (a public zip,
 * the desktop build dir): 0 without a word, and the search is the one before. Refused: logged once, as scan_dir logs
 * a card file, then the card search. Exported: main tries it before rom_path.txt, which never records it
 * (port_host.c). */
#define PKG_ROM_NAME "rom.z64"
int port_pkg_rom(char* out, u32 outlen, char* why, u32 whylen) {
    char path[512], err[96];
    struct stat st;
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    char* s;
    if (n <= 0 || n >= (ssize_t) sizeof(path) - 1) return 0; /* none, or cut short */
    path[n] = 0;
    s = strrchr(path, '/');
    if (s == NULL || (size_t) (s + 1 - path) + sizeof(PKG_ROM_NAME) > sizeof(path)) return 0;
    memcpy(s + 1, PKG_ROM_NAME, sizeof(PKG_ROM_NAME));
    if (stat(path, &st) != 0) return 0;
    if (strcmp(path, sPkgRefused) != 0) {
        if (rom_open(path, err, sizeof(err))) {
            rom_close();
            snprintf(out, outlen, "%s", path);
            port_log_always("rom: %s (next to the game)\n", path); /* App/PaperMario/rom.z64: the auto-ROM */
            return 1;
        }
        port_log_always("assets: skipping %s: %s\n", path, err);
        snprintf(sPkgRefused, sizeof(sPkgRefused), "%s", path);
        sPkgWhy[0] = 0;
        if (strcmp(err, "not an N64 ROM") != 0) snprintf(sPkgWhy, sizeof(sPkgWhy), "%s: %s", PKG_ROM_NAME, err); /* as scan_dir */
    }
    if (sPkgWhy[0]) snprintf(why, whylen, "%s", sPkgWhy);
    return 0;
}

/* Where players may drop the ROM: next to the executable (rom.z64), the game's own data dir, anywhere on the card (root, a
 * folder, or a folder in a folder), anywhere in HOME, or an explicit PM64_ROM=<path> (which wins). Exported: the game
 * streams from the ROM (port_host.c opens it). */
int port_find_rom(char* out, u32 outlen, char* why, u32 whylen) {
    const char* env = getenv("PM64_ROM");
    why[0] = 0;
    if (env && env[0]) {
        char err[96];
        if (rom_open(env, err, sizeof(err))) { rom_close(); snprintf(out, outlen, "%s", env); return 1; }
        snprintf(why, whylen, "PM64_ROM: %s", err);
        return 0;
    }
    if (port_pkg_rom(out, outlen, why, whylen)) return 1;
    if (scan_dir(port_eboot_dir(), out, outlen, why, whylen)) return 1;
    if (scan_dir(port_save_dir(), out, outlen, why, whylen)) return 1;
#if defined(MIYOO)
    for (int i = 0; miyoo_rom_roots[i]; i++) /* Roms/N64, then the whole card, two folders deep each */
        if (scan_tree(miyoo_rom_roots[i], 2, out, outlen, why, whylen)) return 1;
#else
    if (scan_tree(".", 2, out, outlen, why, whylen)) return 1;
#endif
    return 0;
}

/* assets.bin.<pid>.tmp left by a run killed mid-write (~10 MB each) */
static void clean_stale_tmp(void) {
    DIR* d = opendir(port_save_dir());
    struct dirent* ent;
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        size_t l = strlen(ent->d_name);
        if (strncmp(ent->d_name, CACHE_NAME ".", sizeof(CACHE_NAME)) == 0 && l > 4 && strcmp(ent->d_name + l - 4, ".tmp") == 0)
            remove(port_save_path(ent->d_name));
    }
    closedir(d);
}

/* ------------------------------------------------------------------ UI */
#if defined(MIYOO)
/* The Miyoo Mini: the same screens at the game's 320x240 in the menu's 8x8 font (miyoo_text: no SDL_ttf, no OS fonts),
 * doubled and turned onto the panel (miyoo_panel.c), as MK64's and OoT's first-start screens on the Mini. */
#include <SDL/SDL.h>
static SDL_Surface* sScr SAVESTATE_EXCLUDE; /* 320x240, 32 bpp, in memory */
static char sSub[64] = "Looking on the SD card";

static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "assets: SDL_Init: %s\n", SDL_GetError()); return; }
    if (!miyoo_panel_open()) return;
    SDL_ShowCursor(SDL_DISABLE);
    sScr = SDL_CreateRGBSurface(SDL_SWSURFACE, 320, 240, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}
/* centred; k = the font's scale; a line wider than the screen (a long ROM file name) keeps its start and ends in "..." */
static void ui_text(int y, const char* s, int k, int gray) {
    char fit[96];
    if (!sScr || !s || !s[0]) return;
    snprintf(fit, sizeof(fit), "%s", s);
    const int maxc = (320 - 8) / (8 * k);
    if ((int) strlen(fit) > maxc && maxc > 3) snprintf(fit + maxc - 3, sizeof(fit) - (size_t) (maxc - 3), "...");
    miyoo_text(sScr, (320 - miyoo_text_w(fit, k)) / 2, y, k, fit, 0xFF000000u | (uint32_t) gray * 0x010101u);
}
static void ui_show(void) {
    if (SDL_MUSTLOCK(sScr)) SDL_LockSurface(sScr);
    miyoo_panel_present32((uint32_t*) sScr->pixels, 320, 240); /* pitch 1280: a plain 320x240 frame */
    if (SDL_MUSTLOCK(sScr)) SDL_UnlockSurface(sScr);
    miyoo_panel_flip();
}
/* title, up to three body lines, a progress bar (pct < 0: none), a footer: the handheld's layout on the 4:3 screen */
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, 0);
    ui_text(22, "PAPER MARIO", 2, 255);
    ui_text(50, sSub, 1, 150);
    ui_text(86, l1, 1, 230);
    ui_text(104, l2, 1, 230);
    ui_text(122, l3, 1, 230);
    if (pct >= 0) {
        SDL_Rect frame = { 70, 158, 180, 12 }, fill = { 72, 160, (Uint16) (176 * pct / 100), 8 };
        char p[8];
        SDL_FillRect(sScr, &frame, SDL_MapRGB(sScr->format, 70, 70, 70));
        SDL_FillRect(sScr, &fill, SDL_MapRGB(sScr->format, 255, 255, 255));
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(178, p, 1, 200);
    }
    ui_text(212, foot, 1, 150);
    { /* PM64_SPLASH_DUMP=<dir>: the screen as a PPM (debug only) */
        const char* dd = getenv("PM64_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256]; FILE* f; int x, y;
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n320 240\n255\n");
                for (y = 0; y < 240; y++) for (x = 0; x < 320; x++) {
                    const Uint32 p = ((const Uint32*) ((const Uint8*) sScr->pixels + y * sScr->pitch))[x];
                    const Uint8 px[3] = { (Uint8) (p >> 16), (Uint8) (p >> 8), (Uint8) p };
                    fwrite(px, 1, 3, f);
                }
                fclose(f);
            }
        }
    }
    ui_show();
}
static void ui_wait_button(void) {
    SDL_Event ev;
    if (!sScr || getenv("PM64_ASSETS_ONLY")) return; /* headless asset run: never block */
    while (SDL_PollEvent(&ev)) { } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, 0);
    ui_show();
    ui_show(); /* a double buffer's other page too */
}
/* done with the screen: SDL stays up for the game's panel (quit == exiting) */
static void ui_end(int quit) {
    if (sScr) { SDL_FreeSurface(sScr); sScr = NULL; }
    if (quit) SDL_Quit();
}
#else
static char sSub[64] = "Looking on the SD card";
static void ui_begin(void) { }
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (pct >= 0) printf("assets: %s %s %s %d%%\n", l1, l2 ? l2 : "", l3 ? l3 : "", pct);
    else printf("assets: %s | %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", foot ? foot : "");
}
static void ui_wait_button(void) { }
static void ui_black(void) { }
static void ui_end(int quit) { (void) quit; }
#endif

static const char* sStage = "Reading the ROM";
static int sLastPct = -1, sShown;
static const char* sLastStage;
static char sLastSub[64];
static void screen_progress(const char* what, int pct) {
    if (what) sStage = what;
    if (pct == sLastPct && sStage == sLastStage && strcmp(sSub, sLastSub) == 0) return;
    sLastPct = pct;
    sLastStage = sStage;
    snprintf(sLastSub, sizeof(sLastSub), "%s", sSub);
    sShown = 1;
    ui_draw("First start: building the game", "data from your ROM.", sStage, pct, "This happens once.");
}

static void fail_screen(const char* l1, const char* l2, const char* l3) {
    ui_begin();
    if (!sShown || strcmp(sSub, "Looking on the SD card") == 0) sSub[0] = 0; /* an error screen has no name line unless the ROM's */
    port_log_always("assets: FAILED: %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    fprintf(stderr, "assets: %s %s %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}

/* PM64_ASSET_DIFF=<file> (debug): which bytes of the region the game writes
 * while it runs. A copy is taken once the region is filled; at exit the
 * changed ranges (region offsets) go to <file>, named by
 * tools/assets/asset_writes.py. Such data is game state: see savestate.c. */
static u8* sPristine SAVESTATE_EXCLUDE;
static void diff_report(void) {
    const char* path = getenv("PM64_ASSET_DIFF");
    const u32 n = (u32) (__assets_end - __assets_start);
    u32 i = 0;
    FILE* f;
    if (sPristine == NULL || path == NULL || (f = fopen(path, "w")) == NULL) return;
    fprintf(f, "region %08X %u\n", (unsigned) (uintptr_t) __assets_start, (unsigned) n);
    while (i < n) {
        u32 s;
        if (__assets_start[i] == sPristine[i]) { i++; continue; }
        s = i;
        while (i < n && __assets_start[i] != sPristine[i]) i++;
        fprintf(f, "%u %u\n", (unsigned) s, (unsigned) (i - s));
    }
    fclose(f);
}
static void diff_begin(void) {
    const u32 n = (u32) (__assets_end - __assets_start);
    if (getenv("PM64_ASSET_DIFF") == NULL || n == 0 || (sPristine = malloc(n)) == NULL) return;
    memcpy(sPristine, __assets_start, n);
    atexit(diff_report);
}

/* No usable ROM: the screen that says so, then exit. Also from port_host.c when the cache is there but the ROM is not
 * (moved or deleted after the first start, which says "This happens once"): the game reads the ROM the whole time it
 * runs, and it used to quit straight back to the launcher with nothing on the screen. why: the last file rejected. */
/* The screen's sentence naming the version a refused Paper Mario dump is (rom_open's "(code NMQx)": the header's
 * country code, E US, P Europe, J Japan), as OoT, DKR and GoldenEye say it; NULL for any other refusal. */
static const char* version_say(const char* why) {
    const char* c = why ? strstr(why, "(code NMQ") : NULL;
    if (c == NULL) return NULL;
    return c[9] == 'P' ? "It is the European (PAL) version." : c[9] == 'J' ? "It is the Japanese version." : "It is not the US version.";
}
void port_assets_no_rom(const char* why) {
    if (why && why[0]) {
        const char* say = version_say(why);
        port_log_always("assets: rejected: %s\n", why);
        if (say) fail_screen("No usable Paper Mario ROM.", say, "It must be the US (NTSC) cart.");
        fail_screen("No usable Paper Mario ROM.", "It must be the US (NTSC) cart.", NULL);
    }
    fail_screen("No Paper Mario ROM found.", "Copy your US (NTSC) .z64/.n64/.v64", "to the SD card and start again.");
}

static void assets_load(const char* argv0);
/* argv0: the executable's path, used to find recipes.bin next to it. */
void port_assets_load(const char* argv0) {
    assets_load(argv0);
    diff_begin();
}
/* The ROM the game has already found and checked (port_host.c): the region is built from it without a second scan
 * of the card (the first device log scanned twice, round 3). */
static char sRomHint[512];
void port_assets_rom_hint(const char* path) { snprintf(sRomHint, sizeof(sRomHint), "%s", path ? path : ""); }
static void assets_load(const char* argv0) {
    char exe_dir[512] = "", rom[512], err[192], why[160];
    u8* blob; u32 blob_len;
#if defined(PM64_ASSETS_LINKED)
    return; /* the desktop proxy: the data is linked in (gen_build.py, pc) */
#endif
    if (argv0 != NULL) {
        const char* slash = strrchr(argv0, '/');
        if (slash != NULL && (size_t) (slash + 1 - argv0) < sizeof(exe_dir)) {
            memcpy(exe_dir, argv0, slash + 1 - argv0);
            exe_dir[slash + 1 - argv0] = 0;
        }
    }
    if (!exe_dir[0]) { /* a launcher that execs the bare name */
        char self[512]; const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            char* s = strrchr(self, '/');
            if (s != NULL) { s[1] = 0; snprintf(exe_dir, sizeof(exe_dir), "%s", self); }
        }
    }
    if (&__assets_end[0] == &__assets_start[0]) return; /* a build with the assets linked in */
    blob_locate(exe_dir);
    port_log_always("assets: region %08X+%u, build %08X, recipes %s (stamp %08X)\n", (unsigned) (uintptr_t) __assets_start, (unsigned) (__assets_end - __assets_start),
                    (unsigned) port_build_stamp, sBlobPath[0] ? sBlobPath : "(none)", (unsigned) sBlobStamp);
    clean_stale_tmp();

    /* 1. the cache from an earlier run */
    /* <save dir>/rebuild (the player's, any content): do the first start's job again. assets.bin is not read; it is
     * replaced (tmp + rename) only by a copy made from a ROM that checks out, so with no ROM it stays, and so does
     * the file, which port_assets_done removes once the new copy is on the card. */
    if (access(port_save_path("rebuild"), F_OK) == 0) {
        port_log_always("assets: %s found, the first start's job runs again\n", port_save_path("rebuild"));
        snprintf(err, sizeof(err), "rebuild requested");
    } else if (try_load(port_save_path(CACHE_NAME), err, sizeof(err))) {
        return;
    }
    port_log_always("assets: %s%s: %s\n", port_save_dir(), CACHE_NAME, err);

    /* 2. the player's ROM: the one the game found, or a scan */
    ui_begin();
    if (sRomHint[0] && rom_open(sRomHint, err, sizeof(err))) { /* one file checked, not the card scanned */
        rom_close();
        snprintf(rom, sizeof(rom), "%s", sRomHint);
    } else if (screen_progress("Looking for your ROM", 0), !port_find_rom(rom, sizeof(rom), why, sizeof(why))) {
        port_assets_no_rom(why);
        return;
    }
    {
        const char* base = strrchr(rom, '/'); base = base ? base + 1 : rom;
        snprintf(sSub, sizeof(sSub), "%.60s", base);
    }
    screen_progress("Reading the ROM", 0);
    blob = blob_load(&blob_len, err, sizeof(err));
    if (!blob) { fail_screen("Cannot extract the game data:", err, NULL); return; }
    if (!blob_parse(blob, blob_len)) { fail_screen("Cannot extract the game data:", "recipes.bin is corrupt.", NULL); return; }
    if (!generate(rom, screen_progress, err, sizeof(err))) {
        port_log_always("assets: %s\n", err);
        fail_screen("This isn't the expected ROM data.", "It must be the US (NTSC) cart.", NULL);
        return;
    }
    screen_progress("Saving", 90);
    {
        const u32 tw = port_time_us();
        if (!write_cache(port_save_path(CACHE_NAME), err, sizeof(err))) {
            port_log_always("assets: cache not written: %s\n", err); /* the game still runs; next start extracts again */
        } else {
            port_log_always("assets: cache written to %s (%u ms with the syncs)\n", port_save_path(CACHE_NAME), (unsigned) ((port_time_us() - tw) / 1000));
        }
    }
    free(blob);
    sH = NULL; sRecipes = NULL; sLiterals = NULL; sPatterns = NULL; sRuns = NULL; sBlocks = NULL; sRelocs = NULL;
    screen_progress("Starting the game", 99); /* port_state_init follows; port_assets_done ends the screen */
}

/* The last of the first start's work is done (port_host.c, after port_state_init): "Done" at 100%, a beat to
 * read it, one black frame on both pages, the fonts down; SDL stays up for the game's video mode. Nothing on a
 * start that showed no screen. */
void port_assets_done(void) {
    if (sShown && access(port_save_path("rebuild"), F_OK) == 0 && port_assets_cache_current) { /* the rebuild skipped the cache load: 1 means the new one is written */
        if (remove(port_save_path("rebuild")) == 0) port_log_always("assets: rebuild done, %s removed\n", port_save_path("rebuild"));
        else port_log_always("assets: rebuild done, but %s can't be removed: the job runs again next start\n", port_save_path("rebuild"));
    }
    if (sShown) {
        screen_progress("Done", 100);
        if (getenv("PM64_SPLASH_DUMP") == NULL && getenv("PM64_FRAMES") == NULL) usleep(600 * 1000);
        ui_black();
        sShown = 0;
    }
    ui_end(0);
}
