/* hh_pw64.c: what Pilotwings 64's link and ROM layout gave the game on the N64.
 *
 * - ROM layout symbols (config/us/pilotwings64.us.yaml): the file system and sound banks are read from the player's
 *   ROM at their cartridge offsets. The code segments are compiled in, so the app overlay copy is skipped (system.c).
 * - The kernel/app layout symbols memory.c uses to tell the heap from code: the N64's values, so the heap blocks are
 *   the N64's (the native binary lives above 0x80400000, outside them).
 * - Microcode symbols: the task's ucode pointer only names which microcode runs (hh_plat.c).
 * - libultra pieces the decomp does not carry in C. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hh_os.h"
#include "hh_hw.h"
#include "savestate.h"

__asm__(".globl filetable_ROM_START\n.set filetable_ROM_START, 0xDE720\n"
        ".globl filetable_ROM_END\n.set filetable_ROM_END, 0xDF5B0\n"
        ".globl filesys_ROM_START\n.set filesys_ROM_START, 0xDF5B0\n"
        ".globl filesys_ROM_END\n.set filesys_ROM_END, 0x618B70\n"
        ".globl audio_seq_ROM_START\n.set audio_seq_ROM_START, 0x618B70\n"
        ".globl audio_seq_ROM_END\n.set audio_seq_ROM_END, 0x62D460\n"
        ".globl audio_ctl_ROM_START\n.set audio_ctl_ROM_START, 0x62D460\n"
        ".globl audio_ctl_ROM_END\n.set audio_ctl_ROM_END, 0x6314D0\n"
        ".globl audio_tbl_ROM_START\n.set audio_tbl_ROM_START, 0x6314D0\n"
        ".globl audio_tbl_ROM_END\n.set audio_tbl_ROM_END, 0x800000\n"
        /* the N64's link: kernel text at 0x80200050, the end of the game's bss (heap block 4) at 0x803805E0 */
        ".globl kernel_TEXT_START\n.set kernel_TEXT_START, 0x80200050\n"
        ".globl D_803805E0\n.set D_803805E0, 0x803805E0\n"
        ".globl app_ROM_START\n.set app_ROM_START, 0\n"
        ".globl app_ROM_END\n.set app_ROM_END, 0\n"
        ".globl app_TEXT_START\n.set app_TEXT_START, 0\n"
        ".globl app_BSS_START\n.set app_BSS_START, 0\n"
        ".globl app_BSS_END\n.set app_BSS_END, 0\n"
        ".globl app_TEXT_END\n.set app_TEXT_END, 0\n"
        ".globl app_VRAM\n.set app_VRAM, 0\n"
        ".globl app_VRAM_END\n.set app_VRAM_END, 0\n"
        ".globl app_DATA_START\n.set app_DATA_START, 0\n"
        ".globl app_RODATA_END\n.set app_RODATA_END, 0\n"
        ".globl kernel_BSS_END\n.set kernel_BSS_END, 0\n"
        ".globl kernel_BSS_START\n.set kernel_BSS_START, 0\n"
        ".globl kernel_DATA_START\n.set kernel_DATA_START, 0\n"
        ".globl kernel_RODATA_END\n.set kernel_RODATA_END, 0\n"
        ".globl kernel_TEXT_END\n.set kernel_TEXT_END, 0\n"
        ".globl kernel_VRAM\n.set kernel_VRAM, 0\n"
        ".globl kernel_VRAM_END\n.set kernel_VRAM_END, 0\n"
        ".globl _ftext\n.set _ftext, 0\n"
        ".globl _fbss\n.set _fbss, 0\n"
        ".globl __assets_start\n.set __assets_start, 0\n"
        ".globl __assets_end\n.set __assets_end, 0\n");

/* microcode: tags only (hh_plat.c tells the tasks apart by type) */
long long int rspbootTextStart[1], rspbootTextEnd[1];
long long int gspFast3DTextStart[1], gspFast3DTextEnd[1], gspFast3DDataStart[1], gspFast3DDataEnd[1];
long long int gspF3DEX_fifoTextStart[1], gspF3DEX_fifoTextEnd[1], gspF3DEX_fifoDataStart[1], gspF3DEX_fifoDataEnd[1];
long long int aspMainTextStart[1], aspMainTextEnd[1], aspMainDataStart[1], aspMainDataEnd[1];

int32_t gFramebufferWidth = 320, gFramebufferHeight = 240;

void hh_ovl_init(void) {}
void hh_heap_check(void) {}
extern s32 D_80362690;
s32 hh_bk_map(void) { return 0; }

/* the render thread waits for a CPU fault before reading this */
static OSThread sFaulted;
OSThread *__osGetCurrFaultedThread(void) { return &sFaulted; }

