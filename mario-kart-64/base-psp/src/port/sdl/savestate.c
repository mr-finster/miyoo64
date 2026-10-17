/**
 * Save states -- see savestate.h. Snapshots the game's writable memory,
 * keeping the "dontsave" section live across a load.
 */
#include "savestate.h"

#define HH_SS_SPARSE 1 /* sparse states (kit k1.5): MK64_SS_SPARSE=0 writes them whole */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "handheld_menu.h" /* the resume prompt, notify, power-off helpers (stubs off handheld) */
#if defined(HANDHELD) && defined(MIYOO)
#include "miyoo_sleepsave.h" /* the resume state at a POWER press and when idle */
#define SS_SLEEP_JOIN() miyoo_sleepsave_join()
#else
#define SS_SLEEP_JOIN() ((void) 0)
#endif

/* Linker-provided bounds. __data_start/_end wrap all writable data; ld
 * auto-creates __start_/__stop_ for the named "dontsave" section. */
extern char __data_start[];
extern char _end[];
extern char __start_dontsave[];
extern char __stop_dontsave[];
extern char __assets_start[], __assets_end[]; /* the ROM-derived region (M6) */

extern const char *port_save_path(const char *name); /* $MK64_DATA/<name> */
extern const char *port_save_dir(void);
extern void port_request_exit(void);
extern void gfx_savestate_fixup(void);               /* flush tex cache + GE state */
/* The audio backstop thread mixes whenever the ring runs low -- which it does
 * while a state is read from the SD card. Mixing on
 * a half-restored heap crashed the first on-device load (round 6). Both the
 * snapshot and the restore run with the backstop parked and the mix lock
 * held, so no mixer touches game memory while it is in flux. */
extern void port_audio_backstop_enable(int on);
extern void port_audio_mix_lock(void);
extern void port_audio_mix_unlock(void);

/* A state is only valid for the exact executable that wrote it: .data/.bss sit
 * at fixed addresses and .data holds function pointers. port_build_stamp is
 * regenerated whenever the link's inputs change (Makefile.handheld), so any
 * rebuild -- including one that never recompiles this file -- makes older
 * states unloadable instead of a crash; the header also records where .data
 * sat, in case a layout moves without a new stamp. */
extern const uint32_t port_build_stamp; /* build/<plat>/build_stamp.c */
static void build_id(char out[32]) { snprintf(out, 32, "mk64 build %08X", (unsigned) port_build_stamp); }
#define SS_MAGIC 0x4D4B3653u        /* 'MK6S': both ranges written whole */
#define SS_MAGIC_SPARSE 0x4D4B365Au /* 'MK6Z': sparse (below); either loads */

typedef struct {
    uint32_t magic;
    uint32_t region_size; /* _end - __data_start */
    uint32_t hole_off;    /* dontsave section, relative to __data_start */
    uint32_t hole_size;
    uint32_t data_start;  /* __data_start of the build that wrote it */
    char     build[32];
} SaveHeader;

/* Sparse states (kit k1.5, from GoldenEye e037c3b): each of the two ranges is written as a bitmap
 * of its 4 KB pages, then only the pages that aren't all zero; a page missing from the file is
 * zeroed on load. The SD card's write + fsync time is what the player waits for (the power-off
 * save, the EXIT save). MK64_SS_SPARSE=0 writes the whole ranges ('MK6S', as before this build). */
