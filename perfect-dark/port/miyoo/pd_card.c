/* pd_card.c: the player's Perfect Dark ROM, found and checked before the game maps it (romdata.c
 * romdataInit). The GoldenEye port's hh_card.c, for Perfect Dark.
 *
 * Everything this keeps is in the save folder (--savedir; on the Miyoo Mini App/PerfectDark/data):
 *   perfectdark.z64  the player's ROM in z64 order, written once from the dump found on the card
 *                    (not needed with a rom.z64 in the game's folder)
 *   rom_check.ok     "<size> <time> <sha1>" of the game folder's rom.z64 whose SHA-1 checked out
 *   rebuild          (the player's, any content) do the first start's job again at the next start:
 *                    the ROM found, checked and copied anew (or the game folder's checked); the file
 *                    is removed once that has worked
 *
 * The ROM in the game's folder comes first: rom.z64 next to the executable (found through
 * /proc/self/exe, never argv[0]; miyoo_pack.py --rom puts the player's dump there), then rom.z64 or
 * the PC port's pd.ntsc-final.z64 in --basedir. It is mapped where it is, so nothing is copied and
 * nothing is written next to it. Its SHA-1 is checked once (its size and time are kept in
 * rom_check.ok); after that only its size, as perfectdark.z64's, then romdata.c's header checks.
 * Missing, or failing the check: perfectdark.z64, else the card.
 *
 * The ROM finder looks for a Perfect Dark dump (.z64/.n64/.v64, any byte order) anywhere up to two
 * folders below the card root (/mnt/SDCARD; PD_CARD_ROOT=<dir> for tests), the save folder passed
 * over, and accepts only the one whose SHA-1 in z64 order is Perfect Dark (USA) (Rev 1)'s, the
 * version this port is built from. It is written to perfectdark.z64 in z64 order (.tmp, fsync,
 * rename, the folder's fsync): the game maps the ROM for the whole run, so it must be a z64 file
 * on the card, never a copy swapped in memory.
 *
 * The work is shown on the screen (pd_romscreen.c): "Checking your ROM" with a bar, and the errors
 * in words until a button (no ROM: where to put it; a Perfect Dark dump refused: why -- PAL,
 * Japanese, the USA 1.0 (Rev 0), damaged or modified, not 32 MB; the copy can't be written). A
 * normal start opens no screen.
 *
 * The work runs in a child process (fork) and the game's process only draws its progress: the
 * search, the reads and the hashing allocate (opendir, stdio), and a start that did that work would
 * lay the heap out differently from a normal one. Save states hold pointers into the heap and the
 * ROM mapping, so an auto save made in the first session must load after a normal start. The
 * normal start's path (the record, the copy's size) uses system calls only.
 *
 * PD_ROM=<file> (or --rom-file): that ROM as it is, nothing checked here (tests, developers). */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fs.h"
#include "pd_romscreen.h"

#define PD_ROM_SIZE 33554432u /* romdata.c ROMDATA_ROM_SIZE */
#define PD_CARD_ROOT "/mnt/SDCARD"
#define PD_COPY "perfectdark.z64"
#define PD_LOCAL_ROM "rom.z64"
#define PD_LOCAL_ROM_OK "rom_check.ok"
#define PD_LOCAL_WHERE " (in the game's folder)"
#define MB 0x100000u

static const char kRomSha1[] = "af8788ac4d1a57260eae9c53ffe851fcf2a3319b"; /* Perfect Dark (USA) (Rev 1), z64 */
#ifdef PD_CARD_TESTSHA
/* test builds only (-DPD_CARD_TESTSHA): PD_CARD_TESTSHA=<sha1> takes another image for the dump.
 * Never in a release. */
#define kRomSha1 (getenv("PD_CARD_TESTSHA") ? getenv("PD_CARD_TESTSHA") : kRomSha1)
#endif

static const char kFoot[] = "This happens once.";
static const char kCardL1[] = "First start: checking your ROM";
static const char kCardL2[] = "and saving a copy for the game.";
static const char kLocalL1[] = "New rom.z64: checking it";
static const char kLocalL2[] = "once, before the game starts.";

