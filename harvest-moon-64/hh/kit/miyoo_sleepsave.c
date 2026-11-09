/* miyoo_sleepsave: the resume state written as the Miyoo goes to sleep, and the seal that keeps a save the
 * power-off cut short (see miyoo_sleepsave.h). The canonical copy is kit-sleepsave/; kit-sleepsave/sync.sh copies
 * it into all fifteen games. */
#include "miyoo_sleepsave.h"
#if defined(__has_include)
#if __has_include("savestate.h")
#include "savestate.h"
#endif
#endif
#ifndef SAVESTATE_EXCLUDE
#define SAVESTATE_EXCLUDE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The log: stderr, or the port's own (-DMIYOO_SLEEPSAVE_PORT_LOG: port_log; F-Zero X, whose binary must not name
 * stderr, as for miyoo_clock.c). */
#if defined(MIYOO_SLEEPSAVE_PORT_LOG)
extern void port_log(const char *fmt, ...);
#define SS_LOG(...) port_log(__VA_ARGS__)
#else
#define SS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

double miyoo_sleepsave_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------------------------------------------------
 * The seal (1.4.9). Onion freezes the game (SIGSTOP) as POWER is released, about 0.1-0.5 s after the press, but a
 * save takes 0.2-1 s from the press to its rename on the card (device logs: "resume state written (670 ms)",
 * "(976 ms)"): the fsync and the rename usually wait, frozen, for a wake. If none comes -- the battery runs out
 * while asleep, the power is cut -- the card has the whole new state as <slot>.tmp, the old one (or none) under
 * the slot's name, and the next start deleted the .tmp: the resume lost or an older one offered. Likewise when
 * Onion kills the game (SIGKILL ~1.4 s after its SIGTERM) in the middle of the exit save's fsync.
 *
 * So once a save's bytes are all in the file (before its fsync), the kit writes <tmp>.seal: the file's size and
 * CRC-32, read back from the page cache. The seal goes to the card with the state (Onion's own sync() before the
 * freeze, the kernel's writeback while asleep, the sync of Onion's shutdown). At the next start a .tmp whose seal
 * matches it byte for byte is renamed into place (it is newer than the file it replaces: every save goes through
 * the .tmp, and a later save truncates it); one that does not match is left to be deleted as before, and the
 * previous state is kept. After a save's rename the seal is removed. */
#define SEAL_MAGIC 0x314C5353u /* "SSL1" */
typedef struct {
    uint32_t magic, size_lo, size_hi, crc, self; /* self: CRC-32 of the four words before it */
} Seal;

