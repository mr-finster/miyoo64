#include <sys/interp.h>
#ifdef PORT
#include <sys/objtypes.h>
#include <port_log.h>
#include <port_aobj_fixup.h>
extern void portFixupStructU16(void *base, unsigned int byte_offset, unsigned int num_words);
#endif
#include <PR/gu.h>

// Biquadrate; easier to make a symbol than quartic (QT looks familiar to me)
#define BIQUAD(x) ((x) * (x) * (x) * (x))

#ifdef PORT
/* Halfswap fixup for SYInterpDesc + its data blocks loaded from fighter
 * figatree files.  portRelocFixupFighterFigatree halfswaps every non-reloc
 * u32 in the file: that is correct for AObjEvent16 streams (u16 pairs
 * packed into u32s) but corrupts full-width data — Vec3f points, f32
 * keyframes, f32 quartics, and SYInterpDesc's own header words (kind +
 * pad + s16 points_num at +0x00, f32 length at +0x0C).  We undo the
 * halfswap on first access, idempotent via a visited-pointer set.
 *
 * Token slots (desc->points/keyframes/quartics at +0x08/+0x10/+0x14)
 * were skipped by the figatree fixup (reloc_words[i]==1) and are
 * already correct — leave them alone.
 *
 * Only data blocks inside a port_aobj_register_halfswapped_range region are
 * un-halfswapped; non-figatree-derived interp blocks (stage Arwings, CObj
 * camera anim data from non-fighter files) are already native f32 arrays
 * after pass1.
 */
static u32 port_unhalfswap_u32(u32 v)
{
    return ((v & 0xFFFFu) << 16) | ((v >> 16) & 0xFFFFu);
}

static void port_unhalfswap_block(void *p, size_t n_u32)
{
    u32 *w;
    size_t i;
    if (p == NULL) return;
    if (!port_aobj_is_in_halfswapped_range(p)) return;
    if (port_aobj_unhalfswap_visit(p)) return;
    w = (u32*)p;
    for (i = 0; i < n_u32; i++)
    {
        w[i] = port_unhalfswap_u32(w[i]);
    }
}

/* SYInterpDesc header fixup.
 *
 * After pass1 BSWAP32 + figatree halfswap, the desc memory contains:
 *   byte 0 = original BE pad byte
 *   byte 1 = original BE kind byte
 *   bytes 2..3 = LE-readable s16 points_num
 *   bytes 4..7 = unk04 (zero in practice)
 *   bytes 8..B = points reloc token (skipped by figatree fixup, correct)
 *   bytes C..F = halfswapped f32 length
 *   bytes 10..13 = keyframes token (correct)
 *   bytes 14..17 = quartics token (correct)
 *
 * Normal non-figatree files stop after pass1 BSWAP32, so the first word is
 * [points_num_lo, points_num_hi, pad, kind].  Rotate that one word into the
 * same layout the LE struct expects: [pad, kind, points_num_lo,
 * points_num_hi].  The f32 fields and pointed-to f32 arrays are already
 * correct in this path.
 *
 * Figatree files already have that first-word halfswap, but their full-width
 * f32 words were corrupted by the same pass; un-halfswap unk04 and length
 * once on first access.  Idempotency uses the shared visited set so that
 * figatree-heap reloads trigger a fresh fixup pass on the new bytes.
 */
static void port_fixup_interp_desc(SYInterpDesc *desc)
{
    u32 *w;
    if (desc == NULL) return;
    if (!port_aobj_is_in_halfswapped_range(desc))
    {
        portFixupStructU16(desc, 0, 1);
        return;
    }
    if (port_aobj_unhalfswap_visit(desc)) return;
    w = (u32*)desc;
    w[1] = port_unhalfswap_u32(w[1]); /* unk04 f32 at offset 0x04 */
    w[3] = port_unhalfswap_u32(w[3]); /* length f32 at offset 0x0C */
}

/* Compute u32-count for the quartics block.  IDO's encoding uses
 * (points_num - 1) * 5 floats for cubic kinds; Linear has none. */