#define SS_PAGE 4096u
typedef struct { char *base; size_t size; } SsRange;
static size_t ss_pages(size_t size) { return (size + SS_PAGE - 1) / SS_PAGE; }
static size_t ss_page_len(const SsRange *r, size_t p) {
    const size_t off = p * SS_PAGE;
    return r->size - off < SS_PAGE ? r->size - off : SS_PAGE;
}
static int ss_page_zero(const char *p, size_t len) {
    const uint32_t *w = (const uint32_t *) p;
    size_t i;
    for (i = 0; i < len / 4; i++) if (w[i]) return 0;
    for (i = len & ~(size_t) 3; i < len; i++) if (p[i]) return 0;
    return 1;
}
static int ss_sparse(void) {
#if defined(HH_SS_SPARSE)
    static int on = -1;
    if (on < 0) { const char *e = getenv("MK64_SS_SPARSE"); on = !(e && e[0] == '0'); }
    return on;
#else
    return 0;
#endif
}
static void ss_ranges(SsRange r[2], size_t region, size_t hole_off, size_t hole_size) {
    r[0].base = __data_start;                         r[0].size = hole_off;
    r[1].base = __data_start + hole_off + hole_size;  r[1].size = region - hole_off - hole_size;
}
/* Save timing (GoldenEye, 2026-09-27: a Train save hung the device inside fsync/rename): every save
 * logs each stage's ms and the kernel's free/dirty memory, so a slow or stuck card shows where. */
#include <time.h>
static double ss_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void ss_meminfo(char *out, size_t cap) { /* "free 12345k avail 23456k dirty 678k" (Linux), or "" */
    FILE *m = fopen("/proc/meminfo", "r");
    char line[128];
    long fr = -1, di = -1, av = -1;
    out[0] = 0;
    if (!m) return;
    while (fgets(line, sizeof(line), m)) {
        long v;
        if (sscanf(line, "MemFree: %ld", &v) == 1) fr = v;
        else if (sscanf(line, "MemAvailable: %ld", &v) == 1) av = v;
        else if (sscanf(line, "Dirty: %ld", &v) == 1) di = v;
    }
    fclose(m);
    snprintf(out, cap, "free %ldk avail %ldk dirty %ldk", fr, av, di);
}

/* Excluded from the snapshot so a load never clobbers the request itself. */
static int sRequest SAVESTATE_EXCLUDE;      /* >0 save slot, <0 load slot, 0 idle */
static char sProgName[512] SAVESTATE_EXCLUDE;
static char sBootPath[512] SAVESTATE_EXCLUDE; /* --loadStateFile <path>, or the auto save to resume */
static int  sBootFrames SAVESTATE_EXCLUDE;
static int  sResumeSave SAVESTATE_EXCLUDE;   /* power-off: quicksave, then quit */
static int  sExitSave SAVESTATE_EXCLUDE;      /* EXIT GAME: quicksave, then quit */
static volatile int sLoopRunning SAVESTATE_EXCLUDE;
#define BOOT_LOAD_FRAME 3 /* game init has run; the title screen has drawn */

void savestate_set_progname(const char *argv0) {
    if (argv0) { strncpy(sProgName, argv0, sizeof(sProgName) - 1); sProgName[sizeof(sProgName) - 1] = 0; }
}
void savestate_parse_args(int argc, char **argv) {
    if (argc > 0) savestate_set_progname(argv[0]);
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--loadStateFile") == 0) {
            strncpy(sBootPath, argv[i + 1], sizeof(sBootPath) - 1);
            sBootPath[sizeof(sBootPath) - 1] = 0;
            fprintf(stderr, "savestate: will resume from %s\n", sBootPath);
        }
    }
}
int savestate_boot_load_pending(void) { return sBootPath[0] != 0; }
/* Separate from sRequest, so a menu SAVE/LOAD confirmed just before the power
 * switch still runs first. */
void savestate_request_resume_save(void) { sResumeSave = 1; }
void savestate_request_exit_save(void) { sExitSave = 1; }

static void slot_name(int slot, char *out, size_t n) {
    if (slot == QUICKSAVE_SLOT) snprintf(out, n, "state_quick.sav");
    else                        snprintf(out, n, "state_%02d.sav", slot);
}

