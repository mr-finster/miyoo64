#include <ft/fttypes.h>
#include <sc/scsubsys/scsubsys.h>
#include <reloc_data.h>

s32 D_ovl1_80391110[] =
{
	0xA0880000,
	ftMotionCommandWaitAsync(85),
	0xA08FFFFF,
	0xA0800001,
	ftMotionCommandEnd()
};

s32 D_ovl1_80391124[] =
{
	0xA0880000,
	ftMotionCommandWaitAsync(110),
	ftMotionPlayFGM(nSYAudioVoiceFoxWin),
	ftMotionCommandEnd()
};

s32 D_ovl1_80391134[] =
{
	ftMotionCommandWaitAsync(90),
	ftMotionPlayFGM(nSYAudioVoiceFoxSelected),
	ftMotionCommandEnd()
};

s32 D_ovl1_80391140[] =
{
	0xA0800001,
	0xA0500001,
	ftMotionCommandEnd()
};

s32 D_ovl1_8039114C[] =
{
	0xA0880000,
	ftMotionCommandEnd()
};

FTMotionDesc dFTFoxSubMotionDescs[] =
{
#ifdef PORT
	ll_642_FileID, 0x80000000, 0x00000000,
	ll_370_FileID, (intptr_t)D_ovl1_80391110, 0x00000000,
	ll_371_FileID, (intptr_t)D_ovl1_80391124, 0x00000000,
	ll_372_FileID, (intptr_t)D_ovl1_80391134, 0x00000000,
	ll_372_FileID, (intptr_t)D_ovl1_80391134, 0x00000000,
	ll_373_FileID, (intptr_t)D_ovl1_80391140, 0x00000000,
	ll_648_FileID, 0x80000000, 0x00000000,
#else
	&llFTFoxAnimEggLayFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimWin1FileID, D_ovl1_80391110, 0x00000000,
	&llFTFoxAnimWin2FileID, D_ovl1_80391124, 0x00000000,
	&llFTFoxAnimSelectedFileID, D_ovl1_80391134, 0x00000000,
	&llFTFoxAnimSelectedFileID, D_ovl1_80391134, 0x00000000,
	&llFTFoxAnimClapsFileID, D_ovl1_80391140, 0x00000000,
	&llFTFoxAnimRunFileID, 0x80000000, 0x00000000,
#endif
	0x00000000, 0x80000000, 0x00000000,
#ifdef PORT
	ll_374_FileID, 0x80000000, 0x80000000,
	ll_375_FileID, 0x80000000, 0x40000000,
	ll_376_FileID, 0x80000000, 0x40000000,
	ll_377_FileID, 0x80000000, 0x00000000,
	ll_378_FileID, 0x80000000, 0x00000000,
	ll_379_FileID, (intptr_t)D_ovl1_8039114C, 0x00000000,
	ll_380_FileID, (intptr_t)D_ovl1_8039114C, 0x00000000,
	ll_781_FileID, 0x80000000, 0x00000000,
	ll_782_FileID, 0x80000000, 0x00000000,
	ll_783_FileID, 0x80000000, 0x00000000,
	ll_784_FileID, 0x80000000, 0x00000000,
	ll_779_FileID, 0x80000000, 0x00000000,
	ll_790_FileID, 0x80000000, 0x00000000,
	ll_791_FileID, 0x80000000, 0x00000000,
	ll_792_FileID, 0x80000000, 0x00000000,
	ll_793_FileID, 0x80000000, 0x00000000
#else
	&llFTFoxAnimDollPickUpFileID, 0x80000000, 0x80000000,
	&llFTFoxAnimDollFallFileID, 0x80000000, 0x40000000,
	&llFTFoxAnimDollFileID, 0x80000000, 0x40000000,
	&llFTFoxAnimDollRevivalFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimPose1FrameFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimPosePostBlasterFileID, D_ovl1_8039114C, 0x00000000,
	&llFTFoxAnimPoseBlaster2FileID, D_ovl1_8039114C, 0x00000000,
	&llFTFoxAnimFireFoxStartGroundFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimReadyingFireFoxGroundFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimFireFoxGroundFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimFireFoxEndGroundFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimLaserFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimShineStartFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimSwitchDirectionShineFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimReflectingFileID, 0x80000000, 0x00000000,
	&llFTFoxAnimShineFileID, 0x80000000, 0x00000000
#endif
};
s32 dFTFoxSubMotionDescsCount = sizeof(dFTFoxSubMotionDescs)/sizeof(FTMotionDesc); // 0x00000018
