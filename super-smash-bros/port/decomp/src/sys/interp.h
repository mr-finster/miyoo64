#ifndef _SYINTERP_H_
#define _SYINTERP_H_

#include <ssb_types.h>
#include <macros.h>
#include <PR/ultratypes.h>

typedef struct SYInterpDesc
{
#if IS_BIG_ENDIAN
    u8 kind;
    /* implicit 1-byte pad to 2-align points_num */
    s16 points_num;
#else
    /* PORT/LE: syInterp's accessors normalize file-loaded descriptor
     * word 0 to this byte order for both normal pass1-swapped resources
     * and fighter figatree halfswapped resources. */
    u8 _pad0;
    u8 kind;
    s16 points_num;
#endif
    f32 unk04;          // CR scale? count?
#ifdef PORT
    u32 points;         // Relocation token — use PORT_RESOLVE()
#else
    Vec3f *points;
#endif
    f32 length;
#ifdef PORT
    u32 keyframes;      // Relocation token — use PORT_RESOLVE()
    u32 quartics;       // Relocation token — use PORT_RESOLVE()
#else
    f32 *keyframes;     // maybe keyframes as fraction t?
    f32 *quartics;      // quartic coef
#endif

} SYInterpDesc;

#ifdef PORT
_Static_assert(sizeof(SYInterpDesc) == 24, "SYInterpDesc must be 24 bytes to match file data layout");
#endif

typedef enum SYInterpKind
{
    nSYInterpKindLinear,
    nSYInterpKindBezierS3,
    nSYInterpKindBezier,
    nSYInterpKindCatrom

} SYInterpKind;

extern void syInterpCatromCubicSpline(Vec3f *out, Vec3f *ctrl, f32 s, f32 t);
extern void syInterpQuadSpline(Vec3f *out, Vec3f *ctrl, f32 s, f32 t);
extern void syInterpBezier3Points(Vec3f *out, Vec3f *ctrl, f32 t);
extern void syInterpBezier4Points(Vec3f* out, Vec3f* ctrl, f32 t);
extern void syInterpCubicBezierScale(Vec3f *out, Vec3f *ctrl, f32 t);
extern void syInterpQuadBezier4Points(Vec3f* out, Vec3f* ctrl, f32 t);
extern void syInterpCubicSplineTimeFrac(Vec3f *out, SYInterpDesc *desc, f32 t);
extern void syInterpQuadSplineTimeFrac(Vec3f *out, SYInterpDesc *desc, f32 t);
extern f32 syInterpGetQuartSum(f32 x, f32 *cof);
extern f32 syInterpGetCubicIntegralApprox(f32 t, f32 f, f32 *cof);
extern f32 syInterpGetFracFrame(SYInterpDesc *desc, f32 t);
extern void syInterpCubic(Vec3f *out, SYInterpDesc *desc, f32 t);
extern void syInterpQuad(Vec3f *out, SYInterpDesc *desc, f32 t);

#endif
