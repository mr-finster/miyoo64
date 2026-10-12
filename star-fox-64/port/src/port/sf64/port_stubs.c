/**
 * SF64 port: logging, CPU segment lookup, and the libultra / N64-library
 * entry points the game references but the port has no hardware for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ultra64.h>
#include "port.h"
#include "savestate.h"

/* ------------------------------------------------------------- logging --- */
static void log_write(const char* s) {
    fputs(s, stdout);
}

void port_log(const char* fmt, ...) {
    /* SF64_QUIET=1 drops the chatty logging (per-frame traces). */
    static int sQuiet SAVESTATE_EXCLUDE = -1;
    char buf[256];
    va_list ap;

    if (sQuiet < 0) { const char* e = getenv("SF64_QUIET"); sQuiet = (e && e[0] == '1') ? 1 : 0; }
    if (sQuiet) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}

void port_log_always(const char* fmt, ...) {
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(buf);
}

/* ------------------------------------------------------ segment lookup --- */
/* The renderer's fallback for a segmented address with no RSP segment set:
 * the game's own gSegments (real pointers on the port, see PR/R4300.h). */
extern u32 gSegments[16];

void* port_seg_to_ptr(uintptr_t addr) {
    if (addr >= PORT_REAL_PTR_MIN || addr == 0) {
        return (void*) addr;
    }
    {
        const u32 seg = (u32) (addr >> 24) & 0xF;
        if (gSegments[seg] == 0) {
            return NULL;
        }
        return (void*) (uintptr_t) (gSegments[seg] + (addr & 0x00FFFFFF));
    }
}

/* --------------------------------------------- N64 library math entries --- */
/* __sinf/__cosf come straight from the decomp's libultra (sinf.c/cosf.c, fsin
 * and fcos are their aliases in the N64 ELF): the game keeps the N64's own
 * trig results bit for bit. Those routines return this NaN for bad input. */
typedef union { u32 i; f32 f; } PortFU;
const PortFU __libm_qnan_f_u = { 0x7F810000 };
__asm__(".globl __libm_qnan_f\n.set __libm_qnan_f, __libm_qnan_f_u\n");

/* Math_FAtanF / Math_FAcosF: the decomp's own src/libc_math64.c (matched C, ROM
 * 0x206B0) since round 12, not host libm. */

/* ----------------------------------------------------- boot-only / asm --- */
/* MIO0 is only reached for compressed DMA entries; the port's table has none
 * (DMA_ENTRY sets compFlag false and the image is decompressed). */
void Mio0_Decompress(void* header, u8* dst) {
    (void) header;
    (void) dst;
    port_log_always("Mio0_Decompress called: the port ROM image should hold no compressed files\n");
}
void RdRam_CheckIPL3(void) { }

/* --------------------------------------------------- libultra hardware --- */
u32 osMemSize = 0x400000;
OSViMode osViModeNtscLan1, osViModePalLan1, osViModeMpalLan1;
OSPiHandle* osDriveRomInit(void) {
    extern OSPiHandle* osCartRomInit(void);
    return osCartRomInit();
}
void osViRepeatLine(u8 active) { (void) active; }
void osDpSetStatus(u32 data) { (void) data; }
OSThread* __osGetActiveQueue(void) { return NULL; }
s32 osContStartQuery(OSMesgQueue* mq) { osSendMesg(mq, NULL, OS_MESG_NOBLOCK); return 0; }
void osContGetQuery(OSContStatus* status) {
    int i;
    status[0].type = CONT_TYPE_NORMAL;
    status[0].status = 0;
    status[0].errno = 0;
    for (i = 1; i < 4; i++) status[i].errno = CONT_NO_RESPONSE_ERROR;
}
/* No Rumble Pak. */
s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, int channel) { (void) mq; (void) pfs; (void) channel; return -1; }
s32 osMotorStart(OSPfs* pfs) { (void) pfs; return 0; }
s32 osMotorStop(OSPfs* pfs) { (void) pfs; return 0; }

/* RSP microcode symbols the task setup takes the address of (never run). */
long long int rspbootTextStart[1], rspbootTextEnd[1];
long long int gspF3DEX_fifoTextStart[1], gspF3DEX_fifoDataStart[1];
long long int aspMainTextStart[1], aspMainDataStart[1], aspMainDataEnd[1];

/* ------------------------------------------------------ printf engine --- */
/* libultra's _Printf(out, arg, fmt, ap): format with the host's vsnprintf,
 * hand the text to the game's output callback in one call. */
typedef void* (*PrintCallback)(void*, const char*, size_t);
int _Printf(PrintCallback pfn, void* arg, const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n < 0) return n;
    if (n >= (int) sizeof(buf)) n = (int) sizeof(buf) - 1;
    return pfn(arg, buf, (size_t) n) != NULL ? n : -1;
}
