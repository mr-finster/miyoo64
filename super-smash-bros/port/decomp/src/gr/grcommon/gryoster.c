#include <gr/ground.h>
#include <ft/fighter.h>
#include <reloc_data.h>
#ifdef PORT
extern void *func_800269C0_275C0(u16 id);
#endif

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x8012EB20
#ifdef PORT
intptr_t dGRYosterCloudMatAnimJoints[/* */] = { llGRYosterMapCloudSolidMatAnimJoint, llGRYosterMapCloudEvaporateMatAnimJoint };
#else
intptr_t dGRYosterCloudMatAnimJoints[/* */] = { &llGRYosterMapCloudSolidMatAnimJoint, &llGRYosterMapCloudEvaporateMatAnimJoint };
#endif

// 0x8012EB28
u8 dGRYosterCloudLineIDs[/* */] = { 0x1, 0x2, 0x3 };

// // // // // // // // // // // //
//                               //
//          ENUMERATORS          //
//                               //
// // // // // // // // // // // //

enum grYosterCloudStatus
{
    nGRYosterCloudStatusSolid,
    nGRYosterCloudStatusEvaporate
};

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x80108550
LBGenerator* grYosterCloudVaporMakeEffect(Vec3f *pos)
{
    LBGenerator *gn = lbParticleMakeGenerator(gGRCommonStruct.yoster.particle_bank_id, 0);

    if (gn != NULL)
    {
        gn->pos.x = pos->x;
        gn->pos.y = pos->y;
        gn->pos.z = pos->z;
    }
    return gn;
}

// 0x801085A8
sb32 grYosterCheckFighterCloudStand(s32 cloud_id)
{
    GObj *fighter_gobj = gGCCommonLinks[nGCCommonLinkIDFighter];
    s32 line_id = dGRYosterCloudLineIDs[cloud_id];

    while (fighter_gobj != NULL)
    {
        FTStruct *fp = ftGetStruct(fighter_gobj);

        if (fp->ga == nMPKineticsGround)
        {
            if ((fp->coll_data.floor_line_id != -2) && (mpCollisionSetDObjNoID(fp->coll_data.floor_line_id) == line_id))
            {
                return TRUE;
            }
        }
        fighter_gobj = fighter_gobj->link_next;
    }
    return FALSE;
}

// 0x80108634
void grYosterUpdateCloudSolid(s32 cloud_id)
{
    Vec3f pos;
    DObj *dobj;

    if (gGRCommonStruct.yoster.clouds[cloud_id].dobj[0]->mobj->anim_wait == AOBJ_ANIM_NULL)
    {
        if (gGRCommonStruct.yoster.clouds[cloud_id].is_cloud_line_active == FALSE)
        {
            mpCollisionSetYakumonoOnID(dGRYosterCloudLineIDs[cloud_id]);

            gGRCommonStruct.yoster.clouds[cloud_id].is_cloud_line_active = TRUE;
        }
        if (gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer == 0)
        {
            gGRCommonStruct.yoster.clouds[cloud_id].status = nGRYosterCloudStatusEvaporate;
            gGRCommonStruct.yoster.clouds[cloud_id].anim_id = nGRYosterCloudStatusEvaporate;
            gGRCommonStruct.yoster.clouds[cloud_id].evaporate_wait = 180;

            pos = DObjGetStruct(gGRCommonStruct.yoster.clouds[cloud_id].gobj)->translate.vec.f;

            pos.x += (-750.0F);
            pos.y += (-350.0F);

            grYosterCloudVaporMakeEffect(&pos);

            func_800269C0_275C0(nSYAudioFGMYosterCloudVapor);
        }
        else
        {
            if (grYosterCheckFighterCloudStand(cloud_id) != FALSE)
            {
                if (gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer == -1)
                {
                    gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer = 120;
                }
                gGRCommonStruct.yoster.clouds[cloud_id].pressure += 5.0F;

                if (gGRCommonStruct.yoster.clouds[cloud_id].pressure > 180.0F)
                {
                    gGRCommonStruct.yoster.clouds[cloud_id].pressure = 180.0F;
                }
            }
            else
            {
                gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer = -1;
                gGRCommonStruct.yoster.clouds[cloud_id].pressure -= 5.0F;

                if (gGRCommonStruct.yoster.clouds[cloud_id].pressure < 0.0F)
                {
                    gGRCommonStruct.yoster.clouds[cloud_id].pressure = 0.0F;
                }
            }
            if (gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer > 0)
            {
                gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer--;
            }
        }
    }
    dobj = DObjGetStruct(gGRCommonStruct.yoster.clouds[cloud_id].gobj);
    dobj->translate.vec.f.y = gGRCommonStruct.yoster.clouds[cloud_id].altitude - gGRCommonStruct.yoster.clouds[cloud_id].pressure;

    mpCollisionSetYakumonoPosID(dGRYosterCloudLineIDs[cloud_id], &dobj->translate.vec.f);
}

