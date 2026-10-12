#include <ft/fighter.h>
#include <sys/objanim.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
#endif

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x800EC160
f32 ftAnimGetTargetValue(s16 arg, s32 track, sb32 value_or_step)
{
    f32 unused;
    s32 id;
    f32 ret;

    // 0x8012B910
    f32 fracs[/* */] =
    {
        1.0F / 512.0F,
        1.0F / 4.0F,
        1.0F / 4096.0F,
        1.0F / 16384.0F - (3.0F / 1000000000000.0F), // why tho
        1.0F / 512.0F,
        1.0F / 32.0F,
        1.0F / 8192.0F,
        1.0F / 16384.0F - (3.0F / 1000000000000.0F)  // ???
    };

    switch (track)
    {
    case nGCAnimTrackRotX:
    case nGCAnimTrackRotY:
    case nGCAnimTrackRotZ:
        id = 0;
        break;

    case nGCAnimTrackTraX:
    case nGCAnimTrackTraY:
    case nGCAnimTrackTraZ:
        id = 1;
        break;

    case nGCAnimTrackScaX:
    case nGCAnimTrackScaY:
    case nGCAnimTrackScaZ:
        id = 2;
        break;

    case nGCAnimTrackTraI:
        id = 3;
        break;
    }
    if (value_or_step != 0)
    {
        id += 4;
    }
    ret = arg;

    ret *= fracs[id];

    return ret;
}

