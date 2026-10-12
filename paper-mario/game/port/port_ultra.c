/* libultra for the native port (M2): the calls Paper Mario's code makes, on one thread. Game side: the decomp's
 * headers, no system headers (see port.h). The game's own bss files define libultra's and nusys's globals
 * (src/bss/*.c); only the functions are here.
 *   threads      the game starts two (audio, crash screen); nothing runs them: the main loop drives audio itself
 *   messages     plain FIFOs; a blocking receive on an empty queue returns -1 (one thread cannot wait)
 *   memory       osVirtualToPhysical is the identity (pointers stay host pointers); caches and the TLB are no-ops
 *                (effects are linked in, port_segments.c)
 *   VI, AI, PI   the video mode is ignored; audio buffers go to port_audio; a PI DMA reads the ROM (port_rom.c)
 *   flash        the 128 KB save chip is a file (port_save.c) */
#include "common.h"
#include "nu/nusys.h"
#include "port.h"

#define PORT_UNUSED __attribute__((unused))

s32 osTvType = OS_TV_NTSC;
u32 osMemSize = 0x800000;

u32 osVirtualToPhysical(void *addr) { return (u32) (uintptr_t) addr; }

/* ---------------------------------------------------------------- threads */
void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
    port_log("ultra: thread %d (%p) created, not run (the main loop does its work)\n", (int) id, (void *) entry);
    (void) t, (void) arg, (void) sp, (void) pri;
}
void osStartThread(OSThread *t) { (void) t; }
void osStopThread(OSThread *t) { (void) t; }
void osDestroyThread(OSThread *t) { (void) t; }
void osYieldThread(void) {}
void osSetThreadPri(OSThread *t, OSPri pri) { (void) t, (void) pri; }
OSPri osGetThreadPri(OSThread *t) { (void) t; return 0; }
OSIntMask osSetIntMask(OSIntMask m) { (void) m; return 0; }

/* ---------------------------------------------------------------- messages */
void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}
s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    (void) flag;
    if (mq->validCount >= mq->msgCount) return -1;
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    return 0;
}
s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    (void) flag;
    if (mq->validCount >= mq->msgCount) return -1;
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    return 0;
}
s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    (void) flag;
    if (mq->validCount == 0) return -1;
    if (msg != NULL) *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}
void osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg) { (void) e, (void) mq, (void) msg; }

/* ---------------------------------------------------------------- time */
static u64 sTimeBase;
u32 osGetCount(void) { return (u32) ((u64) port_time_us() * 46875 / 1000); } /* the CPU counter: 46.875 MHz */
OSTime osGetTime(void) { return (OSTime) osGetCount() - sTimeBase; }
void osSetTime(OSTime t) { sTimeBase = (u64) osGetCount() - t; }

/* ---------------------------------------------------------------- memory */
void osWritebackDCache(void *p, s32 n) { (void) p, (void) n; }
void osWritebackDCacheAll(void) {}
void osInvalDCache(void *p, s32 n) { (void) p, (void) n; }
void osInvalICache(void *p, s32 n) { (void) p, (void) n; }
void osMapTLB(s32 index, OSPageMask pm, void *vaddr, u32 evenpaddr, u32 oddpaddr, s32 asid) {
    (void) index, (void) pm, (void) vaddr, (void) evenpaddr, (void) oddpaddr, (void) asid;
}
void osUnmapTLB(s32 index) { (void) index; }
void osUnmapTLBAll(void) {}

/* ---------------------------------------------------------------- video */
static void *sViFramebuffer;
void osViSetMode(OSViMode *m) { (void) m; }
void osViSetSpecialFeatures(u32 f) { (void) f; }
void osViSetYScale(f32 s) { (void) s; }
void osViBlack(u8 black) { (void) black; }
void osViRepeatLine(u8 on) { (void) on; }
void osViSwapBuffer(void *fb) { sViFramebuffer = fb; }
void *osViGetCurrentFramebuffer(void) { return sViFramebuffer; }
void *osViGetNextFramebuffer(void) { return sViFramebuffer; }

