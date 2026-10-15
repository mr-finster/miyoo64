#ifdef PLATFORM_MIYOO
#include <sched.h>
#endif
#include <ultra64.h>
#include "constants.h"
#include "game/timing.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "system.h"
#endif

void frametimeInit(void)
{
	g_Vars.thisframestartt = osGetCount();
	g_Vars.prevframestartt = g_Vars.thisframestartt;
}

#if defined(PLATFORM_MIYOO)
// The Miyoo menu pause and a save state load: the game didn't run, so the next frame
// is one normal step from now, not the time since the last frame (pd_glue.c).
void frametimeRebase(void)
{
	const u32 step = (g_Vars.mininc60 ? g_Vars.mininc60 : 1) * CYCLES_PER_FRAME;

	g_Vars.thisframestartt = osGetCount() - step;
	g_Vars.lostframetime60t = 0;
	g_Vars.lostframetime240t = 0;
}
#endif

void frametimeApply(s32 diffframe60, s32 diffframe240, s32 frametime)
{
	g_Vars.prevframestartt = g_Vars.thisframestartt;
	g_Vars.thisframestartt = frametime;

	g_Vars.diffframe60 = diffframe60;
	g_Vars.diffframe60f = diffframe60;
	g_Vars.diffframe60freal = PALUPF(g_Vars.diffframe60f);

	g_Vars.prevframestart240 = g_Vars.thisframestart240;
	g_Vars.thisframestart240 += diffframe240;
	g_Vars.diffframe240 = diffframe240;
	g_Vars.diffframe240f = diffframe240;
	g_Vars.diffframe240freal = PALUPF(g_Vars.diffframe240f);
}

void frametimeCalculate(void)
{
	u32 count;
	u32 diffframet;
	u32 diffframe60;
	u32 diffframe240;

	do {
		count = osGetCount();
		diffframet = count - g_Vars.thisframestartt;
#if defined(PLATFORM_MIYOO)
		// a stall the game didn't run through (a save written to the card, a slow first frame) is
		// not game time: at most 8 frames in one step, as a long step sends the idle head sway far
		// past its range (bondhead.c) and the view banks (TEST12)
		if (diffframet > 8 * CYCLES_PER_FRAME) {
			g_Vars.thisframestartt = count - 8 * CYCLES_PER_FRAME;
			diffframet = 8 * CYCLES_PER_FRAME;
		}
#endif
		g_Vars.diffframet = diffframet;

		diffframe60 = (g_Vars.lostframetime60t + diffframet + CYCLES_PER_FRAME / 2) / CYCLES_PER_FRAME;
		diffframe240 = (g_Vars.lostframetime240t + diffframet + CYCLES_PER_FRAME / 2 / 4) / (CYCLES_PER_FRAME / 4);

#if defined(PLATFORM_MIYOO)
		// a sleep is a ~10 ms kernel tick on the Mini (and this one ran even when no wait was needed):
		// yield only while the frame really has to wait
		if (g_TickExtraSleep && g_Vars.mininc60 && diffframe60 < g_Vars.mininc60) {
			sched_yield();
		}
#elif !defined(PLATFORM_N64)
		if (g_TickExtraSleep) {
			sysSleep(EXTRA_SLEEP_TIME);
		}
#endif
	} while (g_Vars.mininc60 && diffframe60 < g_Vars.mininc60);

#ifdef PLATFORM_MIYOO
	{
		extern s32 sysFixClock(void);
		if (sysFixClock()) {
			diffframe60 = g_Vars.mininc60 ? g_Vars.mininc60 : 2;
			diffframe240 = diffframe60 * 4;
			diffframet = diffframe60 * CYCLES_PER_FRAME;
			count = g_Vars.thisframestartt + diffframet;
			g_Vars.diffframet = diffframet;
		}
	}
#endif
	g_Vars.lostframetime60t = g_Vars.lostframetime60t + diffframet - diffframe60 * CYCLES_PER_FRAME;
	g_Vars.lostframetime240t = g_Vars.lostframetime240t + diffframet - diffframe240 * (CYCLES_PER_FRAME / 4);

#ifdef PLATFORM_N64
	g_Vars.mininc60 = 1;
#else
	g_Vars.mininc60 = g_TickRateDiv;
#endif

	frametimeApply(diffframe60, diffframe240, count);
}

void func0f16cf8c(s32 arg0)
{
	// empty
}

void func0f16cf94(void)
{
	// empty
}
