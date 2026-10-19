/**
 * Save states -- see savestate.h. Snapshots the game's writable memory in one of
 * two layouts, chosen in the port's hh_game.h:
 *  - hole (the default; MK64, Star Fox, SM64): everything from __data_start to the
 *    end of the game's writable data, minus the "dontsave" section, which a load
 *    keeps live;
 *  - include list (HH_SS_INCLUDE_LIST; DKR): exactly the .game_data and .game_bss
 *    ranges the port's linker script collects between fixed symbols.
 *
 * handheld-kit: from mario-kart-64 bbe66b9 (base-psp/src/port/sdl/savestate.c), with
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
#include "handheld_menu.h" /* the resume prompt and notices (stubs off handheld) */
#if defined(HANDHELD) && defined(MIYOO)
#include "miyoo_sleepsave.h" /* the resume state at a POWER press and when idle */
#define SS_SLEEP_JOIN() miyoo_sleepsave_join()
#else
#define SS_SLEEP_JOIN() ((void) 0)
#endif

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
 * auto save when the OS shuts the game down). A port whose game does write the head of that region marks its end with
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
/* A state of an earlier build whose saved ranges this build keeps at the same addresses (the port's linker script
 * pins them: mm-miyoo's tools/mm_pin_ld.py) loads too; the port says which builds, and fixes up what the two builds'
 * images still differ in after the read. Weak: a port without them accepts only its own build. */
