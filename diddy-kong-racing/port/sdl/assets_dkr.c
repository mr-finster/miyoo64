/**
 * Game data for the handheld / desktop-proxy build.
 *
 * The executable carries none of the game's data. On the N64 every asset is
 * read with PI DMA from the cartridge's asset region (the LUT at 0xED0E0 up to
 * 0xB8CFD0 in us.v80); the port serves those reads from dkr_assets.bin, a
 * verbatim copy of that region taken from the player's own ROM:
 *
 *   1. dkr_assets.bin in the save dir, written by an earlier first start;
 *   2. otherwise find the ROM (DKR_ROM=<path>, rom.z64 in the game's folder, exe dir,
 *      save dir, the SD card),
 *      in any byte order, check its SHA-1 is Diddy Kong Racing US 1.1, copy the
 *      region out (tmp, fsync, rename) and use it.
 *
 * The file is mmap'd read-only: it costs page cache, not process memory. The
 * data is copied as-is (big-endian); the game code byte-swaps each asset as it
 * loads it (port/asset_swap.c).
 */
#include <PR/ultratypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "port.h"

#define CACHE_NAME "dkr_assets.bin"
#if defined(MIYOO)
/* Miyoo Mini: the card is /mnt/SDCARD; Onion and the stock firmware keep N64 ROMs in Roms/N64. */
#define SD_CARD    "/mnt/SDCARD/"
#define SD_HOME    "/mnt/SDCARD/Roms/N64/"
#else
#define SD_CARD    "/mnt/"
#define SD_HOME    "/mnt/"
#endif

/* us.v80 (Diddy Kong Racing US 1.1, Rev 1). */
#define ROM_SIZE        0xC00000u
#define REGION_START    ((u32) PORT_ASSETS_LUT_START)
#define REGION_END      ((u32) PORT_ASSETS_END)
#define REGION_SIZE     (REGION_END - REGION_START)
static const char kRomSha1[]    = "6d96743d46f8c0cd0edb0ec5600b003c89b93755";
static const char kRegionSha1[] = "7c3b1d346c29774e9c30e4cb897b525f3833b1b5";

static const u8 *sRegion;   /* mmap of dkr_assets.bin */
u32 gPortRomBytes; /* not game state: outside the save ranges (port file, not in gamestate.ld) */
static u32 sRegionSize;

