/* First start (M3b): build the game data on the device from the player's own ROM.
 *
 * The game folder holds oot_recipes.bin next to the executable (numbers only: the pack's file
 * table and the ops of hh_recipe_ops.h; tools/handheld/hh_recipes.c derives it at build
 * time and the release gates leak-scan it against the ROM). At boot:
 *
 *   <root>/data/segments/oot_psp_assets.bin + oot_psp_assets.id  (the id names the
 *     recipes that built the pack: a game update whose pack recipes changed rebuilds it)
 *   <root>/embedded.bin  (the NOLOAD region's bytes + this link's stamp + "HHEB";
 *     rebuilt from the pack alone, no ROM needed, whenever the stamp differs)
 *
 * Both present and current: nothing to do. Otherwise the ROM is found (OOT_ROM=<path>;
 * rom.z64 next to the executable; <root>; then the SD card, two folders deep, as
 * mk64/fzx), checked (32 MB, header in any of the three byte orders, then SHA-1 of the
 * whole ROM), and each pack entry is read from it through the ROM's own file table
 * (dmadata), Yaz0-decompressed and reshaped by its ops. Progress and errors use
 * hh_romscreen.c's screens.
 *
 * rom.z64 next to the executable (a personal zip puts the player's ROM there): found
 * through /proc/self/exe (as oot_recipes.bin), never argv[0]. It is only ever read;
 * everything built goes to <root> on the card. It gets the same header and SHA-1 checks
 * as a card ROM; one that fails them is passed over for the card search. Nothing keeps
 * the ROM's path: a rebuild is decided by the pack id and the build stamp alone.
 *
 * The magics (HHR1/HHRC in the recipes, HHPI in the pack id, HHEB in embedded.bin) are written as
 * shown; the readers also take FKR1/FKRC/FKPI/FKEB, the magics earlier builds wrote, so a card
 * set up by one of them keeps its pack.
 *
 * OOT_CONVERT_ONLY=1 exits after this step (tests, timing); OOT_CONVERT_FORCE=1
 * rebuilds both files even when they are current. */
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
void port_log_always(const char* fmt, ...); /* stdout + console.log (hh_port.c) */

extern unsigned char __hhassets_start[];
extern unsigned char __hhassets_end[];
extern const uint32_t hh_build_stamp;

#define ROM_SIZE 0x2000000u
#define DMADATA_OFFSET 0x7430u
#define DMADATA_COUNT 1510u
#define RECIPES_NAME "oot_recipes.bin"
#define PKG_ROM_NAME "rom.z64" /* the ROM in the game folder, next to the executable */

#define ROM_SHA1 "ad69c91157f6705e8ab06c79fe08aad47bb57ba7" /* USA 1.0, z64 order */

typedef struct {
    uint32_t ovs, ove, vs, fo, flags, size;
} HhcEntry;

typedef struct {
    uint32_t stamp, packId, nE, packSize, embSize, nSyms, opsN, embOpsStart, zn;
    HhcEntry* e;
    unsigned char* ops;
} HhcRecipes;

typedef struct {
    uint32_t vs, ve, rs, re;
} HhcDma;

static HhrPerms sPerms;
static HhcRecipes sR;
static char sRomName[64];
static int sLater;                 /* only embedded.bin is rebuilt (the game's data is current): the screen says so */
static int sEmbLo, sEmbHi;         /* the bar's share of the code data build (the Saving step follows at sEmbHi + 1) */

/* ------------------------------------------------------------------ small helpers */
static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}
static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
/* one screen for the whole job; the screen itself draws only when pct, stage or sub line changed */
static void progress(const char* stage, int pct) {
    const char* sub = sRomName[0] ? sRomName : (sLater ? "" : "Looking on the SD card");
    if (sLater)
        HhRomScreen_Progress(sub, "Building the game data", "for this version.", stage, pct, "Just a moment.");
    else
        HhRomScreen_Progress(sub, "First start: building the game", "data from your ROM.", stage, pct, "This happens once.");
}

/* ------------------------------------------------------------------ SHA-1 */
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
    for (i = 0; i < 5; i++) { out[i * 4] = (unsigned char)(s->h[i] >> 24); out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16); out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8); out[i * 4 + 3] = (unsigned char)s->h[i]; }
}

