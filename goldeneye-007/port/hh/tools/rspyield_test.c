/* rspyield_test.c: the sound across slow frames, with and without GE_RSPYIELD (2026-10-02, the user: at the Dam
 * "the explosions and smoke are so slow and dropped fps that it started to skip sound").
 *
 * The real kernel (hh/hh_os.c + hh_ctx.S), the real hardware layer (hh/hh_hw.c: the AI FIFO and its catch-up, the
 * VI), the real output ring (hh/audio/hh_audio_sdl.c, SDL stubbed below: a host thread plays the card, 512 frames
 * every 16 ms) and the game's real scheduler (src/sched.c), driven by two stand-in game threads:
 *   - "main" (priority 10, as boss.c's loop): on a retrace, with fewer than two frames pending and two retraces since
 *     the last, runs ~8 ms of logic (an OS call every 0.5 ms, as game code makes), writes the frame's "display list"
 *     (a buffer stamped with the frame number) and sends its graphics task to the scheduler (rsp.c's flags);
 *   - "audio" (priority 20, as audi.c's amMain with GE_AUDDIRECT): every retrace the scheduler hands it, queues the
 *     last frame to the AI and sizes the next exactly as amHandleFrameMessage does ((g_FrameSize - AI length) + 16 +
 *     EXTRA_SAMPLES, rounded down to 16, at least the minimum), "mixing" a continuous counter (one value a sample) in
 *     ~1.5 ms;
 * and a graphics task (hh_plat_rsp_task) that "draws" for the frame's planned time, reading its display list and
 * polling hh_rsp_poll every 0.5 ms as gfx_pc.c / gfx_soft.c do. The plan: 45 ms frames, every 25th 220 ms (a burst
 * of explosions), and a stretch of 130 ms frames (the death's 8-12 fps).
 * RY_PLAIN=1: every frame 45 ms (the default must not yield: such frames run as before).
 * Checks, both modes:
 *   - the game's sound is the same samples: the AI stream is the counter, continuous, none dropped or repeated;
 *   - no main-thread code runs while a graphics task is in flight, and every task reads the display list its frame
 *     wrote, unchanged from its first read to its last (what is drawn is unchanged);
 *   - underruns of the card ("aud dry"): the old kernel (GE_RSPYIELD=0) against the default.
 * Run: sh port/hh/tools/rspyield_test.sh [seconds] */
#include <ultra64.h>
#include <PR/os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL/SDL.h>
#include "sched.h"
#include "hh_os.h"

/* ---- what the real files need from the rest of the port ---- */
uint32_t gPortProfFrame[16];
extern OSSched os_scheduler;
OSViMode osViModeTable[64];
OSViMode *viMode;
u32 g_viOriginalHstart, g_viOriginalVstart0, g_viOriginalVstart1;
void *setSPToEnd(void *sp, u32 size) { return (u8 *)sp + size; }
u8 sp_shed[0x1000];
void speedgraphMarkerUpdate(void) {}
void speedgraphMarkerHandler(u32 m) { (void)m; }
void viVsyncRelated(void) {}
void joyPoll(void) {}
void joyRumblePakStop(void) {}
void musicFadeTick(void) {}
void crashRenderFrame(void *b) { (void)b; }
void handheld_menu_notify(const char *m) { (void)m; }
unsigned int hh_vicount, hh_frames;

void hh_plat_state_loaded(void) {}
void hh_plat_vi(u32 vicount, int new_frame) { hh_vicount = vicount; if (new_frame) hh_frames++; }
void hh_audio_push(const void *pcm, uint32_t bytes, uint32_t rate);

/* ---- SDL 1.2 audio, stubbed: a host thread plays the card ---- */
static void (*sCb)(void *, Uint8 *, int);
static pthread_mutex_t sMx = PTHREAD_MUTEX_INITIALIZER;
static volatile int sStop;
int SDL_InitSubSystem(Uint32 f) { (void)f; return 0; }
char *SDL_GetError(void) { return "stub"; }
void SDL_LockAudio(void) { pthread_mutex_lock(&sMx); }
void SDL_UnlockAudio(void) { pthread_mutex_unlock(&sMx); }
void SDL_CloseAudio(void) {}
static u64 now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void *card(void *a) {
    static Uint8 buf[512 * 4];
    u64 next = now_ns();
    (void)a;
    while (!sStop) {
        struct timespec ts;
        next += 16000000ull; /* 512 frames at 32 kHz */
        const u64 n = now_ns();
        if (next > n) { ts.tv_sec = (next - n) / 1000000000ull; ts.tv_nsec = (next - n) % 1000000000ull; nanosleep(&ts, NULL); }
        pthread_mutex_lock(&sMx);
        sCb(NULL, buf, sizeof(buf));
        pthread_mutex_unlock(&sMx);
    }
    return NULL;
}
void SDL_PauseAudio(int on) { static pthread_t t; if (!on && !t) pthread_create(&t, NULL, card, NULL); }
int SDL_OpenAudio(SDL_AudioSpec *want, SDL_AudioSpec *have) { (void)have; sCb = want->callback; return 0; }

