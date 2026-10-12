/* Save states and the asset region (the kit's menu, kit/platform/savestate.c). Host side.
 *
 * The kit saves .data and .bss (hh_game.h). The game's own data -- every decomp file's initialised data, 10.9 MB --
 * lives in the asset region after .bss, rebuilt from the player's ROM at first boot, and almost all of it is writable
 * (the decomp rarely says const). The game changes very little of it: a few pages a map (a 600-frame run in each of
 * six maps and a battle changed 44-136 bytes on 6-11 pages). So the region stays out of the kit's range, and its
 * changed pages ride inside the state:
 *  - once the region is filled, each 4 KB page's 64-bit hash is kept (2677 pages, ~20 KB);
 *  - before a save, port_state_stage() copies each page whose hash differs now into sStage, a .bss array the save
 *    then carries (a sparse state leaves its unused, all-zero pages out);
 *  - after a load (gfx_savestate_fixup), port_state_loaded() writes the staged pages back and returns every other
 *    page that differs from its pristine hash to the pristine bytes (assets.bin on the card, a copy on the desktop,
 *    where the region is linked in).
 * A first version tracked writes with write protection and a fault handler; under qemu a crash elsewhere then looped
 * on its instruction instead of reaching the crash report, so it gave way to this, which runs the same everywhere.
 * The staging array holds every page of the region (STAGE_PAGES = MAX_PAGES), so a state is never short of room: up to
 * 2026-10-09 it held 512, and past that the save went on, the menu said SAVED and a load returned the rest to pristine.
 * Its unused pages are never written, so they cost no memory, the state file nothing, and a load nothing (the kit's
 * sparse load leaves zero pages alone). A save this code cannot make whole (a region too large for the array, no
 * pristine source) is refused through the kit's hh_game_state_ready: SAVE FAILED, the old state kept.
 * PM64_STATE_SAVE=<frame>,<slot> / PM64_STATE_LOAD=<frame>,<slot> (tests): a save or a load at that game frame.
 * PM64_STATE_DIRTY=<n> (test, with either): the save stages n more changed pages, spread over the region, that the
 * game never sees (flipped just before the stage and back right after; a load flips them back after the restore), and
 * both log the whole region's sum so a round trip past the old 512-page limit can be checked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "port.h"
#include "savestate.h"

#define PG 4096u
#define MAX_PAGES 3072u        /* a 12 MB region (the game's is 2677 pages, 10.9 MB) */
#define STAGE_PAGES MAX_PAGES /* every page of the region fits: a save is never short of room */

extern uint8_t __assets_start[], __assets_end[];
extern const uint32_t port_build_stamp;

/* In the state: the region's changed pages as they were at the save. */
static uint8_t sStage[STAGE_PAGES][PG] __attribute__((aligned(4096)));
static uint32_t sStagePage[STAGE_PAGES];
static uint32_t sStageN;

/* This run's own: the pristine pages' hashes and where their bytes come from. */
static uint64_t sHash[MAX_PAGES] SAVESTATE_EXCLUDE;
static uint8_t sStagedNow[MAX_PAGES] SAVESTATE_EXCLUDE;
static uint32_t sPages SAVESTATE_EXCLUDE, sStageHigh SAVESTATE_EXCLUDE;
static int sReady SAVESTATE_EXCLUDE;
static int sNoRegion SAVESTATE_EXCLUDE; /* the linked region is empty: nothing to stage */
static uint32_t sOver SAVESTATE_EXCLUDE; /* pages the last stage had no room for (0 since STAGE_PAGES = MAX_PAGES) */
static uint32_t sDirtyN SAVESTATE_EXCLUDE;
static int sPristineFd SAVESTATE_EXCLUDE = -1;
static long sPristineOff SAVESTATE_EXCLUDE;
static uint8_t *sPristineCopy SAVESTATE_EXCLUDE;
static uint32_t sSaveAt SAVESTATE_EXCLUDE, sLoadAt SAVESTATE_EXCLUDE;
static int sSaveSlot SAVESTATE_EXCLUDE, sLoadSlot SAVESTATE_EXCLUDE;

static uint8_t *page_at(uint32_t p) { return __assets_start + (size_t) p * PG; }

