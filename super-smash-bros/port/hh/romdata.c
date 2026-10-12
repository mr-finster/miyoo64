/* romdata.c -- find the player's ROM, keep one checked slice of it, serve reads.
 * See romdata.h. The slice file is written atomically (tmp, fsync, rename) so
 * a power cut during first boot leaves either nothing or a complete file. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "romdata.h"
#include "port_log.h"
extern void port_log_always(const char *fmt, ...);
#include "bs/vpk0.h"

#define ROM_SIZE        0x01000000u     /* 16 MiB */
#define SLICE_MAGIC     "SSB64RD1"
#define SLICE_NAME      "romdata.bin"

typedef struct SliceHeader {
    char magic[8];
    uint32_t start, end;
    char sha1[40];
    uint8_t pad[8];
} SliceHeader;  /* 64 bytes */

static const uint8_t *sSlice;   /* ROM byte at address a is sSlice[a - START] */
static int sSliceFixed;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ---- SHA-1 (FIPS 180-4), small and self-contained ---- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } Sha1;
static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static void sha1_block(Sha1 *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | p[i*4+1] << 16 | p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (int i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6; }
        t = rol(a, 5) + f + e + k + w[i]; e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(Sha1 *c)
{
    static const uint32_t h0[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(c->h, h0, sizeof h0); c->len = 0; c->n = 0;
}
static void sha1_update(Sha1 *c, const uint8_t *p, size_t n)
{
    c->len += n;
    while (n--) { c->buf[c->n++] = *p++; if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; } }
}
static void sha1_hex(Sha1 *c, char out[41])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0, lb[8];
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &z, 1);
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) sprintf(out + i * 8, "%08x", c->h[i]);
}

/* ---- first-start / failure screens (mario-kart-64's layout) ---- */
#if defined(MIYOO)
/* The Miyoo Mini: the same screens in the kit's 8x8 font (miyoo_menu.c's miyoo_text) on a 640x480
 * surface, shown centred and turned on the panel (miyoo_panel_show_full). No SDL_ttf on the Mini. */
#include <SDL/SDL.h>
#include "miyoo_panel.h"
#define MW 640
#define MH 480
static SDL_Surface *sScr;
#define SUB_DEFAULT "Looking on the SD card"
static char sSub[64] = SUB_DEFAULT;
static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "romdata: SDL_Init: %s\n", SDL_GetError()); return; }
    if (!miyoo_panel_open()) return;
    SDL_ShowCursor(SDL_DISABLE);
    sScr = SDL_CreateRGBSurface(SDL_SWSURFACE, MW, MH, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}
