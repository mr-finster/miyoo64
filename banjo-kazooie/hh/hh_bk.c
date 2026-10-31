/* hh_bk.c: what Banjo-Kazooie's link and hand-written assembly gave the game on the N64.
 *
 * - The fixed-address buffers (the decomp's manual_syms.us.v10.txt) at their N64 addresses, inside the 4 MB of N64
 *   DRAM mapped at 0x80000000 (hh_boot.c): the game heap, the depth buffer / overlay scratch, the frame buffers, the
 *   inflate tables. Code that walks the heap by address (memory.c's D_8023DA00 = its last block) stays exact.
 * - The ROM layout symbols (decompressed.us.v10.yaml): assets and sound banks are read from the player's ROM at their
 *   cartridge offsets. Code segments are compiled in, so their addresses are 0.
 * - The level overlays: compiled in too; loading one puts its .data and .bss back as the N64's overlay manager does
 *   (hh/banjo.ld collects each overlay's variables).
 * - bkmemcpy64 / bkmemset64 / bkGetSR / func_8025C29C (hand-written MIPS in core1), in C. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hh_os.h"
#include "savestate.h"

/* ------------------------------------------------------------------------------------ */
/* fixed addresses                                                                      */

__asm__(".globl D_8000E800\n.set D_8000E800, 0x8000E800\n"
        ".globl gHeapBase\n.set gHeapBase, 0x8002D500\n"
        ".globl D_8023DA00\n.set D_8023DA00, 0x8023DA00\n"
        ".globl gFramebuffers\n.set gFramebuffers, 0x803A5D00\n"
        ".globl gGlobalHuffTable\n.set gGlobalHuffTable, 0x803FBE00\n"
        ".globl gChecksumsCore1\n.set gChecksumsCore1, 0x803FFE00\n"
        /* cartridge offsets (US 1.0) */
        ".globl boot_bk_boot_ROM_START\n.set boot_bk_boot_ROM_START, 0x1050\n"
        ".globl boot_bk_boot_ROM_END\n.set boot_bk_boot_ROM_END, 0x5E70\n"
        ".globl crc_ROM_START\n.set crc_ROM_START, 0x5E70\n"
        ".globl assets_ROM_START\n.set assets_ROM_START, 0x5E90\n"
        ".globl soundfont1ctl_ROM_START\n.set soundfont1ctl_ROM_START, 0xD846C0\n"
        ".globl soundfont1ctl_ROM_END\n.set soundfont1ctl_ROM_END, 0xD954B0\n"
        ".globl soundfont1tbl_ROM_START\n.set soundfont1tbl_ROM_START, 0xD954B0\n"
        ".globl soundfont2ctl_ROM_START\n.set soundfont2ctl_ROM_START, 0xEA3EB0\n"
        ".globl soundfont2ctl_ROM_END\n.set soundfont2ctl_ROM_END, 0xEADE60\n"
        ".globl soundfont2tbl_ROM_START\n.set soundfont2tbl_ROM_START, 0xEADE60\n");