static uint32_t sCrcTab[4][256] SAVESTATE_EXCLUDE;
static pthread_once_t sCrcOnce SAVESTATE_EXCLUDE = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        sCrcTab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 4; t++) sCrcTab[t][i] = (sCrcTab[t - 1][i] >> 8) ^ sCrcTab[0][sCrcTab[t - 1][i] & 0xFF];
}
/* CRC-32 (zlib's), four bytes a step. */
static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n) {
    uint32_t c = ~crc;
    while (n && ((uintptr_t) p & 3)) { c = (c >> 8) ^ sCrcTab[0][(c ^ *p++) & 0xFF]; n--; }
    while (n >= 4) {
        uint32_t w;
        memcpy(&w, p, 4); /* little-endian (ARM, x86) */
        c ^= w;
        c = sCrcTab[3][c & 0xFF] ^ sCrcTab[2][(c >> 8) & 0xFF] ^ sCrcTab[1][(c >> 16) & 0xFF] ^ sCrcTab[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) c = (c >> 8) ^ sCrcTab[0][(c ^ *p++) & 0xFF];
    return ~c;
}
/* The size and CRC-32 of the file at path, as it reads now. 1 = read to its end. */
static int file_crc(const char *path, uint64_t *size, uint32_t *crc) {
    pthread_once(&sCrcOnce, crc_init);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    enum { BUF = 64 * 1024 };
    unsigned char *buf = (unsigned char *) malloc(BUF);
    uint64_t n = 0;
    uint32_t c = 0;
    int ok = buf != NULL;
    while (ok) {
        const ssize_t r = read(fd, buf, BUF);
        if (r < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (r == 0) break;
        c = crc_update(c, buf, (size_t) r);
        n += (uint64_t) r;
    }
    free(buf);
    close(fd);
    *size = n;
    *crc = c;
    return ok;
}
static void seal_path(const char *tmp, char *out, size_t cap) { snprintf(out, cap, "%s.seal", tmp); }
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void miyoo_sleepsave_seal(const char *tmp) {
    const double t0 = miyoo_sleepsave_ms();
    char sp[600];
    seal_path(tmp, sp, sizeof(sp));
    Seal s;
    uint64_t size;
    if (!file_crc(tmp, &size, &s.crc)) { unlink(sp); SS_LOG("sleepsave: no seal for %s (read: %s)\n", base_name(tmp), strerror(errno)); return; }
    s.magic = SEAL_MAGIC;
    s.size_lo = (uint32_t) size;
    s.size_hi = (uint32_t) (size >> 32);
    s.self = crc_update(0, (const unsigned char *) &s, offsetof(Seal, self));
    const int fd = open(sp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, &s, sizeof(s)) != (ssize_t) sizeof(s)) {
        SS_LOG("sleepsave: no seal for %s (write: %s)\n", base_name(tmp), strerror(errno));
        if (fd >= 0) close(fd);
        unlink(sp);
        return;
    }
    close(fd); /* no fsync: Onion's sync() before the freeze, the kernel's writeback or the shutdown's sync */
    SS_LOG("sleepsave: sealed %s (%llu bytes, crc %08X, %.0f ms)\n", base_name(tmp), (unsigned long long) size,
           (unsigned) s.crc, miyoo_sleepsave_ms() - t0);
}

void miyoo_sleepsave_unseal(const char *tmp) {
    char sp[600];
    seal_path(tmp, sp, sizeof(sp));
    unlink(sp);
}

static void fsync_path(const char *path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}
static void fsync_dir_of(const char *path) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { if (slash == dir) slash[1] = 0; else *slash = 0; }
    else snprintf(dir, sizeof(dir), ".");
    fsync_path(dir);
}

int miyoo_sleepsave_recover(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int kept = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n <= 9 || strcmp(e->d_name + n - 9, ".tmp.seal") != 0) continue;
        char sp[600], tmp[600], dst[600];
        snprintf(sp, sizeof(sp), "%s/%s", dir, e->d_name);
        snprintf(tmp, sizeof(tmp), "%s", sp);
        tmp[strlen(tmp) - 5] = 0;   /* <name>.tmp */
        snprintf(dst, sizeof(dst), "%s", tmp);
        dst[strlen(dst) - 4] = 0;   /* <name> */
        Seal s;
        const char *why = NULL;
        struct stat st;
        const int fd = open(sp, O_RDONLY | O_CLOEXEC);
        const ssize_t r = fd >= 0 ? read(fd, &s, sizeof(s)) : -1;
        if (fd >= 0) close(fd);
        if (stat(tmp, &st) != 0) why = ""; /* the save finished (its seal outlived it): nothing to do */
        else if (r != (ssize_t) sizeof(s) || s.magic != SEAL_MAGIC ||
                 s.self != (pthread_once(&sCrcOnce, crc_init), crc_update(0, (const unsigned char *) &s, offsetof(Seal, self))))
            why = "its seal is damaged";
        else {
            uint64_t size;
            uint32_t crc;
            const uint64_t want = (uint64_t) s.size_lo | ((uint64_t) s.size_hi << 32);
            if ((uint64_t) st.st_size != want) why = "shorter than its seal says";
            else if (!file_crc(tmp, &size, &crc)) why = "it cannot be read";
            else if (size != want || crc != s.crc) why = "its CRC-32 does not match";
        }
        if (why == NULL) {
            if (rename(tmp, dst) == 0) {
                fsync_path(dst);
                fsync_dir_of(dst);
                kept++;
                SS_LOG("sleepsave: %s was cut off before its rename (a power-off) but is whole on the card: "
                       "kept as %s\n", base_name(tmp), base_name(dst));
            } else {
                SS_LOG("sleepsave: %s is whole but could not be renamed (%s); the previous file is kept\n",
                       base_name(tmp), strerror(errno));
            }
        } else if (why[0]) {
            SS_LOG("sleepsave: %s was cut off by a power-off and is not whole (%s): not used, the previous file is kept\n",
                   base_name(tmp), why);
        }
        unlink(sp);
    }
    closedir(d);
    return kept;
}

