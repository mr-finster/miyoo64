/**
 * Finding the player's ROM. The game reads it like a cartridge for as long as
 * it runs (ultra/rom.c), so it has to stay where it is: on the SD card, or
 * in the game folder.
 *
 * Order: FZX_ROM=<path>; rom.z64 in the game folder (next to the executable: a
 * personal zip puts it there); the path that worked last time
 * (<data>/rompath.txt); the data folder; then anywhere on the card,
 * up to two folders deep. Any file name; .z64/.n64/.v64/.rom/.bin/.u64 in any
 * of the three byte orders. A file is taken when its header says F-Zero X USA
 * (quick check); the full SHA-1 then runs once and its verdict is cached by
 * path, size and mtime (<data>/romcheck.txt), so later boots skip hashing
 * 16 MB. The game folder's copy is hashed every start (its path and size are the
 * same in every build); a refused copy (header or SHA-1) leads to the card search,
 * and its path is never put in rompath.txt.
 *
 * On the device the first-boot check shows a progress bar (the SHA-1, then the verdict's save, "Done" at 100%, one black
 * frame; SDL stays up), and a missing or wrong ROM gets a
 * message that stays until a button is pressed, then the game quits.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#if defined(MIYOO)
#include "miyoo_panel.h"
#endif

extern int port_rom_open_checked(const char* path, int skipHash, char* err, int errlen);
extern int port_write_file_atomic(const char* path, const void* data, size_t size);

#define ROM_SIZE 0x1000000

static int ext_ok(const char* n) {
    const size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') {
        return 0;
    }
    e[0] = (char) (n[l - 3] | 0x20);
    e[1] = (char) (n[l - 2] | 0x20);
    e[2] = (char) (n[l - 1] | 0x20);
    e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") || !strcmp(e, "rom") || !strcmp(e, "bin") ||
           !strcmp(e, "u64");
}

/* The last file the search turned down that wasn't F-Zero X (another system's .bin, another N64
 * game): for the log only. B9 (the lead's matrix; Star Fox's search is the model): with no F-Zero X
 * on the card, the screen used to say "No usable F-Zero X ROM." and name whichever other .bin or
 * .rom was scanned last ("not a 16 MB ROM"), or the remembered path of a ROM since removed, where
 * "No F-Zero X ROM found." was meant. */
static char sOther[256];

/* Header check in any byte order: "F-ZERO X" and cartridge id CFZE (USA). A rejection's reason
 * goes to `why` (shown) only for an F-Zero X dump or a file the player named (FZX_ROM); any other
 * file's goes to sOther. */
static int quick_check(const char* path, int named, char* why, size_t whylen) {
    u8 h[64], n[64];
    struct stat st;
    FILE* f;
    int i;
    if (stat(path, &st) != 0) {
        snprintf(named ? why : sOther, named ? whylen : sizeof(sOther), "%s: not found", path);
        return 0;
    }
    if (st.st_size != ROM_SIZE) {
        snprintf(named ? why : sOther, named ? whylen : sizeof(sOther), "%s: not a 16 MB ROM", path);
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL || fread(h, 1, sizeof(h), f) != sizeof(h)) {
        if (f != NULL) {
            fclose(f);
        }
        snprintf(named ? why : sOther, named ? whylen : sizeof(sOther), "%s: unreadable", path);
        return 0;
    }
    fclose(f);
    if (h[0] == 0x80 && h[1] == 0x37) {
        memcpy(n, h, sizeof(n));
    } else if (h[0] == 0x37 && h[1] == 0x80) {
        for (i = 0; i < 64; i += 2) { n[i] = h[i + 1]; n[i + 1] = h[i]; }
    } else if (h[0] == 0x40 && h[1] == 0x12) {
        for (i = 0; i < 64; i += 4) { n[i] = h[i + 3]; n[i + 1] = h[i + 2]; n[i + 2] = h[i + 1]; n[i + 3] = h[i]; }
    } else {
        snprintf(named ? why : sOther, named ? whylen : sizeof(sOther), "%s: not an N64 ROM", path);
        return 0;
    }
    if (memcmp(n + 0x20, "F-ZERO X", 8) != 0 || memcmp(n + 0x3B, "CFZ", 3) != 0) {
        snprintf(named ? why : sOther, named ? whylen : sizeof(sOther), "%s: not F-Zero X", path);
        return 0;
    }
    if (n[0x3E] != 'E') {
        /* an F-Zero X dump: shown, naming the version by the header's country code (E USA, J Japan,
         * P Europe), as OoT, DKR and GoldenEye do */
        snprintf(why, whylen, "%s: %s", path,
                 n[0x3E] == 'P' ? "It is the European (PAL) version." :
                 n[0x3E] == 'J' ? "It is the Japanese version." : "It is not the US version.");
        return 0;
    }
    return 1;
}