/* ------------------------------------------------------------------ the game folder's files */
/* <the executable's folder>/<name>, through /proc/self/exe (a launcher may run the bare name,
 * so not argv[0]); the build folder elsewhere */
static int exe_dir_file(char* out, size_t len, const char* name) {
    char exe[512];
    ssize_t n;
    char* slash;
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = 0;
    return snprintf(out, len, "%s/%s", exe, name) < (int)len;
}

/* ------------------------------------------------------------------ recipes */
static int recipes_path(char* out, size_t len) {
    const char* env = getenv("OOT_RECIPES");
    if (env && env[0]) { snprintf(out, len, "%s", env); return 1; }
    return exe_dir_file(out, len, RECIPES_NAME);
}

/* header (and file table) only when full == 0 */
static const char* recipes_load(int full) {
    char path[600];
    unsigned char h[44], t[8];
    FILE* f;
    long size;
    uint32_t i;
    if (!recipes_path(path, sizeof(path))) return "Can't find the game's own files.";
    f = fopen(path, "rb");
    if (!f) { port_log_always("hh: missing %s\n", path); return "Missing " RECIPES_NAME "."; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 52 || fread(h, 1, 44, f) != 44 || (memcmp(h, "HHR1", 4) && memcmp(h, "FKR1", 4)) || hhr_le32(h + 4) != 1) { fclose(f); return "The game's " RECIPES_NAME " is damaged."; }
    sR.stamp = hhr_le32(h + 8); sR.packId = hhr_le32(h + 12); sR.nE = hhr_le32(h + 16); sR.packSize = hhr_le32(h + 20);
    sR.embSize = hhr_le32(h + 24); sR.nSyms = hhr_le32(h + 28); sR.opsN = hhr_le32(h + 32); sR.embOpsStart = hhr_le32(h + 36);
    sR.zn = hhr_le32(h + 40);
    if (size != (long)(44 + sR.nE * 24 + sR.zn + 8) || sR.nE == 0 || sR.nE > 4096) { fclose(f); return "The game's " RECIPES_NAME " is damaged."; }
    fseek(f, size - 8, SEEK_SET);
    if (fread(t, 1, 8, f) != 8 || (memcmp(t + 4, "HHRC", 4) && memcmp(t + 4, "FKRC", 4)) || hhr_le32(t) != sR.stamp || sR.stamp != hh_build_stamp ||
        sR.embSize != (uint32_t)(__hhassets_end - __hhassets_start)) {
        fclose(f);
        return "The game's files don't belong together.";
    }
    if (full) {
        unsigned char* z = malloc(sR.zn);
        uLongf n = sR.opsN;
        sR.e = malloc(sizeof(*sR.e) * sR.nE);
        sR.ops = malloc(sR.opsN + 16);
        fseek(f, 44, SEEK_SET);
        for (i = 0; i < sR.nE; i++) {
            unsigned char b[24];
            if (fread(b, 1, 24, f) != 24) break;
            sR.e[i].ovs = hhr_le32(b); sR.e[i].ove = hhr_le32(b + 4); sR.e[i].vs = hhr_le32(b + 8);
            sR.e[i].fo = hhr_le32(b + 12); sR.e[i].flags = hhr_le32(b + 16); sR.e[i].size = hhr_le32(b + 20);
        }
        if (!z || !sR.e || !sR.ops || i != sR.nE || fread(z, 1, sR.zn, f) != sR.zn || uncompress(sR.ops, &n, z, sR.zn) != Z_OK || n != sR.opsN) {
            free(z); fclose(f);
            return "The game's " RECIPES_NAME " is damaged.";
        }
        free(z);
    }
    fclose(f);
    return NULL;
}

