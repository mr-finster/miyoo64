#include "objects.h"

#include "macros.h"
#include "structs.h"
#include "types.h"

/*
 * obj_animate, translated from src/hasm/obj_animate.s.
 *
 * Hand-written MIPS with no C counterpart in the repository, so this is a
 * reading of the assembly rather than a recovered original. It is not written
 * to match: the GameCube port never assembles the .s, and the only thing that
 * has to hold is behaviour.
 *
 * WHAT THE ANIMATION FORMAT IS, as the assembly describes it.
 *
 * A model's animation (ObjectModel_44) is one flat byte block. It is cut into
 * fixed-size keys of
 *
 *     keyStride = 3 * numberOfAnimatedVertices + 12
 *
 * bytes: twelve bytes of header followed by three signed bytes per animated
 * vertex. Key k lives at animData + keyStride * (k + 2) -- the first two
 * strides are the base pose and a header the walk below never indexes
 * directly.
 *
 * The three bytes per vertex are a *delta*, not a position. So a pose is
 * reached by starting from the base pose and adding every key up to the one
 * wanted, and moving to a neighbouring frame costs one key's worth of adds or
 * subtracts rather than a full rebuild. ModelInstance::vertices[2] holds that
 * running sum -- it is a Vec3s array, three s16 per animated vertex, not the
 * Vertex array its declared type suggests.
 *
 * obj->animFrame is 12.4 fixed point: the whole part selects the key, the low
 * four bits interpolate towards the next one. The fractional part is built
 * into D_8011D644 (a 0xC00-byte scratch the model code allocates at startup,
 * object_models.c:57) and added on at the end, so the accumulated sum stays
 * exact and only the display copy carries the interpolation.
 *
 * The twelve header bytes of a key carry four big-endian s16 the RSP never
 * sees: the instance's x/y/z offset and its head tilt, interpolated the same
 * way as the vertices.
 *
 * Output double-buffers between vertices[0] and vertices[1], flipped through
 * animationTaskNum, because the previous frame's copy may still be in a
 * display list the GP has not finished with.
 */

extern s32 D_8011D644;

/* Three s16 per animated vertex: what vertices[2] and D_8011D644 really are. */
typedef struct AnimVec {
    s16 x, y, z;
} AnimVec;

