/**
 * Save states (M6), after mario-kart-64's savestate.c: the same build
 * check, tmp + fsync + rename and launch prompt, with F-Zero X's
 * memory (src/port/savestate.ld). A state is two ranges:
 *   game: the game's .data/.bss, which hold the OS layer too (src/port/ultra: fibers
 *         and their stacks, message queues, clock, VI, SRAM) and anything tagged
 *         SAVESTATE_INCLUDE;
 *   ram:  the N64's 4 MB (.n64_rdram) and the overlays' .bss.
 * The port's own objects are renamed out of both at build time, so no heap, FILE* or
 * SDL pointer is ever saved or restored (the device randomises the heap every boot:
 * MK64 v25).
 *
 * gzip level 1: ~7 MB of memory, most of it idle fiber stack, is 1-2 MB on the card,
 * which keeps the exit save short. Since 2026-09-27 the zero pages aren't even
 * compressed (sparse states, below). A load decompresses the whole state first
 * (length and CRC checked) and only then copies it over game memory.
 *
 * Saves and loads run between retraces (port_main.c), when every fiber is blocked and
 * no RSP task is pending. Logging goes through port_log: the game binary must never
 * name stderr (gate 2b).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <zlib.h>
#include "libultra/ultra64.h"
#include "port_os.h"
#include "port_platform.h"
#include "savestate.h"
#include "handheld_menu.h"
#if defined(MIYOO)
#include "miyoo_sleepsave.h" /* the resume state at a POWER press and when idle (src/port/miyoo) */
#define SS_SLEEP_JOIN() miyoo_sleepsave_join()
#else
#define SS_SLEEP_JOIN() ((void) 0)
#endif

extern char __ss_game_start[], __ss_game_end[], __ss_ram_start[], __ss_ram_end[];
extern const unsigned int port_build_stamp; /* build/<plat>/gen/build_stamp.c */
extern const char* port_data_path(const char* name);
extern void port_request_exit(void);
extern void gfx_savestate_fixup(void);
extern void port_audio_flush(void);
/* k1.8 (statefix B): a load is a gap in the sound. port_audio_out_gap(1) before it: the card fades out
 * and stays silent; port_audio_out_gap(0) after it: the queue (the old timeline's sound) is dropped,
 * and the restored game's fades in once its cushion is back (audio_sdl.c, hh_declick.h). */
extern void port_audio_out_gap(int begin);
extern void port_audio_wav_mark(const char* what);
/* k1.8 (statefix A): the game's own save (fzerox.sram and the game's RAM copies of it) is the player's
 * progress, not part of the moment a state captured: after a load that happened, the card's save goes
 * back over what the state restored (port_misc.c; it says what it refreshes and what stays the
 * state's). Before the texture fixup; nothing of the game runs during a load. */
extern void hh_game_save_reapply(void);

#define SS_MAGIC 0x53585A46u /* "FZXS" */
#define SS_VERSION 1
typedef struct {
    uint32_t magic, version, stamp;
    uint32_t game_start, game_size; /* __ss_game_start.. of the build that wrote it */
    uint32_t ram_start, ram_size;
    uint32_t reserved;
} SaveHeader;

static int sRequest;          /* >0 save slot, <0 load slot, 0 idle */
static char sProgName[512];
static char sBootPath[512];   /* --loadStateFile <path>, or the auto save */
static int sBootFrames;
static int sExitSave;         /* EXIT GAME: quicksave, then quit */
static volatile int sLoopRunning;
static long sTestSaveAt = -1, sTestLoadAt = -1; /* FZX_SAVE_AT / FZX_LOAD_AT=<vi>:<slot> */
static int sTestSaveSlot, sTestLoadSlot;
static long sTestExitAt = -1; /* FZX_EXITSAVE_AT=<vi>: EXIT GAME at that retrace */
static int sTestExitSlot;
/* statefix tests: FZX_SAVEWRITE_AT=<vi>: a game save write at that retrace (save.c Save_PortTestWrite:
 * the routines a Time Attack record, an option and a Death Race end in, from the game's RAM copies);
 * FZX_MENU_AT=<vi> (device build): MENU pressed twice, so the handheld menu opens and closes at once
 * (its mute and unmute, headless). */
