/* Asset region setup for the handheld / desktop-proxy build (M6).
 *
 * The shipped executable carries none of the game's data: the ROM-derived
 * objects live in a NOLOAD region (__assets_start..__assets_end, see
 * Makefile.handheld "Asset split") that is filled at boot
 *   1. from assets.bin, a cache in the save dir written on the first run, or
 *   2. from the player's own Mario Kart 64 (USA) ROM (rom.z64 inside the
 *      zip, else on the SD card), by
 *      executing the recipe table recipes.bin that ships in the zip
 *      (numbers only: ROM offsets, byte-swap patterns, sizes -- see
 *      tools/psp/derive_recipes.py / emit_recipes.py), then cached as 1.
 *
 * Ported from src/port/psp/assets_gen.c + assets_load.c. Differences: the
 * ELF links at its runtime address (no PRX base to add: the reloc entries
 * are only used to zero the pointer words for the CRC and to rebuild them);
 * scratch is malloc'd; the first-start / error screens are SDL 1.2 + the
 * OS's OpenSans font (the desktop proxy prints to stdout instead). */
#include <PR/ultratypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "port.h"
#include "port_assets.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: nothing here is game state */

extern u8 __assets_start[], __assets_end[];
extern void mio0decode(u8* in, u8* out);
extern void displaylist_unpack(uintptr_t* data, uintptr_t finalDisplaylistOffset, u32 arg2);
extern uintptr_t gHeapEndPtr;
extern u32 port_time_us(void);
extern const uint32_t port_build_stamp; /* build/<plat>/build_stamp.c: this link's random id */

#define CACHE_NAME  "assets.bin"
#define RECIPE_NAME "recipes.bin"
/* ROMs are normally kept in a folder on the card. */
#if defined(MIYOO)
/* Miyoo Mini: the card is /mnt/SDCARD; Onion and the stock firmware keep N64 ROMs in Roms/N64. */
#define SD_CARD     "/mnt/SDCARD/"
#define SD_HOME     "/mnt/SDCARD/Roms/N64/"
#else
#define SD_CARD     "/mnt/"
#define SD_HOME     "/mnt/"
#endif

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

/* A Mario Kart 64 dump was seen (its header's title) but refused: the no-ROM screen then says
 * "No usable" (the wrong cart), not "not found". Other files the search opens -- the game's own
 * eeprom.bin, other systems' .bin ROMs, other N64 games -- don't count (B9; SF64 4e9483c, SM64). */
static int sMk64Seen SAVESTATE_EXCLUDE;
/* The version of a refused dump by its header's country code (E USA, J Japan, P Europe), for the
 * screen, as OoT, DKR and GoldenEye name it; NULL: none seen. */
static const char* sMk64Ver SAVESTATE_EXCLUDE;

