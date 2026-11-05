/* hh_tnt.c: what The New Tetris's link, ROM layout and hand-written assembly gave the game on the N64.
 *
 * - Fixed RAM (symbol_addrs.txt): lzobuf (0x80000400, the depth buffer and LZO scratch), the heap from the end of the
 *   N64's .bss (0x8012F7D0) to the audio heap (0x8034AD00, 256 KB), the two 400x300 frame buffers (cfb, 0x8038AD00).
 *   All inside the 4 MB of N64 DRAM mapped at 0x80000000 (hh_boot.c); the native binary lives above 0x80400000.
 * - ROM layout: the asset tables (fx, image, anim, dcm, sample) are read from the player's ROM at their cartridge
 *   offsets (the game passes &D_<offset> as the PI address).
 * - The hand-written MIPS of 069170.s, 0691B0.s, 069530.s and 06F220.s, in C.
 * - Controller/Transfer Pak calls: no pak, as with a bare controller. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hh_os.h"
#include "hh_hw.h"
#include "savestate.h"

__asm__(".globl lzobuf\n.set lzobuf, 0x80000400\n"
        ".globl cfb\n.set cfb, 0x8038AD00\n"
        ".globl main_BSS_END\n.set main_BSS_END, 0x8012F7D0\n"
        ".globl audioheap_BASE\n.set audioheap_BASE, 0x8034AD00\n"
        ".globl osAppNMIBuffer\n.set osAppNMIBuffer, 0x8000031C\n"
        /* the boot's checksum of the main segment's code: an empty range natively (its sum is never read) */
        ".globl main_TEXT_START\n.set main_TEXT_START, 0x8003ADD0\n"
        ".globl main_TEXT_END\n.set main_TEXT_END, 0x8003ADD0\n"
        /* cartridge offsets (newtetris.yaml) */
        ".globl D_A8190\n.set D_A8190, 0xA8190\n"     /* fx_lut */
        ".globl D_ADAE0\n.set D_ADAE0, 0xADAE0\n"     /* fx assets */
        ".globl D_273A00\n.set D_273A00, 0x273A00\n"  /* image_lut */
        ".globl D_5206B0\n.set D_5206B0, 0x5206B0\n"  /* anim_lut */
        ".globl D_5B9AB0\n.set D_5B9AB0, 0x5B9AB0\n"  /* dcm_lut */
        ".globl D_7C1960\n.set D_7C1960, 0x7C1960\n"  /* sample_lut */
        /* the kit's PC-port layout names: none here */
        ".globl _ftext\n.set _ftext, 0\n.globl _fbss\n.set _fbss, 0\n"
        ".globl __assets_start\n.set __assets_start, 0\n.globl __assets_end\n.set __assets_end, 0\n");

/* the crash screen thread waits for a CPU fault (0497E0.c): never comes */
static OSThread sFaulted;
OSThread *__osGetCurrFaultedThread(void) { return &sFaulted; }

/* microcode: tags only (hh_plat.c tells the tasks apart by type) */
long long int rspbootTextStart[1], rspbootTextEnd[1];
long long int gspF3DEX2_NoN_fifoTextStart[1], gspF3DEX2_NoN_fifoTextEnd[1], gspF3DEX2_NoN_fifoDataStart[1],
    gspF3DEX2_NoN_fifoDataEnd[1];
long long int aspMainTextStart[1], aspMainTextEnd[1], aspMainDataStart[1], aspMainDataEnd[1];

int32_t gFramebufferWidth = 400, gFramebufferHeight = 300;

const float __libm_qnan_f = __builtin_nanf("");

void hh_ovl_init(void) {}
void hh_heap_check(void) {}
s32 hh_bk_map(void) { return 0; }