/* centred; a line wider than the screen (a long ROM file name) keeps its start and ends in "..." */
static void ui_text(int y, const char *s, int k, int gray) {
    char fit[96];
    if (!sScr || !s || !s[0]) return;
    snprintf(fit, sizeof(fit), "%s", s);
    while (fit[0] && miyoo_text_w(fit, k) > MW - 24) {
        size_t n = strlen(fit);
        if (n >= 4 && strcmp(fit + n - 3, "...") == 0) n -= 3;
        if (n == 0) break;
        snprintf(fit + n - 1, sizeof(fit) - (n - 1), "...");
    }
    miyoo_text(sScr, (MW - miyoo_text_w(fit, k)) / 2, y, k, fit, 0xFF000000u | (uint32_t)gray * 0x010101u);
}
static void ui_fill(int x, int y, int w, int h, int gray) {
    SDL_Rect r = { (Sint16) x, (Sint16) y, (Uint16) w, (Uint16) h };
    SDL_FillRect(sScr, &r, SDL_MapRGB(sScr->format, (Uint8) gray, (Uint8) gray, (Uint8) gray));
}
/* title, up to three body lines, a progress bar (pct < 0: none), a footer (the handheld screen's layout, x2) */
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    if (!sScr) return;
    ui_fill(0, 0, MW, MH, 0);
    ui_text(56, "SUPER SMASH BROS.", 4, 255);
    ui_text(112, sSub, 2, 150);
    ui_text(176, l1, 2, 230);
    ui_text(206, l2, 2, 230);
    ui_text(236, l3, 2, 230);
    if (pct >= 0) {
        char p[8];
        ui_fill(100, 300, 440, 24, 70);
        ui_fill(104, 304, 432 * pct / 100, 16, 255);
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(340, p, 2, 200);
    }
    ui_text(420, foot, 2, 150);
    {   /* SSB64_SPLASH_DUMP=<dir>: the screen as a PPM (tests) */
        const char *dd = getenv("SSB64_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256];
            static int seq;
            FILE *f;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            if ((f = fopen(path, "wb")) != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", MW, MH);
                for (int y = 0; y < MH; y++)
                    for (int x = 0; x < MW; x++) {
                        const uint32_t v = ((const uint32_t *) ((const uint8_t *) sScr->pixels + y * sScr->pitch))[x];
                        const uint8_t px[3] = { (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
                        fwrite(px, 1, 3, f);
                    }
                fclose(f);
            }
        }
    }
    miyoo_panel_show_full(sScr);
}
static void ui_wait_button(void) {
    SDL_Event ev;
    if (!sScr || getenv("SSB64_FRAMES")) return; /* headless test run: never block */
    while (SDL_PollEvent(&ev)) { } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    ui_fill(0, 0, MW, MH, 0);
    miyoo_panel_show_full(sScr);
    miyoo_panel_show_full(sScr); /* a double buffer's other page too */
}
/* done with the screen: SDL stays up for gfx_init (quit == exiting) */
static void ui_end(int quit) {
    if (sScr) { SDL_FreeSurface(sScr); sScr = NULL; }
    if (quit) SDL_Quit();
}
#else
#define SUB_DEFAULT "Looking on the SD card"
static char sSub[64] = SUB_DEFAULT;
static void ui_begin(void) { }
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    if (pct >= 0) printf("romdata: %s %s %s %d%%\n", l1, l2 ? l2 : "", l3 ? l3 : "", pct);
    else printf("romdata: %s | %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", foot ? foot : "");
}
static void ui_wait_button(void) { }
static void ui_black(void) { }
static void ui_end(int quit) { (void) quit; }
#endif

static const char *sStage = "Finding the ROM";
static int sLastPct = -1;
static const char *sLastStage;
static char sLastSub[64];
static void screen_progress(const char *what, int pct) {
    if (what) sStage = what;
    if (pct == sLastPct && sStage == sLastStage && strcmp(sSub, sLastSub) == 0) return;
    sLastPct = pct;
    sLastStage = sStage;
    snprintf(sLastSub, sizeof(sLastSub), "%s", sSub);
    ui_draw("First start: building the game", "data from your ROM.", sStage, pct, "This happens once.");
}

static void fail_screen(const char *l1, const char *l2, const char *l3) {
    ui_begin();
    port_log_always("SSB64: FAILED: %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    fprintf(stderr, "SSB64: %s %s %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}

static void set_sub(const char *path) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(sSub, sizeof(sSub), "%.60s", base);
}

/* Why the last Super Smash Bros. dump was refused (NULL: none was), for the no-ROM screen; sWhy: why
 * the last file try_rom opened was, for the log. B9 (2026-10-01; diddy-kong-racing 3730150, mario-kart-64
 * efd9b8a, F-Zero X aefb4d2): the search opens every .bin/.z64/... two folders deep on the card, and
 * any refusal turned "No Super Smash Bros. ROM found." into "No usable Super Smash Bros. ROM." with
 * that file's reason -- the other ports' N64 ROMs ("It is not a 16 MB N64 ROM.", or for a 16 MB one
 * such as Diddy Kong Racing or F-Zero X "It must be the US (NTSC) cart.") and other systems' 16 MB
 * .bin games. Only a file whose header says Smash (header_is_smash) sets sReject now. */