// 0x800EC238
void ftAnimParseDObjFigatree(DObj *root_dobj)
{
    AObj *track_aobjs[nGCAnimTrackJointEnd - nGCAnimTrackJointStart + 1];
    AObj *current_aobj;
    f32 payload;
    s32 i;
    u16 command_kind;
    u16 flags;
#ifdef PORT
    s32 watchdog = 0;
    static s32 sFTAnimTranslateInterpLogCount = 0;
    static s32 sFTAnimTraICommandLogCount = 0;
#endif

    if (root_dobj->anim_wait != AOBJ_ANIM_NULL)
    {
#ifdef PORT
        if (root_dobj->parent_gobj == NULL)
        {
            root_dobj->anim_wait = AOBJ_ANIM_NULL;
            return;
        }
        /* Parse-entry counter per synth fkind — if Crash's APPEAR figatree
         * is stuck (no End opcode ever fires), the count keeps climbing
         * while gobj->anim_frame stays positive. Mario / NLink go through
         * gcParseDObjAnimJoint (event32) because their motions have
         * is_anim_joint=1, so they don't reach this diagnostic. Also dumps
         * the first 16 bytes at event16 once per synth fkind so we can
         * verify the u16-halfswap applied during file load (opcode and
         * payload u16s should be in the LE-readable layout). */
#ifdef SSB64_ANIM_DEBUG
        {
            s32 _fkind_parse = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
            if (_fkind_parse >= 27 && _fkind_parse <= 64) {
                static s32 s_figatree_parse_count = 0;
                static s32 s_figatree_dump_done = 0;
                if (s_figatree_dump_done < 4 && root_dobj->anim_joint.event16 != NULL) {
                    const uint8_t *p = (const uint8_t *)root_dobj->anim_joint.event16;
                    port_log("SSB64: figatree first-bytes fkind=%d dobj=%p ev16=%p bytes=%02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n",
                        (int)_fkind_parse, (void *)root_dobj, (void *)p,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                        p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                    s_figatree_dump_done++;
                }
                if ((s_figatree_parse_count & 0x1FF) == 0) {
                    port_log("SSB64: figatree parse fkind=%d count=%d dobj=%p ev16=%p anim_wait=%f gobj_anim_frame=%f\n",
                        (int)_fkind_parse, (int)s_figatree_parse_count,
                        (void *)root_dobj,
                        (void *)root_dobj->anim_joint.event16,
                        (double)root_dobj->anim_wait,
                        (double)root_dobj->parent_gobj->anim_frame);
                }
                s_figatree_parse_count++;
            }
        }
#endif /* SSB64_ANIM_DEBUG */
#endif
        if (root_dobj->anim_wait == AOBJ_ANIM_CHANGED)
        {
            root_dobj->anim_wait = -root_dobj->anim_frame;
        }
        else
        {
            root_dobj->anim_wait -= root_dobj->anim_speed;
            root_dobj->anim_frame += root_dobj->anim_speed;
            root_dobj->parent_gobj->anim_frame = root_dobj->anim_frame;

            if (root_dobj->anim_wait > 0.0F)
            {
                return;
            }
        }
        for (i = 0; i < ARRAY_COUNT(track_aobjs); i++)
        {
            track_aobjs[i] = NULL;
        }
        current_aobj = root_dobj->aobj;

        while (current_aobj != NULL)
        {
            if ((current_aobj->track >= nGCAnimTrackJointStart) && (current_aobj->track <= ARRAY_COUNT(track_aobjs)))
            {
                track_aobjs[current_aobj->track - nGCAnimTrackJointStart] = current_aobj;
            }
            current_aobj = current_aobj->next;
        }
        do
        {
            if (root_dobj->anim_joint.event16 == NULL)
            {
#if defined(PORT) && defined(SSB64_ANIM_DEBUG)
                {
                    static int s_appear_end_dump = 0;
                    s32 _fkind_end_dump = -1;
                    if (root_dobj->parent_gobj != NULL &&
                        (uintptr_t)root_dobj->parent_gobj->user_data.p > 0x10000)
                    {
                        _fkind_end_dump = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                    }
                    if (_fkind_end_dump == 30 && s_appear_end_dump < 200)
                    {
                        s_appear_end_dump++;
                        port_log("APPEARDUMP END-null dobj=%p wait=%f frame=%f\n",
                            (void *)root_dobj,
                            (double)root_dobj->anim_wait,
                            (double)root_dobj->anim_frame);
                    }
                }
#endif
                current_aobj = root_dobj->aobj;

                while (current_aobj != NULL)
                {
                    if (current_aobj->kind != nGCAnimKindNone)
                    {
                        current_aobj->length += root_dobj->anim_speed + root_dobj->anim_wait;
                    }
                    current_aobj = current_aobj->next;
                }
                /* Vanilla event16 joint-stream End: write this joint's leftover
                 * anim_wait (<= 0 at this point) to its own and the gobj's
                 * anim_frame, then retire the joint (anim_wait = AOBJ_ANIM_END;
                 * the play pass converts that to AOBJ_ANIM_NULL so the guard
                 * above skips it next tick). Async figatrees (SR appears: TopN
                 * root ends early, body joints run long) stay correct because
                 * each ended joint retires and stops writing
                 * parent_gobj->anim_frame, so the LAST still-running joint
                 * drives it; when it ends, its <= 0 leftover trips
                 * ftCommonAppearProcUpdate's `anim_frame <= 0` transition.
                 * Slamming AOBJ_ANIM_END here instead made the first joint to
                 * end win (with the latch) -> the appear snapped to the final
                 * pose; without the latch the gobj never went <= 0 -> soft-lock. */
                root_dobj->anim_frame = root_dobj->anim_wait;
                root_dobj->parent_gobj->anim_frame = root_dobj->anim_wait;
                root_dobj->anim_wait = AOBJ_ANIM_END;
#if defined(PORT) && defined(SSB64_ANIM_DEBUG)
                {
                    s32 _fkind_end = -1;
                    if (root_dobj->parent_gobj != NULL &&
                        (uintptr_t)root_dobj->parent_gobj->user_data.p > 0x10000)
                    {
                        _fkind_end = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                    }
                    if (_fkind_end >= 0 && _fkind_end <= 64) {
                        static s32 s_figatree_end_count = 0;
                        if ((s_figatree_end_count & 0x7) == 0) {
                            port_log("SSB64: figatree END dobj=%p fkind=%d gobj_anim_frame=%f speed=%f count=%d\n",
                                (void *)root_dobj, (int)_fkind_end,
                                (double)root_dobj->parent_gobj->anim_frame,
                                (double)root_dobj->anim_speed,
                                (int)s_figatree_end_count);
                        }
                        s_figatree_end_count++;
                    }
                }
#endif

                return;
            }
            command_kind = root_dobj->anim_joint.event16->command.opcode;
#ifdef PORT
#ifdef SSB64_ANIM_DEBUG
            /* APPEARDUMP one-shot: dump the real opcode stream the parser walks
             * for Crash's match-entry APPEAR (fkind 30). Capped to the first
             * ~200 lines so it captures one appear instance (~22 joints, a few
             * opcodes each) and never floods. Read-only; no parse change. */
            {
                static int s_appear_dump = 0;
                s32 _fkind_dump = -1;
                if (root_dobj->parent_gobj != NULL &&
                    (uintptr_t)root_dobj->parent_gobj->user_data.p > 0x10000)
                {
                    _fkind_dump = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                }
                if (_fkind_dump == 30 && s_appear_dump < 200)
                {
                    u16 *raw16 = (u16 *)root_dobj->anim_joint.event16;
                    s_appear_dump++;
                    port_log("APPEARDUMP dobj=%p op=%d raw=[0x%04x 0x%04x] toggle=%d wait=%f frame=%f\n",
                        (void *)root_dobj, (int)command_kind,
                        raw16[0], raw16[1],
                        (int)root_dobj->anim_joint.event16->command.toggle,
                        (double)root_dobj->anim_wait,
                        (double)root_dobj->anim_frame);
                }
            }
#endif /* SSB64_ANIM_DEBUG */
            if (++watchdog >= 256)
            {
                u16 *raw16 = (u16*)root_dobj->anim_joint.event16;
                u32 raw32 = *(u32*)root_dobj->anim_joint.event16;

                port_log("SSB64: ftAnimParseDObjFigatree - watchdog dobj=%p event16=%p opcode=%u wait=%f frame=%f speed=%f flags=0x%x raw16=[0x%04x 0x%04x] raw32=0x%08x\n",
                    root_dobj, root_dobj->anim_joint.event16, command_kind, root_dobj->anim_wait, root_dobj->anim_frame, root_dobj->anim_speed, root_dobj->flags,
                    raw16[0], raw16[1], raw32);
                root_dobj->anim_wait = AOBJ_ANIM_END;
                return;
            }
#endif

            switch (command_kind)
            {
            case nGCAnimEvent16SetVal0RateBlock:
            case nGCAnimEvent16SetVal0Rate:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif
                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->value_base = track_aobjs[i]->value_target;
                        track_aobjs[i]->value_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 0);

                        track_aobjs[i]->rate_base = track_aobjs[i]->rate_target;
                        track_aobjs[i]->rate_target = 0.0F;

                        track_aobjs[i]->kind = nGCAnimKindCubic;

                        if (payload != 0.0F)
                        {
                            track_aobjs[i]->length_invert = 1.0F / payload;
                        }
                        track_aobjs[i]->length = -root_dobj->anim_wait - root_dobj->anim_speed;
                    }
                }
                if (command_kind == nGCAnimEvent16SetVal0RateBlock)
                {
                    root_dobj->anim_wait += payload;
                }
                break;

            case nGCAnimEvent16SetValBlock:
            case nGCAnimEvent16SetVal:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif
                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->value_base = track_aobjs[i]->value_target;
                        track_aobjs[i]->value_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 0);

                        track_aobjs[i]->kind = nGCAnimKindLinear;

                        if (payload != 0.0F)
                        {
                            track_aobjs[i]->rate_base = (track_aobjs[i]->value_target - track_aobjs[i]->value_base) / payload;
                        }
                        track_aobjs[i]->length = -root_dobj->anim_wait - root_dobj->anim_speed;
                        track_aobjs[i]->rate_target = 0.0F;
                    }
                }
                if (command_kind == nGCAnimEvent16SetValBlock)
                {
                    root_dobj->anim_wait += payload;
                }
                break;

            case nGCAnimEvent16SetValRateBlock:
            case nGCAnimEvent16SetValRate:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif
                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->value_base = track_aobjs[i]->value_target;
                        track_aobjs[i]->value_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 0);

                        track_aobjs[i]->rate_base = track_aobjs[i]->rate_target;
                        track_aobjs[i]->rate_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 1);

                        track_aobjs[i]->kind = nGCAnimKindCubic;

                        if (payload != 0.0F)
                        {
                            track_aobjs[i]->length_invert = 1.0F / payload;
                        }
                        track_aobjs[i]->length = -root_dobj->anim_wait - root_dobj->anim_speed;
                    }
                }
                if (command_kind == nGCAnimEvent16SetValRateBlock)
                {
                    root_dobj->anim_wait += payload;
                }
                break;

            case nGCAnimEvent16SetTargetRate:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif

                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->rate_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 1);
                    }
                }
                break;

            case nGCAnimEvent16Block:
                if (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle)
                {
#ifdef PORT
                    s16 _block_payload = AObjAnimAdvance(root_dobj->anim_joint.event16)->s;
                    if (_block_payload == (s16)0x8000) {
                        /* SR figatree end-of-opcode-stream terminator. Read as a
                         * signed -32768 wait it kept the do-while loop running
                         * into the out-of-line keyframe floats, decoding them as
                         * opcodes -> garbage waits -> the 256-iter watchdog
                         * force-ended every joint on frame 0 -> APPEAR snapped.
                         * Terminate this joint's stream so the next loop pass
                         * takes the clean End path; the TraI keyframes already
                         * parsed for this joint stand. */
#ifdef SSB64_ANIM_DEBUG
                        {
                            static int s_appear_term_dump = 0;
                            s32 _fkind_term_dump = -1;
                            if (root_dobj->parent_gobj != NULL &&
                                (uintptr_t)root_dobj->parent_gobj->user_data.p > 0x10000)
                            {
                                _fkind_term_dump = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                            }
                            if (_fkind_term_dump == 30 && s_appear_term_dump < 200)
                            {
                                s_appear_term_dump++;
                                port_log("APPEARDUMP 0x8000-term dobj=%p wait=%f frame=%f\n",
                                    (void *)root_dobj,
                                    (double)root_dobj->anim_wait,
                                    (double)root_dobj->anim_frame);
                            }
                        }
#endif /* SSB64_ANIM_DEBUG */
                        root_dobj->anim_joint.event16 = NULL;
                        break;
                    }
#ifdef SSB64_ANIM_DEBUG
                    if (_block_payload > 1000 || _block_payload < -1000) {
                        s32 _fkind_blk = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                        static s32 s_blk_log = 0;
                        if ((s_blk_log & 0x1F) == 0) {
                            port_log("SSB64: figatree Block-huge fkind=%d dobj=%p payload_s=%d payload_u=%u prev_wait=%f\n",
                                (int)_fkind_blk, (void *)root_dobj,
                                (int)_block_payload, (unsigned)(uint16_t)_block_payload,
                                (double)root_dobj->anim_wait);
                        }
                        s_blk_log++;
                    }
#endif /* SSB64_ANIM_DEBUG */
                    root_dobj->anim_wait += _block_payload;
#else
                    root_dobj->anim_wait += AObjAnimAdvance(root_dobj->anim_joint.event16)->u;
#endif
                }
                break;

            case nGCAnimEvent16SetValAfterBlock:
            case nGCAnimEvent16SetValAfter:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif
                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->value_base = track_aobjs[i]->value_target;
                        track_aobjs[i]->value_target = ftAnimGetTargetValue(AObjAnimAdvance(root_dobj->anim_joint.event16)->s, i + nGCAnimTrackJointStart, 0);

                        track_aobjs[i]->kind = nGCAnimKindStep;

                        track_aobjs[i]->length_invert = payload;
                        track_aobjs[i]->length = -root_dobj->anim_wait - root_dobj->anim_speed;

                        track_aobjs[i]->rate_target = 0.0F;
                    }
                }
                if (command_kind == nGCAnimEvent16SetValAfterBlock)
                {
                    root_dobj->anim_wait += payload;
                }
                break;

            case nGCAnimEvent16Loop:
                AObjAnimAdvance(root_dobj->anim_joint.event16);

                root_dobj->anim_joint.event16 += root_dobj->anim_joint.event16->s / 2;

                root_dobj->anim_frame = -root_dobj->anim_wait;
                root_dobj->parent_gobj->anim_frame = -root_dobj->anim_wait;

                if (root_dobj->is_anim_root != FALSE)
                {
                    if (root_dobj->parent_gobj->func_anim != NULL)
                    {
                        root_dobj->parent_gobj->func_anim(root_dobj, -2, 0);
                    }
                }
                break;

            case nGCAnimEvent1611:
                flags = root_dobj->anim_joint.event16->command.flags;
