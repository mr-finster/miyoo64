/* hh_card.c: the handheld's card side, before anything else runs (hh_boot.c main).
 *
 * Everything the port keeps is in HH_DEFAULT_SAVE_DIR (on the Miyoo Mini data/ next to the
 * executable; the card root the search starts from is /mnt/SDCARD):
 *   console.log    this session's output (the two before it: console.1.log, console.2.log)
 *   debug.cfg      test knobs, one name=value per line (intres=85 renders the 3D at 85%,
 *                  args=-level_33 boots the Dam, ...): a lowercase name is BK_<NAME>, as OoT's
 *                  short keys; the BK_NAME=value form works too. '#' starts a comment
 *   banjo.z64  the player's ROM in z64 order, written once from the dump found on the card
 *                  (not needed with a rom.z64 in the game's folder)
 *   rom_check.ok   the size and time of the game folder's rom.z64 whose SHA-1 checked out
 *   rebuild        (the player's, any content) do the first start's job again at the next start: the
 *                  ROM found, checked and copied anew (or the game folder's checked), the tables filled, all on
 *                  the screen; the file is removed once that has worked (hh_card_rebuild_done)
 *
 * The ROM in the game's folder comes first. The packer's --rom (miyoo/tools/miyoo_pack.py)
 * adds the player's .z64 to the game's folder as rom.z64, next to the executable,
 * stored uncompressed. A launcher may run the bare name from
 * there, so it is found through /proc/self/exe (as recipes.bin), never argv[0]. The game maps
 * it where it is (hh_boot.c hh_load_rom), so nothing is copied and nothing is written next to
 * it. Its SHA-1 is checked once per rom.z64 (its size and time are kept in rom_check.ok);
 * after that only its size, as banjo.z64's, then the header and the romtables CRC as for any
 * ROM. Missing, or failing the check: the card as before.
 *
 * The ROM finder (miyoo_romfind.h: the game's folder, the path that worked last time, Roms/ and its
 * folders, then the card two folders deep) looks for a Banjo-Kazooie (USA 1.0) dump (.z64/.n64/.v64, any
 * name, any byte order) and accepts only the one whose SHA-1 in z64 order is the decomp's
 * (banjo.us.v10.sha1); any other file is passed over and the search goes on.
 *
 * The work (the first start's search, check and copy; a new rom.z64's one check) is shown on the
 * screen as the other ports show theirs (hh_romscreen.c): "Checking your ROM" with a bar, a step a
 * megabyte read, hashed and written, and the errors in words until a button (no ROM: where to put
 * it; a Banjo-Kazooie dump refused: why, its country code naming the PAL or Japanese cart; the copy
 * can't be written). The screen then stays up through the data tables' fill (hh_romtables.c) and
 * ends on "Done" (hh_romscreen_finish, hh_boot.c). A normal start opens no screen. The log keeps every
 * line it had. */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ultra64.h>
#include "hh_plat.h"
#include "hh_romscreen.h"
#include "miyoo_romfind.h"

#ifndef HH_CARD_ROOT
#define HH_CARD_ROOT "/mnt" /* the Miyoo Mini's is hh_plat.h's */
#endif
#define HH_ROM_SIZE 0x1000000u
#define HH_LOCAL_ROM "rom.z64"       /* the ROM in the game's folder, next to the executable */
#define HH_LOCAL_ROM_OK "rom_check.ok" /* in the save folder: "<size> <time> <sha1>" of the rom.z64 that checked out */
#if defined(TARGET_MIYOO)
/* the Miyoo Mini: rom.z64 in the game's folder (App/Banjo-Kazooie, miyoo/tools/miyoo_pack.py --rom) */
#define HH_LOCAL_WHERE " (in the game's folder)"
#define HH_LOCAL_NEW "New rom.z64: checking it"
#define HH_LOCAL_NEW2 "once, before the game data."
#define HH_LOCAL_BAD "The rom.z64 can't be used."
#define HH_LOCAL_FIX "Use the USA ROM as rom.z64."
#else
#define HH_LOCAL_WHERE " (next to the executable)"
#define HH_LOCAL_NEW "New rom.z64: checking it"
#define HH_LOCAL_NEW2 "once, before the game data."
#define HH_LOCAL_BAD "The rom.z64 can't be used."
#define HH_LOCAL_FIX "Use the USA ROM as rom.z64."
#endif
static const char kRomSha1[] = "1fe1632098865f639e22c11b9a81ee8f29c75d7a"; /* banjo-kazooie baserom.us.v10.z64 */
#ifdef HH_CARD_TESTSHA
/* test builds only (EXTRA_CFLAGS=-DHH_CARD_TESTSHA): BK_CARD_TESTSHA=<sha1> takes a synthetic
 * image for the dump, so the first-start screens can be run without the ROM. Never in a release. */
