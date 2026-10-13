#include "video.h"
#if defined(PORT)
extern s32 port_vi_fields(s32 minFields);
#endif
#include "memory.h"
#include "PRinternal/viint.h"
#include "types.h"

/************ .data ************/

u16 *gVideoDepthBuffer = NULL;
UNUSED s32 D_800DE774 = 0;
UNUSED s8 D_800DE778 = 2;

VideoModeResolution gVideoModeResolutions[] = {
    { SCREEN_WIDTH, SCREEN_HEIGHT },                   // 320x240
    { SCREEN_WIDTH, SCREEN_HEIGHT },                   // 320x240
    { HIGH_RES_SCREEN_WIDTH, SCREEN_HEIGHT },          // 640x240
    { HIGH_RES_SCREEN_WIDTH, SCREEN_HEIGHT },          // 640x240
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT }, // 640x480
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT }, // 640x480
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT }, // 640x480
    { HIGH_RES_SCREEN_WIDTH, HIGH_RES_SCREEN_HEIGHT }, // 640x480
};

// This value exists in order to make sure there are no out of bounds accesses of gVideoModeResolutions
#define NUM_RESOLUTION_MODES ((s32) (sizeof(gVideoModeResolutions) / sizeof(VideoModeResolution)) - 1)

/*******************************/

/************ .bss ************/

s32 gVideoRefreshRate; // Official Name: viFramesPerSecond
f32 gVideoAspectRatio;
f32 gVideoHeightRatio;
OSMesg gVideoMesgBuf[8];
OSMesgQueue gVideoMesgQueue[8];
OSViMode gTvViMode;
s32 gVideoFbWidths[2];
s32 gVideoFbHeights[2];
u16 *gVideoFramebuffers[2];
s32 gVideoCurrFbIndex;
s32 gVideoModeIndex;
s32 sBlackScreenTimer;
u16 *gVideoCurrFramebuffer; // Official Name: currentScreen
u16 *gVideoLastFramebuffer; // Official Name: otherScreen
u16 *gVideoCurrDepthBuffer;
u16 *gVideoLastDepthBuffer;
u8 D_801262E4;
UNUSED OSMesg D_801262E8[8];
u8 gVideoDeltaCounter;
u8 gVideoDeltaTime;
OSScClient gVideoSched;

/******************************/

/**
 * Set up the framebuffers and the VI.
 * Framebuffers are allocated at runtime.
 * Official Name: viInit
 */
void video_init(s32 videoModeIndex, OSSched *sc) {
    if (osTvType == OS_TV_TYPE_PAL) {
        gVideoRefreshRate = REFRESH_50HZ;
        gVideoAspectRatio = ASPECT_RATIO_PAL;
        gVideoHeightRatio = HEIGHT_RATIO_PAL;
    } else if (osTvType == OS_TV_TYPE_MPAL) {
        gVideoRefreshRate = REFRESH_60HZ;
        gVideoAspectRatio = ASPECT_RATIO_MPAL;
        gVideoHeightRatio = HEIGHT_RATIO_MPAL;
    } else {
        gVideoRefreshRate = REFRESH_60HZ;
        gVideoAspectRatio = ASPECT_RATIO_NTSC;
        gVideoHeightRatio = HEIGHT_RATIO_NTSC;
    }

    if (osTvType == OS_TV_TYPE_PAL) {
        s32 i;
        for (i = 0; i <= NUM_RESOLUTION_MODES; i++) {
            gVideoModeResolutions[i].height += PAL_HEIGHT_DIFFERENCE;
        }
    }

    video_delta_reset();
    fb_mode_set(videoModeIndex);
    gVideoFramebuffers[0] = NULL;
    gVideoFramebuffers[1] = NULL;
    fb_alloc(0);
    fb_alloc(1);
    gVideoCurrFbIndex = 1;
    fb_swap();
    osCreateMesgQueue((OSMesgQueue *) &gVideoMesgQueue, gVideoMesgBuf, ARRAY_COUNT(gVideoMesgBuf));
    osScAddClient(sc, &gVideoSched, (OSMesgQueue *) &gVideoMesgQueue, OS_SC_ID_VIDEO);
    fb_init_vi();
    sBlackScreenTimer = 12;
    osViBlack(TRUE);
    gVideoDeltaCounter = 0;
    D_801262E4 = 3;
}

/**
 * Set the current video mode to the id specified.
 */
void fb_mode_set(s32 videoModeIndex) {
    gVideoModeIndex = videoModeIndex;
}

/**
 * Unused function that would return the current video mode index.
 */
UNUSED s32 fb_mode(void) {
    return gVideoModeIndex;
}

/**
 * Unused function that would change the framebuffer dimensions.
 * Since only one kind of video mode is ever used, this function is never called.
 */
