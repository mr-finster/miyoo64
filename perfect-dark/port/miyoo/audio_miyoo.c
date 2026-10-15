/* audio_miyoo.c -- the PC port's audio.h on SDL 1.2 (callback + ring buffer; SDL 1.2 has no
 * SDL_QueueAudio). Replaces port/src/audio.c. The game mixes 22020 Hz stereo s16 on the CPU
 * (port/src/mixer.c). PD_NOAUDIO=1: no device (qemu).
 *
 * PD_AUDRATE (default 32000, 8000..48000): the rate the card is opened at. Asked for 22020, the
 * Mini's SDL opens it, but its mi_ao stays at 48000 ("MI_AO_SetPubAttr failed") and the driver
 * converts at a rate of its own; every released Miyoo port opens 32000 and resamples itself. The
 * game's frames are resampled here as they are pushed (linear, the phase and the last frame
 * carried across buffers, as GoldenEye's hh_audio_sdl.c), so the ring holds output-rate frames.
 * PD_AUDRATE=22020 with PD_AUDRATECTL=0 and PD_AUDHEADROOM=0: the step is exactly one frame and
 * the ring a copy of the game's buffers -- the old direct path, bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"
#include "kit/savestate.h"
#include "kit/hh_declick.h"

#define GAME_RATE 22020
#define RING_FRAMES 32768 /* output frames: 1.5 s at 22020, 0.68 s at 48000 (the drop limit tops out at 10304 game frames) */

/* live output state, kept out of save states (the kit's "dontsave" hole) */
static s16 sRing[RING_FRAMES * 2] SAVESTATE_EXCLUDE;
static volatile u32 sHead SAVESTATE_EXCLUDE, sTail SAVESTATE_EXCLUDE; /* output frames written / read, free-running */
static s32 sOpen SAVESTATE_EXCLUDE;
static s32 sOutRate SAVESTATE_EXCLUDE = GAME_RATE;
static volatile u32 sCbFrames SAVESTATE_EXCLUDE = 512; /* output frames a callback takes */
static u32 sPhase SAVESTATE_EXCLUDE = 0x10000;         /* 16.16: the next output frame; 0 = sPrev, k = the buffer's k-1 */
static s16 sPrevL SAVESTATE_EXCLUDE, sPrevR SAVESTATE_EXCLUDE; /* the last frame of the previous buffer */
static const s16 *nextBuf;
static u32 nextSize;
static s32 queueLimit = 4096; /* samples (game frames): ~186 ms */
/* Audio.Gain (pd.ini, percent, default 80): the output level. PD's mix is hot (the intro averages about -10 dBFS and
 * peaks at full scale), and the Mini's small speaker distorts at the top -- the TEST14 report of pops "like
 * clipping" with no dropouts left. 100: as the game mixes it (TEST14). Applied after the resampler. */
static s32 audioGain = 80;
/* Audio.BassCut (pd.ini, Hz, default 100, 0 = off): a 2nd-order high-pass before the speaker. The intro's mix has
 * nearly all its energy below 500 Hz (measured against the N64 reference: same spectrum, same level once the
 * emulator's 80% volume is taken out), and the Mini's speaker buzzes on it -- "still a bit crackly at the start"
 * (TEST15). Below ~100 Hz the speaker plays nothing anyway. Butterworth biquad at the output rate, per channel. */
static s32 audioBassCut = 100;
static struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; s32 hz, rate; } sHp SAVESTATE_EXCLUDE;
static inline s32 hpRun(float x, int c)
{
	const float y = sHp.b0 * x + sHp.z1[c];
	sHp.z1[c] = sHp.b1 * x - sHp.a1 * y + sHp.z2[c];
	sHp.z2[c] = sHp.b2 * x - sHp.a2 * y;
	return (s32)y;
}
static void hpSetup(void)
{
	if (sHp.hz == audioBassCut && sHp.rate == sOutRate) return;
	sHp.hz = audioBassCut; sHp.rate = sOutRate;
	sHp.z1[0] = sHp.z1[1] = sHp.z2[0] = sHp.z2[1] = 0;
	if (audioBassCut <= 0) return;
	const float w = 2.0f * 3.14159265f * (float)audioBassCut / (float)sOutRate, cw = cosf(w), al = sinf(w) / (2.0f * 0.7071068f);
	const float a0 = 1.0f + al;
	sHp.b0 = (1.0f + cw) / 2.0f / a0; sHp.b1 = -(1.0f + cw) / a0; sHp.b2 = sHp.b0;
	sHp.a1 = -2.0f * cw / a0; sHp.a2 = (1.0f - al) / a0;
}