static long sTestWriteAt = -1, sTestMenuAt = -1;
static int sTestWriteSlot, sTestMenuSlot;
#define BOOT_LOAD_FRAME 3     /* the game has booted its threads; the load replaces them */

int savestate_available(void) {
    return 1;
}

static size_t game_size(void) {
    return (size_t) (__ss_game_end - __ss_game_start);
}

static size_t ram_size(void) {
    return (size_t) (__ss_ram_end - __ss_ram_start);
}

static void header_make(SaveHeader* h) {
    memset(h, 0, sizeof(*h));
    h->magic = SS_MAGIC;
    h->version = SS_VERSION;
    h->stamp = port_build_stamp;
    h->game_start = (uint32_t) (uintptr_t) __ss_game_start;
    h->game_size = (uint32_t) game_size();
    h->ram_start = (uint32_t) (uintptr_t) __ss_ram_start;
    h->ram_size = (uint32_t) ram_size();
}

/* Sparse states (handheld-kit k1.5, from GoldenEye e037c3b; the lead's round, 2026-09-27): each
 * range goes into the gzip stream as a bitmap of its 4 KB pages and then only the non-zero pages
 * (most of the N64's 4 MB and the idle fiber stacks are zero), so the device neither compresses
 * nor writes them. The header's reserved field says so (SS_FLAG_SPARSE); a full-size state of
 * the same build still loads. FZX_SSSPARSE=0 writes the full layout (the compatibility test). */
#define SS_FLAG_SPARSE 1u
#define SS_PAGE 4096u
static int ss_sparse_on(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("FZX_SSSPARSE");
        s = !(e && e[0] == '0');
    }
    return s;
}
/* The header this build wrote, full or sparse; *sparse says which. */
static int header_ok(const SaveHeader* h, int* sparse) {
    SaveHeader want, c = *h;
    header_make(&want);
    if (h->reserved != 0 && h->reserved != SS_FLAG_SPARSE) {
        return 0;
    }
    *sparse = h->reserved == SS_FLAG_SPARSE;
    c.reserved = 0;
    return memcmp(&c, &want, sizeof(c)) == 0;
}
/* One range out of the stream into dst (NULL: read and check only). Sparse: dst must be zeroed. */
static int range_read(gzFile gz, uint8_t* dst, size_t n, int sparse) {
    static uint8_t chunk[65536];
    const size_t pages = (n + SS_PAGE - 1) / SS_PAGE, bm_n = (pages + 7) / 8;
    uint8_t* bm;
    size_t i;
    int ok = 1;
    if (!sparse) {
        if (dst != NULL) {
            return gzread(gz, dst, (unsigned) n) == (int) n;
        }
        while (n > 0 && ok) {
            const unsigned want = n < sizeof(chunk) ? (unsigned) n : (unsigned) sizeof(chunk);
            ok = gzread(gz, chunk, want) == (int) want;
            n -= want;
        }
        return ok;
    }
    bm = malloc(bm_n ? bm_n : 1);
    if (bm == NULL || gzread(gz, bm, (unsigned) bm_n) != (int) bm_n) {
        free(bm);
        return 0;
    }
    for (i = 0; i < pages && ok; i++) {
        if ((bm[i >> 3] >> (i & 7)) & 1) {
            const size_t off = i * SS_PAGE, len = n - off < SS_PAGE ? n - off : SS_PAGE;
            ok = gzread(gz, dst != NULL ? dst + off : chunk, (unsigned) len) == (int) len;
        }
    }
    free(bm);
    return ok;
}
/* One range into the stream; *skipped counts the zero bytes left out. */
static int range_write(gzFile gz, const uint8_t* src, size_t n, int sparse, size_t* skipped) {
    static const uint8_t zero[SS_PAGE];
    const size_t pages = (n + SS_PAGE - 1) / SS_PAGE, bm_n = (pages + 7) / 8;
    uint8_t* bm;
    size_t i;
    int ok;
    if (!sparse) {
        return gzwrite(gz, src, (unsigned) n) == (int) n;
    }
    bm = calloc(bm_n ? bm_n : 1, 1);
    if (bm == NULL) {
        return 0;
    }
    for (i = 0; i < pages; i++) {
        const size_t off = i * SS_PAGE, len = n - off < SS_PAGE ? n - off : SS_PAGE;
        if (memcmp(src + off, zero, len) != 0) {
            bm[i >> 3] |= (uint8_t) (1u << (i & 7));
        } else {
            *skipped += len;
        }
    }
    ok = gzwrite(gz, bm, (unsigned) bm_n) == (int) bm_n;
    for (i = 0; i < pages && ok; i++) {
        if ((bm[i >> 3] >> (i & 7)) & 1) {
            const size_t off = i * SS_PAGE, len = n - off < SS_PAGE ? n - off : SS_PAGE;
            ok = gzwrite(gz, src + off, (unsigned) len) == (int) len;
        }
    }
    free(bm);
    return ok;
}
/* "free 12345k dirty 678k" from /proc/meminfo (Linux), or "" (the kit's save log) */
static void ss_meminfo(char* out, size_t cap) {
    FILE* m = fopen("/proc/meminfo", "r");
    char line[128];
    long fr = -1, dirty = -1;
    out[0] = 0;
    if (m == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), m)) {
        if (strncmp(line, "MemFree:", 8) == 0) fr = atol(line + 8);
        else if (strncmp(line, "Dirty:", 6) == 0) dirty = atol(line + 6);
    }
    fclose(m);
    if (fr >= 0) snprintf(out, cap, "free %ldk dirty %ldk", fr, dirty);
}

