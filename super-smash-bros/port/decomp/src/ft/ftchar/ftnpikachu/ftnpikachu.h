#ifndef _FTNPIKACHU_H_
#define _FTNPIKACHU_H_

#include <ssb_types.h>
#include <sys/objdef.h>
#include <ft/ftdef.h>

extern void *gFTDataNPikachuMain;
extern void *gFTDataNPikachuSubMotion;
extern void *gFTDataNPikachuModel;
#ifdef PORT
extern void *gFTDataNPikachuParticleBankID;
#else
extern s32 gFTDataNPikachuParticleBankID;
#endif
extern s32 gFTDataNPikachuPad0x801310E0;

#endif