/* PD_AUDFILL (default 1): the game's mixer (audiomgr.c amgrFrame) makes 2 naudio frames (368
 * samples) per 60 Hz step until the ring holds more than its mark, then 1. The original mark was
 * 1100 samples (50 ms): at 60-130 ms a frame the ring must last the whole next frame, so it ran
 * dry every slow frame (the device's stutter when the fps drops). The mark follows the slowest
 * recent frame (steps held, decaying one step a second) plus 2 steps; the drop limit sits above it.
 * The mark and the drop limit are in game (22020 Hz) frames; the ring and the callback in output
 * frames. The cushion and fades of the kit's hh_declick (GoldenEye's fk_declick) smooth any gap left. */
static s32 sFill = -1;
static s32 sPeakSteps = 1, sPeakAge;
static HhDeclick sDeclick SAVESTATE_EXCLUDE;
volatile u32 g_AudDry SAVESTATE_EXCLUDE, g_AudDrop SAVESTATE_EXCLUDE, g_AudCalls SAVESTATE_EXCLUDE;
volatile u32 g_AudMinFill SAVESTATE_EXCLUDE = 0xffffffff; /* output frames */
static volatile u32 sReqFrames SAVESTATE_EXCLUDE; /* output frames the card has asked for */
static volatile u64 sCbUs SAVESTATE_EXCLUDE;      /* when it last asked */

/* PD_AUDHEADROOM=<game frames> (default: one callback, 352 at 32000): the game is told the fill
 * less this (osAiGetLength), so amgrFrame keeps that much more queued (OoT's audiofill). At 60 fps
 * the mark (1104) left a 512-frame callback about one callback of margin: a frame a few ms late
 * ran it dry. 0: the whole fill, as before. */
static s32 sHeadroom = -2; /* -2: not read yet; -1: one callback */

/* PD_AUDRATECTL (default 1): the game's clock and the card's never quite match, and SDL drains a
 * whole callback at a time. Once playing, the step moves by up to +-0.5% (5000 ppm, RetroArch's
 * dynamic rate control delta, inaudible) toward the fill the game aims for (the mark plus the
 * headroom). PD_AUDSTRETCH (default 1): below half of that the step keeps shrinking, to -4% at
 * empty, so a run of slow frames slows the sound a few percent instead of cutting it (GoldenEye's
 * rounds 19 and 42). PD_AUDRATECTL=0: a fixed step. */
static s32 sRateCtl = -1, sStretch = -1;

static s32 envOn(const char *name)
{
	const char *e = getenv(name);
	return !(e && e[0] == '0');
}

static s32 fillOn(void)
{
	if (sFill < 0) sFill = envOn("PD_AUDFILL");
	return sFill;
}

static u32 toOut(u32 game) { return (u32)((u64)game * (u32)sOutRate / GAME_RATE); }
static u32 toGame(u32 out) { return (u32)((u64)out * GAME_RATE / (u32)sOutRate); }

static u32 headroom(void)
{
	if (sHeadroom == -2) {
		const char *e = getenv("PD_AUDHEADROOM");
		sHeadroom = e && e[0] ? atoi(e) : -1;
		if (sHeadroom < -1) sHeadroom = -1;
	}
	return sHeadroom >= 0 ? (u32)sHeadroom : toGame(sCbFrames);
}

void audioSetFrameSteps(s32 steps)
{
	if (steps > 12) steps = 12; /* a load or a hitch: not the pace to keep */
	if (steps >= sPeakSteps) { sPeakSteps = steps; sPeakAge = 0; }
	else if (++sPeakAge >= 60 / (sPeakSteps > 0 ? sPeakSteps : 1)) { sPeakSteps--; sPeakAge = 0; }
}

s32 audioFillMark(void)
{
	if (!fillOn()) return 1100;
	const s32 m = (sPeakSteps + 2) * 368;
	return m > 1100 ? m : 1100;
}