static int scan_dir(const char* dir, char* out, size_t outlen, char* why, size_t whylen) {
    DIR* d = opendir(dir);
    struct dirent* ent;
    int found = 0;
    if (d == NULL) {
        return 0;
    }
    while (!found && (ent = readdir(d)) != NULL) {
        char path[512];
        const size_t dl = strlen(dir);
        if (ent->d_name[0] == '.' || !ext_ok(ent->d_name)) {
            continue; /* hidden files, macOS "._x.z64" forks */
        }
        snprintf(path, sizeof(path), "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", ent->d_name);
        if (quick_check(path, 0, why, whylen)) {
            snprintf(out, outlen, "%s", path);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static int scan_tree(const char* root, int depth, char* out, size_t outlen, char* why, size_t whylen) {
    DIR* d;
    struct dirent* ent;
    if (scan_dir(root, out, outlen, why, whylen)) {
        return 1;
    }
    if (depth <= 0 || (d = opendir(root)) == NULL) {
        return 0;
    }
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;
        const size_t rl = strlen(root);
        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s%s%s", root, (rl && root[rl - 1] == '/') ? "" : "/", ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (scan_tree(sub, depth - 1, out, outlen, why, whylen)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int read_line(const char* path, char* buf, size_t len) {
    FILE* f = fopen(path, "r");
    size_t n;
    if (f == NULL) {
        return 0;
    }
    n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    return n > 0;
}

/* rom.z64 in the game folder, next to the executable (where a personal zip puts it). A launcher may
 * run the binary by a bare name, so it's found through /proc/self/exe, not argv[0].
 * 1 if the file is there (absent: a zip without it, the desktop build dir). Only read. */
static int pkg_rom(char* out, size_t outlen) {
    char exe[512];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe));
    const char* slash;
    struct stat st;
    if (n <= 0 || n >= (ssize_t) sizeof(exe)) {
        return 0; /* no link, or perhaps cut short */
    }
    exe[n] = 0;
    if ((slash = strrchr(exe, '/')) == NULL) {
        return 0;
    }
    snprintf(out, outlen, "%.*s/rom.z64", (int) (slash - exe), exe);
    return stat(out, &st) == 0;
}

/* <data>/rebuild (the player's, any content): redo the first start's job. The cached verdict (romcheck.txt) and the
 * remembered path (rompath.txt) are not trusted for this start, so the ROM is searched for and hashed in full on
 * the screen; both files are rewritten only by a ROM that checks out, so with no usable ROM they stay. The request
 * is removed once the ROM has opened (port_find_and_open_rom). */
static int sRebuild;
static char sRebuildPath[600];

static int find(char* out, size_t outlen, char* why, size_t whylen) {
    const char* env = getenv("FZX_ROM");
    char saved[512];
    if (env && env[0]) {
        snprintf(out, outlen, "%s", env);
        return quick_check(env, 1, why, whylen);
    }
    if (!sRebuild && read_line(port_data_path("rompath.txt"), saved, sizeof(saved)) && quick_check(saved, 0, why, whylen)) {
        snprintf(out, outlen, "%s", saved);
        return 1;
    }
    if (scan_dir(port_data_path(""), out, outlen, why, whylen)) {
        return 1;
    }
#if defined(MIYOO)
    { /* the Miyoo Mini's card: Roms/N64, then the whole card, two folders deep each (miyoo_panel.c) */
        int i;
        for (i = 0; miyoo_rom_roots[i] != NULL; i++) {
            if (scan_tree(miyoo_rom_roots[i], 2, out, outlen, why, whylen)) {
                return 1;
            }
        }
        return 0;
    }
#else
    return scan_tree(".", 2, out, outlen, why, whylen);
#endif
}

/* ------------------------------------------------------------------ UI */
/* Same screens as mario-kart-64 (base-psp/src/port/sdl/assets_handheld.c). */
#if defined(MIYOO)
/* The Miyoo Mini: the same screens at 320x240 in the Miyoo menu's 8x8 font (miyoo_text), doubled onto
 * the panel (miyoo_panel_present32); no OS fonts there. */
#include <SDL/SDL.h>
static SDL_Surface* sScr; /* 320x240 32 bpp, software */
static char sSub[64] = "Looking on the SD card";
static void ui_begin(void) {
    if (sScr) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { port_log("rom: SDL_Init: %s\n", SDL_GetError()); return; }
    if (!miyoo_panel_open()) return;
    SDL_ShowCursor(SDL_DISABLE);
    sScr = SDL_CreateRGBSurface(SDL_SWSURFACE, 320, 240, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}
static void ui_text(int y, const char* s, int k, int gray) {
    char fit[41];
    if (!sScr || !s || !s[0]) return;
    snprintf(fit, sizeof(fit), "%s", s); /* 40 characters fill the 320 columns at k = 1 */
    if (miyoo_text_w(fit, k) > 316) k = 1;
    miyoo_text(sScr, (320 - miyoo_text_w(fit, k)) / 2, y, k, fit, 0xFF000000u | (uint32_t) gray * 0x010101u);
}
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, 0);
    ui_text(30, "F-ZERO X", 3, 255);
    ui_text(66, sSub, 1, 150);
    ui_text(96, l1, 1, 230);
    ui_text(112, l2, 1, 230);
    ui_text(128, l3, 1, 230);
    if (pct >= 0) {
        SDL_Rect frame = { 60, 158, 200, 12 }, fill = { 62, 160, (Uint16) (196 * pct / 100), 8 };
        char p[16];
        SDL_FillRect(sScr, &frame, SDL_MapRGB(sScr->format, 70, 70, 70));
        SDL_FillRect(sScr, &fill, SDL_MapRGB(sScr->format, 255, 255, 255));
        snprintf(p, sizeof(p), "%d%%", pct);
        ui_text(178, p, 1, 200);
    }
    ui_text(214, foot, 1, 150);
    {   /* FZX_SPLASH_DUMP=<dir>: the screen as a PPM (tests, under qemu's dummy video) */
        const char* dd = getenv("FZX_SPLASH_DUMP");
        if (dd && dd[0]) {
            char path[256];
            FILE* f;
            int x, y;
            static int seq;
            if (pct >= 0) snprintf(path, sizeof(path), "%s/splash_%02d_%03d.ppm", dd, seq++, pct);
            else snprintf(path, sizeof(path), "%s/splash_fail.ppm", dd);
            f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n320 240\n255\n");
                for (y = 0; y < 240; y++) for (x = 0; x < 320; x++) {
                    const uint32_t p = ((const uint32_t*) ((const Uint8*) sScr->pixels + y * sScr->pitch))[x];
                    const u8 px[3] = { (u8) (p >> 16), (u8) (p >> 8), (u8) p };
                    fwrite(px, 1, 3, f);
                }
                fclose(f);
            }
        }
    }
    miyoo_panel_present32((uint32_t*) sScr->pixels, 320, 240);
    miyoo_panel_flip();
}
static void ui_wait_button(void) {
    SDL_Event ev;
    const char* hl = getenv("FZX_HEADLESS");
    if (!sScr || (hl && hl[0] && strcmp(hl, "0") != 0)) return; /* headless run: never block */
    while (SDL_PollEvent(&ev)) { } /* drop anything held from the launch */
    for (;;) {
        if (!SDL_WaitEvent(&ev)) return;
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_QUIT) return;
    }
}
static void ui_black(void) {
    if (!sScr) return;
    SDL_FillRect(sScr, NULL, 0);
    miyoo_panel_present32((uint32_t*) sScr->pixels, 320, 240);
    miyoo_panel_flip();
    miyoo_panel_present32((uint32_t*) sScr->pixels, 320, 240); /* a double buffer's other page too */
    miyoo_panel_flip();
}
/* done with the screen: SDL and the panel stay up for gfx init (quit == exiting) */
static void ui_end(int quit) {
    if (sScr) SDL_FreeSurface(sScr);
    sScr = NULL;
    if (quit) SDL_Quit();
}
#else
static char sSub[64] = "Looking on the SD card";
static void ui_begin(void) { }
static void ui_draw(const char* l1, const char* l2, const char* l3, int pct, const char* foot) {
    if (pct >= 0) port_log("rom: %s %s %s %d%%\n", l1, l2 ? l2 : "", l3 ? l3 : "", pct);
    else port_log("rom: %s | %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "", foot ? foot : "");
}
static void ui_wait_button(void) { }
static void ui_black(void) { }
static void ui_end(int quit) { (void) quit; }
#endif

static const char* sStage = "Checking the ROM";
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
    ui_draw("First start: checking", "your ROM.", sStage, pct, "This happens once.");
}
/* the SHA-1 is the bar's 0..96 */
static void hash_progress(int pct) {
    screen_progress(NULL, pct * 96 / 100);
}