/* Two independent 32-bit word hashes (32-bit multiplies: the handheld's Cortex-A7), 64 bits together */
static uint64_t page_hash(const uint8_t *page) {
    const uint32_t *w = (const uint32_t *) page;
    uint32_t a = 0x811C9DC5u, b = 0x9E3779B9u;
    for (uint32_t i = 0; i < PG / 4; i++) {
        a = (a ^ w[i]) * 0x01000193u;
        b = (b + w[i] * 0x85EBCA6Bu);
        b = (b << 13 | b >> 19) * 5u + 0xE6546B64u;
    }
    return (uint64_t) a << 32 | b;
}

static int pristine(uint32_t p, uint8_t *out) {
    if (sPristineCopy) { memcpy(out, sPristineCopy + (size_t) p * PG, PG); return 1; }
    return sPristineFd >= 0 && pread(sPristineFd, out, PG, (off_t) (sPristineOff + (long) p * (long) PG)) == (ssize_t) PG;
}

/* assets.bin's header (port_assets_hh.c): the region verbatim at data_off, for this build only */
struct StateAssetHeader { char magic[8]; uint32_t version, region_addr, region_size, reloc_count, sym_count, names_size, build_id, data_crc, data_off; };

static int open_cache(uint32_t size) {
#if !defined(PM64_ASSETS_LINKED)
    /* Only a cache this run loaded (crc checked) or wrote: when a bad one failed its crc and the rebuilt region could
     * not be written over it (a full card), the old file is still there with a header that passes the checks below,
     * and a load would have copied its bad pages into the region. The copy in memory serves then. */
    if (!port_assets_cache_current) { port_log("state: assets.bin is not this run's region: pristine pages from a copy\n"); return 0; }
    const int fd = open(port_save_path("assets.bin"), O_RDONLY);
    struct StateAssetHeader h;
    if (fd < 0) return 0;
    if (pread(fd, &h, sizeof h, 0) == (ssize_t) sizeof h && memcmp(h.magic, "PM64ASST", 8) == 0 && h.version == 1 &&
        h.region_addr == (uint32_t) (uintptr_t) __assets_start && h.region_size == size && h.build_id == port_build_stamp &&
        h.data_crc != 0 && h.data_off != 0) {
        sPristineFd = fd;
        sPristineOff = (long) h.data_off;
        return 1;
    }
    close(fd);
#endif
    (void) size;
    return 0;
}

/* Once the region is filled (port_host.c, after port_assets_load): the pristine source and the pages' hashes. */
void port_state_init(void) {
    const uint32_t size = (uint32_t) (__assets_end - __assets_start), t0 = port_time_us();
    const char *e = getenv("PM64_STATE_SAVE");
    if (e && e[0]) { sSaveAt = (uint32_t) strtoul(e, (char **) &e, 10); sSaveSlot = *e == ',' ? atoi(e + 1) : 1; }
    e = getenv("PM64_STATE_LOAD");
    if (e && e[0]) { sLoadAt = (uint32_t) strtoul(e, (char **) &e, 10); sLoadSlot = *e == ',' ? atoi(e + 1) : 1; }
    e = getenv("PM64_STATE_DIRTY");
    if (e && e[0]) sDirtyN = (uint32_t) strtoul(e, NULL, 10);
    sPages = size / PG;
    if (size == 0) { sNoRegion = 1; port_log("state: no asset region\n"); return; }
    if (size % PG != 0 || sPages > MAX_PAGES) {
        port_log("state: the asset region (%u bytes) does not fit the staging array (%u pages): save states are off\n",
                 size, (unsigned) MAX_PAGES);
        return;
    }
    if (!open_cache(size)) {
        sPristineCopy = (uint8_t *) malloc(size);
        if (!sPristineCopy) { port_log("state: no pristine copy of the region: save states are off\n"); return; }
        memcpy(sPristineCopy, __assets_start, size);
    }
    for (uint32_t p = 0; p < sPages; p++) sHash[p] = page_hash(page_at(p));
    sReady = 1;
    port_log("state: region %u pages hashed (%u ms), pristine from %s\n", (unsigned) sPages,
             (unsigned) ((port_time_us() - t0) / 1000), sPristineCopy ? "a copy" : "assets.bin");
}

