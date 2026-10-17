/* sram.c -- see hh_sram.h. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hh_sram.h"
#include "handheld_menu.h" /* GAME SAVE FAILED on the OS bar (a no-op off the handheld) */
#include "port.h"
#include "hh_game.h"

/* The mirror of the cartridge SRAM is in save states (HH_GS_BSS), but sram.bin is the player's
 * progress, not part of the moment a state captured: after a load the card's save goes back over
 * the state's (hh_game_save_reapply below; statefix A, kit k1.8, 2026-10-09). Until then a load
 * brought back the records the state had, and the game's next write (it writes its whole backup block
 * each time, from gSCManagerBackupData, also in the state) put them over the newer file.
 * sSramRead: the game has read sSram from sram.bin in this timeline. It is in the state with sSram
 * (statefix, 2026-10-09): as the process's flag, a state made before the game's first read (its first
 * ticks), loaded in a process that had read, left the state's all-zero sSram standing as "read", and the
 * restored boot's read found a blank cartridge, formatted it and wrote the defaults over sram.bin. Now
 * that load reads sram.bin again at the game's own first read, as a cold start does. The file's flags
 * below (sKeep, sDirty, sFails) are this session's, not game state. */
static uint8_t sSram[HH_SRAM_SIZE] HH_GS_BSS;
static int sSramRead HH_GS_BSS;
/* 2026-10-01 (OPEN_ISSUES S3; paper-mario 742bdb4's rule). sKeep: sram.bin is there but could not be
 * opened or read whole (EIO, a broken cluster chain). The game then sees a blank cartridge, formats it
 * and writes the defaults, which replaced the player's file: now no write replaces it this session (a
 * later start that reads it finds it as it was). sDirty: a write failed, so the image differs from the
 * file: the next game write tries the whole image again, even one that changes nothing. sFails counts
 * the failed tries since the last good write. */
static int sKeep, sDirty, sFails;

static void sram_load(void)
{
    FILE *f;
    if (sSramRead) return;
    hh_sram_import(); /* main runs it first; this is for a start that does not */
    sSramRead = 1;
    memset(sSram, 0, sizeof sSram);   /* a blank cartridge: the game formats it */
    errno = 0;
    f = fopen(port_save_path("sram.bin"), "rb");
    if (f) {
        size_t n = fread(sSram, 1, sizeof sSram, f);
        const int bad = ferror(f), err = errno;
        fclose(f);
        if (bad) {
            sKeep = 1;
            port_log_always("SSB64: sram.bin: read error after %u bytes (%s): kept as it is, no save replaces it this session\n",
                            (unsigned)n, strerror(err));
        } else {
            port_log("SSB64: sram.bin loaded (%u bytes)\n", (unsigned)n);
        }
    } else if (errno != ENOENT) {
        sKeep = 1;
        port_log_always("SSB64: sram.bin cannot be opened (%s): no save replaces it this session\n", strerror(errno));
    }
}

/* Written to sram.bin.tmp, fsynced, renamed over sram.bin, then the folder fsynced so the rename itself
 * survives a power cut (kit k1.5 / GoldenEye e0a9c32; the lead's X4, round 18). A failure names
 * its stage and the reason on the console.
 * 2026-10-01: the file is fsynced again after the rename, before the folder. On the card's vfat (Linux
 * 4.14 vfat_rename) a rename over an existing file moves the new file into the OLD file's directory slot
 * and only marks it dirty: the slot's new start cluster and size reach the card when that inode is
 * written back (~30 s later) or synced. The folder fsync writes the deleted .tmp entry and the FAT (the
 * old file's clusters freed) but not that slot, so a hard power cut in those ~30 s left sram.bin pointing
 * at the freed clusters of the previous save: the new save lost and the FAT inconsistent (a read past the
 * first cluster is an "invalid cluster chain" error). fsync on the still-open file writes the slot.
 * SSB64_DIRSYNC=0: neither sync after the rename (A/B: tmp, fsync, rename only, as before round 18). */