static size_t port_interp_quartics_u32_count(const SYInterpDesc *desc)
{
    if (desc == NULL || desc->kind == nSYInterpKindLinear) return 0;
    if (desc->points_num < 2) return 0;
    return (size_t)(desc->points_num - 1) * 5u;
}

/* Compute u32-count for the points block.  Linear stores exactly
 * points_num Vec3f.  Cubic kinds (Bezier, Catrom, BezierS3) need two
 * trailing phantom control points so the sliding-window-of-4 access
 * pattern in syInterpBezier3Points / syInterpCatromCubicSpline doesn't
 * read past the array — verified empirically by measuring the byte
 * gap between desc->points and desc->keyframes in a Samus EscapeF
 * spline (7 Vec3f for points_num=5). */
static size_t port_interp_points_u32_count(const SYInterpDesc *desc)
{
    s32 n;
    if (desc == NULL || desc->points_num <= 0) return 0;
    n = desc->points_num;
    if (desc->kind != nSYInterpKindLinear) n += 2;
    return (size_t)n * 3u;
}
#endif

static Vec3f* syInterpGetPoints(SYInterpDesc *desc)
{
#ifdef PORT
    Vec3f *p;
    port_fixup_interp_desc(desc);
    p = (Vec3f*)PORT_RESOLVE(desc->points);
    if (p != NULL)
    {
        size_t n = port_interp_points_u32_count(desc);
        if (n > 0) port_unhalfswap_block(p, n);
    }
    return p;
#else
    return desc->points;
#endif
}

static f32* syInterpGetKeyframes(SYInterpDesc *desc)
{
#ifdef PORT
    f32 *kf;
    port_fixup_interp_desc(desc);
    kf = (f32*)PORT_RESOLVE(desc->keyframes);
    if ((kf != NULL) && (desc->points_num > 0))
    {
        port_unhalfswap_block(kf, (size_t)desc->points_num);
    }
    return kf;
#else
    return desc->keyframes;
#endif
}

static f32* syInterpGetQuartics(SYInterpDesc *desc)
{
#ifdef PORT
    f32 *q;
    port_fixup_interp_desc(desc);
    q = (f32*)PORT_RESOLVE(desc->quartics);
    if (q != NULL)
    {
        size_t n = port_interp_quartics_u32_count(desc);
        if (n > 0) port_unhalfswap_block(q, n);
    }
    return q;
#else
    return desc->quartics;
#endif
}

// Catmull-Rom cubic spline
void syInterpCatromCubicSpline(Vec3f *out, Vec3f *ctrl, f32 s, f32 t)
{
    Vec3f *lctrl = ctrl;
    f32 sqt = SQUARE(t);
    f32 w0, w1, w2, w3;
    f32 cbt = sqt * t;

    w0 = (2.0F * sqt - cbt - t) * s;
    w1 = (2.0F - s) * cbt + (s - 3.0F) * sqt + 1.0F;
    w2 = (s - 2.0F) * cbt + (3.0F - 2.0F * s) * sqt + s * t;
    w3 = (cbt - sqt) * s;

    out->x = lctrl[0].x * w0 + lctrl[1].x * w1 + lctrl[2].x * w2 + lctrl[3].x * w3;
    out->y = lctrl[0].y * w0 + lctrl[1].y * w1 + lctrl[2].y * w2 + lctrl[3].y * w3;
    out->z = lctrl[0].z * w0 + lctrl[1].z * w1 + lctrl[2].z * w2 + lctrl[3].z * w3;
}