static void slot_name(int slot, char* out, size_t n) {
    if (slot == QUICKSAVE_SLOT) {
        snprintf(out, n, "state_quick.sav");
    } else {
        snprintf(out, n, "state_%02d.sav", slot);
    }
}

static void slot_path(int slot, char* out, size_t n) {
    char name[32];
    slot_name(slot, name, sizeof(name));
    snprintf(out, n, "%s", port_data_path(name));
}

/* 1 = a state this build can load, 0 = another build's or damaged, -1 = can't be read
 * (a read error is never a mismatch: only a 0 may ever delete a file). `full` also
 * decompresses the rest, checking its length and CRC: the launch prompt must not offer
 * RESUME for a state cut short (mk64 checked the file size; a gzip state has none to
 * check). The menu's "Used" reads the header only. */
enum { STATE_OK = 1, STATE_MISMATCH = 0, STATE_UNREADABLE = -1 };
static int gz_failed(gzFile gz) {
    int err = Z_OK;
    gzerror(gz, &err);
    return err == Z_ERRNO ? STATE_UNREADABLE : STATE_MISMATCH;
}
static int state_file_check(const char* path, int full) {
    SaveHeader h, want;
    gzFile gz = gzopen(path, "rb");
    int n, st = STATE_OK, sparse = 0;
    if (gz == NULL) {
        return STATE_UNREADABLE;
    }
    (void) want;
    n = gzread(gz, &h, sizeof(h));
    if (n < 0) {
        st = gz_failed(gz);
    } else if (n != (int) sizeof(h) || !header_ok(&h, &sparse)) {
        st = STATE_MISMATCH;
    } else if (full) {
        uint8_t end;
        if (!range_read(gz, NULL, game_size(), sparse) || !range_read(gz, NULL, ram_size(), sparse)) {
            int err = Z_OK;
            gzerror(gz, &err);
            st = err == Z_ERRNO ? STATE_UNREADABLE : STATE_MISMATCH; /* cut short or damaged */
        } else {
            n = gzread(gz, &end, 1); /* 0 only at a clean end, CRC checked */
            st = n == 0 ? STATE_OK : n < 0 ? gz_failed(gz) : STATE_MISMATCH;
        }
    }
    gzclose(gz);
    return st;
}