static int file_write_durable(const char *name, const uint8_t *data, size_t size)
{
    char path[700], tmp[720], dir[700];
    int fd, err = 0;
    ssize_t n;
    const char *stage = NULL;
    const char *e = getenv("SSB64_DIRSYNC");
    const int sync_rename = !(e && e[0] == '0');
    extern void port_log_always(const char *fmt, ...);
    snprintf(path, sizeof path, "%s", port_save_path(name));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { err = errno; port_log_always("SSB64: %s write failed at open: %s\n", name, strerror(err)); return -1; }
    n = write(fd, data, size);
    if (n != (ssize_t)size) { stage = "write"; err = n < 0 ? errno : ENOSPC; }
    else if (fsync(fd) != 0) { stage = "fsync"; err = errno; }
    else if (rename(tmp, path) != 0) { stage = "rename"; err = errno; }
    if (stage != NULL) {
        close(fd);
        unlink(tmp);
        port_log_always("SSB64: %s write failed at %s: %s\n", name, stage, strerror(err));
        return -1;
    }
    /* The new data is in place under its name from here on; a failed sync below only leaves it to the
     * kernel's writeback, as before. */
    if (sync_rename && fsync(fd) != 0) port_log_always("SSB64: %s fsync after the rename failed: %s\n", name, strerror(errno));
    close(fd);
    if (sync_rename) {
        char *slash;
        snprintf(dir, sizeof dir, "%s", path);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
            fd = open(dir[0] ? dir : "/", O_RDONLY);
            if (fd < 0 || fsync(fd) != 0) port_log_always("SSB64: %s folder fsync failed: %s\n", name, strerror(errno));
            if (fd >= 0) close(fd);
        }
    }
    return 0;
}

static int sram_flush(void)
{
    if (sKeep) { port_log_always("SSB64: sram.bin not written: it could not be read at the start\n"); return -1; }
    return file_write_durable("sram.bin", sSram, sizeof sSram);
}

/* ---- Save import (2026-10-16; Reddit: "I can't import my Super Smash Bros save") ----
 * sram.bin is the port's own format, not a cartridge image: the game's SRAM DMA is a memcpy of its save
 * struct (lbbackup.c, gSCManagerBackupData), so the u16/u32 fields are little-endian. A .sra/.srm/.sav from
 * an emulator or a flash cart, renamed to sram.bin, fails the game's checksum in every byte order and the
 * game formats it: a fresh game, and the player's file overwritten with the defaults.
 * Now, at start-up (main, before the boot resume can load a state; sram_load runs it too), each *.sra,
 * *.srm or *.sav in the data folder (not the save states, state_*.sav) and sram.bin itself are tried as:
 * the port's own format, the cartridge's big-endian order (EverDrive, ares), 32-bit word-swapped
 * (Project64, mupen64plus, RetroArch Mupen64Plus-Next / ParaLLEl), 16-bit byte-swapped and halfword-swapped;
 * a 0x48800-byte RetroArch .srm (EEPROM 0x800, 4 Controller Paks 0x20000, SRAM 0x8000, FlashRAM 0x20000) by
 * its SRAM part at 0x20800. The first reading where the game's own check passes (lbbackup.c,
 * lbBackupPortImportCart / lbBackupPortIsNativeValid: checksum and signature 666, copy 0 or copy 1) is
 * converted field by field. If several files pass, the newest wins. The old sram.bin is kept as
 * sram.bin.bak-<time>, the new one written the durable way (file_write_durable), the source renamed
 * <name>.imported so it is not applied again. Nothing passes: nothing is changed, one line says why.
 * A save state made before the import: the boot resume or a later load runs hh_game_save_reapply, which
 * puts sram.bin's save over the state's (statefix A): the imported save wins.
 * SSB64_SAVEIMPORT=0: off (tests). */
extern int32_t lbBackupPortIsNativeValid(const uint8_t *sram, uint32_t size);
extern int32_t lbBackupPortImportCart(const uint8_t *cart, uint32_t size, uint8_t *out, uint32_t out_size,
                                      char *info, uint32_t info_size);

#define IMPORT_SRM_SIZE     0x48800u
#define IMPORT_SRM_SRAM_OFF 0x20800u
#define IMPORT_MIN_SIZE     0x5ECu    /* one copy of the game's save */
#define IMPORT_MAX_SIZE     0x80000u
#define IMPORT_MAX_FILES    16

