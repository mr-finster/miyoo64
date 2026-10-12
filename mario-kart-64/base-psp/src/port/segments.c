/**
 * Segmented-address resolution and the memory regions the N64 linker used to
 * provide (see port.h for the overall scheme).
 */
#include <ultra64.h>
#include <macros.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "port.h"
#ifdef TARGET_HANDHELD
#include "savestate.h" /* SAVESTATE_EXCLUDE */
#else
#define SAVESTATE_EXCLUDE
#endif

/* ------------------------------------------------------------------------- */
/* Memory pool                                                                */
/* ------------------------------------------------------------------------- */

/* On the N64 the pool sits between the main code segment and the racing/ending
 * overlays; gHeapEndPtr allocates downward from the overlay boundary and
 * gNextFreeMemoryAddress upward from the pool start.  The port gives the game
 * one contiguous pool and makes every boundary symbol point at its ends. */
ALIGNED16 u8 gPortMemoryPool[PORT_MEMORY_POOL_SIZE];

/* The decomp's segments.h reads these as `extern u8 sym[]`; alias them onto
 * the pool with the assembler so they resolve to the right addresses. */
__asm__(
    ".globl _memoryPoolSegmentNoloadStart\n"
    ".set _memoryPoolSegmentNoloadStart, gPortMemoryPool\n"
    ".globl _memoryPoolSegmentNoloadEnd\n"
    ".set _memoryPoolSegmentNoloadEnd, gPortMemoryPool + 0x300000\n"
    /* SEG_ENDING / SEG_RACING are the top of the downward-growing heap. */
    ".globl _endingSegmentStart\n"
    ".set _endingSegmentStart, gPortMemoryPool + 0x300000\n"
    ".globl _endingSegmentNoloadEnd\n"
    ".set _endingSegmentNoloadEnd, gPortMemoryPool + 0x300000\n"
    ".globl _racingSegmentStart\n"
    ".set _racingSegmentStart, gPortMemoryPool + 0x300000\n"
    ".globl _racingSegmentNoloadEnd\n"
    ".set _racingSegmentNoloadEnd, gPortMemoryPool + 0x300000\n"
);

/* Code overlays: nothing to DMA on the PSP (everything is linked in).  The
 * *End symbols alias the *Start ones so every computed size is 0. */
ALIGNED16 u8 _racingSegmentRomStart[16];
ALIGNED16 u8 _endingSegmentRomStart[16];
__asm__(
    ".globl _racingSegmentRomEnd\n"
    ".set _racingSegmentRomEnd, _racingSegmentRomStart\n"
    ".globl _endingSegmentRomEnd\n"
    ".set _endingSegmentRomEnd, _endingSegmentRomStart\n"
);

/* Trig tables: on the N64 gSineTable & co are DMA'd from ROM into a bss
 * buffer the code is linked against; here they are initialised data, so the
 * "DMA" is a 0-byte copy (aliases via --defsym in Makefile.psp). */
__asm__(".globl _trigTablesSegmentSize\n.set _trigTablesSegmentSize, 0\n");

/* data_segment2 / common textures / ceremony / startup logo: all compiled in
 * and reached through the segment tables, so the loads become no-ops. */
ALIGNED16 u8 _data_segment2SegmentRomStart[16];
ALIGNED16 u8 _common_texturesSegmentRomStart[16];
ALIGNED16 u8 _ceremonyDataSegmentRomStart[16];
ALIGNED16 u8 _startupLogoSegmentRomStart[16];
__asm__(
    ".globl _data_segment2SegmentRomEnd\n"
    ".set _data_segment2SegmentRomEnd, _data_segment2SegmentRomStart\n"
    ".globl _common_texturesSegmentRomEnd\n"
    ".set _common_texturesSegmentRomEnd, _common_texturesSegmentRomStart\n"
    ".globl _ceremonyDataSegmentRomEnd\n"
    ".set _ceremonyDataSegmentRomEnd, _ceremonyDataSegmentRomStart\n"
    ".globl _startupLogoSegmentRomEnd\n"
    ".set _startupLogoSegmentRomEnd, _startupLogoSegmentRomStart\n"
);

/* Texture blobs the game DMAs by (segment offset) - on the PSP the callers
 * pass real pointers, so these bases are never dereferenced (see the
 * PORT_ROM_PTR sites in the game code). */
u8 _kart_texturesSegmentRomStart[16];
u8 _other_texturesSegmentRomStart[16];
u8 _textures_0aSegmentRomStart[16];
u8 _textures_0bSegmentRomStart[16];

/* The sound data and the segment-0x0B noise texture are aliased with
 * --defsym in Makefile.psp (the assembler cannot alias external symbols). */