static int vrom_cb(void* ctx, uint32_t v, uint32_t* out) {
    uint32_t i;
    (void)ctx;
    if (v == 0) return 0;
    for (i = 0; i < sR.nE; i++) {
        if (sR.e[i].ovs == v) { *out = sR.e[i].vs; return 1; }
        if (sR.e[i].ove == v) { *out = sR.e[i].vs + sR.e[i].size; return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------------ state of the files */
static int pack_current(const char* root) {
    char path[600];
    unsigned char id[8];
    struct stat st;
    FILE* f;
    snprintf(path, sizeof(path), "%s/data/segments/oot_psp_assets.bin", root);
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != sR.packSize) return 0;
    snprintf(path, sizeof(path), "%s/data/segments/oot_psp_assets.id", root);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(id, 1, 8, f) != 8) { fclose(f); return 0; }
    fclose(f);
    return hhr_le32(id) == sR.packId && (!memcmp(id + 4, "HHPI", 4) || !memcmp(id + 4, "FKPI", 4));
}
static int embedded_current(const char* root) {
    char path[600];
    unsigned char t[8];
    struct stat st;
    FILE* f;
    snprintf(path, sizeof(path), "%s/embedded.bin", root);
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != sR.embSize + 8) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, -8, SEEK_END);
    if (fread(t, 1, 8, f) != 8) { fclose(f); return 0; }
    fclose(f);
    return hhr_le32(t) == hh_build_stamp && (!memcmp(t + 4, "HHEB", 4) || !memcmp(t + 4, "FKEB", 4));
}

/* ------------------------------------------------------------------ the ROM */
typedef struct {
    FILE* f;
    int order; /* 0 z64, 1 v64 (16-bit swapped), 2 n64 (32-bit little endian) */
} Rom;

static void unswap(unsigned char* p, size_t n, int order) {
    size_t i;
    if (order == 1) for (i = 0; i + 1 < n; i += 2) { unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    else if (order == 2) for (i = 0; i + 3 < n; i += 4) { unsigned char a = p[i], b = p[i + 1]; p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a; }
}
/* read n bytes at a 4-aligned offset (n rounded up to 4 internally) in z64 order */
static int rom_read(Rom* r, uint32_t off, unsigned char* out, uint32_t n) {
    uint32_t n4 = (n + 3) & ~3u;
    if (off & 3) return 0;
    if (off + n4 > ROM_SIZE) return 0;
    if (fseek(r->f, off, SEEK_SET) != 0 || fread(out, 1, n4, r->f) != n4) return 0;
    unswap(out, n4, r->order);
    return 1;
}

static int ext_ok(const char* n) {
    size_t l = strlen(n);
    char e[4];
    if (l < 5 || n[l - 4] != '.') return 0;
    e[0] = (char)(n[l - 3] | 0x20); e[1] = (char)(n[l - 2] | 0x20); e[2] = (char)(n[l - 1] | 0x20); e[3] = 0;
    return !strcmp(e, "z64") || !strcmp(e, "n64") || !strcmp(e, "v64") || !strcmp(e, "rom") || !strcmp(e, "bin") || !strcmp(e, "u64");
}

/* 1 = OoT USA 1.0 by its header; 0 = not OoT (why stays as it was); -1 = another OoT (why says which) */
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
    if (memcmp(h + 0x20, "THE LEGEND OF ZELDA", 19) != 0) return 0;
    if (h[0x3B] != 'C' || h[0x3C] != 'Z' || h[0x3D] != 'L') {
        snprintf(why, whylen, "is a Zelda ROM, but not Ocarina of Time for the N64.");
        return -1;
    }
    if (h[0x3E] == 'P') { snprintf(why, whylen, "is the European (PAL) version."); return -1; }
    if (h[0x3E] == 'J') { snprintf(why, whylen, "is the Japanese version."); return -1; }
    if (h[0x3E] != 'E') { snprintf(why, whylen, "is not the USA version."); return -1; }
    if (h[0x3F] != 0) { snprintf(why, whylen, "is USA version 1.%d (Rev %d).", h[0x3F], h[0x3F]); return -1; }
    if ((uint64_t)st.st_size != ROM_SIZE) { snprintf(why, whylen, "is not 32 MB (a trimmed or damaged ROM)."); return -1; }
    *order = o;
    return 1;
}