/* ---------------------------------------------------------------- PI: the cartridge */
static OSPiHandle sCartHandle;
OSPiHandle *osCartRomInit(void) { return &sCartHandle; }
s32 osEPiStartDma(OSPiHandle *h, OSIoMesg *mb, s32 direction) {
    (void) h;
    if (direction == OS_READ) port_pi_read((u32) mb->devAddr, mb->dramAddr, (u32) mb->size);
    if (mb->hdr.retQueue != NULL) osSendMesg(mb->hdr.retQueue, (OSMesg) mb, OS_MESG_NOBLOCK);
    return 0;
}
s32 osEPiReadIo(OSPiHandle *h, u32 devAddr, u32 *data) {
    u8 b[4];
    (void) h;
    port_rom_read(devAddr & 0x0FFFFFFF, b, 4);
    *data = (u32) b[0] << 24 | (u32) b[1] << 16 | (u32) b[2] << 8 | b[3];
    return 0;
}
s32 osEPiWriteIo(OSPiHandle *h, u32 devAddr, u32 data) { (void) h, (void) devAddr, (void) data; return 0; }

/* ---------------------------------------------------------------- AI: the audio DAC */
static u32 sAiFrequency = 32000;
s32 osAiSetFrequency(u32 frequency) { sAiFrequency = frequency; return (s32) frequency; }
s32 osAiSetNextBuffer(void *buf, u32 size) { (void) buf, (void) size; return 0; }
u32 osAiGetLength(void) { return 0; }
u32 osAiGetStatus(void) { return 0; }

/* ---------------------------------------------------------------- flash: the 128 KB save chip, 128-byte pages */
#define FLASH_PAGE 128
static u8 *sFlashBuf;
OSPiHandle *osFlashInit(void) { port_flash_open(); return NULL; }
s32 osFlashReadArray(OSIoMesg *mb, s32 priority, u32 page, void *dram, u32 pages, OSMesgQueue *mq) {
    (void) priority;
    port_flash_read(page * FLASH_PAGE, dram, pages * FLASH_PAGE);
    if (mq != NULL) osSendMesg(mq, (OSMesg) mb, OS_MESG_NOBLOCK);
    return 0;
}
s32 osFlashWriteBuffer(OSIoMesg *mb, s32 priority, void *dram, OSMesgQueue *mq) {
    (void) priority;
    sFlashBuf = dram;
    if (mq != NULL) osSendMesg(mq, (OSMesg) mb, OS_MESG_NOBLOCK);
    return 0;
}
s32 osFlashWriteArray(u32 page) {
    if (sFlashBuf != NULL) port_flash_write(page * FLASH_PAGE, sFlashBuf, FLASH_PAGE);
    return 0;
}
s32 osFlashSectorErase(u32 page) { port_flash_erase(page * FLASH_PAGE, 128 * FLASH_PAGE); return 0; }
s32 osFlashAllErase(void) { port_flash_erase(0, 1024 * FLASH_PAGE); return 0; }

/* ---------------------------------------------------------------- debug */
/* _Printf (libultra's xprintf.c): the game's own printf core, for its debug output and the crash screen */
int vsnprintf(char *buf, unsigned int size, const char *fmt, va_list args);
int _Printf(char *(*prout)(char *, const char *, size_t), char *arg, const char *fmt, va_list args) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, args);
    if (n < 0) return n;
    if (n >= (int) sizeof buf) n = sizeof buf - 1;
    prout(arg, buf, (size_t) n);
    return n;
}
OSThread *__osGetActiveQueue(void) { return NULL; }

/* the audio microcode the game names in its tasks (the port mixes in C) */
u64 n_aspMainTextStart[1], n_aspMainDataStart[1];
