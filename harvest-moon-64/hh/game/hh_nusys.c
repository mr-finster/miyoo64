/* nusys, libultra and libmus for the native port, as Harvest Moon 64 calls them. Game side: the decomp's headers only.
 * One thread: mainLoop's waits for the retrace (decomp.patch: HH_WAIT_RETRACE) run the retrace here, which calls the
 * game's retrace callback (gfxRetraceCallback: input, drawing, the main loop's step flag) once per 1/60 s. A graphics
 * task is drawn at once by the renderer (hh_gfx_task, host side), so no task is ever pending. Sound: libmus, the
 * nusys audio manager and n_audio are compiled natively; hh/audio runs their command lists.
 * After Paper Mario's Miyoo port (port_nusys.c, port_ultra.c), which does the same for its nusys. */
#include "common.h"
#include "hh_port.h"

/* ---------------------------------------------------------------- nusys: graphics */
NUGfxFunc nuGfxFunc;
NUGfxPreNMIFunc nuGfxPreNMIFunc;
NUGfxSwapCfbFunc nuGfxSwapCfbFunc;
NUGfxTaskEndFunc nuGfxTaskEndFunc;
NUUcode *nuGfxUcode;
u16 **nuGfxCfb;
u16 *nuGfxCfb_ptr;
u32 nuGfxCfbNum;
u32 nuGfxCfbCounter;
u16 *nuGfxZBuffer;
u32 nuGfxDisplay;
volatile u32 nuGfxTaskSpool;
NUSched nusched;
long long int gspF3DEX2_fifoTextStart[1], gspF3DEX2_fifoDataStart[1];

void nuGfxThreadStart(void) {}
void nuGfxTaskMgrInit(void) { hh_gfx_init(); }
void nuGfxSetCfb(u16 **framebuf, u32 framebufnum) {
    nuGfxCfb = framebuf;
    nuGfxCfbNum = framebufnum;
    nuGfxCfbCounter = 0;
    nuGfxCfb_ptr = nuGfxCfb[0];
}
void nuGfxTaskStart(Gfx *gfxList_ptr, u32 gfxListSize, u32 ucode, u32 flag) {
    (void) gfxListSize, (void) ucode;
    hh_gfx_task(gfxList_ptr, (flag & NU_SC_SWAPBUFFER) != 0);
    if (flag & NU_SC_SWAPBUFFER) {
        if (nuGfxSwapCfbFunc != NULL) nuGfxSwapCfbFunc(NULL);
        nuGfxCfbCounter = (nuGfxCfbCounter + 1) % nuGfxCfbNum;
        nuGfxCfb_ptr = nuGfxCfb[nuGfxCfbCounter];
    }
}
void nuGfxSwapCfb(void *task) { (void) task; }
void nuGfxFuncSet(NUGfxFunc func) { nuGfxFunc = func; }
void nuGfxPreNMIFuncSet(NUGfxPreNMIFunc func) { nuGfxPreNMIFunc = func; }
void nuGfxSwapCfbFuncSet(NUGfxSwapCfbFunc func) { nuGfxSwapCfbFunc = func; }
void nuGfxTaskAllEndWait(void) {}
void nuGfxDisplayOff(void) { nuGfxDisplay = NU_GFX_DISPLAY_OFF; hh_display_on(0); }
void nuGfxDisplayOn(void) { nuGfxDisplay = NU_GFX_DISPLAY_ON; hh_display_on(1); }

/* One retrace (the decomp's busy waits on stepMainLoop call this, decomp.patch): the host paces it, polls the pad and
 * may end the run; then the game's own retrace callback. */
void hh_wait_retrace(void) {
    hh_host_retrace(0);
    if (nuGfxFunc != NULL) nuGfxFunc(nuGfxTaskSpool);
}
/* mainLoop's wait between two steps (decomp.patch): the game's state is all in its globals here (no step running, no
 * local of a game function live), so this is where save states are taken and restored */
void hh_wait_retrace_main(void) {
    hh_host_retrace(1);
    if (nuGfxFunc != NULL) nuGfxFunc(nuGfxTaskSpool);
}

