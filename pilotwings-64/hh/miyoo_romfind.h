/* miyoo_romfind.h: where a Miyoo64 game looks for the player's ROM (the same file in every game of the release).
 *
 * The order, the first ROM that checks out wins:
 *   1. the game's folder (App/<Game>/: rom.z64 or any other name), then its data folder;
 *   2. the path that worked last time (data/rom_path.txt, written by this search; mrf_known: that ROM, unchanged, at a
 *      normal start without any search);
 *   3. Roms/ on the card and its folders, two deep (Roms/N64/, Roms/<anything>/<anything>/);
 *   4. the whole card, two folders deep (the folders above passed over: they were searched already).
 * Any file name with a ROM extension (.z64 .v64 .n64; .bin .rom .u64 for the games that took them before), any byte
 * order: the game's own check (try_rom) reads the header and the SHA-1 and says yes or no; a no keeps the search going,
 * so a wrong version, another game's ROM or a damaged copy found first is passed over. The game's own files (its data
 * pack, caches, recipes and saves: `own`) are never taken for a ROM, whatever their extension.
 * <GAME>_CARD_ROOT=<dir> (root_env) searches <dir> instead of /mnt/SDCARD (tests). */
#ifndef MIYOO_ROMFIND_H
#define MIYOO_ROMFIND_H
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MRF_CARD_ROOT "/mnt/SDCARD"
#define MRF_REMEMBER "rom_path.txt"
#define MRF_UNUSED __attribute__((unused))

/* 1: `path` is the game's ROM (its header and SHA-1 checked out): the search stops there. 0: passed over. */
typedef int (*MrfTry)(const char *path, void *ctx);

typedef struct {
    const char *app_dir;    /* the game's folder, or NULL */
    const char *save_dir;   /* the game's data folder (rom_path.txt), or NULL */
    const char *const *own; /* file-name prefixes of the game's own files (NULL-terminated), case-insensitive */
    int bin_ok;             /* also .bin, .rom, .u64 */
    const char *root_env;   /* "<GAME>_CARD_ROOT" */
    MrfTry try_rom;
    void *ctx;
    char found[PATH_MAX];   /* the ROM, when mrf_find returns 1 */
    /* private */
    char done[4][PATH_MAX]; /* folders searched already (real paths) */
    int ndone;
    char tried[PATH_MAX];   /* the remembered path, tried already */
} MrfSearch;

/* ------------------------------------------------------------------ byte orders */
enum { MRF_NONE = -1, MRF_Z64 = 0, MRF_V64 = 1, MRF_N64 = 2 };

static MRF_UNUSED int mrf_order(const unsigned char *h) {
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) return MRF_Z64;
    if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) return MRF_V64;
    if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) return MRF_N64;
    return MRF_NONE;
}

/* to z64 order in place (n a multiple of 4) */
static MRF_UNUSED void mrf_to_z64(unsigned char *p, size_t n, int order) {
    size_t i;
    if (order == MRF_V64) {
        for (i = 0; i + 1 < n; i += 2) { const unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    } else if (order == MRF_N64) {
        for (i = 0; i + 3 < n; i += 4) {
            const unsigned char a = p[i], b = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a;
        }
    }
}

/* The first 64 bytes in z64 order, the byte order and the size. 0: not a file, or not an N64 ROM. */
static MRF_UNUSED int mrf_header(const char *path, unsigned char h[64], int *order, uint64_t *size) {
    struct stat st;
    FILE *f;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if ((f = fopen(path, "rb")) == NULL) return 0;
    if (fread(h, 1, 64, f) != 64) { fclose(f); return 0; }
    fclose(f);
    if ((*order = mrf_order(h)) == MRF_NONE) return 0;
    mrf_to_z64(h, 64, *order);
    *size = (uint64_t) st.st_size;
    return 1;
}

/* ------------------------------------------------------------------ SHA-1 (FIPS 180-1) */
typedef struct { uint32_t h[5]; uint64_t n; unsigned char buf[64]; uint32_t fill; } MrfSha1;

static MRF_UNUSED void mrf_sha1_block(MrfSha1 *s, const unsigned char *p) {
#define MRF_ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], t;
    int i;
    for (i = 0; i < 16; i++) w[i] = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 | (uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 80; i++) w[i] = MRF_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = MRF_ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = MRF_ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
#undef MRF_ROL
}
static MRF_UNUSED void mrf_sha1_init(MrfSha1 *s) {
    memset(s, 0, sizeof(*s));
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu; s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
}
static MRF_UNUSED void mrf_sha1_update(MrfSha1 *s, const unsigned char *p, size_t n) {
    s->n += n;
    while (n > 0) {
        if (s->fill == 0 && n >= 64) { mrf_sha1_block(s, p); p += 64; n -= 64; continue; }
        {
            const size_t k = 64 - s->fill < n ? 64 - s->fill : n;
            memcpy(s->buf + s->fill, p, k);
            s->fill += (uint32_t) k; p += k; n -= k;
            if (s->fill == 64) { mrf_sha1_block(s, s->buf); s->fill = 0; }
        }
    }
}
static MRF_UNUSED void mrf_sha1_hex(MrfSha1 *s, char hex[41]) {
    const uint64_t bits = s->n * 8;
    const uint32_t padn = (s->fill < 56 ? 56 : 120) - s->fill;
    unsigned char pad[72];
    int i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padn + i] = (unsigned char) (bits >> (56 - 8 * i));
    mrf_sha1_update(s, pad, padn + 8);
    for (i = 0; i < 5; i++) snprintf(hex + i * 8, 9, "%08x", (unsigned) s->h[i]);
}