/* Open + validate: an N64 ROM in any byte order whose header says
 * MARIOKART64, US region. The CRC of the generated region is the real test. */
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
    if (memcmp(hdr + 0x20, "MARIOKART64", 11) != 0) { snprintf(err, errlen, "not Mario Kart 64"); rom_close(); return 0; }
    sMk64Seen = 1;
    if (hdr[0x3E] != 'E') {
        sMk64Ver = hdr[0x3E] == 'P' ? "It is the European (PAL) version." : hdr[0x3E] == 'J' ? "It is the Japanese version." : "It is not the US version.";
        snprintf(err, errlen, "not the US (NTSC) version (region '%c')", hdr[0x3E]); rom_close(); return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ transforms */
static const PortAssetPattern* sPatterns;
static void xf_sw16(u8* b, u32 n) { u32 i; for (i = 0; i + 1 < n; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; } }
static void xf_sw32(u8* b, u32 n) { u32 i; for (i = 0; i + 3 < n; i += 4) { u8 t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; } }
static void xf_pattern(u8* b, u32 n, const PortAssetPattern* p) {
    u32 w, nw = n / 4;
    for (w = 0; w < nw; w++) {
        u8* q = b + w * 4; u8 t;
        switch (p->shapes[w % p->nwords]) {
            case PA_SHAPE_4:    t = q[0]; q[0] = q[3]; q[3] = t; t = q[1]; q[1] = q[2]; q[2] = t; break;
            case PA_SHAPE_22:   t = q[0]; q[0] = q[1]; q[1] = t; t = q[2]; q[2] = q[3]; q[3] = t; break;
            case PA_SHAPE_211:  t = q[0]; q[0] = q[1]; q[1] = t; break;
            case PA_SHAPE_112:  t = q[2]; q[2] = q[3]; q[3] = t; break;
            default: break;
        }
    }
}
static void apply_xform(u8* b, u32 n, u16 xf) {
    if (xf == PA_XF_SW16) xf_sw16(b, n);
    else if (xf == PA_XF_SW32) xf_sw32(b, n);
    else if (xf >= PA_XF_PATTERN) xf_pattern(b, n, &sPatterns[xf - PA_XF_PATTERN]);
}
/* The recipes were matched against whole transformed streams, so a symbol
 * whose size is not a multiple of the transform's word gets its last partial
 * word from the transformed neighbourhood: transform a rounded-up range in a
 * temp buffer and keep `size` bytes. */
static u8* sXfTmp SAVESTATE_EXCLUDE;  /* sXfTmpSize bytes: the largest MIO0 block or 64 KB, whichever is bigger */
static u32 sXfTmpSize;
static int emit_xformed(u8* dst, const u8* src, u32 size, u16 xf) {
    u32 n = (size + 3) & ~3u;
    if (xf == PA_XF_ID) { memcpy(dst, src, size); return 1; }
    if (n > sXfTmpSize) return 0; /* a recipe larger than any source it can come from: corrupt blob */
    memcpy(sXfTmp, src, n);
    apply_xform(sXfTmp, n, xf);
    memcpy(dst, sXfTmp, size);
    return 1;
}

/* ------------------------------------------------------------------ crc */
/* Standard CRC-32 (zlib's crc32(), incrementally from 0), slice-by-8: eight
 * table lookups per 8 bytes instead of a byte-serial dependency chain. Same
 * result (checked against the byte-wise loop on random data, lengths and
 * alignments); every launch and resume checks the whole
 * 13 MB cache, where the byte loop cost ~70-80 ms on the device. */
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
static const PortAssetCourseDl* sCourses;
static const PortAssetBlock* sBlocks;
static const PortAssetReloc* sRelocs;
static u32 sBlobStamp;   /* "STMP" trailer: the build stamp of the executable the table was emitted for */
static char sBlobPath[512];

static u32 read_le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

/* recipes.bin next to the executable (the game's folder), else in the data dirs. */
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
    if (!sBlobPath[0]) { snprintf(err, errlen, "recipes.bin is missing from the game's folder"); return NULL; }
    f = fopen(sBlobPath, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", sBlobPath); return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < (long) sizeof(PortAssetBlobHeader) || len > 64 * 1024 * 1024) { snprintf(err, errlen, "recipes.bin has a bad size"); fclose(f); return NULL; }
    buf = malloc((size_t) len);
    if (!buf || fread(buf, 1, (size_t) len, f) != (size_t) len) { snprintf(err, errlen, "cannot read recipes.bin"); fclose(f); free(buf); return NULL; }
    fclose(f);
    if (memcmp(buf, "MK64RCP1", 8) != 0) { snprintf(err, errlen, "recipes.bin is not a recipe table (this build cannot extract from a ROM)"); free(buf); return NULL; }
    *out_len = (u32) len;
    return buf;
}

static int blob_parse(u8* buf, u32 len) {
    const u8* p = buf;
    uint64_t need;
    sH = (const PortAssetBlobHeader*) p; p += sizeof(*sH);
    need = sizeof(*sH) + (uint64_t) sH->recipe_count * sizeof(PortAssetRecipe) + (((uint64_t) sH->literal_size + 3) & ~3u)
         + (uint64_t) sH->pattern_count * sizeof(PortAssetPattern) + (uint64_t) sH->course_count * sizeof(PortAssetCourseDl)
         + (uint64_t) sH->block_count * sizeof(PortAssetBlock) + (uint64_t) sH->reloc_count * sizeof(PortAssetReloc);
    if (need > len) return 0;
    if (sH->max_block > 64u << 20 || sH->max_unpacked > 64u << 20 || sH->region_size > 256u << 20) return 0;
    sRecipes = (const PortAssetRecipe*) p; p += sH->recipe_count * sizeof(PortAssetRecipe);
    sLiterals = p; p += (sH->literal_size + 3) & ~3u;
    sPatterns = (const PortAssetPattern*) p; p += sH->pattern_count * sizeof(PortAssetPattern);
    sCourses = (const PortAssetCourseDl*) p; p += sH->course_count * sizeof(PortAssetCourseDl);
    sBlocks = (const PortAssetBlock*) p; p += sH->block_count * sizeof(PortAssetBlock);
    sRelocs = (const PortAssetReloc*) p; p += sH->reloc_count * sizeof(PortAssetReloc);
    (void) p;
    {   /* every pattern index a recipe names must exist */
        u32 i;
        for (i = 0; i < sH->recipe_count; i++)
            if (sRecipes[i].xform >= PA_XF_PATTERN && sRecipes[i].xform - PA_XF_PATTERN >= sH->pattern_count) return 0;
        for (i = 0; i < sH->pattern_count; i++)
            if (sPatterns[i].nwords == 0 || sPatterns[i].nwords > 16) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ generate */
static u8* sBlockBuf SAVESTATE_EXCLUDE;  /* decompressed mio0 block */
static u32 sBlockRom = 0xFFFFFFFF;
static u8* sCompBuf SAVESTATE_EXCLUDE;   /* compressed block read from the ROM */
static u8* sUnpackBuf SAVESTATE_EXCLUDE; /* displaylist_unpack output */

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
    mio0decode(sCompBuf, sBlockBuf);
    /* a block's last symbol may be transformed as a word-rounded range that
     * runs up to 3 bytes past the block: the recipes were derived against a
     * zero-padded block, so the tail must be zero, not stale heap */
    memset(sBlockBuf + bk->decomp_len, 0, 16);
    sBlockRom = rom_off;
    return 1;
}

/* A crash inside the generator would otherwise be a silent exit on the
 * device: say which recipe was running, then die. */
static volatile u32 sCurRecipe = 0xFFFFFFFF;
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
    if (n > 0) { (void) !write(2, buf, (size_t) n); { FILE* f = fopen(port_save_path("log.txt"), "a"); if (f) { fputs(buf, f); fclose(f); } } }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void scratch_free(void) {
    free(sBlockBuf); free(sCompBuf); free(sUnpackBuf); free(sXfTmp);
    sBlockBuf = sCompBuf = sUnpackBuf = sXfTmp = NULL;
}

static int generate(const char* rom_path, void (*progress)(const char*, int), char* err, u32 errlen) {
    u32 i, region_size = (u32) (__assets_end - __assets_start), blk;
    u8* region = __assets_start;
    uintptr_t saved_heap_end = gHeapEndPtr; /* displaylist_unpack allocates below it: point it at scratch, restore on every exit */
    u32 t0 = port_time_us();

    if (region_size != sH->region_size || sBlobStamp != port_build_stamp) { snprintf(err, errlen, "recipes.bin is for another build of the game"); return 0; }
    if (!rom_open(rom_path, err, errlen)) return 0;
    blk = (sH->max_block + 63) & ~63u;
    sBlockBuf = malloc(blk + 16);                 /* + the zeroed tail, see load_block */
    sCompBuf = malloc(blk);                       /* compressed <= decompressed; course packed streams too */
    sUnpackBuf = malloc(sH->max_unpacked + 256);
    sXfTmpSize = (blk > 64 * 1024 ? blk : 64 * 1024) + 16; /* MIO0 recipes are block slices (course vertices: 133 KB), RAW is chunked */
    sXfTmp = malloc(sXfTmpSize);
    if (!sBlockBuf || !sCompBuf || !sUnpackBuf || !sXfTmp) { snprintf(err, errlen, "out of memory"); scratch_free(); rom_close(); return 0; }
    sBlockRom = 0xFFFFFFFF;
    memset(region, 0, region_size);
    signal(SIGSEGV, crash_handler); signal(SIGBUS, crash_handler); signal(SIGFPE, crash_handler); signal(SIGILL, crash_handler);

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
                    u32 done = 0;
                    /* through the temp buffer in bounded pieces; a pattern's phase must
                     * restart at word 0 of every piece, so pieces are whole periods */
                    u32 piece = 64 * 1024;
                    if (r->xform >= PA_XF_PATTERN) { u32 period = 4 * sPatterns[r->xform - PA_XF_PATTERN].nwords; piece -= piece % period; }
                    while (done < r->size) {
                        u32 n = r->size - done; if (n > piece) n = piece;
                        if (!rom_read(r->src + done, sCompBuf, (n + 3) & ~3u)) { snprintf(err, errlen, "ROM read failed at %08X", (unsigned) r->src); goto fail; }
                        if (!emit_xformed(dst + done, sCompBuf, n, r->xform)) { snprintf(err, errlen, "recipe %u too large", (unsigned) i); goto fail; }
                        done += n;
                    }
                }
                break;
            case PA_MIO0: {
                const PortAssetBlock* bk;
                if (!load_block(r->src, err, errlen)) goto fail;
                bk = find_block(r->src);
                if (r->extra > bk->decomp_len || r->size > bk->decomp_len - r->extra) { snprintf(err, errlen, "recipe %u outside its block", (unsigned) i); goto fail; }
                if (!emit_xformed(dst, sBlockBuf + r->extra, r->size, r->xform)) { snprintf(err, errlen, "recipe %u too large", (unsigned) i); goto fail; }
                break;
            }
            case PA_LITERAL:
                if (r->src > sH->literal_size || r->size > sH->literal_size - r->src) { snprintf(err, errlen, "recipe %u outside the literals", (unsigned) i); goto fail; }
                memcpy(dst, sLiterals + r->src, r->size);
                break;
            case PA_RELOCS:
            case PA_UNPACK:
                break;
            default:
                snprintf(err, errlen, "recipe %u has an unknown kind %u", (unsigned) i, (unsigned) r->kind); goto fail;
        }
        if (progress) progress("Extracting", (int) (i * 70 / sH->recipe_count)); /* the screen draws when pct or stage changed */
    }

    /* Packed course display lists: what load_course does on the N64. */
    for (i = 0; i < sH->course_count; i++) {
        const PortAssetCourseDl* c = &sCourses[i];
        u32 k;
        /* the packed stream is raw ROM data right after the course's vertex mio0 block */
        if (c->unpacked_len > sH->max_unpacked || c->packed_off >= c->rom_len) { snprintf(err, errlen, "bad course entry %u", (unsigned) i); goto fail; }
        if (c->rom_len > blk || !rom_read(c->rom_off, sCompBuf, c->rom_len)) {
            snprintf(err, errlen, "ROM read failed at %08X", (unsigned) c->rom_off); goto fail;
        }
        sBlockRom = 0xFFFFFFFF; /* sCompBuf reused */
        gHeapEndPtr = (uintptr_t) sUnpackBuf + ((c->unpacked_len + 15) & ~15u) + 8; /* unpack writes at gHeapEndPtr - (ALIGN16(len)+8) */
        memset(sUnpackBuf, 0, c->unpacked_len + 64);
        displaylist_unpack((uintptr_t*) (sCompBuf + c->packed_off), c->unpacked_len, 0);
        for (k = 0; k < sH->recipe_count; k++) {
            const PortAssetRecipe* r = &sRecipes[k];
            if (r->kind == PA_UNPACK && r->src == c->course) {
                if (r->extra > c->unpacked_len || r->size > c->unpacked_len - r->extra) { snprintf(err, errlen, "recipe %u outside its course stream", (unsigned) k); goto fail; }
                memcpy(region + r->dst, sUnpackBuf + r->extra, r->size);
            }
        }
        if (progress) progress("Unpacking courses", 70 + (int) (i * 20 / sH->course_count));
    }
    gHeapEndPtr = saved_heap_end;
    rom_close();
    scratch_free();
    sCurRecipe = 0xFFFFFFFF;
    signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL); signal(SIGFPE, SIG_DFL); signal(SIGILL, SIG_DFL);

    /* Verify before the pointers go in: crc of the region with pointer words
     * zero (literal recipes carry their link-time pointer values: clear them). */
    if (progress) progress("Checking the data", 90);
    for (i = 0; i < sH->reloc_count; i++) {
        u32 o = sRelocs[i].off & 0x7FFFFFFFu;
        if (region_size < 4 || o > region_size - 4) { snprintf(err, errlen, "reloc %u outside the region", (unsigned) i); return 0; }
        *(u32*) (region + o) = 0;
    }
    {
        u32 crc = 0, at = 0;
        while (at < region_size) { /* 1 MB steps, the bar 90..94 */
            u32 n = region_size - at; if (n > 1024 * 1024) n = 1024 * 1024;
            crc = port_crc32(crc, region + at, n);
            at += n;
            if (progress) progress("Checking the data", 90 + (int) ((uint64_t) at * 4 / region_size));
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
        if (progress && (i & 4095) == 0) progress("Linking the data", 94 + (int) ((uint64_t) i * 1 / sH->reloc_count));
    }
    port_log_always("assets: generated from %s in %u ms\n", rom_path, (unsigned) ((port_time_us() - t0) / 1000));
    return 1;
fail:
    gHeapEndPtr = saved_heap_end;
    rom_close();
    scratch_free();
    sCurRecipe = 0xFFFFFFFF;
    signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL); signal(SIGFPE, SIG_DFL); signal(SIGILL, SIG_DFL);
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
    memcpy(h.magic, "MK64ASST", 8);
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
        screen_progress("Saving", 95 + (int) ((uint64_t) off * 3 / region_size));
    }
    /* data_crc last: a file that has it is complete */
    if (fseek(f, (long) ((char*) &h.data_crc - (char*) &h), SEEK_SET) != 0 || fwrite(&crc, 4, 1, f) != 1) {
        snprintf(err, errlen, "write failed (SD card full?)"); fclose(f); remove(tmp); return 0;
    }
    /* On the card before the rename, and the rename itself after it, as eeprom.bin and the save states
     * (the DKR port 9c91d40). Nothing here was synced: a power cut within ~30 s of the first start (or of a
     * rebuild after an update) could leave assets.bin empty or naming clusters the FAT has freed -- the
     * next start extracts it all again, and a dangling name leaves the FAT inconsistent. The file again
     * after the rename: on the card's vfat a rename to a new name makes the directory slot with cluster 0
     * and size 0 and only marks the inode dirty; its fsync writes the slot (the SSB64 port cd5128c). */
    screen_progress("Finishing the copy", 98); /* the fsyncs and the rename: no megabyte, but time */
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { snprintf(err, errlen, "fsync failed"); fclose(f); remove(tmp); return 0; }
    if (fclose(f) != 0) { snprintf(err, errlen, "write failed (SD card full?)"); remove(tmp); return 0; }
    if (rename(tmp, path) != 0) { snprintf(err, errlen, "cannot rename %s", tmp); remove(tmp); return 0; }
    screen_progress("Finishing the copy", 99);
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
        fd = open(port_save_dir(), O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
    return 1;
}

