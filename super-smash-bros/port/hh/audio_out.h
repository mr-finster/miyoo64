/* audio_out.h -- audio output sink between the game's audio thread and the
 * platform (SDL) layer.
 *
 * Format of what the game produces (src/sys/audio.c PORT loop ->
 * portAudioSubmitFrame -> hh_audio_push):
 *   - signed 16-bit PCM, host (little) endian;
 *   - 2 channels, interleaved L,R,L,R (n_alAudioFrame's A_INTERLEAVE output;
 *     in "mono" sound mode both channels carry the same (L+R)/2 value);
 *   - 32000 Hz (dSYAudioPublicSettings.output_rate; osAiSetFrequency is an
 *     identity stub in the port, so syn_config.outputRate == 32000);
 *   - one call per audio tick (60 per second: the audio thread runs once per
 *     retrace message). `frames` is the per-channel sample count
 *     (dSYAudioSampleCounts[], from syAudioGetPortSampleCount): 552 on most
 *     ticks and 368 on roughly one tick in ten, a pattern that averages
 *     32000/60 = 533.3 frames per tick. The byte size of the buffer is
 *     frames * 4. Never more than 920 frames (the 0xE60-byte buffers).
 *
 * The buffer is only valid for the duration of the call (it is one of the
 * game's triple-buffered sSYAudioDataBuffers); copy it. */
#ifndef HH_AUDIO_OUT_H
#define HH_AUDIO_OUT_H

#include <stdint.h>

#define HH_AUDIO_RATE        32000
#define HH_AUDIO_CHANNELS    2
#define HH_AUDIO_MAX_FRAMES  920   /* 0xE60 bytes / 4 */

/* Queue `frames` stereo frames (frames * 2 int16 values) for playback.
 * Implemented by the platform layer; audio_submit.c carries a weak no-op
 * default so the game links without one. */
void hh_audio_push(const int16_t *stereo_samples, int frames);

#endif
