/**
 * Port odds and ends: logging, the data directory, atomic file writes, the
 * overlay reset, C versions of the decomp's hand-written gu routines, and the
 * 64DD entry points the cartridge game links against (no drive on this port).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "port_platform.h" /* port_time_us */
#include "handheld_menu.h"   /* GAME SAVE FAILED on the OS bar (a no-op off the handheld) */

/* ---------------------------------------------------------------- logging --- */
static FILE* sLog;

void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vdprintf(1, fmt, ap); /* fd, not stdout: naming stdout copies it into the executable (gate 2b) */
    va_end(ap);
    if (sLog != NULL) {
        va_start(ap, fmt);
        vfprintf(sLog, fmt, ap);
        va_end(ap);
        fflush(sLog);
    }
}

/* The log's file descriptor, for the crash handler (write() only); -1 before it opens. */
int port_log_fd(void) {
    return sLog != NULL ? fileno(sLog) : -1;
}
/* The same for gfx_sdl12.c's log lines (its weak default has no log file). */
int handheld_log_fd(void) {
    return port_log_fd();
}

void port_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    dprintf(2, "FATAL: ");
    vdprintf(2, fmt, ap);
    dprintf(2, "\n");
    va_end(ap);
    if (sLog != NULL) {
        va_start(ap, fmt);
        fprintf(sLog, "FATAL: ");
        vfprintf(sLog, fmt, ap);
        fprintf(sLog, "\n");
        va_end(ap);
        fclose(sLog);
        sLog = NULL; /* the crash handler that abort() triggers must not touch it */
    }
    abort();
}

/* ------------------------------------------------------------- data dir --- */
static char sDataDir[256] = "./data/";

void port_set_data_dir(const char* dir) {
    snprintf(sDataDir, sizeof(sDataDir), "%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/");
    mkdir(sDataDir, 0777);
    if (sLog == NULL) {
        /* keep the last three runs' logs: a device test is several sessions */
        rename(port_data_path("console.1.log"), port_data_path("console.2.log"));
        rename(port_data_path("console.log"), port_data_path("console.1.log"));
        sLog = fopen(port_data_path("console.log"), "w");
    }
}

const char* port_data_path(const char* name) {
    static char buf[4][512];
    static int i;
    char* b = buf[i++ & 3];
    snprintf(b, sizeof(buf[0]), "%s%s", sDataDir, name);
    return b;
}

/* Write-to-temp, fsync, rename, fsync the renamed file, fsync the directory: a power cut leaves
 * either the old file or the new one, never a short one (MK64 lesson).
 * The renamed file's fsync (2026-10-01, super-smash-bros cd5128c, mario-kart-64 2ba9042): on the card's
 * vfat (Linux 4.14 vfat_rename) a rename over an existing file moves the new file into the OLD
 * file's directory slot (under a new name: a new slot with cluster 0 and size 0) and only marks its
 * inode dirty; the slot's start cluster and size reach the card at that inode's writeback (~30 s) or
 * its own fsync. The folder's fsync writes the removed .tmp entry and the FAT with the old file's
 * clusters freed, not that slot, so a hard power cut in those ~30 s (a hang and a forced power-off,
 * a flat battery) left fzerox.sram pointing at freed clusters: the records and unlocks just written
 * lost and the FAT inconsistent. Reopened for it, so a failed close still keeps the old file. The
 * same for rompath.txt and romcheck.txt (the first start's ROM check).
 * A failed write, fsync or close keeps the old file: the fsync's and close's results used to be
 * ignored, so an I/O error there still renamed a file that may never have reached the card over
 * the good one; a failure left <file>.tmp behind (which sram_load reads when fzerox.sram is
 * missing); and nothing was logged, so a lost SRAM write (records, unlocks) was silent. */