s32 obj_animate(Object *obj) {
    ModelInstance *inst;
    ObjectModel *model;
    const s8 *key;
    const s8 *cur;
    const s8 *next;
    AnimVec *pose;  /* the running sum, vertices[2] */
    AnimVec *frac;  /* the interpolation towards the next key, D_8011D644 */
    Vertex *out;
    s32 modelIndex;
    s32 animID;
    s32 animFrame;
    s32 frameWhole;
    s32 frameFrac;
    s32 prevFrame;
    s32 lastFrame;
    s32 keyStride;
    s32 numAnimVerts;
    s32 i;
    s32 j;
    s32 base[4];
    s32 delta;

    /* Which of the object's model instances this animation drives. The upper
     * clamp really is `>= n` -> `n` rather than `n - 1`; it is what the
     * assembly does. */
    modelIndex = obj->modelIndex;
    if (modelIndex < 0) {
        modelIndex = 0;
    }
    if (modelIndex >= obj->header->numberOfModelIds) {
        modelIndex = obj->header->numberOfModelIds;
    }

    inst = obj->modelInstances[modelIndex];
    model = inst->objModel;
    if (model->animations == NULL) {
        return 0;
    }

    animFrame = obj->animFrame;
    animID = obj->animationID;

    /* Already showing exactly this. animationFrameCount holds the whole 12.4
     * frame, not a count -- the name is the decomp's. */
    if (animFrame == inst->animationFrameCount && animID == inst->animationID) {
        return 0;
    }

    if (animID < 0) {
        animID = 0;
    }
    if (animID >= model->numberOfAnimations) {
        animID = model->numberOfAnimations - 1;
    }

    lastFrame = 0;
    if (model->numberOfAnimations > 0) {
        lastFrame = model->animations[animID].animLength - 2;
    }

    /* Logical, not arithmetic: the assembly uses srl on the sign-extended
     * halfword, so a negative frame becomes a very large positive one and is
     * caught by the clamp below rather than indexing backwards. */
    frameWhole = (s32) (((u32) animFrame) >> 4);
    if (lastFrame < frameWhole) {
        animFrame = 0;
        frameWhole = 0;
        /* Forces the rebuild below: no previous pose can be trusted. */
        inst->animationID = -1;
    }

    pose = (AnimVec *) inst->vertices[2];
    prevFrame = (animID == inst->animationID) ? inst->animationFrame : -1;

    inst->animationID = (s16) animID;
    inst->animationFrameCount = (s16) animFrame;
    inst->animationFrame = (s16) frameWhole;
    frameFrac = animFrame & 0xF;

    numAnimVerts = model->numberOfAnimatedVertices;
    keyStride = 3 * numAnimVerts + 12;

    if (frameWhole == 0 || prevFrame == -1) {
        /*
         * Rebuild the running sum from the base pose.
         *
         * animatedVertexIndices runs parallel to the model's own vertex array
         * -- one entry per vertex, -1 for a vertex this animation does not
         * move -- and its value indexes the animated-vertex arrays. So the
         * source walks vertices in order while the destination is indexed.
         */
        const s16 *idx = (const s16 *) model->animatedVertexIndices;
        const AnimVec *rest = (const AnimVec *) ((const u8 *) model->animations[animID].animData + 12);
        const Vertex *src = model->vertices;

        for (i = 0; i < model->numberOfVertices; i++) {
            s32 k = idx[i];

            if (k != -1) {
                pose[k].x = (s16) (src[i].x + rest[k].x);
                pose[k].y = (s16) (src[i].y + rest[k].y);
                pose[k].z = (s16) (src[i].z + rest[k].z);
            }
        }
        prevFrame = 0;
    }

    /* Walk the running sum to the wanted key, one key at a time, in whichever
     * direction is shorter -- which is the whole point of storing deltas. */
    key = (const s8 *) model->animations[animID].animData;

    while (prevFrame < frameWhole) {
        const s8 *d = key + keyStride * (prevFrame + 2);

        for (j = 0; j < numAnimVerts; j++) {
            pose[j].x = (s16) (pose[j].x + d[j * 3 + 0]);
            pose[j].y = (s16) (pose[j].y + d[j * 3 + 1]);
            pose[j].z = (s16) (pose[j].z + d[j * 3 + 2]);
        }
        prevFrame++;
    }

    while (prevFrame > frameWhole) {
        const s8 *d = key + keyStride * (prevFrame + 1);

        for (j = 0; j < numAnimVerts; j++) {
            pose[j].x = (s16) (pose[j].x - d[j * 3 + 0]);
            pose[j].y = (s16) (pose[j].y - d[j * 3 + 1]);
            pose[j].z = (s16) (pose[j].z - d[j * 3 + 2]);
        }
        prevFrame--;
    }

    /*
     * The fraction towards the next key, kept separate so the running sum
     * above stays exact. The assembly multiplies unsigned and shifts logically
     * before truncating to s16, which for an s8 delta and a four-bit fraction
     * agrees with the signed arithmetic shift in every bit that survives the
     * store.
     */
    frac = (AnimVec *) D_8011D644;
    next = key + keyStride * (frameWhole + 2);
    for (j = 0; j < numAnimVerts; j++) {
        frac[j].x = (s16) ((((u32) next[j * 3 + 0]) * (u32) frameFrac) >> 4);
        frac[j].y = (s16) ((((u32) next[j * 3 + 1]) * (u32) frameFrac) >> 4);
        frac[j].z = (s16) ((((u32) next[j * 3 + 2]) * (u32) frameFrac) >> 4);
    }

    /*
     * The four big-endian s16 in a key's twelve-byte header: the instance
     * offset and the head tilt. Read byte-wise because the block has no
     * alignment guarantee.
     */
    if (frameWhole == 0) {
        cur = key;
    } else {
        cur = key + keyStride * (frameWhole + 1) - 12;
    }
#if defined(PORT)
    /* Port: asset_swap_object_animation() has already put every key header's s16 in host order
     * (frame-0 header and headers m = 2..animLength), so read them natively. memcpy because a
     * header can be odd-aligned (keyStride is odd when numAnimVerts is). */
    {
        s16 h;

        __builtin_memcpy(&h, cur + 0, 2);
        base[0] = h;
        __builtin_memcpy(&h, cur + 2, 2);
        base[1] = h;
        __builtin_memcpy(&h, cur + 4, 2);
        base[2] = h;
        __builtin_memcpy(&h, cur + 10, 2);
        base[3] = h;

        next = key + keyStride * (frameWhole + 2) - 12;
        for (i = 0; i < 4; i++) {
            s32 off = (i == 3) ? 10 : i * 2;

            __builtin_memcpy(&h, next + off, 2);
            delta = h - base[i];
            base[i] += (s32) ((((u32) delta) * (u32) frameFrac) >> 4);
        }
    }
#else
    base[0] = (cur[0] << 8) | (u8) cur[1];
    base[1] = (cur[2] << 8) | (u8) cur[3];
    base[2] = (cur[4] << 8) | (u8) cur[5];
    base[3] = (cur[10] << 8) | (u8) cur[11];

    next = key + keyStride * (frameWhole + 2) - 12;
    for (i = 0; i < 4; i++) {
        s32 off = (i == 3) ? 10 : i * 2;

        delta = ((next[off] << 8) | (u8) next[off + 1]) - base[i];
        base[i] += (s32) ((((u32) delta) * (u32) frameFrac) >> 4);
    }
#endif
    inst->offsetX = (s16) base[0];
    inst->offsetY = (s16) base[1];
    inst->offsetZ = (s16) base[2];
    inst->headTilt = (s16) base[3];

    /*
     * Emit into the buffer the display list is not reading. The output pointer
     * advances for every vertex, including the ones this animation does not
     * move -- their positions are simply left as model_instance_init copied
     * them, and only x/y/z are written, so the colours survive.
     */
    inst->animationTaskNum ^= 1;
    out = inst->vertices[inst->animationTaskNum];
    {
        const s16 *idx = (const s16 *) model->animatedVertexIndices;

        for (i = 0; i < model->numberOfVertices; i++) {
            s32 k = idx[i];

            if (k != -1) {
                out[i].x = (s16) (pose[k].x + frac[k].x);
                out[i].y = (s16) (pose[k].y + frac[k].y);
                out[i].z = (s16) (pose[k].z + frac[k].z);
            }
        }
    }

    return 1;
}
