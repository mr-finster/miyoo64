/* input_miyoo.c -- the PC port's input.h for the Miyoo Mini's buttons (SDL 1.2 key events from
 * gfx_sdl12.c handle_events -> controller_handheld_key). Replaces port/src/input.c (SDL2).
 * One N64 controller. The GoldenEye Miyoo port's layout (its hh_input.c), Perfect Dark's actions
 * on the same keys:
 *
 *   in a stage (not the title, no menu open):
 *     D-pad      walk forward/back, turn (a tap turns a little, holding ramps up)
 *     B          fire (Z)
 *     A          action / reload (B); hold A (B held): switch fire mode; A + B: the same at once (B+Z)
 *     X          next weapon (A); hold X: the weapon/inventory wheel
 *     Y (hold)   aim (R): the d-pad moves the crosshair, past its edge the view turns
 *     L1 / R1    strafe left / right (C-left / C-right)
 *     L2 / R2    look down / up (C-down / C-up); while aiming: crouch / stand
 *     START      pause
 *   in menus: D-pad = stick, A select, B back, X = Z, Y = R, L1 = L, R1 = R, L2/R2 = C-down/up
 *   SELECT held + D-pad: the N64 D-pad (both)
 *   MENU (Escape) never reaches here: it opens the kit's menu. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "types.h"
#include "constants.h"
#include "bss.h"
#include "input.h"
#include "system.h"

enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_A, MK_B, MK_X, MK_Y, MK_L1, MK_R1, MK_L2, MK_R2, MK_START, MK_SELECT, MK_N };
static volatile u8 sDown[MK_N];
static u64 sDownUs[MK_N];

#define STICK_FULL 80 /* full deflection */
#define AIM_EDGE 60   /* aiming: the crosshair's travel; past it the view turns */
static s32 sTurnStart = 24;   /* stick x the moment a turn key goes down */
static s32 sTurnRampMs = 250; /* ... reaching full deflection after this long */
static f32 sAimRate = 150.f;  /* crosshair stick units per second */
static f32 sAimX, sAimY;
static u64 sLastPollUs;

void controller_handheld_key(int sym, int down)
{
	int k = -1;
	switch (sym) {
	case 273: k = MK_UP; break;        /* SDLK_UP */
	case 274: k = MK_DOWN; break;      /* SDLK_DOWN */
	case 276: k = MK_LEFT; break;      /* SDLK_LEFT */
	case 275: k = MK_RIGHT; break;     /* SDLK_RIGHT */
	case 32: k = MK_A; break;          /* SDLK_SPACE */
	case 306: k = MK_B; break;         /* SDLK_LCTRL */
	case 304: k = MK_X; break;         /* SDLK_LSHIFT */
	case 308: k = MK_Y; break;         /* SDLK_LALT */
	case 101: k = MK_L1; break;        /* SDLK_e */
	case 116: k = MK_R1; break;        /* SDLK_t */
	case 9: k = MK_L2; break;          /* SDLK_TAB */
	case 8: k = MK_R2; break;          /* SDLK_BACKSPACE */
	case 13: k = MK_START; break;      /* SDLK_RETURN */
	case 305: k = MK_SELECT; break;    /* SDLK_RCTRL */
	}
	if (k >= 0 && sDown[k] != (down ? 1 : 0)) {
		sDown[k] = down ? 1 : 0;
		if (down) sDownUs[k] = sysGetMicroseconds();
	}
}

/* PD_PADSCRIPT (tests): "<frame>:<keys>,..." holds keys from that input frame until the next
 * entry: u d l r = d-pad, a b x y, m = L1, n = R1, k = L2, o = R2, s = START, e = SELECT.
 * PD_AUTOPRESS=<frames>: START+A for 3 frames every <frames> frames (walks the title and menus). */
static u32 sFrame;
static int keyOf(char c)
{
	switch (c) {
	case 'u': return MK_UP; case 'd': return MK_DOWN; case 'l': return MK_LEFT; case 'r': return MK_RIGHT;
	case 'a': return MK_A; case 'b': return MK_B; case 'x': return MK_X; case 'y': return MK_Y;
	case 'm': return MK_L1; case 'n': return MK_R1; case 'k': return MK_L2; case 'o': return MK_R2;
	case 's': return MK_START; case 'e': return MK_SELECT;
	}
	return -1;
}
static void padScript(void)
{
	static const char *p = (const char *)1;
	static u32 nextAt = 0;
	static char nextKeys[16];
	if (p == (const char *)1) {
		p = getenv("PD_PADSCRIPT");
		nextAt = 0xffffffff;
		if (p && *p) { nextAt = strtoul(p, (char **)&p, 10); }
	}
	while (p && nextAt != 0xffffffff && sFrame >= nextAt) {
		int n = 0;
		if (*p == ':') p++;
		while (*p && *p != ',' && n < 15) nextKeys[n++] = *p++;
		nextKeys[n] = 0;
		for (int k = 0; k < MK_N; k++) sDown[k] = 0;
		for (int i = 0; i < n; i++) { int k = keyOf(nextKeys[i]); if (k >= 0) { sDown[k] = 1; sDownUs[k] = sysGetMicroseconds(); } }
		if (*p == ',') { p++; nextAt = strtoul(p, (char **)&p, 10); } else nextAt = 0xffffffff;
	}
}

