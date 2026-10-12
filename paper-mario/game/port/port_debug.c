/* Test switches for the native game (M2). Game side. Off unless set, so the shipped game never takes them:
 *   PM64_MAP=<map>[,<entry>[,<story progress>]]   start in that map instead of the logos: a new game's data, the
 *                                                 story where you ask (default: the intro), the map's own entrance
 *                                                 walk-in (kmr_20 | kmr_20,1 | nok_01,0,-64)
 *   PM64_PARTNER=<n>                              that partner joins and follows (1 Goombario ... 8 Watt, 9 Sushie ...)
 *   PM64_BATTLE=<battle id>[,<stage id>]          start in that battle (0x0100 = area 1's formation 0), with the
 *                                                 attract demo's player (level 3, every partner at level 2, Goombario)
 *                                                 but no demo input: PM64_KEYS plays it
 *   PM64_SHROUD=<amount>[,<frame>]                from that game frame (150) the scene takes a Star Power's darkening
 *                                                 at that amount (0..254): the renderer's shroud test (round 3)
 *   PM64_UNDERWATER=<frame>                       at that game frame, Sushie's Tidal Wave screen ripple (fx_underwater,
 *                                                 as battle/partner/sushie.c starts it): the G_MODIFYVTX test
 *   PM64_DEMOTIDAL=1                              the attract demo plays its Parakarry battle (demo scene 9) over and
 *                                                 over, with Sushie as the partner and Tidal Wave as the move (the
 *                                                 ripple from the real move). The same writes as GameShark codes
 *                                                 (m64ref -- --gs "80074096 0009;D00740CE 0073;8010F2A2 0007;
 *                                                 D00740CE 0073;810DC1EC 00AC") hang the real game at the move's
 *                                                 action command, so the reference uses PM64_DEMORIPPLE's scene
 *   PM64_DEMORIPPLE=1                             the same demo battle with Parakarry, its script's first three
 *                                                 commands replaced by PlayEffect(EFFECT_UNDERWATER, 0, -50, 20, 0, 1,
 *                                                 120): the ripple from the battle's start. GameShark codes make the
 *                                                 real game do the same (refs: the BUILDLOG, 2026-10-09)
 *   PM64_SEED=<n>                                 the random seed instead of the boot time's: two runs draw the same
 *                                                 frames, for renderer A/Bs in the real game (the sampler round)
 * The map sweep (tools/port/map_sweep.sh) enters every map this way to find what crashes before a player does; the
 * battle sweep (tools/port/battle_sweep.sh) every battle. */
#include "common.h"
#include "game_modes.h"
#include "port.h"
#include "npc.h"
#include "effects.h"

char *getenv(const char *);

extern AreaConfig gAreas[];

static s32 port_atoi(const char **p) {
    s32 v = 0, neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return neg ? -v : v;
}

static s32 same_name(const char *a, const char *b, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) if (a[i] != b[i] || a[i] == 0) return 0;
    return a[n] == 0;
}

void setup_demo_player(void);
void load_battle(s32 battleID);
void hud_element_clear_cache(void);
void initialize_status_bar(void);
void set_battle_stage(s32 stageID);

static s32 port_strtol(const char **p) {
    s32 v = 0;
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        for (*p += 2;; (*p)++) {
            const char c = **p;
            if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
            else break;
        }
        return v;
    }
    return port_atoi(p);
}

extern int port_quit;
typedef struct { const char *name; s32 formationSize; void *formation; void *stage; void *onBattleStart; } PortBattle;
typedef struct { char *name; void *dmaStart, *dmaEnd, *dmaDest; PortBattle (*battles)[]; void *stages; s32 u; void *d; }
    PortBattleArea; /* battle/battle.h's Battle and BattleArea, whose header this file does not take */
extern PortBattleArea gBattleAreas[];

