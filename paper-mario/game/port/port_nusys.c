/* nusys for the native port (M2): the scheduler, graphics task manager, controller manager and PI reads collapse
 * into direct calls on one thread. Game side (the decomp's headers). The retrace callback the game registers
 * (gfxRetrace_Callback) is called by the main loop (port_main.c); a graphics task runs at once in the renderer
 * (port_gfx_task), so the task spool is always empty and nothing waits. The framebuffer rotation is nusys's own
 * (nugfxtaskmgr.c, nugfxsetcfb.c). The game's bss files define nusys's variables (src/bss/*.c). */
#include "common.h"
#include "nu/nusys.h"
#include "port.h"

NUGfxFunc nuGfxFunc;
NUGfxPreNMIFunc nuGfxPreNMIFunc;
NUGfxSwapCfbFunc nuGfxSwapCfbFunc;
NUGfxTaskEndFunc nuGfxTaskEndFunc;
u32 nuGfxCfbNum;

/* the three framebuffers are the game's (heaps3.c, fixed RDRAM addresses on the N64); the depth buffer, at
 * 0x80000400 on the N64, is the port's */
extern u16 gFrameBuf0[], gFrameBuf1[], gFrameBuf2[];
static u16 *sFrameBufs[] = { gFrameBuf0, gFrameBuf1, gFrameBuf2 };
static u16 sZBuffer[SCREEN_WIDTH * SCREEN_HEIGHT] ALIGNED(64);

/* ---------------------------------------------------------------- graphics */
void nuGfxSetCfb(u16 **framebuf, u32 framebufnum) {
    u32 i;
    nuGfxCfb = framebuf;
    nuGfxCfbNum = framebufnum;
    nuGfxCfbCounter = 0;
    nuGfxCfb_ptr = nuGfxCfb[0];
    for (i = 0; i < framebufnum; i++) { /* start on a buffer that is not on screen, as nusys does */
        if (osViGetCurrentFramebuffer() != nuGfxCfb[i]) {
            nuGfxCfbCounter = i;
            nuGfxCfb_ptr = nuGfxCfb[i];
            break;
        }
    }
}

/* nuGfxInit() is nuGfxInitEX2() in the game's nusys.h */
void nuGfxInitEX2(void) {
    nuGfxSetCfb(sFrameBufs, ARRAY_COUNT(sFrameBufs));
    nuGfxZBuffer = sZBuffer;
    nuGfxDisplay = NU_GFX_DISPLAY_OFF;
    nuGfxTaskSpool = 0;
    port_gfx_init();
}

void nuGfxTaskStart(Gfx *gfxList_ptr, u32 gfxListSize, u32 ucode, u32 flag) {
    u16 *const framebuffer = nuGfxCfb_ptr;
    (void) gfxListSize, (void) ucode;
    if (flag & NU_SC_SWAPBUFFER) {
        nuGfxCfbCounter = (nuGfxCfbCounter + 1) % nuGfxCfbNum;
        nuGfxCfb_ptr = nuGfxCfb[nuGfxCfbCounter];
    }
    port_gfx_task(gfxList_ptr, (flag & NU_SC_SWAPBUFFER) != 0);
    if (flag & NU_SC_SWAPBUFFER) {
        port_prev_frame_done(framebuffer);
        osViSwapBuffer(framebuffer);
        if (nuGfxDisplay & NU_GFX_DISPLAY_ON_TRIGGER) nuGfxDisplay = NU_GFX_DISPLAY_ON;
    }
}

void nuGfxFuncSet(NUGfxFunc func) { nuGfxFunc = func; }
void nuGfxPreNMIFuncSet(NUGfxPreNMIFunc func) { nuGfxPreNMIFunc = func; }
void nuGfxSwapCfbFuncSet(NUGfxSwapCfbFunc func) { nuGfxSwapCfbFunc = func; }
void nuGfxTaskEndFuncSet(NUGfxTaskEndFunc func) { nuGfxTaskEndFunc = func; }
void nuGfxSwapCfb(void *task) { (void) task; }
void nuGfxSetUcodeFifo(void *p, s32 size) { (void) p, (void) size; }
void nuGfxTaskAllEndWait(void) {}
void nuGfxRetraceWait(u32 n) { (void) n; }
void nuGfxDisplayOff(void) { nuGfxDisplay = NU_GFX_DISPLAY_OFF; }
void nuGfxDisplayOn(void) { nuGfxDisplay = NU_GFX_DISPLAY_ON_TRIGGER; }

/* ---------------------------------------------------------------- scheduler */
void nuScCreateScheduler(u8 videoMode, u8 numFields) { (void) videoMode, (void) numFields; }
void nuScAddClient(NUScClient *c, OSMesgQueue *mq, NUScMsg msgType) { (void) c, (void) mq, (void) msgType; }
void nuScRemoveClient(NUScClient *c) { (void) c; }
void nuScResetClientMesgType(NUScClient *c, NUScMsg msgType) { (void) c, (void) msgType; }
OSMesgQueue *nuScGetGfxMQ(void) { return NULL; }
OSMesgQueue *nuScGetAudioMQ(void) { return NULL; }
void nuScSetFrameBufferNum(u8 n) { (void) n; }
s32 nuScGetFrameRate(void) { return 60; }
void nuPreNMIFuncSet(NUScPreNMIFunc func) { (void) func; }
void nuIdleFuncSet(void (*func)(void)) { (void) func; }

/* ---------------------------------------------------------------- PI */
void nuPiInit(void) {}
void nuPiReadRom(u32 rom_addr, void *buf_ptr, u32 size) { port_pi_read(rom_addr, buf_ptr, size); }
void nuPiReadRomOverlay(NUPiOverlaySegment *segment) {
    /* the file select and pause menus: code linked in natively (port_segments.c restores fresh data) */
    port_pi_read((u32) (uintptr_t) segment->romStart, segment->ramStart, (u32) (segment->romEnd - segment->romStart));
}

/* ---------------------------------------------------------------- controller */
u8 nuContInit(void) {
    nuContNum = 1;
    return 1; /* controller 1 is plugged in */
}
void nuContDataGet(OSContPad *contdata, u32 padno) {
    u16 button = 0;
    int8_t x = 0, y = 0;
    if (padno == 0) port_input_read(&button, &x, &y);
    contdata->button = button;
    contdata->stick_x = x;
    contdata->stick_y = y;
    contdata->errno = 0;
}
void nuContDataGetEx(NUContData *contdata, u32 padno) { (void) contdata, (void) padno; }
void nuContDataLock(void) {}
void nuContDataUnLock(void) {}
void nuContRmbStart(u32 contNo, u16 freq, u16 frame) { (void) contNo, (void) freq, (void) frame; }
void nuContRmbForceStop(void) {}
void nuContRmbForceStopEnd(void) {}
void nuContRmbModeSet(u32 contNo, u8 mode) { (void) contNo, (void) mode; }
s32 nuContRmbCheck(u32 contNo) { (void) contNo; return -1; } /* no rumble pak */

/* ---------------------------------------------------------------- nusys's globals (src/bss/main_pre_bss.c keeps
 * them only in the N64 layout, #ifndef SHIFT) */
u32 nuGfxCfbCounter;
u32 nuGfxDisplay;
u32 nuContNum;
volatile u32 nuGfxTaskSpool;
OSPiHandle *nuPiCartHandle;
NUSched nusched;