int port_errno(void); /* sdl/port_errno.c: <errno.h> clashes with the N64 headers */
static int write_fail(const char* path, const char* tmp, const char* stage, int err) {
    remove(tmp);
    port_log("file: %s not written (%s: %s); the old one is kept\n", path, stage, err ? strerror(err) : "short write");
    return 0;
}
int port_write_file_atomic(const char* path, const void* data, size_t size) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return write_fail(path, tmp, "open", port_errno());
    }
    const u8* p = data;
    size_t left = size;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            const int err = n < 0 ? port_errno() : 0;
            close(fd);
            return write_fail(path, tmp, "write", err);
        }
        p += n;
        left -= (size_t) n;
    }
    if (fsync(fd) != 0) {
        const int err = port_errno();
        close(fd);
        return write_fail(path, tmp, "fsync", err);
    }
    if (close(fd) != 0) {
        return write_fail(path, tmp, "close", port_errno());
    }
    if (rename(tmp, path) != 0) {
        return write_fail(path, tmp, "rename", port_errno());
    }
    fd = open(path, O_RDONLY); /* the renamed file's directory slot (above) */
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    int dfd = open(sDataDir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 1;
}

/* ------------------------------------------------------------- fzerox.sram --- */
/* The cartridge's 32 KB SRAM (records, ghosts, unlocks, options, machine settings). ultra_os.c keeps
 * the image (game memory: it is in save states); the file side is here, in a port object, outside
 * them, so a state load never changes what the card's file may take (2026-10-01, the cross-port
 * list's J).
 * - sSramKeep: fzerox.sram is there but could not be opened or read (a card read error: EIO, a broken
 *   cluster chain). It used to be taken for a missing one: the game saw a blank cartridge, formatted
 *   it at boot and the write renamed that blank image over the player's records, without a word.
 *   Now it is kept as it is for the session: every write is refused, logged and shown
 *   (paper-mario 742bdb4, super-smash-bros S3, diddy-kong-racing 4bbc6fe, mario-kart-64 b95117d). A later start
 *   that can read it finds it as it was; one that can't says so in the log each time. A missing file
 *   is a new save, as before.
 * - A failed write (port_write_file_atomic logs its stage and reason and keeps the old file) is shown:
 *   the game ignores the write's result and goes on as if saved, so a full or read-only card lost a
 *   record without a word on the screen (mario-kart-64 37c208f, diddy-kong-racing 8938223, goldeneye-007
 *   3b09a41). GAME SAVE FAILED at most once in 2 s: a record writes its two profile copies, its ghost
 *   and its machine back to back. Every failure is logged. Each write is the whole image, so the next
 *   game write tries again whatever it changes; the log says when one lands after failed ones. (No
 *   extra try at exit: the image is in save states, and after a LOAD it can be older than the file.) */
void port_errno_clear(void);     /* sdl/port_errno.c */
int port_errno_missing(int err); /* 1: "no such file" */
static int sSramKeep, sSramFails;

/* path into img: 0 read (*got bytes), 1 no such file, else the errno (-1: unknown) of a file that is
 * there but could not be opened or read. */
static int sram_read_file(const char* path, void* img, size_t size, size_t* got) {
    FILE* f;
    int bad, err;
    *got = 0;
    port_errno_clear();
    f = fopen(path, "rb");
    if (f == NULL) {
        err = port_errno();
        return port_errno_missing(err) ? 1 : err ? err : -1;
    }
    port_errno_clear();
    *got = fread(img, 1, size, f);
    bad = ferror(f);
    err = port_errno();
    fclose(f);
    return !bad ? 0 : err ? err : -1;
}

/* ultra_os.c's sram_load (img zeroed): the bytes read from fzerox.sram, or from fzerox.sram.tmp when
 * a cut rename left only that (as before). */
size_t port_sram_read(void* img, size_t size) {
    char path[512], tmp[600];
    const char* which = path;
    size_t got;
    int r;
    snprintf(path, sizeof(path), "%s", port_data_path("fzerox.sram"));
    r = sram_read_file(path, img, size, &got);
    if (r == 1) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", path); /* a rename that did not finish */
        r = sram_read_file(tmp, img, size, &got);
        if (r == 1) {
            return 0; /* no save yet: the game formats a blank cartridge */
        }
        if (r == 0) {
            port_log("sram: fzerox.sram missing, using fzerox.sram.tmp (%u bytes)\n", (unsigned) got);
            return got;
        }
        which = tmp;
    }
    if (r != 0) {
        sSramKeep = 1;
        port_log("sram: %s cannot be read (%s, %u bytes): kept as it is, no game save replaces it this session\n",
                 which, r > 0 ? strerror(r) : "read error", (unsigned) got);
    }
    return got;
}

static void sram_failed_caption(void) {
    static int sShown;
    static u32 sShownUs;
    const u32 now = port_time_us();
    if (sShown && now - sShownUs < 2000000u) {
        return;
    }
    sShown = 1;
    sShownUs = now;
    handheld_menu_notify("GAME SAVE FAILED");
}