#ifdef PORT
                if ((flags & (1 << (nGCAnimTrackTraI - nGCAnimTrackJointStart))) && (sFTAnimTraICommandLogCount < 32))
                {
                    u16 *raw16 = (u16*)root_dobj->anim_joint.event16;

                    sFTAnimTraICommandLogCount++;
                    port_log("SSB64: ftAnim TraI command - dobj=%p cmd=%u flags=0x%x event16=%p raw16=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x]\n",
                        root_dobj, command_kind, flags, root_dobj->anim_joint.event16, raw16[0], raw16[1], raw16[2], raw16[3], raw16[4], raw16[5], raw16[6], raw16[7]);
                }
#endif

                /* SR character figatrees use 0x8000 as a Block payload to
                 * mean "fall through to End" (sign-extends to -32768, makes
                 * anim_wait negative). The decomp's `.u` read interprets
                 * that as +32768, freezing the animation. Read as `.s` so
                 * the high bit sign-extends correctly. Mario's vanilla
                 * payloads never set bit 15, so .s == .u for him. */
                payload = (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle) ? (f32)AObjAnimAdvance(root_dobj->anim_joint.event16)->s : 0.0F;

                for (i = 0; i < ARRAY_COUNT(track_aobjs); i++, flags = flags >> 1)
                {
                    if (!(flags)) 
                    {
                        break;
                    }
                    if (flags & 1)
                    {
                        if (track_aobjs[i] == NULL)
                        {
                            track_aobjs[i] = gcAddAObjForDObj(root_dobj, i + nGCAnimTrackJointStart);
                        }
                        track_aobjs[i]->length += payload;
                    }
                }
                break;

            case nGCAnimEvent16SetTranslateInterp:
                AObjAnimAdvance(root_dobj->anim_joint.event16);

                if (track_aobjs[nGCAnimTrackTraI - nGCAnimTrackJointStart] == NULL)
                {
                    track_aobjs[nGCAnimTrackTraI - nGCAnimTrackJointStart] = gcAddAObjForDObj(root_dobj, nGCAnimTrackTraI);
                }