static const char *sReject, *sWhy;
/* The version a refused Smash dump is, by its header's country code (E USA, J Japan, P Europe,
 * U Australia), as OoT, DKR and GoldenEye name it on the screen; NULL: the US one, or not Smash. */
static const char *sRejectVer, *sCurVer;
static const char *version_say(uint8_t c)
{
    return c == 'E' ? NULL : c == 'P' ? "It is the European (PAL) version." :
           c == 'J' ? "It is the Japanese version." : c == 'U' ? "It is the Australian (PAL) version." :
           "It is not the US version.";
}

/* Normalises any of the three N64 dump byte orders to .z64 (big-endian). */
static int normalise_rom(uint8_t *rom, size_t size)
{
    uint32_t magic = (uint32_t)rom[0] << 24 | rom[1] << 16 | rom[2] << 8 | rom[3];
    if (magic == 0x80371240u) return 0;                     /* .z64 */
    if (magic == 0x37804012u) {                             /* .v64: swap byte pairs */
        for (size_t i = 0; i + 1 < size; i += 2) { uint8_t t = rom[i]; rom[i] = rom[i+1]; rom[i+1] = t; }
        return 0;
    }
    if (magic == 0x40123780u) {                             /* .n64: reverse each word */
        for (size_t i = 0; i + 3 < size; i += 4) {
            uint8_t a = rom[i], b = rom[i+1]; rom[i] = rom[i+3]; rom[i+1] = rom[i+2]; rom[i+2] = b; rom[i+3] = a;
        }
        return 0;
    }
    return -1;
}

/* Smash's cart header, in .z64 order: the game code "AL" at 0x3C (NUS-NAL*, every region) or
 * "SMASH" in the 20-byte title at 0x20. */
static int header_is_smash(const uint8_t *h)
{
    if (h[0x3C] == 'A' && h[0x3D] == 'L') return 1;
    for (int i = 0x20; i + 5 <= 0x34; i++)
        if (memcmp(h + i, "SMASH", 5) == 0) return 1;
    return 0;
}

static uint8_t *refuse(int smash, const char *why)
{
    sWhy = why;
    if (smash) { sReject = why; sRejectVer = sCurVer; }   /* the screen's reason (any other file: the log only) */
    return NULL;
}

/* Loads `path` if it is exactly a 16 MiB US Smash ROM; returns malloc'd z64 bytes. The header is read
 * first (64 bytes), so a refusal knows whether the file was a Smash dump at all. */
static uint8_t *try_rom(const char *path, char sha_out[41])
{
    struct stat st;
    uint8_t hdr[0x40];
    sWhy = NULL;
    sCurVer = NULL;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return refuse(0, "It could not be read.");
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || normalise_rom(hdr, sizeof hdr) != 0) {
        fclose(f);
        return refuse(0, "It is not an N64 ROM image.");
    }
    const int smash = header_is_smash(hdr);
    if (smash) sCurVer = version_say(hdr[0x3E]);
    if (st.st_size != ROM_SIZE) { fclose(f); return refuse(smash, "It is not a 16 MB N64 ROM."); }
    set_sub(path);
    uint8_t *rom = malloc(ROM_SIZE);
    screen_progress("Reading the ROM", 1);
    if (rom && fseek(f, 0, SEEK_SET) != 0) { free(rom); rom = NULL; }
    for (size_t got = 0; rom && got < ROM_SIZE; ) { /* 1 MB steps, the bar 1..4 */
        const size_t c = ROM_SIZE - got > 0x100000 ? 0x100000 : ROM_SIZE - got;
        if (fread(rom + got, 1, c, f) != c) { free(rom); rom = NULL; break; }
        got += c;
        screen_progress("Reading the ROM", 1 + (int)((uint64_t)got * 3 / ROM_SIZE));
    }
    fclose(f);
    if (!rom || normalise_rom(rom, ROM_SIZE) != 0) {
        free(rom); snprintf(sSub, sizeof sSub, SUB_DEFAULT); return refuse(smash, "It could not be read.");
    }
    Sha1 c; sha1_init(&c);
    for (size_t at = 0; at < ROM_SIZE; ) { /* 1 MB steps, the bar 4..10 */
        const size_t n = ROM_SIZE - at > 0x100000 ? 0x100000 : ROM_SIZE - at;
        sha1_update(&c, rom + at, n);
        at += n;
        screen_progress("Checking the ROM", 4 + (int)((uint64_t)at * 6 / ROM_SIZE));
    }
    sha1_hex(&c, sha_out);
    port_log_always("SSB64: ROM candidate %s sha1=%s\n", path, sha_out);
    if (strcmp(sha_out, ROMDATA_US_SHA1) != 0) {
        free(rom); snprintf(sSub, sizeof sSub, SUB_DEFAULT); return refuse(smash, "It must be the US (NTSC) cart.");
    }
    return rom;
}