static int try_load(const char* path, char* err, u32 errlen) {
    struct AssetHeader h;
    u32 done, region_size = (u32) (__assets_end - __assets_start), crc = 0;
    FILE* f = fopen(path, "rb");
    if (f == NULL) { snprintf(err, errlen, "not found"); return 0; }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    if (fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "MK64ASST", 8) != 0 || h.version != 1) {
        snprintf(err, errlen, "not an MK64 asset cache"); fclose(f); return 0;
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
    return 1;
}

/* ------------------------------------------------------------------ ROM search */
/* The dump's byte order is auto-detected from its header (z64/v64/n64), so the
 * extension is only a filter to avoid header-probing every file on the card --
 * the header check in rom_open is the real gate. Accept the byte-order
 * extensions and the generic cart-image ones every N64 tool uses. */
static int rom_ext_ok(const char* n) {
    /* two- or three-letter extensions: ".u1" never matched while only n[l - 4] == '.' was
     * accepted (DKR's form) */
    const char* dot = strrchr(n, '.');
    char e[4];
    size_t i, l;
    if (!dot || dot == n || (l = strlen(dot + 1)) < 2 || l > 3) return 0;
    for (i = 0; i <= l; i++) e[i] = (char) (dot[1 + i] ? (dot[1 + i] | 0x20) : 0);
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") ||   /* the three byte orders */
           !strcmp(e, "rom") || !strcmp(e, "bin") || !strcmp(e, "u64") ||   /* generic cart images */
           !strcmp(e, "usa") || !strcmp(e, "u1");                           /* region-suffixed dumps */
}

