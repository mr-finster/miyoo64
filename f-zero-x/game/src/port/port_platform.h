/* Platform layer (window, renderer, audio device, input) used by port_main.c. */
#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

#include "libultra/ultra64.h"

typedef struct {
    u32 gfxTasks;
    u32 gfxRendered;
    u32 audioTasks;
    u32 workUs;
} PortStats;
extern PortStats gPortStats;

void port_platform_init(int headless);
void port_platform_quit(void);
/** Pump window/input events; 0 = quit requested. */
int port_platform_poll(void);
void port_platform_gfx_task(Gfx* dl);
void port_platform_audio_task(u64* acmd, u32 bytes);
/** A retrace happened (present the latest picture). */
void port_platform_vi(u32 vi);
void port_platform_dump(const char* path);
/** Show an N64 framebuffer the CPU drew (no display list): the boot logo. */
void port_platform_show_framebuffer(const void* fb);
uint32_t port_time_us(void);

#endif
