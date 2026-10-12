#include <lb/library.h>
#include <sys/video.h>
#include <reloc_data.h>

#ifdef PORT
#include <bridge/framebuffer_capture.h>
#endif

extern void *gSYSchedulerCurrentFramebuffer;
extern syRdpSetViewport(void*, f32, f32, f32, f32);

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x800D5D60
LBTransitionDesc dLBTransitionDescs[/* */] =
{
    // Paper Plane
    {
#ifdef PORT
        llLBTransitionAeroplaneFileID,
        llLBTransitionAeroplaneDObjDesc,
        llLBTransitionAeroplaneAnimJoint,
#else
        &llLBTransitionAeroplaneFileID,
        &llLBTransitionAeroplaneDObjDesc,
        &llLBTransitionAeroplaneAnimJoint,
#endif
        0
    },

    // Checkered Board
    {
#ifdef PORT
        llLBTransitionCheckFileID,
        llLBTransitionCheckDObjDesc,
        llLBTransitionCheckAnimJoint,
#else
        &llLBTransitionCheckFileID,
        &llLBTransitionCheckDObjDesc,
        &llLBTransitionCheckAnimJoint,
#endif
        0
    },

    // Falling Board
    {
#ifdef PORT
        llLBTransitionGakubuthiFileID,
        llLBTransitionGakubuthiDObjDesc,
        llLBTransitionGakubuthiAnimJoint,
#else
        &llLBTransitionGakubuthiFileID,
        &llLBTransitionGakubuthiDObjDesc,
        &llLBTransitionGakubuthiAnimJoint,
#endif
        0
    },

    // Doors
    {
#ifdef PORT
        llLBTransitionKannonFileID,
        llLBTransitionKannonDObjDesc,
        llLBTransitionKannonAnimJoint,
#else
        &llLBTransitionKannonFileID,
        &llLBTransitionKannonDObjDesc,
        &llLBTransitionKannonAnimJoint,
#endif
        0
    },

    // Star
    {
#ifdef PORT
        llLBTransitionStarFileID,
        llLBTransitionStarDObjDesc,
        llLBTransitionStarAnimJoint,
#else
        &llLBTransitionStarFileID,
        &llLBTransitionStarDObjDesc,
        &llLBTransitionStarAnimJoint,
#endif
        0
    },

    // Vertical Lines
    {
#ifdef PORT
        llLBTransitionSudare1FileID,
        llLBTransitionSudare1DObjDesc,
        llLBTransitionSudare1AnimJoint,
#else
        &llLBTransitionSudare1FileID,
        &llLBTransitionSudare1DObjDesc,
        &llLBTransitionSudare1AnimJoint,
#endif
        0
    },

    // Diagonal Lines
    {
#ifdef PORT
        llLBTransitionSudare2FileID,
        llLBTransitionSudare2DObjDesc,
        llLBTransitionSudare2AnimJoint,
#else
        &llLBTransitionSudare2FileID,
        &llLBTransitionSudare2DObjDesc,
        &llLBTransitionSudare2AnimJoint,
#endif
        0
    },

    // Camera Shutter
    {
#ifdef PORT
        llLBTransitionCameraFileID,
        llLBTransitionCameraDObjDesc,
        llLBTransitionCameraAnimJoint,
#else
        &llLBTransitionCameraFileID,
        &llLBTransitionCameraDObjDesc,
        &llLBTransitionCameraAnimJoint,
#endif
        0
    },

    // Collapsing Blocks
    {
#ifdef PORT
        llLBTransitionBlockFileID,
        llLBTransitionBlockDObjDesc,
        llLBTransitionBlockAnimJoint,
#else
        &llLBTransitionBlockFileID,
        &llLBTransitionBlockDObjDesc,
        &llLBTransitionBlockAnimJoint,
#endif
        0
    },

    // Rotating Frame Zooming Out
    {
#ifdef PORT
        llLBTransitionRotScaleFileID,
        llLBTransitionRotScaleDObjDesc,
        llLBTransitionRotScaleAnimJoint,
#else
        &llLBTransitionRotScaleFileID,
        &llLBTransitionRotScaleDObjDesc,
        &llLBTransitionRotScaleAnimJoint,
#endif
        0
    },

    // Curtain
    {
#ifdef PORT
        llLBTransitionCurtainFileID,
        llLBTransitionCurtainDObjDesc,
        llLBTransitionCurtainAnimJoint,
#else
        &llLBTransitionCurtainFileID,
        &llLBTransitionCurtainDObjDesc,
        &llLBTransitionCurtainAnimJoint,
#endif
        0
    }
};

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