/* The first file in `dir` (case-insensitive extension) that opens as a US
 * Mario Kart 64 ROM. The first valid one wins; `why` gets the last rejection. */
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
        if (rom_open(path, err, sizeof(err))) {
            rom_close();
            snprintf(out, outlen, "%s", path);
            found = 1;
            break;
        }
        snprintf(why, whylen, "%s: %s", n, err);
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

/* rom.z64 in the game's folder (a zip packed with the player's ROM): the file is next to the executable -- found from
 * /proc/self/exe, not argv[0] (a launcher may exec the bare name). Only read, as a card ROM is (rom_open: "rb");
 * the cache still goes to the save dir and nothing records the ROM's path (assets.bin is keyed on the
 * build stamp and the data CRC). Not there: silent, the card search runs as before. Refused by the header
 * check: logged like a card file, then the card search. */
#define LOCAL_ROM_NAME "rom.z64"
static int local_rom(char* out, u32 outlen, char* why, u32 whylen) {
    char path[512], err[96];
    struct stat st;
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    char* s;
    if (n <= 0 || n >= (ssize_t) sizeof(path) - 1) return 0;
    path[n] = 0;
    s = strrchr(path, '/');
    if (s == NULL || (size_t) (s + 1 - path) + sizeof(LOCAL_ROM_NAME) > sizeof(path)) return 0;
    memcpy(s + 1, LOCAL_ROM_NAME, sizeof(LOCAL_ROM_NAME));
    if (stat(path, &st) != 0) return 0;
    if (rom_open(path, err, sizeof(err))) {
        rom_close();
        snprintf(out, outlen, "%s", path);
#if defined(MIYOO)
        port_log_always("rom: %s (in the game's folder)\n", path);
#else
        port_log_always("rom: %s (in the game's folder)\n", path);
#endif
        return 1;
    }
    snprintf(why, whylen, "%s: %s", LOCAL_ROM_NAME, err);
    port_log_always("assets: skipping %s: %s\n", path, err);
    return 0;
}

