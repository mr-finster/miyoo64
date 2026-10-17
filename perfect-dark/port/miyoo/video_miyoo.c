/* video_miyoo.c -- the PC port's video.h on the GoldenEye kit's software renderer
 * (port/miyoo/gfx: gfx_pc.c interpreter + gfx_soft.c rasterizer + gfx_sdl12.c present).
 * Replaces port/src/video.c (SDL2 + OpenGL). The N64 picture is 320x240 (PD draws 320x220
 * in its hi-res-off modes); the present doubles it to the 640x480 panel. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "video.h"
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "bss.h"
#include "config.h"
#include "system.h"
#include "gfx/gfx_pc.h"
#include "gfx/gfx_rendering_api.h"
#include "gfx/gfx_window_manager_api.h"

extern struct GfxRenderingAPI gfx_soft_api;
extern struct GfxWindowManagerAPI gfx_sdl;

extern void audioPerfLine(char *buf, u32 size); /* audio_miyoo.c: the aud fields, their units */
static bool initDone;
static void applyTexFilter(void);
static s32 nativeW = 320, nativeH = 220;
static u32 frames;
static f32 avgFps;
static u64 fpsT0;
static u32 fpsN;

/* PERF: one line per 60 frames: the frame interval (wall), the render (display list
 * interpretation + raster + present hand-off: submit through end), and the rest (game logic,
 * audio mixing). PD_PERF=0: off. */
static u64 perfT0, perfRenderUs, perfMaxRenderUs, perfLastStart, perfIntervalUs, perfMaxIntervalUs;
static u32 perfN;
static u64 frameRenderUs, submitT0;
extern s32 g_StageNum;
u64 g_PerfAudioUs, g_PerfSleepUs; /* pdsched.c schedAudioFrame, pdmain.c's pacer sleeps */

/* VmRSS / VmHWM / VmSwap from /proc/self/status, for the PERF line (the Mini has ~64 MB for Linux). */
static const char *memLine(void)
{
	static char out[96];
	char line[128];
	long rss = -1, hwm = -1, swp = -1;
	FILE *f = fopen("/proc/self/status", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "VmRSS: %ld", &rss);
			sscanf(line, "VmHWM: %ld", &hwm);
			sscanf(line, "VmSwap: %ld", &swp);
		}
		fclose(f);
	}
	snprintf(out, sizeof(out), "VmRSS %ld kB VmHWM %ld kB VmSwap %ld kB", rss, hwm, swp);
	return out;
}

s32 videoInit(void)
{
	gfx_init(&gfx_sdl, &gfx_soft_api, "Perfect Dark", false);
	{ extern int gfx_dump_textures; const char *e = getenv("PD_DUMPTEX"); if (e && e[0] == '1') gfx_dump_textures = 1; }
	{
		/* N64 controls (1.1) under the handheld layout: never the PC port's extended ones, whatever
		 * an older pd.ini says (videoInit runs after configInit) */
		extern struct extplayerconfig g_PlayerExtCfg[];
		for (s32 i = 0; i < MAX_PLAYERS; ++i) {
			g_PlayerExtCfg[i].extcontrols = 0;
		}
	}
	{
		extern int miyoo_clock_boost_to(int mhz);
		const int mhz = miyoo_clock_boost_to(-1);
		sysLogPrintf(LOG_NOTE, "cpu clock: %d MHz%s", mhz, mhz ? "" : " (left alone)");
	}
	{
		/* how long a 100 us sleep really takes here (the pacer's unit; a jiffy-bound kernel rounds up) */
		u64 t0 = sysGetMicroseconds();
		for (s32 i = 0; i < 10; ++i) sysSleep(1000);
		sysLogPrintf(LOG_NOTE, "sleep(100us) takes %.0f us", (sysGetMicroseconds() - t0) / 10.0);
	}
	applyTexFilter();
	initDone = true;
	fpsT0 = sysGetMicroseconds();
	return 0;
}

void videoStartFrame(void)
{
	if (initDone) {
		const u64 now = sysGetMicroseconds();
		if (perfLastStart) {
			const u64 iv = now - perfLastStart;
			perfIntervalUs += iv;
			if (iv > perfMaxIntervalUs) perfMaxIntervalUs = iv;
		}
		perfLastStart = now;
		frameRenderUs = 0;
		submitT0 = now;
		gfx_start_frame();
		frameRenderUs += sysGetMicroseconds() - submitT0;
	}
}