static s32 dropLimit(void)
{
	const s32 m = audioFillMark() * 2; /* above the mark the mixer makes ~307 a step: it settles */
	return m > queueLimit ? m : queueLimit;
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	s16 *out = (s16 *)stream;
	const u32 want = (u32)len / 4, tail = sTail;
	const u32 avail = sHead - tail;
	__sync_synchronize(); /* the frames the head covers are seen before they are read (the A7 has two cores) */
	g_AudCalls++;
	sReqFrames += want;
	sCbUs = sysGetMicroseconds();
	sCbFrames = want;
	if (avail < g_AudMinFill) g_AudMinFill = avail;
	if (fillOn() && !hhd_ready(&sDeclick, avail, want)) {
		memset(stream, 0, len);
		return;
	}
	u32 n = want < avail ? want : avail;
	for (u32 i = 0; i < n; ++i) {
		const u32 k = (tail + i) % RING_FRAMES;
		out[i * 2] = sRing[k * 2];
		out[i * 2 + 1] = sRing[k * 2 + 1];
	}
	if (n < want) {
		memset(out + n * 2, 0, (want - n) * 4);
	}
	__sync_synchronize(); /* read before the producer may reuse the space */
	sTail = tail + n;
	if (fillOn()) {
		if (hhd_apply(&sDeclick, out, n, want, sDeclick.hold)) g_AudDry++;
	} else {
		static int had;
		if (n < want && had) g_AudDry++;
		had = n == want;
	}
}

static void sleepMs(uint32_t ms) { SDL_Delay(ms); }

/* the kit's savestate.c around a load: fade out, then drop the old timeline's queued sound */
void port_audio_out_gap(int begin)
{
	if (!sOpen) return;
	if (begin) { hhd_hold(&sDeclick); return; }
	hhd_settle(&sDeclick, sleepMs, 50);
	SDL_LockAudio();
	sTail = sHead;
	hhd_flush(&sDeclick);
	sDeclick.hold = 0;
	SDL_UnlockAudio();
}

s32 audioInit(void)
{
	const char *e = getenv("PD_NOAUDIO");
	if (e && e[0] == '1') {
		return -1;
	}
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}
	e = getenv("PD_AUDRATE");
	sOutRate = e ? atoi(e) : 32000;
	if (sOutRate < 8000 || sOutRate > 48000) sOutRate = 32000;
	sRateCtl = envOn("PD_AUDRATECTL");
	sStretch = envOn("PD_AUDSTRETCH");
	SDL_AudioSpec want;
	memset(&want, 0, sizeof(want));
	want.freq = sOutRate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	want.callback = audio_cb;
	sCbFrames = want.samples;
	hhd_init(&sDeclick, sOutRate, toOut(1100));
	if (SDL_OpenAudio(&want, NULL) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}
	sysLogPrintf(LOG_NOTE, "audio: %d Hz in, %d Hz out, rate control %d stretch %d, headroom %u game frames, gain %d%%, bass cut %d Hz",
		GAME_RATE, sOutRate, sRateCtl, sStretch, headroom(), audioGain, audioBassCut);
	sOpen = 1;
	SDL_PauseAudio(0);
	return 0;
}

