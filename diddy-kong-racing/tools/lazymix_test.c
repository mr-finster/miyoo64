/* lazymix_test.c (B16, tools/lazymix_test.sh): drives the real port/sdl/audio_sdl.c -- the
 * current file (-DNEW) or the one before B16, extracted from git -- with real SDL 1.2 or SDL2 (the
 * dummy audio driver). Modes: play (90 game frames, the game thread mixing each, a savestate-like
 * swap(0)/lock/unlock/enable(was) every 10th: the thread count and which thread mixed, and a hash
 * of every mixed sample), backstop (a hypothetical enable with the game thread stalled 400 ms:
 * the thread must start and mix), toggle (the benchmark switch both ways, six times: no thread
 * left over), idle (2 s doing nothing: count the sleep syscalls from outside). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#if defined(HANDHELD)
#include <SDL/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
typedef int16_t s16; typedef uint32_t u32;
void port_audio_out_init(void); void port_audio_out_push(const s16 *s, u32 bytes);
void port_audio_out_set_rate(u32 f);
void port_audio_backstop_enable(int on); int port_audio_backstop_swap(int on);
void port_audio_mix_lock(void); void port_audio_mix_unlock(void);
u32 port_audio_thread_mixes(void);
#ifdef NEW
void port_audio_set_lazymix(int on);
#endif
void port_log(const char *fmt, ...) { (void)fmt; }
static pthread_t sMain; static volatile unsigned sMainMix, sWorkerMix; static uint32_t sHash = 2166136261u;
static s16 sBuf[1072 * 2]; static unsigned sSeed = 1;
static void mix_one(void) { /* the game's mixer: deterministic samples, pushed */
    for (int i = 0; i < 1072 * 2; i++) { sSeed = sSeed * 1103515245u + 12345u; sBuf[i] = (s16)(sSeed >> 16); }
    for (int i = 0; i < 1072 * 2; i++) sHash = (sHash ^ (uint16_t)sBuf[i]) * 16777619u;
    port_audio_out_push(sBuf, 1072 * 4);
}
void port_audiomgr_frame(void) {
    if (pthread_equal(pthread_self(), sMain)) sMainMix++; else sWorkerMix++;
    mix_one();
}
static int threads(void) { int n = 0; DIR *d = opendir("/proc/self/task"); struct dirent *e;
    if (!d) return -1; while ((e = readdir(d))) if (e->d_name[0] != '.') n++; closedir(d); return n; }
static void savestate_like(void) { const int b = port_audio_backstop_swap(0); port_audio_mix_lock(); port_audio_mix_unlock(); port_audio_backstop_enable(b); }
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "play";
    sMain = pthread_self();
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    const int t0 = threads();
    port_audio_out_init();
    port_audio_out_set_rate(22050);
    const int t1 = threads();
    if (!strcmp(mode, "idle")) { struct timespec ts = { 2, 0 }; nanosleep(&ts, NULL); printf("idle done\n"); return 0; }
    /* play: 90 frames at ~33 ms, the game thread mixing each, a savestate-like swap every 10th */
    for (int f = 0; f < 90; f++) {
        port_audiomgr_frame();
        if (f % 10 == 5) savestate_like();
        SDL_Delay(33);
    }
    const int t2 = threads();
    printf("play: threads base %d after-init %d after-play %d | game mixes %u, worker mixes %u, thread mixes %u | hash %08x\n",
           t0, t1, t2, sMainMix, sWorkerMix, port_audio_thread_mixes(), sHash);
    if (!strcmp(mode, "backstop")) { /* hypothetical: a backstop enable, the game thread stalled 400 ms */
        port_audio_backstop_enable(1);
        const int t3 = threads();
        SDL_Delay(400);
        port_audio_backstop_enable(0);
        SDL_Delay(20);
        printf("backstop: threads %d, worker mixes %u (expected > 0)\n", t3, sWorkerMix);
    }
#ifdef NEW
    if (!strcmp(mode, "toggle")) {
        for (int i = 0; i < 6; i++) {
            port_audio_set_lazymix(0); SDL_Delay(15); const int a = threads();
            port_audio_set_lazymix(1); SDL_Delay(15); const int b = threads();
            printf("toggle %d: off %d on %d\n", i, a, b);
        }
        printf("toggle: worker mixes %u\n", sWorkerMix);
    }
#endif
    return 0;
}