/* ------------------------------------------------------------------------------------------------------------
 * A whole file written safely: <path>.tmp, fsync, rename, then the renamed file and its folder once more (on the
 * card's vfat a rename over a file reaches the card only at that file's fsync or writeback, ~30 s). */
int miyoo_sleepsave_write_file(const char *path, const void *data, size_t size, char *why, size_t why_cap) {
    char tmp[600];
    const char *stage = NULL;
    int err = 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { stage = "open"; err = errno; }
    else {
        const unsigned char *p = (const unsigned char *) data;
        size_t left = size;
        while (left && !stage) {
            const ssize_t w = write(fd, p, left);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { stage = "write"; err = w < 0 ? errno : ENOSPC; break; }
            p += w;
            left -= (size_t) w;
        }
        if (!stage && fsync(fd) != 0) { stage = "fsync"; err = errno; }
        if (!stage && rename(tmp, path) != 0) { stage = "rename"; err = errno; }
        if (!stage) fsync(fd); /* the renamed file's directory slot */
        close(fd);
        if (stage) unlink(tmp); /* the old file is kept */
        else fsync_dir_of(path);
    }
    if (stage && why && why_cap) snprintf(why, why_cap, "%s: %s", stage, strerror(err));
    return stage == NULL;
}

int miyoo_sleepsave_tmp_fallback(const char *path, long size) {
    char tmp[600];
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (stat(path, &st) == 0 || stat(tmp, &st) != 0 || (long) st.st_size != size) return 0;
    if (rename(tmp, path) != 0) return 0;
    fsync_path(path);
    fsync_dir_of(path);
    SS_LOG("sleepsave: %s was missing; its whole %s (a save cut off by a power-off) is used\n", base_name(path),
           base_name(tmp));
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------
 * The sleep save: the buttons. */
#if defined(MIYOO)
#include <linux/input.h>
#include <SDL/SDL.h>

#define SS_POWER_MAX_AGE_MS 3000.0 /* a press older than this at the boundary (the game was stuck) is dropped */
#define SS_GAP_MS 1500.0           /* boundaries this far apart: the game was stopped (asleep) or loading */
#define SS_IDLE_DEFAULT_S 50       /* Onion's shortest idle sleep is 1 min (keymon checks every 15 s) */

/* Outside the save state (a load must not rewind these). */
static int sStarted SAVESTATE_EXCLUDE;
static int sOn SAVESTATE_EXCLUDE;
static double sIdleMs SAVESTATE_EXCLUDE;
static char sEnv[48] SAVESTATE_EXCLUDE;
static int sFd SAVESTATE_EXCLUDE = -1;
static volatile double sLastInputMs SAVESTATE_EXCLUDE;  /* the last button event (any) */
static volatile double sPowerAtMs SAVESTATE_EXCLUDE;    /* the last POWER press */
static volatile unsigned sPowerSeq SAVESTATE_EXCLUDE;   /* POWER presses seen by the thread */
static unsigned sPowerSeen SAVESTATE_EXCLUDE;           /* ... and handled at a boundary */
static volatile int sMenuHeld SAVESTATE_EXCLUDE;        /* MENU down (MENU+POWER is Onion's screenshot) */
static double sIdleBaseMs SAVESTATE_EXCLUDE;            /* idle time counts from max(last input, this) */
static double sIdleDoneFor SAVESTATE_EXCLUDE = -1;      /* the idle period already saved (its last button's time) */
static double sPrevBoundaryMs SAVESTATE_EXCLUDE;
static volatile int sMenuClosed SAVESTATE_EXCLUDE;      /* the overlay stepped aside for a press: reopen it */
static double sDueAtMs SAVESTATE_EXCLUDE;               /* the press, or the idle boundary, of the save in hand */
static pthread_t sAsync SAVESTATE_EXCLUDE;
static volatile int sAsyncBusy SAVESTATE_EXCLUDE;       /* the helper thread runs */
static int sAsyncJoinable SAVESTATE_EXCLUDE;
static void (*sAsyncFn)(void *) SAVESTATE_EXCLUDE;
static void *sAsyncArg SAVESTATE_EXCLUDE;

static void *reader_main(void *arg) {
    (void) arg;
    struct input_event ev[16];
    for (;;) {
        const ssize_t n = read(sFd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR) continue;
            SS_LOG("sleepsave: button read failed (%s); POWER and idle saves off\n", strerror(errno));
            sOn = 0;
            return NULL;
        }
        if (n == 0) { usleep(100000); continue; } /* a test FIFO with no writer */
        for (int i = 0; i < (int) (n / (ssize_t) sizeof(ev[0])); i++) {
            if (ev[i].type != EV_KEY) continue;
            const double now = miyoo_sleepsave_ms();
            sLastInputMs = now;
            if (ev[i].code == KEY_ESC) sMenuHeld = ev[i].value != 0;
            else if (ev[i].code == KEY_POWER && ev[i].value == 1 && !sMenuHeld) {
                sPowerAtMs = now;
                __sync_synchronize();
                sPowerSeq++;
            }
        }
    }
}

