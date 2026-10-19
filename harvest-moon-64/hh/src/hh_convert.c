/* First start: the data pack (data/hm64-pack.bin) built on the device from the player's own ROM, as the Ocarina of
 * Time and Majora's Mask ports do for theirs. Host side.
 *
 * The game folder holds hm64_recipes.bin next to the executable (hh/tools/hm64_recipe.py: op numbers, lengths and
 * this build's own addresses; no ROM data). The pack is the ROM with the bytecode and text re-assembled for ARM: each
 * of its bytes is the ROM's as it is, a ROM halfword or word with its bytes reversed, or an address in this
 * executable. The result's CRC-32 must match the recipe's.
 *
 *   data/hm64-pack.bin + data/hm64-pack.id ("HMPI" + the recipe's CRC): current -> nothing to do.
 *   Otherwise the ROM is found (HM64_ROM=<path>; rom.z64 next to the executable; any ROM in the game folder or the
 *   data folder; then the SD card's Roms/N64 and two folders deep), checked (16 MB, any byte order, header, SHA-1) and
 *   the pack built, with the progress and error screens below. A data/hm64-pack.bin without an id (an older PERSONAL
 *   zip's) is used as it is when no ROM is found.
 *
 * HM64_CONVERT_ONLY=1 exits after this step (tests, timing); HM64_CONVERT_FORCE=1 rebuilds even a current pack;
 * HM64_SPLASH_DUMP=<dir> writes each screen as a PPM (and never waits for a button). */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include "port.h"

#define ROM_SIZE 0x1000000u
#define RECIPES_NAME "hm64_recipes.bin"
#define PKG_ROM_NAME "rom.z64"
#define PACK_NAME "hm64-pack.bin"
#define PACK_ID_NAME "hm64-pack.id"