typedef struct {
    char path[512];
    int order;
    int inPkg;         /* path is rom.z64 in the game folder */
    char why[96];      /* the first near miss, for the error screen */
    char whyName[64];
} Find;

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
        if (ent->d_name[0] == '.' || !ext_ok(ent->d_name)) continue; /* hidden files, macOS "._x.z64" */
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
/* withPkg = 0: the card search alone (rom.z64 in the game folder failed its SHA-1) */
static int find_rom(Find* fd, const char* root, int withPkg) {
    const char* env = getenv("OOT_ROM");
    char pkg[600];
    memset(fd, 0, sizeof(*fd));
    if (env && env[0]) return try_file(fd, env);
    if (withPkg && exe_dir_file(pkg, sizeof(pkg), PKG_ROM_NAME) && try_file(fd, pkg)) {
        fd->inPkg = 1;
        return 1;
    }
    if (scan_dir(fd, root)) return 1;
#if defined(MIYOO)
    /* Miyoo Mini: the card is /mnt/SDCARD; Onion and the stock OS keep N64 ROMs in Roms/N64 */
    return scan_tree(fd, "/mnt/SDCARD/Roms/N64", 1) || scan_tree(fd, "/mnt/SDCARD", 2);
#else
    return scan_tree(fd, ".", 2);
#endif
}

static int rom_verify(Rom* r) {
    enum { CH = 1 << 20 };
    unsigned char* b = malloc(CH);
    unsigned char out[20];
    Sha1 s;
    uint32_t off;
    if (!b) return 0;
    sha1_init(&s);
    for (off = 0; off < ROM_SIZE; off += CH) {
        if (!rom_read(r, off, b, CH)) { free(b); return 0; }
        sha1_update(&s, b, CH);
        progress("Checking the ROM", (int)((uint64_t)(off + CH) * 8 / ROM_SIZE));
    }
    free(b);
    sha1_final(&s, out);
    {
        char hex[41];
        int i;
        for (i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
        port_log_always("hh: ROM SHA-1 %s\n", hex);
        return strcmp(hex, ROM_SHA1) == 0;
    }
}

/* ------------------------------------------------------------------ Yaz0 */
static int yaz0(const unsigned char* in, size_t inN, unsigned char* out, size_t outN) {
    const unsigned char* ie = in + inN;
    unsigned char* o = out;
    unsigned char* oe = out + outN;
    unsigned code = 0, bits = 0;
    if (inN < 16 || memcmp(in, "Yaz0", 4) || hhr_be32(in + 4) != outN) return 0;
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
            if (dist > (size_t)(o - out) || len > (size_t)(oe - o)) return 0;
            c = o - dist;
            while (len--) *o++ = *c++;
        }
        code <<= 1;
        bits--;
    }
    return 1;
}

/* ------------------------------------------------------------------ building */
static void mkdirs(const char* root) {
    char p[600];
    snprintf(p, sizeof(p), "%s/data", root);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/data/segments", root);
    mkdir(p, 0755);
}