/* the segments' layout symbols the overlay table names (overlaymanager.c): unused natively */
#define HH_SEG0(seg)                                                                                                     \
    __asm__(".globl " #seg "_VRAM\n.set " #seg "_VRAM, 0\n.globl " #seg "_VRAM_END\n.set " #seg "_VRAM_END, 0\n"       \
            ".globl " #seg "_ROM_START\n.set " #seg "_ROM_START, 0\n.globl " #seg "_ROM_END\n.set " #seg "_ROM_END, 0\n" \
            ".globl " #seg "_TEXT_START\n.set " #seg "_TEXT_START, 0\n.globl " #seg "_TEXT_END\n.set " #seg              \
            "_TEXT_END, 0\n.globl " #seg "_DATA_START\n.set " #seg "_DATA_START, 0\n.globl " #seg                        \
            "_RODATA_END\n.set " #seg "_RODATA_END, 0\n.globl " #seg "_BSS_START\n.set " #seg "_BSS_START, 0\n"          \
            ".globl " #seg "_BSS_END\n.set " #seg "_BSS_END, 0\n")
HH_SEG0(core2);
HH_SEG0(emptyLvl);
HH_SEG0(CC);
HH_SEG0(MMM);
HH_SEG0(GV);
HH_SEG0(TTC);
HH_SEG0(MM);
HH_SEG0(BGS);
HH_SEG0(RBB);
HH_SEG0(FP);
HH_SEG0(CCW);
HH_SEG0(SM);
HH_SEG0(cutscenes);
HH_SEG0(lair);
HH_SEG0(fight);
__asm__(".globl core1_BSS_START\n.set core1_BSS_START, 0\n.globl core1_BSS_END\n.set core1_BSS_END, 0\n"
        ".globl core1_VRAM\n.set core1_VRAM, 0\n");

/* the microcodes the tasks name (the renderer and the mixer stand in for them) */
long long int n_aspMainTextStart[1], n_aspMainTextEnd[1], n_aspMainDataStart[1], n_aspMainDataEnd[1];
long long int gspF3DEX_fifoTextStart[1], gspF3DEX_fifoTextEnd[1], gspF3DEX_fifoDataStart[1], gspF3DEX_fifoDataEnd[1];
long long int gspL3DEX_fifoTextStart[1], gspL3DEX_fifoTextEnd[1], gspL3DEX_fifoDataStart[1], gspL3DEX_fifoDataEnd[1];

s32 osViClock = 48681812; /* VI_NTSC_CLOCK; code_1D00.c sets it for PAL/MPAL */

void rmonPrintf(const char *fmt, ...) { (void)fmt; }

/* ------------------------------------------------------------------------------------ */
/* core1's hand-written assembly                                                        */

/* 8-byte block copy / fill (at least one block, as the MIPS loop) */
void bkmemcpy64(void *dest, void *src, s32 size)
{
    memmove(dest, src, (size_t)(((u32)size >> 3) ? ((u32)size >> 3) : 1) * 8);
}

void bkmemset64(void *dest, s32 value, s32 size)
{
    u32 n = ((u32)size >> 3) ? ((u32)size >> 3) : 1, i;
    /* sd of the sign-extended register: the high word then the low one, in the N64's byte order; the game only
     * fills with 0 */
    u32 *d = dest;
    for (i = 0; i < n; i++) {
        d[2 * i] = value < 0 ? 0xFFFFFFFFu : 0;
        d[2 * i + 1] = (u32)value;
    }
}

/* mfc0 $12: interrupts on, the pre-NMI line (IBIT5) unmasked: no reset pending */
u32 bkGetSR(void) { return 0xFF01; }

/* glcrc.c's seed step (mips3: 64-bit shifts):
 *   a2 = (s << 63) >> 31 | (s << 31) >> 32;  a2 ^= (s << 44) >> 32;  s = a2 ^ ((a2 >> 20) & 0xFFF);  return (s32) s */
s32 func_8025C29C(u64 *seed)
{
    u64 s = *seed, a2, a1, a3;
    a2 = (s << 63) >> 31;
    a1 = (s << 31) >> 32;
    a3 = (s << 44) >> 32;
    a2 = (a2 | a1) ^ a3;
    a3 = ((a2 >> 20) & 0xFFF) ^ a2;
    *seed = a3;
    return (s32)(u32)a3;
}

/* ------------------------------------------------------------------------------------ */
/* overlays                                                                             */

#define HH_OVL(o) extern u8 hh_ovl_##o##_data_start[], hh_ovl_##o##_data_end[], hh_ovl_##o##_bss_start[], hh_ovl_##o##_bss_end[];
HH_OVL(core2) HH_OVL(emptyLvl) HH_OVL(CC) HH_OVL(MMM) HH_OVL(GV) HH_OVL(TTC) HH_OVL(MM) HH_OVL(BGS) HH_OVL(RBB)
HH_OVL(FP) HH_OVL(CCW) HH_OVL(SM) HH_OVL(cutscenes) HH_OVL(lair) HH_OVL(fight)
#undef HH_OVL

typedef struct {
    const char *name;
    u8 *data, *data_end, *bss, *bss_end;
    u8 *image; /* .data as linked (the ROM's), kept from the start */
} HhOvl;

#define HH_OVL(o) { #o, hh_ovl_##o##_data_start, hh_ovl_##o##_data_end, hh_ovl_##o##_bss_start, hh_ovl_##o##_bss_end, NULL },
/* overlaymanager.c's order: the overlay ids */
static HhOvl sOvl[] SAVESTATE_EXCLUDE = { HH_OVL(core2) HH_OVL(emptyLvl) HH_OVL(CC) HH_OVL(MMM) HH_OVL(GV) HH_OVL(TTC)
                                          HH_OVL(MM) HH_OVL(BGS) HH_OVL(RBB) HH_OVL(FP) HH_OVL(CCW) HH_OVL(SM)
                                          HH_OVL(cutscenes) HH_OVL(lair) HH_OVL(fight) };
#undef HH_OVL

/* at the start, before any game code: each overlay's .data as linked */
void hh_ovl_init(void)
{
    unsigned i;
    for (i = 0; i < sizeof(sOvl) / sizeof(sOvl[0]); i++) {
        size_t n = (size_t)(sOvl[i].data_end - sOvl[i].data);
        sOvl[i].image = malloc(n ? n : 1);
        memcpy(sOvl[i].image, sOvl[i].data, n);
    }
}

/* overlay.c's overlay_load (its N64 version unpacks the code and data from the ROM): the overlay's .data back to its
 * linked values and its .bss to zero, as the N64 leaves them after a load. */
void overlay_load(s32 id, u8 *ram_start, u8 *ram_end, u32 rom_start, u32 rom_end, u8 *code_start, u8 *code_end,
                  u8 *data_start, u8 *data_end, u8 *bss_start, u8 *bss_end)
{
    HhOvl *o;
    (void)ram_start, (void)ram_end, (void)rom_start, (void)rom_end, (void)code_start, (void)code_end;
    (void)data_start, (void)data_end, (void)bss_start, (void)bss_end;
    if (id < 0 || id >= (s32)(sizeof(sOvl) / sizeof(sOvl[0])))
        hh_fatal("overlay_load: overlay %d", (int)id);
    o = &sOvl[id];
    memcpy(o->data, o->image, (size_t)(o->data_end - o->data));
    memset(o->bss, 0, (size_t)(o->bss_end - o->bss));
    fprintf(stderr, "overlay: %s loaded (data %u, bss %u bytes)\n", o->name, (unsigned)(o->data_end - o->data),
            (unsigned)(o->bss_end - o->bss));
}

/* the map in play, for the log lines */
s32 hh_bk_map(void)
{
    extern s32 gsworld_getMap(void);
    return gsworld_getMap();
}

/* BK_HEAPCHECK=1 (tests): the game heap's block chain checked once a frame (memory.c's HeapHeader: prev, next, ...);
 * the first break is logged with the frame, to find writes past an allocation. */
void hh_heap_check(void)
{
    { /* BK_MEMDUMP=<hex addr>:<hex len>:<vi> (tests): N64 memory to <BK_SAVE_DIR>/mem.bin at that frame */
        static int done;
        extern unsigned int hh_frames;
        const char *e = getenv("BK_MEMDUMP");
        if (e && !done) {
            unsigned long a = strtoul(e, NULL, 16), n = 0, f = 0;
            const char *c = strchr(e, ':');
            if (c) { n = strtoul(c + 1, NULL, 16); c = strchr(c + 1, ':'); }
            if (c) f = strtoul(c + 1, NULL, 10);
            extern unsigned int hh_vicount;
            if (hh_vicount >= f) {
                extern const char *port_save_dir(void);
                char path[600];
                FILE *o;
                snprintf(path, sizeof(path), "%s/mem.bin", port_save_dir());
                o = fopen(path, "wb");
                if (o) { fwrite((void *)(uintptr_t)a, 1, n, o); fclose(o); }
                done = 1;
            }
        }
    }
    { /* BK_CAMLOG=<n> (tests): the camera every n frames */
        static long every = -1;
        extern unsigned int hh_frames, hh_vicount;
        extern f32 sViewportPosition[3], sViewportRotation[3];
        if (every < 0) every = getenv("BK_CAMLOG") ? atol(getenv("BK_CAMLOG")) : 0;
        if (every > 0 && hh_frames % every == 0)
            fprintf(stderr, "CAM vi %u map %x pos %.1f %.1f %.1f rot %.1f %.1f\n", hh_vicount, (unsigned)hh_bk_map(),
                    sViewportPosition[0], sViewportPosition[1], sViewportPosition[2], sViewportRotation[0],
                    sViewportRotation[1]);
    }
    static int on = -1, done;
    extern unsigned int hh_frames;
    u32 *b = (u32 *)0x8002D500u, *last = (u32 *)0x8023DA00u;
    int n = 0;
    if (on < 0) on = getenv("BK_HEAPCHECK") != NULL;
    if (!on || done) return;
    while (b != last && n++ < 100000) {
        u32 *next = (u32 *)(uintptr_t)b[1];
        if (next <= b || next > last || (u32 *)(uintptr_t)next[0] != b) {
            fprintf(stderr, "HEAPCHECK frame %u: block %p next %p (next->prev %p)\n", hh_frames, (void *)b, (void *)next,
                    (next > b && next <= last) ? (void *)(uintptr_t)next[0] : NULL);
            done = 1;
            return;
        }
        b = next;
    }
}

/* BK_SPLLOG, BK_NODELOG2, BK_CAMNODELOG (tests): read once each */
int hh_dbg_env(const char *name)
{
    static const char *names[8];
    static int vals[8], n;
    int i;
    for (i = 0; i < n; i++)
        if (names[i] == name || strcmp(names[i], name) == 0)
            return vals[i];
    if (n < 8) {
        names[n] = name;
        vals[n] = getenv(name) != NULL;
        return vals[n++];
    }
    return getenv(name) != NULL;
}