/* Where players may drop the ROM: in the game's folder (rom.z64), the game's own
 * data dir, anywhere on the card (root, a folder, or a folder in a folder),
 * anywhere in HOME, or an explicit MK64_ROM=<path> (which wins). */
static int find_rom(char* out, u32 outlen, char* why, u32 whylen) {
    const char* env = getenv("MK64_ROM");
    why[0] = 0;
    if (env && env[0]) {
        char err[96];
        if (rom_open(env, err, sizeof(err))) { rom_close(); snprintf(out, outlen, "%s", env); return 1; }
        snprintf(why, whylen, "MK64_ROM: %s", err);
        return 0;
    }
    if (local_rom(out, outlen, why, whylen)) return 1;
    if (scan_dir(port_eboot_dir(), out, outlen, why, whylen)) return 1;
    if (scan_dir(port_save_dir(), out, outlen, why, whylen)) return 1;
#if defined(TARGET_PC)
    if (scan_tree(".", 2, out, outlen, why, whylen)) return 1;
#elif defined(MIYOO)
    if (scan_tree(SD_HOME, 1, out, outlen, why, whylen)) return 1; /* the usual place first */
    if (scan_tree(SD_CARD, 2, out, outlen, why, whylen)) return 1;
#else
    if (scan_tree(SD_CARD, 2, out, outlen, why, whylen)) return 1;
    if (scan_tree(SD_HOME, 2, out, outlen, why, whylen)) return 1;
#endif
    return 0;
}

