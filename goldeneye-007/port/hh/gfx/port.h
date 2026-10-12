/* The port.h interface the MK64 handheld renderer (gfx_pc.c, gfx_soft.c, gfx_sdl*.c)
 * was written against (kit copied from the OoT port 0921936), implemented for GoldenEye
 * in hh/hh_gfx.c. */
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <PR/ultratypes.h>

/* GoldenEye: every real address the game hands the RCP is KSEG0 (N64 DRAM at
 * 0x80000000 and this binary above it); below that are segment addresses, with
 * segment 0 = physical (hh/hh_boot.c). */
#ifndef PORT_REAL_PTR_MIN
#define PORT_REAL_PTR_MIN 0x80000000u
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

/* OoT's display-list tags (sky pass, UI); GoldenEye does not emit them. */
#define HH_TAG_SKY_BEGIN 0x534B5942u /* 'SKYB' */
#define HH_TAG_SKY_END 0x534B5945u   /* 'SKYE' */
#define HH_TAG_UI_BEGIN 0x55494942u  /* 'UIIB' */
#define HH_TAG_UI_END 0x55494945u    /* 'UIIE' */

/* GoldenEye's 1:1 HUD tags (hh/hh_screen.h). */
#define HH_TAG_GEHUD_OFF 0x47454830u  /* 'GEH0' */
#define HH_TAG_GEHUD_LEFT 0x47454831u /* 'GEH1' */
#define HH_TAG_GEHUD_SIDE 0x47454832u /* 'GEH2' */
#define HH_TAG_GEHUD_INTRO 0x47454833u /* 'GEH3': LEFT for the intro's captions, at the game's own margin */
#define HH_TAG_GEHUD_TOP 0x47454834u   /* 'GEH4': the top message (bondview2.c): its text 1:1 as placed and broken there, its full-width box full width */
/* The first-person models (gun, hands, the pause's watch arm): never taken for the sky (round 38). */
#define HH_TAG_FPMODEL_OFF 0x47454630u /* 'GEF0' */
#define HH_TAG_FPMODEL_ON 0x47454631u  /* 'GEF1' */

#endif