void videoSubmitCommands(Gfx *cmds)
{
	if (initDone) {
		const u64 t0 = sysGetMicroseconds();
		gfx_run(cmds);
		frameRenderUs += sysGetMicroseconds() - t0;
	}
}

void videoEndFrame(void)
{
	if (!initDone) {
		return;
	}
	{
		const u64 t0 = sysGetMicroseconds();
		gfx_end_frame();
		/* PD_SLOWFRAME=<ms> (tests): every frame takes at least that long (the audio check) */
		static s32 slow = -1;
		if (slow < 0) { const char *e = getenv("PD_SLOWFRAME"); slow = e ? atoi(e) : 0; }
		if (slow > 0) {
			const u64 spent = sysGetMicroseconds() - t0 + frameRenderUs;
			if (spent < (u64)slow * 1000) SDL_Delay((u32)((slow * 1000 - spent) / 1000));
		}
		frameRenderUs += sysGetMicroseconds() - t0;
	}
	perfRenderUs += frameRenderUs;
	if (frameRenderUs > perfMaxRenderUs) perfMaxRenderUs = frameRenderUs;
	if (++perfN == 60) {
		static s32 on = -1;
		if (on < 0) { const char *e = getenv("PD_PERF"); on = !(e && e[0] == '0'); }
		if (on) {
			const f64 iv = perfIntervalUs / 59.0 / 1000.0, rd = perfRenderUs / 60.0 / 1000.0;
			const f64 au = g_PerfAudioUs / 60.0 / 1000.0, sl = g_PerfSleepUs / 60.0 / 1000.0;
			char aud[160];
			audioPerfLine(aud, sizeof(aud));
			sysLogPrintf(LOG_NOTE, "PERF stage 0x%02x frame %u | interval avg %.2f max %.2f ms | render avg %.2f max %.2f ms"
				" | audio %.2f pace %.2f logic+dl %.2f ms | %s | %s",
				g_StageNum, frames, iv, perfMaxIntervalUs / 1000.0, rd, perfMaxRenderUs / 1000.0,
				au, sl, iv - rd - au - sl > 0 ? iv - rd - au - sl : 0.0, aud, memLine());
		}
		perfN = 0;
		perfRenderUs = perfMaxRenderUs = perfIntervalUs = perfMaxIntervalUs = 0;
		g_PerfAudioUs = g_PerfSleepUs = 0;
	}
	++frames;
	++fpsN;
	{
		/* the kit's frame boundary: EXIT GAME (menu) and the OS's SIGTERM quit from here.
		 * TODO: the game heap is outside the state, so SAVE/LOAD STATE are not usable yet. */
		extern void savestate_check(void);
		extern void savestate_request_save(int slot), savestate_request_load(int slot);
		/* PD_TEST_SAVEAT / PD_TEST_LOADAT=<frame> (tests): slot 1 at that frame */
		static long saveAt = -2, loadAt = -2;
		if (saveAt == -2) {
			const char *e = getenv("PD_TEST_SAVEAT"); saveAt = e ? atol(e) : -1;
			e = getenv("PD_TEST_LOADAT"); loadAt = e ? atol(e) : -1;
		}
		if ((long)frames == saveAt) savestate_request_save(1);
		if ((long)frames == loadAt) savestate_request_load(1);
		savestate_check();
	}
	{
		/* pd.ini when a menu closes (the extended options persist even if the OS later kills the game
		 * outright, which skips the save at exit) */
		extern struct menu g_Menus[];
		static void *prevDialog;
		void *cur = g_Menus[0].curdialog;
		if (prevDialog && !cur) {
			configSave(CONFIG_PATH);
		}
		prevDialog = cur;
	}
	const u64 now = sysGetMicroseconds();
	if (now - fpsT0 >= 1000000) {
		avgFps = (f32)fpsN * 1000000.f / (f32)(now - fpsT0);
		fpsN = 0;
		fpsT0 = now;
	}
}