// quadratic spline?
void syInterpQuadSpline(Vec3f *out, Vec3f *ctrl, f32 s, f32 t)
{
    f32 sqt;
    f32 w1;
    f32 w2;
    f32 w0;
    f32 temp;
    f32 w3;

    sqt = t * t;
    w0 = ((((-3.0F) * sqt) + (4.0F * t)) - 1.0F) * s;
    temp = s - 3.0F;
    w3 = s;
    w1 = (((2.0F - w3) * 3.0F) * sqt) + ((2.0F * temp) * t);
    temp = 3.0F - (2.0F * w3);
    w2 = ((((w3 - 2.0F) * 3.0F) * sqt) + ((2.0F * temp) * t)) + w3;
    w3 = ((3.0F * sqt) - (2.0F * t)) * w3;

    out->x = (ctrl[0].x * w0) + (ctrl[1].x * w1) + (ctrl[2].x * w2) + (ctrl[3].x * w3);
    out->y = (ctrl[0].y * w0) + (ctrl[1].y * w1) + (ctrl[2].y * w2) + (ctrl[3].y * w3);
    out->z = (ctrl[0].z * w0) + (ctrl[1].z * w1) + (ctrl[2].z * w2) + (ctrl[3].z * w3);
}

// some sort of bezier interpolation
void syInterpBezier3Points(Vec3f *out, Vec3f *ctrl, f32 t)
{
    Vec3f *lctrl = ctrl;
    f32 subt;
    f32 cbt;
    f32 w0, w1, w2, w3;
    f32 sqt;

    subt = 1.0F - t;
    sqt = SQUARE(t);
    cbt = sqt * t;

    w0 = (1.0F / 6.0F) * subt * subt * subt;
    w1 = (1.0F / 6.0F) * (3.0F * cbt - 6.0F * sqt + 4.0F);
    w2 = (1.0F / 6.0F) * (3.0F * (sqt - cbt + t) + 1.0F);
    w3 = (1.0F / 6.0F) * cbt;

    out->x = lctrl[0].x * w0 + lctrl[1].x * w1 + lctrl[2].x * w2 + lctrl[3].x * w3;
    out->y = lctrl[0].y * w0 + lctrl[1].y * w1 + lctrl[2].y * w2 + lctrl[3].y * w3;
    out->z = lctrl[0].z * w0 + lctrl[1].z * w1 + lctrl[2].z * w2 + lctrl[3].z * w3;
}

// quadratic bezier with four control points (not three?)
void syInterpBezier4Points(Vec3f* out, Vec3f* ctrl, f32 t)
{
    s32 unused[2];
    f32 sqt;
    f32 mt;
    f32 w1;
    f32 w2;
    f32 w3;
    f32 w0;

    sqt = t * t;
    w0 = 1.0F - t;
    w3 = -0.5F * w0 * w0;
    mt = ((3.0F * sqt) - (4.0F * t)) * 0.5F;
    w1 = ((-3.0F * sqt) + (2.0F * t) + 1.0F) * 0.5F;
    w2 = 0.5F * sqt;

    out->x = (ctrl[0].x * w3) + (ctrl[1].x * mt) + (ctrl[2].x * w1) + (ctrl[3].x * w2);
    out->y = (ctrl[0].y * w3) + (ctrl[1].y * mt) + (ctrl[2].y * w1) + (ctrl[3].y * w2);
    out->z = (ctrl[0].z * w3) + (ctrl[1].z * mt) + (ctrl[2].z * w1) + (ctrl[3].z * w2);
}

// cubic bezier with scale factor of 3?
void syInterpCubicBezierScale(Vec3f *out, Vec3f *ctrl, f32 t)
{
    f32 sqt;
    f32 w1;
    f32 w2;
    f32 w3;
    f32 w0;
    f32 subt;
    f32 sqsubt;

    subt = 1.0F - t;
    sqt = SQUARE(t);
    sqsubt = SQUARE(subt);

    w0 = sqsubt * subt;
    w1 = 3.0F * t * sqsubt;
    w2 = 3.0F * sqt * subt;
    w3 = sqt * t;

    out->x = ctrl[0].x * w0 + ctrl[1].x * w1 + ctrl[2].x * w2 + ctrl[3].x * w3;
    out->y = ctrl[0].y * w0 + ctrl[1].y * w1 + ctrl[2].y * w2 + ctrl[3].y * w3;
    out->z = ctrl[0].z * w0 + ctrl[1].z * w1 + ctrl[2].z * w2 + ctrl[3].z * w3;
}