UNUSED void fb_mode_size(s32 fbIndex) {
    gVideoFbWidths[fbIndex] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].width;
    gVideoFbHeights[fbIndex] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].height;
}

/**
 * Return the current framebuffer dimensions as a single s32 value.
 * The high 16 bits are the height of the frame, and the low 16 bits are the width.
 * Official Name: viGetCurrentSize
 */
s32 fb_size(void) {
    return (gVideoFbHeights[gVideoCurrFbIndex] << 16) | gVideoFbWidths[gVideoCurrFbIndex];
}

/**
 * Initialise the VI settings.
 * It first checks the TV type ad then will set the properties of the VI
 * depending on the gVideoModeIndex value.
 * Most of these go unused, as the value is always 1.
 * Official Name: viSetTiming
 */
void fb_init_vi(void) {
    s32 viModeTableIndex;
    OSViMode *tvViMode;

    viModeTableIndex = OS_VI_NTSC_LPN1;
    if (osTvType == OS_TV_TYPE_PAL) {
        viModeTableIndex = OS_VI_PAL_LPN1;
    } else if (osTvType == OS_TV_TYPE_MPAL) {
        viModeTableIndex = OS_VI_MPAL_LPN1;
    }

    switch (gVideoModeIndex & NUM_RESOLUTION_MODES) {
        case VIDEO_MODE_LOWRES_LAN:
            stubbed_printf("320 by 240 Point sampled, Non interlaced.\n");
            osViSetMode(&osViModeTable[viModeTableIndex]);
            break;
        case VIDEO_MODE_LOWRES_LPN:
            //!@bug: The video mode being set here is Point sampled
            // but the printf implies it was intended to be Anti-aliased.
            // By my understanding, this is the case we will always hit in code,
            // So maybe it was swapped out late in development?
            stubbed_printf("320 by 240 Anti-aliased, Non interlaced.\n");
            tvViMode = &osViModeNtscLpn1;
            if (osTvType == OS_TV_TYPE_PAL) {
                tvViMode = &osViModePalLpn1;
            } else if (osTvType == OS_TV_TYPE_MPAL) {
                tvViMode = &osViModeMpalLpn1;
            }
            fb_memcpy((u8 *) tvViMode, (u8 *) &gTvViMode, sizeof(OSViMode));
            if (osTvType == OS_TV_TYPE_PAL) {
                // A simple osViExtendVStart to add an additional 24 scanlines?
                gTvViMode.fldRegs[0].vStart -= (PAL_HEIGHT_DIFFERENCE << 16);
                gTvViMode.fldRegs[1].vStart -= (PAL_HEIGHT_DIFFERENCE << 16);
                gTvViMode.fldRegs[0].vStart += PAL_HEIGHT_DIFFERENCE;
                gTvViMode.fldRegs[1].vStart += PAL_HEIGHT_DIFFERENCE;
            }
            osViSetMode(&gTvViMode);
            break;
        case VIDEO_MODE_MEDRES_LPN:
            stubbed_printf("640 by 240 Point sampled, Non interlaced.\n");
            tvViMode = &osViModeNtscLpn1;
            if (osTvType == OS_TV_TYPE_PAL) {
                tvViMode = &osViModePalLpn1;
            } else if (osTvType == OS_TV_TYPE_MPAL) {
                tvViMode = &osViModeMpalLpn1;
            }

            fb_memcpy((u8 *) tvViMode, (u8 *) &gTvViMode, sizeof(OSViMode));
            gTvViMode.comRegs.width = WIDTH(HIGH_RES_SCREEN_WIDTH);
            gTvViMode.comRegs.xScale = SCALE(1, 0);
            gTvViMode.fldRegs[0].origin = ORIGIN(HIGH_RES_SCREEN_WIDTH * 2);
            gTvViMode.fldRegs[1].origin = ORIGIN(HIGH_RES_SCREEN_WIDTH * 2);
            osViSetMode(&gTvViMode);
            break;
        case VIDEO_MODE_MEDRES_LAN:
            stubbed_printf("640 by 240 Anti-aliased, Non interlaced.\n");
            tvViMode = &osViModeNtscLan1;
            if (osTvType == OS_TV_TYPE_PAL) {
                tvViMode = &osViModePalLan1;
            } else if (osTvType == OS_TV_TYPE_MPAL) {
                tvViMode = &osViModeMpalLan1;
            }
            fb_memcpy((u8 *) tvViMode, (u8 *) &gTvViMode, sizeof(OSViMode));
            gTvViMode.comRegs.width = WIDTH(HIGH_RES_SCREEN_WIDTH);
            gTvViMode.comRegs.xScale = SCALE(1, 0);
            gTvViMode.fldRegs[0].origin = ORIGIN(HIGH_RES_SCREEN_WIDTH * 2);
            gTvViMode.fldRegs[1].origin = ORIGIN(HIGH_RES_SCREEN_WIDTH * 2);
            osViSetMode(&gTvViMode);
            break;
        case VIDEO_MODE_HIGHRES_HPN:
            stubbed_printf("640 by 480 Point sampled, Interlaced.\n");
            osViSetMode(&osViModeTable[viModeTableIndex + OS_VI_NTSC_HPN1]);
            break;
        case VIDEO_MODE_HIGHRES_HAN:
            stubbed_printf("640 by 480 Anti-aliased, Interlaced.\n");
            osViSetMode(&osViModeTable[viModeTableIndex + OS_VI_NTSC_HAN1]);
            break;
        case VIDEO_MODE_HIGHRES_HPF:
            stubbed_printf("640 by 480 Point sampled, Interlaced, De-flickered.\n");
            osViSetMode(&osViModeTable[viModeTableIndex + OS_VI_NTSC_HPF1]);
            break;
        case VIDEO_MODE_HIGHRES_HAF:
            stubbed_printf("640 by 480 Anti-aliased, Interlaced, De-flickered.\n");
            osViSetMode(&osViModeTable[viModeTableIndex + OS_VI_NTSC_HAF1]);
            break;
    }
    // Could have just called this once like in JFG:
    // osViSetSpecialFeatures(OS_VI_DIVOT_ON | OS_VI_DITHER_FILTER_ON | OS_VI_GAMMA_OFF);
    osViSetSpecialFeatures(OS_VI_DIVOT_ON);
    osViSetSpecialFeatures(OS_VI_DITHER_FILTER_ON);
    osViSetSpecialFeatures(OS_VI_GAMMA_OFF);
}