#ifdef PORT
                {
                    void *interp = root_dobj->anim_joint.event16 + (root_dobj->anim_joint.event16->s / 2);

                    track_aobjs[nGCAnimTrackTraI - nGCAnimTrackJointStart]->interpolate = interp;

                    if (sFTAnimTranslateInterpLogCount < 16)
                    {
                        sFTAnimTranslateInterpLogCount++;

                        if (interp != NULL)
                        {
                            u32 *interp_words = (u32*)interp;

                            port_log("SSB64: ftAnim SetTranslateInterp - dobj=%p rel=%d interp=%p words=[0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x]\n",
                                root_dobj, root_dobj->anim_joint.event16->s, interp,
                                interp_words[0], interp_words[1], interp_words[2], interp_words[3], interp_words[4], interp_words[5]);
                        }
                        else port_log("SSB64: ftAnim SetTranslateInterp - dobj=%p rel=%d interp=NULL\n",
                            root_dobj, root_dobj->anim_joint.event16->s);
                    }
                }

                AObjAnimAdvance(root_dobj->anim_joint.event16);
#else
                track_aobjs[nGCAnimTrackTraI - nGCAnimTrackJointStart]->interpolate = root_dobj->anim_joint.event16 + (root_dobj->anim_joint.event16->s / 2);

                AObjAnimAdvance(root_dobj->anim_joint.event16);
