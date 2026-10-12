/* aspMain (NEAD) command implementations: the MK64 kit's C mixer (sm64-port
 * lineage, SSE4.1/NEON/scalar), driven by alist.c over the game's own command list. */
#ifndef PORT_MIXER_H
#define PORT_MIXER_H

#include <stdbool.h>
#include <stdint.h>
#include "libultra/ultra64.h"

#define NEW_AUDIO_UCODE

void aClearBufferImpl(uint16_t addr, int nbytes);
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr);
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes);
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes);
void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state);
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state);
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state);
void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes);
void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes);
void aInterleaveImpl(uint16_t dest, uint16_t left, uint16_t right, uint16_t c);
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr, uint16_t count);
void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet, uint16_t rate_left, uint16_t rate_right);
void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right);
void aEnvMixerImpl(uint16_t in_addr, uint16_t n_samples, bool swap_reverb,
                   bool neg_left, bool neg_right,
                   uint16_t dry_left_addr, uint16_t dry_right_addr,
                   uint16_t wet_left_addr, uint16_t wet_right_addr);
void aS8DecImpl(uint8_t flags, ADPCM_STATE state);
void aAddMixerImpl(uint16_t in_addr, uint16_t out_addr, uint16_t count);
void aDuplicateImpl(uint16_t in_addr, uint16_t out_addr, uint16_t count);
void aDMEMMove2Impl(uint8_t t, uint16_t in_addr, uint16_t out_addr, uint16_t count);
void aResampleZohImpl(uint16_t pitch, uint16_t start_fract);
void aDownsampleHalfImpl(uint16_t n_samples, uint16_t in_addr, uint16_t out_addr);
void aFilterImpl(uint8_t flags, uint16_t count_or_buf, int16_t state_or_filter[8]);
void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t addr);
void aUnknown25Impl(uint8_t f, uint16_t count, uint16_t out_addr, uint16_t in_addr);
void aInterleaveOldImpl(uint16_t left, uint16_t right);

/* F-Zero X's ENVMIXER (NEAD, not MK64's): the reverb volume ramps with ENVSETUP1's
 * rate, and four sign flags (dry L/R, wet L/R) instead of two. */
void aEnvMixerNeadImpl(uint16_t in_addr, uint16_t n_samples, bool swap_wet,
                       const int16_t xors[4],
                       uint16_t dry_left_addr, uint16_t dry_right_addr,
                       uint16_t wet_left_addr, uint16_t wet_right_addr);
/* NEAD_16 (block copy) and INTERL (every other sample). */
void aCopyBlocksImpl(uint16_t out_addr, uint16_t in_addr, uint16_t block_size, uint8_t count);
void aInterlImpl(uint16_t out_addr, uint16_t in_addr, uint16_t count);

#endif