int savestate_slot_exists(int slot) {
    char name[32]; slot_name(slot, name, sizeof(name));
    FILE *f = fopen(port_save_path(name), "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* The snapshot runs from __data_start to the end of the game's writable data,
 * minus the "dontsave" hole (texture arena, framebuffers, SDL handles, audio
 * ring), which is neither written nor read so nothing live is overwritten.
 *
 * The end is __assets_start, not _end: the asset region linked after .bss is
 * ROM data rebuilt at boot from the player's cache (assets_handheld.c) and never
 * written by the game, so it is not state. Dropping it takes a state from
 * ~17 MB to ~4.5 MB -- which matters most for the power-off quicksave, where
 * the OS only allows a short window before it powers down. A build with the
 * assets linked in has an empty region and keeps the old bound. */
static void layout(size_t *region, size_t *hole_off, size_t *hole_size) {
    const char *end = _end;
    if (&__assets_end[0] == &_end[0] && &__assets_start[0] > &__data_start[0] && &__assets_start[0] < &_end[0]) {
        end = __assets_start;
    }
    *region = (size_t)(end - __data_start);
    if (__start_dontsave >= __data_start && __stop_dontsave <= end && __stop_dontsave > __start_dontsave) {
        *hole_off = (size_t)(__start_dontsave - __data_start);
        *hole_size = (size_t)(__stop_dontsave - __start_dontsave);
    } else {
        *hole_off = 0; *hole_size = 0;
    }
}

/* One check for the boot prompt, the menu and the loader: magic, layout,
 * build id and exact size (header + region minus the hole; shorter = cut off
 * mid-write). Leaves f just past the header. A read error is not a mismatch --
 * only a mismatch may ever delete a file. */
enum { STATE_OK = 1, STATE_MISMATCH = 0, STATE_UNREADABLE = -1 };
static int state_check(FILE *f) {
    size_t region, hole_off, hole_size;
    layout(&region, &hole_off, &hole_size);
    SaveHeader h;
    char id[32];
    build_id(id);
    if (fread(&h, sizeof(h), 1, f) != 1) return ferror(f) ? STATE_UNREADABLE : STATE_MISMATCH;
    if ((h.magic != SS_MAGIC && h.magic != SS_MAGIC_SPARSE) || h.region_size != region || h.hole_off != hole_off ||
        h.hole_size != hole_size || h.data_start != (uint32_t)(uintptr_t)__data_start ||
        strncmp(h.build, id, sizeof(h.build)) != 0) return STATE_MISMATCH;
    long want = (long) (sizeof(SaveHeader) + region - hole_size);
    const long pos = ftell(f);
    if (h.magic == SS_MAGIC_SPARSE) { /* the bitmaps say how long the file must be */
        SsRange r[2];
        ss_ranges(r, region, hole_off, hole_size);
        want = (long) sizeof(SaveHeader);
        long at = pos;
        for (int i = 0; i < 2; i++) {
            const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
            unsigned char bm[2048];
            if (nb > sizeof(bm) || (nb && (fseek(f, at, SEEK_SET) != 0 || fread(bm, 1, nb, f) != nb)))
                return ferror(f) ? STATE_UNREADABLE : STATE_MISMATCH;
            size_t bytes = 0;
            for (size_t p = 0; p < np; p++) if (bm[p >> 3] & (1u << (p & 7))) bytes += ss_page_len(&r[i], p);
            want += (long) (nb + bytes);
            at += (long) (nb + bytes);
        }
    }
    if (pos < 0 || fseek(f, 0, SEEK_END) != 0) return STATE_UNREADABLE;
    const long len = ftell(f);
    if (len < 0 || fseek(f, pos, SEEK_SET) != 0) return STATE_UNREADABLE;
    return len == want ? STATE_OK : STATE_MISMATCH;
}
static int state_file_check(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return STATE_UNREADABLE;
    const int st = state_check(f);
    fclose(f);
    return st;
}
int savestate_slot_valid(int slot) {
    char name[32]; slot_name(slot, name, sizeof(name));
    return state_file_check(port_save_path(name)) == STATE_OK;
}

/* A save in two halves. The snapshot (fopen + fwrite) runs with the audio
 * backstop parked and the mix lock held, so no mixer touches game memory while
 * it is captured. Once the last fwrite returns every byte is in the kernel or
 * the FILE buffer, so flush + fsync + rename -- which wait on the SD card for
 * up to a second -- run after the lock is released and audio keeps playing. */
typedef struct {
    FILE *f;
    char path[512], tmp[520];
    int ok;
    size_t bytes, skipped;
    long written;             /* file bytes (a sparse state is smaller than bytes) */
    char why[96];             /* the first failure: stage and strerror (kit 88210e7, GoldenEye e0a9c32) */
    double t0, t_write;       /* ss_ms() at the start, after the fwrites */
} SaveJob;

static int save_write(FILE *f, size_t region, size_t hole_off, size_t hole_size) {
    SaveHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = SS_MAGIC;
    h.region_size = (uint32_t)region;
    h.hole_off = (uint32_t)hole_off;
    h.hole_size = (uint32_t)hole_size;
    h.data_start = (uint32_t)(uintptr_t)__data_start;
    build_id(h.build);
    const int sparse = ss_sparse();
    if (sparse) h.magic = SS_MAGIC_SPARSE;
    if (fwrite(&h, sizeof(h), 1, f) != 1) return 0;
    SsRange r[2];
    ss_ranges(r, region, hole_off, hole_size);
    for (int i = 0; i < 2; i++) {
        if (!sparse) {
            if (r[i].size && fwrite(r[i].base, 1, r[i].size, f) != r[i].size) return 0;
            continue;
        }
        const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
        unsigned char bm[2048];
        if (nb > sizeof(bm)) return 0;
        memset(bm, 0, nb);
        for (size_t p = 0; p < np; p++)
            if (!ss_page_zero(r[i].base + p * SS_PAGE, ss_page_len(&r[i], p))) bm[p >> 3] |= (unsigned char) (1u << (p & 7));
        if (nb && fwrite(bm, 1, nb, f) != nb) return 0;
        for (size_t p = 0; p < np;) { /* runs of present pages, one fwrite each */
            if (!(bm[p >> 3] & (1u << (p & 7)))) { p++; continue; }
            size_t q = p, len = 0;
            while (q < np && (bm[q >> 3] & (1u << (q & 7)))) len += ss_page_len(&r[i], q++);
            if (fwrite(r[i].base + p * SS_PAGE, 1, len, f) != len) return 0;
            p = q;
        }
    }
    return 1;
}

static void save_begin(int slot, SaveJob *j) {
    size_t region, hole_off, hole_size;
    layout(&region, &hole_off, &hole_size);
    char name[32]; slot_name(slot, name, sizeof(name));
    memset(j, 0, sizeof(*j));
    snprintf(j->path, sizeof(j->path), "%s", port_save_path(name));
    snprintf(j->tmp, sizeof(j->tmp), "%s.tmp", j->path);
    j->bytes = region - hole_size;
    j->skipped = hole_size;
    j->t0 = ss_ms();
    {
        char mi[96];
        ss_meminfo(mi, sizeof(mi));
        fprintf(stderr, "savestate: slot %d saving: %s\n", slot, mi);
    }
    /* Beside the old state, then rename. The old state is never touched until
     * the new one is complete: a power cut, a full card or an I/O error mid-write
     * all leave it as it was. */
    errno = 0;
    j->f = fopen(j->tmp, "wb");
    if (!j->f) snprintf(j->why, sizeof(j->why), "open: %s", strerror(errno));
    else if (!save_write(j->f, region, hole_off, hole_size)) snprintf(j->why, sizeof(j->why), "write: %s", strerror(errno));
    else if (fflush(j->f) != 0) snprintf(j->why, sizeof(j->why), "flush: %s", strerror(errno));
    else {
        j->ok = 1;
        j->written = ftell(j->f);
        j->t_write = ss_ms();
        return;
    }
    if (j->f) { fclose(j->f); j->f = NULL; }
    remove(j->tmp);
}

static int save_finish(int slot, SaveJob *j) {
    int ok = j->ok && j->f != NULL;
    double t_sync = 0, t_close = 0;
    if (!j->ok) j->t_write = j->t0; /* the failed-write path never stamped it */
    if (j->f) {
        if (fflush(j->f) != 0 || fsync(fileno(j->f)) != 0) { if (ok) snprintf(j->why, sizeof(j->why), "fsync: %s", strerror(errno)); ok = 0; }
        t_sync = ss_ms();
        if (fclose(j->f) != 0) { if (ok) snprintf(j->why, sizeof(j->why), "close: %s", strerror(errno)); ok = 0; }
        t_close = ss_ms();
    } else {
        t_sync = t_close = j->t_write;
    }
    if (ok && rename(j->tmp, j->path) != 0) { snprintf(j->why, sizeof(j->why), "rename: %s", strerror(errno)); ok = 0; }
    if (ok) { /* the rename itself onto the card: a power cut right after the save keeps it (GoldenEye lost a
               * Cradle save that way, 2026-09-27: the renamed entry never reached the card).
               * The file first (the SSB64 port cd5128c, the DKR port 9c91d40): the card's vfat (Linux 4.14
               * vfat_rename) moves the new file into the old one's directory slot (a new name: a slot made
               * with cluster 0) and only marks the inode dirty, so the slot's start cluster and size reach
               * the card at its writeback, ~30 s later, while the folder's fsync already writes the FAT with
               * the old file's clusters freed. A power cut in between left the name on freed clusters. */
        const int fd = open(j->path, O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
        const int d = open(port_save_dir(), O_RDONLY);
        if (d >= 0) { fsync(d); close(d); }
    }
    const double t_end = ss_ms();
    if (!ok) { remove(j->tmp); fprintf(stderr, "savestate: slot %d failed at %s\n", slot, j->why[0] ? j->why : "(no stage)"); }
    fprintf(stderr, "savestate: slot %d %s (%zu bytes, %zu skipped; file %ld) ms: write %.0f fsync %.0f close %.0f rename %.0f\n", slot,
            ok ? "saved" : "FAILED, previous state kept", j->bytes, j->skipped, j->written,
            j->t_write - j->t0, t_sync - j->t_write, t_close - t_sync, t_end - t_close);
    return ok;
}

static int do_load_file_locked(const char *path, int slot);
static int do_save(int slot) {
    SaveJob j;
    SS_SLEEP_JOIN(); /* a sleep save still syncing (it may be this slot's .tmp) */
    port_audio_backstop_enable(0); port_audio_mix_lock();
    save_begin(slot, &j);
    port_audio_mix_unlock(); port_audio_backstop_enable(1);
    return save_finish(slot, &j);
}
/* statefix A (the port kit k1.8): the game's own save is the player's progress, not part of the moment a
 * state captured. After a load that happened, ultra_shim.c puts eeprom.bin back over the state's copy of
 * it (the emulated EEPROM and gSaveData), so a record or a cup won after the state was made is neither
 * rolled back in the game nor written back over at the next race's save. */
extern void hh_game_save_reapply(void);
/* statefix B (k1.8): a load is a gap in the sound: the card fades out and holds before it, and after it
 * the old timeline's queue is dropped and the restored game's sound fades in once its cushion is back
 * (audio_sdl.c, hh_declick.h). */
extern void port_audio_out_gap(int begin);
static int do_load_file(const char *path, int slot) {
    SS_SLEEP_JOIN(); /* a sleep save still syncing: its state is the one on the card */
    port_audio_backstop_enable(0);
    port_audio_out_gap(1);
    port_audio_mix_lock();
    const int ok = do_load_file_locked(path, slot);
    port_audio_mix_unlock(); port_audio_backstop_enable(1);
    port_audio_out_gap(0);
    return ok;
}

/* 1 = loaded. 0 = nothing changed (no file, another build, a read error before any game memory was
 * touched) or a short read (game memory then partly restored; only a card failing mid-read gets
 * there). The menu's SAVE/LOAD caption is chosen from this (B7, kit k1). */
static int do_load_file_locked(const char *path, int slot) {
    size_t region, hole_off, hole_size;
    layout(&region, &hole_off, &hole_size);
    const double t0 = ss_ms();
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "savestate: slot %d empty (%s)\n", slot, path); return 0; }

    const int st = state_check(f); /* before a single byte of game memory is touched */
    if (st != STATE_OK) {
        fprintf(stderr, "savestate: slot %d rejected (%s)\n", slot, st == STATE_MISMATCH ? "another build, or truncated" : "read error");
        fclose(f);
        return 0;
    }

#if defined(__GLIBC__) && !(defined(HANDHELD) && !defined(MIYOO)) /* glibc builds; the handheld's musl has none */
    /* glibc's stdin/stdout/stderr are plain variables, which this non-PIE binary gets as copies in
     * its .bss (R_ARM_COPY), inside the state. They point into libc, which lands somewhere else in
     * every process: a state from an earlier run would hand stdio a dead FILE. Keep the live ones. */
    FILE *const keep_in = stdin, *const keep_out = stdout, *const keep_err = stderr;
#endif
    void *saved = NULL;
    size_t keep = 0;
    if (hole_size == 0) {
        /* No hole: preserve the live section around the restore instead. */
        keep = (size_t)(__stop_dontsave - __start_dontsave);
        saved = malloc(keep);
        if (!saved) { fclose(f); return 0; }
        memcpy(saved, __start_dontsave, keep);
    }

    const size_t a = hole_off, b_off = hole_off + hole_size, b = region - b_off;
    size_t got = 0;
    {
        SaveHeader h; /* state_check left f just past the header: which kind of file */
        const long pos = ftell(f);
        int sparse = 0;
        if (pos >= (long) sizeof(h) && fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1)
            sparse = h.magic == SS_MAGIC_SPARSE;
        fseek(f, pos, SEEK_SET);
        SsRange r[2];
        ss_ranges(r, region, hole_off, hole_size);
        for (int i = 0; i < 2; i++) {
            if (!sparse) { if (r[i].size) got += fread(r[i].base, 1, r[i].size, f); continue; }
            const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
            unsigned char bm[2048];
            if (nb > sizeof(bm) || (nb && fread(bm, 1, nb, f) != nb)) break;
            for (size_t p = 0; p < np;) { /* runs: one fread per present run; a zero run is zeroed page by page */
                const int present = (bm[p >> 3] >> (p & 7)) & 1;
                size_t q = p, len = 0;
                while (q < np && (int) ((bm[q >> 3] >> (q & 7)) & 1) == present) len += ss_page_len(&r[i], q++);
                if (present) got += fread(r[i].base + p * SS_PAGE, 1, len, f);
                else {
                    /* Only a page that isn't zero already is written (the DKR port 2da5231). A page this
                     * process never touched (on a resume at boot: most of the state's zero pages) reads as
                     * the kernel's shared zero page and stays that: a memset made each one 4 KB of process
                     * memory that the 64 MB device can only free by swapping it out to the card (the handheld OS
                     * has a swap partition), and the next save's page scan then reads it back from there.
                     * The same bytes either way. */
                    for (size_t k = p; k < q; k++) {
                        char *pg = r[i].base + k * SS_PAGE;
                        const size_t pl = ss_page_len(&r[i], k);
                        if (!ss_page_zero(pg, pl)) memset(pg, 0, pl);
                    }
                    got += len;
                }
                p = q;
            }
        }
    }
    fclose(f);

    if (saved) { memcpy(__start_dontsave, saved, keep); free(saved); }
#if defined(__GLIBC__) && !(defined(HANDHELD) && !defined(MIYOO)) /* glibc builds; the handheld's musl has none */
    stdin = keep_in; stdout = keep_out; stderr = keep_err;
#endif

    if (got != a + b) { fprintf(stderr, "savestate: slot %d short read\n", slot); return 0; }

    const double t_read = ss_ms(); /* a local: the stack is not in the state */
    hh_game_save_reapply(); /* statefix A: eeprom.bin over the state's copy of it */
    gfx_savestate_fixup();
    fprintf(stderr, "savestate: slot %d loaded (read %.0f ms, fixup %.0f ms)\n", slot, t_read - t0, ss_ms() - t_read);
    return 1;
}

static int do_load(int slot) {
    char name[32]; slot_name(slot, name, sizeof(name));
    return do_load_file(port_save_path(name), slot);
}

/* The power-off: the game quits (the OS's SIGTERM). */
static void power_off_now(void) {
    port_request_exit();
}

/* Power-off while the game loop runs: the auto save, offered at the next start (RESUME GAME /
 * NEW GAME). Before the resume has loaded there is nothing new to save: keep the auto save. */
static void resume_quicksave(void) {
    if (sBootPath[0]) {
        fprintf(stderr, "savestate: power off before the resume loaded; auto save kept\n");
    } else {
        handheld_menu_notify("SAVING");
        (void) do_save(QUICKSAVE_SLOT);
    }
    power_off_now(); /* a failed save must still finish the power-off */
}

int savestate_power_ready(void) { return sLoopRunning; }

#if defined(HANDHELD) && defined(MIYOO)
/* The sleep save (miyoo_sleepsave.h): the auto save RESUME GAME offers, written as the Mini goes to sleep. The
 * game waits only for the snapshot into the page cache (save_begin, as any save); the fsync, the rename and the
 * folder's fsync (save_finish) run on the kit's helper thread while the game goes on. A SIGSTOP or SIGKILL in
 * either half leaves the previous state as it was (the .tmp is renamed only once whole and synced). */
static SaveJob sSleepJob SAVESTATE_EXCLUDE;
static int sSleepKind SAVESTATE_EXCLUDE;
static void sleep_finish(void *arg) {
    (void) arg;
    miyoo_sleepsave_done(sSleepKind, save_finish(QUICKSAVE_SLOT, &sSleepJob));
}
static void sleep_save(int kind) {
    miyoo_sleepsave_join();
    port_audio_backstop_enable(0); port_audio_mix_lock();
    save_begin(QUICKSAVE_SLOT, &sSleepJob);
    port_audio_mix_unlock(); port_audio_backstop_enable(1);
    sSleepKind = kind;
    miyoo_sleepsave_async(sleep_finish, NULL);
}
#endif

/* The OS caption for a menu SAVE or LOAD, composed from what happened rather than what was asked
 * for (B7, kit k1 ab5dc5b): the menu used to show SAVED SLOT n as it closed, before the save had
 * run, and even when it then failed (a full card, an I/O error); LOADED SLOT n likewise for a state
 * from another build. */
static void notify_result(int save, int slot, int ok) {
    char msg[32];
    if (!ok)                         snprintf(msg, sizeof(msg), "%s", save ? "SAVE FAILED" : "LOAD FAILED");
    else if (save)                   snprintf(msg, sizeof(msg), "SAVED SLOT %d", slot);
    else if (slot == QUICKSAVE_SLOT) snprintf(msg, sizeof(msg), "LOADED AUTO SAVE");
    else                             snprintf(msg, sizeof(msg), "LOADED SLOT %d", slot);
    handheld_menu_notify(msg);
    (void) msg; /* handheld_menu_notify is a no-op macro off handheld */
}

/* Applied at the iteration boundary (never mid-display-list). */
void savestate_check(void) {
    sLoopRunning = 1;
    if (sBootPath[0] && ++sBootFrames >= BOOT_LOAD_FRAME) {
        do_load_file(sBootPath, QUICKSAVE_SLOT);
        sBootPath[0] = 0;
    }
    const int r = sRequest;
    if (r != 0) {
        sRequest = 0;
        if (r > 0) notify_result(1, r, do_save(r));
        else       notify_result(0, -r, do_load(-r));
    }
#if defined(HANDHELD) && defined(MIYOO)
    /* A POWER press, or no button for a while: the auto save now, before Onion's sleep freezes the game (and
     * maybe powers off). Not before a resume has loaded, not with a quit's save coming, never two at once. */
    miyoo_sleepsave_start("MK64_SLEEPSAVE");
    if (!sBootPath[0] && !sResumeSave && !sExitSave && !miyoo_sleepsave_busy()) {
        const int k = miyoo_sleepsave_due();
        if (k) sleep_save(k);
    }
#endif
    if (sResumeSave) {
        sResumeSave = 0;
        resume_quicksave(); /* does not return */
    }
    if (sExitSave) {
        /* EXIT GAME keeps your place like a power-off does (gnuboy handheld
         * saves its quicksave on exit), so RESUME GAME is always the last
         * session -- never an older one that would also roll back the EEPROM
         * copy held in memory. SAVING while it writes (kit k1.7, X50): the screen
         * otherwise holds the last frame with the music off, which reads as a freeze. */
        sExitSave = 0;
        if (!sBootPath[0]) {
            handheld_menu_notify("SAVING");
            if (!do_save(QUICKSAVE_SLOT)) handheld_menu_notify_wait("SAVE FAILED"); /* shown before the quit */
        }
        port_request_exit();
    }
}

/* After the game loop. */
void savestate_shutdown(void) {
    sLoopRunning = 0;
    SS_SLEEP_JOIN();
}

/* A save interrupted by a power cut leaves <slot>.tmp behind; nothing reads it. */
static void clean_state_tmp(void) {
    DIR *d = opendir(port_save_dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "state_", 6) == 0 && n > 8 && strcmp(e->d_name + n - 8, ".sav.tmp") == 0)
            remove(port_save_path(e->d_name));
    }
    closedir(d);
}