/* ultra_os.c's sram_access, after each game write: the whole image to fzerox.sram. */
void port_sram_write(const void* img, size_t size) {
    if (sSramKeep) {
        port_log("sram: not written: fzerox.sram could not be read at the start\n");
        sram_failed_caption();
        return;
    }
    if (port_write_file_atomic(port_data_path("fzerox.sram"), img, size)) {
        if (sSramFails) {
            port_log("sram: fzerox.sram written, after %d failed %s\n", sSramFails, sSramFails == 1 ? "try" : "tries");
        }
        sSramFails = 0;
        return;
    }
    sSramFails++;
    sram_failed_caption();
}

/* statefix A (handheld-kit k1.8; savestate.c calls it after a load that happened, before the texture
 * fixup; nothing of the game runs during a load). The SRAM image (ultra_os.c's sSram) and the game's
 * RAM copies of the save are in the state, so a LOAD gave back the save as it was when the state was
 * made: a record, an unlock or an option set since was gone in the game, and the game's next write
 * (each one writes the whole image to fzerox.sram) put the old image back over the newer file. Now:
 *  - fzerox.sram is the state's image (nothing written since the state): nothing changes, the load is
 *    exact;
 *  - it differs: the image becomes fzerox.sram, and save.c's Save_PortReapply sets the game's RAM copies
 *    from it the way the game reads them at boot (its comment lists what it refreshes and what stays
 *    the state's). The game writes every change the tick it makes it, so the state never holds
 *    progress the card lacks: the card wins (a write that failed, GAME SAVE FAILED, never became the
 *    save either);
 *  - the state is from before the game's boot read (sSramGameRead): the image becomes fzerox.sram too
 *    (it is the image of the process that made the state; the restored boot reads it), nothing else;
 *  - fzerox.sram could not be read at the start (sSramKeep: no game write replaces it this session),
 *    can't be read now, is missing or is not a whole image: the state's save stands.
 * One line in the log either way. FZX_SAVEREAPPLY=0: as before (the state's save stands; tests). */
void hh_game_save_reapply(void) {
    static u8 card[0x8000]; /* a port object's: outside the state */
    extern u8* port_sram_image(void);
    extern s32 port_sram_game_read(void);
    extern s32 Save_PortReapply(void* card, void* old, s32* bad);
    const char* e = getenv("FZX_SAVEREAPPLY");
    u8* img = port_sram_image();
    char path[512], tmp[600];
    size_t got = 0;
    int r, diff = 0, i;
    s32 mask, bad = 0;
    if (e && e[0] == '0') {
        port_log("sram: FZX_SAVEREAPPLY=0: the state's save stands\n");
        return;
    }
    if (sSramKeep) {
        port_log("sram: fzerox.sram could not be read at the start; the state's save stands (no game write replaces the file)\n");
        return;
    }
    memset(card, 0, sizeof(card));
    snprintf(path, sizeof(path), "%s", port_data_path("fzerox.sram"));
    r = sram_read_file(path, card, sizeof(card), &got);
    if (r == 1) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", path); /* a rename that did not finish, as port_sram_read */
        r = sram_read_file(tmp, card, sizeof(card), &got);
    }
    if (r == 1) {
        port_log("sram: no fzerox.sram on the card; the state's save stands\n");
        return;
    }
    if (r != 0 || got != sizeof(card)) {
        port_log("sram: fzerox.sram %s (%u bytes); the state's save stands\n", r > 0 ? strerror(r) : r ? "read error" : "is not a whole image",
                 (unsigned) got);
        return;
    }
    for (i = 0; i < (int) sizeof(card); i++) {
        diff += card[i] != img[i];
    }
    if (diff == 0) {
        port_log("sram: the state's save is the card's\n");
        return;
    }
    if (!port_sram_game_read()) {
        memcpy(img, card, sizeof(card));
        port_log("sram: the state is from before the game read its save: fzerox.sram (%d bytes differ) goes into the SRAM image, "
                 "and the game reads it itself\n", diff);
        return;
    }
    mask = Save_PortReapply(card, img, &bad);
    memcpy(img, card, sizeof(card));
    port_log("sram: the card's save is newer than the state's (%d bytes): put back%s%s%s%s%s%s%s%s%s\n",
             diff, (mask & 1) ? "; records" : "", (mask & 256) ? "; Death Race" : "", (mask & 2) ? "; settings" : "",
             (mask & 4) ? "; sound mode" : "",
             (mask & 8) ? "; unlock level re-derived" : "", (mask & 16) ? "; engine settings" : "",
             (mask & 32) ? "; the record being named moved to its place in the card's list" : "",
             (mask & 64) ? "; the record being named put back in the card's list" : "",
             (mask & 128) ? "; the record being named is not in the card's top five (its name goes on its old place)" : "");
    if (bad) {
        port_log("sram: %d of the card's sections fail the game's check in both copies; the state's values kept there\n", (int) bad);
    }
}