/* ------------------------------------------------------------------ SHA-1 */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } Sha1;
#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 | (uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0; s->n = 0;
}
static void sha1_update(Sha1 *s, const uint8_t *p, uint32_t n) {
    s->len += n;
    while (n > 0) {
        uint32_t c = 64 - s->n; if (c > n) c = n;
        memcpy(s->buf + s->n, p, c); s->n += c; p += c; n -= c;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}
static void sha1_hex(Sha1 *s, char out[41]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, lb[8];
    int i;
    sha1_update(s, &pad, 1);
    while (s->n != 56) sha1_update(s, &z, 1);
    for (i = 0; i < 8; i++) lb[i] = (uint8_t) (bits >> (56 - 8 * i));
    sha1_update(s, lb, 8);
    for (i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", (unsigned) s->h[i]);
}

/* ------------------------------------------------------------------ ROM */
/* 0 = z64 (big-endian), 1 = v64 (byte-swapped pairs), 2 = n64 (little-endian words) */
static void fix_order(u8 *b, u32 n, int mode) {
    u32 i;
    if (mode == 1) {
        for (i = 0; i + 1 < n; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; }
    } else if (mode == 2) {
        for (i = 0; i + 3 < n; i += 4) { u8 t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; }
    }
}

/* B9 (the matrix; Star Fox 4e9483c, MK64 efd9b8a, F-Zero X aefb4d2): set by rom_load when the file's
 * header says Diddy Kong Racing. Only such a dump's refusal (the wrong version or region, a bad dump)
 * reaches the screen; the search also opens every other .bin/.z64/... on the card -- other ports' N64
 * ROMs, other systems' .bin games, eeprom.bin -- and the last of those used to turn "No Diddy Kong
 * Racing ROM found." into "No usable ... It must be the US 1.1 (Rev 1) cart." with that file's reason. */
static int sRomIsDkr;
static void screen_progress(const char *what, int pct);
static char sSub[64] = "Looking on the SD card"; /* the line under the title: this, then the ROM file name */

/* SHA-1 of a buffer in 1 MB steps, the bar moving from lo to hi under the stage (first-start screen) */
static void sha1_screen(Sha1 *s, const u8 *p, uint32_t n, int lo, int hi) {
    uint32_t done = 0;
    while (done < n) {
        uint32_t c = n - done > 0x100000u ? 0x100000u : n - done;
        sha1_update(s, p + done, c);
        done += c;
        screen_progress("Checking the ROM", lo + (int) ((u64) done * (uint32_t) (hi - lo) / n));
    }
}

/* Reads a whole ROM into memory as big-endian. Quick header checks first, so
 * random files on the card are rejected without reading 12 MB. */
static u8 *rom_load(const char *path, char *err, u32 errlen) {
    FILE *f = fopen(path, "rb");
    u8 hdr[0x40];
    int mode;
    long size;
    u8 *rom;
    sRomIsDkr = 0;
    if (!f) { snprintf(err, errlen, "cannot open"); return NULL; }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); snprintf(err, errlen, "unreadable"); return NULL; }
    if (hdr[0] == 0x80 && hdr[1] == 0x37) mode = 0;
    else if (hdr[0] == 0x37 && hdr[1] == 0x80) mode = 1;
    else if (hdr[0] == 0x40 && hdr[1] == 0x12) mode = 2;
    else { fclose(f); snprintf(err, errlen, "not an N64 ROM"); return NULL; }
    fix_order(hdr, sizeof(hdr), mode);
    if (memcmp(hdr + 0x20, "Diddy Kong Racing", 17) != 0) { fclose(f); snprintf(err, errlen, "not Diddy Kong Racing"); return NULL; }
    sRomIsDkr = 1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size != (long) ROM_SIZE) { fclose(f); snprintf(err, errlen, "wrong size (%ld bytes)", size); return NULL; }
    rom = (u8 *) malloc(ROM_SIZE);
    if (!rom) { fclose(f); snprintf(err, errlen, "out of memory"); return NULL; }
    fseek(f, 0, SEEK_SET);
    {
        const char *base = strrchr(path, '/');
        u32 got = 0;
        snprintf(sSub, sizeof(sSub), "%.60s", base ? base + 1 : path);
        while (got < ROM_SIZE) {
            u32 c = ROM_SIZE - got > 0x100000u ? 0x100000u : ROM_SIZE - got;
            if (fread(rom + got, 1, c, f) != c) { fclose(f); free(rom); snprintf(err, errlen, "short read"); return NULL; }
            got += c;
            screen_progress("Reading the ROM", 1 + (int) ((u64) got * 7 / ROM_SIZE));
        }
    }
    fclose(f);
    fix_order(rom, ROM_SIZE, mode);
    {
        Sha1 s; char hex[41];
        sha1_init(&s); sha1_screen(&s, rom, ROM_SIZE, 8, 20); sha1_hex(&s, hex);
        if (strcmp(hex, kRomSha1) != 0) {
            free(rom);
            if (hdr[0x3E] != 'E') snprintf(err, errlen, "not the US version (region '%c')", hdr[0x3E]);
            else if (hdr[0x3F] != 1) snprintf(err, errlen, "this is the US 1.0 cart"); /* the screen's next line asks for 1.1 */
            else snprintf(err, errlen, "unknown dump (sha1 %.12s...)", hex);
            return NULL;
        }
    }
    return rom;
}

static int rom_ext_ok(const char *n) {
    const char *dot = strrchr(n, '.');
    char e[4];
    size_t i, l;
    if (!dot || dot == n || (l = strlen(dot + 1)) < 2 || l > 3) return 0;
    for (i = 0; i <= l; i++) e[i] = (char) (dot[1 + i] ? (dot[1 + i] | 0x20) : 0);
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") ||   /* the three byte orders */
           !strcmp(e, "rom") || !strcmp(e, "bin") || !strcmp(e, "u64") ||   /* generic cart images */
           !strcmp(e, "usa") || !strcmp(e, "u1");                           /* region-suffixed dumps */
}

static u8 *scan_dir(const char *dir, char *path_out, u32 outlen, char *why, u32 whylen) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (!d) return NULL;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name;
        char path[512], err[96];
        u8 *rom;
        if (n[0] == '.' || !rom_ext_ok(n) || !strcmp(n, CACHE_NAME)) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", n);
        rom = rom_load(path, err, sizeof(err));
        if (rom) { closedir(d); snprintf(path_out, outlen, "%s", path); return rom; }
        if (sRomIsDkr) snprintf(why, whylen, "%s: %s", n, err); /* B9: the screen's reason (any other file: the log only) */
        port_log_always("assets: skipping %s: %s\n", path, err);
    }
    closedir(d);
    return NULL;
}