/**
 * Allocate the selected framebuffer index from the main pool.
 * Will also allocate the depthbuffer if it does not already exist.
 * Framebuffers should be 64 bit aligned, but since the memory allocator
 * already aligns by 16, it only needs 48 bits of alignment in addition.
 */
void fb_alloc(s32 index) {
    if (gVideoFramebuffers[index] != 0) {
        mempool_locked_unset((u8 *) gVideoFramebuffers[index]); // Effectively unused.
        mempool_free(gVideoFramebuffers[index]);
    }
    gVideoFbWidths[index] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].width;
    gVideoFbHeights[index] = gVideoModeResolutions[gVideoModeIndex & NUM_RESOLUTION_MODES].height;
    if (gVideoModeIndex >= VIDEO_MODE_MIDRES_MASK) {
        gVideoFramebuffers[index] =
            mempool_alloc_safe((HIGH_RES_SCREEN_WIDTH * HIGH_RES_SCREEN_HEIGHT * 2) + 0x30, COLOUR_TAG_WHITE);
        gVideoFramebuffers[index] = FBALIGN(gVideoFramebuffers[index]);
        if (gVideoDepthBuffer == NULL) {
            gVideoDepthBuffer =
                mempool_alloc_safe((HIGH_RES_SCREEN_WIDTH * HIGH_RES_SCREEN_HEIGHT * 2) + 0x30, COLOUR_TAG_WHITE);
            gVideoDepthBuffer = FBALIGN(gVideoDepthBuffer);
        }
    } else {
        gVideoFramebuffers[index] =
            mempool_alloc_safe((gVideoFbWidths[index] * gVideoFbHeights[index] * 2) + 0x30, COLOUR_TAG_WHITE);
        gVideoFramebuffers[index] = FBALIGN(gVideoFramebuffers[index]);
        if (gVideoDepthBuffer == NULL) {
            gVideoDepthBuffer =
                mempool_alloc_safe((gVideoFbWidths[index] * gVideoFbHeights[index] * 2) + 0x30, COLOUR_TAG_WHITE);
            gVideoDepthBuffer = FBALIGN(gVideoDepthBuffer);
        }
    }
}

/**
 * Sets the video counters to their default values.
 * Another renmant from an unused system.
 * Official Name: viFrameRateReset
 */
void video_delta_reset(void) {
    gVideoDeltaCounter = 0;
    gVideoDeltaTime = LOGIC_30FPS;
}

/**
 * Wait for the finished message from the scheduler while counting up a timer,
 * then update the current framebuffer index.
 * This function also has a section where it counts a timer that goes no higher
 * than an update magnitude of 2. It's only purpose is to be used as a divisor
 * in the unused function, vi_refresh_rate.
 * Official Name: viFrameSync
 */