/* ---- the stand-in game ---- */
static void spin_ms(double ms) { const u64 t = now_ns() + (u64)(ms * 1e6); while (now_ns() < t) {} }
static int sSeconds = 12;
static volatile int sInFlight;            /* a graphics task is being drawn */
static long sMainInFlight, sTorn, sFramesDrawn, sMainFrames;
static u32 sDL[2][256];                   /* the two display lists the main thread alternates */
static OSScTask sTask[2];
static u8 sCfb[2][16];
static OSMesgQueue sGfxQ; static OSMesg sGfxBuf[32];
static OSScClient sGfxClient[2];
static OSScMsg sDoneMsg = { OS_SC_DONE_MSG };
static double sPlan[4096];

static void main_check(void) { if (sInFlight) sMainInFlight++; }
static void mainproc(void *arg) {
    int pending = 0, frame = 0;
    u32 last = 0, vi = 0;
    OSScMsg *m;
    (void)arg;
    osScAddClient(&os_scheduler, sGfxClient, &sGfxQ, NULL);
    for (;;) {
        osRecvMesg(&sGfxQ, (OSMesg *)&m, OS_MESG_BLOCK);
        main_check();
        if (m->type == OS_SC_DONE_MSG) { pending--; continue; }
        if (m->type != OS_SC_RETRACE_MSG) continue;
        vi++;
        if (pending >= 2 || vi - last < 2) continue;
        last = vi;
        for (int k = 0; k < 16; k++) { spin_ms(0.5); (void)osGetCount(); main_check(); } /* the logic */
        u32 *dl = sDL[frame & 1];
        for (int k = 0; k < 256; k++) dl[k] = (u32)frame * 2654435761u + (u32)k;
        OSScTask *t = &sTask[frame & 1];
        memset(t, 0, sizeof(*t));
        t->list.t.type = M_GFXTASK;
        t->list.t.data_ptr = (u64 *)dl;
        t->list.t.data_size = (u32)frame; /* the frame number, for the checks */
        t->flags = OS_SC_NEEDS_RSP | OS_SC_NEEDS_RDP | OS_SC_LAST_TASK | OS_SC_SWAPBUFFER;
        t->msgQ = &sGfxQ;
        t->msg = (OSMesg)&sDoneMsg;
        t->framebuffer = (void *)sCfb[frame & 1];
        osSendMesg(osScGetCmdQ(&os_scheduler), (OSMesg)t, OS_MESG_BLOCK);
        main_check();
        pending++;
        frame++;
        sMainFrames = frame;
    }
}

/* audi.c's sizing: OUTPUT_RATE 0x5622, two retraces a frame at 60 Hz, EXTRA_SAMPLES 0x25 */
static u32 sFrameSize, sMinFrame;
static s16 sAudBuf[3][2 * 2048];
static u32 sCounter;
static OSMesgQueue sAudQ; static OSMesg sAudBuf2[8];
static OSScClient sAudClient[2];
static OSThread sMainT, sAudT, sSchedT;
static void audproc(void *arg) {
    int cur = 0, have_last = 0, last = 0;
    u32 last_n = 0;
    OSScMsg *m;
    (void)arg;
    osScAddClient(&os_scheduler, sAudClient, &sAudQ, (OSScClient *)1); /* every second retrace, as amMain */
    for (;;) {
        osRecvMesg(&sAudQ, (OSMesg *)&m, OS_MESG_BLOCK);
        if (m->type != OS_SC_RETRACE_MSG) continue;
        if (have_last) osAiSetNextBuffer(sAudBuf[last], last_n * 4);
        u32 n = (u32)(((sFrameSize - (osAiGetLength() >> 2)) + 16 + 0x25) & ~0xf);
        if ((s32)n < (s32)sMinFrame) n = sMinFrame;
        if (n > 2048) n = 2048;
        for (u32 i = 0; i < n; i++) { sAudBuf[cur][2 * i] = (s16)sCounter; sAudBuf[cur][2 * i + 1] = (s16)(sCounter >> 16); sCounter++; }
        spin_ms(1.5); /* alAudioFrame and the mixer */
        last = cur; last_n = n; have_last = 1;
        cur = (cur + 1) % 3;
    }
}

