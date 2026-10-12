/* The port.h interface the MK64 handheld renderer (gfx_pc.c, gfx_soft.c, gfx_sdl*.c)
 * was written against, implemented for OoT in hh_port.c. */
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include "ultra64/ultratypes.h"

#ifndef PORT_REAL_PTR_MIN
#define PORT_REAL_PTR_MIN 0x40000000u
#endif

static inline int port_is_real_ptr(uintptr_t addr) {
    return addr >= PORT_REAL_PTR_MIN;
}

/* Segmented address -> pointer through the game's gSegments table. */
void* port_seg_to_ptr(uintptr_t addr);

/* ARMv7 has no round-to-int instruction: libm floorf/ceilf are real calls. */
static inline int port_ifloorf(float x) {
    int i = (int)x;
    return i - (x < (float)i);
}
static inline int port_iceilf(float x) {
    int i = (int)x;
    return i + (x > (float)i);
}
static inline float port_floorf(float x) {
    return (float)port_ifloorf(x);
}

void port_log(const char* fmt, ...);
void port_log_always(const char* fmt, ...);
#define PORT_LOG(...) port_log(__VA_ARGS__)
uint32_t port_time_us(void);

const char* port_save_dir(void);
const char* port_save_path(const char* name);
int port_fs_mkdir(const char* path);

/* Framebuffer readback (the N64 reads its own framebuffer for effects). */
void port_fb_copy_request(int x, int y, int w, int h, uint16_t* target);
void port_fb_tile_note(void* target, unsigned int bytes);
void port_course_textures_loaded(void* start, unsigned int size);
void port_gfx_overlay(void);

/* Frame profile: wall-clock buckets in microseconds (MK64's PERF line). */
enum { PROF_LOGIC, PROF_AUDIO, PROF_GFX, PROF_R3D, PROF_R2D, PROF_TEX, PROF_COMP, PROF_CLR, PROF_PRES, PROF_SLEEP, PROF_ITER, PROF_COUNT };
extern uint32_t gPortProfFrame[PROF_COUNT];
extern uint32_t gPortProfLast[PROF_COUNT];
extern volatile uint32_t gPortProfFlipUs;
#define PORT_PROF_ADD(slot, us) (gPortProfFrame[(slot)] += (uint32_t)(us))

/* MK64's HUD display-list tags; OoT does not emit them. */
#define PORT_HUD_TAG_ON 0x48554431u
#define PORT_HUD_TAG_OFF 0x48554430u
#define PORT_HUD_TAG_CENTRE 0x48554432u

#endif