/* ------------------------------------------------------------------------------------ */
/* the child's messages to the game's process (one write each, under PIPE_BUF: never split) */

enum { MSG_PROGRESS, MSG_DONE, MSG_FAIL };
struct CardMsg {
    int kind, pct;
    char sub[64], l1[64], l2[64], l3[64], foot[32];
    char path[1024];
};

static int sPipe = -1; /* the child's end */

static void put(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

static void card_send(int kind, int pct, const char *sub, const char *l1, const char *l2, const char *l3,
                      const char *foot, const char *path)
{
    struct CardMsg m;
    const char *p = (const char *)&m;
    size_t left = sizeof(m);
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.pct = pct;
    put(m.sub, sizeof(m.sub), sub);
    put(m.l1, sizeof(m.l1), l1);
    put(m.l2, sizeof(m.l2), l2);
    put(m.l3, sizeof(m.l3), l3);
    put(m.foot, sizeof(m.foot), foot);
    put(m.path, sizeof(m.path), path);
    while (left > 0) {
        const ssize_t w = write(sPipe, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return;
        p += w;
        left -= (size_t)w;
    }
}

static void card_fail(const char *sub, const char *l1, const char *l2, const char *l3) __attribute__((noreturn));
static void card_fail(const char *sub, const char *l1, const char *l2, const char *l3)
{
    fflush(stdout);
    card_send(MSG_FAIL, -1, sub, l1, l2, l3, NULL, NULL);
    _exit(1);
}

static void card_done(const char *path) __attribute__((noreturn));
static void card_done(const char *path)
{
    fflush(stdout);
    card_send(MSG_DONE, 100, NULL, NULL, NULL, NULL, NULL, path);
    _exit(0);
}

/* ------------------------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-1), fed a megabyte at a time                                          */

typedef struct {
    uint32_t h[5];
    uint64_t n;
    uint8_t buf[64];
    uint32_t fill;
} Sha1;

#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    static const uint32_t h0[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    memset(s, 0, sizeof(*s));
    memcpy(s->h, h0, sizeof(h0));
}

static void sha1_update(Sha1 *s, const uint8_t *p, uint32_t n)
{
    s->n += n;
    while (n > 0) {
        if (s->fill == 0 && n >= 64) {
            sha1_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        {
            uint32_t k = 64 - s->fill < n ? 64 - s->fill : n;
            memcpy(s->buf + s->fill, p, k);
            s->fill += k;
            p += k;
            n -= k;
            if (s->fill == 64) {
                sha1_block(s, s->buf);
                s->fill = 0;
            }
        }
    }
}

static void sha1_final(Sha1 *s, char hex[41])
{
    const uint64_t bits = s->n * 8;
    const uint32_t padn = (s->fill < 56 ? 56 : 120) - s->fill;
    uint8_t pad[72];
    uint32_t i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, pad, padn + 8);
    for (i = 0; i < 5; i++)
        sprintf(hex + i * 8, "%08x", s->h[i]);
}

/* ------------------------------------------------------------------------------------ */
/* byte orders: z64 (big-endian, 80 37 12 40), v64 (byte-swapped, 37 80 40 12), n64 (word-swapped,
 * 40 12 37 80) */

enum { ORDER_NONE, ORDER_Z64, ORDER_V64, ORDER_N64 };

static int rom_order(const uint8_t *p)
{
    if (p[0] == 0x80 && p[1] == 0x37 && p[2] == 0x12 && p[3] == 0x40) return ORDER_Z64;
    if (p[0] == 0x37 && p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x12) return ORDER_V64;
    if (p[0] == 0x40 && p[1] == 0x12 && p[2] == 0x37 && p[3] == 0x80) return ORDER_N64;
    return ORDER_NONE;
}

/* to z64 order in place; n a multiple of 4 */
static void rom_to_z64(uint8_t *p, uint32_t n, int order)
{
    uint32_t i;
    if (order == ORDER_V64) {
        for (i = 0; i + 1 < n; i += 2) { const uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    } else if (order == ORDER_N64) {
        for (i = 0; i + 3 < n; i += 4) {
            const uint8_t t0 = p[i], t1 = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = t1; p[i + 3] = t0;
        }
    }
}

static int is_rom_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    char ext[5];
    int i;
    if (dot == NULL || strlen(dot) != 4)
        return 0;
    for (i = 0; i < 4; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[4] = 0;
    return strcmp(ext, ".z64") == 0 || strcmp(ext, ".n64") == 0 || strcmp(ext, ".v64") == 0;
}

static const char *base_name(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* ------------------------------------------------------------------------------------ */
/* the check (and the copy), in the child                                                */

static struct {
    char sub[64];
    const char *l1, *l2, *stage;
    uint32_t done, total;
} sProg;

static void prog_draw(void)
{
    char st[64];
    snprintf(st, sizeof(st), "%s...", sProg.stage);
    card_send(MSG_PROGRESS, sProg.total ? (int)(sProg.done * 99 / sProg.total) : 0, sProg.sub, sProg.l1, sProg.l2,
              st, kFoot, NULL);
}

static void prog_step(void) { sProg.done++; prog_draw(); }
static void prog_stage(const char *s) { sProg.stage = s; prog_draw(); }

/* Why the last Perfect Dark dump was refused, in words for the screen (NULL: none was; another
 * game's ROM is passed over in the log only), and the same for the game folder's ROM. */
static const char *sReject, *sLocalReject;
static char sRejectName[64], sLocalName[64];
static char sCopyErr[96];

static void dir_sync(const char *file)
{
    char dir[PATH_MAX], *slash;
    int d;
    snprintf(dir, sizeof(dir), "%s", file);
    slash = strrchr(dir, '/');
    if (slash == NULL)
        return;
    *slash = 0;
    d = open(dir[0] ? dir : "/", O_RDONLY);
    if (d >= 0) {
        fsync(d);
        close(d);
    }
}

/* The dump this port needs: an N64 header in any byte order, Perfect Dark's title, 32 MB, the USA
 * cart, Rev 1's SHA-1 in z64 order. 1 if it is; 0 and why not (*why: the log; *say: the screen's
 * sentence when the file is a Perfect Dark dump, else NULL). out != NULL: the dump is also written
 * there in z64 order (a second read, hashed again: the copy is exactly what checked out). A copy
 * that can't be written: -1 (sCopyErr says why). */
static int card_check(const char *path, const char *where, const char *out, const char **why, const char **say)
{
    struct stat st;
    uint8_t hdr[0x40], *buf;
    char hex[41];
    int fd, order, pass, ret = 0;
    *say = NULL;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "not a file";
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        *why = fd < 0 ? "can't be read" : "not an N64 ROM";
        if (fd >= 0) close(fd);
        return 0;
    }
    close(fd);
    order = rom_order(hdr);
    if (order == ORDER_NONE) {
        *why = "not an N64 ROM";
        return 0;
    }
    rom_to_z64(hdr, sizeof(hdr), order);
    if (strncasecmp((const char *)hdr + 0x20, "Perfect Dark", 12) != 0) {
        *why = "not a Perfect Dark ROM";
        return 0;
    }
    printf("[card] %s: a Perfect Dark ROM (%s order), ID %.4s, country '%c', revision byte (0x3F) %u\n", path,
           order == ORDER_Z64 ? "z64" : order == ORDER_V64 ? "v64" : "n64", (const char *)hdr + 0x3B,
           hdr[0x3E] >= 0x20 && hdr[0x3E] < 0x7F ? hdr[0x3E] : '?', hdr[0x3F]);
    if ((uint64_t)st.st_size != PD_ROM_SIZE) {
        *why = "not a 32 MB file";
        *say = "It is not a 32 MB ROM.";
        return 0;
    }
    if (hdr[0x3E] != 'E') { /* the cart's country code: E USA, P Europe, J Japan */
        *why = "not the Perfect Dark (USA) dump";
        *say = hdr[0x3E] == 'P' ? "It is the European (PAL) version." :
               hdr[0x3E] == 'J' ? "It is the Japanese version." : "It is not the USA version.";
        return 0;
    }
    snprintf(sProg.sub, sizeof(sProg.sub), "%s%s", base_name(path), where);
    sProg.done = 0;
    sProg.total = PD_ROM_SIZE / MB * (out ? 2 : 1) + (out ? 1 : 0);
    buf = malloc(MB);
    if (buf == NULL) {
        *why = "out of memory";
        *say = "It could not be read (out of memory).";
        return 0;
    }
    for (pass = 0; pass < (out ? 2 : 1); pass++) {
        char tmp[PATH_MAX + 8] = "";
        Sha1 s;
        uint32_t got;
        int w = -1, ok = 1;
        ret = 0;
        sha1_init(&s);
        fd = open(path, O_RDONLY);
        if (pass == 1) {
            prog_stage("Saving a copy");
            snprintf(tmp, sizeof(tmp), "%s.tmp", out);
            w = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (w < 0) {
                snprintf(sCopyErr, sizeof(sCopyErr), "%s", strerror(errno));
                if (fd >= 0) close(fd);
                ret = -1;
                break;
            }
        } else {
            prog_stage("Checking");
        }
        for (got = 0; fd >= 0 && got < PD_ROM_SIZE; got += MB) {
            size_t have = 0;
            while (have < MB) {
                const ssize_t r = read(fd, buf + have, MB - have);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r <= 0)
                    break;
                have += (size_t)r;
            }
            if (have != MB)
                break;
            rom_to_z64(buf, MB, order);
            sha1_update(&s, buf, MB);
            if (w >= 0) {
                size_t put = 0;
                while (ok && put < MB) {
                    const ssize_t r = write(w, buf + put, MB - put);
                    if (r < 0 && errno == EINTR)
                        continue;
                    if (r <= 0) {
                        snprintf(sCopyErr, sizeof(sCopyErr), "%s", r < 0 ? strerror(errno) : "short write");
                        ok = 0;
                    } else {
                        put += (size_t)r;
                    }
                }
                if (!ok)
                    break;
            }
            prog_step();
        }
        if (fd >= 0)
            close(fd);
        if (got != PD_ROM_SIZE && ok) {
            if (w >= 0) { close(w); remove(tmp); }
            *why = "can't be read";
            *say = "It could not be read.";
            break;
        }
        if (!ok) {
            close(w);
            remove(tmp);
            ret = -1;
            break;
        }
        sha1_final(&s, hex);
        if (strcmp(hex, kRomSha1) != 0) {
            if (w >= 0) { close(w); remove(tmp); }
            printf("[card] %s: a Perfect Dark ROM, but not the USA Rev 1 this port is built for (SHA-1 %s%s)\n", path,
                   hex, pass ? ", on the second read" : "");
            *why = "not the Perfect Dark (USA) (Rev 1) dump";
            *say = pass ? "It could not be read the same twice." :
                   hdr[0x3F] == 0 ? "It is the USA 1.0 (Rev 0) version." : "It is damaged or modified.";
            break;
        }
        if (pass == 0) {
            ret = 1;
            continue;
        }
        /* the copy: whole on the card before the rename, then the folder (on vfat a rename to a new name
         * only marks the folder dirty: a power cut before its writeback left a 0-byte file) */
        prog_stage("Finishing the copy");
        if (fsync(w) != 0 || rename(tmp, out) != 0) {
            snprintf(sCopyErr, sizeof(sCopyErr), "%s", strerror(errno));
            close(w);
            remove(tmp);
            ret = -1;
            break;
        }
        fsync(w);
        close(w);
        dir_sync(out);
        prog_step();
        ret = 1;
    }
    free(buf);
    if (ret == 0 && *why == NULL)
        *why = "can't be read";
    return ret;
}

/* A dump that checks out, written as out: 1; not the dump: 0. Can't be written: the error screen
 * (another dump on the card would fail the same way). */
static int card_try(const char *path, const char *out)
{
    const char *why = NULL, *say = NULL;
    const int r = card_check(path, "", out, &why, &say);
    if (r < 0) {
        printf("[card] %s: verified, but writing %s failed (%s)\n", path, out, sCopyErr);
        card_fail(sProg.sub, "Can't save the ROM copy:", sCopyErr, "Is the SD card full or locked?");
    }
    if (r == 0) {
        printf("[card] %s: %s, passed over\n", path, why);
        if (say != NULL) {
            sReject = say;
            snprintf(sRejectName, sizeof(sRejectName), "%s", base_name(path));
        }
        return 0;
    }
    printf("[card] %s: Perfect Dark (USA) (Rev 1) verified, saved as %s\n", path, out);
    return 1;
}

static int card_search(const char *dir, int depth, const char *skip, const char *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[PATH_MAX], real[PATH_MAX];
    int found = 0;
    if (d == NULL)
        return 0;
    while (!found && (e = readdir(d)) != NULL) {
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2 && !(realpath(path, real) != NULL && strcmp(real, skip) == 0))
                found = card_search(path, depth + 1, skip, out);
        } else if (is_rom_name(e->d_name)) {
            found = card_try(path, out);
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------------------------------------------------ */
/* the paths; the normal start (system calls only, in the game's process)                */

static char sSave[PATH_MAX];  /* the save folder */
static char sLocal[3][PATH_MAX]; /* the game folder's ROM, where it may be */
static int sNumLocal;

static void card_paths(void)
{
    char self[PATH_MAX], base[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    int i, k;
    snprintf(sSave, sizeof(sSave), "%s", fsFullPath("$S"));
    snprintf(base, sizeof(base), "%s", fsFullPath("$B"));
    sNumLocal = 0;
    if (n > 0 && n < (ssize_t)sizeof(self) - 1) { /* a full buffer may be cut short */
        char *slash;
        self[n] = 0;
        slash = strrchr(self, '/');
        if (slash != NULL) {
            *slash = 0;
            snprintf(sLocal[sNumLocal++], PATH_MAX, "%s/" PD_LOCAL_ROM, self);
        }
    }
    if (base[0]) {
        snprintf(sLocal[sNumLocal++], PATH_MAX, "%s/" PD_LOCAL_ROM, base);
        snprintf(sLocal[sNumLocal++], PATH_MAX, "%s/pd.ntsc-final.z64", base); /* the PC port's name */
    }
    /* the same file twice (the executable in --basedir, as launch.sh runs it): once */
    for (i = 1; i < sNumLocal; i++) {
        struct stat a, b;
        for (k = 0; k < i; k++)
            if (stat(sLocal[i], &a) == 0 && stat(sLocal[k], &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                memmove(sLocal[i], sLocal[i + 1], (size_t)(sNumLocal - i - 1) * PATH_MAX);
                sNumLocal--;
                i--;
                break;
            }
    }
}

/* the first of the game folder's ROMs that exists (-1: none) */
static int local_index(struct stat *st)
{
    int i;
    for (i = 0; i < sNumLocal; i++)
        if (stat(sLocal[i], st) == 0)
            return i;
    return -1;
}

/* rom_check.ok says this rom.z64 checked out (its size, its time and the SHA-1 this build wants) */
static int local_recorded(const struct stat *st)
{
    char ok[PATH_MAX + 16], line[160], sha[48];
    unsigned long long size;
    long long mtime;
    ssize_t n;
    int fd;
    snprintf(ok, sizeof(ok), "%s/" PD_LOCAL_ROM_OK, sSave);
    fd = open(ok, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    line[n] = 0;
    return sscanf(line, "%llu %lld %47s", &size, &mtime, sha) == 3 && size == (unsigned long long)st->st_size &&
           mtime == (long long)st->st_mtime && strcmp(sha, kRomSha1) == 0;
}

/* the normal start: the ROM to map without any work, or NULL */
static const char *card_quick(char *copy, size_t len)
{
    struct stat st;
    char rb[PATH_MAX + 16];
    int i;
    snprintf(rb, sizeof(rb), "%s/rebuild", sSave);
    if (stat(rb, &st) == 0)
        return NULL;
    i = local_index(&st);
    if (i >= 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == PD_ROM_SIZE && local_recorded(&st)) {
        printf("[card] rom: %s" PD_LOCAL_WHERE "\n", sLocal[i]);
        return sLocal[i];
    }
    if (i >= 0)
        return NULL; /* a new or changed rom.z64: checked once (the child) */
    snprintf(copy, len, "%s/" PD_COPY, sSave);
    if (stat(copy, &st) == 0 && (uint64_t)st.st_size == PD_ROM_SIZE) {
        printf("[card] rom: %s\n", copy);
        return copy;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* the work (the child): the head of this file, in order                                 */

static void card_work(void)
{
    char rb[PATH_MAX + 16], ok[PATH_MAX + 16], copy[PATH_MAX + 16], real[PATH_MAX];
    const char *root = getenv("PD_CARD_ROOT") && getenv("PD_CARD_ROOT")[0] ? getenv("PD_CARD_ROOT") : PD_CARD_ROOT;
    struct stat st;
    int rebuild = 0, i, found;
    snprintf(rb, sizeof(rb), "%s/rebuild", sSave);
    snprintf(ok, sizeof(ok), "%s/" PD_LOCAL_ROM_OK, sSave);
    snprintf(copy, sizeof(copy), "%s/" PD_COPY, sSave);
    if (stat(rb, &st) == 0) {
        rebuild = 1;
        printf("[card] rebuild: %s found, the first start's job runs again\n", rb);
        remove(ok); /* the game folder's rom.z64 is checked again */
    }
    /* the game folder's ROM: checked once, the record kept */
    i = local_index(&st);
    if (i >= 0) {
        const char *why = NULL, *say = NULL;
        sProg.l1 = kLocalL1;
        sProg.l2 = kLocalL2;
        snprintf(sLocalName, sizeof(sLocalName), "%s" PD_LOCAL_WHERE, base_name(sLocal[i]));
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size == PD_ROM_SIZE && !rebuild && local_recorded(&st)) {
            card_done(sLocal[i]);
        }
        printf("[card] a new ROM in the game's folder: checking %s once\n", sLocal[i]);
        if (card_check(sLocal[i], PD_LOCAL_WHERE, NULL, &why, &say) > 0) {
            FILE *f = fopen(ok, "w"); /* a torn or lost record only means one more check */
            int w = -1;
            if (f != NULL) {
                w = fprintf(f, "%llu %lld %s\n", (unsigned long long)st.st_size, (long long)st.st_mtime, kRomSha1);
                if (fclose(f) != 0)
                    w = -1;
            }
            if (w < 0)
                printf("[card] %s can't be written: the ROM in the game's folder is checked again next start\n", ok);
            if (rebuild && remove(rb) == 0)
                printf("[card] rebuild: done, %s removed\n", rb);
            printf("[card] rom: %s" PD_LOCAL_WHERE "\n", sLocal[i]);
            card_done(sLocal[i]);
        }
        printf("[card] %s: %s, passed over for the card\n", sLocal[i], why);
        sLocalReject = say ? say : "It is not a Perfect Dark ROM.";
    }
    /* the copy made at the first start */
    if (!rebuild && stat(copy, &st) == 0 && (uint64_t)st.st_size == PD_ROM_SIZE) {
        printf("[card] rom: %s\n", copy);
        card_done(copy);
    }
    /* the card */
    printf("[card] first start: looking for your Perfect Dark (USA) ROM under %s\n", root);
    sProg.l1 = kCardL1;
    sProg.l2 = kCardL2;
    card_send(MSG_PROGRESS, 0, "Looking on the SD card", kCardL1, kCardL2, "Looking for your ROM...", kFoot, NULL);
    if (realpath(sSave, real) == NULL)
        snprintf(real, sizeof(real), "%s", sSave);
    found = card_search(root, 0, real, copy);
    if (!found && rebuild && stat(copy, &st) == 0) {
        /* a rebuild with the dump gone from the card: the copy itself is checked in full */
        const char *why = NULL, *say = NULL;
        if (card_check(copy, "", NULL, &why, &say) <= 0) {
            printf("[card] rebuild: no dump on the card and %s: %s\n", copy, why);
            card_fail(PD_COPY, "The ROM copy failed its check.", say ? say : "It could not be read.",
                      "Copy your USA ROM to the SD card.");
        }
        printf("[card] rebuild: no dump on the card; %s verified in place\n", copy);
        found = 1;
    }
    if (!found) {
        printf("FATAL: no Perfect Dark (USA) (Rev 1) ROM found. Copy your dump (.z64, .n64 or .v64) to the\n"
               "card, at most two folders deep (the root, Roms/, Roms/N64/, ...), and start again.\n");
        if (sReject != NULL)
            card_fail(sRejectName, "This Perfect Dark ROM can't be used.", sReject, "This port needs the USA Rev 1 ROM.");
        if (sLocalReject != NULL)
            card_fail(sLocalName, "This ROM can't be used.", sLocalReject, "Use the USA Rev 1 ROM as rom.z64.");
        card_fail(NULL, "No Perfect Dark ROM found.", "Copy your USA ROM (.z64/.v64/.n64)",
                  "to the SD card, up to 2 folders deep.");
    }
    if (rebuild && remove(rb) == 0)
        printf("[card] rebuild: done, %s removed\n", rb);
    printf("[card] rom: %s\n", copy);
    card_done(copy);
}

/* ------------------------------------------------------------------------------------ */

/* The ROM to map (z64 order, 32 MB; romdata.c checks its header). Returns only with one: no ROM, or
 * a refused one, ends on the error screen. */
const char *pdCardRom(void)
{
    static char copy[PATH_MAX + 16], rom[PATH_MAX];
    static struct CardMsg m; /* .bss, not the stack: PATH_MAX in it */
    const char *env = getenv("PD_ROM");
    const char *quick;
    int fds[2], status = 0;
    pid_t pid;
    if (env != NULL && env[0]) {
        printf("[card] rom: %s (PD_ROM, not checked here)\n", env);
        return env;
    }
    card_paths();
    quick = card_quick(copy, sizeof(copy));
    if (quick != NULL)
        return quick;
    fflush(stdout);
    fflush(stderr);
    if (pipe(fds) != 0) {
        printf("[card] pipe: %s\n", strerror(errno));
        pd_romscreen_fail(NULL, "The ROM check could not start.", strerror(errno), NULL);
    }
    pid = fork();
    if (pid < 0) {
        printf("[card] fork: %s\n", strerror(errno));
        pd_romscreen_fail(NULL, "The ROM check could not start.", strerror(errno), NULL);
    }
    if (pid == 0) {
        close(fds[0]);
        sPipe = fds[1];
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* the game killed: the work stops too (the copy is a .tmp until done) */
        signal(SIGTERM, SIG_DFL);
        card_work();
        _exit(1);
    }
    close(fds[1]);
    rom[0] = 0;
    for (;;) {
        size_t have = 0;
        while (have < sizeof(m)) {
            const ssize_t r = read(fds[0], (char *)&m + have, sizeof(m) - have);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            have += (size_t)r;
        }
        if (have < sizeof(m))
            break;
        m.sub[sizeof(m.sub) - 1] = m.l1[sizeof(m.l1) - 1] = m.l2[sizeof(m.l2) - 1] = 0;
        m.l3[sizeof(m.l3) - 1] = m.foot[sizeof(m.foot) - 1] = m.path[sizeof(m.path) - 1] = 0;
        if (m.kind == MSG_PROGRESS) {
            pd_romscreen_progress(m.sub, m.l1, m.l2, m.l3, m.pct, m.foot);
        } else if (m.kind == MSG_DONE) {
            snprintf(rom, sizeof(rom), "%s", m.path);
        } else if (m.kind == MSG_FAIL) {
            close(fds[0]);
            waitpid(pid, &status, 0);
            pd_romscreen_fail(m.sub[0] ? m.sub : NULL, m.l1, m.l2[0] ? m.l2 : NULL, m.l3[0] ? m.l3 : NULL);
        }
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!rom[0]) {
        printf("[card] the ROM check stopped (status 0x%x)\n", status);
        pd_romscreen_fail(NULL, "The ROM check stopped.", "See App/PerfectDark/data/console.log.", NULL);
    }
    pd_romscreen_finish();
    return rom;
}
