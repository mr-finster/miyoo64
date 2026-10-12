/**
 * Save states -- see savestate.h. Snapshots the game's writable memory in one of
 * two layouts, chosen in the port's hh_game.h:
 *  - hole (the default; MK64, Star Fox, SM64): everything from __data_start to the
 *    end of the game's writable data, minus the "dontsave" section, which a load
 *    keeps live;
 *  - include list (HH_SS_INCLUDE_LIST; DKR): exactly the .game_data and .game_bss
 *    ranges the port's linker script collects between fixed symbols.
 *
 * kit: from mario-kart-64 bbe66b9 (base-psp/src/port/sdl/savestate.c), with
 * DKR's include list and backstop swap, SM64's asset-state bound and Star Fox's
 * stdio guard folded in; save and load results reach the player (B7).
 */
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "hh_game.h"
#include "handheld_menu.h" /* the resume prompt, notify, power-off helpers (stubs off the handheld) */

#ifndef HH_BOOT_LOAD_FRAME
#define HH_BOOT_LOAD_FRAME 3 /* game init has run; the title screen has drawn */
#endif

extern const char *port_save_path(const char *name); /* $<GAME>_DATA/<name> */
extern const char *port_save_dir(void);
extern void port_request_exit(void);
extern void gfx_savestate_fixup(void);               /* flush tex cache + GE state */
/* The audio backstop thread mixes whenever the ring runs low -- which it does
 * while a state is read from the SD card. Mixing on a half-restored heap crashed
 * MK64's first on-device load (round 6). Both the snapshot and the restore run
 * with the backstop parked and the mix lock held, so no mixer touches game memory
 * while it is in flux. A port whose backstop can be off (DKR mixes on the game
 * thread only) defines port_audio_backstop_swap, and the backstop comes back in
 * the state it was in; without it the backstop comes back on, as on MK64. */
extern void port_audio_backstop_enable(int on);
extern int  port_audio_backstop_swap(int on) __attribute__((weak));
extern void port_audio_mix_lock(void);
extern void port_audio_mix_unlock(void);

static int backstop_park(void) {
    if (port_audio_backstop_swap) return port_audio_backstop_swap(0);
    port_audio_backstop_enable(0);
    return 1;
}

/* A state is only valid for the exact executable that wrote it: .data/.bss sit
 * at fixed addresses and .data holds function pointers. port_build_stamp is
 * regenerated whenever the link's inputs change, so any rebuild -- including one
 * that never recompiles this file -- makes older states unloadable instead of a
 * crash; the header also records where each range sat, in case a layout moves
 * without a new stamp. */
extern const uint32_t port_build_stamp; /* build/<plat>/build_stamp.c */
static void build_id(char out[32]) { snprintf(out, 32, "%s build %08X", HH_GAME_ID, (unsigned) port_build_stamp); }

/* The ranges a state holds, in file order. */
enum { SS_MAX_RANGES = 3 };
typedef struct { char *base; size_t size; } SsRange;

typedef struct {
    uint32_t magic;
    uint32_t nranges;
    uint32_t start[SS_MAX_RANGES]; /* where each range sat in the build that wrote it */
    uint32_t size[SS_MAX_RANGES];
    char     build[32];
} SaveHeader;

#if defined(HH_SS_INCLUDE_LIST)
/* Include list: the port's linker script (DKR: port/gamestate.ld) gathers every
 * decomp object's .data and .bss, plus the few port files that hold game state,
 * between these symbols. SDL, libc, the renderer, the audio ring and this file lie
 * outside both ranges by construction, so a load can't overwrite a live host
 * pointer, and nothing needs the "dontsave" section. */
extern char __gamedata_start[], __gamedata_end[];
extern char __gamebss_start[], __gamebss_end[];
static int layout(SsRange r[SS_MAX_RANGES]) {
    r[0].base = __gamedata_start; r[0].size = (size_t) (__gamedata_end - __gamedata_start);
    r[1].base = __gamebss_start;  r[1].size = (size_t) (__gamebss_end - __gamebss_start);
    return 2;
}
static size_t layout_skipped(void) { return 0; }
#else
/* Hole: linker-provided bounds. __data_start/_end wrap all writable data; ld
 * auto-creates __start_/__stop_ for the named "dontsave" section. */
