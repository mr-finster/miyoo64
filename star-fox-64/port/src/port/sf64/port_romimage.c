/**
 * First start: build the port ROM image (sf64.rom) from the player's own ROM.
 *
 * The game folder carries no game data. On the first start (or after an update whose
 * recipes changed) this:
 *   1. finds a Star Fox 64 ROM (any byte order: z64/v64/n64) whose SHA-1 is
 *      US 1.1's (09f0d105...): first next to the game, where a personal zip
 *      puts the player's ROM as rom.z64 (only read), then on the SD card;
 *      every candidate is hashed, so another version, or the old image
 *      (sf64.rom keeps the cartridge header), is skipped rather than stopping
 *      the search;
 *   2. decompresses it exactly as the decomp's tools/comptool.py does: walk
 *      the cartridge's DMA table, MIO0-decompress the compressed files, and
 *      lay every file out at its virtual ROM address;
 *   3. replays recipes.bin (shipped in the game folder; numbers only: offsets,
 *      lengths, byte-swap units -- tools/handheld/derive_recipes.py) to turn
 *      each asset bank into the port's little-endian layout;
 *   4. writes sf64.rom to the save dir (tmp, fsync, rename) with a trailer
 *      tying it to this build's recipes, so a stale image is rebuilt.
 * port_rom.c then maps the image and every game DMA reads from it.
 *
 * Adapted from the MK64 handheld port's assets_handheld.c (ROM search, byte order
 * detection, the SDL 1.2 progress / error screens).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <PR/ultratypes.h>
#include "port.h"
#include "savestate.h" /* SAVESTATE_EXCLUDE: nothing here is game state */

#define IMAGE_NAME   "sf64.rom"
#define RECIPE_NAME  "recipes.bin"
#define EMBEDDED_NAME "embedded.bin"
#define PKG_ROM_NAME "rom.z64" /* the ROM in the game folder, next to the executable */
#define SD_CARD      "/mnt/"
#define DMA_TABLE_US11 0xDE480u /* the cartridge's file table (comptool.py file_table_dict, US 1.1) */
#define ROM_SIZE_US11 0xC00000u
/* One source of truth: the hex text (also shown on the error screen and in
 * the log); the check compares against the same text, so a hand-typed byte
 * array can't drift from it (round 1: one did). */
#define SHA1_US11_TEXT "09f0d105f476b00efa5303a3ebc42e60a7753b7a"

/* Trailer after the image bytes: the image is only used by the build whose
 * recipes produced it (port_rom.c checks it). */
typedef struct {
    char magic[8];      /* "SF64IMG1" */
    u32 image_size;
    u32 recipe_crc;     /* CRC-32 of recipes.bin */
    u32 image_crc;      /* CRC-32 of the image bytes */
    u32 reserved;
} ImageTrailer;
/* port_rom.c reads the trailer as the image's last 24 bytes */
typedef char ImageTrailerIs24Bytes[sizeof(ImageTrailer) == 24 ? 1 : -1];

extern u32 port_time_us(void);
static void exe_dir_of(const char* argv0, char* out, size_t outlen);