int savestate_slot_exists(int slot) {
    char path[512];
    slot_path(slot, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int savestate_slot_valid(int slot) {
    char path[512];
    slot_path(slot, path, sizeof(path));
    return state_file_check(path, 0) == STATE_OK;
}

/* Beside the old state, then rename: a power cut, a full card or an I/O error
 * mid-write leaves the previous state as it was.
 * The rename itself reaches the card before the save reports done: the state file is fsynced once
 * more after the rename, still open, then the folder (2026-10-01, super-smash-bros cd5128c, diddy-kong-racing
 * 9c91d40, mario-kart-64 2ba9042). On the card's vfat (Linux 4.14 vfat_rename) a rename over an
 * existing state moves the new file into the OLD file's directory slot (under a new name: a new slot
 * with cluster 0 and size 0) and only marks its inode dirty; the slot's start cluster and size reach
 * the card at that inode's writeback (~30 s) or its own fsync. The folder's fsync writes the removed
 * .tmp entry and the FAT with the old state's clusters freed, not that slot, so a hard power cut in
 * those ~30 s after a menu SAVE or the EXIT GAME save (a hang and a forced power-off, a flat battery)
 * left the state pointing at freed clusters: the state lost and
 * the FAT inconsistent. In the log line "rename" is now the rename and both fsyncs. */
int port_errno(void); /* port_errno.c: <errno.h> clashes with the N64 headers */
void port_errno_clear(void);

/* A save in two halves (the kit's sleep save, miyoo_sleepsave.h): save_begin writes the state into the page cache
 * (the game's memory is read here, between retraces); save_finish waits on the card (fsync, rename, fsync, the
 * folder) and logs. A menu SAVE and EXIT GAME run both at once (do_save); the sleep save runs save_finish on the
 * kit's helper thread while the game goes on. */
typedef struct {
    char path[512], tmp[520];
    u32 t0, tw, tc, tf, tr;
    int sparse, ok, keep; /* keep: a copy of the state's descriptor (gzclose closes the first) for the fsyncs */
    size_t skipped;
    const char* fail;     /* the stage that failed and why (the lead's matrix X4, kit 88210e7) */
    int ferr;
} SaveJob;
#define SS_FAIL(stage) do { if (!j->fail) { j->fail = (stage); j->ferr = port_errno(); } } while (0)
static void save_begin(int slot, SaveJob* j) {
    SaveHeader h;
    const size_t gs = game_size(), rs = ram_size();
    memset(j, 0, sizeof(*j));
    j->t0 = j->tw = j->tc = j->tf = j->tr = port_time_us();
    j->sparse = ss_sparse_on();
    j->keep = -1;
    slot_path(slot, j->path, sizeof(j->path));
    snprintf(j->tmp, sizeof(j->tmp), "%s.tmp", j->path);
    header_make(&h);
    if (j->sparse) {
        h.reserved = SS_FLAG_SPARSE;
    }
    {
        const int fd = open(j->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            SS_FAIL("open");
        } else {
            gzFile gz;
            j->keep = dup(fd); /* gzclose closes fd; fsync through the copy, before and after the rename */
            if (j->keep < 0) SS_FAIL("dup");
            port_errno_clear();
            gz = gzdopen(fd, "wb1");
            if (gz != NULL) {
                port_errno_clear();
                j->ok = gzwrite(gz, &h, sizeof(h)) == (int) sizeof(h) &&
                        range_write(gz, (const uint8_t*) __ss_game_start, gs, j->sparse, &j->skipped) &&
                        range_write(gz, (const uint8_t*) __ss_ram_start, rs, j->sparse, &j->skipped);
                if (!j->ok) SS_FAIL("write");
                j->tw = port_time_us();
                port_errno_clear();
                if (gzclose(gz) != Z_OK) {
                    SS_FAIL("close");
                    j->ok = 0;
                }
            } else {
                SS_FAIL("gzdopen");
                close(fd);
                j->tw = port_time_us();
            }
            j->tc = port_time_us();
        }
    }
}
static int save_finish(int slot, SaveJob* j) {
    char mi[64];
    struct stat st;
    int ok = j->ok;
    if (j->keep >= 0 || ok) {
        if (j->keep < 0 || fsync(j->keep) != 0) {
            if (j->keep >= 0) SS_FAIL("fsync");
            ok = 0;
        }
    }
    j->tf = port_time_us();
    if (ok && rename(j->tmp, j->path) != 0) {
        SS_FAIL("rename");
        ok = 0;
    }
    if (ok) {
        int dir;
        if (fsync(j->keep) != 0) { /* the renamed file's directory slot (see above) */
            port_log("savestate: fsync after the rename: %s (the state is saved)\n", strerror(port_errno()));
        }
        dir = open(port_data_path(""), O_RDONLY);
        if (dir < 0 || fsync(dir) != 0) {
            port_log("savestate: folder fsync: %s (the state is saved)\n", strerror(port_errno()));
        }
        if (dir >= 0) {
            close(dir);
        }
    } else {
        remove(j->tmp);
        port_log("savestate: slot %d FAILED at %s: %s\n", slot == QUICKSAVE_SLOT ? 0 : slot, j->fail ? j->fail : "?",
                 j->fail && j->ferr ? strerror(j->ferr) : "unknown error");
    }
    if (j->keep >= 0) {
        close(j->keep);
        j->keep = -1;
    }
    j->tr = port_time_us();
    ss_meminfo(mi, sizeof(mi));
    port_log("savestate: slot %d %s, %u KB (%s, %u KB of zero pages left out) in %u ms: write %u close %u fsync %u rename %u | %s\n",
             slot == QUICKSAVE_SLOT ? 0 : slot, ok ? "saved" : "FAILED, previous state kept",
             ok && stat(j->path, &st) == 0 ? (unsigned) (st.st_size / 1024) : 0, j->sparse ? "sparse" : "full", (unsigned) (j->skipped / 1024),
             (unsigned) ((j->tr - j->t0) / 1000), (unsigned) ((j->tw - j->t0) / 1000), (unsigned) ((j->tc - j->tw) / 1000),
             (unsigned) ((j->tf - j->tc) / 1000), (unsigned) ((j->tr - j->tf) / 1000), mi);
    if (ok) {
        port_audio_wav_mark("save");
    }
    return ok;
}
#undef SS_FAIL

static int do_save(int slot) {
    SaveJob j;
    SS_SLEEP_JOIN(); /* a sleep save still syncing (it may be this slot's .tmp) */
    save_begin(slot, &j);
    return save_finish(slot, &j);
}

/* The checked state over game memory, page by page of memory: a page that is zero in the state and
 * already zero in memory is left alone (diddy-kong-racing 2da5231, mario-kart-64 3234d2a; the cross-port
 * list's E). A page this process never touched -- most of the idle fiber stacks and of the N64's
 * 4 MB on a resume at boot -- reads as the kernel's shared zero page and stays that; the plain copy
 * wrote every one (a race state has ~4.85 MB of zero pages), process memory on the 64 MB device
 * that only swap to the card could free (the OS has a swap partition), and the next save's page
 * scan read it back from there. The same bytes either way. */
static void copy_back(uint8_t* dst, const uint8_t* src, size_t n) {
    static const uint8_t zero[SS_PAGE];
    size_t off = 0;
    while (off < n) {
        size_t len = SS_PAGE - (size_t) ((uintptr_t) (dst + off) & (SS_PAGE - 1));
        if (len > n - off) {
            len = n - off;
        }
        if (memcmp(src + off, zero, len) != 0 || memcmp(dst + off, zero, len) != 0) {
            memcpy(dst + off, src + off, len);
        }
        off += len;
    }
}

static int do_load_file_locked(const char* path, int slot) {
    SaveHeader h, want;
    const size_t gs = game_size(), rs = ram_size();
    uint8_t* buf;
    uint8_t extra;
    int ok, sparse = 0;
    gzFile gz = gzopen(path, "rb");
    if (gz == NULL) {
        port_log("savestate: slot %d empty (%s)\n", slot == QUICKSAVE_SLOT ? 0 : slot, path);
        return 0;
    }
    (void) want;
    if (gzread(gz, &h, sizeof(h)) != (int) sizeof(h) || !header_ok(&h, &sparse)) {
        port_log("savestate: slot %d rejected (another build, or damaged)\n", slot == QUICKSAVE_SLOT ? 0 : slot);
        gzclose(gz);
        return 0;
    }
    /* All of it into memory first: game memory is only touched once the whole state
     * has read back with the right length and CRC. A sparse state's missing pages are the
     * zeros calloc gives. */
    buf = sparse ? calloc(gs + rs, 1) : malloc(gs + rs);
    if (buf == NULL) {
        gzclose(gz);
        return 0;
    }
    ok = range_read(gz, buf, gs, sparse) && range_read(gz, buf + gs, rs, sparse);
    if (ok && gzread(gz, &extra, 1) != 0) {
        ok = 0; /* 0 only at a clean end, CRC checked */
    }
    if (gzclose(gz) != Z_OK) {
        ok = 0;
    }
    if (!ok) {
        free(buf);
        port_log("savestate: slot %d short or damaged; not loaded\n", slot == QUICKSAVE_SLOT ? 0 : slot);
        return 0;
    }
    copy_back((uint8_t*) __ss_game_start, buf, gs);
    copy_back((uint8_t*) __ss_ram_start, buf + gs, rs);
    free(buf);
    hh_game_save_reapply(); /* k1.8: the card's save over the state's copy of it */
    gfx_savestate_fixup(); /* textures re-import from the restored memory */
    { extern int gPortStateLoaded; extern void port_platform_load_picture(void); /* FZX_LOADPIC */
      gPortStateLoaded = 1; port_platform_load_picture(); } /* the saving process's last picture, drawn again */
    port_audio_flush();    /* the old timeline's queued samples (FZX_DECLICK=0; else port_audio_out_gap(0)) */
    port_log("savestate: slot %d loaded, vi %u\n", slot == QUICKSAVE_SLOT ? 0 : slot, (unsigned) port_os_vi_count());
    return 1;
}

static int do_load_file(const char* path, int slot) {
    int ok;
    SS_SLEEP_JOIN(); /* a sleep save still syncing: its state is the one on the card */
    port_audio_out_gap(1);
    ok = do_load_file_locked(path, slot);
    port_audio_out_gap(0);
    return ok;
}

/* bench.c: one state in RAM, raw (the bench restores it ten times). */
static uint8_t* sMemSnap;
int savestate_mem_snapshot(void) {
    if (sMemSnap == NULL) {
        sMemSnap = malloc(game_size() + ram_size());
    }
    if (sMemSnap == NULL) {
        return 0;
    }
    memcpy(sMemSnap, __ss_game_start, game_size());
    memcpy(sMemSnap + game_size(), __ss_ram_start, ram_size());
    return 1;
}

int savestate_mem_restore(void) {
    if (sMemSnap == NULL) {
        return 0;
    }
    memcpy(__ss_game_start, sMemSnap, game_size());
    memcpy(__ss_ram_start, sMemSnap + game_size(), ram_size());
    gfx_savestate_fixup();
    { extern int gPortStateLoaded; extern void port_platform_load_picture(void); gPortStateLoaded = 1; port_platform_load_picture(); }
    port_audio_flush();
    return 1;
}

static int do_load(int slot) {
    char path[512];
    slot_path(slot, path, sizeof(path));
    return do_load_file(path, slot);
}


/* A state is to be loaded at boot (--loadStateFile, RESUME GAME): port_main.c shows no boot picture
 * (FZX_LOADPIC). */
int savestate_boot_load_pending(void) {
    return sBootPath[0] != 0;
}

void savestate_request_save(int slot) {
    sRequest = slot;
}

void savestate_request_load(int slot) {
    sRequest = -slot;
}

void savestate_request_exit_save(void) {
    sExitSave = 1;
}

static void test_hook(const char* env, long* vi, int* slot) {
    const char* e = getenv(env);
    char* end;
    if (e && e[0]) {
        *vi = strtol(e, &end, 10);
        *slot = (*end == ':') ? atoi(end + 1) : 1;
    }
}

void savestate_parse_args(int argc, char** argv) {
    int i;
    if (argc > 0 && argv[0]) {
        snprintf(sProgName, sizeof(sProgName), "%s", argv[0]);
    }
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--loadStateFile") == 0) {
            snprintf(sBootPath, sizeof(sBootPath), "%s", argv[i + 1]);
        }
    }
    /* Tests: FZX_SAVE_AT / FZX_LOAD_AT=<vi>:<slot> save or load at that retrace. */
    test_hook("FZX_SAVE_AT", &sTestSaveAt, &sTestSaveSlot);
    test_hook("FZX_LOAD_AT", &sTestLoadAt, &sTestLoadSlot);
    test_hook("FZX_EXITSAVE_AT", &sTestExitAt, &sTestExitSlot);
    test_hook("FZX_SAVEWRITE_AT", &sTestWriteAt, &sTestWriteSlot);
    test_hook("FZX_MENU_AT", &sTestMenuAt, &sTestMenuSlot);
}

