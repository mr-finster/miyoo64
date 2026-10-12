#include "objects.h"

#include "camera.h"
#include "macros.h"
#include "math_util.h"
#include "structs.h"
#include "textures_sprites.h"
#include "types.h"

/*
 * obj_shade_fast, translated from src/hasm/obj_shade_fast.s.
 *
 * Hand-written MIPS with no C counterpart in the repository. Like
 * obj_animate.c this is a reading of the assembly, not a recovered original,
 * and it is not written to match.
 *
 * WHAT IT DOES. Per-vertex diffuse shading, written straight into the
 * object's live vertex buffer as a grey: one dot product of the vertex normal
 * against a fixed light direction, scaled and biased by an ambient level, and
 * stored as r = g = b with alpha forced opaque. There is no coloured light
 * here -- ShadeProperties carries lightR/G/B, but this function never reads
 * them.
 *
 * The light direction it uses is shadowDirX/Y/Z (ShadeProperties 0x1C..0x20).
 * The decomp's field names say "shadow"; the assembly uses them as the shading
 * direction, and that is what is reproduced.
 *
 * The ambient level is `shading->unk0 * intensity * 160`, truncated -- the
 * cfc1/ctc1 dance around the cvt.w.s in the original is the IDO idiom for
 * round-toward-zero, which is what a C cast does.
 *
 * Batches whose miscData is BATCH_VTX_COL (0xFF) carry their own vertex
 * colours and are skipped; the vertex cursor still advances past them, and the
 * normal cursor only advances for those carrying flag 0x8000.
 */

void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batches;
    const Vec3s *normal;
    Vertex *vtx;
    s32 lightX, lightY, lightZ;
    s32 ambient;
    s32 i;

    if (shading == NULL) {
        return;
    }

    lightX = shading->shadowDirX;
    lightY = shading->shadowDirY;
    lightZ = shading->shadowDirZ;

    ambient = (s32) (shading->unk0 * intensity * 160.0f);

    batches = model->batches;
    normal = (const Vec3s *) model->normals;
    vtx = obj->curVertData;

    for (i = 0; i < model->numberOfBatches; i++) {
        /* The batch list carries one entry past the last batch, so the vertex
         * count is always the next entry's offset minus this one's. */
        s32 count = batches[i + 1].verticesOffset - batches[i].verticesOffset;
        s32 j;

        if (batches[i].miscData == BATCH_VTX_COL) {
            vtx += count;
            if ((batches[i].flags & 0x8000) != 0) {
                normal += count;
            }
            continue;
        }

        for (j = 0; j < count; j++) {
            s32 dot = (normal->x * lightX + normal->y * lightY + normal->z * lightZ) >> 11;
            s32 c;

            if (dot > 0) {
                c = (s32) ((((u32) (dot * ambient)) >> 16) + (u32) ambient);
                if (c >= 0x100) {
                    c = 0xFF;
                }
            } else {
                /* Facing away: the ambient level, uncapped -- the assembly
                 * does not clamp this branch. */
                c = ambient;
            }

            vtx->r = (u8) c;
            vtx->g = (u8) c;
            vtx->b = (u8) c;
            vtx->a = 0xFF;

            normal++;
            vtx++;
        }
    }
}