/* the game's view (osAiGetLength): the fill in game frames, less the headroom */
s32 audioGetBytesBuffered(void)
{
	if (!sOpen) return 0;
	const u32 fill = toGame(sHead - sTail), hr = headroom();
	return (s32)((fill > hr ? fill - hr : 0) * 4);
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

/* the step (16.16 game frames per output frame) for a push that finds fill output frames queued */
static u32 stepFor(u32 fill)
{
	const u32 step = (u32)(((u64)GAME_RATE << 16) / (u32)sOutRate);
	if (!sRateCtl || (fillOn() && !sDeclick.primed)) return step;
	const s32 target = (s32)toOut((u32)audioFillMark() + headroom());
	const s32 err = (s32)fill - target; /* frames above the target */
	s32 ppm = (s32)((s64)err * 5000 / target); /* +-100% of the target = +-0.5% */
	if (ppm > 5000) ppm = 5000;
	if (ppm < -5000) ppm = -5000;
	const s32 half = target / 2;
	if (sStretch && err < -half && half > 0) {
		s32 deep = -err - half; /* 0 .. half */
		if (deep > half) deep = half;
		ppm = -5000 - (s32)((s64)deep * 35000 / half);
	}
	return (u32)((s64)step * (1000000 + ppm) / 1000000);
}

/* linear resample the game's n frames into the ring (a full ring drops the newest, as before) */
static void push(const s16 *s, u32 n, u32 fill)
{
	hpSetup();
	const u32 step = stepFor(fill), room = RING_FRAMES - fill, head = sHead;
	u32 out = 0;
	for (;;) {
		const u32 k = sPhase >> 16, f = sPhase & 0xffff;
		if (k > n || (k == n && f)) break;
		if (out < room) {
			s32 l = k ? s[(k - 1) * 2] : sPrevL, r = k ? s[(k - 1) * 2 + 1] : sPrevR;
			if (f) {
				l += (s32)(((s64)(s[k * 2] - l) * f) >> 16);
				r += (s32)(((s64)(s[k * 2 + 1] - r) * f) >> 16);
			}
			if (audioBassCut > 0) { l = hpRun((float)l, 0); r = hpRun((float)r, 1); }
			if (audioGain != 100) { l = l * audioGain / 100; r = r * audioGain / 100; }
			if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
			if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
			const u32 w = (head + out) % RING_FRAMES;
			sRing[w * 2] = (s16)l;
			sRing[w * 2 + 1] = (s16)r;
			out++;
		}
		sPhase += step;
	}
	sPhase -= n << 16;
	if (n) { sPrevL = s[(n - 1) * 2]; sPrevR = s[(n - 1) * 2 + 1]; }
	__sync_synchronize();
	sHead = head + out;
}

void audioEndFrame(void)
{
	{ /* PD_AUDRAW=<file> (tests): the game's mix as it comes, 22020 Hz stereo s16 LE, before the resampler */
		static FILE *f SAVESTATE_EXCLUDE;
		static s32 on SAVESTATE_EXCLUDE = -1;
		if (on < 0) { const char *e = getenv("PD_AUDRAW"); on = e && e[0]; if (on) f = fopen(e, "wb"); if (!f) on = 0; }
		if (on && nextBuf && nextSize) fwrite(nextBuf, 1, nextSize, f);
	}
	if (nextBuf && nextSize && sOpen) {
		const u32 fill = sHead - sTail;
		if ((s32)toGame(fill) >= dropLimit()) {
			g_AudDrop++;
		} else {
			push(nextBuf, nextSize / 4, fill);
		}
	}
	nextBuf = NULL;
	nextSize = 0;
}

/* The PERF line's audio fields (video_miyoo.c, every 60 frames); restarts the window's counters.
 * dry: callbacks that ran dry, drop: game buffers dropped at the limit (both since boot); minfill:
 * the lowest fill a callback found this window, in output-rate frames (a callback takes cb's size
 * of those, 512); mark: the game's fill mark, in game (22020 Hz) frames; rate: the card's rate;
 * cb: callbacks this window; drain: output frames per second the card asked for between the
 * window's last callbacks (its real clock); gaps: restarts after a gap (declick fade-ins) this window. */
void audioPerfLine(char *buf, u32 size)
{
	/* outside the state: a load restored the writer's and the next window read garbage (TEST14 device) */
	static u32 lastCalls SAVESTATE_EXCLUDE, lastReq SAVESTATE_EXCLUDE, lastGaps SAVESTATE_EXCLUDE;
	static u64 lastUs SAVESTATE_EXCLUDE;
	if (sOpen) SDL_LockAudio();
	const u32 calls = g_AudCalls, req = sReqFrames, gaps = sDeclick.gaps;
	const u64 us = sCbUs;
	if (sOpen) SDL_UnlockAudio();
	const u32 drain = lastUs && us > lastUs ? (u32)((u64)(req - lastReq) * 1000000 / (us - lastUs)) : 0;
	snprintf(buf, size, "aud dry %u drop %u minfill %d mark %d rate %d cb %u drain %u gaps %u",
		g_AudDry, g_AudDrop, g_AudMinFill == 0xffffffffu ? -1 : (s32)g_AudMinFill, audioFillMark(),
		sOpen ? sOutRate : 0, calls - lastCalls, drain, gaps - lastGaps);
	g_AudMinFill = 0xffffffffu;
	lastCalls = calls;
	lastReq = req;
	lastGaps = gaps;
	lastUs = us;
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.Gain", &audioGain, 10, 100);
	configRegisterInt("Audio.BassCut", &audioBassCut, 0, 400);
}