/* The kit asks before every save (savestate.c save_begin): 0 = the state would not be whole; why says so in the log,
 * the menu shows SAVE FAILED and the old state stays. */
int hh_game_state_ready(char *why, size_t n) {
    if (sNoRegion) return 1;
    if (!sReady) { snprintf(why, n, "the asset region cannot be staged"); return 0; }
    if (sOver) { snprintf(why, n, "%u region pages did not fit the staging array", (unsigned) sOver); return 0; }
    return 1;
}

/* PM64_STATE_DIRTY (test): one byte flipped in each of n pages spread over the region, and the region's sum */
static void dirty_flip(void) {
    const uint32_t n = sDirtyN < sPages ? sDirtyN : sPages;
    for (uint32_t i = 0; i < n; i++) page_at((uint32_t) ((uint64_t) i * sPages / n))[(i * 61u) % PG] ^= 0xA5u;
}
static uint64_t region_sum(void) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t p = 0; p < sPages; p++) h = (h ^ page_hash(page_at(p))) * 0x100000001B3ull;
    return h;
}

/* Right before a save runs (the menu has closed, a power-off, the test switch): the pages that differ from the
 * pristine region go to sStage. Unused staging pages are zeroed, so the sparse state leaves them out. */
static void stage(void) {
    if (!sReady) return;
    const uint32_t t0 = port_time_us();
    uint32_t n = 0, over = 0;
    for (uint32_t p = 0; p < sPages; p++) {
        if (page_hash(page_at(p)) == sHash[p]) continue;
        if (n == STAGE_PAGES) { over++; continue; }
        memcpy(sStage[n], page_at(p), PG);
        sStagePage[n++] = p;
    }
    if (sStageHigh > n) {
        memset(sStage[n], 0, (size_t) (sStageHigh - n) * PG);
        memset(&sStagePage[n], 0, (size_t) (sStageHigh - n) * sizeof sStagePage[0]);
    }
    sStageN = n;
    sStageHigh = n;
    sOver = over;
    port_log("state: %u region pages staged (%u ms)%s\n", (unsigned) n, (unsigned) ((port_time_us() - t0) / 1000),
             over ? ", TOO MANY: the save is refused" : "");
}
void port_state_stage(void) { stage(); }

/* After a load (gfx_savestate_fixup): the staged pages back into the region, every other page that differs from its
 * pristine hash back to the pristine bytes. */
void port_state_loaded(void) {
    if (!sReady) return;
    const uint32_t t0 = port_time_us();
    uint32_t n = sStageN, back = 0, bad = 0;
    if (n > STAGE_PAGES) n = 0; /* not a state this code wrote */
    memset(sStagedNow, 0, sPages);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t p = sStagePage[i];
        if (p >= sPages) { bad++; continue; }
        memcpy(page_at(p), sStage[i], PG);
        sStagedNow[p] = 1;
    }
    for (uint32_t p = 0; p < sPages; p++) {
        if (sStagedNow[p] || page_hash(page_at(p)) == sHash[p]) continue;
        if (pristine(p, page_at(p))) back++;
        else bad++;
    }
    if (sStageHigh < n) sStageHigh = n;
    if (sDirtyN && sLoadAt) {
        port_log("state: region sum %016llx after the load\n", (unsigned long long) region_sum());
        dirty_flip();
    }
    port_log("state: %u region pages restored, %u back to pristine (%u ms)%s\n", (unsigned) n, (unsigned) back,
             (unsigned) ((port_time_us() - t0) / 1000), bad ? ", SOME FAILED" : "");
}

/* Once per game frame before savestate_check (port_host.c): the test switches */
void port_state_frame(uint32_t frame) {
    if (sSaveAt && frame == sSaveAt) {
        if (sDirtyN && sReady) {
            dirty_flip();
            port_log("state: region sum %016llx at the save (%u pages flipped)\n", (unsigned long long) region_sum(),
                     (unsigned) sDirtyN);
            stage();
            dirty_flip();
        } else stage();
        savestate_request_save(sSaveSlot);
    }
    if (sLoadAt && frame == sLoadAt) savestate_request_load(sLoadSlot);
}
