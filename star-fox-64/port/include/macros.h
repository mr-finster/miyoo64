#ifndef MACROS_H
#define MACROS_H

#include "alignment.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define SCREEN_MARGIN 8

#define TIME_IN_SECONDS(x) (x * 30);

#define CLAMP(x, min, max) ((x) < (min) ? (min) : (x) > (max) ? (max) : (x))
#define CLAMP_MAX(x, max) ((x) > (max) ? (max) : (x))
#define CLAMP_MIN(x, min) ((x) < (min) ? (min) : (x))

#define RAND_FLOAT(max) (Rand_ZeroOne()*(max))
#define RAND_INT(max) ((s32)(Rand_ZeroOne()*(max)))
#define RAND_FLOAT_CENTERED(width) ((Rand_ZeroOne()-0.5f)*(width))
#define RAND_DOUBLE_CENTERED(width) ((Rand_ZeroOne()-0.5)*(width))
#define RAND_RANGE(min, max) (((max) - (min)) * (Rand_ZeroOne() - (min) / ((min) - (max))))

#define RAND_FLOAT_SEEDED(max) (Rand_ZeroOneSeeded()*(max))
#define RAND_INT_SEEDED(max) ((s32)(Rand_ZeroOneSeeded()*(max)))
#define RAND_FLOAT_CENTERED_SEEDED(width) ((Rand_ZeroOneSeeded()-0.5f)*(width))

#if defined(TARGET_HANDHELD)
/* handheld/desktop port: a real pointer passes through unchanged. On the N64 a
 * KSEG0 pointer converts to itself (segment 0 is physical 0 and the offset
 * keeps all of RAM), and the game relies on that: HUD_Texture_Wave hands an
 * already-converted texture to Lib_Texture_Mottle, which converts it again.
 * Port pointers sit at >= 0x10000000 (PORT_REAL_PTR_MIN) and do not fit the
 * 24-bit offset, so they must skip the segment table. */
#define SEGMENTED_TO_VIRTUAL(segment) ({ uintptr_t _s2v = (uintptr_t)(segment); \
    (void*)(_s2v >= 0x10000000u ? _s2v : (uintptr_t)gSegments[(_s2v >> 24) & 0xF] + (_s2v & 0xFFFFFF)); })
#define SEGMENTED_TO_VIRTUAL_JP(segment) SEGMENTED_TO_VIRTUAL(segment)
#else
#define SEGMENTED_TO_VIRTUAL(segment) ((void*)OS_PHYSICAL_TO_K0(gSegments[((uintptr_t)(segment)<<4)>>0x1C]+(((uintptr_t)(segment))&0xFFFFFF)))
#define SEGMENTED_TO_VIRTUAL_JP(segment) ((void*)OS_PHYSICAL_TO_K0(gSegments[((uintptr_t)(segment)&(0xF<<0x18))>>0x18]+(((uintptr_t)(segment))&0xFFFFFF)))
#endif

#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))
#define ARRAY_COUNTU(arr) (u32)(sizeof(arr) / sizeof(arr[0]))
#define SIGN_OF(x) (((x) > 0) ? 1 : ((x) == 0) ? 0 : -1)
#define SQ(x) ((x) * (x))
#define CUBE(x) ((x) * (x) * (x))

#define DOT_XYZ(v1Ptr, v2Ptr) ((v1Ptr)->x * (v2Ptr)->x + (v1Ptr)->y * (v2Ptr)->y + (v1Ptr)->z * (v2Ptr)->z)
#define VEC3F_MAG(vecPtr) sqrtf(DOT_XYZ(vecPtr, vecPtr))
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#define ABSF(x) ((x) >= 0.0f ? (x) : -(x))
#define ROUND(float) ((s32)((float)+0.5f))

#define RAD_TO_DEG(radians) (((radians) * 180.0f) / M_PI)
#define DEG_TO_RAD(degrees) (((degrees) / 180.0f) * M_PI)

#define SIN_DEG(angle) __sinf((M_DTOR)*(angle))
#define COS_DEG(angle) __cosf((M_DTOR)*(angle))

#define USEC_TO_CYCLES(n) (((u64)(n)*(osClockRate/15625LL))/(1000000LL/15625LL))
#define MSEC_TO_CYCLES(n) (USEC_TO_CYCLES((n) * 1000LL))

#define CYCLES_TO_USEC(c) (((u64)(c)*(1000000LL/15625LL))/(osClockRate/15625LL))
#define CYCLES_TO_MSEC(c) ((s32)CYCLES_TO_USEC(c)/1000)

#define UNPACK_BYTE(data, bytenum) (((data) & (0xFF << ((bytenum) * 8))) >> ((bytenum) * 8))

/*
 * Macros for libultra
 */

#if defined(__sgi)
#define PRINTF
#else
#define PRINTF(...)
#endif

#define ALIGNED(x) __attribute__((aligned(x)))
#define ARRLEN(x) ((s32)(sizeof(x) / sizeof(x[0])))
#define STUBBED_PRINTF(x) ((void)(x))
#define UNUSED __attribute__((unused))

#ifndef __GNUC__
#define __attribute__(x)
#endif

#endif // MACROS_H