void videoClearScreen(void) {}
void *videoGetWindowHandle(void) { return NULL; }

void videoUpdateNativeResolution(s32 w, s32 h)
{
	nativeW = w;
	nativeH = h;
}

s32 videoGetNativeWidth(void) { return nativeW; }
s32 videoGetNativeHeight(void) { return nativeH; }
s32 videoGetWidth(void) { return 640; }
s32 videoGetHeight(void) { return 480; }
/* The game scales its projection by this; the panel is 4:3 like the N64's TV. */
f32 videoGetAspect(void) { return 4.f / 3.f; }

s32 videoGetFullscreen(void) { return 1; }
s32 videoGetFullscreenMode(void) { return 0; }
s32 videoGetMaximizeWindow(void) { return 0; }
void videoSetMaximizeWindow(s32 fs) { (void)fs; }
s32 videoGetCenterWindow(void) { return 0; }
void videoSetCenterWindow(s32 center) { (void)center; }
/* The extended video options this renderer has (port/src/optionsmenu.c hides the rest), kept in pd.ini
 * under the PC port's keys. Texture filtering: 0 nearest, 1 bilinear, 2 (three point) as bilinear. */
static s32 texFilter = 1;
static f32 vidGlareBrightness = 1.f, vidOverexposureScale = 1.f;
static void applyTexFilter(void) { extern int gfx_soft_bilinear; gfx_soft_bilinear = texFilter != 0; }
u32 videoGetTextureFilter(void) { return texFilter; }
s32 videoGetTextureFilter2D(void) { return 1; }
u32 videoGetAnisotropicFilter(void) { return 0; }
u32 videoGetMaxAnisotropyLevel(void) { return 0; }
s32 videoGetDetailTextures(void) { return 1; }
s32 videoGetDisplayModeIndex(void) { return 0; }
s32 videoGetDisplayMode(displaymode *out, const s32 index)
{
	if (index != 0 || !out) {
		return 0;
	}
	out->width = 640;
	out->height = 480;
	return 1;
}
s32 videoGetNumDisplayModes(void) { return 1; }
s32 videoGetVsync(void) { return 0; }
s32 videoGetFramerateLimit(void) { return 0; }
s32 videoGetDisplayFPS(void) { return 0; }
s32 videoGetMSAA(void) { return 1; }
f32 videoGetGlareBrightness(void) { return vidGlareBrightness; }
f32 videoGetOverexposureScale(void) { return vidOverexposureScale; }
f32 videoGetAverageFPS(void) { return avgFps; }

void videoSetWindowOffset(s32 x, s32 y) { (void)x; (void)y; }
void videoSetFullscreen(s32 fs) { (void)fs; }
void videoSetFullscreenMode(s32 mode) { (void)mode; }
void videoSetTextureFilter(u32 filter) { texFilter = filter > 2 ? 1 : (s32)filter; applyTexFilter(); }
void videoSetTextureFilter2D(s32 filter) { (void)filter; }
void videoSetAnisotropicFilter(u32 filter) { (void)filter; }
void videoSetDetailTextures(s32 detail) { (void)detail; }
void videoSetDisplayMode(const s32 index) { (void)index; }
void videoSetVsync(const s32 vsync) { (void)vsync; }
void videoSetFramerateLimit(const s32 limit) { (void)limit; }
void videoSetDisplayFPS(const s32 displayfps) { (void)displayfps; }
void videoSetMSAA(const s32 msaa) { (void)msaa; }
void videoSetGlareBrightness(f32 bright) { vidGlareBrightness = bright; }
void videoSetOverexposureScale(f32 scale) { vidOverexposureScale = scale; }

/* Offscreen framebuffers: not yet (port/miyoo/gfx/pd_fbext.h). -1 and "unsupported" make the
 * game take its plain paths (bondview.c, menugfx.c). */
#define MAX_FBS 32
static struct { u8 *px; s32 w, h; } sFbs[MAX_FBS]; /* id = index + 1 */
/* The pixels in .bss beside sFbs, not calloc'd: a save state holds sFbs, and a calloc'd pointer from the
 * run that wrote it means nothing in a later run. Sparse states skip the unused (zero) pages. */