/* cartridge word reads (bootproc reads 0xFFB000.. for a debug flag) */
s32 osPiRawReadIo(u32 devAddr, u32 *data)
{
    return osPiReadIo(devAddr, data); /* bytes in ROM order: bootproc reads them as a byte array */
}

/* ------------------------------------------------------------------------------------ */
/* MIO0 (the file system's GZIP chunks hold MIO0 streams): header "MIO0", u32 out size, u32 compressed offset,
 * u32 raw offset, then the layout bits. All fields big-endian. */
#define RD32(p) ((u32)((((const u8 *)(p))[0] << 24) | (((const u8 *)(p))[1] << 16) | (((const u8 *)(p))[2] << 8) | ((const u8 *)(p))[3]))
#define RD16(p) ((u16)((((const u8 *)(p))[0] << 8) | ((const u8 *)(p))[1]))
void mio0_decompress(void *in, void *out)
{
    const u8 *src = in;
    u8 *dst = out;
    u32 size = RD32(src + 4), comp = RD32(src + 8), raw = RD32(src + 12);
    u32 lay = 16, n = 0, bits = 0;
    if (memcmp(src, "MIO0", 4) != 0)
        fprintf(stderr, "[mio0] stream without MIO0 magic: %02X %02X %02X %02X\n", src[0], src[1], src[2], src[3]);
    int left = 0;
    while (n < size) {
        if (left == 0) {
            bits = RD32(src + lay);
            lay += 4;
            left = 32;
        }
        if (bits & 0x80000000u) {
            dst[n++] = src[raw++];
        } else {
            u16 v = RD16(src + comp);
            u32 len = (v >> 12) + 3, back = (v & 0xFFF) + 1;
            comp += 2;
            while (len-- && n < size) {
                dst[n] = dst[n - back];
                n++;
            }
        }
        bits <<= 1;
        left--;
    }
}

/* ------------------------------------------------------------------------------------ */
/* sound banks and the sequence file, to host order before libultra turns offsets into pointers */
static u8 *sB;
static u8 *sDone;
static void sw16(s32 o) { u8 t = sB[o]; sB[o] = sB[o + 1]; sB[o + 1] = t; }
static void sw32(s32 o) { u32 v = RD32(sB + o); memcpy(sB + o, &v, 4); }
static void sw16n(s32 o, s32 n) { while (n-- > 0) { sw16(o); o += 2; } }
static void sw32n(s32 o, s32 n) { while (n-- > 0) { sw32(o); o += 4; } }
static s16 h16(s32 o) { s16 v; memcpy(&v, sB + o, 2); return v; }
static s32 h32(s32 o) { s32 v; memcpy(&v, sB + o, 4); return v; }
static int once(s32 o)
{
    if (o <= 0 || o >= 0x40000) return 0;
    if (sDone[o >> 3] & (1u << (o & 7))) return 0;
    sDone[o >> 3] |= 1u << (o & 7);
    return 1;
}
static void bnk_wave(s32 o)
{
    s32 book, loop;
    if (!once(o)) return;
    sw32n(o, 2);
    sw32n(o + 12, 2);
    loop = h32(o + 12);
    book = h32(o + 16);
    if (sB[o + 8] == 0) {
        if (once(book)) { sw32n(book, 2); sw16n(book + 8, h32(book) * h32(book + 4) * 8); }
        if (once(loop)) { sw32n(loop, 3); sw16n(loop + 12, 16); }
    } else if (once(loop)) {
        sw32n(loop, 3);
    }
}
static void bnk_inst(s32 o)
{
    s32 i, n;
    if (!once(o)) return;
    sw16n(o + 12, 2);
    n = h16(o + 14);
    sw32n(o + 16, n);
    for (i = 0; i < n; i++) {
        s32 s = h32(o + 16 + 4 * i);
        if (!once(s)) continue;
        sw32n(s, 3);
        if (once(h32(s))) sw32n(h32(s), 3);
        bnk_wave(h32(s + 8));
    }
}
extern void __real_alBnkfNew(void *file, u8 *table);
void __wrap_alBnkfNew(void *file, u8 *table)
{
    s32 i, n;
    sB = file;
    sDone = calloc(0x40000 / 8 + 1, 1);
    sw16n(0, 2);
    n = h16(2);
    sw32n(4, n);
    for (i = 0; i < n; i++) {
        s32 b = h32(4 + 4 * i), k, ni;
        if (!once(b)) continue;
        sw16(b);
        sw32n(b + 4, 2);
        ni = h16(b);
        sw32n(b + 12, ni);
        if (h32(b + 8)) bnk_inst(h32(b + 8));
        for (k = 0; k < ni; k++) bnk_inst(h32(b + 12 + 4 * k));
    }
    free(sDone);
    sDone = NULL;
    __real_alBnkfNew(file, table);
}
/* ALSeqFile: s16 revision, s16 seqCount, {u8 *offset; s32 len}[seqCount]. audio_manager.c calls it once on the
 * 8-byte header copy and once on the full table: a table already in host order (revision 1 = 0x0001) is left. */