/* ---------------------------------------------------------------- overlays --- */
/* The N64 reloads an overlay's .data from ROM and zeroes its .bss every time the
 * game switches to it. The port links every overlay in; this restores the same
 * semantics from a pristine copy of each overlay's .data taken at boot. */
#define PORT_OVERLAY(n) extern u8 n##_DATA_START[], n##_DATA_END[], n##_BSS_START[], n##_BSS_END[];
#include "overlays.h"
#undef PORT_OVERLAY

typedef struct {
    const char* name;
    u8* data;
    u8* dataEnd;
    u8* bss;
    u8* bssEnd;
    u8* pristine;
} OverlayInfo;

#define PORT_OVERLAY(n) { #n, n##_DATA_START, n##_DATA_END, n##_BSS_START, n##_BSS_END, NULL },
static OverlayInfo sOverlays[] = {
#include "overlays.h"
};
#undef PORT_OVERLAY

void port_overlays_init(void) {
    for (size_t i = 0; i < sizeof(sOverlays) / sizeof(sOverlays[0]); i++) {
        OverlayInfo* o = &sOverlays[i];
        size_t n = (size_t) (o->dataEnd - o->data);
        o->pristine = malloc(n ? n : 1);
        memcpy(o->pristine, o->data, n);
    }
}

/* Called from Dma_LoadOverlay (HANDHELD) with the overlay's .bss start, which
 * identifies it. */
void port_overlay_load(void* bssStart) {
    for (size_t i = 0; i < sizeof(sOverlays) / sizeof(sOverlays[0]); i++) {
        OverlayInfo* o = &sOverlays[i];
        if (o->bss == bssStart) {
            memcpy(o->data, o->pristine, (size_t) (o->dataEnd - o->data));
            memset(o->bss, 0, (size_t) (o->bssEnd - o->bss));
            return;
        }
    }
    port_log("overlay load: unknown overlay (bss %p)\n", bssStart);
}

/* ---------------------------------------------------------------------- gu --- */
/* libultra's hand-written asm versions (sqrtf comes from libm). */
void guNormalize(f32* x, f32* y, f32* z) {
    f32 m = 1.0f / sqrtf((*x * *x) + (*y * *y) + (*z * *z));
    *x *= m;
    *y *= m;
    *z *= m;
}

/* ------------------------------------------------------------------- 64DD --- */
/* The cartridge game only reaches these with a 64DD attached and 8 MB of RAM
 * (gRamDDCompatible); the port reports neither. */
#define LEO_STUB(name) \
    s32 name() {       \
        return -1;     \
    }
LEO_STUB(LeoBootGame)
LEO_STUB(LeoByteToLBA)
LEO_STUB(LeoCACreateLeoManager)
LEO_STUB(LeoGetAAdr2)
LEO_STUB(LeoLBAToByte)
LEO_STUB(LeoReadDiskID)
LEO_STUB(LeoReadWrite)
LEO_STUB(LeoReset)
LEO_STUB(LeoResetClear)
LEO_STUB(LeoSpdlMotor)
LEO_STUB(LeoTestUnitReady)
LEO_STUB(Mfs_CreateLeoManager)
LEO_STUB(Mfs_GetFileIndex)
LEO_STUB(Mfs_InitRamArea)
LEO_STUB(Mfs_LoadFileInDir)
LEO_STUB(Mfs_ModeSelectAsync)
LEO_STUB(Mfs_SetGameCode)
LEO_STUB(func_i1_8040428C)
LEO_STUB(func_i1_804046F0)
u8 leoBootID[0x20];
s32 gMfsError;
u8 D_i1_80428618[0x40];