s32 inputInit(void)
{
	const char *e;
	if ((e = getenv("PD_TURN_START")) != NULL) sTurnStart = atoi(e);
	if ((e = getenv("PD_TURN_RAMP_MS")) != NULL) sTurnRampMs = atoi(e);
	if ((e = getenv("PD_AIM_RATE")) != NULL) sAimRate = (f32)atof(e);
	return 1;
}
void inputUpdate(void) { ++sFrame; padScript(); }
s32 inputControllerConnected(s32 idx) { return idx == 0; }
s32 inputControllerMask(void) { return 1; }

/* In a stage with no menu up: the gameplay layout. */
extern s32 g_StageNum;
static bool inGameplay(void)
{
	return g_StageNum != STAGE_TITLE && g_Vars.currentplayer != NULL && g_Menus[0].curdialog == NULL;
}

static s32 turnAmount(int k, u64 now)
{
	const u64 ms = (now - sDownUs[k]) / 1000u;
	if (sTurnRampMs <= 0 || ms >= (u64)sTurnRampMs) return STICK_FULL;
	return sTurnStart + (s32)((STICK_FULL - sTurnStart) * (s64)ms / sTurnRampMs);
}

static s32 aimAxis(f32 *pos, bool neg, bool posk, f32 step)
{
	if (neg && !posk) {
		*pos -= step;
		if (*pos <= -AIM_EDGE) { *pos = -AIM_EDGE; return -STICK_FULL; }
	} else if (posk && !neg) {
		*pos += step;
		if (*pos >= AIM_EDGE) { *pos = AIM_EDGE; return STICK_FULL; }
	}
	return (s32)*pos;
}

static OSContPad sPad;
static u32 sPadFrame = 0xffffffff;

static void buildPad(void)
{
	const u64 now = sysGetMicroseconds();
	f32 dt = sLastPollUs ? (f32)(now - sLastPollUs) / 1e6f : 0.f;
	if (dt > 0.1f) dt = 0.1f;
	sLastPollUs = now;
	const bool l = sDown[MK_LEFT], r = sDown[MK_RIGHT], u = sDown[MK_UP], d = sDown[MK_DOWN];
	const bool sel = sDown[MK_SELECT];
	u32 b = 0;
	s32 sx = 0, sy = 0;

	if (sel) {
		if (u) b |= U_JPAD;
		if (d) b |= D_JPAD;
		if (l) b |= L_JPAD;
		if (r) b |= R_JPAD;
	} else if (inGameplay()) {
		if (sDown[MK_Y]) {
			const f32 step = sAimRate * dt;
			b |= R_TRIG;
			sx = aimAxis(&sAimX, l, r, step);
			sy = aimAxis(&sAimY, d, u, step);
		} else {
			sAimX = sAimY = 0.f;
			if (l && !r) sx = -turnAmount(MK_LEFT, now);
			if (r && !l) sx = turnAmount(MK_RIGHT, now);
			if (u && !d) sy = STICK_FULL;
			if (d && !u) sy = -STICK_FULL;
		}
	} else {
		sAimX = sAimY = 0.f;
		if (l && !r) sx = -STICK_FULL;
		if (r && !l) sx = STICK_FULL;
		if (u && !d) sy = STICK_FULL;
		if (d && !u) sy = -STICK_FULL;
	}

	if (inGameplay()) {
		if (sDown[MK_B]) b |= Z_TRIG;
		if (sDown[MK_A]) b |= B_BUTTON;
		if (sDown[MK_X]) b |= A_BUTTON;
		if (sDown[MK_L1]) b |= L_CBUTTONS;
		if (sDown[MK_R1]) b |= R_CBUTTONS;
	} else {
		if (sDown[MK_A]) b |= A_BUTTON;
		if (sDown[MK_B]) b |= B_BUTTON;
		if (sDown[MK_X]) b |= Z_TRIG;
		if (sDown[MK_Y]) b |= R_TRIG;
		if (sDown[MK_L1]) b |= L_TRIG;
		if (sDown[MK_R1]) b |= R_TRIG;
	}
	if (sDown[MK_L2] && !sDown[MK_R2]) b |= D_CBUTTONS;
	if (sDown[MK_R2]) b |= U_CBUTTONS;
	if (sDown[MK_START]) b |= START_BUTTON;
	{
		static int every = -1;
		if (every < 0) { const char *e = getenv("PD_AUTOPRESS"); every = e ? atoi(e) : 0; }
		if (every > 0 && (sFrame % (u32)every) < 3) b |= START_BUTTON | A_BUTTON;
	}
	memset(&sPad, 0, sizeof(sPad));
	sPad.button = b;
	sPad.stick_x = (s8)sx;
	sPad.stick_y = (s8)sy;
}