extern int hh_state_compat_build(const char *build) __attribute__((weak));
extern void hh_state_compat_fixup(const char *build) __attribute__((weak));
static char sStateBuild[32] SAVESTATE_EXCLUDE; /* the build id of the state state_check last passed */
static char sMismatch[160] SAVESTATE_EXCLUDE; /* why the last state_check said STATE_MISMATCH */
static int state_check(FILE *f) {
    SsRange r[SS_MAX_RANGES];
    const int n = layout(r);
    SaveHeader h;
    char id[32];
    build_id(id);
    snprintf(sMismatch, sizeof(sMismatch), "?");
    if (fread(&h, sizeof(h), 1, f) != 1) {
        snprintf(sMismatch, sizeof(sMismatch), "shorter than a header");
        return ferror(f) ? STATE_UNREADABLE : STATE_MISMATCH;
    }
    const int sparse = (h.nranges & SS_SPARSE_FLAG) != 0;
    if (h.magic != HH_SS_MAGIC || (h.nranges & ~SS_SPARSE_FLAG) != (uint32_t) n) {
        snprintf(sMismatch, sizeof(sMismatch), "not a state of this layout (magic %08X, %u ranges, want %d)",
                 (unsigned) h.magic, (unsigned) (h.nranges & ~SS_SPARSE_FLAG), n);
        return STATE_MISMATCH;
    }
    if (strncmp(h.build, id, sizeof(h.build)) != 0 &&
        !(hh_state_compat_build != NULL && hh_state_compat_build(h.build))) {
        snprintf(sMismatch, sizeof(sMismatch), "another build (\"%.31s\", this is \"%s\")", h.build, id);
        return STATE_MISMATCH;
    }
    memcpy(sStateBuild, h.build, sizeof(sStateBuild));
    for (int i = 0; i < n; i++)
        if (h.start[i] != (uint32_t) (uintptr_t) r[i].base || h.size[i] != (uint32_t) r[i].size) {
            snprintf(sMismatch, sizeof(sMismatch), "range %d at 0x%08X size 0x%X in the state, 0x%08X size 0x%X now", i,
                     (unsigned) h.start[i], (unsigned) h.size[i], (unsigned) (uintptr_t) r[i].base, (unsigned) r[i].size);
            return STATE_MISMATCH;
        }
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
    if (len != want) snprintf(sMismatch, sizeof(sMismatch), "%ld bytes, the header says %ld (cut off)", len, want);
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

/* ALPHA6: the state is captured into memory (the file's bytes) under the mix lock, and written to the card on the
 * helper thread: the game no longer waits for the card (a menu save froze the picture 0.4-1.5 s on the device). */
typedef struct { FILE *f; unsigned char *buf; size_t len, cap; } SsSink;
static size_t ss_put(SsSink *k, const void *p, size_t n) {
    if (k->f) return fwrite(p, 1, n, k->f);
    if (k->buf == NULL || k->len + n > k->cap) return 0;
    memcpy(k->buf + k->len, p, n);
    k->len += n;
    return n;
}
static unsigned char *sCapBuf SAVESTATE_EXCLUDE; /* the captured state (save_capture), until it is written */
static size_t sCapLen SAVESTATE_EXCLUDE;
static int save_write_sink(SsSink *f, const SsRange *r, int n) {
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
    if (ss_put(f, &h, sizeof(h)) != sizeof(h)) return 0;
    for (int i = 0; i < n; i++) {
        if (!ss_sparse) {
            if (r[i].size && ss_put(f, r[i].base, r[i].size) != r[i].size) return 0;
            continue;
        }
        const size_t np = ss_pages(r[i].size), nb = (np + 7) / 8;
        unsigned char bm[2048];
        if (nb > sizeof(bm)) return 0;
        memset(bm, 0, nb);
        for (size_t p = 0; p < np; p++)
            if (!ss_page_zero(r[i].base + p * SS_PAGE, ss_page_len(&r[i], p))) bm[p >> 3] |= (unsigned char) (1u << (p & 7));
        if (ss_put(f, bm, nb) != nb) return 0;
        for (size_t p = 0; p < np;) { /* runs of present pages, one fwrite each */
            if (!(bm[p >> 3] & (1u << (p & 7)))) { p++; continue; }
            size_t q = p, len = 0;
            while (q < np && (bm[q >> 3] & (1u << (q & 7)))) len += ss_page_len(&r[i], q++);
            if (ss_put(f, r[i].base + p * SS_PAGE, len) != len) return 0;
            p = q;
        }
    }
    return 1;
}
static int save_write(FILE *f, const SsRange *r, int n) {
    if (sCapBuf != NULL) { /* the captured bytes */
        const int ok = fwrite(sCapBuf, 1, sCapLen, f) == sCapLen;
        free(sCapBuf);
        sCapBuf = NULL;
        sCapLen = 0;
        return ok;
    }
    SsSink k = { f, NULL, 0, 0 };
    return save_write_sink(&k, r, n);
}
/* The state's bytes into memory (the file as it will be): the size first, then the copy. 1 = captured. */
static int save_capture(void) {
    SsRange r[SS_MAX_RANGES];
    const int n = layout(r);
    size_t size = sizeof(SaveHeader);
    for (int i = 0; i < n; i++) {
        if (!ss_sparse) { size += r[i].size; continue; }
        const size_t np = ss_pages(r[i].size);
        size += (np + 7) / 8;
        for (size_t p = 0; p < np; p++)
            if (!ss_page_zero(r[i].base + p * SS_PAGE, ss_page_len(&r[i], p))) size += ss_page_len(&r[i], p);
    }
    free(sCapBuf);
    sCapLen = 0;
    sCapBuf = (unsigned char *) malloc(size);
    if (sCapBuf == NULL) return 0; /* save_write then writes from game memory, as before */
    const double t0 = ss_ms();
    SsSink k = { NULL, sCapBuf, 0, size };
    if (!save_write_sink(&k, r, n)) {
        free(sCapBuf);
        sCapBuf = NULL;
        return 0;
    }
    sCapLen = k.len;
    fprintf(stderr, "savestate: captured %zu bytes in %.1f ms; the card is written on a helper thread\n", sCapLen,
            ss_ms() - t0);
    return 1;
}

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
    SS_SLEEP_JOIN(); /* a sleep save still syncing (it may be this slot's .tmp) */
    const int backstop = backstop_park();
    port_audio_mix_lock();
    if (save_capture()) { /* ALPHA6: into memory under the lock; the card is written without it */
        port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
        save_begin(slot, &j);
    } else { /* no memory for the copy: from game memory, under the lock, as before */
        save_begin(slot, &j);
        port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    }
    return save_finish(slot, &j);
}
static void notify_result(int save, int slot, int ok);
/* A menu save (ALPHA6): captured now, written to the card on the helper thread while the game goes on; its
 * result is shown when it is on the card. */