/* PM64_BATTLE: as load_demo_battle (battle/battle.c) sets a battle up, without its demo flags */
static s32 port_debug_battle(const char *e) {
    const char *p = e;
    s32 battleID = port_strtol(&p), stage = -1; /* BTL_STAGE_DEFAULT: the battle's own */
    s32 area = (battleID >> 8) & 0xFF, index = battleID & 0xFF, n = 0;
    if (*p == ',') { p++; stage = port_strtol(&p); }
    /* battle_names.h names more formations than some areas list (Toad Town: 5 of 13): not a battle */
    if (area < 0x30 && gBattleAreas[area].battles != NULL)
        while ((*gBattleAreas[area].battles)[n].formation != NULL) n++;
    if (index >= n) {
        port_log("PM64_BATTLE: no battle 0x%04X (area %d lists %d)\n", battleID, area, n);
        port_quit = 1;
        return TRUE;
    }
    hud_element_clear_cache(); /* as load_demo_battle: without these two the status bar's icons were never made */
    initialize_status_bar();   /* and drew digits in their places ("4 4 15/15 5 5 10/10", the user, 29 Sep) */
    setup_demo_player();
    if ((p = getenv("PM64_PARTNER")) != NULL && p[0] != 0) /* 0: none, as in the prologue's Bowser battle */
        gPlayerData.curPartner = port_atoi(&p);
    gGameStatusPtr->areaID = 0;
    gGameStatusPtr->mapID = 0;
    gGameStatusPtr->entryID = 0;
    gGameStatusPtr->context = CONTEXT_WORLD;
    gGameStatusPtr->debugEnemyContact = DEBUG_CONTACT_NONE;
    gGameStatusPtr->healthBarsEnabled = TRUE;
    evt_set_variable(NULL, GB_StoryProgress, STORY_CH0_ARRIVED_AT_TOAD_TOWN);
    gCurrentEncounter.firstStrikeType = FIRST_STRIKE_NONE;
    gCurrentEncounter.hitType = ENCOUNTER_TRIGGER_NONE;
    gCurrentEncounter.hitTier = 0;
    gCurrentEncounter.unk_07 = 0;
    gCurrentEncounter.instigatorValue = 0;
    set_battle_stage(stage);
    port_log("PM64_BATTLE: battle 0x%04X, stage %d\n", battleID, stage);
    load_battle(battleID);
    return TRUE;
}

/* PM64_SEED: the boot's random seed (port_main.c), or the time-based one it is given */
uint32_t port_debug_seed(uint32_t seed) {
    const char *p = getenv("PM64_SEED");
    if (p == NULL || p[0] == 0) return seed;
    return (uint32_t) port_atoi(&p);
}

/* PM64_UNDERWATER: the Tidal Wave ripple at a frame (port_debug_frame) */
static void port_debug_underwater(void) {
    static s32 at = -2;
    if (at == -2) { const char *p = getenv("PM64_UNDERWATER"); at = (p != NULL && p[0] != 0) ? port_atoi(&p) : -1; }
    if (at < 0 || port_frame != (u32) at) return;
    fx_underwater(0, -50.0f, 20.0f, 0.0f, 1.0f, 120);
    port_log("PM64_UNDERWATER: at frame %u\n", port_frame);
}

/* PM64_DEMOTIDAL: the GameShark codes' writes, once a game frame (the emulator makes them every retrace): the next
 * demo scene (gGameStatus.nextDemoScene), and, only while that battle runs (demoBattleFlags 0x73: enabled, partner
 * acting, mode 0; load_demo_battle), the partner and the selected move (gPlayerData.curPartner,
 * gBattleStatus.selectedMoveID). In the real game, the partner written from the start hung the intro story, and with
 * these conditions the move hangs at its action command (all three m64ref renderers), so this one is the port's. */
static void port_debug_demotidal(void) {
    static s32 on = -1;
    if (on < 0) { const char *p = getenv("PM64_DEMOTIDAL"); on = p != NULL && p[0] == '1'; }
    if (!on) return;
    gGameStatusPtr->nextDemoScene = 9;
    if ((u8) gGameStatusPtr->demoBattleFlags != 0x73) return;
    gPlayerData.curPartner = PARTNER_SUSHIE;
    gBattleStatus.selectedMoveID = MOVE_TIDAL_WAVE;
}

/* PM64_DEMORIPPLE: as the GameShark codes, once a game frame: the next demo scene, and while that battle runs, its
 * script (battle/area/dig/script/dig_03.c) starting with Call(PlayEffect_impl, EFFECT_UNDERWATER, 0, -50, 20, 0, 1,
 * 120, 0) in the 11 words of Call(UseBattleCamPreset, BTL_CAM_DEFAULT), Call(MoveBattleCamOver, 1), Wait(3); the real
 * game's copy of that script is at 0x8022A9F0 */
extern EvtScript b_area_dig_dig_03_script;
static void port_debug_demoripple(void) {
    static s32 on = -1;
    const Bytecode patch[11] = { EVT_OP_CALL, 9, (Bytecode) PlayEffect_impl, EFFECT_UNDERWATER, 0, -50, 20, 0, 1, 120, 0 };
    s32 i;
    if (on < 0) { const char *p = getenv("PM64_DEMORIPPLE"); on = p != NULL && p[0] == '1'; }
    if (!on) return;
    gGameStatusPtr->nextDemoScene = 9;
    if ((u8) gGameStatusPtr->demoBattleFlags != 0x73) return;
    for (i = 0; i < 11; i++) b_area_dig_dig_03_script[i] = patch[i];
}