void hh_watch_beat(void);
void hh_watch_menu(int d);

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint32_t be32(const unsigned char *p) { return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- the screens
 * A 320x240 picture in the menus' bitmap font (kit miyoo_menu.c: 2x for the title, 1x for the rest), put on the panel
 * as the game's frames are (kit miyoo_panel_present32: doubled on a Mini's 640x480, at the panel's full height on a
 * Mini v4 / Flip's 752x560, turned). Elsewhere (the desktop) printed. An error waits for a button, then ends the
 * process. */
#if defined(PM64_SDL12) && defined(MIYOO)
#include <SDL/SDL.h>
#include "miyoo_panel.h"
static SDL_Surface *sScr; /* 320x240 XRGB8888 */
static char sSub[64];
static void ui_begin(void) {
    if (sScr) return;
    if (!miyoo_panel_open()) return; /* port_video_init opened it: the same surface */
    sScr = SDL_CreateRGBSurface(SDL_SWSURFACE, 320, 240, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}
static void ui_text(int y, const char *s, int k, int gray) {
    char fit[128];
    if (!sScr || !s || !s[0]) return;
    snprintf(fit, sizeof fit, "%s", s); /* a long line (a ROM's file name) keeps its start and ends in "..." */
    const int maxc = 312 / (8 * k);
    if ((int) strlen(fit) > maxc && maxc > 3) memcpy(fit + maxc - 3, "...", 4);
    const uint32_t c = 0xFF000000u | (uint32_t) gray << 16 | (uint32_t) gray << 8 | (uint32_t) gray;
    miyoo_text(sScr, (sScr->w - miyoo_text_w(fit, k)) / 2, y + (k == 1 ? 2 : 1), k, fit, c);
}
static void ui_show(void) {
    miyoo_panel_present32((uint32_t *) sScr->pixels, 320, 240);
    miyoo_panel_flip();
}
static void ui_fill(int x, int y, int w, int h, int gray) {
    SDL_Rect r = { (Sint16) x, (Sint16) y, (Uint16) w, (Uint16) h };
    SDL_FillRect(sScr, &r, 0xFF000000u | (uint32_t) gray << 16 | (uint32_t) gray << 8 | (uint32_t) gray);
}
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    ui_begin();
    if (!sScr) return;
    ui_fill(0, 0, 320, 240, 0);
    ui_text(22, "HARVEST MOON 64", 2, 255);
    ui_text(50, sSub, 1, 150);
    ui_text(86, l1, 1, 230);
    ui_text(104, l2, 1, 230);
    ui_text(122, l3, 1, 230);
    if (pct >= 0) {
        char p[8];
        ui_fill(70, 158, 180, 12, 70);
        ui_fill(72, 160, 176 * pct / 100, 8, 255);
        snprintf(p, sizeof p, "%d%%", pct);
        ui_text(176, p, 1, 200);
    }
    ui_text(212, foot, 1, 150);
    const char *dd = getenv("HM64_SPLASH_DUMP");
    if (dd && dd[0]) {
        static int seq;
        char path[512];
        snprintf(path, sizeof path, "%s/splash_%02d_%03d.ppm", dd, seq++, pct < 0 ? 999 : pct);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n320 240\n255\n");
            for (int i = 0; i < 320 * 240; i++) {
                const uint32_t px = ((uint32_t *) sScr->pixels)[i];
                const unsigned char b[3] = { (unsigned char) (px >> 16), (unsigned char) (px >> 8), (unsigned char) px };
                fwrite(b, 1, 3, f);
            }
            fclose(f);
        }
    }
    ui_show();
}
static void ui_wait_button(void) {
    SDL_Event ev;
    if (!sScr || getenv("HM64_EXIT_AT") || getenv("HM64_SPLASH_DUMP")) return; /* headless runs never block */
    while (SDL_PollEvent(&ev)) {} /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    ui_fill(0, 0, 320, 240, 0);
    ui_show();
    ui_show(); /* a double buffer's other page too */
}
static void ui_quit(void) {
    port_video_shutdown();
}
#else
static char sSub[64];
static void ui_draw(const char *l1, const char *l2, const char *l3, int pct, const char *foot) {
    printf("hm64: %s | %s | %s | %s | %d%% | %s\n", sSub, l1 ? l1 : "", l2 ? l2 : "", l3 ? l3 : "", pct, foot ? foot : "");
}
static void ui_wait_button(void) {}
static void ui_black(void) {}
static void ui_quit(void) {}
#endif

static int sOpen, sLastPct = -2;
static char sLastStage[96], sRomName[64];

static void progress(const char *stage, int pct) {
    hh_watch_beat(); /* building is no stall */
    if (pct == sLastPct && strcmp(stage, sLastStage) == 0) return;
    sLastPct = pct;
    snprintf(sLastStage, sizeof sLastStage, "%s", stage);
    snprintf(sSub, sizeof sSub, "%s", sRomName[0] ? sRomName : "Looking on the SD card");
    sOpen = 1;
    ui_draw("First start: building the game", "data from your ROM.", stage, pct, "This happens once.");
}

static void fail(const char *l1, const char *l2, const char *l3) {
    port_log_always("hm64: first start stopped: %s %s %s\n", l1 ? l1 : "", l2 ? l2 : "", l3 ? l3 : "");
    if (strcmp(sSub, "Looking on the SD card") == 0) sSub[0] = 0;
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    hh_watch_menu(1); /* waiting for the player is no stall */
    ui_wait_button();
    ui_quit();
    fflush(NULL);
    exit(1);
}

/* "Done" at 100%, a beat to read it, then black: the game's first frame starts from black */
static void close_screen(void) {
    if (!sOpen) return;
    progress("Done", 100);
    if (!getenv("HM64_SPLASH_DUMP") && !getenv("HM64_EXIT_AT") && !getenv("HM64_CONVERT_ONLY")) usleep(600 * 1000);
    ui_black();
    sOpen = 0;
}