extern char __data_start[];
extern char _end[];
extern char __start_dontsave[];
extern char __stop_dontsave[];
/* The ROM-derived region some ports link after .bss, and its optional head that
 * the game does change (SM64: the levels' paintings and moving textures). */
extern char __assets_start[] __attribute__((weak)), __assets_end[] __attribute__((weak));
extern char __assets_state_end[] __attribute__((weak));

/* The snapshot runs from __data_start to the end of the game's writable data.
 * That end is __assets_start, not _end, when an asset region follows .bss: it is
 * ROM data rebuilt at boot from the player's cache and never written by the game,
 * so it is not state (MK64: ~17 MB down to ~4.5 MB, which matters most for the
 * power-off auto save, where the OS allows only a short window before it ends
 * the game). A port whose game does write the head of that region marks its end with
 * __assets_state_end (SM64: ~5.8 MB down to ~3.2 MB). A build with the assets
 * linked in has an empty region and keeps _end. */
static const char *state_end(void) {
    const char *end = _end;
    if (__assets_start && __assets_end && &__assets_end[0] == &_end[0] &&
        &__assets_start[0] > &__data_start[0] && &__assets_start[0] < &_end[0]) {
        end = __assets_start;
        if (__assets_state_end && &__assets_state_end[0] >= &__assets_start[0] &&
            &__assets_state_end[0] <= &__assets_end[0])
            end = __assets_state_end;
    }
    return end;
}
/* Minus the "dontsave" hole (texture arena, framebuffers, SDL handles, audio
 * ring), which is neither written nor read so nothing live is overwritten. */
static int hole(const char *end, size_t *off, size_t *size) {
    if (&__start_dontsave[0] >= &__data_start[0] && &__stop_dontsave[0] <= end && &__stop_dontsave[0] > &__start_dontsave[0]) {
        *off = (size_t) (__start_dontsave - __data_start);
        *size = (size_t) (__stop_dontsave - __start_dontsave);
        return 1;
    }
    *off = 0; *size = 0;
    return 0;
}
/* A range of game memory outside the binary, from the port (e.g. GoldenEye: the N64 DRAM, mapped at a
 * fixed address below the executable). */
extern int hh_game_extra_range(char **base, size_t *size) __attribute__((weak));
static int extra(SsRange r[SS_MAX_RANGES], int n) {
    char *b; size_t sz;
    if (hh_game_extra_range && n < SS_MAX_RANGES && hh_game_extra_range(&b, &sz) && sz) {
        r[n].base = b; r[n].size = sz;
        n++;
    }
    return n;
}
static int layout(SsRange r[SS_MAX_RANGES]) {
    const char *end = state_end();
    const size_t region = (size_t) (end - __data_start);
    size_t off, size;
    if (!hole(end, &off, &size)) {
        r[0].base = __data_start; r[0].size = region;
        return extra(r, 1);
    }
    r[0].base = __data_start;              r[0].size = off;
    r[1].base = __data_start + off + size; r[1].size = region - off - size;
    return extra(r, 2);
}
static size_t layout_skipped(void) {
    size_t off, size;
    hole(state_end(), &off, &size);
    return size;
}
#endif

static size_t layout_bytes(const SsRange *r, int n) {
    size_t t = 0;
    for (int i = 0; i < n; i++) t += r[i].size;
    return t;
}

/* Sparse states (HH_SS_SPARSE; kit k1.5, from GoldenEye e037c3b): each range is written as a bitmap of
 * its 4 KB pages, then only the pages that aren't all zero. GoldenEye's 8.6 MB of state is 70-89%
 * zero pages (the N64 DRAM's free heap, the unused thread stacks), and the SD card's write + fsync
 * time is what the player waits for -- a Train save hung there on the device. A page missing from
 * the file is zeroed on load. The header's nranges carries SS_SPARSE_FLAG. */
#define SS_PAGE 4096u
#define SS_SPARSE_FLAG 0x100u
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
#if defined(HH_SS_SPARSE)
static const int ss_sparse = 1;
#else
static const int ss_sparse = 0;
#endif