void miyoo_sleepsave_start(const char *env) {
    if (sStarted) return;
    sStarted = 1;
    snprintf(sEnv, sizeof(sEnv), "%s", env ? env : "MIYOO_SLEEPSAVE");
    const char *e = getenv(sEnv);
    if (e && e[0] == '0') { SS_LOG("sleepsave: off (%s=0)\n", sEnv); return; }
    char ienv[64];
    snprintf(ienv, sizeof(ienv), "%s_IDLE", sEnv);
    const char *ie = getenv(ienv);
    const int idle_s = ie && ie[0] ? atoi(ie) : SS_IDLE_DEFAULT_S;
    sIdleMs = idle_s > 0 ? idle_s * 1000.0 : 0;
    const char *dev = getenv("MIYOO_INPUT_DEV");
    if (!dev || !dev[0]) dev = "/dev/input/event0";
    sFd = open(dev, O_RDONLY | O_CLOEXEC);
    if (sFd < 0) { SS_LOG("sleepsave: off (%s: %s)\n", dev, strerror(errno)); return; }
    const double now = miyoo_sleepsave_ms();
    sLastInputMs = now;
    sIdleBaseMs = now;
    sPrevBoundaryMs = now;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 32 * 1024);
    if (pthread_create(&t, &a, reader_main, NULL) == 0) {
        pthread_detach(t);
        sOn = 1;
        atexit(miyoo_sleepsave_join); /* a sleep save still syncing finishes before the game quits */
        if (sIdleMs > 0)
            SS_LOG("sleepsave: on (resume state at a POWER press, and after %d s with no button; %s=0 off, %s=<s>)\n",
                    idle_s, sEnv, ienv);
        else
            SS_LOG("sleepsave: on (resume state at a POWER press; no idle save, %s=0; %s=0 off)\n", ienv, sEnv);
    } else {
        SS_LOG("sleepsave: off (no thread)\n");
        close(sFd);
        sFd = -1;
    }
    pthread_attr_destroy(&a);
}

static void reopen_menu(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = SDLK_ESCAPE;
    SDL_PushEvent(&ev);
}