#if defined(MIYOO)
/* The sleep save (miyoo_sleepsave.h): the auto save RESUME GAME offers, written as the Mini goes to sleep. The
 * game waits only for the snapshot into the page cache (save_begin: the gzip stream, between retraces); the
 * fsync, the rename and the folder's fsync (save_finish) run on the kit's helper thread while the game goes on.
 * A SIGSTOP or SIGKILL in either half leaves the previous state as it was (the .tmp is renamed only once whole
 * and synced). */
static SaveJob sSleepJob;
static int sSleepKind;
static void sleep_finish(void* arg) {
    (void) arg;
    miyoo_sleepsave_done(sSleepKind, save_finish(QUICKSAVE_SLOT, &sSleepJob));
}
static void sleep_save(int kind) {
    miyoo_sleepsave_join();
    save_begin(QUICKSAVE_SLOT, &sSleepJob);
    sSleepKind = kind;
    miyoo_sleepsave_async(sleep_finish, NULL);
}
#endif

/* The OS notification for a menu SAVE or LOAD, composed from what happened rather than what was
 * asked for (B7, handheld-kit k1): the menu showed SAVED SLOT n as it closed, before the save had
 * run, and even when it then failed (a full card, a damaged state: the log said FAILED, the screen
 * said SAVED). */
