/* os.c -- the libultra services Smash still needs once its threads are
 * coroutines (bs/n64_stubs.c) and its graphics go to the software renderer:
 * the clock, caches, cartridge DMA (served from the ROM slice), controllers,
 * rumble and SRAM. */
#include <ssb_types.h>
#include <PR/os.h>
#include <PR/os_internal.h>
#include <PR/rcp.h>

#include "port_log.h"
#include "romdata.h"
#include "hh_input.h"
#include "hh_sram.h"

extern u32 port_time_us(void);

/* ---- clock: the VR4300 count register runs at 46.875 MHz ---- */
static u64 sTimeBase;

/* SSB64_TESTCLOCK=1 (tests): the game's clock is the tick count, one NTSC field a tick, kept in save
 * states (HH_GS_BSS), so the title's and the menus' clock-seeded random picks (syUtilsRandTime*) are
 * the same in every run and across a save and a load (tools/qstate.sh). Each read also moves it on
 * by 64 cycles, so the game's busy waits on the clock end. Off: the host clock, as the
 * N64's free-running counter; this file stays out of save states. */
#include "hh_game.h"
extern char *getenv(const char *name);
static u64 sTestClock HH_GS_BSS;
static int sTestClockOn = -1;
void hh_os_test_clock_tick(void)
{
    if (sTestClockOn < 0) sTestClockOn = getenv("SSB64_TESTCLOCK") != NULL;
    if (sTestClockOn) sTestClock += OS_USEC_TO_CYCLES(16667ull);
}

OSTime osGetTime(void)
{
    u64 us = (u64)port_time_us();   /* wraps every ~71 min; fine for deltas */
    if (sTestClockOn > 0) return sTestClock += 64; /* moves on each read too: a busy wait on it ends */
    return OS_USEC_TO_CYCLES(us) - sTimeBase;
}

void osSetTime(OSTime t)
{
    if (sTestClockOn > 0) sTestClock = t;
    sTimeBase = OS_USEC_TO_CYCLES((u64)port_time_us()) - t;
}

u32 osGetCount(void)
{
    return (u32)osGetTime();
}

/* ---- caches and addresses: one coherent address space ---- */
void osInvalDCache(void *p, s32 n) { (void)p; (void)n; }
void osInvalICache(void *p, s32 n) { (void)p; (void)n; }
void osWritebackDCache(void *p, s32 n) { (void)p; (void)n; }
void osWritebackDCacheAll(void) {}

/* The game hands "physical" addresses back to itself (audio buffers, DMA
 * targets); on the host they must stay usable as pointers. */
/* PORT declares it u64 (sized for LP64 pointers). */
u64 osVirtualToPhysical(void *p) { return (u64)(uintptr_t)p; }

/* ---- cartridge: reads come from the extracted ROM slice ---- */
static OSPiHandle sRomHandle;

OSPiHandle *osCartRomInit(void)
{
    sRomHandle.type = DEVICE_TYPE_CART;
    sRomHandle.baseAddress = 0xB0000000u;
    return &sRomHandle;
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt)
{
    (void)pri; (void)cmdQ; (void)cmdBuf; (void)cmdMsgCnt;
}

/* Synchronous: the transfer is done before the completion message is posted,
 * so a thread blocking on it resumes with the data in place. */
s32 osEPiStartDma(OSPiHandle *h, OSIoMesg *mb, s32 direction)
{
    (void)h;
    if (direction == OS_READ) {
        if (romdata_read((u32)mb->devAddr, mb->dramAddr, (u32)mb->size) != 0) {
            port_log("SSB64: cartridge DMA out of range 0x%08X size 0x%X\n",
                     (unsigned)mb->devAddr, (unsigned)mb->size);
        }
    }
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 direction, u32 devAddr, void *dramAddr,
                 u32 size, OSMesgQueue *mq)
{
    (void)pri;
    mb->hdr.retQueue = mq;
    mb->devAddr = devAddr;
    mb->dramAddr = dramAddr;
    mb->size = size;
    return osEPiStartDma(&sRomHandle, mb, direction);
}

/* ---- controllers: port 1 is the handheld; ports 2-4 are empty ---- */
s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status)
{
    (void)mq;
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = (i == 0) ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = (i == 0) ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    *bitpattern = 1;
    return 0;
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    if (mq != NULL) {
        osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    }
    return 0;
}

void osContGetReadData(OSContPad *pads)
{
    HhPad p;
    hh_input_read(&p);
    for (int i = 0; i < MAXCONTROLLERS; i++) {
        pads[i].button = 0;
        pads[i].stick_x = 0;
        pads[i].stick_y = 0;
        pads[i].errno = CONT_NO_RESPONSE_ERROR;
    }
    pads[0].button = p.button;
    pads[0].stick_x = p.stick_x;
    pads[0].stick_y = p.stick_y;
    pads[0].errno = 0;
}

/* ---- no Rumble Pak ---- */
s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
{
    (void)mq; (void)pfs; (void)channel;
    return PFS_ERR_NOPACK;
}

s32 __osMotorAccess(OSPfs *pfs, s32 flag)
{
    (void)pfs; (void)flag;
    return PFS_ERR_NOPACK;
}

/* ---- SRAM: the game's backup data (lbbackup.c) ---- */
int port_save_read(uintptr_t offset, void *dst, size_t size)
{
    return hh_sram_read((u32)offset, dst, (u32)size);
}

int port_save_write(uintptr_t offset, const void *src, size_t size)
{
    return hh_sram_write((u32)offset, src, (u32)size);
}
