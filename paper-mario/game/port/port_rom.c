/* The player's ROM, read like the cartridge (M2). The game streams from it by offset (map shapes and textures,
 * sprites, messages, audio); nothing is kept whole in memory (SPIKE.md Q2). A .v64 (16-bit swapped) or .n64
 * (32-bit swapped) dump is put back in .z64 order as it is read. The code segments the game DMAs are linked in
 * natively, so port_pi_read skips them (port_segments.c). */
#include "port.h"
#include "handheld_menu.h" /* the failed-save caption (a no-op off the handheld) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

static FILE *sRom;
static uint32_t sRomSize;
static int sRomOrder; /* 0 = z64 (big-endian), 1 = v64 (16-bit swapped), 2 = n64 (32-bit swapped) */

int port_rom_open(const char *path) {
    uint8_t h[0x40];
    sRom = fopen(path, "rb");
    if (!sRom) { port_log("rom: cannot open %s\n", path); return 0; }
    if (fread(h, 1, sizeof h, sRom) != sizeof h) { port_log("rom: %s is too short\n", path); return 0; }
    fseek(sRom, 0, SEEK_END);
    sRomSize = (uint32_t) ftell(sRom);
    if (h[0] == 0x80 && h[1] == 0x37) sRomOrder = 0;
    else if (h[0] == 0x37 && h[1] == 0x80) sRomOrder = 1;
    else if (h[0] == 0x40 && h[1] == 0x12) sRomOrder = 2;
    else { port_log("rom: %s is not an N64 ROM\n", path); return 0; }
    uint8_t name[21];
    port_rom_read(0x20, name, 20);
    name[20] = 0;
    uint8_t cc[4];
    port_rom_read(0x3B, cc, 4); /* media, game code, region: "NMQE" = Paper Mario (USA) */
    if (memcmp(name, "PAPER MARIO", 11) != 0 || memcmp(cc, "NMQE", 4) != 0) {
        port_log("rom: %s is \"%s\" (%c%c%c%c), not Paper Mario (USA)\n", path, name, cc[0], cc[1], cc[2], cc[3]);
        return 0;
    }
    port_log("rom: %s, %u bytes, %s order\n", path, sRomSize, sRomOrder == 0 ? "z64" : sRomOrder == 1 ? "v64" : "n64");
    return 1;
}

uint32_t port_rom_size(void) { return sRomSize; }

void port_rom_read(uint32_t rom_addr, void *dst, uint32_t size) {
    if (!sRom || size == 0) return;
    if (rom_addr >= sRomSize || size > sRomSize - rom_addr) {
        port_log("rom: read of %u bytes at 0x%08X is past the end (0x%X)\n", size, rom_addr, sRomSize);
        abort();
    }
    if (sRomOrder == 0) {
        fseek(sRom, (long) rom_addr, SEEK_SET);
        if (fread(dst, 1, size, sRom) != size) { port_log("rom: short read at 0x%08X\n", rom_addr); abort(); }
        return;
    }
    /* swapped dumps: read the aligned words around the range and put each back in order */
    const uint32_t a0 = rom_addr & ~3u, a1 = (rom_addr + size + 3) & ~3u;
    uint8_t buf[4096];
    uint8_t *out = dst;
    for (uint32_t a = a0; a < a1; a += sizeof buf) {
        const uint32_t n = a1 - a < sizeof buf ? a1 - a : (uint32_t) sizeof buf;
        fseek(sRom, (long) a, SEEK_SET);
        if (fread(buf, 1, n, sRom) != n) { port_log("rom: short read at 0x%08X\n", a); abort(); }
        for (uint32_t i = 0; i < n; i += 4) {
            uint8_t t;
            if (sRomOrder == 1) { t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; t = buf[i + 2]; buf[i + 2] = buf[i + 3]; buf[i + 3] = t; }
            else { t = buf[i]; buf[i] = buf[i + 3]; buf[i + 3] = t; t = buf[i + 1]; buf[i + 1] = buf[i + 2]; buf[i + 2] = t; }
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t at = a + i;
            if (at >= rom_addr && at < rom_addr + size) out[at - rom_addr] = buf[i];
        }
    }
}