static u8 *scan_tree(const char *root, int depth, char *out, u32 outlen, char *why, u32 whylen) {
    DIR *d;
    struct dirent *ent;
    u8 *rom = scan_dir(root, out, outlen, why, whylen);
    if (rom || depth <= 0) return rom;
    d = opendir(root);
    if (!d) return NULL;
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;
        size_t rl = strlen(root);
        if (ent->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s%s%s/", root, (rl && root[rl - 1] == '/') ? "" : "/", ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if ((rom = scan_tree(sub, depth - 1, out, outlen, why, whylen)) != NULL) { closedir(d); return rom; }
    }
    closedir(d);
    return NULL;
}

/* rom.z64 in the game's folder (a zip packed with the player's ROM): the file is next to the executable -- found from
 * /proc/self/exe, not argv[0] (a launcher may exec the bare name), so it doesn't depend on the cwd. Only read
 * ("rb", as a card ROM); dkr_assets.bin still goes to the save dir and records no ROM path. Not there:
 * silent, and the search is the one before. Refused by rom_load: logged like a card file, then the card
 * search (which, with the cwd on the mount, tries it once more as ./rom.z64, as it did before). */
#define LOCAL_ROM_NAME "rom.z64"
static u8 *local_rom(char *out, u32 outlen, char *why, u32 whylen) {
    char path[512], err[96];
    struct stat st;
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    char *s;
    u8 *rom;
    if (n <= 0 || n >= (ssize_t) sizeof(path) - 1) return NULL;
    path[n] = 0;
    s = strrchr(path, '/');
    if (!s || (size_t) (s + 1 - path) + sizeof(LOCAL_ROM_NAME) > sizeof(path)) return NULL;
    memcpy(s + 1, LOCAL_ROM_NAME, sizeof(LOCAL_ROM_NAME));
    if (stat(path, &st) != 0) return NULL;
    if ((rom = rom_load(path, err, sizeof(err))) != NULL) {
        snprintf(out, outlen, "%s", path);
#if defined(MIYOO)
        port_log_always("rom: %s (in the game's folder)\n", path);
#else
        port_log_always("rom: %s (in the game's folder)\n", path);
#endif
        return rom;
    }
    if (sRomIsDkr) snprintf(why, whylen, "%s: %s", LOCAL_ROM_NAME, err); /* B9, as scan_dir */
    port_log_always("assets: skipping %s: %s\n", path, err);
    return NULL;
}

/* DKR_ROM=<path> wins; then rom.z64 in the game's folder; then the card search as before. */
static u8 *find_rom(char *out, u32 outlen, char *why, u32 whylen) {
    const char *env = getenv("DKR_ROM");
    u8 *rom;
    why[0] = 0;
    if (env && env[0]) {
        char err[96];
        if ((rom = rom_load(env, err, sizeof(err))) != NULL) { snprintf(out, outlen, "%s", env); return rom; }
        snprintf(why, whylen, "DKR_ROM: %s", err);
        return NULL;
    }
    if ((rom = local_rom(out, outlen, why, whylen))) return rom;
    if ((rom = scan_dir(port_eboot_dir(), out, outlen, why, whylen))) return rom;
    if ((rom = scan_dir(port_save_dir(), out, outlen, why, whylen))) return rom;
#if defined(TARGET_PC)
    if ((rom = scan_tree(".", 2, out, outlen, why, whylen))) return rom;
#elif defined(MIYOO)
    if ((rom = scan_tree(SD_HOME, 1, out, outlen, why, whylen))) return rom; /* the usual place first */
    if ((rom = scan_tree(SD_CARD, 2, out, outlen, why, whylen))) return rom;
#else
    if ((rom = scan_tree(SD_CARD, 2, out, outlen, why, whylen))) return rom;
    if ((rom = scan_tree(SD_HOME, 2, out, outlen, why, whylen))) return rom;
#endif
    return NULL;
}

/* ------------------------------------------------------------------ cache */
/* An extraction cut short leaves its tmp behind: a power cut, or a power-off during the first
 * start (the game's SIGTERM handler isn't installed yet, so the signal ends the
 * process mid-write). Nothing reads it, and the next start writes a new one under its own pid, so
 * each interrupted first start left ~11.6 MB on the card for good. Removed before a new extraction:
 * only names of the form dkr_assets.bin.<digits>.tmp, which only write_cache makes. */