#define kRomSha1 (getenv("BK_CARD_TESTSHA") ? getenv("BK_CARD_TESTSHA") : kRomSha1)
#endif

/* ------------------------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-1)                                                                    */

typedef struct {
    u32 h[5];
    u64 n;
    u8 buf[64];
    u32 fill;
} HhSha1;

#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

static void hh_sha1_block(HhSha1 *s, const u8 *p)
{
    u32 w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

/* ------------------------------------------------------------------------------------ */
/* The first-start screen's bar (hh_romscreen.c): one step a megabyte read, hashed and written. */

#define HH_MB 0x100000u
static const char HH_FOOT_CARD[] = "This happens once.";  /* the other ports' footer */
static const char HH_FOOT_LOCAL[] = "This happens once.";
static int sLocalCheck; /* the check on screen is a new rom.z64's, not the first start's */

static struct {
    char sub[64];      /* the ROM's file name, under the title */
    const char *foot;  /* NULL: no screen (the log only) */
    const char *stage; /* "Reading", "Checking", "Saving a copy", "Finishing the copy" */
    u32 done, total;   /* steps */
} sProg;

static void hh_card_draw(void)
{
    char st[64];
    if (sProg.foot == NULL)
        return;
    snprintf(st, sizeof(st), "%s...", sProg.stage);
    /* the other ports' words (ssb64 romdata.c, sf64 port_romimage.c); a new rom.z64's check says so */
    hh_romscreen_progress(sProg.sub, sLocalCheck ? HH_LOCAL_NEW : "First start: building the game",
                          sLocalCheck ? HH_LOCAL_NEW2 : "data from your ROM.", st,
                          sProg.total ? (int)(sProg.done * HH_CARD_PCT / sProg.total) : 0, sProg.foot);
}

static void hh_card_step(void)
{
    sProg.done++;
    hh_card_draw();
}

static void hh_card_stage(const char *stage)
{
    sProg.stage = stage;
    hh_card_draw();
}

static void hh_sha1(const u8 *p, u32 n, char hex[41])
{
    HhSha1 s = { { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u }, 0, { 0 }, 0 };
    u8 tail[128];
    u32 rest, i, k;
    u64 bits = (u64)n * 8;
    for (i = 0; i + 64 <= n; i += 64) {
        hh_sha1_block(&s, p + i);
        if (((i + 64) & (HH_MB - 1)) == 0)
            hh_card_step(); /* the bar, a megabyte at a time */
    }
    rest = n - i;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    k = rest + 1 + 8 <= 64 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[k - 1 - i] = (u8)(bits >> (8 * i));
    hh_sha1_block(&s, tail);
    if (k == 128)
        hh_sha1_block(&s, tail + 64);
    for (i = 0; i < 5; i++)
        sprintf(hex + i * 8, "%08x", s.h[i]);
}

/* ------------------------------------------------------------------------------------ */

static void hh_card_log(const char *dir)
{
    char a[600], b[600];
    snprintf(a, sizeof(a), "%s/console.1.log", dir);
    snprintf(b, sizeof(b), "%s/console.2.log", dir);
    rename(a, b);
    snprintf(b, sizeof(b), "%s/console.log", dir);
    rename(b, a);
    if (freopen(b, "w", stdout) != NULL) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        dup2(fileno(stdout), 2);
    }
}

/* debug.cfg's key: "BK_NAME" as it is, or the short form OoT's file uses, a lowercase name without
 * the prefix ("rspyield" = BK_RSPYIELD); NULL for anything else (the line is skipped, as before).
 * key: the line up to the '=', NUL-terminated; out: room for the BK_ name. */
static const char *hh_card_cfg_key(const char *key, char *out, size_t outlen)
{
    size_t i, n = strlen(key);
    int lower = 0;
    if (strncmp(key, "BK_", 3) == 0)
        return key;
    if (n == 0 || n + 4 > outlen)
        return NULL;
    for (i = 0; i < n; i++) {
        if (!islower((unsigned char)key[i]) && !isdigit((unsigned char)key[i]) && key[i] != '_')
            return NULL;
        lower |= islower((unsigned char)key[i]) != 0;
    }
    if (!lower) /* "2dbilin" is BK_2DBILIN; a name needs a letter */
        return NULL;
    memcpy(out, "BK_", 3);
    for (i = 0; i <= n; i++)
        out[3 + i] = (char)toupper((unsigned char)key[i]);
    return out;
}

