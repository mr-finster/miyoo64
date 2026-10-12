#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#if defined(TARGET_HANDHELD)
/* handheld/desktop port: the host C library (layouts that matter to the game
 * -- bool, the typedefs -- are unchanged). */
#include <math.h>
#include "PR/ultratypes.h"
#undef M_PI
#undef M_SQRT2
#undef M_SQRT1_2
#define M_PI    3.14159265358979323846f
#define M_DTOR	(M_PI / 180.0f)
#define M_RTOD	(180.0f / M_PI)
#define M_SQRT2 1.41421356237309504880f
#define M_SQRT1_2 0.70710678118654752440f
#ifndef FLT_MAX
#define FLT_MAX 340282346638528859811704183484516925440.0f
#endif
#define SHT_MAX 32767.0f
#define SHT_MINV (1.0f / SHT_MAX)
/* A double as two words. The N64 is big-endian (high word first); on a
 * little-endian port the low word comes first in memory, so both the field
 * order and DU_INIT (the libm constant tables, written high word first) flip. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
typedef union { struct { u32 lo; u32 hi; } word; f64 d; } du;
#define DU_INIT(hi, lo) { { (lo), (hi) } }
#else
typedef union { struct { u32 hi; u32 lo; } word; f64 d; } du;
#define DU_INIT(hi, lo) { { (hi), (lo) } }
#endif
typedef union { u32 i; f32 f; } fu;
extern f32 __libm_qnan_f;
/* libm helper names (glibc declares its own __floorf etc.): plain macros. */
#define __floorf(x) (floorf(x))
#define __floor(x) (floor(x))
#define __ceilf(x) (ceilf(x))
#define __ceil(x) (ceil(x))
#define __truncf(x) (truncf(x))
#define __trunc(x) (trunc(x))
#define __roundf(x) (roundf(x))
#define __round(x) (round(x))
#define __nearbyintf(x) (nearbyintf(x))
#define __nearbyint(x) (nearbyint(x))
#define __lfloorf(x) ((s32) floorf(x))
#define __lfloor(x) ((s32) floor(x))
#define __lceilf(x) ((s32) ceilf(x))
#define __lceil(x) ((s32) ceil(x))
#define __lroundf(x) ((s32) roundf(x))
#define __lround(x) ((s32) round(x))
#define __lnearbyintf(x) ((s32) nearbyintf(x))
#define __lnearbyint(x) ((s32) nearbyint(x))
#define __ltruncf(x) ((s32) (x))
#define __ltrunc(x) ((s32) (x))
#else

#include "PR/ultratypes.h"

#define M_PI    3.14159265358979323846f
// Degrees to Radians
#define M_DTOR	(M_PI / 180.0f)
// Radians to Degrees
#define M_RTOD	(180.0f / M_PI)
#define M_SQRT2 1.41421356237309504880f
#define M_SQRT1_2 0.70710678118654752440f	/* 1/sqrt(2) */
#define FLT_MAX 340282346638528859811704183484516925440.0f
#define SHT_MAX 32767.0f
#define SHT_MINV (1.0f / SHT_MAX)

typedef union {
    struct {
        u32 hi;
        u32 lo;
    } word;

    f64 d;
} du;
#define DU_INIT(hi, lo) { { (hi), (lo) } } /* N64: high word first */

typedef union {
    u32 i;
    f32 f;
} fu;

extern f32 __libm_qnan_f;

float fabsf(float f);
#pragma intrinsic(fabsf)
#ifdef __GNUC__
#define fabsf(f) __builtin_fabsf((float)(f))
#endif

double fabs(double f);
#pragma intrinsic(fabs)
#ifdef __GNUC__
#define fabs(f) __builtin_fabs((double)(f))
#endif

float sqrtf(float f);
#if defined(__sgi)
#pragma intrinsic(sqrtf)
#endif

double sqrt(double d);
#pragma intrinsic(sqrt)

f32 __floorf(f32);
f64 __floor(f64);
s32 __lfloorf(f32);
s32 __lfloor(f64);
f32 __ceilf(f32);
f64 __ceil(f64);
s32 __lceilf(f32);
s32 __lceil(f64);
f32 __truncf(f32);
f64 __trunc(f64);
s32 __ltruncf(f32);
s32 __ltrunc(f64);
f32 __roundf(f32);
f64 __round(f64);
s32 __lroundf(f32);
s32 __lround(f64);
f32 __nearbyintf(f32);
f64 __nearbyint(f64);
s32 __lnearbyintf(f32);
s32 __lnearbyint(f64);

#endif /* TARGET_HANDHELD */
#endif