static SaveJob sMenuJob SAVESTATE_EXCLUDE;
static int sMenuSlot SAVESTATE_EXCLUDE;
static void menu_save_finish(void *arg) {
    (void) arg;
    save_begin(sMenuSlot, &sMenuJob);
    notify_result(1, sMenuSlot, save_finish(sMenuSlot, &sMenuJob));
}
static void do_save_async(int slot) {
    SS_SLEEP_JOIN();
    const int backstop = backstop_park();
    port_audio_mix_lock();
    const int captured = save_capture();
    port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    if (!captured) { notify_result(1, slot, do_save(slot)); return; }
#if defined(HANDHELD) && defined(MIYOO)
    sMenuSlot = slot;
    miyoo_sleepsave_async(menu_save_finish, NULL);
#else
    {
        SaveJob j;
        save_begin(slot, &j);
        notify_result(1, slot, save_finish(slot, &j));
    }
#endif
}
/* k1.8 (statefix A): the game's own save -- EEPROM, SRAM, flash, a controller pak, and the copies of
 * it the game keeps in RAM -- is the player's progress, not part of the moment a state captured. A
 * state restores those copies as they were when it was made, so a record or a save made after it was
 * lost: rolled back in the game, and written back over the newer file at the game's next save. After
 * a load the port puts the card's save back over what the state restored: its emulated save memory
 * and each RAM copy the game reads or writes back (what is a copy and what is the live game is the
 * port's call; each port's hh_game_save_reapply says what it does). Runs with the mixer parked and
 * the mix lock held, before the texture fixup. Weak: a port without it is as before. */
extern void hh_game_save_reapply(void) __attribute__((weak));
/* k1.8 (statefix B): a load is a gap in the sound. port_audio_out_gap(1) before it: the card fades out
 * and stays silent (what is queued is not heard on into the read); port_audio_out_gap(0) after it: the
 * queue (the old timeline's sound) is dropped, and the restored game's fades in once its cushion is
 * back (hh_declick.h). Weak: a port without it is as before. */
extern void port_audio_out_gap(int begin) __attribute__((weak));
static int do_load_file(const char *path, int slot) {
    SS_SLEEP_JOIN(); /* a sleep save still syncing: its state is the one on the card */
    const int backstop = backstop_park();
    if (port_audio_out_gap) port_audio_out_gap(1);
    port_audio_mix_lock();
    const int ok = do_load_file_locked(path, slot);
    port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    if (port_audio_out_gap) port_audio_out_gap(0);
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
        fprintf(stderr, "savestate: slot %d rejected (%s)\n", slot, st == STATE_MISMATCH ? sMismatch : "read error");
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
     * the release gate 2b proves it. */
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
    {
        char id[32];
        build_id(id);
        if (hh_state_compat_fixup != NULL && strncmp(sStateBuild, id, sizeof(sStateBuild)) != 0)
            hh_state_compat_fixup(sStateBuild);
    }
    if (hh_game_save_reapply) hh_game_save_reapply(); /* k1.8: the card's save over the state's copy of it */
    gfx_savestate_fixup();
    fprintf(stderr, "savestate: slot %d loaded (read %.0f ms, fixup %.0f ms)\n", slot, t_read - t0, ss_ms() - t_read);
    return 1;
}

static int do_load(int slot) {
    char name[32]; slot_name(slot, name, sizeof(name));
    return do_load_file(port_save_path(name), slot);
}

/* Quit with the auto save left as it is (the OS ending the game at the resume prompt). */
static void quit_keep_auto_save(void) {
    SS_SLEEP_JOIN();
    fflush(NULL);
    sync();
    port_request_exit();
}

#if defined(HANDHELD) && defined(MIYOO)
/* The sleep save (miyoo_sleepsave.h): the auto save RESUME GAME offers, written as the Mini goes to sleep. The
 * game waits only for the snapshot into the page cache (save_begin, as any save); the fsync, the rename and the
 * folder's fsync (save_finish) run on the kit's helper thread while the game goes on. A SIGSTOP or SIGKILL in
 * either half leaves the previous state as it was (the .tmp is renamed only once whole and synced). */
