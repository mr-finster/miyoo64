/* From BattleShip port/audio/audio_dma.c (MIT, see port/NOTICE.md), converted to C. */
/**
 * audio_dma.c -- PORT DMA callback for audio sample loading.
 *
 * On N64 the audio DMA callback (syAudioDma) transfers ADPCM sample data
 * from ROM into RDRAM via the PI. In the port all sample data is already
 * addressable: wavetable->base points into the mapped ROM slice (see
 * audio_bridge.c), so the callback just returns the address it receives.
 */

#include <stdint.h>
#include <stddef.h>

#include "audio/audio_dma.h"

/* Matches ALDMAproc under PORT: uintptr_t (*)(uintptr_t addr, s32 len, void *state) */
static uintptr_t portAudioDma(uintptr_t addr, int32_t len, void *state)
{
	(void)len;
	(void)state;
	return addr;
}

/* Returns a function pointer compatible with ALDMAproc.
 * Installed as syn_config.dmaproc in syAudioMakeBGMPlayers. */
void *portAudioDmaNew(void **state)
{
	if (state != NULL)
		*state = NULL;
	return (void *)portAudioDma;
}