s32 fb_update(s32 mesg) {
    u8 tempUpdateRate;

    tempUpdateRate = LOGIC_60FPS;
    if (sBlackScreenTimer) {
        sBlackScreenTimer--;
        if (sBlackScreenTimer == 0) {
            osViBlack(FALSE);
        }
    }
    if (mesg != MESG_SKIP_BUFFER_SWAP) {
        fb_swap();
    }
#if defined(PORT)
    /* Port: the fields that passed during this frame (no VI retrace messages). */
    tempUpdateRate = port_vi_fields(1);
#else
    while (osRecvMesg(gVideoMesgQueue, NULL, OS_MESG_NOBLOCK) != -1) {
        tempUpdateRate++;
    }
#endif

    if (tempUpdateRate < gVideoDeltaTime) {
        if (gVideoDeltaCounter < 20) {
            gVideoDeltaCounter++;
        }
        if (gVideoDeltaCounter == 20) {
            gVideoDeltaTime = tempUpdateRate;
            gVideoDeltaCounter = 0;
        }
    } else {
        gVideoDeltaCounter = 0;
        if ((gVideoDeltaTime < tempUpdateRate) && (D_801262E4 >= tempUpdateRate)) {
            gVideoDeltaTime = tempUpdateRate;
        }
    }
#if defined(PORT)
    /* Hold the frame to the game's pace, but never above 2 fields (30 fps).
     * The N64 locks gVideoDeltaTime to 3 after one 3-field frame and only
     * lets go after 20 faster ones; on the device, whose race frames hover
     * around 33 ms, that pinned races to 20 fps. port_vi_fields carries the
     * remainder, so a 40 ms frame now alternates 2 and 3 fields (~25 fps) and
     * the game still advances by exactly the fields that passed. */
    {
        /* fps=60 / DKR_FPS=60 (port/sdl/main.c port_fps60; from the Miyoo build, the DKR port 701dccd): no hold at
         * all -- every frame shows when it is done and the game advances by the fields that passed (1 at 60 fps,
         * 2 at 30, a 20 ms frame alternating 1 and 2), where the hold keeps anything between 16.7 and 33 ms at
         * 30 fps. The game drops to 1 by itself after 20 frames under 16.7 ms (above), as on the console.
         * Off (the default, fps=30): the hold as before. */
        /* hold30=1 (DKR_HOLD30, the default since 1014b): the hold is a firm 2 fields. The game's own rule
         * above drops gVideoDeltaTime to 1 after 20 frames done inside one field, so light scenes ran at 60 and fell
         * back to 30 at the first heavy frame (the user, 2026-10-13: "gets to 60fps and then drops and it's
         * jarring"; 1014a's on-time waits made those 20-frame runs common on the device). DKR_HOLD30=0: as before. */
        extern int port_fps60(void), port_hold30(void);
        extern void port_rate_log(s32 rate);
        const s32 hold = port_fps60() ? 1 : port_hold30() ? 2 : gVideoDeltaTime > 2 ? 2 : gVideoDeltaTime;
        if (tempUpdateRate < hold) {
            tempUpdateRate += port_vi_fields(hold - tempUpdateRate);
        }
        port_rate_log(tempUpdateRate);
    }
    return tempUpdateRate;
#else
    while (tempUpdateRate < gVideoDeltaTime) {
        osRecvMesg(gVideoMesgQueue, NULL, OS_MESG_BLOCK);
        tempUpdateRate++;
    }

    osViSwapBuffer(gVideoLastFramebuffer);
    osRecvMesg(gVideoMesgQueue, NULL, OS_MESG_BLOCK);
    return tempUpdateRate;
#endif
}

void func_8007AB24(u8 arg0) {
    D_801262E4 = arg0;
}

/**
 * Unused function that returns the refresh rate, after performance.
 * A fully performant game would return 60.
 * Perhaps may have been used originally to calculate the factor in which to handle frameskipping with.
 */
UNUSED s32 vi_refresh_rate(void) {
    return (s32) ((f32) gVideoRefreshRate / (f32) gVideoDeltaTime);
}

/**
 * Flips the current framebuffer index, swapping to the other framebuffer
 * for the next frame, then update the current and previous framebuffer pointers.
 */
void fb_swap(void) {
    gVideoLastFramebuffer = gVideoFramebuffers[gVideoCurrFbIndex];
    gVideoLastDepthBuffer = gVideoDepthBuffer;
    gVideoCurrFbIndex ^= 1;
    gVideoCurrFramebuffer = gVideoFramebuffers[gVideoCurrFbIndex];
    gVideoCurrDepthBuffer = gVideoDepthBuffer;
}

/**
 * Copy byte-by-byte a region from one address to another.
 */
void fb_memcpy(u8 *src, u8 *dest, s32 len) {
    s32 i;

    for (i = 0; i < len; i++) {
        *dest++ = *src++;
    }
}
