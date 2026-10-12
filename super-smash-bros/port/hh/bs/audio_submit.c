/* From BattleShip port/audio/audio_playback.cpp (MIT, see port/NOTICE.md), converted to C.
 *
 * Only portAudioSubmitFrame is kept: BattleShip's libultraship audio player
 * watermarks, output low-pass filter and WAV dump are dropped. The PCM goes
 * to hh_audio_push (hh/audio_out.h), implemented by the platform layer. */

#include <stdint.h>
#include <stddef.h>

#include "audio/audio_playback.h"
#include "audio_out.h"

/* 0 = mono, 1 = stereo (src/sys/audio.c, set from the options menu). */
extern int32_t dSYAudioSoundQuality;

/* Weak default so the game links before the platform layer provides one. */
__attribute__((weak)) void hh_audio_push(const int16_t *stereo_samples, int frames)
{
	(void)stereo_samples;
	(void)frames;
}

/* buf: interleaved stereo s16 PCM from n_alAudioFrame (one of the game's
 * sSYAudioDataBuffers); sampleCount: frames (samples per channel).
 * Total bytes = sampleCount * 2 channels * 2 bytes. */
void portAudioSubmitFrame(const void *buf, int sampleCount)
{
	int16_t *pcm = (int16_t *)buf;
	int i;

	if (pcm == NULL || sampleCount <= 0)
		return;
	if (sampleCount > HH_AUDIO_MAX_FRAMES)
		sampleCount = HH_AUDIO_MAX_FRAMES;

	/* The N64 audio thread folds the buffer to mono in place before handing
	 * it to the AI when the player picked "mono" (syAudioThreadMain, non-PORT
	 * path); the PORT loop skips that, so do it here. */
	if (dSYAudioSoundQuality == 0) {
		for (i = 0; i < sampleCount * 2; i += 2) {
			int32_t m = (pcm[i] + pcm[i + 1]) / 2;
			pcm[i] = (int16_t)m;
			pcm[i + 1] = (int16_t)m;
		}
	}

	hh_audio_push(pcm, sampleCount);
}