static const char *const kImportOrder[] = {
    "big-endian, the cartridge's own order (EverDrive / flash cart, ares)",
    "32-bit word-swapped (Project64, mupen64plus, RetroArch Mupen64Plus-Next / ParaLLEl)",
    "16-bit byte-swapped",
    "16-bit halfword-swapped",
};

static void import_reorder(uint8_t *dst, const uint8_t *src, int order)
{
    uint32_t i;
    for (i = 0; i < HH_SRAM_SIZE; i += 4) {
        const uint8_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
        switch (order) {
        case 1:  dst[i] = d; dst[i + 1] = c; dst[i + 2] = b; dst[i + 3] = a; break;
        case 2:  dst[i] = b; dst[i + 1] = a; dst[i + 2] = d; dst[i + 3] = c; break;
        case 3:  dst[i] = c; dst[i + 1] = d; dst[i + 2] = a; dst[i + 3] = b; break;
        default: dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d; break;
        }
    }
}

/* data (size bytes, a whole file): 1 a cartridge save converted into img, 2 already the port's format
 * (img = it), 0 neither. what: the reading that passed. */
static int import_try(const uint8_t *data, size_t size, uint8_t *img, char *what, size_t what_n, char *info, size_t info_n)
{
    static uint8_t buf[HH_SRAM_SIZE], cart[HH_SRAM_SIZE];
    size_t offs[2];
    int n = 0, k, order;
    if (size >= IMPORT_SRM_SRAM_OFF + HH_SRAM_SIZE) offs[n++] = IMPORT_SRM_SRAM_OFF;
    offs[n++] = 0;
    for (k = 0; k < n; k++) {
        const size_t len = size - offs[k] < HH_SRAM_SIZE ? size - offs[k] : HH_SRAM_SIZE;
        const char *part = offs[k] ? ", the SRAM part of a RetroArch .srm (at 0x20800)" : "";
        int32_t copy;
        memset(buf, 0, sizeof buf);
        memcpy(buf, data + offs[k], len);
        if ((copy = lbBackupPortIsNativeValid(buf, HH_SRAM_SIZE)) >= 0) {
            memcpy(img, buf, HH_SRAM_SIZE);
            snprintf(what, what_n, "already this port's format%s, copy %d", part, (int)copy);
            info[0] = '\0';
            return 2;
        }
        for (order = 0; order < 4; order++) {
            import_reorder(cart, buf, order);
            copy = lbBackupPortImportCart(cart, HH_SRAM_SIZE, img, HH_SRAM_SIZE, info, (uint32_t)info_n);
            if (copy >= 0) {
                snprintf(what, what_n, "%s%s, copy %d", kImportOrder[order], part, (int)copy);
                return 1;
            }
        }
    }
    return 0;
}

/* The whole file into a new buffer (NULL: missing, unreadable or not min..IMPORT_MAX_SIZE bytes; *why says
 * which, NULL for a missing file). */