// 0x800D6480
s32 sLBTransitionPad0x800D6480;

// 0x800D6484
void *sLBTransitionFileHeap;

// 0x800D6488 - Heap for "photocopy" of last frame drawn to framebuffer
void *sLBTransitionPhotoHeap;

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x800D4130
GObj* lbTransitionMakeCamera(u32 id, s32 link, u32 link_priority, u64 camera_mask)
{
    GObj *gobj;
    CObj *cobj;

    gobj = gcMakeGObjSPAfter(id, NULL, link, GOBJ_PRIORITY_DEFAULT);
    func_80009F74(gobj, func_80017DBC, link_priority, camera_mask, -1);
    
    cobj = gcAddCameraForGObj(gobj);
    gcAddXObjForCamera(cobj, nGCMatrixKindPerspFastF, 1);
    gcAddXObjForCamera(cobj, nGCMatrixKindLookAt, 1);
    syRdpSetViewport(&cobj->viewport, 10.0F, 10.0F, 310.0F, 230.0F);

    cobj->projection.persp.aspect = 15.0F / 11.0F;
    cobj->projection.persp.fovy = 45.0F;
    
    cobj->vec.eye.z = 1100.0F / syUtilsTan(F_CLC_DTOR32(cobj->projection.persp.fovy * 0.5F));

    cobj->flags |= COBJ_FLAG_DLBUFFERS | COBJ_FLAG_ZBUFFER;
    
    return gobj;
}

// 0x800D4248
void lbTransitionProcDisplay(GObj *gobj)
{
    gDPPipeSync(gSYTaskmanDLHeads[0]++);
    gSPSegment(gSYTaskmanDLHeads[0]++, 0x1, sLBTransitionPhotoHeap);
    
    gcDrawDObjTreeForGObj(gobj);

    gDPPipeSync(gSYTaskmanDLHeads[0]++);
}

// 0x800D42C8
void lbTransitionProcUpdate(GObj *gobj)
{
    gcPlayAnimAll(gobj);
    
    if (gobj->anim_frame <= 0.0F)
    {
        gcEjectGObj(gobj);
    }
}

// 0x800D430C
GObj* lbTransitionMakeTransition(s32 transition_id, u32 id, s32 link, void (*proc_display)(GObj*), u8 dl_link_id, void (*func)(GObj*))
{
    LBTransitionDesc *transition_desc = &dLBTransitionDescs[transition_id];
    GObj *gobj;

    lbRelocGetExternHeapFile(transition_desc->file_id, sLBTransitionFileHeap);
    gobj = gcMakeGObjSPAfter(id, NULL, link, GOBJ_PRIORITY_DEFAULT);
    
    gobj->user_data.s = transition_desc->unk_lbtransition_0xC;
    
    gcAddGObjDisplay(gobj, proc_display, dl_link_id, GOBJ_PRIORITY_DEFAULT, ~0);
    gcSetupCustomDObjs
    (
        gobj, 
        (DObjDesc*) (transition_desc->o_dobjdesc + (uintptr_t)sLBTransitionFileHeap),
        NULL,
        nGCMatrixKindTraRotRpyRSca,
        nGCMatrixKindNull,
        nGCMatrixKindNull
    );
    if (transition_desc->o_anim_joint != 0)
    {
        gcAddAnimJointAll(gobj, (AObjEvent32**) (transition_desc->o_anim_joint + (uintptr_t)sLBTransitionFileHeap), 0.0F);
        gcPlayAnimAll(gobj);
    }
    gcAddGObjProcess(gobj, func, nGCProcessKindFunc, 1);
    
    return gobj;
}

