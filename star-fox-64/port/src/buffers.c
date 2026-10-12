#include "sys.h"

u64 gDramStack[SP_DRAM_STACK_SIZE64];
u8 gOSYieldData[OS_YIELD_DATA_SIZE];
FrameBuffer gZBuffer; // z buffer
u8 gTaskOutputBuffer[0x30000];
#if defined(TARGET_HANDHELD)
/* AudioLoad_Init clears it with u64 stores and the heap hands out 16-aligned
 * blocks: pin the base (it was 16-aligned only by link order). */
u8 gAudioHeap[0xB0000] __attribute__((aligned(16)));
#else
u8 gAudioHeap[0xB0000];
#endif
u16 gTextureRenderBuffer[0x3C40];
u16 gFillBuffer[3 * SCREEN_WIDTH];
FrameBuffer gFrameBuffers[3];