static void remove_stale_tmp(void) {
    static const char pre[] = CACHE_NAME ".";
    const size_t np = sizeof(pre) - 1;
    DIR *d = opendir(port_save_dir());
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        const size_t len = strlen(n);
        size_t i;
        if (len <= np + 4 || strncmp(n, pre, np) != 0 || strcmp(n + len - 4, ".tmp") != 0) continue;
        for (i = np; i < len - 4 && n[i] >= '0' && n[i] <= '9'; i++) { }
        if (i != len - 4) continue; /* not <digits> in between: not ours */
        port_log_always("assets: removing %s (an earlier extraction cut short)\n", n);
        remove(port_save_path(n));
    }
    closedir(d);
}

static int write_cache(const u8 *region, char *err, u32 errlen) {
    char tmp[600];
    const char *dst = port_save_path(CACHE_NAME);
    int fd;
    u32 done = 0;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", dst, (int) getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(err, errlen, "cannot create %s", tmp); return 0; }
    while (done < REGION_SIZE) {
        u32 chunk = REGION_SIZE - done;
        ssize_t w;
        if (chunk > 0x80000u) chunk = 0x80000u;
        w = write(fd, region + done, chunk);
        if (w <= 0) { close(fd); remove(tmp); snprintf(err, errlen, "write failed (card full?)"); return 0; }
        done += (u32) w;
        screen_progress("Copying the game data", 30 + (int) ((u64) done * 60 / REGION_SIZE));
    }
    screen_progress("Saving", 90);
    if (fsync(fd) != 0 || close(fd) != 0) { remove(tmp); snprintf(err, errlen, "fsync failed"); return 0; }
    screen_progress("Finishing the copy", 96);
    if (rename(tmp, dst) != 0) { remove(tmp); snprintf(err, errlen, "rename failed"); return 0; }
    { /* the rename itself onto the card (as eeprom.bin and the save states): a power cut soon after
       * the first start otherwise can lose the new name, and the next start extracts it all again. The
       * file first: on vfat its directory entry (made with cluster 0 and size 0 by the rename) gets its
       * start and size only when the inode is written (the SSB64 port cd5128c) */
        const int f2 = open(dst, O_RDONLY);
        if (f2 >= 0) { fsync(f2); close(f2); }
        const int dir = open(port_save_dir(), O_RDONLY);
        if (dir >= 0) { fsync(dir); close(dir); }
    }
    screen_progress("Finishing the copy", 99);
    return 1;
}

static int map_cache(char *err, u32 errlen) {
    const char *path = port_save_path(CACHE_NAME);
    struct stat st;
    void *p;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "not found"); return 0; }
    if (fstat(fd, &st) != 0 || (u32) st.st_size != REGION_SIZE) {
        close(fd); snprintf(err, errlen, "wrong size"); return 0;
    }
    p = mmap(NULL, REGION_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { snprintf(err, errlen, "mmap failed"); return 0; }
    /* Every boot checks the size only (above). The full SHA-1 ran when the file
     * was made (tmp, fsync, rename); DKR_VERIFY=1 re-runs it. */
    if (getenv("DKR_VERIFY")) {
        Sha1 s; char hex[41];
        sha1_init(&s); sha1_update(&s, (const u8 *) p, REGION_SIZE); sha1_hex(&s, hex);
        if (strcmp(hex, kRegionSha1) != 0) { munmap(p, REGION_SIZE); snprintf(err, errlen, "sha1 mismatch"); return 0; }
        port_log_always("assets: %s verified\n", CACHE_NAME);
    }
    sRegion = (const u8 *) p;
    sRegionSize = REGION_SIZE;
    return 1;
}

/* ------------------------------------------------------------------ UI */
#if defined(HANDHELD) && !defined(TARGET_PC)
/* First start / error screen on the panel (SDL 1.2 + the OS font), as MK64. */
#include <SDL/SDL.h>
#include "handheld_screen.h"
/* Miyoo Mini: the menus' bitmap font (miyoo_menu.c), 2x for the title and 1x for the rest, so
 * the first-start screen needs no font file. A "font" here is just its scale. Everything draws
 * into miyoo_video.c's 320x240 surface, which miyoo_flip doubles and turns onto the panel. */