/* ---------------------------------------------------------------- SHA-1 */
typedef struct {
    uint32_t h[5];
    uint64_t n;
    unsigned char buf[64];
    unsigned bl;
} Sha1;
#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static void sha1_block(Sha1 *s, const unsigned char *p) {
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->n = 0; s->bl = 0;
}
static void sha1_update(Sha1 *s, const unsigned char *p, size_t n) {
    s->n += n;
    while (n) {
        if (s->bl == 0 && n >= 64) { sha1_block(s, p); p += 64; n -= 64; continue; }
        s->buf[s->bl++] = *p++; n--;
        if (s->bl == 64) { sha1_block(s, s->buf); s->bl = 0; }
    }
}
static void sha1_final(Sha1 *s, unsigned char out[20]) {
    const uint64_t bits = s->n * 8;
    unsigned char pad = 0x80, z = 0, len[8];
    sha1_update(s, &pad, 1);
    while (s->bl != 56) sha1_update(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (unsigned char) (bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char) (s->h[i] >> 24); out[i * 4 + 1] = (unsigned char) (s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char) (s->h[i] >> 8); out[i * 4 + 3] = (unsigned char) s->h[i];
    }
}

/* ---------------------------------------------------------------- files */
static int exe_dir_file(char *out, size_t len, const char *name) {
    char exe[512];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    return snprintf(out, len, "%s/%s", exe, name) < (int) len;
}
/* under qemu-arm /proc/self/exe is qemu's: HM64_RECIPES=<path> names the recipe (tests) */
static int recipes_path(char *out, size_t len) {
    const char *e = getenv("HM64_RECIPES");
    if (e && e[0]) return snprintf(out, len, "%s", e) < (int) len;
    return exe_dir_file(out, len, RECIPES_NAME);
}

typedef struct {
    uint32_t packSize, crc, opsN;
    char sha1[41];
    unsigned char *ops;
} Recipes;
static Recipes sR;

/* the header only (full = 0), or the ops too. "no": no recipe in the folder */
static const char *recipes_load(int full) {
    char path[600];
    unsigned char h[64];
    if (!recipes_path(path, sizeof path)) return "no";
    FILE *f = fopen(path, "rb");
    if (!f) return "no";
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64 || fread(h, 1, 64, f) != 64 || memcmp(h, "HMR1", 4) != 0 || le32(h + 4) != 1) {
        fclose(f);
        return "The game's " RECIPES_NAME " is damaged.";
    }
    sR.packSize = le32(h + 8);
    sR.crc = le32(h + 12);
    sR.opsN = le32(h + 16);
    memcpy(sR.sha1, h + 20, 40);
    sR.sha1[40] = 0;
    if (full) {
        const size_t zn = (size_t) size - 64;
        unsigned char *z = malloc(zn);
        uLongf n = sR.opsN;
        sR.ops = malloc(sR.opsN + 16);
        if (!z || !sR.ops || fread(z, 1, zn, f) != zn || uncompress(sR.ops, &n, z, zn) != Z_OK || n != sR.opsN) {
            free(z);
            fclose(f);
            return "The game's " RECIPES_NAME " is damaged.";
        }
        free(z);
    }
    fclose(f);
    return NULL;
}

/* 1 = current, 0 = missing or old, -1 = a pack without an id (an older PERSONAL zip's) */
static int pack_current(void) {
    struct stat st;
    unsigned char id[8];
    if (stat(port_save_path(PACK_NAME), &st) != 0 || (uint32_t) st.st_size != sR.packSize) return 0;
    FILE *f = fopen(port_save_path(PACK_ID_NAME), "rb");
    if (!f) return -1;
    const int ok = fread(id, 1, 8, f) == 8 && memcmp(id, "HMPI", 4) == 0 && le32(id + 4) == sR.crc;
    fclose(f);
    return ok;
}

/* ---------------------------------------------------------------- the ROM */
typedef struct {
    char path[512];
    int order; /* 0 z64, 1 v64, 2 n64 */
    char why[96], whyName[64];
} Find;

