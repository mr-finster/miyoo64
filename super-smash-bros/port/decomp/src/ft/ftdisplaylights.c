#include <ft/fighter.h>

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x800FCB70
void ftDisplayLightsDrawReflect(Gfx **display_list, f32 light_angle_x, f32 light_angle_y)
{
    Vec3f vec;

    vec.y = -lbCommonSin(-F_CLC_DTOR32(light_angle_y));
    vec.z = lbCommonCos(-F_CLC_DTOR32(light_angle_y));
    vec.x = lbCommonSin(F_CLC_DTOR32(light_angle_x)) * vec.z;
    vec.z *= lbCommonCos(F_CLC_DTOR32(light_angle_x));

#ifdef PORT
    /* PORT: explicitly initialize the directional light's color (col/colc).
     * The original N64 code only sets l.dir[] and relies on whatever bytes
     * are at gSYTaskmanGraphicsHeap.ptr — on N64 those happened to be left
     * over from previous frames in a way that produced sensible lighting,
     * but on PC the heap contents are different and l.col[] ends up zero,
     * which makes Fast3D's lighting calculation produce a vertex color of
     * 0,0,0 for every lit vertex.  Result: fighters render fully black or
     * very dark.  Set both halves to full white (0xFF) so the directional
     * contribution is at full strength.  Also explicitly emit an ambient
     * light at slot 2 with a low gray base, since the opening scenes never
     * call gSPSetLights1 to set one up — without an ambient, Fast3D adds
     * zero base color and unlit faces are pure black. */
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.col[0] = 0xFF;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.col[1] = 0xFF;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.col[2] = 0xFF;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.colc[0] = 0xFF;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.colc[1] = 0xFF;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.colc[2] = 0xFF;
#endif

    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.dir[0] = vec.x * 100.0F;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.dir[1] = vec.y * 100.0F;
    ((Light*)gSYTaskmanGraphicsHeap.ptr)->l.dir[2] = vec.z * 100.0F;

    gSPNumLights(display_list[0]++, 1);
    gSPLight(display_list[0]++, gSYTaskmanGraphicsHeap.ptr, 1);

    gSYTaskmanGraphicsHeap.ptr = (Light*)gSYTaskmanGraphicsHeap.ptr + 1;

#ifdef PORT
    /* PORT: emit an ambient light at slot 2 (gSPNumLights(1) implicitly
     * means "1 directional + 1 ambient"; Fast3D reads the ambient from
     * current_lights[num_lights - 1]).  Without this the ambient slot is
     * whatever stale data is in Fast3D's mRsp->current_lights[1] from a
     * previous scene's lighting setup — usually zeros on a fresh run. */
    {
        Light *ambient = (Light*)gSYTaskmanGraphicsHeap.ptr;
        ambient->l.col[0]  = 0x40;
        ambient->l.col[1]  = 0x40;
        ambient->l.col[2]  = 0x40;
        ambient->l.colc[0] = 0x40;
        ambient->l.colc[1] = 0x40;
        ambient->l.colc[2] = 0x40;
        ambient->l.dir[0]  = 0;
        ambient->l.dir[1]  = 0;
        ambient->l.dir[2]  = 0;
        gSPLight(display_list[0]++, ambient, 2);
        gSYTaskmanGraphicsHeap.ptr = (Light*)gSYTaskmanGraphicsHeap.ptr + 1;
    }
#endif
}