/* The SHA-1 of the whole file in z64 order (a megabyte at a time; progress(ctx, done, total) after each, may be NULL).
 * 0: the file can't be read whole. */
static MRF_UNUSED int mrf_sha1_file(const char *path, int order, char hex[41], void (*progress)(void *, uint64_t, uint64_t),
                                    void *ctx) {
    struct stat st;
    MrfSha1 s;
    unsigned char *buf;
    uint64_t done = 0;
    FILE *f;
    if (stat(path, &st) != 0 || (f = fopen(path, "rb")) == NULL) return 0;
    if ((buf = (unsigned char *) malloc(1u << 20)) == NULL) { fclose(f); return 0; }
    mrf_sha1_init(&s);
    while (done < (uint64_t) st.st_size) {
        const size_t want = (uint64_t) st.st_size - done < (1u << 20) ? (size_t) ((uint64_t) st.st_size - done) : (1u << 20);
        if (fread(buf, 1, want, f) != want) break;
        mrf_to_z64(buf, want & ~(size_t) 3, order);
        mrf_sha1_update(&s, buf, want);
        done += want;
        if (progress) progress(ctx, done, (uint64_t) st.st_size);
    }
    free(buf);
    fclose(f);
    if (done != (uint64_t) st.st_size) return 0;
    mrf_sha1_hex(&s, hex);
    return 1;
}

/* ------------------------------------------------------------------ the search */
/* the executable's folder (/proc/self/exe, never argv[0]: a launcher may exec the bare name); "" when unknown */
static MRF_UNUSED const char *mrf_exe_dir(char *out, size_t n) {
    const ssize_t k = readlink("/proc/self/exe", out, n - 1);
    char *slash;
    if (k <= 0 || (size_t) k >= n - 1) { out[0] = 0; return out; }
    out[k] = 0;
    if ((slash = strrchr(out, '/')) != NULL) *slash = 0; else out[0] = 0;
    return out;
}

static MRF_UNUSED const char *mrf_base(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* a ROM's name: a ROM extension, not hidden (._x.z64: a Mac's resource fork), not one of the game's own files */
static MRF_UNUSED int mrf_name_ok(const MrfSearch *s, const char *name) {
    const char *dot = strrchr(name, '.');
    char e[5];
    int i;
    if (name[0] == '.' || dot == NULL || strlen(dot) != 4) return 0;
    for (i = 0; i < 4; i++) e[i] = (char) tolower((unsigned char) dot[i]);
    e[4] = 0;
    if (strcmp(e, ".z64") && strcmp(e, ".v64") && strcmp(e, ".n64") &&
        !(s->bin_ok && (!strcmp(e, ".bin") || !strcmp(e, ".rom") || !strcmp(e, ".u64"))))
        return 0;
    for (i = 0; s->own && s->own[i]; i++)
        if (strncasecmp(name, s->own[i], strlen(s->own[i])) == 0) return 0;
    return 1;
}

static MRF_UNUSED int mrf_done_dir(const MrfSearch *s, const char *dir) {
    char real[PATH_MAX];
    int i;
    if (realpath(dir, real) == NULL) return 0;
    for (i = 0; i < s->ndone; i++)
        if (strcmp(real, s->done[i]) == 0) return 1;
    return 0;
}
static MRF_UNUSED void mrf_mark_dir(MrfSearch *s, const char *dir) {
    if (dir && s->ndone < 4 && realpath(dir, s->done[s->ndone]) != NULL) s->ndone++;
}

static MRF_UNUSED int mrf_try(MrfSearch *s, const char *path) {
    if (s->tried[0] && strcmp(path, s->tried) == 0) return 0;
    if (!s->try_rom(path, s->ctx)) return 0;
    snprintf(s->found, sizeof(s->found), "%s", path);
    return 1;
}

static MRF_UNUSED int mrf_scan_dir(MrfSearch *s, const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (d == NULL) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        const size_t dl = strlen(dir);
        if (!mrf_name_ok(s, e->d_name)) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", e->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        found = mrf_try(s, path);
    }
    closedir(d);
    return found;
}