// 0x800D4404
void lbTransitionSetupTransition(void)
{
    s32 i, j;
    size_t current_size;
    size_t largest_size;
    u32 *heap_pixels, *framebuffer_pixels;

    largest_size = 0;
    
    for (i = 0; i < ARRAY_COUNT(dLBTransitionDescs); i++)
    {
        current_size = lbRelocGetFileSize(dLBTransitionDescs[i].file_id);

        if (largest_size < current_size)
        {
            largest_size = current_size;
        }
    }
    sLBTransitionFileHeap = syTaskmanMalloc(largest_size, 0x10);
    heap_pixels = sLBTransitionPhotoHeap = syTaskmanMalloc(300 * 220 * sizeof(u16), 0x10);

#ifdef PORT
    /* GPU framebuffer passthrough (issue #81 -- VS match -> results
     * screen transition). The transition's mesh DLs reference RSP
     * segment 1 (bound to sLBTransitionPhotoHeap in
     * lbTransitionProcDisplay) as a texture. On real hardware the
     * heap holds a CPU-side photocopy of the prior frame's RDRAM
     * framebuffer. Under libultraship the framebuffer never reaches
     * RDRAM, so the copy below is reading zeros and the transition
     * shape's interior is rendered solid black.
     *
     * Replacement: register the photo heap address as a mirror of an
     * internal snapshot FB. When Fast3D sees gsDPSetTextureImage on
     * the resolved segment-1 address it samples the snapshot directly
     * via SelectTextureFb -- full GPU resolution, no readback. The
     * registration is dropped on the next scene change via
     * lbRelocInitSetup -> port_capture_release_all (see
     * port/bridge/lbreloc_bridge.cpp). Falls through to the no-op
     * zero-init copy below if the bridge can't grab a clean source FB
     * (e.g. backend without render-to-fb pinned), matching the pre-fix
     * black-interior behaviour. */
    /* Photo region in the N64 320x240 framebuffer: rows 10..229, cols
     * 10..309 (the inner 300x220 visible area, stripped of the 10px
     * scissor border the lbcommon viewport uses).
     *
     * Y-flip vs. 1P stage clear: the original N64 #else copy below walks
     * the framebuffer BOTTOM-UP (starts at row 230, then `framebuffer_pixels -= 310`
     * after each row), so heap[0] holds the bottom row of the photo and
     * heap[end] holds the top. The transition mesh's vertex UVs (baked in
     * relocData, e.g. dLBTransitionAeroplane*) are authored against that
     * bottom-up heap layout. mGameFb stores rows top-down (N64 row 0 at v=0),
     * so we pass v0/v1 swapped: consumer's local v=0 (mesh top) samples FB
     * v=230/240 (N64 row 230) and consumer's v=1 (mesh bottom) samples
     * FB v=10/240 (N64 row 10). FbUvTransform handles negative scaleV
     * naturally. (1P stage clear in sc1pstageclear.c reads top-down and
     * passes v's in normal order.) */
#ifdef HANDHELD_PORT
    /* The software renderer copies its last frame in, as the #else loop does: heap row i = framebuffer
     * row 230 - i (rows 230..11, bottom-up), sampled where the port's uniform 3D view put it (gfx_pc.c). */
    (void)port_capture_fb_rgba16(sLBTransitionPhotoHeap, 300, 220,
                                 10.0f / 320.0f, 231.0f / 240.0f, 310.0f / 320.0f, 11.0f / 240.0f, 0);
#else
    (void)port_capture_register_fb_for_subrect(sLBTransitionPhotoHeap, 300u * 220u * sizeof(u16),
                                               10.0f / 320.0f, 230.0f / 240.0f,
                                               310.0f / 320.0f, 10.0f / 240.0f);
#endif
    (void)heap_pixels; (void)framebuffer_pixels; (void)i; (void)j;
#else
    framebuffer_pixels = (u32*)
	(
		(uintptr_t)gSYSchedulerCurrentFramebuffer + SYVIDEO_BORDER_SIZE(320, 10, u16) +
        SYVIDEO_BORDER_SIZE(320, 220, u16) + SYVIDEO_BORDER_SIZE(1, 10, u16)
	);
    for (i = 0; i < 220; i++)
    {
        for (j = 0; j < 150; j++)
        {
            *heap_pixels++ = *framebuffer_pixels++;
        }
        framebuffer_pixels -= 310;
    }
#endif
}