static void unswap(unsigned char *p, size_t n, int order) {
    if (order == 1)
        for (size_t i = 0; i + 1 < n; i += 2) { const unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    else if (order == 2)
        for (size_t i = 0; i + 3 < n; i += 4) {
            const unsigned char a = p[i], b = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a;
        }
}
static int ext_ok(const char *n) {
    const size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') return 0;
    e[0] = (char) (n[l - 3] | 0x20); e[1] = (char) (n[l - 2] | 0x20); e[2] = (char) (n[l - 1] | 0x20); e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") || !strcmp(e, "rom") || !strcmp(e, "bin") ||
           !strcmp(e, "u64") || !strcmp(e, "usa");
}
/* 1 = Harvest Moon 64 (USA) by its header; 0 = not this game; -1 = another version of it (why says which) */
static int quick_check(const char *path, char *why, size_t whylen, int *order) {
    unsigned char h[64];
    struct stat st;
    int o;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(h, 1, 64, f) != 64) { fclose(f); return 0; }
    fclose(f);
    if (h[0] == 0x80 && h[1] == 0x37) o = 0;
    else if (h[0] == 0x37 && h[1] == 0x80) o = 1;
    else if (h[0] == 0x40 && h[1] == 0x12) o = 2;
    else return 0;
    unswap(h, 64, o);
    if (memcmp(h + 0x20, "HARVESTMOON64", 13) != 0) return 0;
    if (h[0x3E] == 'P') { snprintf(why, whylen, "is the European (PAL) version."); return -1; }
    if (h[0x3E] != 'E') { snprintf(why, whylen, "is not the USA version."); return -1; }
    if ((uint64_t) st.st_size != ROM_SIZE) { snprintf(why, whylen, "is not 16 MB (a trimmed or damaged ROM)."); return -1; }
    *order = o;
    return 1;
}
static int try_file(Find *fd, const char *path) {
    char why[96];
    int o = 0;
    const int r = quick_check(path, why, sizeof why, &o);
    if (r > 0) { snprintf(fd->path, sizeof fd->path, "%s", path); fd->order = o; return 1; }
    if (r < 0 && !fd->why[0]) {
        const char *b = strrchr(path, '/');
        snprintf(fd->why, sizeof fd->why, "%s", why);
        snprintf(fd->whyName, sizeof fd->whyName, "%.60s", b ? b + 1 : path);
    }
    return 0;
}
static int scan_dir(Find *fd, const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *ent;
    int found = 0;
    if (!d) return 0;
    while (!found && (ent = readdir(d)) != NULL) {
        char path[512];
        const size_t dl = strlen(dir);
        if (ent->d_name[0] == '.' || !ext_ok(ent->d_name)) continue;
        snprintf(path, sizeof path, "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", ent->d_name);
        found = try_file(fd, path);
    }
    closedir(d);
    return found;
}
static int scan_tree(Find *fd, const char *root, int depth) {
    if (scan_dir(fd, root)) return 1;
    DIR *d = depth > 0 ? opendir(root) : NULL;
    struct dirent *ent;
    if (!d) return 0;
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;
        const size_t rl = strlen(root);
        if (ent->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s%s%s", root, rl && root[rl - 1] == '/' ? "" : "/", ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (scan_tree(fd, sub, depth - 1)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}
static int find_rom(Find *fd) {
    const char *env = getenv("HM64_ROM");
    char pkg[600];
    memset(fd, 0, sizeof *fd);
    if (env && env[0]) return try_file(fd, env);
    if (exe_dir_file(pkg, sizeof pkg, PKG_ROM_NAME) && try_file(fd, pkg)) return 1;
    if (exe_dir_file(pkg, sizeof pkg, "") && scan_dir(fd, pkg)) return 1; /* any ROM in the game folder */
    if (scan_dir(fd, port_save_dir())) return 1;
#if defined(MIYOO)
    return scan_tree(fd, "/mnt/SDCARD/Roms/N64", 1) || scan_tree(fd, "/mnt/SDCARD", 2);
#else
    return scan_tree(fd, ".", 2);
#endif
}

/* the whole ROM, in .z64 order, its SHA-1 checked */
static unsigned char *rom_load(const Find *fd, const char **err) {
    unsigned char *rom = malloc(ROM_SIZE);
    FILE *f = fopen(fd->path, "rb");
    Sha1 s;
    unsigned char out[20];
    char hex[41];
    *err = NULL;
    if (!rom || !f) { free(rom); if (f) fclose(f); *err = rom ? "Could not open the ROM." : "Not enough memory."; return NULL; }
    sha1_init(&s);
    for (uint32_t off = 0; off < ROM_SIZE; off += 1u << 20) {
        if (fread(rom + off, 1, 1u << 20, f) != 1u << 20) { fclose(f); free(rom); *err = "Could not read the ROM."; return NULL; }
        unswap(rom + off, 1u << 20, fd->order);
        sha1_update(&s, rom + off, 1u << 20);
        progress("Checking the ROM", (int) ((uint64_t) (off + (1u << 20)) * 40 / ROM_SIZE));
    }
    fclose(f);
    sha1_final(&s, out);
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    port_log_always("hm64: ROM SHA-1 %s\n", hex);
    if (strcmp(hex, sR.sha1) != 0) { free(rom); *err = "sha1"; return NULL; }
    return rom;
}

/* ---------------------------------------------------------------- building */
static int rd_leb(const unsigned char **c, const unsigned char *end, uint32_t *v) {
    uint32_t n = 0;
    int s = 0;
    while (*c < end && s < 35) {
        const unsigned char b = *(*c)++;
        n |= (uint32_t) (b & 0x7F) << s;
        s += 7;
        if (!(b & 0x80)) { *v = n; return 1; }
    }
    return 0;
}

/* the pack in memory: the ROM reshaped in place by the ops */
static const char *build(unsigned char *rom) {
    const unsigned char *c = sR.ops, *end = sR.ops + sR.opsN;
    uint32_t pos = 0;
    for (;;) {
        uint32_t n;
        if (c >= end) return "The game data recipe doesn't match.";
        const unsigned char op = *c++;
        if (op == 0) break;
        if (!rd_leb(&c, end, &n)) return "The game data recipe doesn't match.";
        switch (op) {
        case 1: if (n > sR.packSize - pos) return "The game data recipe doesn't match."; pos += n; break;
        case 2:
            if (n > (sR.packSize - pos) / 4) return "The game data recipe doesn't match.";
            for (uint32_t k = 0; k < n; k++, pos += 4) {
                unsigned char *p = rom + pos;
                const unsigned char a = p[0], b = p[1];
                p[0] = p[3]; p[1] = p[2]; p[2] = b; p[3] = a;
            }
            break;
        case 3:
            if (n > (sR.packSize - pos) / 2) return "The game data recipe doesn't match.";
            for (uint32_t k = 0; k < n; k++, pos += 2) { const unsigned char t = rom[pos]; rom[pos] = rom[pos + 1]; rom[pos + 1] = t; }
            break;
        case 4:
            if (n > (sR.packSize - pos) / 4 || (size_t) (end - c) < (size_t) n * 4) return "The game data recipe doesn't match.";
            memcpy(rom + pos, c, (size_t) n * 4);
            c += (size_t) n * 4;
            pos += n * 4;
            break;
        default: return "The game data recipe doesn't match.";
        }
        if ((pos & 0xFFFF) < 64) progress("Building the game data", 40 + (int) ((uint64_t) pos * 40 / sR.packSize));
    }
    if (pos != sR.packSize || c != end) return "The game data recipe doesn't match.";
    const uint32_t crc = (uint32_t) crc32(crc32(0L, Z_NULL, 0), rom, sR.packSize);
    if (crc != sR.crc) {
        port_log_always("hm64: rebuilt pack CRC %08x, the recipe says %08x\n", (unsigned) crc, (unsigned) sR.crc);
        return "The rebuilt game data doesn't check out.";
    }
    return NULL;
}

static const char *write_pack(const unsigned char *pack) {
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s", port_save_path(PACK_NAME));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(port_save_path(PACK_ID_NAME)); /* written last: an interrupted build is redone */
    FILE *out = fopen(tmp, "wb");
    if (!out) return "Could not write to the SD card.";
    for (uint32_t off = 0; off < sR.packSize; off += 1u << 20) {
        const uint32_t n = sR.packSize - off < (1u << 20) ? sR.packSize - off : 1u << 20;
        if (fwrite(pack + off, 1, n, out) != n) { fclose(out); unlink(tmp); return "Could not write to the SD card (is it full?)."; }
        progress("Saving the game data", 80 + (int) ((uint64_t) (off + n) * 19 / sR.packSize));
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) { fclose(out); unlink(tmp); return "Could not write to the SD card (is it full?)."; }
    fclose(out);
    if (rename(tmp, path) != 0) return "Could not write to the SD card.";
    unsigned char id[8] = { 'H', 'M', 'P', 'I', (unsigned char) sR.crc, (unsigned char) (sR.crc >> 8),
                            (unsigned char) (sR.crc >> 16), (unsigned char) (sR.crc >> 24) };
    out = fopen(port_save_path(PACK_ID_NAME), "wb");
    if (!out || fwrite(id, 1, 8, out) != 8) { if (out) fclose(out); return "Could not write to the SD card."; }
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    return NULL;
}

/* main(), after the panel is open and before the pack is read. HM64_PACK set (tests): nothing to do here. */
void hh_convert_ensure(void) {
    const char *err;
    const int force = getenv("HM64_CONVERT_FORCE") != NULL;
    const double t0 = now_s();
    Find fd;
    if (getenv("HM64_PACK") && getenv("HM64_PACK")[0] && !force) return;
    err = recipes_load(0);
    if (err) {
        if (strcmp(err, "no") == 0) { port_log_always("hm64: no %s next to the game: the data pack is used as it is\n", RECIPES_NAME); return; }
        fail(err, "Copy the whole game folder to the", "SD card again.");
    }
    const int cur = pack_current();
    if (cur > 0 && !force) {
        port_log_always("hm64: game data current (recipe %08x)\n", (unsigned) sR.crc);
        if (getenv("HM64_CONVERT_ONLY")) exit(0);
        return;
    }
    sRomName[0] = 0;
    progress("Looking for your ROM...", 0);
    if (!find_rom(&fd)) {
        if (cur < 0 && !force) { /* an older zip's own pack, and no ROM to rebuild it from: use it */
            port_log_always("hm64: data pack without an id and no ROM found: used as it is\n");
            close_screen();
            return;
        }
        if (fd.why[0]) fail(fd.whyName, fd.why, "Harvest Moon 64 needs the USA version.");
        fail("No Harvest Moon 64 ROM found.", "Copy your USA ROM (.z64/.v64/.n64)", "to App/HarvestMoon64/rom.z64.");
    }
    {
        const char *b = strrchr(fd.path, '/');
        snprintf(sRomName, sizeof sRomName, "%.60s", b ? b + 1 : fd.path);
    }
    port_log_always("hm64: first start: ROM %s (%s order)\n", fd.path, fd.order == 0 ? "z64" : fd.order == 1 ? "v64" : "n64");
    if ((err = recipes_load(1)) != NULL) fail(err, NULL, NULL);
    unsigned char *rom = rom_load(&fd, &err);
    if (!rom) {
        if (strcmp(err, "sha1") == 0) fail("This ROM's checksum doesn't match", "Harvest Moon 64 (USA).", "It may be damaged or modified.");
        fail(err, NULL, NULL);
    }
    const double t1 = now_s();
    port_log_always("hm64: ROM SHA-1 OK in %.1f s\n", t1 - t0);
    err = build(rom);
    if (!err) err = write_pack(rom);
    free(rom);
    free(sR.ops);
    sR.ops = NULL;
    if (err) fail(err, NULL, NULL);
    port_log_always("hm64: game data built in %.1f s (first start %.1f s)\n", now_s() - t1, now_s() - t0);
    close_screen();
    if (getenv("HM64_CONVERT_ONLY")) exit(0);
}