void port_pi_read(uint32_t rom_addr, void *dst, uint32_t size) {
    int seg;
    /* data the N64 links at address 0 and reads as <seg>_ROM_START + symbol (imgfx_data, charset, entity animations)
     * is linked in natively with <seg>_ROM_START = 0 (gen_build.py): the "ROM address" is the data itself */
    if (rom_addr >= 0x10000000u) { memmove(dst, (const void *) (uintptr_t) rom_addr, size); return; }
    if (port_code_segment(rom_addr, rom_addr + size, &seg)) return; /* linked in: see port_segments.c */
    port_rom_read(rom_addr, dst, size);
}

/* ---------------------------------------------------------------- the save chip (128 KB flash) as a file */
/* PM64_SAVE=<file> (default pm64.sav in the save dir, port_save_dir). Erased flash reads 0xFF. The file
 * holds the game's save structures in the host's byte order (not an N64 .fla). The game's erases and page writes
 * change the image in memory (the only thing the game reads); once a frame, if any did, the whole image goes to
 * <save>.tmp, is synced, and is renamed over the save (port_flash_commit; the lead's L4, 28 Sep). It was written in
 * place a page at a time: a power cut mid-save could leave it torn or cut short, and the card's FAT
 * had already been damaged once. A cut now leaves the old save or the new one. */
#define FLASH_SIZE (128u * 1024u)
static uint8_t sFlash[FLASH_SIZE];
static const char *sFlashPath;
static int sFlashOpen;
/* A save that cannot be written is shown and tried again (2026-10-01 sweep): it was only logged and then forgotten (not
 * even the exit tried again), so on a full or read-only card every save was lost while the game said it had saved.
 * sFlashDirty: the image differs from the file (the exit tries once more); sFlashTry: the game wrote
 * since the last try (each in-game save is tried once, at the end of its frame); sFlashKeep: the file was there but
 * could not be read whole, so an image built on part of it must never replace it (it would drop the other slots). */
static int sFlashDirty, sFlashTry, sFlashFails, sFlashKeep;
static void flash_commit_exit(void);

void port_flash_open(void) {
    if (sFlashOpen) return;
    sFlashOpen = 1;
    sFlashPath = getenv("PM64_SAVE");
    if (!sFlashPath) sFlashPath = port_save_path("pm64.sav");
    memset(sFlash, 0xFF, sizeof sFlash);
    { char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", sFlashPath);
      if (remove(tmp) == 0) port_log("save: dropped %s (a save cut short; the save before it stands)\n", tmp); }
    errno = 0;
    FILE *f = fopen(sFlashPath, "rb");
    if (f) {
        const size_t n = fread(sFlash, 1, sizeof sFlash, f);
        const int bad = ferror(f), err = errno;
        fclose(f);
        if (bad) {
            sFlashKeep = 1;
            port_log("save: %s: read error after %u bytes (%s): kept as it is, no save replaces it this session\n",
                     sFlashPath, (unsigned) n, strerror(err));
        } else {
            port_log("save: %s (%u bytes)\n", sFlashPath, (unsigned) n);
        }
    } else if (errno == ENOENT) {
        port_log("save: %s is new\n", sFlashPath);
    } else {
        sFlashKeep = 1;
        port_log("save: %s cannot be opened (%s): no save replaces it this session\n", sFlashPath, strerror(errno));
    }
    atexit(flash_commit_exit); /* a save the last frame made, or one that failed, tried once more on the way out */
}

static void flash_store(uint32_t offset, uint32_t len) { (void) offset, (void) len; sFlashDirty = 1; sFlashTry = 1; }

/* the whole image to <save>.tmp, synced, renamed over the save, the file synced again, the directory synced; 1 =
 * written. The second fsync (super-smash-bros cd5128c, the 2026-10-01 sweep): on the card's vfat (Linux 4.14 vfat_rename)
 * a rename over an existing file moves the new file into the OLD file's directory slot and only marks its inode dirty,
 * so the slot's new start cluster and size reach the card at that inode's writeback (~30 s) or its own fsync. The
 * folder fsync writes the removed .tmp entry and the FAT with the old save's clusters freed, not that slot: a hard
 * power cut in those seconds left pm64.sav pointing at freed clusters, the save lost and the FAT inconsistent.
 * PM64_SAVEKILL=1 (test): the process dies halfway through the temporary file, as a power cut would; the old save
 * must survive. PM64_SAVEFAIL=1 (test): every write fails, as on a read-only card. */