extern void __real_alSeqFileNew(void *file, u8 *base);
void __wrap_alSeqFileNew(void *file, u8 *base)
{
    u8 *p = file;
    if (p[2] == 0 && p[3] != 0) { /* big-endian seqCount (31) */
        s32 i, n;
        sB = p;
        sw16n(0, 2);
        n = h16(2);
        sw32n(4, 2 * n);
    }
    __real_alSeqFileNew(file, base);
}

/* system.c's kernel thread drops to priority 0 and spins: the N64's idle loop. Here it blocks for good (the kernel
 * loop idles instead). */
void hh_idle_forever(void)
{
    static OSMesgQueue q;
    static OSMesg m[1];
    OSMesg x;
    osCreateMesgQueue(&q, m, 1);
    for (;;)
        osRecvMesg(&q, &x, OS_MESG_BLOCK);
}

void hh_swap16n(void *p, int n)
{
    u16 *q = p;
    while (n-- > 0) { *q = __builtin_bswap16(*q); q++; }
}
void hh_swap32n(void *p, int n)
{
    u32 *q = p;
    while (n-- > 0) { *q = __builtin_bswap32(*q); q++; }
}
/* Vtx: s16 ob[3], u16 flag, s16 tc[2] (six halfwords), u8 cn[4] */
void hh_swap_vtx(void *p, int n)
{
    u16 *q = p;
    while (n-- > 0) { hh_swap16n(q, 6); q += 8; }
}
/* libultra sprite Bitmap: s16 width, width_img, s, t; void *buf; s16 actualHeight, LUToffset */
void hh_swap_bitmaps(void *p, int n)
{
    u8 *q = p;
    while (n-- > 0) { hh_swap16n(q, 4); hh_swap32n(q + 8, 1); hh_swap16n(q + 12, 2); q += 16; }
}

/* A struct array in host order by a layout string, one letter per 4 bytes: w = a 32-bit word (s32/f32/pointer),
 * h = two 16-bit halves, b = bytes (u8 fields, pads). Repeats over size bytes. */
void hh_swap_desc(void *p, const char *desc, int size)
{
    u8 *q = p, *end = q + size;
    while (q < end) {
        const char *d;
        for (d = desc; *d && q + 4 <= end; d++, q += 4) {
            if (*d == 'w') hh_swap32n(q, 1);
            else if (*d == 'h') hh_swap16n(q, 2);
        }
        if (!*desc) break;
    }
}

/* the game's debug prints (_uvDebugPrintf), to the log with PW64_GAMELOG=1 */
#include <stdarg.h>
void hh_game_vlog(const char *fmt, va_list ap)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("PW64_GAMELOG"); on = e && e[0] == '1'; }
    if (on) { fputs("[game] ", stderr); vfprintf(stderr, fmt, ap); }
}

/* a test switch read once per name (game code's debug hooks) */
int hh_dbg_env(const char *name)
{
    /* cached by the name's address (callers pass literals): read in per-tile loops */
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

/* PW64_PERFECT=1 (test switch): every test's points read 100, as the emulators' "perfect score in all classes" code does
 * (its 8-bit pokes are the low bytes of these s16 points fields of D_80364210, offsets from the record start). Opens every
 * class and the extra games for the sweeps. */
void hh_cheat_perfect(void *results)
{
    static const unsigned short offs[] = { 0x6C, 0x9C, 0xCC, 0xFC, 0x12C, 0x15C, 0x24C, 0x39C, 0x4EC, 0x700, 0x730,
        0x760, 0x790, 0x7C0, 0x7F0, 0x850, 0x880, 0x8B0, 0x8E0, 0xA30, 0xB80, 0xD94, 0xDC4, 0xDF4, 0xE24, 0xE54, 0xE84,
        0xEE4, 0xF14, 0xF44, 0xF74, 0x1034, 0x1064, 0x1094, 0x10C4, 0x1214, 0x1428, 0x1458, 0x1488, 0x1578, 0x15A8,
        0x15D8, 0x16C8, 0x16F8, 0x1728 };
    unsigned i;
    if (!hh_dbg_env("PW64_PERFECT")) return;
    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) *(s16 *)((u8 *)results + offs[i]) = 100;
}