static const char* build_pack(const char* root, Rom* rom) {
    char path[600], idPath[600];
    HhcDma* dma = malloc(sizeof(HhcDma) * DMADATA_COUNT);
    unsigned char* raw = malloc(DMADATA_COUNT * 16);
    unsigned char *src = NULL, *dst = NULL, *z = NULL;
    size_t srcCap = 0, zCap = 0;
    const unsigned char* c = sR.ops;
    const unsigned char* end = sR.ops + sR.embOpsStart;
    uint32_t i, j, pos = 0;
    uint64_t done = 0;
    FILE* out;
    unsigned char id[8];
    const char* err = NULL;
    static const unsigned char zeros[16];

    if (!dma || !raw || !rom_read(rom, DMADATA_OFFSET, raw, DMADATA_COUNT * 16)) { free(dma); free(raw); return "Could not read the ROM."; }
    for (i = 0; i < DMADATA_COUNT; i++) {
        dma[i].vs = hhr_be32(raw + i * 16); dma[i].ve = hhr_be32(raw + i * 16 + 4);
        dma[i].rs = hhr_be32(raw + i * 16 + 8); dma[i].re = hhr_be32(raw + i * 16 + 12);
    }
    free(raw);
    mkdirs(root);
    snprintf(idPath, sizeof(idPath), "%s/data/segments/oot_psp_assets.id", root);
    unlink(idPath); /* written last: an interrupted build is rebuilt next time */
    snprintf(path, sizeof(path), "%s/data/segments/oot_psp_assets.bin", root);
    out = fopen(path, "wb");
    if (!out) { free(dma); return "Could not write to the SD card."; }
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    for (i = 0; i < sR.nE && !err; i++) {
        const HhcEntry* e = &sR.e[i];
        const HhcDma* d = NULL;
        uint32_t stored;
        for (j = 0; j < DMADATA_COUNT; j++) if (dma[j].vs == e->ovs && dma[j].ve == e->ove) { d = &dma[j]; break; }
        if (!d || e->ove - e->ovs != e->size || d->rs == 0xFFFFFFFFu) { err = "The ROM's file table doesn't match."; break; }
        if (e->size + 16 > srcCap) { srcCap = e->size + 16; free(src); free(dst); src = malloc(srcCap); dst = malloc(srcCap); }
        if (!src || !dst) { err = "Not enough memory."; break; }
        if (d->re == 0) {
            if (!rom_read(rom, d->rs, src, e->size)) { err = "Could not read the ROM."; break; }
        } else {
            stored = d->re - d->rs;
            if (stored + 16 > zCap) { zCap = stored + 16; free(z); z = malloc(zCap); }
            if (!z || !rom_read(rom, d->rs, z, stored) || !yaz0(z, stored, src, e->size)) { err = "Could not unpack the ROM."; break; }
        }
        if (e->flags) {
            if (!hhr_decode(&sPerms, src, dst, e->size, &c, end, vrom_cb, NULL)) { err = "The game data recipe doesn't match."; break; }
        }
        while (pos < e->fo) { /* 16-byte alignment between entries */
            uint32_t k = e->fo - pos > 16 ? 16 : e->fo - pos;
            if (fwrite(zeros, 1, k, out) != k) { err = "Could not write to the SD card (is it full?)."; break; }
            pos += k;
        }
        if (err) break;
        if (fwrite(e->flags ? dst : src, 1, e->size, out) != e->size) { err = "Could not write to the SD card (is it full?)."; break; }
        pos += e->size;
        done += e->size;
        progress("Building the game data", 8 + (int)(done * 84 / sR.packSize));
    }
    if (!err && (pos != sR.packSize || c != end)) err = "The game data recipe doesn't match.";
    if (!err) progress("Saving the game data", 93);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) { if (!err) err = "Could not write to the SD card (is it full?)."; }
    fclose(out);
    free(dma); free(src); free(dst); free(z);
    if (err) return err;
    progress("Saving the game data", 94);
    put32(id, sR.packId);
    memcpy(id + 4, "HHPI", 4);
    out = fopen(idPath, "wb");
    if (!out || fwrite(id, 1, 8, out) != 8) { if (out) fclose(out); return "Could not write to the SD card."; }
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    return NULL;
}

