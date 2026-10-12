#include <ft/fighter.h>
#ifdef PORT
extern void port_log(const char *fmt, ...);
#include "fighter_registry.h"
#endif

// // // // // // // // // // // //
//                               //
//       INITIALIZED DATA        //
//                               //
// // // // // // // // // // // //

// 0x80188C00
void (*dFTKirbySpecialNStatusList[/* */])(GObj*) = 
{
    ftKirbyCopyMarioSpecialNSetStatus,
    ftKirbyCopyFoxSpecialNSetStatus,
    ftKirbyCopyDonkeySpecialNStartSetStatus,
    ftKirbyCopySamusSpecialNStartSetStatus,
    ftKirbyCopyMarioSpecialNSetStatus,
    ftKirbyCopyLinkSpecialNSetStatus,
    ftKirbyCopyYoshiSpecialNSetStatus,
    ftKirbyCopyCaptainSpecialNSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbyCopyPikachuSpecialNSetStatus,
    ftKirbyCopyPurinSpecialNSetStatus,
    ftKirbyCopyNessSpecialNSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbyCopyMarioSpecialNSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus,
    ftKirbySpecialNStartSetStatus
};

// 0x80188C6C
void (*dFTCommonSpecialNStatusList[/* */])(GObj*) = 
{
    ftMarioSpecialNSetStatus,
    ftFoxSpecialNSetStatus,
    ftDonkeySpecialNStartSetStatus,
    ftSamusSpecialNStartSetStatus,
    ftMarioSpecialNSetStatus,
    ftLinkSpecialNSetStatus,
    ftYoshiSpecialNSetStatus,
    ftCaptainSpecialNSetStatus,
    ftKirbySpecialNSetStatusSelect,
    ftPikachuSpecialNSetStatus,
    ftPurinSpecialNSetStatus,
    ftNessSpecialNSetStatus,
    ftMarioSpecialNSetStatus,
    ftMarioSpecialNSetStatus,
    ftMarioSpecialNSetStatus,
    ftFoxSpecialNSetStatus,
    ftDonkeySpecialNStartSetStatus,
    ftSamusSpecialNStartSetStatus,
    ftMarioSpecialNSetStatus,
    ftLinkSpecialNSetStatus,
    ftMarioSpecialNSetStatus,           // un bro momento
    ftCaptainSpecialNSetStatus,
    ftKirbySpecialNSetStatusSelect,
    ftPikachuSpecialNSetStatus,
    ftPurinSpecialNSetStatus,
    ftNessSpecialNSetStatus,
    ftDonkeySpecialNStartSetStatus
};

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

// 0x80151060
void ftKirbySpecialNSetStatusSelect(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);

#ifdef PORT
    /* Synth copy power: copy_id is the inhaled synth's own fkind, which OOBs
     * the 27-entry vanilla dispatch table. SR patches kirby_ground_nsp[victim]
     * to the synth's ground neutral-B (e.g. Crash -> CrashNSP.ground_initial_),
     * so run the synth's registered ground-N handler directly on Kirby's gobj
     * -- Kirby performs the synth's REAL spin, never the parent's special. */
    if (fp->passive_vars.kirby.copy_id >= nFTKindEnumCount)
    {
        PortFTSpecialEnterFn h =
            port_fighter_special_handler(fp->passive_vars.kirby.copy_id, PORT_FIGHTER_SPECIAL_N);
        if (h != NULL)
        {
            /* The synth handler calls ftMainSetStatus with the synth's own
             * special action id; route that status's desc lookup to the synth's
             * table for the duration of the call, then restore. */
            port_kirby_set_copy_special_fkind(fp->passive_vars.kirby.copy_id);
            h(fighter_gobj);
            port_kirby_set_copy_special_fkind(-1);
        }
        else
        {
            /* No registered ground-N handler for this synth: fall back to
             * Kirby's own inhale start rather than indexing OOB. */
            ftKirbySpecialNStartSetStatus(fighter_gobj);
        }
        return;
    }
#endif
    dFTKirbySpecialNStatusList[fp->passive_vars.kirby.copy_id](fighter_gobj);
}

// 0x80151098
sb32 ftCommonSpecialNCheckInterruptCommon(GObj *fighter_gobj)
{
    FTStruct *fp = ftGetStruct(fighter_gobj);
    FTAttributes *attr = fp->attr;

#ifdef PORT
    if (fp->input.pl.button_tap & fp->input.button_mask_b)
    {
        port_log("SSB64: SpecialNCheck fkind=%d B_tap=0x%04X have_n=%d stick_y=%d (need %d..%d)\n",
            fp->fkind, fp->input.pl.button_tap & fp->input.button_mask_b,
            attr->is_have_specialn, fp->input.pl.stick_range.y,
            FTCOMMON_SPECIALLW_STICK_RANGE_MIN, FTCOMMON_SPECIALHI_STICK_RANGE_MIN);
    }
#endif
    if ((fp->input.pl.button_tap & fp->input.button_mask_b) && (attr->is_have_specialn))
    {
        if ((fp->input.pl.stick_range.y < FTCOMMON_SPECIALHI_STICK_RANGE_MIN) && (fp->input.pl.stick_range.y > FTCOMMON_SPECIALLW_STICK_RANGE_MIN))
        {
#ifdef PORT
            port_log("SSB64: SpecialNCheck -> PASS, calling neutral-B for fkind=%d\n", fp->fkind);
#endif
            if ((fp->input.pl.stick_range.x * fp->lr) < FTCOMMON_SPECIALN_TURN_STICK_RANGE_MIN)
            {
                ftParamSetStickLR(fp);
            }
#ifdef PORT
            {
                PortFTSpecialEnterFn h = port_fighter_special_handler(fp->fkind, PORT_FIGHTER_SPECIAL_N);
                if (h != NULL) h(fighter_gobj);
            }
#else
            dFTCommonSpecialNStatusList[fp->fkind](fighter_gobj);
#endif

            return TRUE;
        }
    }
    return FALSE;
}