/* --------------------------------------------------------------- CRC-32 --- */
static u32 sCrcTable[256];
static u32 crc32_update(u32 crc, const u8* b, size_t n) {
    size_t i;
    if (sCrcTable[1] == 0) {
        u32 k, j;
        for (k = 0; k < 256; k++) {
            u32 c = k;
            for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            sCrcTable[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) crc = sCrcTable[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------------------------------------------------------- SHA-1 --- */
typedef struct { u32 h[5]; u64 len; u8 buf[64]; u32 fill; } Sha1;
static u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(Sha1* s, const u8* p) {
    u32 w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = (u32) p[i * 4] << 24 | (u32) p[i * 4 + 1] << 16 | (u32) p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void progress(const char* stage, int pct); /* the first-start screen, below */
/* from > to: the screen's bar moves from `lo` to `hi` as the blocks go, in 1 MB steps ("Checking the ROM") */
static void sha1_screen(const u8* data, size_t n, u8 out[20], int lo, int hi);
static void sha1(const u8* data, size_t n, u8 out[20]) {
    sha1_screen(data, n, out, 0, 0);
}
static void sha1_screen(const u8* data, size_t n, u8 out[20], int lo, int hi) {
    Sha1 s = { { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u }, 0, { 0 }, 0 };
    size_t i;
    u8 pad[128];
    size_t tail = n % 64, padlen;
    for (i = 0; i + 64 <= n; i += 64) {
        sha1_block(&s, data + i);
        if (hi > lo && (i & 0xFFFFF) == 0xFFFC0) progress("Checking the ROM", lo + (int) ((u64) i * (u32) (hi - lo) / n));
    }
    memcpy(pad, data + n - tail, tail);
    pad[tail] = 0x80;
    padlen = tail < 56 ? 64 : 128;
    memset(pad + tail + 1, 0, padlen - tail - 1);
    for (i = 0; i < 8; i++) pad[padlen - 1 - i] = (u8) ((u64) n * 8 >> (i * 8));
    sha1_block(&s, pad);
    if (padlen == 128) sha1_block(&s, pad + 64);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (u8) (s.h[i] >> 24); out[i * 4 + 1] = (u8) (s.h[i] >> 16);
        out[i * 4 + 2] = (u8) (s.h[i] >> 8); out[i * 4 + 3] = (u8) s.h[i];
    }
}

/* ----------------------------------------------------------------- MIO0 --- */
static u32 be32(const u8* p) { return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]; }

/* Decode one MIO0 block into `out` (capacity `cap`); returns the decoded size, 0 on error. */
static u32 mio0_decode(const u8* in, u32 inlen, u8* out, u32 cap) {
    u32 size, comp_off, raw_off, layout = 16, pos = 0, bits = 0, nbits = 0;
    if (inlen < 16 || memcmp(in, "MIO0", 4) != 0) return 0;
    size = be32(in + 4); comp_off = be32(in + 8); raw_off = be32(in + 12);
    if (size > cap || comp_off > inlen || raw_off > inlen) return 0;
    while (pos < size) {
        if (nbits == 0) {
            if (layout + 4 > inlen) return 0;
            bits = be32(in + layout); layout += 4; nbits = 32;
        }
        if (bits & 0x80000000u) { /* literal byte */
            if (raw_off >= inlen) return 0;
            out[pos++] = in[raw_off++];
        } else {                  /* back-reference: 4-bit length-3, 12-bit distance-1 */
            u32 v, len, dist, k;
            if (comp_off + 2 > inlen) return 0;
            v = (u32) in[comp_off] << 8 | in[comp_off + 1]; comp_off += 2;
            len = (v >> 12) + 3; dist = (v & 0xFFF) + 1;
            if (dist > pos || pos + len > size) return 0;
            for (k = 0; k < len; k++, pos++) out[pos] = out[pos - dist];
        }
        bits <<= 1; nbits--;
    }
    return size;
}

/* ------------------------------------------------------------------ ROM --- */
/* Read a whole ROM file into memory in big-endian (z64) order. */
static u8* rom_load(const char* path, u32* out_len, char* err, size_t errlen) {
    FILE* f = fopen(path, "rb");
    long n;
    u8* b;
    u32 i;
    if (f == NULL) { snprintf(err, errlen, "cannot open"); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0x1000 || n > 0x4000000) {
        fclose(f); snprintf(err, errlen, "not an N64 ROM (size)"); return NULL;
    }
    rewind(f);
    b = malloc((size_t) n);
    for (i = 0; b != NULL && i < (u32) n; ) { /* 1 MB steps, the bar 1..5 */
        const u32 c = (u32) n - i > 0x100000u ? 0x100000u : (u32) n - i;
        if (fread(b + i, 1, c, f) != c) { free(b); b = NULL; break; }
        i += c;
        progress("Reading the ROM", 1 + (int) ((u64) i * 4 / (u32) n));
    }
    if (b == NULL) {
        fclose(f); snprintf(err, errlen, "unreadable"); return NULL;
    }
    fclose(f);
    if (b[0] == 0x37 && b[1] == 0x80) {        /* v64: byte-swapped pairs */
        for (i = 0; i + 1 < (u32) n; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; }
    } else if (b[0] == 0x40 && b[1] == 0x12) { /* n64: little-endian words */
        for (i = 0; i + 3 < (u32) n; i += 4) { u8 t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; }
    } else if (!(b[0] == 0x80 && b[1] == 0x37)) {
        free(b); snprintf(err, errlen, "not an N64 ROM"); return NULL;
    }
    *out_len = (u32) n;
    return b;
}

/* The version a Star Fox 64 header names, for the no-usable-ROM screen (as OoT, DKR and GoldenEye
 * say it): the country code (E USA, J Japan, P Europe = Lylat Wars, U Australia) and, for the US
 * cart, the revision byte (0 = 1.0); NULL for the US 1.1 header (a bad dump says nothing more). */
static const char* sHdrVer SAVESTATE_EXCLUDE;
static const char* sStarFoxVer SAVESTATE_EXCLUDE;

/* Cheap filter before hashing a whole file: the cartridge header. 1 = a US Star
 * Fox 64 header, -1 = Star Fox 64 from another region, 0 = anything else. */
static int rom_header_ok(const char* path, char* err, size_t errlen) {
    u8 h[0x40];
    FILE* f = fopen(path, "rb");
    size_t i;
    if (f == NULL) { snprintf(err, errlen, "cannot open"); return 0; }
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); snprintf(err, errlen, "unreadable"); return 0; }
    fclose(f);
    if (h[0] == 0x37 && h[1] == 0x80) { for (i = 0; i < sizeof(h); i += 2) { u8 t = h[i]; h[i] = h[i + 1]; h[i + 1] = t; } }
    else if (h[0] == 0x40 && h[1] == 0x12) { for (i = 0; i < sizeof(h); i += 4) { u8 t = h[i]; h[i] = h[i + 3]; h[i + 3] = t; t = h[i + 1]; h[i + 1] = h[i + 2]; h[i + 2] = t; } }
    else if (!(h[0] == 0x80 && h[1] == 0x37)) { snprintf(err, errlen, "not an N64 ROM"); return 0; }
    sHdrVer = NULL;
    /* the game code NFX* too: Lylat Wars (Europe) is the same game under another title */
    if (memcmp(h + 0x20, "STARFOX64", 9) != 0 && memcmp(h + 0x3B, "NFX", 3) != 0) { snprintf(err, errlen, "not Star Fox 64"); return 0; }
    sHdrVer = h[0x3E] == 'E' ? (h[0x3F] == 0 ? "It is the US 1.0 version." : NULL) :
              h[0x3E] == 'P' ? "It is the European (PAL) version." :
              h[0x3E] == 'J' ? "It is the Japanese version." :
              h[0x3E] == 'U' ? "It is the Australian (PAL) version." : "It is not the US version.";
    if (h[0x3E] != 'E') { snprintf(err, errlen, "not the US (NTSC) version (region '%c')", h[0x3E]); return -1; }
    return 1;
}

static int rom_ext_ok(const char* n) {
    size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') return 0;
    e[0] = (char) (n[l - 3] | 0x20); e[1] = (char) (n[l - 2] | 0x20); e[2] = (char) (n[l - 1] | 0x20); e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") || !strcmp(e, "rom") ||
           !strcmp(e, "bin") || !strcmp(e, "u64") || !strcmp(e, "usa") ||
           !strcmp(e, "u1");
}

static void progress_rom(const char* path);

/* The ROM find_rom settled on, loaded (z64 order) and hashed; handed to the build. */
static u8* sRomData SAVESTATE_EXCLUDE;
static u32 sRomLen SAVESTATE_EXCLUDE;
/* A Star Fox 64 dump was seen but was not usable: "No usable", not "not found". */
static int sStarFoxSeen SAVESTATE_EXCLUDE;

/* Is `path` the US 1.1 ROM? Header first, then the whole file's SHA-1, so a wrong
 * version is skipped and the search goes on (as the DKR and SSB64 ports do). */
static int rom_candidate(const char* path, char* err, size_t errlen) {
    u8 digest[20], *data;
    char got[41];
    u32 len;
    int i;
    const int h = rom_header_ok(path, err, errlen);
    if (h != 1) {
        if (h < 0) { sStarFoxSeen = 1; sStarFoxVer = sHdrVer; }
        return 0;
    }
    sStarFoxSeen = 1;
    progress_rom(path);
    data = rom_load(path, &len, err, errlen);
    if (data == NULL) return 0;
    sha1_screen(data, len, digest, 5, 10);
    for (i = 0; i < 20; i++) snprintf(got + i * 2, 3, "%02x", digest[i]);
    if (len != ROM_SIZE_US11 || strcmp(got, SHA1_US11_TEXT) != 0) {
        snprintf(err, errlen, "SHA-1 %s, want %s (US 1.1)", got, SHA1_US11_TEXT);
        if (sHdrVer) sStarFoxVer = sHdrVer;
        free(data);
        return 0;
    }
    free(sRomData);
    sRomData = data;
    sRomLen = len;
    return 1;
}

/* The first file in `dir` that is the US 1.1 ROM; `why` gets the last rejection. */
static int scan_dir(const char* dir, char* out, size_t outlen, char* why, size_t whylen) {
    DIR* d = opendir(dir);
    struct dirent* ent;
    int found = 0;
    if (d == NULL) return 0;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        char path[512], err[160];
        /* hidden files ("._x.z64" resource forks), other names, and our own files:
         * sf64.rom keeps the cartridge header, so it would pass for a ROM */
        if (n[0] == '.' || !rom_ext_ok(n) || !strcmp(n, IMAGE_NAME) || !strcmp(n, RECIPE_NAME) ||
            !strcmp(n, EMBEDDED_NAME))
            continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", n);
        if (rom_candidate(path, err, sizeof(err))) {
            snprintf(out, outlen, "%s", path);
            found = 1;
            break;
        }
        snprintf(why, whylen, "%s: %s", n, err);
        port_log_always("romimage: skipping %s: %s\n", path, err);
    }
    closedir(d);
    return found;
}