static u8 sFbPixels[MAX_FBS][64 * 64 * 2] __attribute__((aligned(16)));

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize)
{
	if (autoresize && w == 0 && h == 0) {
		/* screen-sized: kept by the soft renderer at its own size (gfx_soft_scr_*), ids 100, 101 */
		static s32 nscr;
		return nscr < 2 ? 100 + nscr++ : -1;
	}
	if (w == 0 || h == 0 || autoresize || w * h > 64 * 64) {
		return -1;
	}
	for (s32 i = 0; i < MAX_FBS; ++i) {
		if (!sFbs[i].px) {
			sFbs[i].px = sFbPixels[i];
			memset(sFbs[i].px, 0, w * h * 2);
			sFbs[i].w = w;
			sFbs[i].h = h;
			return i + 1;
		}
	}
	return -1;
}

uint8_t *videoFbPixels(int fb, int *w, int *h)
{
	if (fb < 1 || fb > MAX_FBS || !sFbs[fb - 1].px) return NULL;
	*w = sFbs[fb - 1].w;
	*h = sFbs[fb - 1].h;
	return sFbs[fb - 1].px;
}
void videoSetFramebuffer(s32 target) { (void)target; }
void videoResetFramebuffer(void) {}
void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top)
{
	extern void gfx_soft_read_rgba16(int x, int y, int w, int h, uint8_t *dst);
	int w, h;
	uint8_t *px = videoFbPixels(dst, &w, &h);
	if (dst >= 100 && dst < 102 && src == 0) { extern void gfx_soft_scr_capture(int slot); gfx_soft_scr_capture(dst - 100); return; }
	if (px && src == 0) gfx_soft_read_rgba16(left < 0 ? 0 : left, top < 0 ? 0 : top, w, h, px);
}
void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize) {}
s32 videoFramebuffersSupported(void) { return 0; }

/* The texture cache keys by content (gfx_pc.c), so freed or reused texture memory never serves
 * stale pixels: these need nothing. */
void videoResetTextureCache(void) {}
void videoFreeCachedTexture(const void *texptr) { (void)texptr; }
void videoFreeCachedTextures(const void *start, const void *end) { (void)start; (void)end; }

void videoShutdown(void) {}

/* The menu blur (menugfx.c): the last rendered frame averaged down to w x h RGBA5551, big-endian
 * as the N64's g_BlurBuffer. */
void videoCaptureBlur(u8 *dst, s32 w, s32 h)
{
	extern uint32_t *gfx_soft_get_framebuffer(int *w, int *h);
	int fw = 0, fh = 0;
	const uint32_t *fb = gfx_soft_get_framebuffer(&fw, &fh);
	if (!dst) return;
	if (!fb || fw < w || fh < h) {
		memset(dst, 0, (size_t)w * h * 2);
		return;
	}
	for (s32 y = 0; y < h; ++y) {
		const s32 y0 = y * fh / h, y1 = (y + 1) * fh / h;
		for (s32 x = 0; x < w; ++x) {
			const s32 x0 = x * fw / w, x1 = (x + 1) * fw / w;
			u32 r = 0, g = 0, b = 0, n = 0;
			for (s32 sy = y0; sy < y1; ++sy) {
				const uint32_t *row = fb + sy * fw;
				for (s32 sx = x0; sx < x1; ++sx) {
					const uint32_t p = row[sx];
					r += (p >> 16) & 0xff; g += (p >> 8) & 0xff; b += p & 0xff; ++n;
				}
			}
			if (n == 0) n = 1;
			const u16 c = (u16)(((r / n) >> 3) << 11 | ((g / n) >> 3) << 6 | ((b / n) >> 3) << 1 | 1);
			dst[(y * w + x) * 2 + 0] = c >> 8;
			dst[(y * w + x) * 2 + 1] = c & 0xff;
		}
	}
}

PD_CONSTRUCTOR static void videoConfigInit(void)
{
	configRegisterInt("Video.TextureFilter", &texFilter, 0, 2);
	configRegisterFloat("Video.GlareBrightness", &vidGlareBrightness, 0.f, 1.f);
	configRegisterFloat("Video.OverexposureScale", &vidOverexposureScale, 0.f, 1.f);
}
