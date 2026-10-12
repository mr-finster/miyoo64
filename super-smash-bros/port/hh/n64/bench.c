/* bench.c -- SSB64_BENCH=<gkind>: boot straight into the M3 benchmark fight.
 *
 * Four level-9 CPUs (Mario, Fox, Donkey Kong, Pikachu), items on "high",
 * a 1-minute timed VS match on stage <gkind> (grdef.h: 0 Peach's Castle,
 * 1 Sector Z, 2 Kongo Jungle, 3 Planet Zebes, 4 Hyrule, 5 Yoshi's Island,
 * 6 Dream Land, 7 Saffron City, 8 Mushroom Kingdom). The battle state is filled exactly as the VS
 * character select would leave it, so the fight itself runs unmodified.
 * SSB64_BENCH_MINUTES overrides the time limit. On the device the same
 * setting comes from <save dir>/bench.cfg ("stage=6"), since a launcher start
 * has no environment of its own. SSB64_BENCH_FIGHTERS=a,b,c,d replaces the four
 * fighter kinds (ftdef.h: 13 is Metal Mario), e.g. SSB64_BENCH=13
 * SSB64_BENCH_FIGHTERS=13,0,1,2 for Metal Mario on Meta Crystal, the texgen
 * (reflection) check; a VS match takes the 1P-only kinds unmodified. */
#include <stdio.h>
#include <ssb_types.h>
#include <sc/scene.h>
#include <sc/scmanager.h>
#include <ft/fighter.h>
#include <gr/ground.h>

#include "port_log.h"

extern const char *port_save_path(const char *name);
extern void port_log_always(const char *fmt, ...);
/* The decomp's own stdlib.h/string.h (first on this file's include path)
 * don't declare these. */
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern int strncmp(const char *a, const char *b, unsigned int n);

static int bench_stage(void)
{
    const char *env = getenv("SSB64_BENCH");
    char line[64];
    FILE *f;
    if (env != NULL) return atoi(env);
    f = fopen(port_save_path("bench.cfg"), "r");
    if (f == NULL) return -1;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "stage=", 6) == 0) { fclose(f); return atoi(line + 6); }
    }
    fclose(f);
    return -1;
}

void hh_bench_setup(void)
{
    u8 kinds[4] = { nFTKindMario, nFTKindFox, nFTKindDonkey, nFTKindPikachu };
    SCBattleState *bs = &gSCManagerTransferBattleState;
    const char *mins = getenv("SSB64_BENCH_MINUTES");
    const char *fighters = getenv("SSB64_BENCH_FIGHTERS");
    int stage = bench_stage();
    int i;

    if (stage < 0) return;
    for (i = 0; fighters != NULL && *fighters != '\0' && i < 4; i++) {
        int k = atoi(fighters);
        if (k >= 0 && k < nFTKindEnumCount) kinds[i] = (u8)k;
        while (*fighters != '\0' && *fighters != ',') fighters++;
        if (*fighters == ',') fighters++;
    }
    bs->game_type = nSCBattleGameTypeRoyal;
    bs->gkind = (u8)stage;
    bs->is_team_battle = FALSE;
    bs->game_rules = SCBATTLE_GAMERULE_TIME;
    bs->pl_count = 0;
    bs->cp_count = 4;
    bs->time_limit = (u8)(mins ? atoi(mins) : 1);
    bs->item_toggles = ~0;
    bs->item_appearance_rate = nSCBattleItemSwitchHigh;
    for (i = 0; i < 4; i++) {
        SCPlayerData *p = &bs->players[i];
        p->pkind = nFTPlayerKindCom;
        p->fkind = kinds[i];
        p->level = 9;
        p->handicap = 9;
        p->team = i;
        p->player = i;
        {   /* the costume the VS select gives on A (a raw 0 is not every fighter's own; round 17) */
            extern s32 ftParamGetCostumeCommonID(s32 fkind, s32 color);
            p->costume = (u8)ftParamGetCostumeCommonID(kinds[i], 0);
        }
        p->shade = 0;
        p->color = i;
        p->tag = 4;             /* "CP" */
        p->is_single_stockicon = TRUE;
    }
    gSCManagerSceneData.gkind = (u8)stage;
    gSCManagerSceneData.scene_prev = nSCKindMaps;
    gSCManagerSceneData.scene_curr = nSCKindVSBattle;
    {
        extern int hh_neon_enabled(void);
        port_log_always("SSB64: bench: 4 CPUs (kinds %d,%d,%d,%d), stage %d, %d min, items high, renderer NEON %s\n",
                        kinds[0], kinds[1], kinds[2], kinds[3], stage, bs->time_limit, hh_neon_enabled() ? "on" : "off (bench.cfg neon=0)");
    }
}