static int scan_tree(const char* root, int depth, char* out, size_t outlen, char* why, size_t whylen) {
    DIR* d;
    struct dirent* ent;
    if (scan_dir(root, out, outlen, why, whylen)) return 1;
    if (depth <= 0 || (d = opendir(root)) == NULL) return 0;
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

/* The player's ROM: SF64_BASEROM=<path>, next to the game, in the save dir,
 * or anywhere on the SD card (two folders deep). Next to the game is the game
 * folder (see exe_dir_of): a personal zip puts the ROM there as rom.z64, the only
 * file of the game folder scan_dir takes for a ROM, so it is checked before the card; one that fails the check is
 * skipped like a card file and the search goes on. It is only read: sf64.rom
 * and everything else the game writes go to the save dir. */
static int find_rom(const char* exe_dir, char* out, size_t outlen, char* why, size_t whylen) {
    const char* env = getenv("SF64_BASEROM");
    why[0] = 0;
    if (env && env[0]) {
        char err[160];
        if (rom_candidate(env, err, sizeof(err))) { snprintf(out, outlen, "%s", env); return 1; }
        snprintf(why, whylen, "SF64_BASEROM: %s", err);
        port_log_always("romimage: skipping %s: %s\n", env, err);
        return 0;
    }
    if (exe_dir[0] && scan_dir(exe_dir, out, outlen, why, whylen)) return 1;
    if (scan_dir(port_save_dir(), out, outlen, why, whylen)) return 1;
#if defined(TARGET_PC)
    if (scan_tree(".", 2, out, outlen, why, whylen)) return 1;
#elif defined(MIYOO)
    { /* the Miyoo Mini: Roms/N64, then the whole card, two folders deep each (miyoo_panel.c) */
        extern const char* const miyoo_rom_roots[];
        for (int i = 0; miyoo_rom_roots[i]; i++)
            if (scan_tree(miyoo_rom_roots[i], 2, out, outlen, why, whylen)) return 1;
    }
#endif
    return 0;
}

/* ------------------------------------------------------------------- UI --- */
#if defined(MIYOO)
/* The Miyoo Mini: the same screens at 320x240 in the Miyoo menu's 8x8 font (miyoo_text), doubled and turned onto
 * the panel (miyoo_panel.c), which main.c opened before anything else. The panel stays up for the game. */
#include <SDL/SDL.h>
#include "miyoo_panel.h"
#define UI_W 320
#define UI_H 240
static SDL_Surface* sScr SAVESTATE_EXCLUDE;
static uint32_t sUiPix[UI_W * UI_H] SAVESTATE_EXCLUDE;
#define SUB_DEFAULT "Looking on the SD card" /* the name line until a ROM is being read */
static char sSub[64] = SUB_DEFAULT;
static void ui_begin(void) {
    if (sScr) return;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_Init(SDL_INIT_VIDEO) < 0) return;
    if (!miyoo_panel_open()) return;
    sScr = SDL_CreateRGBSurfaceFrom(sUiPix, UI_W, UI_H, 32, UI_W * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}
static void ui_text(int y, const char* s, int k, int gray) {
    char fit[96];
    if (!sScr || !s || !s[0]) return;
    /* a line wider than 312 px (a long ROM file name) keeps its start and ends in "..." */
    snprintf(fit, sizeof(fit), "%s", s);
    while (fit[0] && miyoo_text_w(fit, k) > UI_W - 8) {
        size_t n = strlen(fit);
        if (n >= 4 && strcmp(fit + n - 3, "...") == 0) n -= 3;
        if (n == 0) break;
        snprintf(fit + n - 1, sizeof(fit) - (n - 1), "...");
    }
    miyoo_text(sScr, (UI_W - miyoo_text_w(fit, k)) / 2, y, k, fit, 0xFF000000u | (u32) gray << 16 | (u32) gray << 8 | (u32) gray);
}
static void ui_fill(int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h && j < UI_H; j++)
        for (int i = x; i < x + w && i < UI_W; i++) sUiPix[j * UI_W + i] = c;
}
static void ui_show(void) {
    miyoo_panel_present32(sUiPix, UI_W, UI_H);
    miyoo_panel_flip();
}
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (!sScr) return;
    memset(sUiPix, 0, sizeof(sUiPix));
    ui_text(22, "STAR FOX 64", 2, 255);
    ui_text(52, sSub, 1, 150);
    ui_text(88, l1, 1, 230);
    ui_text(106, l2, 1, 230);
    ui_text(124, l3, 1, 230);
    if (pct >= 0) {
        char p[8];
        ui_fill(40, 158, 240, 12, 0xFF464646u);
        ui_fill(42, 160, 236 * pct / 100, 8, 0xFFFFFFFFu);
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(178, p, 1, 200);
    }
    ui_text(214, foot, 1, 150);
    { /* SF64_SPLASH_DUMP=<dir>: the screen as a PPM (tests, under qemu's dummy video) */
        const char* dd = getenv("SF64_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256];
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
                for (int i = 0; i < UI_W * UI_H; i++) {
                    const u8 px[3] = { (u8) (sUiPix[i] >> 16), (u8) (sUiPix[i] >> 8), (u8) sUiPix[i] };
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
    if (!sScr) return;
    while (SDL_PollEvent(&ev)) { }
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    memset(sUiPix, 0, sizeof(sUiPix));
    ui_show();
    ui_show(); /* a double buffer's other page too */
}
static void ui_end(int quit) {
    if (sScr) { SDL_FreeSurface(sScr); sScr = NULL; }
    if (quit) SDL_Quit();
}
#else
#define SUB_DEFAULT "Looking on the SD card"
static char sSub[64] = SUB_DEFAULT;
static void ui_begin(void) { }
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (pct >= 0) printf("romimage: %s %s %d%%\n", l1, l3 ? l3 : "", pct);
    else printf("romimage: %s | %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", foot ? foot : "");
}
static void ui_wait_button(void) { }
static void ui_black(void) { }
static void ui_end(int quit) { (void) quit; }
#endif

/* Stages and wording as the MK64 port's first start: Reading the ROM, Extracting,
 * Saving, Done. */
static int sProgressLast SAVESTATE_EXCLUDE = -1;
static const char* sProgressStage SAVESTATE_EXCLUDE;
static char sProgressSub[64] SAVESTATE_EXCLUDE;
static void progress(const char* stage, int pct) {
    /* drawn only when the pct, the stage or the name line changed */
    if (pct == sProgressLast && stage == sProgressStage && strcmp(sSub, sProgressSub) == 0) return;
    sProgressLast = pct;
    sProgressStage = stage;
    snprintf(sProgressSub, sizeof(sProgressSub), "%s", sSub);
    ui_draw("First start: building the game", "data from your ROM.", stage, pct, "This happens once.");
}

/* The name line shows the file being read; redrawn for each candidate. */
static void progress_rom(const char* path) {
    const char* base = strrchr(path, '/');
    snprintf(sSub, sizeof(sSub), "%.60s", base ? base + 1 : path);
    progress("Reading the ROM", 0);
}

static void fail_screen(const char* l1, const char* l2, const char* l3) {
    ui_begin();
    if (strcmp(sSub, SUB_DEFAULT) == 0) sSub[0] = 0; /* an error screen has no "Looking" line */
    port_log_always("romimage: FAILED: %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}

/* -------------------------------------------------------------- recipes --- */
static u8* file_load(const char* path, u32* out_len) {
    FILE* f = fopen(path, "rb");
    long n;
    u8* b;
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) { fclose(f); return NULL; }
    rewind(f);
    b = malloc((size_t) n);
    if (b == NULL || fread(b, 1, (size_t) n, f) != (size_t) n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *out_len = (u32) n;
    return b;
}
static u32 le32(const u8* p) { return p[0] | (u32) p[1] << 8 | (u32) p[2] << 16 | (u32) p[3] << 24; }

/* Replay recipes.bin over the decompressed image, in place. 0 on a malformed file. */
static int apply_recipes(u8* img, u32 img_len, const u8* r, u32 rlen) {
    u32 banks, b, off = 12;
    if (rlen < 12 || memcmp(r, "SF64RCP2", 8) != 0) return 0;
    banks = le32(r + 8);
    for (b = 0; b < banks; b++) {
        u32 start, size, runs, k, pos;
        if (off + 12 > rlen) return 0;
        start = le32(r + off); size = le32(r + off + 4); runs = le32(r + off + 8); off += 12;
        if (start > img_len || size > img_len - start || runs > (rlen - off) / 4) return 0; /* off <= rlen here */
        pos = start;
        for (k = 0; k < runs; k++, off += 4) {
            const u32 w = le32(r + off), len = w >> 3, unit = w & 7;
            u8* p = img + pos;
            u32 i;
            if (len > start + size - pos) return 0;
            if (unit == 2) {
                for (i = 0; i + 1 < len; i += 2) { u8 t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
            } else if (unit == 4) {
                for (i = 0; i + 3 < len; i += 4) { u8 t = p[i]; p[i] = p[i + 3]; p[i + 3] = t; t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t; }
            } else if (unit != 1) {
                return 0;
            }
            pos += len;
        }
        if (pos != start + size) return 0;
        if (b % 4 == 0) progress("Extracting", 55 + 35 * (int) b / (int) banks);
    }
    return 1;
}

/* The radio (message) bank: linked into this executable's NOLOAD region
 * (ast_radio_VRAM, Makefile.handheld), rebuilt from the ROM image at every
 * start. Its pointers are real addresses, so the recipe's unit 5 swaps the
 * word and adds (port_base - n64_base). */
extern u8 ast_radio_VRAM[], ast_radio_VRAM_END[];
static int apply_radio(const u8* r, u32 rlen, char* err, size_t errlen) {
    extern void port_rom_read(uintptr_t romAddr, void* dst, size_t size);
    u32 banks, b, off = 12, start, size, n64_base, port_base, runs, k, pos = 0, delta;
    u8* region = ast_radio_VRAM;
    if (rlen < 12 || memcmp(r, "SF64RCP2", 8) != 0) { snprintf(err, errlen, "bad recipe file"); return 0; }
    banks = le32(r + 8);
    for (b = 0; b < banks; b++) { /* skip the bank sections */
        if (off + 12 > rlen || le32(r + off + 8) > (rlen - off - 12) / 4) { snprintf(err, errlen, "bad recipe file"); return 0; }
        off += 12 + le32(r + off + 8) * 4;
    }
    if (off + 20 > rlen) { snprintf(err, errlen, "no radio section"); return 0; }
    start = le32(r + off); size = le32(r + off + 4); n64_base = le32(r + off + 8);
    port_base = le32(r + off + 12); runs = le32(r + off + 16); off += 20;
    if (port_base != (u32) (uintptr_t) region) {
        snprintf(err, errlen, "recipes are for another build (radio %08X, here %08X)", port_base, (u32) (uintptr_t) region);
        return 0;
    }
    if (size > (u32) (ast_radio_VRAM_END - ast_radio_VRAM) || runs > (rlen - off) / 4) {
        snprintf(err, errlen, "radio section does not fit");
        return 0;
    }
    port_rom_read(start, region, size); /* the ROM's bytes, then in place: */
    delta = port_base - n64_base;
    for (k = 0; k < runs; k++, off += 4) {
        const u32 w = le32(r + off), len = w >> 3, unit = w & 7;
        u8* p = region + pos;
        u32 i;
        if (len > size - pos) { snprintf(err, errlen, "radio run overflows"); return 0; }
        if (unit == 2) {
            for (i = 0; i + 1 < len; i += 2) { u8 t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
        } else if (unit == 4 || unit == 5) {
            for (i = 0; i + 3 < len; i += 4) {
                u32 v = be32(p + i);
                if (unit == 5) v += delta;
                p[i] = (u8) v; p[i + 1] = (u8) (v >> 8); p[i + 2] = (u8) (v >> 16); p[i + 3] = (u8) (v >> 24);
            }
        } else if (unit != 1) {
            snprintf(err, errlen, "bad radio unit %u", unit);
            return 0;
        }
        pos += len;
    }
    if (pos != size) { snprintf(err, errlen, "radio runs cover %u of %u bytes", pos, size); return 0; }
    return 1;
}

/* Every start, after port_rom_open(): fill the message bank from the image. */
void port_radio_fill(const char* argv0) {
    char exe_dir[512], rpath[600], err[160];
    u8* r;
    u32 rlen;
    exe_dir_of(argv0, exe_dir, sizeof(exe_dir));
    snprintf(rpath, sizeof(rpath), "%s%s", exe_dir, RECIPE_NAME);
    r = file_load(rpath, &rlen);
    if (r == NULL) fail_screen("This build is missing", RECIPE_NAME ".", "Reinstall the game.");
    if (!apply_radio(r, rlen, err, sizeof(err))) {
        free(r);
        fail_screen("Cannot set up the game text:", err, NULL);
    }
    free(r);
}

/* The game data compiled into the decomp's C sources (.hhasset, NOLOAD in the
 * shipped executable): embedded.bin (tools/handheld/derive_embedded.py) gives
 * each symbol's port address, ROM offset, size and overlay, and runs as in
 * recipes.bin plus unit 6 (the port's own word -- a pointer or a build
 * address -- stored as the literal that follows) and unit 7 (the same for a
 * 1-3 byte tail).
 * Filled at every start; an overlay's entries again whenever the game
 * reloads that overlay (the N64 re-DMAs its data). */
static u8* sEmb SAVESTATE_EXCLUDE;   /* the file, kept for overlay reloads */
static u32 sEmbLen SAVESTATE_EXCLUDE;

static int embedded_apply(u32 only_ovl, char* err, size_t errlen) {
    extern void port_rom_read(uintptr_t romAddr, void* dst, size_t size);
    extern u8 __hhasset_start[], __hhasset_end[];
    u32 n, e, off = 12;
    if (sEmbLen < 12 || memcmp(sEmb, "SF64EMB1", 8) != 0) { snprintf(err, errlen, "bad " EMBEDDED_NAME); return 0; }
    n = le32(sEmb + 8);
    for (e = 0; e < n; e++) {
        u32 addr, rom_off, size, ovl, words, k, pos = 0;
        u8* dst;
        if (off + 20 > sEmbLen) { snprintf(err, errlen, "truncated " EMBEDDED_NAME); return 0; }
        addr = le32(sEmb + off); rom_off = le32(sEmb + off + 4); size = le32(sEmb + off + 8);
        ovl = le32(sEmb + off + 12); words = le32(sEmb + off + 16); off += 20;
        if (words > (sEmbLen - off) / 4) { snprintf(err, errlen, "truncated " EMBEDDED_NAME); return 0; }
        dst = (u8*) (uintptr_t) addr;
        if (dst < __hhasset_start || dst > __hhasset_end || size > (u32) (__hhasset_end - dst)) {
            snprintf(err, errlen, "entry %u outside the region (another build?)", e);
            return 0;
        }
        if (only_ovl != 0xFFFFFFFEu && ovl != only_ovl) { off += words * 4; continue; }
        port_rom_read(rom_off, dst, size);
        for (k = 0; k < words; k++) {
            const u32 w = le32(sEmb + off + k * 4), len = w >> 3, unit = w & 7;
            u8* p = dst + pos;
            u32 i;
            if (len > size - pos) { snprintf(err, errlen, "entry %u overflows", e); return 0; }
            if (unit == 2) {
                for (i = 0; i + 1 < len; i += 2) { u8 t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
            } else if (unit == 4) {
                for (i = 0; i + 3 < len; i += 4) { u8 t = p[i]; p[i] = p[i + 3]; p[i + 3] = t; t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t; }
            } else if (unit == 6) {
                if (len / 4 > words - 1 - k) { snprintf(err, errlen, "entry %u: literals missing", e); return 0; }
                for (i = 0; i + 3 < len; i += 4) {
                    const u32 v = le32(sEmb + off + (k + 1) * 4);
                    memcpy(p + i, &v, 4);
                    k++;
                }
            } else if (unit == 7) {
                /* a 1-3 byte tail of the port's own bytes */
                if (len > 3 || k + 1 >= words) { snprintf(err, errlen, "entry %u: bad tail", e); return 0; }
                const u32 v = le32(sEmb + off + (k + 1) * 4);
                memcpy(p, &v, len);
                k++;
            } else if (unit != 1) {
                snprintf(err, errlen, "entry %u: bad unit %u", e, unit);
                return 0;
            }
            pos += len;
        }
        if (pos != size) { snprintf(err, errlen, "entry %u covers %u of %u bytes", e, pos, size); return 0; }
        off += words * 4;
    }
    return 1;
}

/* Every start, after port_rom_open(). */
void port_embedded_fill(const char* argv0) {
    char exe_dir[512], path[600], err[160];
    exe_dir_of(argv0, exe_dir, sizeof(exe_dir));
    snprintf(path, sizeof(path), "%s%s", exe_dir, EMBEDDED_NAME);
    sEmb = file_load(path, &sEmbLen);
    if (sEmb == NULL) fail_screen("This build is missing", EMBEDDED_NAME ".", "Reinstall the game.");
    if (!embedded_apply(0xFFFFFFFEu, err, sizeof(err))) fail_screen("Cannot set up the game data:", err, NULL);
    {
        /* debug: SF64_HHDUMP=<file> writes the filled region, to compare
         * with the recipe link's bytes (tools/handheld/check_hhdump.py) */
        extern u8 __hhasset_start[], __hhasset_end[];
        const char* dump = getenv("SF64_HHDUMP");
        FILE* f = dump != NULL ? fopen(dump, "wb") : NULL;
        if (f != NULL) {
            fwrite(__hhasset_start, 1, (size_t) (__hhasset_end - __hhasset_start), f);
            fclose(f);
        }
    }
}

/* port_overlay_load: the overlay's own entries, fresh from the ROM. */
void port_embedded_refill(u32 ovl) {
    char err[160];
    if (sEmb != NULL && !embedded_apply(ovl, err, sizeof(err))) port_log_always("embedded refill: %s\n", err);
}

/* ---------------------------------------------------------- the image --- */
static void put_be32(u8* p, u32 v) { p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v; }
static u32 rol32v(u32 x, u32 n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }

/* comptool.py calculate_crcs(): the header checksums over 0x1000-0x101000. */
static void header_crcs(const u8* rom, u32 out[2]) {
    u32 t1, t2, t3, t4, t5, t6, i;
    t1 = t2 = t3 = t4 = t5 = t6 = 0xF8CA4DDCu;
    for (i = 0x1000; i < 0x101000; i += 4) {
        const u32 d = be32(rom + i), r = rol32v(d, d & 0x1F);
        if (t6 + d < t6) t4++;
        t3 ^= d;
        t6 += d;
        t2 ^= (t2 > d) ? r : (t6 ^ d);
        t5 += r;
        t1 += t5 ^ d;
    }
    out[0] = t6 ^ t4 ^ t3;
    out[1] = t5 ^ t2 ^ t1;
}
/* The cartridge's DMA table walk, exactly as comptool.py decompress(): each
 * file goes at its virtual ROM address (the output is cut or zero-extended
 * to that address first), MIO0-compressed files decoded. */
static u8* decompress_rom(const u8* rom, u32 rom_len, u32* out_len, char* err, size_t errlen) {
    u32 i, cap = 0, len = 0, files = 0;
    u8* out;
    /* the image size: the furthest virtual end of any file (compressed files
     * read their decoded size from the MIO0 header) */
    for (i = 0;; i++) {
        const u32 e = DMA_TABLE_US11 + i * 16;
        u32 v, ps, pe, flag, sz;
        if (e + 16 > rom_len) { snprintf(err, errlen, "DMA table runs off the ROM"); return NULL; }
        v = be32(rom + e); ps = be32(rom + e + 4); pe = be32(rom + e + 8); flag = be32(rom + e + 12);
        if (v == 0 && pe == 0) break;
        files++;
        if (pe < ps || pe > rom_len) { snprintf(err, errlen, "bad DMA entry %u", i); return NULL; }
        sz = flag == 1 ? (pe - ps >= 8 ? be32(rom + ps + 4) : 0) : pe - ps;
        if (v + sz > cap) cap = v + sz;
    }
    if (cap == 0 || cap > 0x2000000) { snprintf(err, errlen, "bad DMA table"); return NULL; }
    out = calloc(1, cap);
    if (out == NULL) { snprintf(err, errlen, "out of memory (%u bytes)", cap); return NULL; }
    for (i = 0;; i++) {
        const u32 e = DMA_TABLE_US11 + i * 16;
        const u32 v = be32(rom + e), ps = be32(rom + e + 4), pe = be32(rom + e + 8), flag = be32(rom + e + 12);
        if (v == 0 && pe == 0) break;
        if (v > len) memset(out + len, 0, v - len); /* truncate() semantics: zero-extend... */
        len = v;                                    /* ...or cut back to the file's start */
        if (flag == 0) {
            memcpy(out + len, rom + ps, pe - ps);
            len += pe - ps;
        } else if (flag == 1) {
            const u32 n = mio0_decode(rom + ps, pe - ps, out + len, cap - len);
            if (n == 0) { free(out); snprintf(err, errlen, "MIO0 error in file %u", i); return NULL; }
            len += n;
        } else {
            free(out); snprintf(err, errlen, "bad compression flag in file %u", i); return NULL;
        }
        { /* comptool rewrites the entry in the OUTPUT at the table offset:
           * (vrom start, vrom end, flag 0). File semantics exactly: a write
           * past the current end zero-fills up to it, and a later file's
           * truncate() cuts it (entries 0 and 1 are lost that way when the
           * table file itself is laid down -- reproduced, not "fixed"). */
            const u32 fe = DMA_TABLE_US11 + i * 16 + 4, vend = len;
            if (fe + 12 > cap) { free(out); snprintf(err, errlen, "DMA entry %u outside the image", i); return NULL; }
            if (fe > len) memset(out + len, 0, fe - len);
            put_be32(out + fe, v);
            put_be32(out + fe + 4, vend);
            put_be32(out + fe + 8, 0);
            if (fe + 12 > len) len = fe + 12;
        }
        progress("Extracting", 10 + (int) ((u64) 45 * (i + 1) / files)); /* the bar 10..55 over the table's files */
    }
    if (len < 0x101000) { free(out); snprintf(err, errlen, "image too small"); return NULL; }
    { /* and the header checksums of the decompressed ROM, big-endian at 0x10 */
        u32 crc[2];
        header_crcs(out, crc);
        put_be32(out + 0x10, crc[0]);
        put_be32(out + 0x14, crc[1]);
    }
    *out_len = len;
    return out;
}

static int write_image(const char* path, const u8* img, u32 len, u32 recipe_crc, char* err, size_t errlen) {
    char tmp[600];
    ImageTrailer t;
    FILE* f;
    int ok, dir;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL) { snprintf(err, errlen, "cannot create %s", tmp); return 0; }
    memset(&t, 0, sizeof(t));
    memcpy(t.magic, "SF64IMG1", 8);
    t.image_size = len;
    t.recipe_crc = recipe_crc;
    { /* the image's CRC and the write in 1 MB steps, the bar 90..96 */
        u32 at, crc = 0;
        for (at = 0; at < len; ) {
            const u32 c = len - at > 0x100000u ? 0x100000u : len - at;
            crc = crc32_update(crc, img + at, c);
            at += c;
            progress("Saving", 90 + (int) ((u64) at * 3 / len));
        }
        t.image_crc = crc;
    }
    ok = 1;
    { u32 at; for (at = 0; ok && at < len; ) {
        const u32 c = len - at > 0x100000u ? 0x100000u : len - at;
        ok = fwrite(img + at, 1, c, f) == c;
        at += c;
        progress("Saving", 93 + (int) ((u64) at * 3 / len));
    } }
    ok = ok && fwrite(&t, 1, sizeof(t), f) == sizeof(t);
    progress("Finishing the copy", 97); /* the flush, fsync, rename and folder sync: no megabyte, but time */
    ok = fflush(f) == 0 && ok;
    ok = ok && fsync(fileno(f)) == 0;
    if (!ok || rename(tmp, path) != 0) {
        fclose(f);
        remove(tmp);
        snprintf(err, errlen, "write failed (SD card full?)");
        return 0;
    }
    /* The renamed file once more, still open, before the folder (as eeprom.bin and the save states):
     * on the card's vfat (Linux 4.14 vfat_rename) the rename only marks the moved inode dirty, so its
     * directory slot (made with cluster 0 for a new name, or the old image's slot) gets the start
     * cluster and size at that inode's writeback, ~30 s later, while the folder's fsync already writes
     * the FAT. A power cut in that window left sf64.rom empty or on freed clusters, its ~16 MB of
     * clusters orphaned, and the next start built it again (super-smash-bros cd5128c, diddy-kong-racing 9c91d40).
     * The close moved after the rename: its result never mattered after a successful fflush and fsync. */
    progress("Finishing the copy", 98);
    fsync(fileno(f));
    fclose(f);
    dir = open(port_save_dir(), O_RDONLY);
    if (dir >= 0) { fsync(dir); close(dir); }
    return 1;
}

/* An existing image made from these recipes (trailer check only: the full
 * CRC is verified when the image is built). */
static int image_current(const char* path, u32 recipe_crc) {
    ImageTrailer t;
    FILE* f = fopen(path, "rb");
    long n;
    int ok;
    if (f == NULL) return 0;
    ok = fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > (long) sizeof(t) &&
         fseek(f, n - (long) sizeof(t), SEEK_SET) == 0 && fread(&t, 1, sizeof(t), f) == sizeof(t) &&
         memcmp(t.magic, "SF64IMG1", 8) == 0 && t.recipe_crc == recipe_crc &&
         (long) t.image_size + (long) sizeof(t) == n;
    fclose(f);
    return ok;
}

/* The image's usable size (without the trailer); 0 if it has none. */
u32 port_romimage_payload_size(u32 file_size, const u8* tail) {
    ImageTrailer t;
    if (file_size < sizeof(t)) return 0;
    memcpy(&t, tail, sizeof(t));
    if (memcmp(t.magic, "SF64IMG1", 8) != 0 || t.image_size + sizeof(t) != file_size) return 0;
    return t.image_size;
}

/* The game's folder, with its '/': argv0's when it names one (a qemu run through the loader makes
 * /proc/self/exe the loader), else /proc/self/exe's (a launcher may exec the bare name). */
static void exe_dir_of(const char* argv0, char* out, size_t outlen) {
    out[0] = 0;
    if (argv0 != NULL) {
        const char* slash = strrchr(argv0, '/');
        if (slash != NULL && (size_t) (slash + 1 - argv0) < outlen) {
            memcpy(out, argv0, (size_t) (slash + 1 - argv0));
            out[slash + 1 - argv0] = 0;
        }
    }
    if (!out[0]) { /* a launcher may exec the bare name */
        char self[512];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            char* s;
            self[n] = 0;
            s = strrchr(self, '/');
            if (s != NULL) { s[1] = 0; snprintf(out, outlen, "%s", self); }
        }
    }
}

/* Make sure sf64.rom exists and matches this build's recipes. Returns 1 when
 * the image is ready (port_rom_open maps it). SF64_ROM=<image> skips all of
 * this (desktop runs use the locally built image). */
int port_romimage_ensure(const char* argv0) {
    char exe_dir[512], rpath[600], rom[512], why[160], err[160], rb[600];
    u8 *recipes, *romdata, *img;
    u32 rlen, romlen, imglen, recipe_crc;
    const u32 t0 = port_time_us();
    const char* image = port_save_path(IMAGE_NAME);

    if (getenv("SF64_ROM") && getenv("SF64_ROM")[0]) return 1;
    exe_dir_of(argv0, exe_dir, sizeof(exe_dir));
    snprintf(rpath, sizeof(rpath), "%s%s", exe_dir, RECIPE_NAME);
    recipes = file_load(rpath, &rlen);
    if (recipes == NULL) {
        fail_screen("This build is missing", RECIPE_NAME ".", "Reinstall the game.");
        return 0;
    }
    recipe_crc = crc32_update(0, recipes, rlen);
    /* <save dir>/rebuild (the player's, any content): do the first start's job again. sf64.rom is not trusted; it is
     * replaced (tmp, fsync, rename) only by an image made from a ROM that checks out, so with no ROM it stays, and so
     * does the file, which is removed once the new image is on the card. */
    snprintf(rb, sizeof(rb), "%srebuild", port_save_dir());
    const int rebuild = access(rb, F_OK) == 0;
    if (rebuild) port_log_always("romimage: %s found, the first start's job runs again\n", rb);
    else if (image_current(image, recipe_crc)) { free(recipes); return 1; }

    ui_begin();
    progress("Looking for your ROM", 0);
    if (!find_rom(exe_dir, rom, sizeof(rom), why, sizeof(why))) {
        if (why[0]) port_log_always("romimage: rejected: %s\n", why);
        if (sStarFoxVer) fail_screen("No usable Star Fox 64 ROM.", sStarFoxVer, "It must be the US (NTSC) 1.1 cart.");
        if (sStarFoxSeen) fail_screen("No usable Star Fox 64 ROM.", "It must be the US (NTSC) 1.1 cart.", NULL);
        fail_screen("No Star Fox 64 ROM found.", "Copy your US (NTSC) 1.1 .z64/.n64/.v64", "to the SD card and start again.");
    }
    { /* which ROM, in the line every port logs */
        char pkg_rom[600];
        snprintf(pkg_rom, sizeof(pkg_rom), "%s%s", exe_dir, PKG_ROM_NAME);
#if defined(MIYOO)
        port_log_always("rom: %s%s\n", rom, exe_dir[0] && !strcmp(rom, pkg_rom) ? " (next to the game)" : "");
#else
        port_log_always("rom: %s%s\n", rom, exe_dir[0] && !strcmp(rom, pkg_rom) ? " (in the game folder)" : "");
#endif
    }
    romdata = sRomData; /* loaded and checked by find_rom */
    romlen = sRomLen;
    sRomData = NULL;
    progress("Extracting", 10);
    img = decompress_rom(romdata, romlen, &imglen, err, sizeof(err));
    free(romdata);
    if (img == NULL) fail_screen("Cannot extract the game data:", err, NULL);
    if (!apply_recipes(img, imglen, recipes, rlen)) fail_screen("Cannot extract the game data:", RECIPE_NAME " is corrupt.", NULL);
    free(recipes);
    progress("Saving", 90);
    if (!write_image(image, img, imglen, recipe_crc, err, sizeof(err))) fail_screen("Cannot save the game data:", err, NULL);
    free(img);
    port_log_always("romimage: built %s (%u bytes) from %s in %u ms\n", image, imglen, rom, (port_time_us() - t0) / 1000);
    if (rebuild) {
        if (remove(rb) == 0) port_log_always("romimage: rebuild done, %s removed\n", rb);
        else port_log_always("romimage: rebuild done, but %s can't be removed: the job runs again next start\n", rb);
    }
    progress("Done", 100); /* the image is on the card: the last of the first start */
    if (getenv("SF64_SPLASH_DUMP") == NULL && getenv("SF64_FAST") == NULL) usleep(600 * 1000);
    ui_black();
    ui_end(0);
    return 1;
}
