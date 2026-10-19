/* First start: build the data pack (data/mm_pack.bin) on the device from the player's own ROM, as the Ocarina of
 * Time port's hh_convert.c does for its pack.
 *
 * The game folder holds mm_recipes.bin next to the executable (hh/tools/mm_recipes.py: op numbers only, no ROM data).
 * The pack keeps the decompressed ROM's vrom layout, so it is rebuilt as: every dmadata file of the ROM
 * Yaz0-decompressed at its vrom (zeros between), each 8-byte block then reshaped by one op of hh_recipe_ops.h. The
 * result's CRC-32 must match the recipe's.
 *
 *   data/mm_pack.bin + data/mm_pack.id ("MMPI" + the recipe's CRC): current -> nothing to do.
 *   Otherwise the ROM is found (MM_ROM=<path>; rom.z64 next to the executable; the data folder; then the SD card's
 *   Roms/N64 and two folders deep), checked (32 MB, any byte order, header, SHA-1) and the pack built, with
 *   hh_romscreen.c's progress and error screens. data/mm_pack.bin without an id (a PERSONAL zip's, or an older
 *   build's) is used as it is when no ROM is found.
 *
 * MM_CONVERT_ONLY=1 exits after this step (tests, timing); MM_CONVERT_FORCE=1 rebuilds even a current pack. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

#include "hh_recipe_ops.h"

void HhRomScreen_Progress(const char* sub, const char* l1, const char* l2, const char* stage, int pct, const char* foot);
void HhRomScreen_Fail(const char* l1, const char* l2, const char* l3);
void HhRomScreen_Close(void);
void port_log_always(const char* fmt, ...);
const char* port_save_path(const char* name);
const char* port_save_dir(void);

#define ROM_SIZE 0x2000000u
#define RECIPES_NAME "mm_recipes.bin"
#define PKG_ROM_NAME "rom.z64"

typedef struct {
    uint32_t version, packSize, dmaOff, crc, opsN;
    char sha1[41];
    unsigned char* ops;
} MmRecipes;

typedef struct {
    FILE* f;
    int order; /* 0 z64, 1 v64, 2 n64 */
} Rom;

typedef struct {
    char path[512];
    int order;
    char why[96];
    char whyName[64];
} Find;

static HhrPerms sPerms;
static MmRecipes sR;
static char sRomName[64];

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void progress(const char* stage, int pct) {
    extern void HhMm_Beat(void);
    HhMm_Beat(); /* the watchdog: building is no stall */
    HhRomScreen_Progress(sRomName[0] ? sRomName : "Looking on the SD card", "First start: building the game",
                         "data from your ROM.", stage, pct, "This happens once.");
}

/* ---- SHA-1 */
typedef struct {
    uint32_t h[5];
    uint64_t n;
    unsigned char buf[64];
    unsigned bl;
} Sha1;
#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static void sha1_block(Sha1* s, const unsigned char* p) {
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++) w[i] = hhr_be32(p + i * 4);
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
static void sha1_init(Sha1* s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->n = 0; s->bl = 0;
}
static void sha1_update(Sha1* s, const unsigned char* p, size_t n) {
    s->n += n;
    while (n) {
        if (s->bl == 0 && n >= 64) { sha1_block(s, p); p += 64; n -= 64; continue; }
        s->buf[s->bl++] = *p++; n--;
        if (s->bl == 64) { sha1_block(s, s->buf); s->bl = 0; }
    }
}
static void sha1_final(Sha1* s, unsigned char out[20]) {
    uint64_t bits = s->n * 8;
    unsigned char pad = 0x80, z = 0, len[8];
    int i;
    sha1_update(s, &pad, 1);
    while (s->bl != 56) sha1_update(s, &z, 1);
    for (i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24); out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8); out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

/* ---- files */
static int exe_dir_file(char* out, size_t len, const char* name) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char* slash;
    if (n <= 0) return 0;
    exe[n] = 0;
    slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    return snprintf(out, len, "%s/%s", exe, name) < (int)len;
}

