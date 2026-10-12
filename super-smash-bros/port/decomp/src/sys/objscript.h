#ifndef _OBJSCRIPT_H_
#define _OBJSCRIPT_H_

#include <ssb_types.h>
#include <sys/objdef.h>

extern s32 gcSetupGObjScript(GObj *this_gobj, s32 id, GObj *next_gobj);
#ifdef PORT
extern GObj* gcAddGObjScript(GObj *gobj, uintptr_t param);
#else
extern GObj* gcAddGObjScript(GObj *gobj, GObjScript *gobjscript);
#endif
extern void gcAddGObjScriptByLink(s32 link, s32 id, GObj *gobj);
extern sb32 gcParseGObjScript(void (*func)(GObjScript));

#endif
