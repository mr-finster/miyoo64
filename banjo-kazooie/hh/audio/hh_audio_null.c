/* hh_audio_null.c: the headless proxy's sound output (PLAT=pc): no device. BK_WAV=<file> writes what the game
 * plays (16-bit stereo at the game's rate) as a WAV, for listening and for comparing with the N64. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "savestate.h"

volatile uint32_t hh_audio_underruns SAVESTATE_EXCLUDE, hh_audio_overruns SAVESTATE_EXCLUDE;
static FILE *sWav SAVESTATE_EXCLUDE;
static uint32_t sBytes SAVESTATE_EXCLUDE, sRate SAVESTATE_EXCLUDE;

uint32_t hh_audio_fill_ms(void) { return 0; }
void port_audio_mix_lock(void) {}
void port_audio_mix_unlock(void) {}
void port_audio_backstop_enable(int on) { (void)on; }
void port_audio_out_set_muted(int muted) { (void)muted; }
void port_audio_out_gap(int begin) { (void)begin; }

static void wav_header(void)
{
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
    uint32_t v[5] = { sRate, sRate * 4, 0, 0, 0 };
    int i;
    for (i = 0; i < 4; i++) { h[24 + i] = (uint8_t)(v[0] >> (8 * i)); h[28 + i] = (uint8_t)(v[1] >> (8 * i)); }
    h[32] = 4; h[34] = 16;
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    for (i = 0; i < 4; i++) { h[40 + i] = (uint8_t)(sBytes >> (8 * i)); h[4 + i] = (uint8_t)((sBytes + 36) >> (8 * i)); }
    fseek(sWav, 0, SEEK_SET);
    fwrite(h, 1, 44, sWav);
    fseek(sWav, 0, SEEK_END);
}

void hh_audio_push(const void *pcm, uint32_t bytes, uint32_t rate)
{
    static int sInit SAVESTATE_EXCLUDE;
    if (!sInit) {
        const char *e = getenv("BK_WAV");
        sInit = 1;
        sRate = rate;
        if (e != NULL && (sWav = fopen(e, "wb")) != NULL)
            wav_header();
    }
    if (sWav == NULL)
        return;
    fwrite(pcm, 1, bytes, sWav);
    sBytes += bytes;
    if ((sBytes & 0xFFFF) < bytes)
        wav_header(), fflush(sWav);
}
