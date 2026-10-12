/**
 * SF64 audio on the port (M4). The N64's audio thread (Audio_ThreadEntry) runs
 * on the game thread here:
 *
 * - port_audio_init is its prologue: AudioLoad_Init (heap, tables, the
 *   permanent fonts; it also resets the sequence players) and Audio_InitSounds.
 *   Both must run before the game's first frame: Audio_Update walks the sfx
 *   bank lists Audio_InitSounds links, and Radio_Draw reads the players.
 * - One audio tick is one pass of its loop, once per VI on the N64:
 *   AudioThread_CreateTask builds the next command list, and alist.c runs it on
 *   the C mixer at once, as the RSP would during that VI. The AI buffer it fills
 *   is handed to osAiSetNextBuffer (sdl/audio_sdl.c) by the tick two tasks
 *   later, as on the N64. Every spec has numBuffers = 2, so a task (about 1088
 *   samples at 32 kHz) comes every second tick: ticks run in pairs.
 * - port_audio_frame runs after Audio_Update (the game-side sound layer) each
 *   game frame. On the game clock (SF64_VCLOCK=1, tests) it runs exactly
 *   gVIsPerFrame ticks against a virtual AI, so every run hears the same thing
 *   and the game's audio-driven logic (radio voices) stays bit-exact. Otherwise
 *   it runs gVIsPerFrame ticks and tops the output FIFO up to its cushion
 *   (MK64 main.c's refill).
 * - While a frame renders (the only long stalls here: loads are memcpys out of
 *   sf64.rom), audio_sdl.c's backstop thread may run pairs itself if the FIFO
 *   runs low; the game thread never touches the sound engine meanwhile.
 */
#include "global.h"
#include "sf64audio.h"
#include "port.h"
#include "mixer.h"
#include "savestate.h"

extern void AudioLoad_Init(void);
extern void Audio_InitSounds(void);
extern int port_audio_out_below_cushion(void);
extern int port_audio_out_virtual(void);
extern void port_audio_out_virtual_vi(void);
extern void port_audio_mix_lock(void);
extern void port_audio_mix_unlock(void);
extern void port_audio_backstop_enable(int on);

static int sReady SAVESTATE_EXCLUDE; /* the engine is up (process lifetime) */

void port_audio_init(void) {
    AudioLoad_Init();
    Audio_InitSounds();
    sReady = 1;
    port_audio_backstop_enable(1);
    port_log("audio: engine up (spec %d, %u Hz, %d buffers a task)\n", (int) gAudioSpecId,
             (unsigned) gAudioBufferParams.samplingFrequency, (int) gAudioBufferParams.numBuffers);
}

/* One pass of Audio_ThreadEntry's loop: build the task, run it on the mixer. */
static void audio_tick(void) {
    SPTask* task = AudioThread_CreateTask();

    if (task != NULL) {
        port_audio_run_alist(task->task.t.data_ptr, task->task.t.data_size);
    }
}

/* audio_sdl.c's backstop thread calls this under the mix lock. */
void port_audio_tick_pair(void) {
    if (sReady) {
        audio_tick();
        audio_tick();
    }
}

void port_audio_frame(void) {
    /* The VIs this frame lasts, as the pacer counts them (gfx_sdl*.c
     * port_set_frame_vis; Main_Initialize starts gVIsPerFrame at 0). */
    const int vis = gVIsPerFrame < 1 ? 1 : gVIsPerFrame > 4 ? 4 : gVIsPerFrame;
    int i;

    if (!sReady) {
        return;
    }
    if (port_audio_out_virtual()) {
        /* On the game clock: one tick per VI of this frame, then the AI plays a
         * VI's worth, as on the N64. */
        for (i = 0; i < vis; i++) {
            audio_tick();
            port_audio_out_virtual_vi();
        }
        return;
    }
    /* One tick per VI the frame covers (the N64's rate at any frame rate), then
     * up to 4 more while the queue is below its cushion: a frame that ran long
     * (device slower than the VI clock) is made up at once. The game sizes each
     * task from osAiGetLength (audio_sdl.c: the queue minus the cushion), so it
     * keeps the queue there itself while the frames keep time. */
    port_audio_mix_lock();
    for (i = 0; i < vis + 4; i++) {
        if (i >= vis && !port_audio_out_below_cushion()) {
            break;
        }
        audio_tick();
    }
    port_audio_mix_unlock();
}

/* Audio_HandleReset's blocking wait: the N64 spins while its audio thread
 * finishes the reset; here each pass steps the engine one VI. */
void port_audio_pump(void) {
    if (!sReady) {
        return;
    }
    port_audio_mix_lock();
    audio_tick();
    port_audio_mix_unlock();
    if (port_audio_out_virtual()) {
        port_audio_out_virtual_vi();
    }
}