static uint8_t *import_read(const char *name, size_t min, size_t *size, const char **why)
{
    struct stat st;
    uint8_t *p;
    FILE *f;
    *why = NULL;
    if (stat(port_save_path(name), &st) != 0) { if (errno != ENOENT) *why = strerror(errno); return NULL; }
    if (!S_ISREG(st.st_mode)) { *why = "not a file"; return NULL; }
    if (st.st_size < (off_t)min || st.st_size > (off_t)IMPORT_MAX_SIZE) { *why = "its size is no N64 save's"; return NULL; }
    p = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (p == NULL) { *why = "out of memory"; return NULL; }
    errno = 0;
    f = fopen(port_save_path(name), "rb");
    if (f == NULL || fread(p, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        *why = f == NULL ? strerror(errno) : "read error";
        if (f) fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *size = (size_t)st.st_size;
    return p;
}

/* <base><suffix><time>[-n] that does not exist yet (port_save_path names). */
static void import_free_name(char *out, size_t n, const char *base, const char *suffix, int stamp)
{
    char ts[32] = "";
    struct stat st;
    int i;
    if (stamp) {
        const time_t t = time(NULL);
        struct tm tm;
        if (localtime_r(&t, &tm) != NULL) strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    }
    snprintf(out, n, "%s%s%s", base, suffix, ts);
    for (i = 2; i < 100 && stat(port_save_path(out), &st) == 0; i++) snprintf(out, n, "%s%s%s-%d", base, suffix, ts, i);
}

/* port_save_path's buffer is static: both names copied first */
static int import_rename(const char *from, const char *to)
{
    char a[700], b[700];
    snprintf(a, sizeof a, "%s", port_save_path(from));
    snprintf(b, sizeof b, "%s", port_save_path(to));
    return rename(a, b);
}

static int import_has_ext(const char *name)
{
    static const char *const kExt[] = { ".sra", ".srm", ".sav" };
    const size_t n = strlen(name);
    size_t i;
    if (strncasecmp(name, "state_", 6) == 0) return 0; /* the save states (state_01.sav, state_quick.sav) */
    for (i = 0; i < sizeof kExt / sizeof kExt[0]; i++)
        if (n > 4 && strcasecmp(name + n - 4, kExt[i]) == 0) return 1;
    return 0;
}

/* img over sram.bin, the old one (when there is one) kept first as sram.bin.bak-<time>.
 * 0, or -1 (logged; sram.bin as it was). bak: the backup's name, or "" when there was no sram.bin. */
static int import_install(const uint8_t *img, char *bak, size_t bak_n)
{
    const char *why;
    size_t old_size = 0;
    uint8_t *old = import_read("sram.bin", 0, &old_size, &why);
    bak[0] = '\0';
    if (old == NULL && why != NULL) { /* there but unreadable: never replaced (sKeep's rule) */
        port_log_always("SSB64: save import: sram.bin cannot be read (%s): nothing replaced\n", why);
        return -1;
    }
    if (old != NULL) {
        import_free_name(bak, bak_n, "sram.bin", ".bak-", 1);
        if (file_write_durable(bak, old, old_size) != 0) {
            port_log_always("SSB64: save import: the old sram.bin could not be backed up: nothing replaced\n");
            free(old);
            bak[0] = '\0';
            return -1;
        }
        free(old);
    }
    if (file_write_durable("sram.bin", img, HH_SRAM_SIZE) != 0) {
        port_log_always("SSB64: save import: sram.bin could not be written: nothing replaced%s%s\n",
                        bak[0] ? "; the old one is also in " : "", bak);
        return -1;
    }
    return 0;
}

void hh_sram_import(void)
{
    static int sDone; /* the process's: hh/sram.o's plain statics are not in the save states */
    static uint8_t img[HH_SRAM_SIZE];
    char names[IMPORT_MAX_FILES][256], what[200], info[120], bak[300], dst[300];
    time_t mtimes[IMPORT_MAX_FILES];
    int ok[IMPORT_MAX_FILES];
    int count = 0, best = -1, i, r;
    const char *e = getenv("SSB64_SAVEIMPORT");
    struct dirent *de;
    DIR *d;
    if (sDone) return;
    sDone = 1;
    if (e && e[0] == '0') return;
    d = opendir(port_save_dir());
    if (d == NULL) return;
    while ((de = readdir(d)) != NULL && count < IMPORT_MAX_FILES) {
        struct stat st;
        if (!import_has_ext(de->d_name) || strlen(de->d_name) >= sizeof names[0]) continue;
        if (stat(port_save_path(de->d_name), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(names[count], sizeof names[0], "%s", de->d_name);
        mtimes[count] = st.st_mtime;
        ok[count] = 0;
        count++;
    }
    closedir(d);

    /* each dropped file: which reading passes; the newest that passes is installed */
    for (i = 0; i < count; i++) {
        const char *why;
        size_t size = 0;
        uint8_t *data = import_read(names[i], IMPORT_MIN_SIZE, &size, &why);
        if (data == NULL) {
            port_log_always("SSB64: save import: %s: not imported (%s); left as it is\n", names[i], why ? why : "?");
            continue;
        }
        r = import_try(data, size, img, what, sizeof what, info, sizeof info);
        free(data);
        if (r == 0) {
            port_log_always("SSB64: save import: %s: not imported: no valid Super Smash Bros. (USA) save in it in any byte order "
                            "(%u bytes; maybe a PAL or Japanese save, another game's, or a blank one); left as it is\n",
                            names[i], (unsigned)size);
            continue;
        }
        ok[i] = 1;
        if (best < 0 || mtimes[i] > mtimes[best] || (mtimes[i] == mtimes[best] && strcmp(names[i], names[best]) > 0)) best = i;
    }
    if (best >= 0) {
        const char *why;
        size_t size = 0;
        uint8_t *data = import_read(names[best], IMPORT_MIN_SIZE, &size, &why);
        r = data ? import_try(data, size, img, what, sizeof what, info, sizeof info) : 0;
        free(data);
        if (r == 0) {
            port_log_always("SSB64: save import: %s: changed while it was read; not imported\n", names[best]);
            return;
        }
        if (import_install(img, bak, sizeof bak) != 0) return;
        import_free_name(dst, sizeof dst, names[best], ".imported", 0);
        if (import_rename(names[best], dst) != 0)
            port_log_always("SSB64: save import: %s could not be renamed (%s): it will be imported again next start\n",
                            names[best], strerror(errno));
        port_log_always("SSB64: save import: %s: OK, %s%s%s; installed as sram.bin%s%s; the file renamed %s\n",
                        names[best], what, info[0] ? "; " : "", info,
                        bak[0] ? ", the old one kept as " : " (there was none)", bak, dst);
        for (i = 0; i < count; i++) {
            if (!ok[i] || i == best) continue;
            import_free_name(dst, sizeof dst, names[i], ".not-imported", 0);
            if (import_rename(names[i], dst) == 0)
                port_log_always("SSB64: save import: %s: also a valid save but older than %s: not used, renamed %s\n",
                                names[i], names[best], dst);
        }
        return;
    }

    /* no file dropped (or none valid): sram.bin itself, e.g. a .sra renamed to sram.bin as we first advised */
    {
        const char *why;
        size_t size = 0, j;
        uint8_t *data = import_read("sram.bin", IMPORT_MIN_SIZE, &size, &why);
        if (data == NULL) return; /* none (a first start), or unreadable: sram_load's own rules */
        r = import_try(data, size, img, what, sizeof what, info, sizeof info);
        if (r == 2) {
            port_log("SSB64: save import: sram.bin is a valid save (%s)\n", what);
        } else if (r == 1) {
            if (import_install(img, bak, sizeof bak) == 0)
                port_log_always("SSB64: save import: sram.bin: OK, it was a cartridge save, %s%s%s; converted to this port's format, "
                                "the original kept as %s\n", what, info[0] ? "; " : "", info, bak);
        } else {
            for (j = 0; j < size && data[j] == 0; j++) {}
            if (j < size) { /* the game formats it: a copy of the player's file first */
                import_free_name(bak, sizeof bak, "sram.bin", ".bak-", 1);
                if (file_write_durable(bak, data, size) == 0)
                    port_log_always("SSB64: save import: sram.bin is no valid Super Smash Bros. (USA) save in any byte order (%u bytes; "
                                    "maybe a PAL or Japanese save, or damaged): the game starts a new save; the file is kept as %s\n",
                                    (unsigned)size, bak);
            }
        }
        free(data);
    }
}

int hh_sram_read(uint32_t offset, void *dst, uint32_t size)
{
    sram_load();
    if (offset >= HH_SRAM_SIZE || size > HH_SRAM_SIZE - offset) return -1;
    memcpy(dst, sSram + offset, size);
    return 0;
}

/* A failed write (logged with its stage and reason by sram_flush) is shown: the game has already said
 * it saved, so the player sees GAME SAVE FAILED on the OS bar (the kit's caption; MK64 37c208f, DKR
 * 8938223, PM64 742bdb4). lbBackupWrite writes its two copies back to back: one caption for both. */
static void sram_failed_caption(void)
{
    static int sShown;
    static uint32_t sShownUs;
    const uint32_t now = port_time_us();
    if (sShown && now - sShownUs < 2000000u) return;
    sShown = 1;
    sShownUs = now;
    handheld_menu_notify("GAME SAVE FAILED");
}

int hh_sram_write(uint32_t offset, const void *src, uint32_t size)
{
    sram_load();
    if (offset >= HH_SRAM_SIZE || size > HH_SRAM_SIZE - offset) return -1;
    if (!sDirty && memcmp(sSram + offset, src, size) == 0) return 0;
    memcpy(sSram + offset, src, size);
    if (sram_flush() == 0) {
        if (sFails) port_log_always("SSB64: sram.bin written, after %d failed %s\n", sFails, sFails == 1 ? "try" : "tries");
        sDirty = 0;
        sFails = 0;
        return 0;
    }
    sDirty = 1;
    sFails++;
    sram_failed_caption();
    return -1;
}

/* statefix A (kit k1.8; the kit's savestate.c calls it after a load that happened, with the mix
 * lock held, before the texture fixup). sSram and the game's RAM copy of its save (gSCManagerBackupData,
 * which every game write writes whole, twice, from RAM) are in the state, so a LOAD gave back the save as
 * it was when the state was made: a record, an unlock or an option set since was gone in the game, and
 * the game's next write (a title screen, a character select, a match's VS record) put the old block back
 * over the newer sram.bin. Now:
 *  - sram.bin is the state's sSram (nothing written since the state): nothing changes, the load is exact;
 *  - it differs: sSram = sram.bin, and lbbackup.c's lbBackupPortReapply sets gSCManagerBackupData from it
 *    the way the game reads it at boot (its comment lists what it refreshes and what stays the state's).
 *    Every game write follows its change to the block at once, so the state never holds progress the
 *    card lacks: the card wins (a write that failed, GAME SAVE FAILED, never became the save either);
 *  - the state is from before the game first read sram.bin (sSramRead), sram.bin could not be read at
 *    the start (sKeep) or now, or both its copies fail the game's check: the state's save stands.
 * One line in the log either way. SSB64_SAVEREAPPLY=0: as before (the state's save stands; tests). */
void hh_game_save_reapply(void)
{
    static uint8_t card[HH_SRAM_SIZE]; /* the process's (hh/sram.o is outside the state's include list) */
    extern int lbBackupPortReapply(const uint8_t *sram, uint32_t size);
    const char *e = getenv("SSB64_SAVEREAPPLY");
    FILE *f;
    size_t n;
    int diff = 0, r, bad, err, i;
    if (e && e[0] == '0') {
        port_log_always("sram: SSB64_SAVEREAPPLY=0: the state's save stands\n");
        return;
    }
    if (!sSramRead) {
        port_log_always("sram: the state is from before the game read its save; the game reads sram.bin itself\n");
        return;
    }
    if (sKeep) {
        port_log_always("sram: sram.bin could not be read at the start; the state's save stands\n");
        return;
    }
    memset(card, 0, sizeof card);
    errno = 0;
    f = fopen(port_save_path("sram.bin"), "rb");
    if (f == NULL) {
        port_log_always("sram: sram.bin cannot be opened (%s); the state's save stands\n", strerror(errno));
        return;
    }
    n = fread(card, 1, sizeof card, f);
    bad = ferror(f);
    err = errno;
    fclose(f);
    if (bad) {
        port_log_always("sram: sram.bin read error after %u bytes (%s); the state's save stands\n", (unsigned)n, strerror(err));
        return;
    }
    for (i = 0; i < HH_SRAM_SIZE; i++) diff += card[i] != sSram[i];
    if (diff == 0) {
        port_log_always("sram: the state's save is the card's\n");
        return;
    }
    r = lbBackupPortReapply(card, HH_SRAM_SIZE);
    if (r < 0) {
        port_log_always("sram: sram.bin differs from the state's save (%d bytes) but both its copies fail the game's check; the state's save stands\n", diff);
        return;
    }
    memcpy(sSram, card, sizeof sSram);
    port_log_always("sram: the card's save is newer than the state's (%d bytes): put back%s%s%s\n", diff,
                    (r & 1) ? "; sound mode" : "", (r & 2) ? "; screen offsets" : "",
                    (r & 4) ? "; the state's unwritten error flags kept" : "");
}