/* Excluded from the snapshot so a load never clobbers the request itself. */
static int sRequest SAVESTATE_EXCLUDE;      /* >0 save slot, <0 load slot, 0 idle */
static char sProgName[512] SAVESTATE_EXCLUDE;
static char sBootPath[512] SAVESTATE_EXCLUDE; /* --loadStateFile <path>, or the auto save to resume */
static int  sBootFrames SAVESTATE_EXCLUDE;
static int  sPowerSave SAVESTATE_EXCLUDE;   /* power-off: the auto save, then quit */
static int  sExitSave SAVESTATE_EXCLUDE;      /* EXIT GAME: quicksave, then quit */
static volatile int sLoopRunning SAVESTATE_EXCLUDE;

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
void savestate_request_power_save(void) { sPowerSave = 1; }
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

/* One check for the boot prompt, the menu and the loader: magic, layout, build
 * id and exact size (header + the ranges; shorter = cut off mid-write). Leaves f
 * just past the header. A read error is not a mismatch -- only a mismatch may
 * ever delete a file. */
enum { STATE_OK = 1, STATE_MISMATCH = 0, STATE_UNREADABLE = -1 };
static int state_check(FILE *f) {
    SsRange r[SS_MAX_RANGES];
    const int n = layout(r);
    SaveHeader h;
    char id[32];
    build_id(id);
    if (fread(&h, sizeof(h), 1, f) != 1) return ferror(f) ? STATE_UNREADABLE : STATE_MISMATCH;
    const int sparse = (h.nranges & SS_SPARSE_FLAG) != 0;
    if (h.magic != HH_SS_MAGIC || (h.nranges & ~SS_SPARSE_FLAG) != (uint32_t) n || strncmp(h.build, id, sizeof(h.build)) != 0)
        return STATE_MISMATCH;
    for (int i = 0; i < n; i++)
        if (h.start[i] != (uint32_t) (uintptr_t) r[i].base || h.size[i] != (uint32_t) r[i].size) return STATE_MISMATCH;
    long want = (long) (sizeof(SaveHeader) + layout_bytes(r, n));
    const long pos = ftell(f);
    if (sparse) { /* the bitmaps say how long the file must be */
        want = (long) sizeof(SaveHeader);
        long at = pos;
        for (int i = 0; i < n; i++) {
            const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
            unsigned char bm[2048];
            if (nb > sizeof(bm) || fseek(f, at, SEEK_SET) != 0 || fread(bm, 1, nb, f) != nb)
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
    char why[96];             /* the first failure: stage and strerror (GoldenEye 2026-09-27) */
    size_t bytes, skipped;
    long written;             /* file bytes (a sparse state is smaller than bytes) */
    double t0, t_write;       /* ss_ms() at the start, after the fwrites */
} SaveJob;

/* Save timing (GoldenEye, 2026-09-27: a Train save hung the device inside fsync/rename, the
 * tmp complete). Every save logs each stage's ms and the kernel's free/dirty memory, so a
 * slow or stuck card shows where. */
#include <time.h>
static double ss_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void ss_meminfo(char *out, size_t cap) { /* "free 12345k dirty 678k" (Linux), or "" */
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

static int save_write(FILE *f, const SsRange *r, int n) {
    SaveHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = HH_SS_MAGIC;
    h.nranges = (uint32_t) n;
    for (int i = 0; i < n; i++) {
        h.start[i] = (uint32_t) (uintptr_t) r[i].base;
        h.size[i] = (uint32_t) r[i].size;
    }
    build_id(h.build);
    if (ss_sparse) h.nranges |= SS_SPARSE_FLAG;
    if (fwrite(&h, sizeof(h), 1, f) != 1) return 0;
    for (int i = 0; i < n; i++) {
        if (!ss_sparse) {
            if (r[i].size && fwrite(r[i].base, 1, r[i].size, f) != r[i].size) return 0;
            continue;
        }
        const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
        unsigned char bm[2048];
        if (nb > sizeof(bm)) return 0;
        memset(bm, 0, nb);
        for (size_t p = 0; p < np; p++)
            if (!ss_page_zero(r[i].base + p * SS_PAGE, ss_page_len(&r[i], p))) bm[p >> 3] |= (unsigned char) (1u << (p & 7));
        if (fwrite(bm, 1, nb, f) != nb) return 0;
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

/* k1.8 proposal (paper-mario, 2026-10-09): the port may refuse a save it cannot make whole (PM64: the asset region's
 * changed pages that ride in the state did not all fit). 0 = refuse, with a reason for the log; the save then fails
 * before any file is touched, so the menu shows SAVE FAILED, the old state stays, and the power-off goes ahead without
 * a new quicksave. A port without the hook saves as before. */
extern int hh_game_state_ready(char *why, size_t n) __attribute__((weak));

static void save_begin(int slot, SaveJob *j) {
    SsRange r[SS_MAX_RANGES];
    const int n = layout(r);
    char name[32]; slot_name(slot, name, sizeof(name));
    memset(j, 0, sizeof(*j));
    snprintf(j->path, sizeof(j->path), "%s", port_save_path(name));
    snprintf(j->tmp, sizeof(j->tmp), "%s.tmp", j->path);
    j->bytes = layout_bytes(r, n);
    j->skipped = layout_skipped();
    j->t0 = ss_ms();
    j->t_write = j->t0; /* a save that fails before its fwrites logs 0 ms, not garbage */
    {
        char mi[96];
        ss_meminfo(mi, sizeof(mi));
        fprintf(stderr, "savestate: slot %d saving: %s\n", slot, mi);
        fflush(stderr);
    }
    if (hh_game_state_ready && !hh_game_state_ready(j->why, sizeof(j->why))) {
        if (!j->why[0]) snprintf(j->why, sizeof(j->why), "the game refused");
        return;
    }
    /* Beside the old state, then rename. The old state is never touched until
     * the new one is complete: a power cut, a full card or an I/O error mid-write
     * all leave it as it was. */
    errno = 0;
    j->f = fopen(j->tmp, "wb");
    if (!j->f) snprintf(j->why, sizeof(j->why), "open: %s", strerror(errno));
    else if (!save_write(j->f, r, n)) snprintf(j->why, sizeof(j->why), "write: %s", strerror(errno));
    else if (fflush(j->f) != 0) snprintf(j->why, sizeof(j->why), "flush: %s", strerror(errno));
    else {
        j->ok = 1;
        j->written = ftell(j->f);
        j->t_write = ss_ms();
        return;
    }
    j->t_write = ss_ms();
    if (j->f) { fclose(j->f); j->f = NULL; }
    remove(j->tmp);
}

static int save_finish(int slot, SaveJob *j) {
    int ok = j->ok && j->f != NULL;
    double t_sync = j->t_write, t_close = j->t_write;
    char mi[96];
    ss_meminfo(mi, sizeof(mi));
    fprintf(stderr, "savestate: slot %d written %ld bytes in %.0f ms, syncing: %s\n", slot, j->written,
            j->t_write - j->t0, mi);
    fflush(stderr);
    if (j->f) {
        if (fflush(j->f) != 0 || fsync(fileno(j->f)) != 0) { if (ok) snprintf(j->why, sizeof(j->why), "fsync: %s", strerror(errno)); ok = 0; }
        t_sync = ss_ms();
    }
    if (ok && rename(j->tmp, j->path) != 0) { snprintf(j->why, sizeof(j->why), "rename: %s", strerror(errno)); ok = 0; }
    if (j->f) {
        /* k1.7 proposal (super-smash-bros cd5128c; paper-mario's sweep): the file once more, still open, after the
         * rename. On the card's vfat (Linux 4.14 vfat_rename) a rename over an existing state moves the new file
         * into the OLD file's directory slot and only marks its inode dirty: the slot's new start cluster and size
         * reach the card at that inode's writeback (~30 s) or its own fsync, not with the folder's below, which
         * writes the removed .tmp entry and the FAT with the old state's clusters freed. A hard power cut in that
         * window left the state pointing at freed clusters: the state lost, the FAT inconsistent. The close
         * moves after the rename: its result never mattered after a successful fsync. ("close" in the log line
         * below is now the rename, this fsync and the close; "rename" the folder's fsync.) */
        if (ok) fsync(fileno(j->f));
        fclose(j->f);
        j->f = NULL;
        t_close = ss_ms();
    }
    if (ok) { /* the rename itself on the card: a power cut right after the save keeps it (a Cradle save
               * was lost that way, 2026-09-27: the renamed entry never reached the card) */
        int d = open(port_save_dir(), O_RDONLY);
        if (d >= 0) { fsync(d); close(d); }
    }
    const double t_end = ss_ms();
    if (!ok) { remove(j->tmp); fprintf(stderr, "savestate: slot %d failed at %s\n", slot, j->why); }
    fprintf(stderr, "savestate: slot %d %s (%zu bytes, %zu skipped; file %ld) ms: write %.0f fsync %.0f close %.0f rename %.0f\n", slot,
            ok ? "saved" : "FAILED, previous state kept", j->bytes, j->skipped, j->written,
            j->t_write - j->t0, t_sync - j->t_write, t_close - t_sync, t_end - t_close);
    return ok;
}

static int do_load_file_locked(const char *path, int slot);
static int do_save(int slot) {
    SaveJob j;
    const int backstop = backstop_park();
    port_audio_mix_lock();
    save_begin(slot, &j);
    port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    return save_finish(slot, &j);
}
static int do_load_file(const char *path, int slot) {
    const int backstop = backstop_park();
    port_audio_mix_lock();
    const int ok = do_load_file_locked(path, slot);
    port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    return ok;
}

/* 1 = loaded. 0 = nothing changed (no file, another build, a read error before
 * any game memory was touched) or a short read (game memory is then partly
 * restored; only a card failing mid-read gets there). */
static int do_load_file_locked(const char *path, int slot) {
    SsRange r[SS_MAX_RANGES];
    const int n = layout(r);
    const double t0 = ss_ms();
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "savestate: slot %d empty (%s)\n", slot, path); return 0; }

    const int st = state_check(f); /* before a single byte of game memory is touched */
    if (st != STATE_OK) {
        fprintf(stderr, "savestate: slot %d rejected (%s)\n", slot, st == STATE_MISMATCH ? "another build, or truncated" : "read error");
        fclose(f);
        return 0;
    }

#if !defined(HH_SS_INCLUDE_LIST)
    /* No hole: preserve the live section around the restore instead. */
    void *saved = NULL;
    size_t keep = 0;
    if (n == 1) {
        keep = (size_t) (__stop_dontsave - __start_dontsave);
        saved = malloc(keep ? keep : 1);
        if (!saved) { fclose(f); return 0; }
        if (keep) memcpy(saved, __start_dontsave, keep);
    }
    /* A glibc desktop build copies stdin/stdout/stderr into the executable's .bss
     * (R_386_COPY), inside the region: keep this process's handles, not the
     * writer's (Star Fox 64). The handheld build's copies sit below __data_start;
     * miyoo.sh gate 2 proves it. */
#ifdef __GLIBC__
    FILE *const live_in = stdin, *const live_out = stdout, *const live_err = stderr;
#endif
#endif

    size_t got = 0;
    {
        SaveHeader h; /* state_check left f just past the header: read the sparse flag again */
        const long pos = ftell(f);
        int sparse = 0;
        if (pos >= (long) sizeof(h) && fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1)
            sparse = (h.nranges & SS_SPARSE_FLAG) != 0;
        fseek(f, pos, SEEK_SET);
        for (int i = 0; i < n; i++) {
            if (!sparse) { if (r[i].size) got += fread(r[i].base, 1, r[i].size, f); continue; }
            const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
            unsigned char bm[2048];
            if (nb > sizeof(bm) || fread(bm, 1, nb, f) != nb) break;
            for (size_t p = 0; p < np;) { /* runs: one fread per present run; a zero run is zeroed page by page */
                const int present = (bm[p >> 3] >> (p & 7)) & 1;
                size_t q = p, len = 0;
                while (q < np && (int) ((bm[q >> 3] >> (q & 7)) & 1) == present) len += ss_page_len(&r[i], q++);
                if (present) got += fread(r[i].base + p * SS_PAGE, 1, len, f);
                else {
                    /* k1.7 (DKR 2da5231): only a page that isn't zero already is written. A page this
                     * process never touched (a free heap tail, unused staging pages, on a resume at boot)
                     * reads as the kernel's shared zero page and stays that: a memset made each one 4 KB
                     * of process memory (DKR: +8.6 MB resident -> +0.4 MB), which the 64 MB device can
                     * only free by swapping it out to the card, and the next save's page scan then reads
                     * it back from there. The same bytes either way. */
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

#if !defined(HH_SS_INCLUDE_LIST)
#ifdef __GLIBC__
    stdin = live_in; stdout = live_out; stderr = live_err;
#endif
    if (saved) { if (keep) memcpy(__start_dontsave, saved, keep); free(saved); }
#endif

    if (got != layout_bytes(r, n)) {
        fprintf(stderr, "savestate: slot %d short read -- game memory is now inconsistent\n", slot);
        return 0;
    }

    const double t_read = ss_ms(); /* a local: the stack is not in the state */
    gfx_savestate_fixup();
    fprintf(stderr, "savestate: slot %d loaded (read %.0f ms, fixup %.0f ms)\n", slot, t_read - t0, ss_ms() - t_read);
    return 1;
}

static int do_load(int slot) {
    char name[32]; slot_name(slot, name, sizeof(name));
    return do_load_file(port_save_path(name), slot);
}

/* A power-off the game owns: quit (the OS powers off after its SIGTERM). */
static void power_off_now(void) {
    port_request_exit();
}

/* A power-off request while the game loop runs. Before the resume has loaded there is
 * nothing new to save: keep the auto save as it is. */
static void power_save(void) {
    if (sBootPath[0]) {
        fprintf(stderr, "savestate: power off before the resume loaded; auto save kept\n");
    } else {
        handheld_menu_notify("SAVING");
        (void) do_save(QUICKSAVE_SLOT);
    }
    power_off_now(); /* a failed save must still finish the power-off */
}

int savestate_power_ready(void) { return sLoopRunning; }

/* The OS notification for a menu SAVE or LOAD, composed from what happened
 * rather than what was asked for (B7: MK64 showed SAVED SLOT n before the save
 * had run, and even when it failed). */
static void notify_result(int save, int slot, int ok) {
    char msg[32];
    if (!ok)                         snprintf(msg, sizeof(msg), "%s", save ? "SAVE FAILED" : "LOAD FAILED");
    else if (save)                   snprintf(msg, sizeof(msg), "SAVED SLOT %d", slot);
    else if (slot == QUICKSAVE_SLOT) snprintf(msg, sizeof(msg), "LOADED AUTO SAVE");
    else                             snprintf(msg, sizeof(msg), "LOADED SLOT %d", slot);
    handheld_menu_notify(msg);
    (void) msg; /* handheld_menu_notify is a no-op macro off the handheld */
}

/* Applied at the iteration boundary (never mid-display-list). */
void savestate_check(void) {
    sLoopRunning = 1;
    if (sBootPath[0] && ++sBootFrames >= HH_BOOT_LOAD_FRAME) {
        do_load_file(sBootPath, QUICKSAVE_SLOT);
        sBootPath[0] = 0;
    }
    const int r = sRequest;
    if (r != 0) {
        sRequest = 0;
        if (r > 0) notify_result(1, r, do_save(r));
        else       notify_result(0, -r, do_load(-r));
    }
    if (sPowerSave) {
        sPowerSave = 0;
        power_save(); /* does not return */
    }
    if (sExitSave) {
        /* EXIT GAME keeps your place like a power-off does (gnuboy
         * saves its quicksave on exit), so RESUME GAME is always the last
         * session -- never an older one that would also roll back the EEPROM
         * copy held in memory. A failed save is shown before the quit.
         * k1.7 (OoT's proposal): SAVING while it writes (the screen otherwise holds the last frame
         * with the music off for up to a few seconds on a slow card, which reads as a freeze), and
         * the failure waits until its banner is up (exit() could beat the detached notify thread). */
        sExitSave = 0;
        if (!sBootPath[0]) {
            handheld_menu_notify("SAVING");
            if (!do_save(QUICKSAVE_SLOT)) handheld_menu_notify_wait("SAVE FAILED");
        }
        port_request_exit();
    }
}

/* After the game loop. */
void savestate_shutdown(void) {
    sLoopRunning = 0;
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

/* At launch, the handheld method (gnuboy's emu.c + launch_resume_menu_loop):
 * - A relaunch with --loadStateFile <state>:
 *   resume straight away, no prompt.
 * - Cold start with an auto save on the card (EXIT GAME, or a power-off that
 *   wasn't resumed): ask RESUME GAME / NEW GAME. New deletes it.
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
        fprintf(stderr, "savestate: relaunch, resuming from %s\n", sBootPath);
        return;
    }
    const char *ans = getenv(HH_ENV("RESUME_ANSWER")); /* tests: resume | new, no UI */
    const int choice = (ans && (ans[0] == 'r' || ans[0] == 'R')) ? 1
                     : (ans && (ans[0] == 'n' || ans[0] == 'N')) ? 0
                     : handheld_menu_resume_prompt();
    if (choice < 0) {
        /* A power-off at the prompt: keep the auto save so
         * the next start resumes it, power off. */
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