#endif
                break;

            case nGCAnimEvent32End:
                current_aobj = root_dobj->aobj;

                while (current_aobj != NULL)
                {
                    if (current_aobj->kind != nGCAnimKindNone)
                    {
                        current_aobj->length += root_dobj->anim_speed + root_dobj->anim_wait;
                    }
                    current_aobj = current_aobj->next;
                }
                /* Vanilla event16 joint-stream End: write this joint's leftover
                 * anim_wait (<= 0 at this point) to its own and the gobj's
                 * anim_frame, then retire the joint (anim_wait = AOBJ_ANIM_END;
                 * the play pass converts that to AOBJ_ANIM_NULL so the guard
                 * above skips it next tick). Async figatrees (SR appears: TopN
                 * root ends early, body joints run long) stay correct because
                 * each ended joint retires and stops writing
                 * parent_gobj->anim_frame, so the LAST still-running joint
                 * drives it; when it ends, its <= 0 leftover trips
                 * ftCommonAppearProcUpdate's `anim_frame <= 0` transition.
                 * Slamming AOBJ_ANIM_END here instead made the first joint to
                 * end win (with the latch) -> the appear snapped to the final
                 * pose; without the latch the gobj never went <= 0 -> soft-lock. */
                root_dobj->anim_frame = root_dobj->anim_wait;
                root_dobj->parent_gobj->anim_frame = root_dobj->anim_wait;
                root_dobj->anim_wait = AOBJ_ANIM_END;