#include "miyoo_video.h"
typedef struct { int k; } TTF_Font;
static TTF_Font sBig = { 2 }, sSmall = { 1 };
#define SDL_SetVideoMode miyoo_set_video_mode
#define SDL_Flip         miyoo_flip
static SDL_Surface *sScr;
static TTF_Font *sFontBig, *sFontSmall;

static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "assets: SDL_Init: %s\n", SDL_GetError()); return; }
    /* the game's screen size (handheld_screen.c): the text centres on it, the bar is placed by it */
    sScr = SDL_SetVideoMode(handheld_screen_w(), handheld_screen_h(), 16, SDL_SWSURFACE);
    if (handheld_screen_w() != 240 && (!sScr || sScr->w != handheld_screen_w() || sScr->h != handheld_screen_h())) {
        handheld_screen_confirm(sScr ? sScr->w : 0, sScr ? sScr->h : 0); /* no 320x240: the square size */
        sScr = SDL_SetVideoMode(240, 240, 16, SDL_SWSURFACE);
    }
    if (!sScr) { fprintf(stderr, "assets: SDL_SetVideoMode: %s\n", SDL_GetError()); return; }
    SDL_ShowCursor(SDL_DISABLE);
    sFontBig = &sBig; sFontSmall = &sSmall;
}
/* the TTF calls this screen makes, on the bitmap font: centred text, and a width for the sub line */
static void ui_text(int y, const char *str, TTF_Font *f, int gray) {
    if (!sScr || !f || !str || !str[0]) return;
    const Uint32 c = 0xFF000000u | ((Uint32) gray << 16) | ((Uint32) gray << 8) | (Uint32) gray;
    miyoo_text(sScr, (sScr->w - miyoo_text_w(str, f->k)) / 2, y + (f->k == 1 ? 2 : 1), f->k, str, c);
}
static int TTF_SizeText(TTF_Font *f, const char *str, int *w, int *h) {
    if (w) *w = miyoo_text_w(str, f->k);
    if (h) *h = 8 * f->k;
    return 0;
}
/* title, up to three body lines, a progress bar (pct < 0: none), a footer */
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, SDL_MapRGB(sScr->format, 0, 0, 0));
    ui_text(22, "DIDDY KONG RACING", sFontBig, 255);
    { /* the sub line fits 232 px (Miyoo: the width less 8): a long file name gets an ellipsis */
        char fit[64];
        int w = 0;
        snprintf(fit, sizeof(fit), "%s", sSub);
        while (fit[0] && TTF_SizeText(sFontSmall, fit, &w, NULL) == 0 && w > sScr->w - 8) { /* 39 characters at 320 */
            size_t n = strlen(fit);
            if (n >= 4 && strcmp(fit + n - 3, "...") == 0) n -= 3;
            if (n == 0) break;
            snprintf(fit + n - 1, sizeof(fit) - (n - 1), "...");
        }
        ui_text(50, fit, sFontSmall, 150);
    }
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
    /* DKR_SPLASH_DUMP=<dir>: write the rendered surface to a PPM (debug only,
     * env-gated). Lets the splash be captured off-device (SDL_ttf renders to
     * the surface even under the dummy video driver). */
    {
        const char *dd = getenv("DKR_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256]; FILE *f; int x, y;
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", sScr->w, sScr->h);
                if (SDL_MUSTLOCK(sScr)) SDL_LockSurface(sScr);
                for (y = 0; y < sScr->h; y++) for (x = 0; x < sScr->w; x++) {
                    Uint16 *p = (Uint16 *) ((Uint8 *) sScr->pixels + y * sScr->pitch + x * 2);
                    Uint8 r, g, b, px[3];
                    SDL_GetRGB(*p, sScr->format, &r, &g, &b);
                    px[0] = r; px[1] = g; px[2] = b; fwrite(px, 1, 3, f);
                }
                if (SDL_MUSTLOCK(sScr)) SDL_UnlockSurface(sScr);
                fclose(f);
            }
        }
    }
    SDL_Flip(sScr);
}
static void ui_wait_button(void) {
    SDL_Event ev;
    if (!sScr || getenv("DKR_ASSETS_ONLY")) return; /* headless asset run: never block */
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
static void ui_begin(void) { }
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    if (pct >= 0) printf("assets: %s %s %s %d%%\n", l1, l2 ? l2 : "", l3 ? l3 : "", pct);
    else printf("assets: %s | %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", foot ? foot : "");
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
    sSub[0] = 0;
    port_log_always("assets: FAILED: %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    fprintf(stderr, "assets: %s %s %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
    fflush(NULL);
    exit(1);
}

void port_assets_load(void) {
    char err[160], why[200], rom_path[512];
    u8 *rom;
    /* Handheld/.dkr/rebuild (the player's, any content): do the first start's job again. The old
     * dkr_assets.bin is not read; it is replaced (tmp, fsync, rename) only by a copy that checks out,
     * so with no usable ROM it stays. The file goes once the new copy is in place. */
    char rb[600]; /* port_save_path's buffer is reused */
    struct stat rbst;
    const int rebuild = (snprintf(rb, sizeof(rb), "%s", port_save_path("rebuild")), stat(rb, &rbst) == 0);
    if (rebuild) {
        err[0] = 0;
        port_log_always("assets: %s found -- the first start's job runs again\n", rb);
    } else {
        if (map_cache(err, sizeof(err))) return;
        port_log_always("assets: %s%s: %s -- extracting from the ROM\n", port_save_dir(), CACHE_NAME, err);
    }
    remove_stale_tmp();
    ui_begin();
    screen_progress("Finding the ROM", 0);
    rom = find_rom(rom_path, sizeof(rom_path), why, sizeof(why));
    if (!rom) {
        if (why[0]) {
            /* the reason after "<file>: ", one line of the 240 px screen (36 characters
             * and a full stop fit the 12 px font), as a sentence; the log keeps the whole of why */
            char reason[40];
            snprintf(reason, sizeof(reason), "%.36s.", strrchr(why, ':') ? strrchr(why, ':') + 2 : why);
            if (reason[0] >= 'a' && reason[0] <= 'z') reason[0] = (char) (reason[0] - 'a' + 'A');
            fail_screen("No usable Diddy Kong Racing ROM.", reason, "It must be the US 1.1 (Rev 1) cart.");
        }
        fail_screen("No Diddy Kong Racing ROM found.", "Copy your US 1.1 .z64/.n64/.v64", "to the SD card and start again.");
    }
    {
        const char *base = strrchr(rom_path, '/');
        base = base ? base + 1 : rom_path;
        snprintf(sSub, sizeof(sSub), "%.60s", base);
    }
    port_log_always("assets: ROM %s is US 1.1 (sha1 %s)\n", rom_path, kRomSha1);
    screen_progress("Checking the ROM", 20);
    {
        Sha1 s; char hex[41];
        sha1_init(&s); sha1_screen(&s, rom + REGION_START, REGION_SIZE, 20, 30); sha1_hex(&s, hex);
        if (strcmp(hex, kRegionSha1) != 0) {
            free(rom);
            fail_screen("This isn't the expected ROM data.", "It must be the US 1.1 (Rev 1) cart.", NULL);
        }
    }
    screen_progress("Copying the game data", 30);
    if (!write_cache(rom + REGION_START, err, sizeof(err))) { free(rom); fail_screen("Cannot write the game data:", err, NULL); }
    free(rom);
    if (!map_cache(err, sizeof(err))) fail_screen("Cannot read back the game data:", err, NULL);
    port_log_always("assets: wrote %s (%u bytes)\n", port_save_path(CACHE_NAME), (unsigned) REGION_SIZE);
    if (rebuild) {
        if (remove(rb) == 0) port_log_always("assets: rebuild done, %s removed\n", rb);
        else port_log_always("assets: rebuild done, but %s can't be removed: the job runs again next start\n", rb);
    }
    screen_progress("Done", 100);
    if (!getenv("DKR_SPLASH_DUMP") && !getenv("DKR_ASSETS_ONLY")) usleep(600 * 1000);
    ui_black();
    ui_end(0);
}

/* PI DMA source: `rom` is a cartridge offset inside the asset region. */
int port_rom_read(u32 rom, void *dst, u32 len) {
    if (!sRegion || rom < REGION_START || rom + len > REGION_END || rom + len < rom) {
        port_log_always("pi: read outside the asset region: %08X+%X\n", (unsigned) rom, (unsigned) len);
        memset(dst, 0, len);
        return 0;
    }
    memcpy(dst, sRegion + (rom - REGION_START), len);
    gPortRomBytes += len; /* SLOW lines: ROM bytes read this iteration */
    return 1;
}