static void notify_result(int save, int slot, int ok) {
    char msg[32];
    if (!ok) {
        snprintf(msg, sizeof(msg), "%s", save ? "SAVE FAILED" : "LOAD FAILED");
    } else if (save) {
        snprintf(msg, sizeof(msg), "SAVED SLOT %d", slot);
    } else if (slot == QUICKSAVE_SLOT) {
        snprintf(msg, sizeof(msg), "LOADED AUTO SAVE");
    } else {
        snprintf(msg, sizeof(msg), "LOADED SLOT %d", slot);
    }
    handheld_menu_notify(msg);
    (void) msg; /* handheld_menu_notify is a no-op macro off the device */
}

/* Between retraces (port_main.c): every fiber is blocked, no RSP task is pending. */
void savestate_check(void) {
    const u32 vi = port_os_vi_count();
    int r;
    sLoopRunning = 1;
    if (sBootPath[0] && ++sBootFrames >= BOOT_LOAD_FRAME) {
        do_load_file(sBootPath, QUICKSAVE_SLOT);
        sBootPath[0] = 0;
    }
    if (sTestSaveAt >= 0 && vi >= (u32) sTestSaveAt) {
        sTestSaveAt = -1;
        do_save(sTestSaveSlot);
    }
    if (sTestLoadAt >= 0 && vi >= (u32) sTestLoadAt) {
        sTestLoadAt = -1;
        do_load(sTestLoadSlot);
    }
    if (sTestExitAt >= 0 && vi >= (u32) sTestExitAt) {
        sTestExitAt = -1;
        sExitSave = 1;
    }
    if (sTestWriteAt >= 0 && vi >= (u32) sTestWriteAt) {
        extern void Save_PortTestWrite(void);
        sTestWriteAt = -1;
        port_log("savestate: FZX_SAVEWRITE_AT: the game's save writes (Mute City's records, the settings, Death Race)\n");
        Save_PortTestWrite();
    }
    if (sTestMenuAt >= 0 && vi >= (u32) sTestMenuAt) {
        sTestMenuAt = -1;
#if defined(HANDHELD_DEVICE)
        {
            extern void port_test_menu_press(void);
            port_test_menu_press();
        }
#endif
    }
    r = sRequest;
    if (r != 0) {
        sRequest = 0;
        if (r > 0) {
            notify_result(1, r, do_save(r));
        } else {
            notify_result(0, -r, do_load(-r));
        }
    }
#if defined(MIYOO)
    /* A POWER press, or no button for a while: the auto save now, before Onion's sleep freezes the game (and
     * maybe powers off). Not before a resume has loaded, not with a quit's save coming, never two at once. */
    miyoo_sleepsave_start("FZX_SLEEPSAVE");
    if (!sBootPath[0] && !sExitSave && !miyoo_sleepsave_busy()) {
        const int k = miyoo_sleepsave_due();
        if (k) {
            sleep_save(k);
        }
    }
#endif
    if (sExitSave) {
        /* EXIT GAME keeps your place, as the OS shutting the game down does, so RESUME GAME is
         * always the last session.
         * X50 (from ocarina-of-time): SAVING while it writes (the screen otherwise holds the last
         * frame with the music off for up to a second or more on a slow card, which reads as a
         * freeze), and a failed save says so, waiting until its banner is up (the process quits
         * right after: a detached notify thread could lose the race with exit()). */
        sExitSave = 0;
        if (!sBootPath[0]) {
            handheld_menu_notify("SAVING");
            if (!do_save(QUICKSAVE_SLOT)) {
                handheld_menu_notify_wait("SAVE FAILED");
            }
        }
        port_request_exit();
    }
}