static int has_rom_ext(const char *n)
{
    const char *d = strrchr(n, '.');
    if (!strcmp(n, SLICE_NAME)) return 0;   /* our own slice, not a cart image */
    return d && (!strcasecmp(d, ".z64") || !strcasecmp(d, ".n64") || !strcasecmp(d, ".v64") ||  /* the three byte orders */
                 !strcasecmp(d, ".rom") || !strcasecmp(d, ".bin") || !strcasecmp(d, ".u64") ||  /* generic cart images */
                 !strcasecmp(d, ".usa") || !strcasecmp(d, ".u1"));                               /* region-suffixed dumps */
}

/* Walks `dir` up to `depth` levels for a ROM-named file that is the US ROM.
 * Hidden folders are skipped except the port's own save dir. */
static uint8_t *scan_tree(const char *dir, int depth, char *found, size_t found_size, char sha[41])
{
    DIR *d = opendir(dir);
    struct dirent *e;
    uint8_t *rom = NULL;
    char path[512];
    if (!d) return NULL;
    while (rom == NULL && (e = readdir(d)) != NULL) {
        struct stat st;
        if (e->d_name[0] == '.' && strcmp(e->d_name, ".ssb64") != 0) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) rom = scan_tree(path, depth - 1, found, found_size, sha);
        } else if (has_rom_ext(e->d_name)) {
            if ((rom = try_rom(path, sha)) != NULL) snprintf(found, found_size, "%s", path);
            else port_log_always("SSB64: skipping %s (%s)\n", path, sWhy ? sWhy : "not the US ROM");
        }
    }
    closedir(d);
    return rom;
}

/* rom.z64 next to the executable (App/SuperSmashBros/rom.z64: the auto-ROM a personal zip carries,
 * .z64 order). The launcher may exec the bare name, so argv[0] can have no folder: the file is the
 * folder of /proc/self/exe + "/rom.z64". A desktop build looks in its build folder, which has no rom.z64. */
#define PKG_ROM_NAME "rom.z64"
static int sRomInPkg;   /* the ROM in use is the rom.z64 next to the executable (the log line says so) */

static int pkg_rom_path(char *out, size_t out_size)
{
    char self[512];
    char *slash;
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0 || (size_t)n >= sizeof self - 1) return -1;   /* a full buffer may be cut short */
    self[n] = '\0';
    if ((slash = strrchr(self, '/')) == NULL) return -1;
    *slash = '\0';
    return snprintf(out, out_size, "%s/%s", self, PKG_ROM_NAME) < (int)out_size ? 0 : -1;
}

/* $SSB64_ROM (a test override; nothing sets it on the device), then rom.z64 next to the executable, then the
 * save dir, then the card. That copy gets a card ROM's checks (size, header, SHA-1) and is only
 * read (the slice goes to the save dir as before); missing or refused, the card search below runs
 * unchanged. The card search walks the Miyoo's ROM roots two folders deep, so a ROM in e.g.
 * Roms/N64/ is found. */