/* the header only (full = 0), or the ops too */
static const char* recipes_load(int full) {
    char path[600];
    unsigned char h[64];
    FILE* f;
    long size;
    const char* env = getenv("MM_RECIPES");
    if (env && env[0]) snprintf(path, sizeof(path), "%s", env);
    else if (!exe_dir_file(path, sizeof(path), RECIPES_NAME)) return "no";
    f = fopen(path, "rb");
    if (!f) return "no";
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 64 || fread(h, 1, 64, f) != 64 || memcmp(h, "MMR1", 4) != 0 || hhr_le32(h + 4) != 1) {
        fclose(f);
        return "The game's " RECIPES_NAME " is damaged.";
    }
    sR.version = 1;
    sR.packSize = hhr_le32(h + 8);
    sR.dmaOff = hhr_le32(h + 12);
    sR.crc = hhr_le32(h + 16);
    sR.opsN = hhr_le32(h + 20);
    memcpy(sR.sha1, h + 24, 40);
    sR.sha1[40] = 0;
    if (full) {
        const size_t zn = (size_t)size - 64;
        unsigned char* z = malloc(zn);
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

static int pack_current(void) {
    char path[600];
    unsigned char id[8];
    struct stat st;
    FILE* f;
    snprintf(path, sizeof(path), "%s", port_save_path("mm_pack.bin"));
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != sR.packSize) return 0;
    f = fopen(port_save_path("mm_pack.id"), "rb");
    if (!f) return -1; /* a pack without an id: a PERSONAL zip's or an older build's */
    if (fread(id, 1, 8, f) != 8) { fclose(f); return 0; }
    fclose(f);
    return memcmp(id, "MMPI", 4) == 0 && hhr_le32(id + 4) == sR.crc;
}