// 0x80108814
void grYosterUpdateCloudEvaporate(s32 cloud_id)
{
    if (gGRCommonStruct.yoster.clouds[cloud_id].is_cloud_line_active != FALSE)
    {
        mpCollisionSetYakumonoOffID(dGRYosterCloudLineIDs[cloud_id]);

        gGRCommonStruct.yoster.clouds[cloud_id].is_cloud_line_active = FALSE;
    }
    if (gGRCommonStruct.yoster.clouds[cloud_id].evaporate_wait == 0)
    {
        gGRCommonStruct.yoster.clouds[cloud_id].status = nGRYosterCloudStatusSolid;
        gGRCommonStruct.yoster.clouds[cloud_id].anim_id = nGRYosterCloudStatusSolid;
        gGRCommonStruct.yoster.clouds[cloud_id].pressure_timer = -1;
        gGRCommonStruct.yoster.clouds[cloud_id].pressure = 0.0F;
    }
    else gGRCommonStruct.yoster.clouds[cloud_id].evaporate_wait--;
}

// 0x80108890
void grYosterUpdateCloudAnim(s32 cloud_id)
{
    s8 anim_id = gGRCommonStruct.yoster.clouds[cloud_id].anim_id;

    if (anim_id != -1)
    {
        void *map_head = gGRCommonStruct.yoster.map_head;
        s32 i;

        for (i = 0; i < ARRAY_COUNT(gGRCommonStruct.yoster.clouds[cloud_id].dobj); i++)
        {
            DObj *dobj = gGRCommonStruct.yoster.clouds[cloud_id].dobj[i];

            lbCommonAddTreeDObjsAnimAll(dobj, NULL, (void*) ((intptr_t)dGRYosterCloudMatAnimJoints[anim_id] + (uintptr_t)map_head), 0.0F);
            gcPlayDObjAnimJoint(dobj);
        }
        gGRCommonStruct.yoster.clouds[cloud_id].anim_id = -1;
    }
}

// 0x80108960
void grYosterProcUpdate(GObj *ground_gobj)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gGRCommonStruct.yoster.clouds); i++)
    {
        switch (gGRCommonStruct.yoster.clouds[i].status)
        {
        case nGRYosterCloudStatusSolid:
            grYosterUpdateCloudSolid(i);
            break;

        case nGRYosterCloudStatusEvaporate:
            grYosterUpdateCloudEvaporate(i);
            break;
        }
        grYosterUpdateCloudAnim(i);
    }
}