int miyoo_sleepsave_due(void) {
    if (!sOn) return MIYOO_SLEEPSAVE_NONE;
    const double now = miyoo_sleepsave_ms();
    if (now - sPrevBoundaryMs > SS_GAP_MS) sIdleBaseMs = now; /* stopped (asleep) or loading: no idle save on a wake */
    sPrevBoundaryMs = now;
    const unsigned seq = sPowerSeq;
    __sync_synchronize();
    if (seq != sPowerSeen) {
        sPowerSeen = seq;
        const double at = sPowerAtMs, age = now - at;
        const int reopen = sMenuClosed;
        sMenuClosed = 0;
        if (age <= SS_POWER_MAX_AGE_MS) {
            sDueAtMs = at;
            sIdleDoneFor = sLastInputMs; /* this idle period is saved (the press is its last button) */
            if (reopen) reopen_menu(); /* opens at the next frame's events, after this save */
            return MIYOO_SLEEPSAVE_POWER;
        }
        SS_LOG("sleepsave: power press %.0f ms ago (the game was busy or stopped); not saved\n", age);
        if (reopen) reopen_menu();
    }
    if (sIdleMs > 0 && sLastInputMs != sIdleDoneFor) { /* one save per idle period: until the next button */
        const double start = sLastInputMs > sIdleBaseMs ? sLastInputMs : sIdleBaseMs;
        if (now - start >= sIdleMs) {
            sIdleDoneFor = sLastInputMs;
            sDueAtMs = now;
            return MIYOO_SLEEPSAVE_IDLE;
        }
    }
    return MIYOO_SLEEPSAVE_NONE;
}

void miyoo_sleepsave_done(int kind, int ok) {
    const double ms = miyoo_sleepsave_ms() - sDueAtMs;
    char what[48];
    if (kind == MIYOO_SLEEPSAVE_POWER) snprintf(what, sizeof(what), "power press");
    else snprintf(what, sizeof(what), "idle %.0f s", sIdleMs / 1000.0);
    if (ok) SS_LOG("sleepsave: %s, resume state written (%.0f ms)\n", what, ms);
    else SS_LOG("sleepsave: %s, resume state NOT written (%.0f ms; the previous one is kept)\n", what, ms);
}

int miyoo_sleepsave_menu_should_close(void) {
    if (!sOn || sPowerSeq == sPowerSeen) return 0;
    if (miyoo_sleepsave_ms() - sPowerAtMs > SS_POWER_MAX_AGE_MS) return 0;
    sMenuClosed = 1;
    return 1;
}

static void *async_main(void *arg) {
    (void) arg;
    sAsyncFn(sAsyncArg);
    __sync_synchronize();
    sAsyncBusy = 0;
    return NULL;
}

void miyoo_sleepsave_join(void) {
    if (sAsyncJoinable) {
        pthread_join(sAsync, NULL);
        sAsyncJoinable = 0;
    }
}

void miyoo_sleepsave_async(void (*fn)(void *), void *arg) {
    miyoo_sleepsave_join();
    sAsyncFn = fn;
    sAsyncArg = arg;
    sAsyncBusy = 1;
    __sync_synchronize();
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    if (pthread_create(&sAsync, &a, async_main, NULL) == 0) sAsyncJoinable = 1;
    else { fn(arg); sAsyncBusy = 0; }
    pthread_attr_destroy(&a);
}

int miyoo_sleepsave_busy(void) { return sAsyncBusy; }

#else /* not a Miyoo build: no buttons to watch */
void miyoo_sleepsave_start(const char *env) { (void) env; }
int miyoo_sleepsave_due(void) { return MIYOO_SLEEPSAVE_NONE; }
void miyoo_sleepsave_done(int kind, int ok) { (void) kind; (void) ok; }
int miyoo_sleepsave_menu_should_close(void) { return 0; }
void miyoo_sleepsave_async(void (*fn)(void *), void *arg) { fn(arg); }
void miyoo_sleepsave_join(void) {}
int miyoo_sleepsave_busy(void) { return 0; }
#endif