/* ---------------------------------------------------------------- nusys: the controller */
OSContStatus nuContStatus[NU_CONT_MAXCONTROLLERS];
OSContPad nuContData[NU_CONT_MAXCONTROLLERS];
u32 nuContNum;
u8 nuContInit(void) {
    int i;
    for (i = 0; i < NU_CONT_MAXCONTROLLERS; i++) {
        nuContStatus[i].type = i == 0 ? CONT_TYPE_NORMAL : 0;
        nuContStatus[i].status = 0;
        nuContStatus[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    nuContNum = 1;
    return 1; /* controller 1 only */
}
void nuContDataGetExAll(NUContData *data) {
    static u16 last;
    int i;
    for (i = 0; i < NU_CONT_MAXCONTROLLERS; i++) {
        u16 b = 0;
        s8 x = 0, y = 0;
        if (i == 0) hh_input_read(&b, &x, &y);
        data[i].button = b;
        data[i].stick_x = x;
        data[i].stick_y = y;
        data[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
        data[i].trigger = i == 0 ? (u16) (b & ~last) : 0;
        if (i == 0) last = b;
    }
}
/* no Controller Pak: every call fails as with no pak inserted */
void nuContPakOpen(NUContPakFile *file, u32 contNo) { (void) contNo; file->type = 0; file->error = PFS_ERR_NOPACK; }
void nuContPakRepairId(NUContPakFile *file) { file->error = PFS_ERR_NOPACK; }
s32 nuContPakGetFree(NUContPakFile *file) { file->error = PFS_ERR_NOPACK; return 0; }
void nuContPakFileNum(NUContPakFile *file, s32 *max_files, s32 *used_files) {
    file->error = PFS_ERR_NOPACK;
    *max_files = 0;
    *used_files = 0;
}
void nuContPakCodeSet(u8 *companyCode, u8 *gameCode) { (void) companyCode, (void) gameCode; }
void nuContPakFileOpenJis(NUContPakFile *file, u8 *noteName, u8 *extName, u32 mode, s32 size) {
    (void) noteName, (void) extName, (void) mode, (void) size;
    file->error = PFS_ERR_NOPACK;
}
void nuContPakFileReadWrite(NUContPakFile *file, s32 offset, s32 size, u8 *buf, u32 mode) {
    (void) offset, (void) size, (void) buf, (void) mode;
    file->error = PFS_ERR_NOPACK;
}
void nuContPakFileDeleteJis(NUContPakFile *file, u8 *noteName, u8 *extName) {
    (void) noteName, (void) extName;
    file->error = PFS_ERR_NOPACK;
}

/* ---------------------------------------------------------------- nusys: PI (the cartridge) */
void nuPiReadRom(u32 rom_addr, void *buf_ptr, u32 size) { hh_rom_read(rom_addr, buf_ptr, size); }

/* ---------------------------------------------------------------- libultra */
s32 osTvType = OS_TV_NTSC;
OSViMode osViModeTable[56];
void osViSetMode(OSViMode *m) { (void) m; }
void osViSetSpecialFeatures(u32 f) { (void) f; }
void osViSetYScale(f32 s) { (void) s; }
u32 osVirtualToPhysical(void *addr) { return (u32) addr; }
void osWritebackDCache(void *p, s32 n) { (void) p, (void) n; }
void osInvalDCache(void *p, s32 n) { (void) p, (void) n; }
void osStopThread(OSThread *t) { (void) t; }
s32 __osSpSetPc(u32 pc) { (void) pc; return 0; }

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
s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    (void) flag;
    if (mq->validCount == 0) return -1;
    if (msg != NULL) *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    return 0;
}

/* SRAM (memory.c: the battery save, 32 KB at 0x08000000): a file on the host */
s32 osEPiLinkHandle(OSPiHandle *h) { (void) h; return 0; }
s32 osEPiStartDma(OSPiHandle *h, OSIoMesg *mb, s32 direction) {
    (void) h;
    if ((mb->devAddr & 0xFF000000u) == 0x08000000u) { /* SRAM */
        if (direction == OS_READ) hh_sram_read(mb->devAddr, mb->dramAddr, mb->size);
        else hh_sram_write(mb->devAddr, mb->dramAddr, mb->size);
    } else if (direction == OS_READ) {
        hh_rom_read(mb->devAddr, mb->dramAddr, mb->size); /* the cartridge (the audio manager's sample DMA) */
    }
    if (mb->hdr.retQueue != NULL) osSendMesg(mb->hdr.retQueue, (OSMesg) mb, OS_MESG_NOBLOCK);
    return 0;
}

void __assert(const char *exp, const char *filename, int line) { hh_log_game("assert: %s (%s:%d)\n", exp, filename, line); }

/* libm's quiet NaN (libultra's sqrtf/libm_vals.s) */
float __libm_qnan_f = __builtin_nanf("");

OSPiHandle *osCartRomInit(void) { static OSPiHandle h; return &h; }
s32 osAiSetFrequency(u32 frequency) { return (s32) frequency; }
OSIntMask osSetIntMask(OSIntMask m) { (void) m; return 0; }