/* The last piece of first-start work is done: "Done" at 100%, a beat to read it, one black frame on
 * both pages, the fonts down; SDL stays up for gfx init. Nothing on a start that showed no screen. */
static void finish_screen(void) {
    if (sShown) {
        screen_progress("Done", 100);
        if (getenv("FZX_SPLASH_DUMP") == NULL) usleep(600 * 1000);
        ui_black();
        sShown = 0;
    }
    ui_end(0);
}

/* The message stays until a button; the caller then quits. */
static void fail_screen(const char* l1, const char* l2, const char* l3) {
    ui_begin();
    port_log("rom: FAILED: %s | %s | %s\n", l1, l2 ? l2 : "", l3 ? l3 : "");
    ui_draw(l1, l2, l3, -1, "Press any button to quit.");
    ui_wait_button();
    ui_end(1);
}

static void set_sub(const char* path, size_t len) {
    const char* b = path;
    const char* s;
    for (s = path; *s && (size_t) (s - path) < len; s++) {
        if (*s == '/') b = s + 1;
    }
    len -= (size_t) (b - path);
    if (len >= sizeof(sSub)) len = sizeof(sSub) - 1;
    memcpy(sSub, b, len);
    sSub[len] = 0;
}

extern void (*port_rom_hash_progress)(int pct);