#if defined(PORT) && defined(SSB64_ANIM_DEBUG)
                {
                    s32 _fkind_end = -1;
                    if (root_dobj->parent_gobj != NULL &&
                        (uintptr_t)root_dobj->parent_gobj->user_data.p > 0x10000)
                    {
                        _fkind_end = (s32)(*(s32 *)((char *)root_dobj->parent_gobj->user_data.p + 16));
                    }
                    if (_fkind_end >= 0 && _fkind_end <= 64) {
                        static s32 s_figatree_op_end_count = 0;
                        if ((s_figatree_op_end_count & 0x7) == 0) {
                            port_log("SSB64: figatree End-op dobj=%p fkind=%d gobj_anim_frame=%f speed=%f is_anim_root=%d count=%d\n",
                                (void *)root_dobj, (int)_fkind_end,
                                (double)root_dobj->parent_gobj->anim_frame,
                                (double)root_dobj->anim_speed,
                                (int)root_dobj->is_anim_root,
                                (int)s_figatree_op_end_count);
                        }
                        s_figatree_op_end_count++;
                    }
                }
#endif

                if (root_dobj->is_anim_root != FALSE)
                {
                    if (root_dobj->parent_gobj->func_anim != NULL)
                    {
                        root_dobj->parent_gobj->func_anim(root_dobj, -1, 0);
                    }
                }
                return;

            case nGCAnimEvent16SetFlags:
                root_dobj->flags = root_dobj->anim_joint.event16->command.flags;

                if (AObjAnimAdvance(root_dobj->anim_joint.event16)->command.toggle)
                {
                    /* Sign-extend per the figatree-payload rule above. */
                    root_dobj->anim_wait += AObjAnimAdvance(root_dobj->anim_joint.event16)->s;
                }
                break;

            default:
                break;
            }
        } 
        while (root_dobj->anim_wait <= 0.0F);
    }
}

// 0x800ECCA4 - Unused?
void func_ovl2_800ECCA4(GObj *gobj)
{
    DObj *main_dobj = DObjGetStruct(gobj);
    MObj *mobj;

    while (main_dobj != NULL)
    {
        ftAnimParseDObjFigatree(main_dobj);
        gcPlayDObjAnimJoint(main_dobj);

        mobj = main_dobj->mobj;

        while (mobj != NULL)
        {
            gcParseMObjMatAnimJoint(mobj);
            gcPlayMObjMatAnim(mobj);

            mobj = mobj->next;
        }
        if (main_dobj->child != NULL)
        {
            main_dobj = main_dobj->child;
        }
        else if (main_dobj->sib_next != NULL)
        {
            main_dobj = main_dobj->sib_next;
        }
        else while (TRUE)
        {
            if (main_dobj->parent == DOBJ_PARENT_NULL)
            {
                main_dobj = NULL;

                break;
            }
            else if (main_dobj->parent->sib_next != NULL)
            {
                main_dobj = main_dobj->parent->sib_next;

                break;
            }
            else main_dobj = main_dobj->parent;
        }
    }
}