// 0x801089F4
void grYosterInitAll(void)
{
    DObj *cloud_dobj;
    GObj *map_gobj;
    DObj *coll_dobj;
    void *map_head;
    s32 i, j;

#ifdef PORT
    map_head = (void *)((uintptr_t)PORT_RESOLVE(gMPCollisionGroundData->map_nodes) - (intptr_t)llGRYosterMapMapHead);
#else
    map_head = (uintptr_t)gMPCollisionGroundData->map_nodes - (intptr_t)&llGRYosterMapMapHead;
#endif
    gGRCommonStruct.yoster.map_head = map_head;

    for (i = 0; i < ARRAY_COUNT(gGRCommonStruct.yoster.clouds); i++)
    {
        map_gobj = gcMakeGObjSPAfter(nGCCommonKindGround, NULL, nGCCommonLinkIDGround, GOBJ_PRIORITY_DEFAULT);

        gGRCommonStruct.yoster.clouds[i].gobj = map_gobj;

        gcAddGObjDisplay(map_gobj, gcDrawDObjTreeForGObj, 6, GOBJ_PRIORITY_DEFAULT, ~0);
        gcSetupCustomDObjs
        (
            map_gobj, 
#ifdef PORT
            (DObjDesc*) ((intptr_t)llGRYosterMapMapHead + (uintptr_t)map_head), 
#else
            (DObjDesc*) ((intptr_t)&llGRYosterMapMapHead + (uintptr_t)map_head), 
#endif
            NULL, 
            nGCMatrixKindTra,    // Make this nGCMatrixKindTraRotRpyRSca to see cloud scale animation
            nGCMatrixKindNull, 
            nGCMatrixKindNull
        );
        gcAddGObjProcess(map_gobj, gcPlayAnimAll, nGCProcessKindFunc, 5);

#ifdef PORT
        gcAddAnimJointAll(map_gobj, (AObjEvent32 **)((uintptr_t)map_head + (intptr_t)llGRYosterMap_1E0_AnimJoint), 0);
#else
        gcAddAnimJointAll(map_gobj, (uintptr_t)map_head + (intptr_t)&llGRYosterMap_1E0_AnimJoint, 0);
#endif

        coll_dobj = DObjGetStruct(map_gobj);
        coll_dobj->translate.vec.f = gMPCollisionYakumonoDObjs->dobjs[dGRYosterCloudLineIDs[i]]->translate.vec.f;

        gGRCommonStruct.yoster.clouds[i].altitude = coll_dobj->translate.vec.f.y;

        coll_dobj = coll_dobj->child;

        for (j = 0; j < ARRAY_COUNT(gGRCommonStruct.yoster.clouds[i].dobj); j++, coll_dobj = coll_dobj->sib_next)
        {
#ifdef PORT
            cloud_dobj = gcAddChildForDObj(coll_dobj, (void *)((uintptr_t)map_head + (intptr_t)llGRYosterMapCloudDisplayList));
#else
            cloud_dobj = gcAddChildForDObj(coll_dobj, (uintptr_t)map_head + (intptr_t)&llGRYosterMapCloudDisplayList);
#endif
            gGRCommonStruct.yoster.clouds[i].dobj[j] = cloud_dobj;

            gcAddXObjForDObjFixed(cloud_dobj, nGCMatrixKindTra, 0);
            gcAddXObjForDObjFixed(cloud_dobj, nGCMatrixKind48, 0);
#ifdef PORT
            lbCommonAddMObjForTreeDObjs(cloud_dobj, (MObjSub ***)((uintptr_t)map_head + (intptr_t)llGRYosterMap_4B8_MObjSub));
#else
            lbCommonAddMObjForTreeDObjs(cloud_dobj, (uintptr_t)map_head + (intptr_t)&llGRYosterMap_4B8_MObjSub);
#endif
        }
        gcPlayAnimAll(map_gobj);

        gGRCommonStruct.yoster.clouds[i].status = nGRYosterCloudStatusSolid;
        gGRCommonStruct.yoster.clouds[i].anim_id = nGRYosterCloudStatusSolid;
        gGRCommonStruct.yoster.clouds[i].pressure_timer = -1;
        gGRCommonStruct.yoster.clouds[i].is_cloud_line_active = FALSE;
        gGRCommonStruct.yoster.clouds[i].pressure = 0.0F;

        mpCollisionSetYakumonoOnID(dGRYosterCloudLineIDs[i]);
    }
#ifdef PORT
    gGRCommonStruct.yoster.particle_bank_id = efParticleGetLoadBankID((uintptr_t)&lGRYosterParticleScriptBankLo, (uintptr_t)&lGRYosterParticleScriptBankHi, (uintptr_t)&lGRYosterParticleTextureBankLo, (uintptr_t)&lGRYosterParticleTextureBankHi);
#else
    gGRCommonStruct.yoster.particle_bank_id = efParticleGetLoadBankID(&lGRYosterParticleScriptBankLo, &lGRYosterParticleScriptBankHi, &lGRYosterParticleTextureBankLo, &lGRYosterParticleTextureBankHi);
#endif
}

// 0x80108C80
GObj* grYosterMakeGround(void)
{
    GObj *ground_gobj = gcMakeGObjSPAfter(nGCCommonKindGround, NULL, nGCCommonLinkIDGround, GOBJ_PRIORITY_DEFAULT);

    grYosterInitAll();
    gcAddGObjProcess(ground_gobj, grYosterProcUpdate, nGCProcessKindFunc, 4);

    return ground_gobj;
}

#ifdef PORT
// 0xPORT
void grYosterStopCloudEvaporation(void)
{
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gGRCommonStruct.yoster.clouds); i++)
    {
        // If a player is standing on the cloud, lock the timer at 2.
        // It will never hit 0, meaning it will never evaporate.
        if (gGRCommonStruct.yoster.clouds[i].pressure_timer > 0)
        {
            gGRCommonStruct.yoster.clouds[i].pressure_timer = 2;
        }
    }
}
#endif