static int flash_write(void) {
    if (sFlashKeep) { port_log("save: not written: %s could not be read at the start\n", sFlashPath); return 0; }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", sFlashPath);
    const uint32_t t0 = port_time_us();
    /* each stage says what failed and why (the kit's 88210e7, X4): a full card and a read-only one look alike */
    const char *fail = getenv("PM64_SAVEFAIL");
    FILE *f = fail && fail[0] == '1' ? (errno = EROFS, NULL) : fopen(tmp, "wb");
    if (!f) { port_log("save: open %s: %s; the save is as it was\n", tmp, strerror(errno)); return 0; }
    const char *kill = getenv("PM64_SAVEKILL");
    if (kill && kill[0] == '1') { fwrite(sFlash, 1, FLASH_SIZE / 2, f); fflush(f); _exit(9); }
    const char *stage = NULL;
    errno = 0;
    if (fwrite(sFlash, 1, FLASH_SIZE, f) != FLASH_SIZE) stage = "write";
    else if (fflush(f) != 0) stage = "flush";
    else if (fsync(fileno(f)) != 0) stage = "fsync";
    if (stage) {
        const int err = errno;
        fclose(f);
        port_log("save: %s %s: %s; the save is as it was\n", stage, tmp, strerror(err ? err : ENOSPC));
        remove(tmp);
        return 0;
    }
    const uint32_t t1 = port_time_us();
    if (rename(tmp, sFlashPath) != 0) {
        port_log("save: rename to %s: %s; the save is as it was\n", sFlashPath, strerror(errno));
        fclose(f);
        remove(tmp);
        return 0;
    }
    /* still open: its directory slot is now the save's (the comment above); a failure here is only logged, the save
     * is complete and reaches the card at the next writeback */
    if (fsync(fileno(f)) != 0) port_log("save: fsync after the rename: %s (the save is written)\n", strerror(errno));
    fclose(f);
    char dir[600];
    snprintf(dir, sizeof dir, "%s", sFlashPath);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        const int d = open(dir[0] ? dir : "/", O_RDONLY);
        if (d < 0 || fsync(d) != 0) port_log("save: folder fsync %s: %s (the save is written)\n", dir, strerror(errno));
        if (d >= 0) close(d);
    }
    port_log("save: written (%u ms: file %u, rename + syncs %u)\n", (unsigned) ((port_time_us() - t0) / 1000),
             (unsigned) ((t1 - t0) / 1000), (unsigned) ((port_time_us() - t1) / 1000));
    return 1;
}

static void flash_commit(int at_exit) {
    if (!sFlashPath || !sFlashDirty || (!sFlashTry && !at_exit)) return;
    sFlashTry = 0;
    if (flash_write()) {
        if (sFlashFails) port_log("save: written, after %d failed %s\n", sFlashFails, sFlashFails == 1 ? "try" : "tries");
        sFlashDirty = 0;
        sFlashFails = 0;
        return;
    }
    sFlashFails++;
    /* the game has already said it saved: the player must hear otherwise (the kit's caption; nothing off the handheld) */
    if (!at_exit) handheld_menu_notify("GAME SAVE FAILED");
}
/* Once a frame (port_host.c): the frame's in-game save, whole, as one replace */
void port_flash_commit(void) { flash_commit(0); }
static void flash_commit_exit(void) { flash_commit(1); }

void port_flash_read(uint32_t offset, void *dst, uint32_t len) {
    port_flash_open();
    if (offset >= FLASH_SIZE) { memset(dst, 0xFF, len); return; }
    if (len > FLASH_SIZE - offset) len = FLASH_SIZE - offset;
    memcpy(dst, sFlash + offset, len);
}

void port_flash_write(uint32_t offset, const void *src, uint32_t len) {
    port_flash_open();
    if (offset >= FLASH_SIZE) return;
    if (len > FLASH_SIZE - offset) len = FLASH_SIZE - offset;
    for (uint32_t i = 0; i < len; i++) sFlash[offset + i] &= ((const uint8_t *) src)[i]; /* flash bits only clear */
    flash_store(offset, len);
}

void port_flash_erase(uint32_t offset, uint32_t len) {
    port_flash_open();
    if (offset >= FLASH_SIZE) return;
    if (len > FLASH_SIZE - offset) len = FLASH_SIZE - offset;
    memset(sFlash + offset, 0xFF, len);
    flash_store(offset, len);
}