/* assets.bin.<pid>.tmp left by a run killed mid-write (~13 MB each) */
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
#if defined(HANDHELD) && !defined(TARGET_PC)
#include <SDL/SDL.h>
#include "handheld_screen.h"
#include "miyoo_video.h"
/* Miyoo Mini: the menus' bitmap font (miyoo_menu.c), 2x for the title and 1x for the rest, so
 * the first-start screen needs no font file. A "font" here is just its scale. */
typedef struct { int k; } TTF_Font;
static TTF_Font sBig = { 2 }, sSmall = { 1 };
static SDL_Surface* sScr SAVESTATE_EXCLUDE;
static TTF_Font *sFontBig SAVESTATE_EXCLUDE, *sFontSmall SAVESTATE_EXCLUDE;
static char sSub[64] = "Looking on the SD card"; /* second line under the title (the ROM's file name once known) */

static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "assets: SDL_Init: %s\n", SDL_GetError()); return; }
    /* the game's screen size (handheld_screen.c): the text centres on it, the bar is placed by it */
    sScr = HH_SetVideoMode(handheld_screen_w(), handheld_screen_h(), 16, SDL_SWSURFACE);
    if (handheld_screen_w() != 240 && (!sScr || sScr->w != handheld_screen_w() || sScr->h != handheld_screen_h())) {
        handheld_screen_confirm(sScr ? sScr->w : 0, sScr ? sScr->h : 0); /* no 320x240: the square size */
        sScr = HH_SetVideoMode(240, 240, 16, SDL_SWSURFACE);
    }
    if (!sScr) { fprintf(stderr, "assets: SDL_SetVideoMode: %s\n", SDL_GetError()); return; }
    SDL_ShowCursor(SDL_DISABLE);
    sFontBig = &sBig; sFontSmall = &sSmall;
}
static void ui_text(int y, const char* s, TTF_Font* f, int gray) {
    if (!sScr || !f || !s || !s[0]) return;
    const Uint32 c = 0xFF000000u | ((Uint32) gray << 16) | ((Uint32) gray << 8) | (Uint32) gray;
    miyoo_text(sScr, (sScr->w - miyoo_text_w(s, f->k)) / 2, y + (f->k == 1 ? 2 : 1), f->k, s, c);
}
/* title, up to three body lines, a progress bar (pct < 0: none), a footer */
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    ui_text(22, "MARIO KART 64", sFontBig, 255);
    ui_text(50, sSub, sFontSmall, 150);
    ui_text(86, l1, sFontSmall, 230);
    ui_text(104, l2, sFontSmall, 230);
    ui_text(122, l3, sFontSmall, 230);
    if (pct >= 0) {
        const Sint16 bx = (Sint16) ((sScr->w - 180) / 2); /* 30 on the 240 screen */
        SDL_Rect frame = { bx, 158, 180, 12 }, fill = { (Sint16) (bx + 2), 160, (Uint16) (176 * pct / 100), 8 };
        char p[8];
        SDL_FillRect(sScr, &frame, SDL_MapRGB(sScr->format, 70, 70, 70));
        SDL_FillRect(sScr, &fill, SDL_MapRGB(sScr->format, 255, 255, 255));
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(176, p, sFontSmall, 200);
    }
    ui_text(212, foot, sFontSmall, 150);
    /* MK64_SPLASH_DUMP=<dir>: write the rendered surface to a PPM (debug only,
     * env-gated). Lets the splash be captured off-device (SDL_ttf renders to
     * the surface even under the dummy video driver). */
    {
        const char* dd = getenv("MK64_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256]; FILE* f; int x, y;
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", sScr->w, sScr->h);
                if (SDL_MUSTLOCK(sScr)) SDL_LockSurface(sScr);
                for (y = 0; y < sScr->h; y++) for (x = 0; x < sScr->w; x++) {
                    Uint16* p = (Uint16*) ((Uint8*) sScr->pixels + y * sScr->pitch + x * 2);
                    Uint8 r, g, b, px[3];
                    SDL_GetRGB(*p, sScr->format, &r, &g, &b);
                    px[0] = r; px[1] = g; px[2] = b; fwrite(px, 1, 3, f);
                }
                if (SDL_MUSTLOCK(sScr)) SDL_UnlockSurface(sScr);
                fclose(f);
            }
        }
    }
    HH_Flip(sScr);
}
static void ui_wait_button(void) {
    SDL_Event ev;
    if (!sScr || getenv("MK64_ASSETS_ONLY")) return; /* headless asset run: never block */
    while (SDL_PollEvent(&ev)) { } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    SDL_Flip(sScr);
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0)); /* a double buffer's other page too */
    SDL_Flip(sScr);
}
/* done with the splash: fonts go, SDL stays up for gfx_init (quit == exiting) */
static void ui_end(int quit) {
    sFontBig = sFontSmall = NULL;
    if (quit && sScr) { SDL_Quit(); sScr = NULL; }
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

/* argv0: the executable's path, used to find recipes.bin next to it. */
void port_assets_load(const char* argv0) {
    char exe_dir[512] = "", rom[512], err[192], why[160];
    u8* blob; u32 blob_len;
    int rebuild = 0;
    if (argv0 != NULL) {
        const char* slash = strrchr(argv0, '/');
        if (slash != NULL && (size_t) (slash + 1 - argv0) < sizeof(exe_dir)) {
            memcpy(exe_dir, argv0, slash + 1 - argv0);
            exe_dir[slash + 1 - argv0] = 0;
        }
    }
    if (!exe_dir[0]) { /* a launcher may exec the bare name "mk64" */
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
    /* <data dir>/rebuild (the other ports' place) or <save dir>/rebuild (data/rebuild, beside assets.bin), the
     * player's, any content: do the first start's job again. assets.bin is not read; it is replaced (tmp + rename) only
     * by a copy made from a ROM that checks out, so with no ROM it stays, and so does the file, which is removed (both
     * places) once the new copy is on the card. */
    char rb_top[512];
    snprintf(rb_top, sizeof(rb_top), "%srebuild", port_eboot_dir());
    const int rb_top_on = access(rb_top, F_OK) == 0, rb_data_on = access(port_save_path("rebuild"), F_OK) == 0;
    if (rb_top_on || rb_data_on) {
        port_log_always("assets: %s found, the first start's job runs again\n", rb_top_on ? rb_top : port_save_path("rebuild"));
        snprintf(err, sizeof(err), "rebuild requested");
        rebuild = 1;
    } else if (try_load(port_save_path(CACHE_NAME), err, sizeof(err))) {
        return;
    }
    port_log_always("assets: %s%s: %s\n", port_save_dir(), CACHE_NAME, err);

    /* 2. the player's ROM */
    ui_begin();
    screen_progress("Looking for your ROM", 0);
    if (!find_rom(rom, sizeof(rom), why, sizeof(why))) {
        if (why[0]) port_log_always("assets: rejected: %s\n", why); /* the last file refused; each is logged above */
        if (sMk64Ver) fail_screen("No usable Mario Kart 64 ROM.", sMk64Ver, "It must be the US (NTSC) cart.");
        if (sMk64Seen) fail_screen("No usable Mario Kart 64 ROM.", "It must be the US (NTSC) cart.", NULL);
        fail_screen("No Mario Kart 64 ROM found.", "Copy your US (NTSC) .z64/.n64/.v64", "to the SD card and start again.");
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
    screen_progress("Saving", 95);
    if (!write_cache(port_save_path(CACHE_NAME), err, sizeof(err))) {
        port_log_always("assets: cache not written: %s\n", err); /* the game still runs; next start extracts again */
    } else {
        port_log_always("assets: cache written to %s\n", port_save_path(CACHE_NAME));
        if (rebuild) {
            const char* rbs[2] = { rb_top_on ? rb_top : NULL, rb_data_on ? port_save_path("rebuild") : NULL };
            for (int k = 0; k < 2; k++) {
                if (!rbs[k]) continue;
                if (remove(rbs[k]) == 0) port_log_always("assets: rebuild done, %s removed\n", rbs[k]);
                else port_log_always("assets: rebuild done, but %s can't be removed: the job runs again next start\n", rbs[k]);
            }
        }
    }
    free(blob);
    sH = NULL; sRecipes = NULL; sLiterals = NULL; sPatterns = NULL; sCourses = NULL; sBlocks = NULL; sRelocs = NULL;
    screen_progress("Done", 100); /* the cache is on the card: nothing else of the first start follows */
    if (getenv("MK64_SPLASH_DUMP") == NULL && getenv("MK64_ASSETS_ONLY") == NULL) usleep(600 * 1000);
    ui_black();
    sShown = 0;
    ui_end(0);
}