static uint8_t *find_rom(const char *save_dir, char *found, size_t found_size, char sha[41])
{
    const char *env = getenv("SSB64_ROM");
    char pkg[512];
    uint8_t *rom;
    sRomInPkg = 0;
    if (env && (rom = try_rom(env, sha))) { snprintf(found, found_size, "%s", env); return rom; }
    if (pkg_rom_path(pkg, sizeof pkg) == 0) {
        if ((rom = try_rom(pkg, sha)) != NULL) {
            snprintf(found, found_size, "%s", pkg);
            sRomInPkg = 1;
            return rom;
        }
        if (sWhy) port_log_always("SSB64: skipping %s (%s)\n", pkg, sWhy);   /* present but refused */
    }
    if ((rom = scan_tree(save_dir, 0, found, found_size, sha))) return rom;
#if defined(MIYOO)
    {
        /* The Miyoo Mini: Roms/N64, then the whole card, two folders deep each (kit miyoo_panel.c's
         * miyoo_rom_roots). SSB64_CARD=<dir>: one test root instead (qemu runs). */
        const char *card = getenv("SSB64_CARD");
        if (card != NULL) {
            if ((rom = scan_tree(card, 2, found, found_size, sha))) return rom;
        } else {
            for (int i = 0; miyoo_rom_roots[i] != NULL; i++)
                if ((rom = scan_tree(miyoo_rom_roots[i], 2, found, found_size, sha))) return rom;
        }
    }
#endif
    return scan_tree(".", 1, found, found_size, sha);
}

/* tmp, fsync, rename; then (2026-10-01, OPEN_ISSUES S4, as sram.bin in cd5128c) the file is fsynced
 * again after the rename, then the folder. On the card's vfat (Linux 4.14 vfat_rename) the renamed
 * file's directory slot is only marked dirty and reaches the card at the inode's writeback, ~30 s
 * later, so a hard power cut in that time left romdata.bin missing or empty (the next start extracts
 * again) and the 14 MB .tmp chain as lost clusters until a fsck. fsync on the still-open file writes
 * the slot. SSB64_DIRSYNC=0: neither sync after the rename (A/B, as sram.bin). A failure leaves errno
 * set for the caller's message: a short write (a full card) sets none, so it reads ENOSPC; the
 * progress screen between the writes no longer clobbers it. */
static int write_slice(const char *path, const uint8_t *rom, const char *sha)
{
    char tmp[600], dir[600];
    SliceHeader h;
    int err = 0;
    ssize_t w;
    const char *e = getenv("SSB64_DIRSYNC");
    const int sync_rename = !(e && e[0] == '0');
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    memset(&h, 0, sizeof h);
    memcpy(h.magic, SLICE_MAGIC, 8);
    h.start = ROMDATA_SLICE_START;
    h.end = ROMDATA_SLICE_END;
    memcpy(h.sha1, sha, 40);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const size_t n = ROMDATA_SLICE_END - ROMDATA_SLICE_START, chunk = 512 * 1024;
    w = write(fd, &h, sizeof h);
    if (w != (ssize_t)sizeof h) err = w < 0 ? errno : ENOSPC;
    for (size_t off = 0; !err && off < n; off += chunk) {
        size_t len = n - off < chunk ? n - off : chunk;
        w = write(fd, rom + ROMDATA_SLICE_START + off, len);
        if (w != (ssize_t)len) err = w < 0 ? errno : ENOSPC;
        screen_progress("Copying the game data", 10 + (int)((off + len) * 80 / n));
    }
    screen_progress("Saving", 90);
    if (!err && fsync(fd) != 0) err = errno;
    screen_progress("Finishing the copy", 96);
    if (!err && rename(tmp, path) != 0) err = errno;
    if (err) { close(fd); unlink(tmp); errno = err; return -1; }
    /* The slice is in place under its name from here on; a failed sync below only leaves it to the
     * kernel's writeback, as before. */
    if (sync_rename && fsync(fd) != 0) port_log_always("SSB64: romdata.bin fsync after the rename failed: %s\n", strerror(errno));
    close(fd);
    screen_progress("Finishing the copy", 98);
    if (sync_rename) {
        char *slash;
        snprintf(dir, sizeof dir, "%s", path);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
            fd = open(dir[0] ? dir : "/", O_RDONLY);
            if (fd < 0 || fsync(fd) != 0) port_log_always("SSB64: romdata.bin folder fsync failed: %s\n", strerror(errno));
            if (fd >= 0) close(fd);
        }
    }
    return 0;
}

