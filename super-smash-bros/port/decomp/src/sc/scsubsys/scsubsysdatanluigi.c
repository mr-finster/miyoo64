#include <ft/fttypes.h>
#include <sc/scsubsys/scsubsys.h>
#include <reloc_data.h>

FTMotionDesc dFTNLuigiSubMotionDescs[] =
{
#ifdef PORT
	ll_1103_FileID, 0x80000000, 0x00000000
#else
	&llFTLuigiAnimEggLayFileID, 0x80000000, 0x00000000
#endif
};
s32 dFTNLuigiSubMotionDescsCount = sizeof(dFTNLuigiSubMotionDescs)/sizeof(FTMotionDesc); // 0x00000001