/* RSP microcode symbols referenced when building SP tasks - never executed. */
u64 rspF3DBootStart[2], rspF3DBootEnd[1];
u64 gspF3DEXTextStart[2], gspF3DEXDataStart[2];
u64 gspF3DLXTextStart[2], gspF3DLXDataStart[2];
u64 rspAspMainStart[2], rspAspMainDataStart[2], rspAspMainDataEnd[1];

/* ------------------------------------------------------------------------- */
/* Segment translation                                                        */
/* ------------------------------------------------------------------------- */

static PortSegTable sSegTables[16];

void port_set_segment_table(s32 segment, const PortSegTable* table) {
    if (table != NULL) {
        sSegTables[segment] = *table;
    } else {
        sSegTables[segment].entries = NULL;
        sSegTables[segment].count = 0;
    }
}

void port_clear_segment_table(s32 segment) {
    port_set_segment_table(segment, NULL);
}

static const PortSegEntry* seg_table_lookup(const PortSegTable* t, u32 offset) {
    s32 lo = 0;
    s32 hi = t->count - 1;
    while (lo <= hi) {
        s32 mid = (lo + hi) >> 1;
        const PortSegEntry* e = &t->entries[mid];
        if (offset < e->offset) {
            hi = mid - 1;
        } else if (offset >= e->offset + e->size) {
            lo = mid + 1;
        } else {
            return e;
        }
    }
    return NULL;
}

void* port_seg_to_ptr(uintptr_t addr) {
    u32 segment;
    u32 offset;
    const PortSegTable* table;

    if (addr >= PORT_REAL_PTR_MIN) {
        return (void*) addr; // already a real pointer (above the segment window)
    }
    segment = addr >> 24;
    offset = addr & 0x00FFFFFF;
    if (segment >= 16) {
        return (void*) addr;
    }
    table = &sSegTables[segment];
    if (table->count != 0) {
        const PortSegEntry* e = seg_table_lookup(table, offset);
        if (e != NULL) {
            return (void*) ((const u8*) e->ptr + (offset - e->offset));
        }
        if (gSegmentTable[segment] == 0) {
            PORT_LOG("seg %X: offset %06X not in table\n", segment, offset);
            return NULL;
        }
    }
    return (void*) (gSegmentTable[segment] + offset);
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

/* Excluded from save states: a FILE* is a heap address in THIS process. A state
 * loaded in a later session would otherwise restore the old session's pointer,
 * and the next log line would write through it (the device address layout
 * differs every boot) -- the v24 "loading a save crashes" bug. */
static FILE* sLogFile SAVESTATE_EXCLUDE;

/* MK64_LOGVERBOSE=1 (or logverbose=1 in mk64_display.cfg): the full logging of before 2026-10-05 -- the boot and
 * course-load trace lines, a PERF line every 150 iterations (not every 750) and the data/log.txt copy of the
 * console log. Off in normal play: console.log alone carries the log. */
int port_logverbose(void) {
    static int sV SAVESTATE_EXCLUDE = -1;
    if (sV < 0) {
        const char* e = getenv("MK64_LOGVERBOSE");
#if defined(HANDHELD)
        extern int handheld_cfg_logverbose(void);
        sV = (e && e[0]) ? e[0] == '1' : handheld_cfg_logverbose() == 1;
#else
        sV = e && e[0] == '1';
#endif
    }
    return sV;
}

static void log_write(const char* buf) {
    fputs(buf, stdout);
    fflush(stdout);
    if (!port_logverbose()) return;
    if (sLogFile == NULL) {
        /* Own copy of the log, independent of where stdout points; rotate the
         * previous session so a short later launch never wipes a long one. */
        rename(port_save_path("log.1.txt"), port_save_path("log.2.txt"));
        rename(port_save_path("log.txt"), port_save_path("log.1.txt"));
        sLogFile = fopen(port_save_path("log.txt"), "w");
    }
    if (sLogFile != NULL) {
        fputs(buf, sLogFile);
        fflush(sLogFile);
    }
}

void port_log(const char* fmt, ...) {
    /* MK64_QUIET=1 drops the chatty logging: the per-frame traces (with an
     * fflushed log file) dominate wall-clock time on slow/emulated hosts. */
    static int sQuiet SAVESTATE_EXCLUDE = -1;
    if (sQuiet < 0) { const char* e = getenv("MK64_QUIET"); sQuiet = (e && e[0] == '1') ? 1 : 0; }
    if (sQuiet) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}

/* PERF / STALL / session markers: never silenced. */
void port_log_always(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}