static const char* build_embedded(const char* root) {
    char path[600];
    const unsigned char* c = sR.ops + sR.embOpsStart;
    const unsigned char* end = sR.ops + sR.opsN;
    unsigned char* region = calloc(sR.embSize + 8, 1);
    unsigned char* src = NULL;
    size_t srcCap = 0;
    uint32_t i;
    FILE *pk, *out;
    const char* err = NULL;
    if (!region) return "Not enough memory.";
    snprintf(path, sizeof(path), "%s/data/segments/oot_psp_assets.bin", root);
    pk = fopen(path, "rb");
    if (!pk) { free(region); return "Could not read the game data."; }
    for (i = 0; i < sR.nSyms && !err; i++) {
        uint32_t off, size, s;
        if (end - c < 12) { err = "The code data recipe is damaged."; break; }
        off = hhr_le32(c); size = hhr_le32(c + 4); s = hhr_le32(c + 8);
        c += 12;
        if (off + size > sR.embSize) { err = "The code data recipe is damaged."; break; }
        if (size + 8 > srcCap) { srcCap = size + 8; free(src); src = malloc(srcCap); }
        if (!src) { err = "Not enough memory."; break; }
        if (s == 0xFFFFFFFFu) memset(src, 0, size + 8);
        else if (s + size > sR.packSize || fseek(pk, s, SEEK_SET) != 0 || fread(src, 1, size, pk) != size) { err = "Could not read the game data."; break; }
        if (!hhr_decode(&sPerms, src, region + off, size, &c, end, vrom_cb, NULL)) err = "The code data recipe doesn't match.";
        if ((i & 31) == 0) progress("Building the code data", sEmbLo + (int)((uint64_t)i * (uint32_t)(sEmbHi - sEmbLo) / sR.nSyms));
    }
    fclose(pk);
    free(src);
    if (!err && c != end) err = "The code data recipe doesn't match.";
    if (!err) {
        put32(region + sR.embSize, hh_build_stamp);
        memcpy(region + sR.embSize + 4, "HHEB", 4);
        progress("Saving the code data", sEmbHi + 1);
        snprintf(path, sizeof(path), "%s/embedded.bin", root);
        out = fopen(path, "wb");
        if (!out || fwrite(region, 1, sR.embSize + 8, out) != sR.embSize + 8) err = "Could not write to the SD card (is it full?).";
        if (out) { fflush(out); fsync(fileno(out)); fclose(out); }
    }
    free(region);
    return err;
}

/* ------------------------------------------------------------------ entry */
#if defined(MIYOO)
/* The Mini's CPU clock (kit miyoo_clock, 30024bc), from the first thing main does after reading
 * debug.cfg (HhConvert_Ensure, below), so the first start runs at it too: Onion's own choice for the model by
 * default (1600 MHz Mini, 1900 MHz Mini Plus), debug.cfg cpuclock=<MHz> / OOT_CPUCLOCK to change, 0 to leave it.
 * Put back at exit (gfx_wm_shutdown, and atexit for every other way out). */