s32 inputReadController(s32 idx, OSContPad *npad)
{
	if (!npad || idx != 0) {
		return -1;
	}
	if (sPadFrame != sFrame) { /* one pad per input frame (the aim crosshair integrates dt) */
		buildPad();
		sPadFrame = sFrame;
	}
	*npad = sPad;
	return 0;
}

s32 inputButtonPressed(s32 idx, u32 contbtn)
{
	OSContPad p;
	if (inputReadController(idx, &p) < 0) return 0;
	return (p.button & contbtn) != 0;
}

/* The PC port's keyboard/mouse/rebinding API: no keyboard or mouse on the device. */
s32 inputRumbleSupported(s32 idx) { return 0; }
s32 inputControllerGetSticksSwapped(s32 cidx) { return 0; }
void inputControllerSetSticksSwapped(s32 cidx, s32 swapped) {}
s32 inputControllerGetDualAnalog(s32 cidx) { return 0; }
void inputControllerSetDualAnalog(s32 cidx, s32 enable) {}
s32 inputControllerGetCancelCButtons(s32 cidx) { return 0; }
void inputControllerSetCancelCButtons(s32 cidx, s32 cancel) {}
f32 inputControllerGetAxisScale(s32 cidx, s32 stick, s32 axis) { return 1.f; }
void inputControllerSetAxisScale(s32 cidx, s32 stick, s32 axis, f32 value) {}
f32 inputControllerGetAxisDeadzone(s32 cidx, s32 stick, s32 axis) { return 0.f; }
void inputControllerSetAxisDeadzone(s32 cidx, s32 stick, s32 axis, f32 value) {}
s32 inputGetConnectedControllers(s32 *out) { return 0; }
const char * inputGetConnectedControllerName(s32 id) { return NULL; }
s32 inputGetAssignedControllerId(s32 cidx) { return 0; }
s32 inputAssignController(s32 cidx, s32 id) { return 0; }
s32 inputKeyPressed(u32 vk) { return 0; }
s32 inputKeyJustPressed(u32 vk) { return 0; }
void inputKeyBind(s32 idx, u32 ck, s32 bind, u32 vk) {}
const u32 *inputKeyGetBinds(s32 idx, u32 ck) { static const u32 none[INPUT_MAX_BINDS]; return none; }
s32 inputGetKeyByName(const char *name) { return 0; }
const char * inputGetKeyName(s32 vk) { return NULL; }
s32 inputGetContKeyByName(const char *name) { return 0; }
const char * inputGetContKeyName(u32 ck) { return NULL; }
void inputRumble(s32 idx, f32 strength, f32 time) {}
f32 inputRumbleGetStrength(s32 cidx) { return 0.f; }
void inputRumbleSetStrength(s32 cidx, f32 val) {}
void inputLockMouse(s32 lock) {}
s32 inputMouseIsLocked(void) { return 0; }
s32 inputMouseGetPosition(s32 *x, s32 *y) { return 0; }
void inputMouseGetRawDelta(s32 *dx, s32 *dy) {}
void inputMouseGetScaledDelta(f32 *dx, f32 *dy) {}
void inputMouseGetAbsScaledDelta(f32 *dx, f32 *dy) {}
void inputMouseGetSpeed(f32 *x, f32 *y) { *x = *y = 1.f; }
void inputMouseSetSpeed(f32 x, f32 y) {}
s32 inputMouseIsEnabled(void) { return 0; }
void inputMouseEnable(s32 enabled) {}
void inputSaveBinds(void) {}
void inputSetDefaultKeyBinds(s32 cidx, s32 n64mode) {}
void inputClearLastKey(void) {}
s32 inputGetLastKey(void) { return 0; }
s32 inputGetMouseLockMode(void) { return 0; }
void inputSetMouseLockMode(s32 lockmode) {}
s32 inputAutoLockMouse(s32 wantlock) { return 0; }
void inputMouseShowCursor(s32 show) {}
void inputStartTextInput(void) {}
void inputStopTextInput(void) {}
s32 inputIsTextInputActive(void) { return 0; }
void inputClearLastTextChar(void) {}
char inputGetLastTextChar(void) { return 0; }
s32 inputTextHandler(char *out, const u32 outSize, s32 *curCol, s32 oskCharsOnly) { return 0; }
void inputClearClipboard(void) {}
const char * inputGetClipboard(void) { return NULL; }
u32 inputGetKeyModState(void) { return 0; }