/* Verify and open `rom`: the full SHA-1 with a progress screen, unless romcheck.txt holds the verdict
 * for this exact file. `inside`: the game folder's copy (no verdict is kept or trusted, the SHA-1
 * runs every start). On success the progress screen
 * is still up; the caller ends it. */
static int open_rom(const char* rom, int inside, char* err, size_t errlen, int* cached) {
    char cache[704], want[704];
    struct stat st;
    int keyed = 1, ok;
    set_sub(rom, strlen(rom));
    if (stat(rom, &st) != 0) {
        memset(&st, 0, sizeof(st)); /* gone since the quick check: the open below says so */
        keyed = 0;
    }
    snprintf(want, sizeof(want), "%ld %ld %s", (long) st.st_mtime, (long) st.st_size, rom);
    if (inside && keyed) {
        keyed = 0;
        port_log("rom: %s: in the game folder, so its SHA-1 runs every start\n", rom);
    }
    *cached = keyed && !sRebuild && read_line(port_data_path("romcheck.txt"), cache, sizeof(cache)) && strcmp(cache, want) == 0;
    if (!*cached) { /* first boot with this file: the full SHA-1, with a progress screen */
        ui_begin();
        screen_progress("Checking the ROM", 0);
        port_rom_hash_progress = hash_progress;
    }
    ok = port_rom_open_checked(rom, *cached, err, (int) errlen);
    port_rom_hash_progress = NULL;
    if (ok && !*cached) {
        screen_progress("Saving the result", 97);
        if (keyed) {
            port_write_file_atomic(port_data_path("romcheck.txt"), want, strlen(want));
        }
    }
    return ok;
}