static int map_slice(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    size_t want = sizeof(SliceHeader) + (ROMDATA_SLICE_END - ROMDATA_SLICE_START);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != want) { close(fd); return -1; }
    /* A fixed address: parsed audio banks and loaded files keep pointers into
     * the slice, and a save state restored in a later run must find it at the
     * same place. MAP_FIXED_NOREPLACE fails rather than clobber a mapping. */
    void *m = mmap((void *)ROMDATA_MAP_ADDR, want, PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (m == MAP_FAILED || m != (void *)ROMDATA_MAP_ADDR) {
        if (m != MAP_FAILED) munmap(m, want);
        port_log("SSB64: ROM slice could not map at 0x%08X; save states disabled\n", ROMDATA_MAP_ADDR);
        sSliceFixed = 0;
        m = mmap(NULL, want, PROT_READ, MAP_SHARED, fd, 0);
    } else {
        sSliceFixed = 1;
    }
    close(fd);
    if (m == MAP_FAILED) return -1;
    const SliceHeader *h = m;
    if (memcmp(h->magic, SLICE_MAGIC, 8) != 0 || h->start != ROMDATA_SLICE_START ||
        h->end != ROMDATA_SLICE_END || memcmp(h->sha1, ROMDATA_US_SHA1, 40) != 0) {
        munmap(m, want);
        return -1;
    }
    sSlice = (const uint8_t *)m + sizeof(SliceHeader);
    return 0;
}

int romdata_init(const char *save_dir, char *err, size_t err_size)
{
    char path[512], found[512], sha[41], rb[512];
    snprintf(path, sizeof path, "%s/%s", save_dir, SLICE_NAME);
    /* <save dir>/rebuild (the player's, any content): do the first start's job again. romdata.bin is not read; it is
     * replaced (tmp, fsync, rename) only by a slice made from a ROM that checks out, so with no ROM it stays, and so
     * does the file, which is removed once the new slice is saved and mapped. */
    snprintf(rb, sizeof rb, "%s/rebuild", save_dir);
    const int rebuild = access(rb, F_OK) == 0;
    if (rebuild) port_log_always("SSB64: %s found, the first start's job runs again\n", rb);
    else if (map_slice(path) == 0) return 0;

    /* First start (or a bad slice): build it behind mk64's progress screen. */
    ui_begin();
    screen_progress("Finding the ROM", 0);
    sReject = NULL;
    sRejectVer = NULL;
    uint8_t *rom = find_rom(save_dir, found, sizeof found, sha);
    if (!rom) {
        snprintf(err, err_size, "No Super Smash Bros. (USA) ROM found on the card (searched 2 folders deep)");
        sSub[0] = 0; /* the error screens carry no name line */
        if (sReject) {
            port_log_always("SSB64: rejected: %s%s%s\n", sReject, sRejectVer ? " " : "", sRejectVer ? sRejectVer : "");
            if (sRejectVer) fail_screen("No usable Super Smash Bros. ROM.", sRejectVer, "It must be the US (NTSC) cart.");
            fail_screen("No usable Super Smash Bros. ROM.", sReject, NULL);
        }
        fail_screen("No Super Smash Bros. ROM found.", "Copy your US (NTSC) .z64/.n64/.v64", "to the SD card and start again.");
        return -1;
    }
    port_log_always("SSB64: using ROM %s%s\n", found, sRomInPkg ? " (rom.z64 next to the game)" : "");
    set_sub(found);
    int rc = write_slice(path, rom, sha);
    int saved_errno = errno;
    free(rom);
    if (rc != 0 || map_slice(path) != 0) {
        snprintf(err, err_size, "Could not write %s (%s)", path, strerror(saved_errno));
        port_log_always("SSB64: %s\n", err);
        fail_screen("Cannot save the game data:", strerror(saved_errno), "Is the SD card full?");
        return -1;
    }
    if (rebuild) {
        if (remove(rb) == 0) port_log_always("SSB64: rebuild done, %s removed\n", rb);
        else port_log_always("SSB64: rebuild done, but %s can't be removed: the job runs again next start\n", rb);
    }
    screen_progress("Done", 100); /* the slice is saved and mapped: the last of the first start */
    if (getenv("SSB64_SPLASH_DUMP") == NULL && getenv("SSB64_FRAMES") == NULL) usleep(600 * 1000);
    ui_black();
    ui_end(0);   /* SDL stays up; gfx_init sets its own video mode next */
    return 0;
}