/*
 * calc_dynamic_lighting_for_object_2, translated from the second half of
 * src/hasm/obj_shade_fast.s (glabel at line 107, 175 lines of MIPS).
 *
 * This one did not have to be read blind. Its sibling
 * calc_dynamic_lighting_for_object_1 is fully decompiled C at
 * src/objects.c:7963, and the two are the arms of a single if/else at
 * src/objects.c:7949 -- directional lighting on one side (intro Diddy, Taj,
 * T.T., the bosses), ambient on the other (racers, the Rare logo, Wizpig's
 * face). So every quantity below was checked against a decompiled counterpart
 * computing the same thing from the same struct, and every offset the
 * assembly touches was checked against include/structs.h: ShadeProperties
 * 0x1C/0x1E/0x20 = shadowDir, 0x28 = ambient, 0x2C = diffuse, 0x00 = unk0;
 * Object 0x54 = shading, 0x44 = curVertData, 0x00 = trans; ObjectModel
 * 0x28 = numberOfBatches, 0x38 = batches, 0x40 = normals; TriangleBatchInfo
 * is 12 bytes with miscData at 6 and flags at 8; Vertex is 10 bytes with
 * r,g,b,a at 6..9. Nothing here is inferred from what the picture looks like.
 *
 * WHAT IT DOES, and how it differs from _1. The same per-vertex grey: one dot
 * product of the vertex normal against a direction, scaled by a diffuse factor
 * and biased by an ambient one. Three differences from _1:
 *
 *   - _1 rotates the direction into object space with vec3f_rotate_ypr; this
 *     one builds the object's inverse transform as a matrix and multiplies by
 *     it. Same intent, different route, and the assembly's route is the one
 *     reproduced here.
 *   - _1 computes a separate shadeStrength for alpha; this one forces alpha
 *     opaque and writes r = g = b = the same grey.
 *   - _1 reads lightDir and shadowDir both; this one only ever reads
 *     shadowDir. The coloured light fields are never touched.
 *
 * The shifts are the assembly's: >> 7 arithmetic on the dot product, then a
 * *logical* >> 21 on the low 32 bits of dot * diffuseFactor. The logical shift
 * is not interchangeable with an arithmetic one -- the original is `mult`
 * followed by `srl`, so a product that ran into the sign bit comes back as a
 * large positive value and then clamps to 255. Doing the multiply in u32
 * reproduces that without signed overflow.
 *
 * The float order is the assembly's too, not _1's: the common subexpression
 * (unk0 * intensity * 255) is formed once and each of ambient and diffuse
 * multiplies it, where _1 writes its products left to right.
 *
 * Two deliberate, checked divergences from the machine code:
 *
 *   - the batch loop is a `for` here and a do-while in the original, so the
 *     original executes its body once even with numberOfBatches == 0 and reads
 *     one batch past the array. That case is unreachable: the caller only
 *     enters this function once it has already found a batch whose miscData is
 *     not BATCH_VTX_COL, which requires at least one batch. The `for` matches
 *     the sibling and obj_shade_fast above.
 *   - the original leaves ObjectTransform.flags (offset 6) uninitialised on
 *     the stack. mtxf_from_inverse_transform reads only rotation and position
 *     (src/hasm/math_util.c:422), so the field is dead; it is initialised here
 *     rather than left indeterminate. `scale` is likewise never read, but the
 *     assembly does store 1.0f into it, so that store is kept.
 */

#ifdef GC_DEBUG
/*
 * The instrument, per the port's rule that a defect gets a number before it
 * gets a fix. "The dynamic lighting is wrong" splits into three questions this
 * answers separately: whether the branch runs at all (objects), whether it
 * reaches vertices (verts), and whether the grey it writes spans a plausible
 * range or is pinned at one end (min/max). Accumulated freely and zeroed by
 * the heartbeat once it has printed them, so each line is one second's worth.
 */
u32 gGcDynLit2Objects;
u32 gGcDynLit2NoShading;
u32 gGcDynLit2Verts;
u32 gGcDynLit2Min = 0xFFFFFFFF;
u32 gGcDynLit2Max;
#endif

/*
 * An isolation knob, in the shape the rest of the port uses (GC_TEXTEST,
 * GC_NO_CULL, GC_BILLBOARD...). Building with GC_DYNLIT2=0 turns this function
 * back into the stub it replaced, so "the shading is wrong" can be attributed
 * to this translation or cleared of it in one build instead of argued about.
 */
#ifndef GC_DYNLIT2
#define GC_DYNLIT2 1
#endif