/* The AI stream (hh_plat_audio_out, as hh_plat.c): continuous counter values, then to the ring */
static u32 sExpect; static long sAiBad, sAiSamples;
void hh_plat_audio_out(const void *pcm, u32 len, u32 rate) {
    const s16 *s = (const s16 *)pcm;
    for (u32 i = 0; i < len / 4; i++) {
        const u32 v = (u16)s[2 * i] | ((u32)(u16)s[2 * i + 1] << 16);
        if (v != sExpect) sAiBad++;
        sExpect = v + 1;
        sAiSamples++;
    }
    hh_audio_push(pcm, len, rate);
}

/* The graphics task: draws for its planned time, reading its display list, polling as the renderer does */
void hh_plat_rsp_task(OSTask *task) {
    extern uint32_t hh_rsp_poll(void) __attribute__((weak));
    if (task->t.type != M_GFXTASK) return;
    const int frame = (int)task->t.data_size;
    const u32 *dl = (const u32 *)task->t.data_ptr;
    u32 h0 = 0, h1 = 0;
    sInFlight = 1;
    for (int k = 0; k < 256; k++) h0 = h0 * 31u + dl[k];
    const u64 end = now_ns() + (u64)(sPlan[frame % 4096] * 1e6);
    u64 drew = 0;
    while (drew < end) {
        spin_ms(0.5);
        drew = now_ns();
        if (hh_rsp_poll != NULL) { const uint32_t away = hh_rsp_poll(); if (away) { /* the kernel's time is not drawing */ } }
    }
    for (int k = 0; k < 256; k++) h1 = h1 * 31u + dl[k];
    u32 want = 0;
    for (int k = 0; k < 256; k++) want = want * 31u + ((u32)frame * 2654435761u + (u32)k);
    if (h0 != want || h1 != want) sTorn++;
    sInFlight = 0;
    sFramesDrawn++;
}

extern volatile uint32_t hh_audio_underruns, hh_audio_overruns;
extern uint32_t hh_ai_drops, hh_rsp_yields;
static void *stopper(void *a) {
    (void)a;
    sleep((unsigned)sSeconds);
    sStop = 1;
    const char *e = getenv("GE_RSPYIELD");
    printf("rspyield_test %s: %d s, %ld frames drawn, AI %ld samples (%ld out of sequence, %u dropped), "
           "underruns %u, overruns %u, yields %u, main ran during a draw %ld, torn display lists %ld\n",
           e && e[0] == '0' ? "GE_RSPYIELD=0 (one piece)" : getenv("RY_PLAIN") ? "default, 45 ms frames" : "default (yield)",
           sSeconds, sFramesDrawn, sAiSamples, sAiBad,
           (unsigned)hh_ai_drops, (unsigned)hh_audio_underruns, (unsigned)hh_audio_overruns, (unsigned)hh_rsp_yields,
           sMainInFlight, sTorn);
    fflush(stdout);
    exit(sAiBad || hh_ai_drops || sMainInFlight || sTorn ? 1 : 0);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t st;
    if (argc > 1) sSeconds = atoi(argv[1]);
    const int plain = getenv("RY_PLAIN") != NULL; /* every frame 45 ms: the default must never yield */
    for (int k = 0; k < 4096; k++)
        sPlan[k] = plain ? 45.0 : (k >= 150 && k < 220) ? 130.0 : (k % 25 == 24) ? 220.0 : 45.0;
    hh_os_init(1);
    {   /* audi.c amCreateAudioManager's frame size */
        const s32 rate = osAiSetFrequency(0x5622);
        const float fsize = (float)((rate << 1) / 60.0f);
        sFrameSize = (u32)fsize;
        if (sFrameSize < fsize) sFrameSize++;
        if (sFrameSize & 0xf) sFrameSize = (sFrameSize & ~0xf) + 0x10;
        sMinFrame = sFrameSize - 0x10;
    }
    osCreateMesgQueue(&sGfxQ, sGfxBuf, 32);
    osCreateMesgQueue(&sAudQ, sAudBuf2, 8);
    osCreateScheduler(&os_scheduler, &sSchedT, OS_VI_NTSC_LAN1, 1);
    osCreateThread(&sAudT, 4, audproc, NULL, NULL, 20);
    osCreateThread(&sMainT, 3, mainproc, NULL, NULL, 10);
    osStartThread(&sAudT);
    osStartThread(&sMainT);
    pthread_create(&st, NULL, stopper, NULL);
    hh_os_run();
}