// four point quadratic bezier with scale of 3?
void syInterpQuadBezier4Points(Vec3f* out, Vec3f* ctrl, f32 t)
{
    f32 mt;
    f32 w0;
    f32 w1;
    f32 w2;
    f32 w3;

    mt = t - 1.0F;
    w3 = -3.0F * mt * mt;
    w0 = SQUARE(t);
    w0 = 3.0F * w0;
    w1 = ((1.0F - (4.0F * t)) + w0) * 3.0F;
    w2 = ((2.0F * t) - w0) * 3.0F;

    out->x = (ctrl[0].x * w3) + (ctrl[1].x * w1) + (ctrl[2].x * w2) + (ctrl[3].x * w0);
    out->y = (ctrl[0].y * w3) + (ctrl[1].y * w1) + (ctrl[2].y * w2) + (ctrl[3].y * w0);
    out->z = (ctrl[0].z * w3) + (ctrl[1].z * w1) + (ctrl[2].z * w2) + (ctrl[3].z * w0);
}


// arg1->points_num is total frames? elapsed frames?
// delta time cubic interpolation?
void syInterpCubicSplineTimeFrac(Vec3f *out, SYInterpDesc *desc, f32 t)
{
    s16 target_frame; // f10
    Vec3f *point;
    Vec3f *points = syInterpGetPoints(desc);

    if ((t < 0.0F) || (t > 1.0F))
    {
        return;
    }
    else if (t < 1.0F)
    {
        // convert interval from [0,1] to [0,totalFrames]
        t *= (f32) (desc->points_num - 1);

        target_frame = t;

        // get only the rounding difference for interpolation?
        t -= target_frame;

        switch (desc->kind)
        {
        case nSYInterpKindLinear:
            point = &points[target_frame];
            out->x = (point[1].x - point[0].x) * t + point[0].x;
            out->y = (point[1].y - point[0].y) * t + point[0].y;
            out->z = (point[1].z - point[0].z) * t + point[0].z;
            break;

        case nSYInterpKindBezierS3:
            syInterpCubicBezierScale(out, &points[target_frame * 3], t);
            break;
                
        case nSYInterpKindBezier:
            syInterpBezier3Points(out, &points[target_frame], t);
            break;

        case nSYInterpKindCatrom:
            syInterpCatromCubicSpline(out, &points[target_frame], desc->unk04, t);
            break;
        }
    }
    else
    {
        target_frame = desc->points_num - 1;

        switch (desc->kind)
        {
        case nSYInterpKindLinear:
            point  = &points[target_frame];
            *out = *point;
            break;

        case nSYInterpKindBezierS3:
            point  = &points[target_frame * 3];
            *out = *point;
            break;
                
        case nSYInterpKindBezier:
            syInterpBezier3Points(out, &points[target_frame - 1], 1.0F);
            break;
        
        case nSYInterpKindCatrom:
            point  = &points[target_frame + 1];
            *out = *point;
            break;
        }
    }
}

// quadratic interpolation
void syInterpQuadSplineTimeFrac(Vec3f *out, SYInterpDesc *desc, f32 t)
{
    s16 target_frame;
    f32 t_origin;
    Vec3f *point;
    Vec3f *points = syInterpGetPoints(desc);

    if ((t < 0.0F) || (t > 1.0F))
    {
        return;
    }
    else
    {
        t_origin = t;
        t *= (f32) (desc->points_num - 1);

        target_frame = t;
        t = t - (f32) target_frame;

        switch (desc->kind)
        {
        case nSYInterpKindLinear:
            if (t_origin == 1.0F)
            {
                target_frame--;
            }
            point    = points + target_frame;
            out->x = point[1].x - point[0].x;
            out->y = point[1].y - point[0].y;
            out->z = point[1].z - point[0].z;
            break;
           
        case nSYInterpKindBezierS3:
            syInterpQuadBezier4Points(out, &points[target_frame * 3], t);
            break;
            
        case nSYInterpKindBezier:
            syInterpBezier4Points(out, &points[target_frame], t);
            break;

        case nSYInterpKindCatrom:
            syInterpQuadSpline(out, &points[target_frame], desc->unk04, t);
            break;
        }
    }
}