/* the game's debug prints, to the log with TNT_GAMELOG=1 */
#include <stdarg.h>
void rmonPrintf(const char *fmt, ...)
{
    static int on = -1;
    va_list ap;
    if (on < 0) { const char *e = getenv("TNT_GAMELOG"); on = e && e[0] == '1'; }
    if (!on) return;
    va_start(ap, fmt);
    fputs("[game] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* the kernel's idle spin: blocks for good (the kernel loop idles instead) */
void hh_idle_forever(void)
{
    static OSMesgQueue q;
    static OSMesg m[1];
    OSMesg x;
    osCreateMesgQueue(&q, m, 1);
    for (;;)
        osRecvMesg(&q, &x, OS_MESG_BLOCK);
}

/* a test switch read once per name (game code's debug hooks) */
int hh_dbg_env(const char *name)
{
    static const char *names[16];
    static signed char vals[16];
    int i;
    for (i = 0; i < 16 && names[i]; i++)
        if (names[i] == name) return vals[i];
    {
        const char *e = getenv(name);
        int v = e && e[0] && e[0] != '0';
        if (i < 16) { names[i] = name; vals[i] = (signed char)v; }
        return v;
    }
}

/* ------------------------------------------------------------------------------------ */
/* 069170.s                                                                             */

/* cvt.w.s: the FPU's rounding mode, round to nearest (even) as libultra leaves it */
s32 func_800A2EF0(f32 x) { return (s32)lrintf(x); }

/* 8-bit samples to 16-bit (<< 8), returns the end of the output */
s16 *func_800A2F00(s8 *in, s16 *out, s32 n)
{
    s8 *end = in + n;
    while (in != end)
        *out++ = (s16)(*in++ << 8);
    return out;
}

/* ------------------------------------------------------------------------------------ */
/* 0691B0.s: cube tiles                                                                 */

/* copy a 64-entry tile (128 bytes) */
void func_800A2F30(s16 *dst, s16 *src)
{
    memcpy(dst, src, 0x80);
}

/* move every byte of b one step toward a's; 1 if any differed */
s32 func_800A2F50(s8 *a, s8 *b)
{
    s32 moved = 0, i;
    for (i = 0; i < 0x80; i++) {
        if (b[i] == a[i]) continue;
        b[i] = b[i] < a[i] ? b[i] + 1 : b[i] - 1;
        moved = 1;
    }
    return moved;
}

/* UnkStruct_0 (03B700.h): +0x00 image (128x128 bytes), +0x08 palette table, +0x14 UnkStruct_24 (gradients +0x14,
 * output +0x18, source bytes +0x1C, row +0x20, size +0x2C, steps +0x30/+0x34, corners +0x38..+0x54) */
typedef struct UnkStruct_0 UnkStruct_0;
#define F32(o) (*(s32 *)((u8 *)s + (o)))
#define FP(o) (*(u8 **)((u8 *)s + (o)))
static inline s32 hi_mul(s32 a, s32 b) { return (s32)(((s64)a * (s64)b) >> 32); }

/* mode 2: an 8x8 byte tile sampled from the image through the corners and the per-texel offsets (x4) */
void func_800A2FB0(UnkStruct_0 *s)
{
    s32 s0 = F32(0x38), s1 = F32(0x3C), s2 = F32(0x40), s3 = F32(0x44);
    s32 s4 = (F32(0x48) - s0) >> 3, s5 = (F32(0x4C) - s1) >> 3, s6 = (F32(0x50) - s2) >> 3, s7 = (F32(0x54) - s3) >> 3;
    u8 *img = FP(0x00), *out = FP(0x18), *end = out + 0x40;
    s8 *g = (s8 *)FP(0x14);
    while (out != end) {
        s32 t4 = (s2 - s0) >> 3, t5 = (s3 - s1) >> 3, t2 = s0, t3 = s1;
        u8 *row = out + 8;
        while (out != row) {
            s32 x = (t2 >> 16) + (g[1] << 2), y = (t3 >> 16) + (g[0] << 2);
            *out = ((x >> 8) || (y >> 8)) ? 0 : img[((x >> 1) << 7) | (y >> 1)];
            t2 += t4;
            t3 += t5;
            out++;
            g += 2;
        }
        s0 += s4; s1 += s5; s2 += s6; s3 += s7;
    }
}

/* mode 1: 16-bit texels: (image byte as the light level, source byte) -> the palette table, over any size */
void func_800A30F0(UnkStruct_0 *s)
{
    s32 t0 = F32(0x34);
    s32 s0 = F32(0x38), s1 = F32(0x3C), s2 = F32(0x40), s3 = F32(0x44);
    s32 s4 = hi_mul(F32(0x48) - s0, t0), s5 = hi_mul(F32(0x4C) - s1, t0);
    s32 s6 = hi_mul(F32(0x50) - s2, t0), s7 = hi_mul(F32(0x54) - s3, t0);
    u8 *src = FP(0x1C), *end = src + F32(0x2C);
    u16 *out = (u16 *)FP(0x18);
    s8 *g = (s8 *)FP(0x14);
    u8 *img = FP(0x00);
    u16 *pal = (u16 *)FP(0x08);
    while (src != end) {
        s32 v0 = F32(0x30);
        s32 t4 = hi_mul(s2 - s0, v0), t5 = hi_mul(s3 - s1, v0), t2 = s0, t3 = s1;
        u8 *row = src + F32(0x20);
        while (src != row) {
            s32 x = (t2 >> 16) + g[1], y = (t3 >> 16) + g[0];
            u32 l = ((x >> 8) || (y >> 8)) ? img[0] : img[(((u32)x >> 1) << 7) | ((u32)y >> 1)];
            *out++ = pal[(l << 8) | *src]; /* the table holds N64-order texels (03B700.c) */
            t2 += t4;
            t3 += t5;
            src++;
            g += 2;
        }
        s0 += s4; s1 += s5; s2 += s6; s3 += s7;
    }
}
#undef F32
#undef FP

/* ------------------------------------------------------------------------------------ */
/* 069530.s: the CP0 watch registers (debug): nothing                                   */

void func_800A32B0(u32 addr, u32 mode) { (void)addr; (void)mode; }
void func_800A32E0(void) {}

/* ------------------------------------------------------------------------------------ */
/* 06F220.s: word copy, halfword fill, byte fill; each returns the end of the output     */

u32 *func_800A8FA0(u32 *dst, u32 *src, s32 nbytes)
{
    u8 *end = (u8 *)src + nbytes;
    while ((u8 *)src != end)
        *dst++ = *src++;
    return dst;
}

s16 *func_800A8FC8(s16 *dst, s32 count, s32 value)
{
    while (count-- != 0)
        *dst++ = (s16)value;
    return dst;
}

u8 *func_800A8FE8(u8 *dst, s32 value, s32 n)
{
    u8 *end = dst + n;
    while (dst != end)
        *dst++ = (u8)value;
    return dst;
}

/* ------------------------------------------------------------------------------------ */
/* Controller Pak / Transfer Pak: none plugged in                                        */

s32 osPfsInitPak(OSMesgQueue *mq, OSPfs *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return PFS_ERR_NOPACK; }
s32 osPfsRepairId(OSPfs *pfs) { (void)pfs; return PFS_ERR_NOPACK; }
s32 osPfsNumFiles(OSPfs *pfs, s32 *max, s32 *used) { (void)pfs; *max = 0; *used = 0; return PFS_ERR_NOPACK; }
s32 osPfsFreeBlocks(OSPfs *pfs, s32 *left) { (void)pfs; *left = 0; return PFS_ERR_NOPACK; }
s32 osPfsFindFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e, s32 *f) { (void)pfs; (void)c; (void)g; (void)n; (void)e; (void)f; return PFS_ERR_NOPACK; }
s32 osPfsFileState(OSPfs *pfs, s32 f, OSPfsState *st) { (void)pfs; (void)f; (void)st; return PFS_ERR_NOPACK; }
s32 osPfsDeleteFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e) { (void)pfs; (void)c; (void)g; (void)n; (void)e; return PFS_ERR_NOPACK; }
s32 osPfsAllocateFile(OSPfs *pfs, u16 c, u32 g, u8 *n, u8 *e, int len, s32 *f) { (void)pfs; (void)c; (void)g; (void)n; (void)e; (void)len; (void)f; return PFS_ERR_NOPACK; }
s32 osPfsReadWriteFile(OSPfs *pfs, s32 f, u8 flag, int off, int n, u8 *buf) { (void)pfs; (void)f; (void)flag; (void)off; (void)n; (void)buf; return PFS_ERR_NOPACK; }
s32 osGbpakInit(OSMesgQueue *mq, OSPfs *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return PFS_ERR_NOPACK; }
s32 osGbpakPower(OSPfs *pfs, s32 flag) { (void)pfs; (void)flag; return PFS_ERR_NOPACK; }
s32 osGbpakGetStatus(OSPfs *pfs, u8 *st) { (void)pfs; *st = 0; return PFS_ERR_NOPACK; }
s32 osGbpakReadId(OSPfs *pfs, OSGbpakId *id, u8 *st) { (void)pfs; (void)id; *st = 0; return PFS_ERR_NOPACK; }
s32 osGbpakReadWrite(OSPfs *pfs, u16 flag, u16 addr, u8 *buf, u16 size) { (void)pfs; (void)flag; (void)addr; (void)buf; (void)size; return PFS_ERR_NOPACK; }

/* TNT_AIP1=<0-4> (tests only): the game's own CPU player (aiplayer.c, the attract demo's) plays player 1's solo
 * games, rank 0 easy .. 4 master; -1 (unset): off */
int hh_test_aip1(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("TNT_AIP1"); v = e ? atoi(e) : -1; if (v > 4) v = 4; }
    return v;
}

/* TNT_WONDERLINES=<n> (tests only): the save's total wonder lines at the start (sram.c load_from_sram) */
int hh_test_wonderlines(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("TNT_WONDERLINES"); v = e ? atoi(e) : -1; }
    return v;
}

/* a line in console.log at each game start (066700.c): the soak's count, and the owner's log */
void hh_note_game_start(int players, int type)
{
    extern unsigned int hh_vicount;
    static const char *const kType[] = { "marathon", "sprint", "ultra" };
    fprintf(stderr, "GAME start vi %u players %d type %d (%s)%s\n", hh_vicount, players, type,
            type >= 0 && type < 3 ? kType[type] : "?", hh_test_aip1() >= 0 ? " cpu plays p1 (test)" : "");
}