/* PM64_SHROUD, once per game frame (port_host.c): as FadeBackgroundDarken (battle/common) turns the shroud on */
void port_debug_frame(void) {
    static s32 amount = -2, at;
    port_debug_underwater();
    port_debug_demotidal();
    port_debug_demoripple();
    if (amount == -2) {
        const char *p = getenv("PM64_SHROUD");
        amount = -1;
        if (p != NULL && p[0] != 0) {
            amount = port_atoi(&p);
            at = 150;
            if (*p == ',') { p++; at = port_atoi(&p); }
        }
    }
    if (amount < 0 || port_frame != (u32) at) return;
    mdl_set_all_tint_type(ENV_TINT_SHROUD);
    *gBackgroundTintModePtr = ENV_TINT_SHROUD;
    mdl_set_shroud_tint_params(0, 0, 0, amount);
    port_log("PM64_SHROUD: %d at frame %u\n", amount, port_frame);
}

/* called at the end of state_step_startup: TRUE when it entered a map or a battle (the logos are skipped) */
s32 port_debug_warp(void) {
    const char *e = getenv("PM64_MAP");
    const char *p;
    s32 n, a, m, entry = 0, progress = STORY_INTRO;
    const char *b = getenv("PM64_BATTLE");
    if (b != NULL && b[0] != 0) return port_debug_battle(b);
    if (e == NULL || e[0] == 0) return FALSE;
    for (n = 0; e[n] != 0 && e[n] != ','; n++) {}
    p = e + n;
    if (*p == ',') { p++; entry = port_atoi(&p); }
    if (*p == ',') { p++; progress = port_atoi(&p); }
    for (a = 0; gAreas[a].maps != NULL; a++) {
        for (m = 0; m < gAreas[a].mapCount; m++) {
            if (!same_name(gAreas[a].maps[m].id, e, n)) continue;
            gGameStatusPtr->areaID = a;
            gGameStatusPtr->mapID = m;
            gGameStatusPtr->entryID = entry;
            evt_set_variable(NULL, GB_StoryProgress, progress);
            if ((e = getenv("PM64_PARTNER")) != NULL && e[0] != 0) {
                p = e;
                n = port_atoi(&p);
                if (n > 0 && n < ARRAY_COUNT(gPlayerData.partners)) {
                    gPlayerData.partners[n].enabled = TRUE;
                    gPlayerData.curPartner = n;
                }
            }
            port_log("PM64_MAP: %s (area %d map %d) entry %d, story progress %d\n", gAreas[a].maps[m].id, a, m, entry,
                     progress);
            set_game_mode(GAME_MODE_ENTER_DEMO_WORLD); /* the entrance's walk-in, as from another map */
            return TRUE;
        }
    }
    port_log("PM64_MAP: no map named %s\n", e);
    return FALSE;
}

/* evt.c's ExecWait of something that is no script (the whale's LVarD, M2): logged from here, as a string in the
 * decomp's own code would sit in the asset region, which the ROM rebuilds */
void port_evt_bad_exec(void *source, void *readPos, void *firstLine) {
    port_log("evt: ExecWait of %p at %p (arg %08X, script from %p)\n", source, readPos, *(u32 *) readPos, firstLine);
}

/* A heap that could not give `want` bytes: its blocks, for the log (the ending parade's sprites, M2). The decomp's
 * HeapNode: next, length, allocated, entryID, capacity. */
typedef struct PortHeapNode { struct PortHeapNode *next; u32 length; u16 allocated; u16 entryID; u32 capacity; } PortHeapNode;
void port_heap_report(const char *heap, void *head, u32 want) {
    PortHeapNode *n = (PortHeapNode *) (((u32) head + 15) & ~15u);
    u32 used = 0, freeTotal = 0, largest = 0, blocks = 0;
    for (; n != NULL && blocks < 4096; n = n->next, blocks++) {
        if (n->allocated) {
            used += n->length;
            if (blocks < 64) port_log("  %s heap block %u: %u bytes\n", heap, blocks, n->length);
        } else {
            freeTotal += n->length;
            if (n->length > largest) largest = n->length;
        }
    }
    port_log("%s heap: no %u bytes: %u blocks, %u used, %u free, largest free %u\n", heap, want, blocks, used, freeTotal,
             largest);
}