static void hh_card_cfg(const char *dir)
{
    char path[600], line[512], name[80];
    const char *key;
    FILE *f;
    snprintf(path, sizeof(path), "%s/debug.cfg", dir);
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq, *v, *end;
        if (line[0] == '#' || (eq = strchr(line, '=')) == NULL)
            continue;
        *eq = 0;
        if ((key = hh_card_cfg_key(line, name, sizeof(name))) == NULL)
            continue;
        v = eq + 1;
        end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;
        if (end - v >= 2 && v[0] == '"' && end[-1] == '"') {
            end[-1] = 0;
            v++;
        }
        setenv(key, v, getenv("BK_BENCH_CHILD") == NULL); /* a bench run keeps its variant's values */
        printf("[card] debug.cfg: %s=%s\n", key, v);
    }
    fclose(f);
}

/* z64 order in place; 0 if it is not an N64 ROM */
static int hh_card_z64(u8 *p, u32 n)
{
    u32 i;
    if (p[0] == 0x80 && p[1] == 0x37)
        return 1;
    if (p[0] == 0x37 && p[1] == 0x80) {
        for (i = 0; i + 1 < n; i += 2) { u8 t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
        return 1;
    }
    if (p[0] == 0x40 && p[1] == 0x12) {
        for (i = 0; i + 3 < n; i += 4) {
            u8 t0 = p[i], t1 = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = t1; p[i + 3] = t0;
        }
        return 1;
    }
    return 0;
}

/* Why the last Banjo-Kazooie dump was refused, in words for the screen (NULL: none was; any other file
 * is refused in the log only, as Smash's B9: another game's ROM on the card says nothing), and the
 * same for the game folder's rom.z64. */
static const char *sReject, *sLocalReject;
static char sRejectName[64];

static const char *hh_card_base(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* The dump this port needs (an N64 header in any byte order, Banjo-Kazooie's title, 12 MB, the USA
 * cart, the decomp's SHA-1 in z64 order): its z64 image (malloc'd), or NULL and why not (why: the
 * log; *say: the screen's sentence when the file is a Banjo-Kazooie dump, else NULL). The header comes
 * first (64 bytes), so another game's ROM is passed over without reading 12 MB. foot: the screen's
 * footer (NULL: no screen); steps: the bar's steps after the read and the hash (a copy to write). */
static u8 *hh_card_check(const char *path, const char **why, const char **say, const char *foot, u32 steps)
{
    struct stat st;
    FILE *f;
    u8 *p, hdr[0x40];
    char hex[41];
    u32 got;
    *say = NULL;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "not a file";
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        *why = f == NULL ? "can't be read" : "not an N64 ROM";
        if (f) fclose(f);
        return NULL;
    }
    if (!hh_card_z64(hdr, sizeof(hdr)) || memcmp(hdr + 0x20, "Banjo-Kazooie", 13) != 0) {
        *why = "not a Banjo-Kazooie ROM";
        fclose(f);
        return NULL;
    }
    if ((u32)st.st_size != HH_ROM_SIZE) {
        *why = "not a 16 MB file";
        *say = "It is not a 16 MB ROM.";
        fclose(f);
        return NULL;
    }
    if (hdr[0x3E] == 'E' && hdr[0x3F] != 0) {
        *why = "the USA 1.1 dump, not 1.0";
        *say = "It is the USA 1.1 version (this needs 1.0).";
        fclose(f);
        return NULL;
    }
    if (hdr[0x3E] != 'E') { /* the cart's country code: E USA, P Europe, J Japan */
        printf("[card] %s: a Banjo-Kazooie ROM, but not the USA one this port is built for (country code '%c')\n",
               path, hdr[0x3E] >= 0x20 && hdr[0x3E] < 0x7F ? hdr[0x3E] : '?');
        *why = "not the Banjo-Kazooie (USA 1.0) dump";
        *say = hdr[0x3E] == 'P' ? "It is the European (PAL) version." :
               hdr[0x3E] == 'J' ? "It is the Japanese version." : "It is not the USA version.";
        fclose(f);
        return NULL;
    }
    snprintf(sProg.sub, sizeof(sProg.sub), "%s%s", hh_card_base(path), sLocalCheck ? HH_LOCAL_WHERE : "");
    sProg.foot = foot;
    sProg.done = 0;
    sProg.total = 2 * (HH_ROM_SIZE / HH_MB) + steps;
    hh_card_stage("Reading");
    p = malloc(HH_ROM_SIZE);
    if (p == NULL) {
        *why = "out of memory";
        *say = "It could not be read (out of memory).";
        fclose(f);
        return NULL;
    }
    rewind(f);
    for (got = 0; got < HH_ROM_SIZE; got += HH_MB) {
        if (fread(p + got, 1, HH_MB, f) != HH_MB)
            break;
        hh_card_step();
    }
    fclose(f);
    if (got != HH_ROM_SIZE || !hh_card_z64(p, HH_ROM_SIZE)) {
        *why = "can't be read";
        *say = "It could not be read.";
        free(p);
        return NULL;
    }
    hh_card_stage("Checking");
    hh_sha1(p, HH_ROM_SIZE, hex);
    if (strcmp(hex, kRomSha1) != 0) {
        printf("[card] %s: a Banjo-Kazooie ROM, but not the USA one this port is built for (SHA-1 %s)\n", path, hex);
        *why = "not the Banjo-Kazooie (USA 1.0) dump";
        *say = "It is damaged or modified.";
        free(p);
        return NULL;
    }
    return p;
}

/* A dump that checks out, written as out (z64 order): 1; not the dump: 0. Can't be written: the
 * error screen (another dump on the card would fail the same way). */
static int hh_card_try(const char *path, const char *out)
{
    FILE *f;
    const char *why, *say;
    u8 *p = hh_card_check(path, &why, &say, HH_FOOT_CARD, HH_ROM_SIZE / HH_MB + 1);
    char tmp[640], err[96];
    int ok = 0;
    u32 put;
    if (p == NULL) {
        printf("[card] %s: %s, passed over\n", path, why);
        if (say != NULL) {
            sReject = say;
            snprintf(sRejectName, sizeof(sRejectName), "%s", hh_card_base(path));
        }
        return 0;
    }
    hh_card_stage("Saving a copy");
    snprintf(tmp, sizeof(tmp), "%s.tmp", out);
    f = fopen(tmp, "wb");
    if (f != NULL) {
        ok = 1;
        for (put = 0; ok && put < HH_ROM_SIZE; put += HH_MB) {
            ok = fwrite(p + put, 1, HH_MB, f) == HH_MB;
            hh_card_step();
        }
        hh_card_stage("Finishing the copy"); /* the flush, fsync, rename and folder sync: no megabyte, but time */
        ok = ok && fflush(f) == 0;
        ok = ok && fsync(fileno(f)) == 0; /* whole on the card before the rename (was unchecked) */
        ok = ok && rename(tmp, out) == 0;
    }
    if (!ok)
        snprintf(err, sizeof(err), "%s", hh_last_error()); /* the failed call's */
    if (ok) {
        /* The rename onto the card, as eeprom.bin's (hh_hw.c hh_eeprom_sync_rename; the 2026-10-01
         * consistency pass after the SSB64 port cd5128c, and the DKR port 9c91d40 for its asset cache):
         * the file, still open, then its folder. On vfat a rename to a new name makes the directory
         * slot with cluster 0 and size 0 and only marks the inode dirty; until its writeback (~30 s)
         * a power cut left a 0-byte banjo.z64 (the next start converts again) and its 12 MB
         * held in the FAT for no file until a disk check. */
        char dir[640], *slash;
        int d;
        fsync(fileno(f));
        snprintf(dir, sizeof(dir), "%s", out);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = 0;
            d = open(dir[0] ? dir : "/", O_RDONLY);
            if (d >= 0) {
                fsync(d);
                close(d);
            }
        }
    }
    if (ok)
        hh_card_step(); /* the last step: the copy is whole on the card */
    if (f != NULL)
        fclose(f); /* after the rename: its result never mattered after a good fsync */
    free(p);
    if (!ok) {
        printf("[card] %s: verified, but writing %s failed (%s)\n", path, out, err);
        remove(tmp);
        hh_romscreen_fail(sProg.sub, "Can't save the ROM copy:", err, "Is the SD card full or locked?");
    }
    printf("[card] %s: Banjo-Kazooie (USA 1.0) verified, saved as %s\n", path, out);
    return ok;
}

/* The card search (miyoo_romfind.h, the same in every Miyoo64 game): the game's folder (any name but rom.z64, which
 * hh_card_local_rom checked), the save folder (banjo.z64, the copy, never), the path that worked last time, Roms/, the
 * card two folders deep. Each candidate goes through hh_card_try: the header, the SHA-1, the copy; one that fails is
 * passed over. */
static int hh_card_mrf_try(const char *path, void *out) { return hh_card_try(path, (const char *)out); }
static int hh_card_search(const char *save, const char *out)
{
    static const char *const own[] = { HH_LOCAL_ROM, "banjo.z64", NULL };
    char exe[512];
    MrfSearch *s = calloc(1, sizeof(*s));
    int found;
    if (s == NULL)
        return 0;
    s->app_dir = mrf_exe_dir(exe, sizeof(exe));
    s->save_dir = save;
    s->own = own;
    s->root_env = "BK_CARD_ROOT";
    s->try_rom = hh_card_mrf_try;
    s->ctx = (void *)out;
    found = mrf_find(s);
    free(s);
    return found;
}

/* Handheld/.banjo/rebuild (the player's way to redo the first start, README): set by hh_card_setup when the
 * file is there and the game itself is starting (hh_card_allow_rebuild: not the bench's parent or children). */
static int sRebuildAllowed, sRebuild;
static char sRebuildPath[600];
void hh_card_allow_rebuild(void) { sRebuildAllowed = getenv("BK_BENCH_CHILD") == NULL; }

/* hh_boot.c, after the tables' fill and "Done": the job worked, so the request is used up. */
void hh_card_rebuild_done(void)
{
    if (!sRebuild)
        return;
    sRebuild = 0;
    if (remove(sRebuildPath) == 0)
        printf("[card] rebuild: done, %s removed\n", sRebuildPath);
    else
        printf("[card] rebuild: done, but %s can't be removed (%s): the job runs again next start\n", sRebuildPath,
               hh_last_error());
}

/* rom.z64 in the game's folder (the head of this file), if it is the dump: 1 and its path in out.
 * Only read: the record that its SHA-1 checked out goes to dir (the save folder). */
static int hh_card_local_rom(const char *dir, char *out, size_t len)
{
    char self[512], ok[600], line[128], sha[48];
    unsigned long long size;
    long long mtime;
    const char *why, *say;
    struct stat st;
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    char *slash;
    FILE *f;
    u8 *p;
    if (n <= 0 || n >= (ssize_t)sizeof(self) - 1) /* a full buffer may be cut short */
        return 0;
    self[n] = 0;
    slash = strrchr(self, '/');
    if (slash == NULL)
        return 0;
    *slash = 0;
    if (snprintf(out, len, "%s/" HH_LOCAL_ROM, self) >= (int)len || stat(out, &st) != 0)
        return 0; /* no rom.z64 there: the card, as before */
    if (!S_ISREG(st.st_mode) || (unsigned long long)st.st_size != HH_ROM_SIZE) {
        printf("[card] %s: not a 16 MB file, passed over for the card\n", out);
        sLocalReject = "It is not a 16 MB ROM.";
        return 0;
    }
    /* checked before: the same size and time as the rom.z64 whose SHA-1 was the dump's */
    snprintf(ok, sizeof(ok), "%s/" HH_LOCAL_ROM_OK, dir);
    f = fopen(ok, "r");
    if (f != NULL) {
        int same = fgets(line, sizeof(line), f) != NULL &&
                   sscanf(line, "%llu %lld %47s", &size, &mtime, sha) == 3 &&
                   size == (unsigned long long)st.st_size && mtime == (long long)st.st_mtime &&
                   strcmp(sha, kRomSha1) == 0;
        fclose(f);
        if (same)
            return 1;
    }
    printf("[card] a new rom.z64 in the game's folder: checking %s once\n", out);
    sLocalCheck = 1;
    p = hh_card_check(out, &why, &say, HH_FOOT_LOCAL, 0);
    sLocalCheck = 0;
    if (p == NULL) {
        printf("[card] %s: %s, passed over for the card\n", out, why);
        sLocalReject = say ? say : "It is not a Banjo-Kazooie ROM.";
        return 0;
    }
    free(p);
    f = fopen(ok, "w"); /* a torn or lost record only means one more check */
    if (f != NULL) {
        int w = fprintf(f, "%llu %lld %s\n", (unsigned long long)st.st_size, (long long)st.st_mtime, kRomSha1);
        if (fclose(f) != 0 || w < 0)
            f = NULL;
    }
    if (f == NULL)
        printf("[card] %s can't be written: the ROM in the game's folder is checked again next start\n", ok);
    return 1;
}

/* The log and the knobs; then the ROM to run: rom.z64 in the game's folder where it is, else
 * banjo.z64, made from the card's dump the first time. Only the work (the game folder's one check, the
 * first start) opens the screen (hh_romscreen.c), and it is down again before this returns. */
const char *hh_card_setup(void)
{
    static char rom[600];
    const char *dir = HH_DEFAULT_SAVE_DIR;
    struct stat st;
    int found;
    mkdir(dir, 0755);
    if (getenv("BK_BENCH_CHILD") == NULL) /* a bench run appends to its parent's log */
        hh_card_log(dir);
    hh_card_cfg(dir);
    if (getenv("BK_ROM") != NULL)
        return getenv("BK_ROM");
    snprintf(sRebuildPath, sizeof(sRebuildPath), "%s/rebuild", dir);
    if (sRebuildAllowed && stat(sRebuildPath, &st) == 0) {
        char ok[600];
        sRebuild = 1;
        printf("[card] rebuild: %s found, the first start's job runs again\n", sRebuildPath);
        snprintf(ok, sizeof(ok), "%s/" HH_LOCAL_ROM_OK, dir);
        remove(ok); /* the game folder's rom.z64 is checked again (a missing record only means one more check) */
    }
    if (hh_card_local_rom(dir, rom, sizeof(rom))) {
        printf("[card] rom: %s" HH_LOCAL_WHERE "\n", rom);
        hh_romscreen_card_done();
        return rom;
    }
    snprintf(rom, sizeof(rom), "%s/banjo.z64", dir);
    if (!sRebuild && stat(rom, &st) == 0 && (u32)st.st_size == HH_ROM_SIZE) {
        printf("[card] rom: %s\n", rom);
        hh_romscreen_card_done(); /* the game folder's rom.z64 was refused on the screen */
        return rom;
    }
    printf("[card] first start: looking for your Banjo-Kazooie (USA 1.0) ROM on the card\n");
    /* the first frame has every line the later ones have (the name line says where it looks), so the
     * layout holds from the first frame to Done */
    hh_romscreen_progress("Looking on the SD card", "First start: building the game", "data from your ROM.",
                          "Looking for your ROM...", 0, HH_FOOT_CARD);
    found = hh_card_search(dir, rom);
    if (!found && sRebuild && stat(rom, &st) == 0) {
        /* a rebuild with the dump gone from the card (the README lets it go): the copy itself is checked in full on
         * the screen (it is replaced only by a dump that checks out: hh_card_try writes a .tmp and renames) */
        const char *why, *say;
        u8 *p = hh_card_check(rom, &why, &say, HH_FOOT_CARD, 0);
        if (p == NULL) {
            printf("[card] rebuild: no dump on the card and %s: %s\n", rom, why);
            hh_romscreen_fail("banjo.z64", "The ROM copy failed its check.", say ? say : "It could not be read.",
                              "Copy your USA ROM to the SD card.");
        }
        free(p);
        printf("[card] rebuild: no dump on the card; %s verified in place\n", rom);
        hh_romscreen_card_done();
        return rom;
    }
    if (!found) {
        printf("FATAL: no Banjo-Kazooie (USA 1.0) ROM found. Copy your dump (.z64, .n64 or .v64) to the\n"
               "card, at most two folders deep (the root, Games/, Roms/N64/, ...), and start again.\n");
        if (sReject != NULL)
            hh_romscreen_fail(sRejectName, "This Banjo-Kazooie ROM can't be used.", sReject,
                              "This port needs Banjo-Kazooie (USA 1.0).");
        if (sLocalReject != NULL)
            hh_romscreen_fail(HH_LOCAL_ROM HH_LOCAL_WHERE, HH_LOCAL_BAD, sLocalReject, HH_LOCAL_FIX);
        hh_romscreen_fail(NULL, "No Banjo-Kazooie ROM found.", "Copy your USA ROM (.z64/.v64/.n64)",
                          "to the SD card, up to 2 folders deep.");
    }
    printf("[card] rom: %s\n", rom);
    hh_romscreen_card_done();
    return rom;
}