void calc_dynamic_lighting_for_object_2(Object *object, ObjectModel *model, s16 arg2, f32 intensity) {
    ShadeProperties *shading = object->shading;
    ObjectTransform objTrans;
    MtxF objMtx;
    Vec3f direction;
    TriangleBatchInfo *batches;
    const Vec3s *normal;
    Vertex *vtx;
    s32 dirX, dirY, dirZ;
    s32 ambientFactor, diffuseFactor;
    f32 level;
    s32 i;

    if (!GC_DYNLIT2) {
        (void) model;
        (void) arg2;
        (void) intensity;
        return;
    }

    if (shading == NULL) {
#ifdef GC_DEBUG
        gGcDynLit2NoShading++;
#endif
        return;
    }

#ifdef GC_DEBUG
    gGcDynLit2Objects++;
#endif

    /* The direction starts as the shading direction in world space. The decomp
     * calls these fields "shadow"; the assembly uses them as the light
     * direction, exactly as obj_shade_fast does above. */
    direction.x = shading->shadowDirX << 2;
    direction.y = shading->shadowDirY << 2;
    direction.z = shading->shadowDirZ << 2;

    if (arg2) {
        mtxf_transform_dir(get_projection_matrix_f32(), &direction, &direction);
    }

    /* Undo the object's own rotation, so the direction ends up in the space
     * the model's normals live in. Negating the three angles and building the
     * inverse transform is what the assembly does; the position is zero
     * because a direction has no origin. */
    objTrans.rotation.y_rotation = -object->trans.rotation.y_rotation;
    objTrans.rotation.x_rotation = -object->trans.rotation.x_rotation;
    objTrans.rotation.z_rotation = -object->trans.rotation.z_rotation;
    objTrans.flags = 0;
    objTrans.scale = 1.0f;
    objTrans.x_position = 0.0f;
    objTrans.y_position = 0.0f;
    objTrans.z_position = 0.0f;

    mtxf_from_inverse_transform(&objMtx, &objTrans);
    mtxf_transform_dir(&objMtx, &direction, &direction);

    /* Truncating casts, matching the cfc1/ctc1 round-toward-zero the original
     * sets up around its cvt.w.s. */
    level = shading->unk0 * intensity * 255.0f;
    ambientFactor = (s32) (shading->ambient * level);
    diffuseFactor = (s32) (shading->diffuse * level);

    dirX = (s32) direction.x;
    dirY = (s32) direction.y;
    dirZ = (s32) direction.z;

    batches = model->batches;
    normal = (const Vec3s *) model->normals;
    vtx = object->curVertData;

    for (i = 0; i < model->numberOfBatches; i++) {
        /* The batch list carries one entry past the last batch, so the vertex
         * count is always the next entry's offset minus this one's. */
        s32 count = batches[i + 1].verticesOffset - batches[i].verticesOffset;
        s32 j;

        if (batches[i].miscData == BATCH_VTX_COL) {
            /* Carries its own vertex colours. The vertex cursor still walks
             * past it; the normal cursor only does so for batches that have
             * normals allocated, which the envmap flag marks. */
            vtx += count;
            if ((batches[i].flags & RENDER_ENVMAP) != 0) {
                normal += count;
            }
            continue;
        }

        for (j = 0; j < count; j++) {
            s32 dot = (normal->x * dirX + normal->y * dirY + normal->z * dirZ) >> 7;
            s32 c;

            if (dot > 0) {
                c = (s32) (((u32) dot * (u32) diffuseFactor) >> 21) + ambientFactor;
                if (c >= 0x100) {
                    c = 0xFF;
                }
            } else {
                /* Facing away: the ambient level, and the assembly does not
                 * clamp this branch -- same as obj_shade_fast above. */
                c = ambientFactor;
            }

            vtx->r = (u8) c;
            vtx->g = (u8) c;
            vtx->b = (u8) c;
            vtx->a = 0xFF;

#ifdef GC_DEBUG
            gGcDynLit2Verts++;
            if ((u32) (u8) c < gGcDynLit2Min) {
                gGcDynLit2Min = (u32) (u8) c;
            }
            if ((u32) (u8) c > gGcDynLit2Max) {
                gGcDynLit2Max = (u32) (u8) c;
            }
#endif

            normal++;
            vtx++;
        }
    }
}
