/* Port: the host's math.h plus the decomp's own extra constants/macros (include/math.h's
 * "extra defines" block, copied verbatim). The decomp's IDO math.h itself is not used. */
#ifndef HH_MATH_H
#define HH_MATH_H
#include_next <math.h>
#include <float.h>
#if defined(PORT) && !defined(HH_PORT_LAYER)
/* GE implements these itself (math_sincos.s -> hh/ge_math.c, math_asinacos.c,
 * math_atan2f.c); game code must get GE's, libm and SDL keep theirs. */
#define sinf   ge_sinf
#define cosf   ge_cosf
#define asin   ge_asin
#define acos   ge_acos
#define atan2f ge_atan2f
#endif
// extra defines

/* The circumference of a circle with diameter 1, PI.*/
#define M_PI_F        3.1415927f
/* The circumference of a circle with diameter 1, PI.*/
#define M_MINUS_PI_F -3.1415927f

/* The circumference of a circle with radius 1, PI*2.*/
#define M_TAU         6.28318530717958647692
/* The circumference of a circle with radius 1, PI*2.*/
#define M_TAU_F       6.2831855f

#define M_PI_2F     1.5707964f
#define M_LN2F      0.69813174f


/* Half of PI. (PI/2)*/
#define M_HALF_PI       (M_PI_F / 2)
/* 3 Halves of PI. (3*(PI/2))*/
#define M_THREE_HALF_PI (3 * M_HALF_PI)


#define M_U16_MAX_VALUE_F 65536.0f
#define M_U32_MAX_VALUE_F 4294967296.0f
/*
 The difference between 1.0 and the next larger floating point number
*/
#define FLT_EPSILON       1.19209290E-07F

/*Inline Math Funcs*/


#define SECS_TO_TIMER60(SECS) (SECS * GAME_TICKRATE)
#define MINS_TO_TIMER60(MINS) (SECS_TO_TIMER60(MINS * GAME_TICKRATE))
/* Gets the byte representation of an angle (degrees) */
#define DEG2BYTE(DEG)         (char)(256.0f / 360.0f * (DEG))
/* Gets the byte representation of an angle (radians) */
#define RAD2BYTE(RAD)         (char)(256.0f / M_TAU_F * (RAD))
/* Converts an angle from Degrees to Radians */
#define DegToRad(DEG)         (float)((DEG) * M_TAU_F / 360.0f)
/* Converts an angle from Degrees to Radians with only one multiply*/
#define DegToRad1Fact(DEG)         (float)((DEG) * (float)(M_TAU / 360.0))
/* Converts and halves an angle from Degrees to Radians */
#define mDegToHalfRad(x)      ((x * M_PI_F) / 360.0f)
/* Converts an angle from Radians to Degrees */
#define RadToDeg(RAD)         (float)((RAD) * (360.0f / M_TAU_F))
/* Gets the angle in radians represented by a byte */
#define ByteToRadian(Byte)    ((Byte * M_TAU_F) * (1.0f / 256.0f))

/* Macro Versions of functions */

#define ABS(x)    ((x) < 0 ? -(x) : (x))          /* absolute value of x */
#define SGN(x)    ((x) < 0 ? -1 : (x) > 0 ? 1 : 0)/* sign of x */
#define MIN(x, y) ((x) < (y) ? (x) : (y))         /* minimum of x and y */
#define MAX(x, y) ((x) > (y) ? (x) : (y))         /* maximum of x and y */
#define SQR(x)    ((x) * (x))                     /* square of x */

/**
 * 0.1f
*/
#define IDO_POINT_ONE 0.10000001f
#endif