/* `dir`'s files, then its folders down to `depth` levels; folders searched already passed over */
static MRF_UNUSED int mrf_scan_tree(MrfSearch *s, const char *dir, int depth) {
    DIR *d;
    struct dirent *e;
    if (mrf_done_dir(s, dir)) return 0;
    if (mrf_scan_dir(s, dir)) return 1;
    if (depth <= 0 || (d = opendir(dir)) == NULL) return 0;
    while ((e = readdir(d)) != NULL) {
        char sub[PATH_MAX];
        struct stat st;
        const size_t dl = strlen(dir);
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", e->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (mrf_scan_tree(s, sub, depth - 1)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}

/* data/rom_path.txt: the ROM found last time, its path, then its size and time when it checked out */
static MRF_UNUSED int mrf_recall_dir(const char *save_dir, char *out, size_t n, long long *size, long long *mtime) {
    char p[PATH_MAX], line[64];
    FILE *f;
    size_t l;
    *size = *mtime = -1;
    if (!save_dir || !save_dir[0]) return 0;
    snprintf(p, sizeof(p), "%s/" MRF_REMEMBER, save_dir);
    if ((f = fopen(p, "r")) == NULL) return 0;
    if (fgets(out, (int) n, f) == NULL) { fclose(f); return 0; }
    if (fgets(line, sizeof(line), f) == NULL || sscanf(line, "%lld %lld", size, mtime) != 2) *size = *mtime = -1;
    fclose(f);
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}
static MRF_UNUSED int mrf_recall(const MrfSearch *s, char *out, size_t n) {
    long long size, mtime;
    return mrf_recall_dir(s->save_dir, out, n, &size, &mtime);
}

/* The ROM found and checked at an earlier start, unchanged since (the same size and time): 1 and its path, to use
 * without the search or another SHA-1. A game asks this first at a normal start; 0 sends it to mrf_find. */
static MRF_UNUSED int mrf_known(const char *save_dir, char *out, size_t n) {
    long long size, mtime;
    struct stat st;
    if (!mrf_recall_dir(save_dir, out, n, &size, &mtime)) return 0;
    if (stat(out, &st) != 0 || !S_ISREG(st.st_mode) || (long long) st.st_size != size || (long long) st.st_mtime != mtime) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

static MRF_UNUSED void mrf_remember(const MrfSearch *s, const char *path) {
    char p[PATH_MAX], tmp[PATH_MAX + 8], old[PATH_MAX];
    struct stat st;
    long long size, mtime;
    FILE *f;
    if (!s->save_dir || !s->save_dir[0] || stat(path, &st) != 0) return;
    if (mrf_recall_dir(s->save_dir, old, sizeof(old), &size, &mtime) && strcmp(old, path) == 0 &&
        size == (long long) st.st_size && mtime == (long long) st.st_mtime)
        return;
    snprintf(p, sizeof(p), "%s/" MRF_REMEMBER, s->save_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    if ((f = fopen(tmp, "w")) == NULL) return;
    fprintf(f, "%s\n%lld %lld\n", path, (long long) st.st_size, (long long) st.st_mtime);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); remove(tmp); return; }
    fclose(f);
    if (rename(tmp, p) != 0) remove(tmp);
}

/* 1: s->found is the ROM (and is remembered); 0: none on the card */
static MRF_UNUSED int mrf_find(MrfSearch *s) {
    const char *env = s->root_env ? getenv(s->root_env) : NULL;
    const char *root = env && env[0] ? env : MRF_CARD_ROOT;
    char roms[PATH_MAX], last[PATH_MAX];
    int found = 0;
    s->found[0] = 0;
    s->ndone = 0;
    s->tried[0] = 0;
    if (s->app_dir && s->app_dir[0]) {
        found = mrf_scan_dir(s, s->app_dir);
        mrf_mark_dir(s, s->app_dir);
    }
    if (!found && s->save_dir && s->save_dir[0] && !mrf_done_dir(s, s->save_dir)) {
        found = mrf_scan_dir(s, s->save_dir);
        mrf_mark_dir(s, s->save_dir);
    }
    if (!found && mrf_recall(s, last, sizeof(last)) && mrf_name_ok(s, mrf_base(last))) {
        found = mrf_try(s, last);
        snprintf(s->tried, sizeof(s->tried), "%s", last);
    }
    snprintf(roms, sizeof(roms), "%s/Roms", root);
    if (!found) {
        found = mrf_scan_tree(s, roms, 2);
        mrf_mark_dir(s, roms);
    }
    if (!found) found = mrf_scan_tree(s, root, 2);
    if (found) mrf_remember(s, s->found);
    return found;
}

/* the not-found screen's lines (39 characters at most: the screens' 320-pixel width) */
#define MRF_NOT_FOUND_1 "Put your USA ROM (.z64/.v64/.n64)"
#define MRF_NOT_FOUND_2 "anywhere on the SD card."
#endif