static SaveJob sSleepJob SAVESTATE_EXCLUDE;
static int sSleepKind SAVESTATE_EXCLUDE;
static int sSleepBegun SAVESTATE_EXCLUDE; /* the capture failed: save_begin already ran under the lock */
static void sleep_finish(void *arg) {
    (void) arg;
    if (!sSleepBegun) save_begin(QUICKSAVE_SLOT, &sSleepJob); /* ALPHA6: the captured bytes onto the card, here */
    miyoo_sleepsave_done(sSleepKind, save_finish(QUICKSAVE_SLOT, &sSleepJob));
}
static void sleep_save(int kind) {
    miyoo_sleepsave_join();
    const int backstop = backstop_park();
    port_audio_mix_lock();
    sSleepBegun = !save_capture();
    if (sSleepBegun) save_begin(QUICKSAVE_SLOT, &sSleepJob);
    port_audio_mix_unlock(); port_audio_backstop_enable(backstop);
    sSleepKind = kind;
    miyoo_sleepsave_async(sleep_finish, NULL);
}
#endif

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
    (void) msg; /* handheld_menu_notify is a no-op macro off handheld */
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
        if (r > 0) do_save_async(r);
        else       notify_result(0, -r, do_load(-r));
    }
#if defined(HANDHELD) && defined(MIYOO)
    /* A POWER press, or no button for a while: the auto save now, before Onion's sleep freezes the game (and
     * maybe powers off). Not before a resume has loaded, not with a quit's save coming, never two at once. */
    miyoo_sleepsave_start(HH_ENV("SLEEPSAVE"));
    if (!sBootPath[0] && !sExitSave && !miyoo_sleepsave_busy()) {
        const int k = miyoo_sleepsave_due();
        if (k) sleep_save(k);
    }
#endif
    if (sExitSave) {
        /* EXIT GAME keeps your place, as the OS shutting the game down does, so RESUME GAME is
         * always the last
         * session -- never an older one that would also roll back the EEPROM
         * copy held in memory. A failed save is shown before the quit.
         * k1.7: SAVING while it writes (the screen otherwise holds the last frame with the
         * music off for up to a few seconds on a slow card, which reads as a freeze), and the
         * failure waits until its banner is up (exit() could beat the detached notify thread). */
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
 * - Started with --loadStateFile <state> (tests): resume straight away, no prompt.
 * - Cold start with an auto save on the card (EXIT GAME, or the OS shutting the
 *   game down): ask RESUME GAME / NEW GAME. New deletes it.
 * Resume loads it a few frames in (savestate_check). An auto save from another
 * build is kept as state_quick.sav.rejected; one that can't be read is left alone. The desktop build has
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
    const int from_arg = sBootPath[0] != 0;
    if (!from_arg) {
        if (!savestate_slot_exists(QUICKSAVE_SLOT)) return;
        snprintf(sBootPath, sizeof(sBootPath), "%s", quick);
    }
    const int is_quick = strcmp(sBootPath, quick) == 0;
    const int st = state_file_check(sBootPath);
    if (st != STATE_OK) {
        if (st == STATE_MISMATCH && is_quick) {
            /* Not deleted: kept (one) as state_quick.sav.rejected, so nothing is lost while a mismatch is
             * looked into. A newer rejected state replaces the older one. */
            char rej[540];
            snprintf(rej, sizeof(rej), "%s.rejected", sBootPath);
            const int kept = rename(sBootPath, rej) == 0;
            fprintf(stderr, "savestate: auto save not loaded: %s; %s\n", sMismatch,
                    kept ? "kept as state_quick.sav.rejected" : "left in place (rename failed)");
        } else {
            fprintf(stderr, "savestate: %s can't be read; not loaded\n", sBootPath);
        }
        sBootPath[0] = 0;
        return;
    }
    if (from_arg) {
        fprintf(stderr, "savestate: resuming from %s (--loadStateFile)\n", sBootPath);
        return;
    }
    const char *ans = getenv(HH_ENV("RESUME_ANSWER")); /* tests: resume | new, no UI */
    const int choice = (ans && (ans[0] == 'r' || ans[0] == 'R')) ? 1
                     : (ans && (ans[0] == 'n' || ans[0] == 'N')) ? 0
                     : handheld_menu_resume_prompt();
    if (choice < 0) {
        /* The OS ended the game at the prompt: keep the auto save and quit. */
        fprintf(stderr, "savestate: power off at the prompt; auto save kept\n");
        quit_keep_auto_save();
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