/* The ROM opened and its verdict is saved: the rebuild request is used up. */
static void rebuild_done(void) {
    if (!sRebuild) {
        return;
    }
    sRebuild = 0;
    if (remove(sRebuildPath) == 0) {
        port_log("rom: rebuild done, %s removed\n", sRebuildPath);
    } else {
        port_log("rom: rebuild done, but %s can't be removed: the job runs again next start\n", sRebuildPath);
    }
}

/* Find, verify and open the ROM; 0 (after telling the player) if there is none. */
int port_find_and_open_rom(void) {
    char rom[512], why[256] = "", err[256];
    const char* env = getenv("FZX_ROM");
    int cached;
    struct stat rbst;
    snprintf(sRebuildPath, sizeof(sRebuildPath), "%s", port_data_path("rebuild"));
    sRebuild = stat(sRebuildPath, &rbst) == 0;
    if (sRebuild) {
        port_log("rom: %s found, the first start's job runs again\n", sRebuildPath);
    }
    /* The game folder's own copy first (FZX_ROM, a path given on purpose, still wins). Refused by the
     * header (a reason only an F-Zero X dump shows, as for a card file) or the SHA-1: the card
     * search, as for a game folder without one. Its path is never put in rompath.txt: that keeps the
     * card ROM's for a game folder without one. */
    if (!(env && env[0]) && pkg_rom(rom, sizeof(rom))) {
        if (!quick_check(rom, 0, why, sizeof(why))) {
            port_log("rom: refused the game folder's copy (%s); searching the card\n", why[0] ? why : sOther);
#if defined(MIYOO)
        } else if (open_rom(rom, 0, err, sizeof(err), &cached)) { /* App/<game>/rom.z64: a card file, its verdict keyed as one */
            finish_screen();
            port_log("rom: %s (next to the game)%s\n", rom, cached ? " (verified before)" : "");
#else
        } else if (open_rom(rom, 1, err, sizeof(err), &cached)) {
            finish_screen(); /* SDL stays up; gfx init sets its own video mode */
            port_log("rom: %s (in the game folder)%s\n", rom, cached ? " (verified before)" : "");
#endif
            rebuild_done();
            return 1;
        } else {
            port_log("rom: refused the game folder's copy (%s); searching the card\n", err);
            snprintf(why, sizeof(why), "%.200s: %s", rom, /* "<path>: <reason>", shown if the card has none */
                     strstr(err, "SHA-1") ? "its SHA-1 check failed" : "it could not be read");
        }
    }
    if (!find(rom, sizeof(rom), why, sizeof(why))) {
        port_log("rom: none found%s%s%s%s\n", why[0] ? " - F-Zero X rejected: " : "", why,
                 sOther[0] ? " - last other file: " : "", sOther);
        if (why[0]) { /* "<path>: <reason>", an F-Zero X dump (or FZX_ROM) */
            const char* r = strrchr(why, ':');
            set_sub(why, r ? (size_t) (r - why) : 0);
            fail_screen("No usable F-Zero X ROM.", r ? r + 2 : why, "It must be the US (NTSC) cart.");
        } else {
            fail_screen("No F-Zero X ROM found.", "Copy your US (NTSC) .z64/.n64/.v64",
                        "to the SD card and start again.");
        }
        return 0;
    }
    if (!open_rom(rom, 0, err, sizeof(err), &cached)) {
        port_log("rom: %s\n", err);
        fail_screen("No usable F-Zero X ROM.",
                    strstr(err, "SHA-1") ? "Its SHA-1 check failed." : "The file could not be read.",
                    strstr(err, "SHA-1") ? "It must be the US (NTSC) cart." : "See the log.");
        return 0;
    }
    if (sShown) screen_progress("Saving the result", 99);
    port_write_file_atomic(port_data_path("rompath.txt"), rom, strlen(rom));
    finish_screen(); /* SDL stays up; gfx init sets its own video mode */
    port_log("rom: %s%s\n", rom, cached ? " (verified before)" : "");
    rebuild_done();
    return 1;
}