/* At launch:
 * - Started with --loadStateFile <path>: load it straight away, no prompt.
 * - Started with an auto save on the card (EXIT GAME, or a power-off):
 *   ask RESUME GAME / NEW GAME. New deletes it.
 * Resume loads it a few frames in (savestate_check). An auto save from another
 * build is removed; one that can't be read is left alone. The desktop build has
 * no prompt and never deletes: only an explicit --loadStateFile. */
void savestate_boot_prompt(void) {
    clean_state_tmp();
#if !defined(HANDHELD)
    if (sBootPath[0] && state_file_check(sBootPath) != STATE_OK) {
        fprintf(stderr, "savestate: %s is not a state for this build; not loaded\n", sBootPath);
        sBootPath[0] = 0;
    }
#else
    char name[32], quick[512];
    slot_name(QUICKSAVE_SLOT, name, sizeof(name));
    snprintf(quick, sizeof(quick), "%s", port_save_path(name));
    const int relaunch = sBootPath[0] != 0;
    if (!relaunch) {
        if (!savestate_slot_exists(QUICKSAVE_SLOT)) return;
        snprintf(sBootPath, sizeof(sBootPath), "%s", quick);
    }
    const int is_quick = strcmp(sBootPath, quick) == 0;
    const int st = state_file_check(sBootPath);
    if (st != STATE_OK) {
        if (st == STATE_MISMATCH && is_quick) {
            fprintf(stderr, "savestate: auto save is from another build; removed\n");
            remove(sBootPath);
        } else {
            fprintf(stderr, "savestate: %s can't be read; not loaded\n", sBootPath);
        }
        sBootPath[0] = 0;
        return;
    }
    if (relaunch) {
        fprintf(stderr, "savestate: resume, loading from %s\n", sBootPath);
        return;
    }
    const char *ans = getenv("MK64_RESUME_ANSWER"); /* tests: resume | new, no UI */
    const int choice = (ans && (ans[0] == 'r' || ans[0] == 'R')) ? 1
                     : (ans && (ans[0] == 'n' || ans[0] == 'N')) ? 0
                     : handheld_menu_resume_prompt();
    if (choice < 0) {
        /* Power-off at the prompt: keep the auto save, quit. */
        fprintf(stderr, "savestate: power off at the prompt; auto save kept\n");
        power_off_now();
    } else if (choice) {
        fprintf(stderr, "savestate: resuming from %s\n", sBootPath);
    } else {
        fprintf(stderr, "savestate: new game, auto save removed\n");
        if (is_quick) remove(sBootPath);
        sBootPath[0] = 0;
    }
#endif
}

void savestate_request_save(int slot) { sRequest = slot; }
void savestate_request_load(int slot) { sRequest = -slot; }