/* After the game loop. */
void savestate_shutdown(void) {
    sLoopRunning = 0;
    SS_SLEEP_JOIN();
}

/* A save cut short by a power cut leaves <slot>.tmp behind; nothing reads it. */
static void clean_state_tmp(void) {
    char dir[512];
    DIR* d;
    struct dirent* e;
    snprintf(dir, sizeof(dir), "%s", port_data_path(""));
    d = opendir(dir);
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, "state_", 6) == 0 && n > 8 && strcmp(e->d_name + n - 8, ".sav.tmp") == 0) {
            remove(port_data_path(e->d_name));
        }
    }
    closedir(d);
}

/* At launch:
 * - started with --loadStateFile <state> (tests): resume straight away, no prompt;
 * - a cold start with an auto save on the card (EXIT GAME, or the OS shutting the game
 *   down) asks RESUME GAME / NEW GAME; NEW deletes it.
 * The resume loads a few retraces in (savestate_check). An auto save from another build
 * is removed; one that can't be read is left alone. The desktop build has no prompt and
 * never deletes: only an explicit --loadStateFile. */
void savestate_boot_prompt(void) {
    clean_state_tmp();
#if !defined(HANDHELD_DEVICE)
    if (sBootPath[0] && state_file_check(sBootPath, 1) != STATE_OK) {
        port_log("savestate: %s is not a state for this build; not loaded\n", sBootPath);
        sBootPath[0] = 0;
    }
#else
    {
        char quick[512];
        int from_arg, is_quick, st, choice;
        const char* ans;
        slot_path(QUICKSAVE_SLOT, quick, sizeof(quick));
        from_arg = sBootPath[0] != 0;
        if (!from_arg) {
            if (!savestate_slot_exists(QUICKSAVE_SLOT)) {
                return;
            }
            snprintf(sBootPath, sizeof(sBootPath), "%s", quick);
        }
        is_quick = strcmp(sBootPath, quick) == 0;
        st = state_file_check(sBootPath, 1);
        if (st != STATE_OK) {
            if (st == STATE_MISMATCH && is_quick) {
                port_log("savestate: auto save is from another build, or damaged; removed\n");
                remove(sBootPath);
            } else {
                port_log("savestate: %s can't be read; not loaded\n", sBootPath);
            }
            sBootPath[0] = 0;
            return;
        }
        if (from_arg) {
            port_log("savestate: resuming from %s (--loadStateFile)\n", sBootPath);
            return;
        }
        ans = getenv("FZX_RESUME_ANSWER"); /* tests: resume | new, no UI */
        choice = (ans && (ans[0] == 'r' || ans[0] == 'R'))   ? 1
                 : (ans && (ans[0] == 'n' || ans[0] == 'N')) ? 0
                                                             : handheld_menu_resume_prompt();
        if (choice < 0) {
            /* The OS ended the game at the prompt: keep the auto save and quit. */
            port_log("savestate: power off at the prompt; auto save kept\n");
#if defined(MIYOO)
            { /* the OS is ending the game (SIGTERM): nothing new to save; quit now */
                extern void port_platform_quit(void);
                port_platform_quit();
                exit(0);
            }
#endif
        } else if (choice) {
            port_log("savestate: resuming from %s\n", sBootPath);
        } else {
            port_log("savestate: new game, auto save removed\n");
            if (is_quick) {
                remove(sBootPath);
            }
            sBootPath[0] = 0;
        }
    }
#endif
}