const uint8_t *romdata_ptr(uint32_t rom, uint32_t size)
{
    if (!sSlice || rom < ROMDATA_SLICE_START || rom > ROMDATA_SLICE_END ||
        size > ROMDATA_SLICE_END - rom) return NULL;
    return sSlice + (rom - ROMDATA_SLICE_START);
}

int romdata_read(uint32_t rom, void *dst, uint32_t size)
{
    const uint8_t *p = romdata_ptr(rom, size);
    if (!p) {
        port_log("SSB64: ROM read out of range: 0x%08X+0x%X\n", (unsigned)rom, (unsigned)size);
        return -1;
    }
    memcpy(dst, p, size);
    return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Table layout (lbtypes.h LBTableEntry, 12 bytes, one sentinel after the last):
 * u32 is_compressed:1 | data_offset:31, u16 reloc_intern, u16 compressed_words,
 * u16 reloc_extern, u16 decompressed_words. Data offsets are relative to the
 * end of the table; each file's extern id list follows its (compressed) data. */
static const uint8_t *entry(uint32_t id)
{
    return romdata_ptr(ROMDATA_RELOC_TABLE + id * ROMDATA_RELOC_ENTRY, ROMDATA_RELOC_ENTRY * 2);
}
#define RELOC_DATA_START (ROMDATA_RELOC_TABLE + (ROMDATA_RELOC_COUNT + 1) * ROMDATA_RELOC_ENTRY)

int romdata_reloc_info(uint32_t id, RomRelocInfo *info)
{
    const uint8_t *e = (id < ROMDATA_RELOC_COUNT) ? entry(id) : NULL;
    if (!e) return -1;
    uint32_t off = be32(e) & 0x7FFFFFFF, next = be32(e + 12) & 0x7FFFFFFF;
    uint32_t csize = (uint32_t)be16(e + 6) * 4;
    info->file_id = id;
    info->reloc_intern = be16(e + 4);
    info->reloc_extern = be16(e + 8);
    info->size = (uint32_t)be16(e + 10) * 4;
    info->extern_count = (next > off + csize) ? (next - off - csize) / 2 : 0;
    info->extern_ids = romdata_ptr(RELOC_DATA_START + off + csize, info->extern_count * 2);
    if (info->extern_count && !info->extern_ids) return -1;
    return 0;
}

int romdata_reloc_read(uint32_t id, void *dst, uint32_t dst_size)
{
    const uint8_t *e = (id < ROMDATA_RELOC_COUNT) ? entry(id) : NULL;
    if (!e) return -1;
    uint32_t w = be32(e), off = w & 0x7FFFFFFF;
    uint32_t csize = (uint32_t)be16(e + 6) * 4, dsize = (uint32_t)be16(e + 10) * 4;
    const uint8_t *src = romdata_ptr(RELOC_DATA_START + off, csize);
    if (!src || dst_size < dsize) return -1;
    if (w >> 31) {
        if (vpk0_decode(src, csize, dst, dsize) == 0) {
            port_log("SSB64: vpk0 decode failed for reloc file %u\n", (unsigned)id);
            return -1;
        }
    } else {
        memcpy(dst, src, dsize);
    }
    return 0;
}

int romdata_at_fixed_address(void)
{
    return sSliceFixed;
}