// sqrt of quartic polynomial of x?
f32 syInterpGetQuartSum(f32 x, f32 *cof)
{
    f32 sum = cof[0] * BIQUAD(x) + cof[1] * CUBE(x) + cof[2] * SQUARE(x) + cof[3] * x + cof[4];

    if ((sum < 0.0F) && (sum > -0.001F))
    {
        sum = 0.0F;
    }
    return sqrtf(sum);
}

f32 syInterpGetCubicIntegralApprox(f32 t, f32 f, f32 *cof)
{
    f32 factor = (f - t) / 8;
    f32 sum = 0.0F;
    f32 time_scale = t + factor;
    s32 i;

    for (i = 2; i < 9; i++)
    {
        if (!(i & 1))
        {
            sum += 4.0F * syInterpGetQuartSum(time_scale, cof);
        }
        else sum += 2.0F * syInterpGetQuartSum(time_scale, cof);
        
        time_scale += factor;
    }
    return ((syInterpGetQuartSum(t, cof) + sum + syInterpGetQuartSum(f, cof)) * factor) / 3.0F;
}

// something like get t as fractional frames?
f32 syInterpGetFracFrame(SYInterpDesc *desc, f32 t)
{
    f32 *point; // v0
    f32 *keyframes = syInterpGetKeyframes(desc);
    f32 *quartics = syInterpGetQuartics(desc);
#ifdef PORT
    Vec3f *points = syInterpGetPoints(desc);
    static s32 sSYInterpNullResolveLogCount = 0;
#endif
    s32 id;
    f32 frac_frame; // sp5C
    f32 time_scale;
    f32 min = 0.0F;
    f32 max = 1.0F;
    f32 res;
    f32 diff; // f24

#ifdef PORT
    if (((points == NULL) || (keyframes == NULL) || (quartics == NULL)) && (sSYInterpNullResolveLogCount < 16))
    {
        sSYInterpNullResolveLogCount++;
        port_log("SSB64: syInterpGetFracFrame - null resolve desc=%p kind=%u points_num=%d points=0x%08x keyframes=0x%08x quartics=0x%08x length=%f resolved=[%p %p %p]\n",
            desc, desc->kind, desc->points_num, desc->points, desc->keyframes, desc->quartics, desc->length,
            points, keyframes, quartics);
    }
#endif

    id = 0;
    point = keyframes;

    while (point[1] < t)
    {
        id++;
        point++;
    }
    switch (desc->kind)
    {
    case nSYInterpKindLinear:
        frac_frame = (t - keyframes[id]) / (keyframes[id + 1] - keyframes[id]);
        break;

    case nSYInterpKindBezierS3:
    case nSYInterpKindBezier:
    case nSYInterpKindCatrom:
        time_scale = (t - keyframes[id]) * desc->length;

        do
        {
            frac_frame = (min + max) / 2.0F;
            res = syInterpGetCubicIntegralApprox(min, frac_frame, quartics + (id * 5));

            if (time_scale < (res + 0.00001F)) 
            {
                max = frac_frame;
            }
            else
            {
                min = frac_frame;
                time_scale -= res;
            }
            diff = (min < max) ? -(min - max) : min - max;

            if (diff < 0.00001F)
            {
                break;
            }
        }
        while ((res + 0.00001F) < time_scale || time_scale < (res - 0.00001F));

        break;
    }
    return ((f32) id + frac_frame) / ((f32) desc->points_num - 1.0F);
}

void syInterpCubic(Vec3f *out, SYInterpDesc *desc, f32 t)
{
    syInterpCubicSplineTimeFrac(out, desc, syInterpGetFracFrame(desc, t));
}

void syInterpQuad(Vec3f *out, SYInterpDesc *desc, f32 t)
{
    syInterpQuadSplineTimeFrac(out, desc, syInterpGetFracFrame(desc, t));
}