/* ---- the ROM */
static void unswap(unsigned char* p, size_t n, int order) {
    size_t i;
    if (order == 1) for (i = 0; i + 1 < n; i += 2) { unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    else if (order == 2) for (i = 0; i + 3 < n; i += 4) { unsigned char a = p[i], b = p[i + 1]; p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a; }
}
static int rom_read(Rom* r, uint32_t off, unsigned char* out, uint32_t n) {
    uint32_t n4 = (n + 3) & ~3u;
    if ((off & 3) || off + n4 > ROM_SIZE) return 0;
    if (fseek(r->f, off, SEEK_SET) != 0 || fread(out, 1, n4, r->f) != n4) return 0;
    unswap(out, n4, r->order);
    return 1;
}
static int ext_ok(const char* n) {
    size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') return 0;
    e[0] = (char)(n[l - 3] | 0x20); e[1] = (char)(n[l - 2] | 0x20); e[2] = (char)(n[l - 1] | 0x20); e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") || !strcmp(e, "rom") || !strcmp(e, "bin") ||
           !strcmp(e, "u64");
}
/* 1 = Majora's Mask USA by its header; 0 = not MM; -1 = another MM (why says which) */
static int quick_check(const char* path, char* why, size_t whylen, int* order) {
    unsigned char h[64];
    struct stat st;
    FILE* f;
    int o;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(h, 1, 64, f) != 64) { fclose(f); return 0; }
    fclose(f);
    if (h[0] == 0x80 && h[1] == 0x37) o = 0;
    else if (h[0] == 0x37 && h[1] == 0x80) o = 1;
    else if (h[0] == 0x40 && h[1] == 0x12) o = 2;
    else return 0;
    unswap(h, 64, o);
    if (memcmp(h + 0x20, "ZELDA MAJORA'S MASK", 19) != 0) return 0;
    if (h[0x3E] == 'P') { snprintf(why, whylen, "is the European (PAL) version."); return -1; }
    if (h[0x3E] == 'J') { snprintf(why, whylen, "is the Japanese version."); return -1; }
    if (h[0x3E] != 'E') { snprintf(why, whylen, "is not the USA version."); return -1; }
    if ((uint64_t)st.st_size != ROM_SIZE) { snprintf(why, whylen, "is not 32 MB (a trimmed or damaged ROM)."); return -1; }
    *order = o;
    return 1;
}
static int try_file(Find* fd, const char* path) {
    char why[96];
    int o = 0, r = quick_check(path, why, sizeof(why), &o);
    if (r > 0) { snprintf(fd->path, sizeof(fd->path), "%s", path); fd->order = o; return 1; }
    if (r < 0 && !fd->why[0]) {
        const char* b = strrchr(path, '/');
        snprintf(fd->why, sizeof(fd->why), "%s", why);
        snprintf(fd->whyName, sizeof(fd->whyName), "%.60s", b ? b + 1 : path);
    }
    return 0;
}
static int scan_dir(Find* fd, const char* dir) {
    DIR* d = opendir(dir);
    struct dirent* ent;
    int found = 0;
    if (!d) return 0;
    while (!found && (ent = readdir(d)) != NULL) {
        char path[512];
        size_t dl = strlen(dir);
        if (ent->d_name[0] == '.' || !ext_ok(ent->d_name)) continue;
        snprintf(path, sizeof(path), "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", ent->d_name);
        found = try_file(fd, path);
    }
    closedir(d);
    return found;
}
static int scan_tree(Find* fd, const char* root, int depth) {
    DIR* d;
    struct dirent* ent;
    if (scan_dir(fd, root)) return 1;
    if (depth <= 0 || (d = opendir(root)) == NULL) return 0;
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        struct stat st;
        size_t rl = strlen(root);
        if (ent->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s%s%s", root, (rl && root[rl - 1] == '/') ? "" : "/", ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (scan_tree(fd, sub, depth - 1)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}
static int find_rom(Find* fd) {
    const char* env = getenv("MM_ROM");
    char pkg[600];
    memset(fd, 0, sizeof(*fd));
    if (env && env[0]) return try_file(fd, env);
    if (exe_dir_file(pkg, sizeof(pkg), PKG_ROM_NAME) && try_file(fd, pkg)) return 1;
    if (exe_dir_file(pkg, sizeof(pkg), "") && scan_dir(fd, pkg)) return 1; /* any ROM in the game folder */
    if (scan_dir(fd, port_save_dir())) return 1;
#if defined(MIYOO)
    return scan_tree(fd, "/mnt/SDCARD/Roms/N64", 1) || scan_tree(fd, "/mnt/SDCARD", 2);
#else
    return scan_tree(fd, ".", 2);
#endif
}
static int rom_verify(Rom* r) {
    enum { CH = 1 << 20 };
    unsigned char* b = malloc(CH);
    unsigned char out[20];
    char hex[41];
    Sha1 s;
    uint32_t off;
    int i;
    if (!b) return 0;
    sha1_init(&s);
    for (off = 0; off < ROM_SIZE; off += CH) {
        if (!rom_read(r, off, b, CH)) { free(b); return 0; }
        sha1_update(&s, b, CH);
        progress("Checking the ROM", (int)((uint64_t)(off + CH) * 8 / ROM_SIZE));
    }
    free(b);
    sha1_final(&s, out);
    for (i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    port_log_always("mm: ROM SHA-1 %s\n", hex);
    return strcmp(hex, sR.sha1) == 0;
}

/* ---- Yaz0 */
static int yaz0(const unsigned char* in, size_t inN, unsigned char* out, size_t outN) {
    const unsigned char* ie = in + inN;
    unsigned char* o = out;
    unsigned char* oe = out + outN;
    unsigned code = 0, bits = 0;
    if (inN < 16 || memcmp(in, "Yaz0", 4)) return 0;
    if (hhr_be32(in + 4) < outN) return 0;
    in += 16;
    while (o < oe) {
        if (!bits) { if (in >= ie) return 0; code = *in++; bits = 8; }
        if (code & 0x80) {
            if (in >= ie) return 0;
            *o++ = *in++;
        } else {
            size_t dist, len;
            unsigned char* c;
            if (ie - in < 2) return 0;
            dist = ((size_t)(in[0] & 0x0F) << 8 | in[1]) + 1;
            len = in[0] >> 4;
            in += 2;
            if (len == 0) { if (in >= ie) return 0; len = (size_t)*in++ + 0x12; }
            else len += 2;
            if (dist > (size_t)(o - out)) return 0;
            if (len > (size_t)(oe - o)) len = (size_t)(oe - o);
            c = o - dist;
            while (len--) *o++ = *c++;
        }
        code <<= 1;
        bits--;
    }
    return 1;
}

static int no_vrom(void* ctx, uint32_t v, uint32_t* out) {
    (void)ctx; (void)v; (void)out;
    return 0;
}

/* ---- building */
static const char* build_pack(Rom* rom) {
    enum { ZCH = 1 << 16 };
    char path[600], tmp[620];
    unsigned char dmaRaw[16];
    unsigned char *src = NULL, *dst = NULL, *z = NULL;
    static unsigned char zeros[ZCH];
    size_t cap = 0, zCap = 0;
    const unsigned char* c = sR.ops;
    const unsigned char* end = sR.ops + sR.opsN;
    uint32_t pos = 0, k = 0, crc = 0;
    FILE* out;
    const char* err = NULL;

    snprintf(path, sizeof(path), "%s", port_save_path("mm_pack.bin"));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(port_save_path("mm_pack.id")); /* written last: an interrupted build is redone */
    out = fopen(tmp, "wb");
    if (!out) return "Could not write to the SD card.";
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    crc = (uint32_t)crc32(0L, Z_NULL, 0);
    for (;; k++) {
        uint32_t vs, ve, rs, re, n, gapEnd;
        int last;
        if (!rom_read(rom, sR.dmaOff + k * 16, dmaRaw, 16)) { err = "Could not read the ROM."; break; }
        vs = hhr_be32(dmaRaw); ve = hhr_be32(dmaRaw + 4); rs = hhr_be32(dmaRaw + 8); re = hhr_be32(dmaRaw + 12);
        last = k > 0 && vs == 0 && ve == 0 && rs == 0 && re == 0;
        gapEnd = last ? sR.packSize : vs;
        if (!last && (vs < pos || ve < vs || ve > sR.packSize || (vs & 7) || (ve & 7))) { err = "The ROM's file table doesn't match."; break; }
        while (pos < gapEnd && !err) { /* between files: zeros */
            n = gapEnd - pos > ZCH ? ZCH : gapEnd - pos;
            if (n + 8 > cap) { cap = n + 8; free(dst); dst = malloc(cap); free(src); src = malloc(cap); }
            if (!dst) { err = "Not enough memory."; break; }
            if (!hhr_decode(&sPerms, zeros, dst, n, &c, end, no_vrom, NULL)) { err = "The game data recipe doesn't match."; break; }
            if (fwrite(dst, 1, n, out) != n) { err = "Could not write to the SD card (is it full?)."; break; }
            crc = (uint32_t)crc32(crc, dst, n);
            pos += n;
        }
        if (err || last) break;
        n = ve - vs;
        if (n == 0) continue;
        if (n + 16 > cap) { cap = n + 16; free(src); free(dst); src = malloc(cap); dst = malloc(cap); }
        if (!src || !dst) { err = "Not enough memory."; break; }
        if (rs == 0xFFFFFFFFu) {
            memset(src, 0, n); /* a file not in this ROM */
        } else if (re == 0) {
            if (!rom_read(rom, rs, src, n)) { err = "Could not read the ROM."; break; }
        } else {
            const uint32_t stored = re - rs;
            if (stored + 16 > zCap) { zCap = stored + 16; free(z); z = malloc(zCap); }
            if (!z || !rom_read(rom, rs, z, stored) || !yaz0(z, stored, src, n)) { err = "Could not unpack the ROM."; break; }
        }
        if (!hhr_decode(&sPerms, src, dst, n, &c, end, no_vrom, NULL)) { err = "The game data recipe doesn't match."; break; }
        if (fwrite(dst, 1, n, out) != n) { err = "Could not write to the SD card (is it full?)."; break; }
        crc = (uint32_t)crc32(crc, dst, n);
        pos += n;
        progress("Building the game data", 8 + (int)((uint64_t)pos * 84 / sR.packSize));
    }
    if (!err && (pos != sR.packSize || c != end)) err = "The game data recipe doesn't match.";
    if (!err && crc != sR.crc) {
        port_log_always("mm: rebuilt pack CRC %08x, the recipe says %08x\n", (unsigned)crc, (unsigned)sR.crc);
        err = "The rebuilt game data doesn't check out.";
    }
    if (!err) progress("Saving the game data", 93);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) { if (!err) err = "Could not write to the SD card (is it full?)."; }
    fclose(out);
    free(src); free(dst); free(z);
    if (err) { unlink(tmp); return err; }
    if (rename(tmp, path) != 0) return "Could not write to the SD card.";
    {
        unsigned char id[8];
        memcpy(id, "MMPI", 4);
        id[4] = (unsigned char)sR.crc; id[5] = (unsigned char)(sR.crc >> 8);
        id[6] = (unsigned char)(sR.crc >> 16); id[7] = (unsigned char)(sR.crc >> 24);
        out = fopen(port_save_path("mm_pack.id"), "wb");
        if (!out || fwrite(id, 1, 8, out) != 8) { if (out) fclose(out); return "Could not write to the SD card."; }
        fflush(out);
        fsync(fileno(out));
        fclose(out);
    }
    return NULL;
}

/* main(), before the pack is opened */
void HhMm_ConvertEnsure(void) {
    const char* err;
    const int force = getenv("MM_CONVERT_FORCE") != NULL;
    double t0 = now_s(), t1;
    Find fd;
    Rom rom;
    int cur;

    err = recipes_load(0);
    if (err != NULL) {
        if (strcmp(err, "no") == 0) return; /* no recipe in the folder: the pack is the zip's own (PERSONAL) */
        HhRomScreen_Fail(err, "Copy the whole game folder to the", "SD card again.");
        return;
    }
    cur = pack_current();
    if (cur > 0 && !force) {
        if (getenv("MM_CONVERT_ONLY")) { port_log_always("mm: game data current\n"); exit(0); }
        return;
    }
    sRomName[0] = 0;
    progress("Looking for your ROM...", 0);
    if (!find_rom(&fd)) {
        if (cur < 0 && !force) { /* the zip's own pack, and no ROM to rebuild it from: use it */
            port_log_always("mm: data pack without an id and no ROM found: used as it is\n");
            HhRomScreen_Close();
            return;
        }
        if (fd.why[0]) HhRomScreen_Fail(fd.whyName, fd.why, "Majora's Mask needs the USA version.");
        HhRomScreen_Fail("No Majora's Mask ROM found.", "Copy your USA ROM (.z64/.v64/.n64)", "to the SD card.");
        return;
    }
    {
        const char* b = strrchr(fd.path, '/');
        snprintf(sRomName, sizeof(sRomName), "%.60s", b ? b + 1 : fd.path);
    }
    port_log_always("mm: first start: ROM %s (%s order)\n", fd.path, fd.order == 0 ? "z64" : fd.order == 1 ? "v64" : "n64");
    rom.f = fopen(fd.path, "rb");
    rom.order = fd.order;
    if (rom.f == NULL) { HhRomScreen_Fail("Could not open the ROM.", NULL, NULL); return; }
    if (!rom_verify(&rom)) {
        fclose(rom.f);
        HhRomScreen_Fail("This ROM's checksum doesn't match", "Majora's Mask USA.", "It may be damaged or modified.");
        return;
    }
    if (!hhr_perms(&sPerms) || (err = recipes_load(1)) != NULL) {
        fclose(rom.f);
        HhRomScreen_Fail(err ? err : "Internal error.", NULL, NULL);
        return;
    }
    t1 = now_s();
    port_log_always("mm: ROM SHA-1 OK in %.1f s\n", t1 - t0);
    err = build_pack(&rom);
    fclose(rom.f);
    free(sR.ops);
    sR.ops = NULL;
    if (err) { HhRomScreen_Fail(err, NULL, NULL); return; }
    port_log_always("mm: game data built in %.1f s (first start %.1f s)\n", now_s() - t1, now_s() - t0);
    HhRomScreen_Close();
    if (getenv("MM_CONVERT_ONLY")) exit(0);
}
