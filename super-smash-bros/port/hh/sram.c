/* sram.c -- see hh_sram.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static int sram_flush(void)
{
    char path[700], tmp[720], dir[700];
    int fd, err = 0;
    ssize_t n;
    const char *stage = NULL;
    const char *e = getenv("SSB64_DIRSYNC");
    const int sync_rename = !(e && e[0] == '0');
    extern void port_log_always(const char *fmt, ...);
    if (sKeep) { port_log_always("SSB64: sram.bin not written: it could not be read at the start\n"); return -1; }
    snprintf(path, sizeof path, "%s", port_save_path("sram.bin"));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { err = errno; port_log_always("SSB64: sram.bin write failed at open: %s\n", strerror(err)); return -1; }
    n = write(fd, sSram, sizeof sSram);
    if (n != (ssize_t)sizeof sSram) { stage = "write"; err = n < 0 ? errno : ENOSPC; }
    else if (fsync(fd) != 0) { stage = "fsync"; err = errno; }
    else if (rename(tmp, path) != 0) { stage = "rename"; err = errno; }
    if (stage != NULL) {
        close(fd);
        unlink(tmp);
        port_log_always("SSB64: sram.bin write failed at %s: %s\n", stage, strerror(err));
        return -1;
    }
    /* The new data is in place under its name from here on; a failed sync below only leaves it to the
     * kernel's writeback, as before. */
    if (sync_rename && fsync(fd) != 0) port_log_always("SSB64: sram.bin fsync after the rename failed: %s\n", strerror(errno));
    close(fd);
    if (sync_rename) {
        char *slash;
        snprintf(dir, sizeof dir, "%s", path);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
            fd = open(dir[0] ? dir : "/", O_RDONLY);
            if (fd < 0 || fsync(fd) != 0) port_log_always("SSB64: sram.bin folder fsync failed: %s\n", strerror(errno));
            if (fd >= 0) close(fd);
        }
    }
    return 0;
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
