#include <ft/fttypes.h>
#include <sc/scsubsys/scsubsys.h>
#include <reloc_data.h>

s32 D_ovl1_80392010[] =
{
	0xAC000008,
	ftMotionCommandWait(3),
	0xAC000000,
	ftMotionCommandReturn()
};

s32 D_ovl1_80392020[] =
{
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(80),
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(20),
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(140),
	0xAC000005,
	ftMotionCommandWait(80),
	0xAC000000,
	ftMotionCommandWait(70),
	ftMotionCommandGoto(D_ovl1_80392020)
};

s32 D_ovl1_8039205C[] =
{
	ftMotionCommandWaitAsync(158),
	0xAC000005,
	ftMotionCommandEnd()
};

s32 D_ovl1_80392068[] =
{
	ftMotionCommandWaitAsync(158),
	0xAC000005,
	ftMotionCommandEnd()
};

s32 D_ovl1_80392074[] =
{
	ftMotionCommandWaitAsync(120),
	0xAC000005,
	ftMotionCommandEnd()
};

s32 D_ovl1_80392080[] =
{
	0xAC000005,
	ftMotionCommandEnd()
};

s32 D_ovl1_80392088[] =
{
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(0x003C),
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(20),
	ftMotionCommandSubroutine(D_ovl1_80392010),
	ftMotionCommandWait(0x003C),
	ftMotionCommandGoto(D_ovl1_80392088)
};

FTMotionDesc dFTKirbySubMotionDescs[] =
{
#ifdef PORT
    ll_1259_FileID, (intptr_t)D_ovl1_80392020, 0x00000000,
    ll_416_FileID,  (intptr_t)D_ovl1_8039205C, 0x80000000,
    ll_417_FileID,  (intptr_t)D_ovl1_80392068, 0x80000000,
    ll_418_FileID,  (intptr_t)D_ovl1_80392074, 0x00000000,
    ll_418_FileID,  (intptr_t)D_ovl1_80392074, 0x00000000,
    ll_419_FileID,  (intptr_t)D_ovl1_80392080, 0x00000000,
    ll_423_FileID,  0x80000000,      0x00000000,
#else
    &llFTKirbyAnimEggLayFileID, D_ovl1_80392020, 0x00000000,
    &llFTKirbyAnimWin1FileID,  D_ovl1_8039205C, 0x80000000,
    &llFTKirbyAnimWin2FileID,  D_ovl1_80392068, 0x80000000,
    &llFTKirbyAnimSelectedFileID,  D_ovl1_80392074, 0x00000000,
    &llFTKirbyAnimSelectedFileID,  D_ovl1_80392074, 0x00000000,
    &llFTKirbyAnimClapsFileID,  D_ovl1_80392080, 0x00000000,
    &llFTKirbyAnimUnknownFileID,  0x80000000,      0x00000000,
#endif
    0x00000000,      0x80000000,      0x00000000,
#ifdef PORT
    ll_420_FileID,  0x80000000,      0x80000000,
    ll_421_FileID,  0x80000000,      0x40000000,
    ll_422_FileID,  0x80000000,      0x40000000,
    ll_425_FileID,  0x80000000,      0x00000000,
    ll_426_FileID,  0x80000000,      0x00000000,
    ll_427_FileID,  0x80000000,      0x00000000,
    ll_428_FileID,  0x80000000,      0x00000000,
    ll_424_FileID,  (intptr_t)D_ovl1_80392088, 0x00000000
#else
    &llFTKirbyAnimDollPickUpFileID,  0x80000000,      0x80000000,
    &llFTKirbyAnimDollFallFileID,  0x80000000,      0x40000000,
    &llFTKirbyAnimDollRevivalFileID,  0x80000000,      0x40000000,
    &llFTKirbyAnimIntro2FileID,  0x80000000,      0x00000000,
    &llFTKirbyAnimIntroPoseFileID,  0x80000000,      0x00000000,
    &llFTKirbyAnimPoseFileID,  0x80000000,      0x00000000,
    &llFTKirbyAnimPose2FileID,  0x80000000,      0x00000000,
    &llFTKirbyAnimIntroFileID,  D_ovl1_80392088, 0x00000000
#endif
};

s32 dFTKirbySubMotionDescsCount = sizeof(dFTKirbySubMotionDescs)/sizeof(FTMotionDesc); // 0x00000010