static void hh_miyoo_clock_start(void) {
    extern int miyoo_clock_boost_to(int mhz);
    const char* cc = getenv("OOT_CPUCLOCK");
    port_log_always("clock: cpuclock=%s\n", cc != NULL && cc[0] ? cc : "default"); /* console.log open first */
    { /* debug.cfg fps= (OOT_FPS) drew interpolated frames in b2b34c9, taken out again (2026-10-11, the user's call):
       * a leftover line does nothing */
        const char* fps = getenv("OOT_FPS");
        if (fps != NULL && fps[0]) port_log_always("fps=%s ignored: the game draws its own frames only (no interpolation)\n", fps);
    }
    const int mhz = miyoo_clock_boost_to(-1);
    if (mhz > 0) {
        port_log_always("clock: %d MHz\n", mhz);
    } else {
        FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
        long khz = 0;
        if (f != NULL) {
            if (fscanf(f, "%ld", &khz) != 1) khz = 0;
            fclose(f);
        }
        port_log_always("clock: left as it was (cpufreq %ld MHz)\n", khz / 1000);
    }
}
#endif
void HhConvert_Ensure(const char* root) {
    const char* err;
    int force = getenv("OOT_CONVERT_FORCE") != NULL;
#if defined(MIYOO)
    hh_miyoo_clock_start();
#endif
    int needPack, needEmb;
    double t0 = now_s(), t1;
    char rbPath[600]; /* <root>/rebuild (the player's, any content): do the whole job again. The old pack and
                       * embedded.bin stay until a ROM has been found and has passed its SHA-1; with none,
                       * they and the request stay. The request goes once everything is built. */
    struct stat rbSt;
    int rebuild;

    snprintf(rbPath, sizeof(rbPath), "%s/rebuild", root);
    rebuild = stat(rbPath, &rbSt) == 0;
    if (rebuild) {
        force = 1;
        port_log_always("hh: %s found, the first start's job runs again\n", rbPath);
    }
    if ((err = recipes_load(0)) != NULL) { HhRomScreen_Fail(err, "Copy the whole game folder to the", "SD card again."); return; }
    needPack = force || !pack_current(root);
    needEmb = force || needPack || !embedded_current(root);
    if (!needPack && !needEmb) {
        if (getenv("OOT_CONVERT_ONLY")) { printf("hh: game data current, nothing to build\n"); exit(0); }
        return;
    }
    if (!hhr_perms(&sPerms) || (err = recipes_load(1)) != NULL) { HhRomScreen_Fail(err ? err : "Internal error.", NULL, NULL); return; }
    sLater = !needPack;
    sEmbLo = needPack ? 95 : 0;
    sEmbHi = needPack ? 97 : 96;
    port_log_always("hh: first start: building %s%s%s\n", needPack ? "the game data" : "", needPack && needEmb ? " and " : "", needEmb ? "embedded.bin" : "");
    if (needPack) {
        Find fd;
        Rom rom;
        int pkgBad = 0; /* rom.z64 in the game folder failed to open or its SHA-1: the card search alone */
        for (;;) {
            const char* order;
            int opened;
            sRomName[0] = 0;
            progress("Looking for your ROM...", 0);
            if (!find_rom(&fd, root, !pkgBad)) {
                /* rom.z64 sits in App/OcarinaOfTime (a personal zip) */
                if (pkgBad) HhRomScreen_Fail("rom.z64 in the game folder is damaged.", "Copy your USA 1.0 ROM there again,", "or to Roms/N64.");
                if (fd.why[0]) {
                    char l1[96];
                    snprintf(l1, sizeof(l1), "%s", fd.whyName);
                    HhRomScreen_Fail(l1, fd.why, "Ocarina of Time needs USA 1.0.");
                }
                HhRomScreen_Fail("No Ocarina of Time ROM found.", "Copy your USA 1.0 ROM (.z64/.v64/.n64)", "to the SD card.");
                return;
            }
            order = fd.order == 0 ? "z64" : fd.order == 1 ? "v64" : "n64";
            if (fd.inPkg) {
                snprintf(sRomName, sizeof(sRomName), "%s (in the game's folder)", PKG_ROM_NAME);
                port_log_always("hh: ROM %s (next to the executable, %s order)\n", fd.path, order);
            } else {
                const char* b = strrchr(fd.path, '/');
                snprintf(sRomName, sizeof(sRomName), "%.60s", b ? b + 1 : fd.path);
                port_log_always("hh: ROM %s (%s order)\n", fd.path, order);
            }
            rom.f = fopen(fd.path, "rb");
            rom.order = fd.order;
            opened = rom.f != NULL;
            if (opened && rom_verify(&rom)) break;
            if (opened) fclose(rom.f);
            if (fd.inPkg) {
                port_log_always("hh: ROM %s in the game folder %s; looking on the SD card\n", fd.path,
                                opened ? "fails the SHA-1 check" : "can't be opened");
                pkgBad = 1;
                continue;
            }
            if (!opened) { HhRomScreen_Fail("Could not open the ROM.", NULL, NULL); return; }
            HhRomScreen_Fail("This ROM's checksum doesn't match", "Ocarina of Time USA 1.0.", "It may be damaged or modified.");
            return;
        }
        t1 = now_s();
        port_log_always("hh: ROM SHA-1 OK in %.1f s\n", t1 - t0);
        err = build_pack(root, &rom);
        fclose(rom.f);
        if (err) { HhRomScreen_Fail(err, NULL, NULL); return; }
        port_log_always("hh: game data built in %.1f s\n", now_s() - t1);
    }
    t1 = now_s();
    progress("Building the code data", sEmbLo);
    if ((err = build_embedded(root)) != NULL) { HhRomScreen_Fail(err, NULL, NULL); return; }
    port_log_always("hh: embedded.bin built in %.1f s; first start total %.1f s\n", now_s() - t1, now_s() - t0);
    free(sR.e); free(sR.ops);
    sR.e = NULL; sR.ops = NULL;
    HhRomScreen_Close(); /* "Done" at 100%, a beat, one black frame; SDL stays up for the game's screen */
    if (rebuild) {
        if (remove(rbPath) == 0) port_log_always("hh: rebuild done, %s removed\n", rbPath);
        else port_log_always("hh: rebuild done, but %s can't be removed: the job runs again next start\n", rbPath);
    }
    if (getenv("OOT_CONVERT_ONLY")) exit(0);
}
