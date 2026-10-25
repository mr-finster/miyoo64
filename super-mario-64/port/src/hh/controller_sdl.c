/**
 * Keys -> the N64 pad (osContInit / osContGetReadData, which sm64-port's
 * controller_entry_point.c provided). The window backends feed key events in:
 * gfx_sdl.c (desktop, SDL2 scancodes) and gfx_sdl12.c (handheld, SDL 1.2 syms).
 *
 * Desktop keys: arrows = stick, X/Space = A, Z = B, LShift = Z-trigger,
 * Enter = START, W = R, I/J/K/L = C-up/left/down/right.
 *
 * Headless autopilot (SM64_AUTO=1): taps START until gameplay runs, so a run
 * goes Nintendo logo -> Mario head -> (level select with SM64_LEVEL) -> a level
 * without a keyboard. SM64_AUTO=2 then also wanders Mario about.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ultra64.h>
#include "macros.h"
#include "types.h"
#include "sm64.h"
#include "object_fields.h"
#include "game/object_list_processor.h"
#include "engine/math_util.h"
#include "surface_terrains.h"
#include "sounds.h"

#if defined(TARGET_PC)
#include <SDL2/SDL_scancode.h>
#elif defined(MIYOO)
#include "miyoo_panel.h" /* the Mini's key codes */
#endif

#include "savestate.h"
#if defined(MIYOO)
#include "miyoo_remap.h"
#endif
/* live hardware state, not game state: a load must not restore keys held at save time */
static bool sKeys[512] SAVESTATE_EXCLUDE;
static int sAuto SAVESTATE_EXCLUDE = -1; /* SM64_AUTO: -1 unread, 0 off, 1 on */
static unsigned sFrame SAVESTATE_EXCLUDE;

void controller_sdl_key(int scancode, bool down) {
    if (scancode >= 0 && scancode < 512) sKeys[scancode] = down;
}

/* Handheld: the buttons arrive as keyboard SDLK syms (kit miyoo_panel.h). Stored by sym; MENU is
 * the overlay's, handled in gfx_sdl12.c before it reaches here. */
#if defined(MIYOO)
/* MENU > CONTROLS (kit miyoo_remap.c): the layout below, in play, as the screen shows it; a remap moves the
 * pressed buttons, so every mode and chord here follows. */
const struct miyoo_remap_row miyoo_remap_rows[] = {
    { MR_A, MB_A, 0, 0, "JUMP" },
    { MR_B, MB_B, 0, 0, "PUNCH, KICK" },
    { MR_Z, MB_X, 0, 0, "CROUCH, POUND" },
    { MR_R, MB_Y, 0, 0, "CAMERA MODE" },
    { MR_START, MB_START, 0, 0, "PAUSE" },
    { MR_CUP, MB_L2, MB_SELECT, 0, "CAMERA" },
    { MR_CDOWN, MB_R2, 0, 0, "CAMERA" },
    { MR_CLEFT, MB_L1, 0, 0, "CAMERA" },
    { MR_CRIGHT, MB_R1, 0, 0, "CAMERA" },
    { MR_L, MB_NONE, 0, 0, NULL },
    { MR_STICK, MB_DPAD, 0, 0, "MOVE" },
    { MR_DPAD, MB_NONE, 0, 0, NULL },
};
const int miyoo_remap_nrows = (int)(sizeof(miyoo_remap_rows) / sizeof(miyoo_remap_rows[0]));
#endif

void controller_handheld_key(int sym, int down) {
#if defined(MIYOO)
    sym = miyoo_remap_key(sym); /* MENU > CONTROLS */
#endif
    if (sym >= 0 && sym < 512) sKeys[sym] = (bool)down;
}

/* SM64_LEVEL=<id> (test hook): boot through the debug level select into that
 * level (title_screen.c reads it). 0 = off. */
int port_debug_level(void) {
    static int sLevel SAVESTATE_EXCLUDE = -1;
    { extern int port_bench2_on(void), port_bench2_level(void); if (port_bench2_on()) return port_bench2_level(); } /* the level bench (hh_bench.c) */
    if (sLevel < 0) { const char *e = getenv("SM64_LEVEL"); sLevel = e ? atoi(e) : 0; }
    return sLevel;
}

/* SM64_ROOM=<n> (test hook): see geo_switch_area. 0 = off. */
int port_debug_room(void) {
    static int sRoom SAVESTATE_EXCLUDE = -1;
    if (sRoom < 0) { const char *e = getenv("SM64_ROOM"); sRoom = e ? atoi(e) : 0; }
    return sRoom;
}

static void read_pad(OSContPad *pad) {
    u16 b = 0;
    s8 sx = 0, sy = 0;

#if defined(MIYOO)
    /* Miyoo Mini (its SDL's keys, kit miyoo_panel.h): A jump, B punch, X = Z (crouch, ground pound), Y = R (camera
     * mode), L1/R1 turn the camera (C-left/C-right), L2 = C-up (look around), R2 = C-down (camera back),
     * SELECT = C-up too. MENU (Escape) opens the menu in gfx_sdl12.c and never reaches here. */
    if (sKeys[MIYOO_KEY_A])      b |= A_BUTTON;
    if (sKeys[MIYOO_KEY_B])      b |= B_BUTTON;
    if (sKeys[MIYOO_KEY_START])  b |= START_BUTTON;
    if (sKeys[MIYOO_KEY_X])      b |= Z_TRIG;
    if (sKeys[MIYOO_KEY_Y])      b |= R_TRIG;
    if (sKeys[MIYOO_KEY_L1])     b |= L_CBUTTONS;
    if (sKeys[MIYOO_KEY_R1])     b |= R_CBUTTONS;
    if (sKeys[MIYOO_KEY_L2] || sKeys[MIYOO_KEY_SELECT]) b |= U_CBUTTONS;
    if (sKeys[MIYOO_KEY_R2])     b |= D_CBUTTONS;
    if (sKeys[MIYOO_KEY_UP])    sy =  80;
    if (sKeys[MIYOO_KEY_DOWN])  sy = -80;
    if (sKeys[MIYOO_KEY_LEFT])  sx = -80;
    if (sKeys[MIYOO_KEY_RIGHT]) sx =  80;
#elif defined(TARGET_PC)
    if (sKeys[SDL_SCANCODE_X] || sKeys[SDL_SCANCODE_SPACE]) b |= A_BUTTON;
    if (sKeys[SDL_SCANCODE_Z])      b |= B_BUTTON;
    if (sKeys[SDL_SCANCODE_RETURN]) b |= START_BUTTON;
    if (sKeys[SDL_SCANCODE_LSHIFT]) b |= Z_TRIG;
    if (sKeys[SDL_SCANCODE_W])      b |= R_TRIG;
    if (sKeys[SDL_SCANCODE_I])      b |= U_CBUTTONS;
    if (sKeys[SDL_SCANCODE_K])      b |= D_CBUTTONS;
    if (sKeys[SDL_SCANCODE_J])      b |= L_CBUTTONS;
    if (sKeys[SDL_SCANCODE_L])      b |= R_CBUTTONS;
    if (sKeys[SDL_SCANCODE_UP])    sy =  80;
    if (sKeys[SDL_SCANCODE_DOWN])  sy = -80;
    if (sKeys[SDL_SCANCODE_LEFT])  sx = -80;
    if (sKeys[SDL_SCANCODE_RIGHT]) sx =  80;
#endif

    { /* SM64_PRESS=<frame>:<buttons>[,...] (test hook): hold those buttons for 4
       * frames from that frame -- S START, A, B, Z, R, U/D/L/C C-buttons (u d l r),
       * and the stick as ^ v < > -- to reach screens the autopilot doesn't (the
       * pause menu, a dialog). Frames count from boot, like SM64_DUMP. */
        static struct { unsigned frame; u16 buttons; s8 x, y; } sPress[16] SAVESTATE_EXCLUDE;
        static int sPressN SAVESTATE_EXCLUDE = -1;
        if (sPressN < 0) {
            const char *e = getenv("SM64_PRESS");
            sPressN = 0;
            while (e && *e && sPressN < 16) {
                char *end;
                unsigned f = (unsigned) strtoul(e, &end, 10);
                if (*end != ':') break;
                sPress[sPressN].frame = f; sPress[sPressN].buttons = 0; sPress[sPressN].x = sPress[sPressN].y = 0;
                for (e = end + 1; *e && *e != ','; e++) {
                    switch (*e) {
                        case 'S': sPress[sPressN].buttons |= START_BUTTON; break;
                        case 'A': sPress[sPressN].buttons |= A_BUTTON; break;
                        case 'B': sPress[sPressN].buttons |= B_BUTTON; break;
                        case 'Z': sPress[sPressN].buttons |= Z_TRIG; break;
                        case 'R': sPress[sPressN].buttons |= R_TRIG; break;
                        case 'u': sPress[sPressN].buttons |= U_CBUTTONS; break;
                        case 'd': sPress[sPressN].buttons |= D_CBUTTONS; break;
                        case 'l': sPress[sPressN].buttons |= L_CBUTTONS; break;
                        case 'r': sPress[sPressN].buttons |= R_CBUTTONS; break;
                        case '^': sPress[sPressN].y = 80; break;
                        case 'v': sPress[sPressN].y = -80; break;
                        case '<': sPress[sPressN].x = -80; break;
                        case '>': sPress[sPressN].x = 80; break;
                        default: break;
                    }
                }
                sPressN++;
                if (*e == ',') e++;
            }
        }
        for (int i = 0; i < sPressN; i++) {
            if (sFrame >= sPress[i].frame && sFrame < sPress[i].frame + 4) {
                b |= sPress[i].buttons;
                if (sPress[i].x) sx = sPress[i].x;
                if (sPress[i].y) sy = sPress[i].y;
            }
        }
    }

    { /* SM64_KEYS="<poll>:<key>+|-,..." (test hook): a pad script in the format of
       * the emulator reference script (m64ref.sh)'s M64REF_KEYS, so one script drives this port
       * and the emulator the same way (reference checks at the same instant).
       * <poll> counts reads of the pad from 1 (the game reads it once a frame);
       * key+ holds the key from that poll on, key- releases it. Keys: a b z start
       * l r, cu cd cl cr, du dd dl dr, and up down left right (the stick at 80). */
        static const char *const names[18] = { "a", "b", "z", "start", "l", "r", "cu", "cd", "cl", "cr",
                                                "du", "dd", "dl", "dr", "up", "down", "left", "right" };
        static const u16 bits[14] = { A_BUTTON, B_BUTTON, Z_TRIG, START_BUTTON, L_TRIG, R_TRIG, U_CBUTTONS,
                                      D_CBUTTONS, L_CBUTTONS, R_CBUTTONS, U_JPAD, D_JPAD, L_JPAD, R_JPAD };
        static bool sHeld[18] SAVESTATE_EXCLUDE;
        const char *p = getenv("SM64_KEYS");
        const unsigned long poll = sFrame + 1ul;
        for (; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
            char *e;
            const unsigned long at = strtoul(p, &e, 10);
            if (at != poll || *e != ':') continue;
            for (int k = 0; k < 18; k++) {
                const size_t n = strlen(names[k]);
                if (!strncmp(e + 1, names[k], n) && (e[1 + n] == '+' || e[1 + n] == '-')) sHeld[k] = e[1 + n] == '+';
            }
        }
        for (int k = 0; k < 14; k++) if (sHeld[k]) b |= bits[k];
        if (sHeld[14]) sy = 80; else if (sHeld[15]) sy = -80;
        if (sHeld[16]) sx = -80; else if (sHeld[17]) sx = 80;
    }

    { /* SM64_WARP="x,y,z,yaw" (test hook, 2026-10-06: the Snowman's Land yell recordings): the pad read after one
       * with L held puts Mario at x,y,z facing yaw (his action is left alone: the game's own step makes him stand,
       * walk or fall from there). The emulator does the same with GameShark codes on L (D033AFA1 0020 + 81 writes
       * of gMarioStates' pos and faceAngle), so one pad script drives both. */
        static int sWarpOn SAVESTATE_EXCLUDE = -1;
        static float sWarp[4] SAVESTATE_EXCLUDE;
        static u16 sPrevB SAVESTATE_EXCLUDE;
        static int sAvLog SAVESTATE_EXCLUDE = -1, sWarpLog SAVESTATE_EXCLUDE = -1;
        if (sAvLog < 0) { sAvLog = getenv("SM64_AVLOG") != NULL; sWarpLog = getenv("SM64_WARPLOG") != NULL; }
        if (sWarpOn < 0) {
            const char *e = getenv("SM64_WARP");
            sWarpOn = e && sscanf(e, "%f,%f,%f,%f", &sWarp[0], &sWarp[1], &sWarp[2], &sWarp[3]) == 4;
        }
        if (sWarpOn && (sPrevB & L_TRIG)) {
            extern struct MarioState gMarioStates[];
            extern void port_log_always(const char *fmt, ...);
            struct MarioState *m = &gMarioStates[0];
            m->pos[0] = sWarp[0]; m->pos[1] = sWarp[1]; m->pos[2] = sWarp[2];
            m->faceAngle[1] = (s16)(int)sWarp[3];
            port_log_always("warp: Mario to (%d %d %d) at poll %lu\n", (int)sWarp[0], (int)sWarp[1], (int)sWarp[2], sFrame + 1ul);
        }
        sPrevB = b;
        if (sAvLog) { /* the samples pushed before this frame's audio: frame_N's sound starts there */
            extern u32 port_audio_out_pushed(void);
            extern void port_log_always(const char *fmt, ...);
            port_log_always("av: frame %u pushed %u\n", sFrame, port_audio_out_pushed());
        }
        if (sWarpOn && sWarpLog) { /* where he is, on what, doing what */
            extern struct MarioState gMarioStates[];
            extern void port_log_always(const char *fmt, ...);
            const struct MarioState *m = &gMarioStates[0];
            if (m->floor != NULL)
                port_log_always("warplog: poll %lu pos %d %d %d floor %d type %02X action %08X\n", sFrame + 1ul, (int)m->pos[0],
                                (int)m->pos[1], (int)m->pos[2], (int)m->floorHeight, m->floor->type, (unsigned)m->action);
        }
    }

    { /* SM64_METAL=1 (test hook): metal Mario, as mupen64plus's GameShark code
       * 8033B177 0015 makes him (the flags' low byte = normal cap + metal cap +
       * cap on head, rewritten every frame) -- a large env-mapped object for
       * texgen checks against the emulator (the emulator reference script (m64ref.sh)). */
        extern struct MarioState gMarioStates[];
        static int sMetal SAVESTATE_EXCLUDE = -1;
        if (sMetal < 0) { const char *e = getenv("SM64_METAL"); sMetal = e ? e[0] - '0' : 0; }
        /* SM64_METAL=2: the vanish cap instead (GameShark 8033B177 0013), the noise alpha the invincibility blink uses too */
        if (sMetal == 1) gMarioStates[0].flags = (gMarioStates[0].flags & ~0xFFu) | MARIO_NORMAL_CAP | MARIO_METAL_CAP | MARIO_CAP_ON_HEAD;
        if (sMetal == 2) gMarioStates[0].flags = (gMarioStates[0].flags & ~0xFFu) | MARIO_NORMAL_CAP | MARIO_VANISH_CAP | MARIO_CAP_ON_HEAD;
    }

    { /* SM64_DOORTEST=<poll> (test hook, 2026-10-06: the castle-entry hitch): at that pad read, put Mario walking
       * up to the castle grounds' main door (the warp door to the lobby's node 0), so he opens it the game's way
       * (door animation, sound, warp, the music change) without a route through the grounds. */
        static long sDoorAt SAVESTATE_EXCLUDE = -2;
        if (sDoorAt == -2) { const char *e = getenv("SM64_DOORTEST"); sDoorAt = e ? atol(e) : -1; }
        if (sDoorAt >= 0 && sFrame + 1ul == (unsigned long)sDoorAt) {
            extern struct Object gObjectPool[];
            extern struct MarioState gMarioStates[];
            extern const BehaviorScript bhvDoorWarp[];
            extern void *segmented_to_virtual(const void *);
            extern u32 set_mario_action(struct MarioState *, u32, u32);
            extern void port_log_always(const char *fmt, ...);
            const void *bhv = segmented_to_virtual(bhvDoorWarp);
            for (int i = 0; i < OBJECT_POOL_CAPACITY; i++) {
                struct Object *o = &gObjectPool[i];
                if (!o->activeFlags || o->behavior != bhv || ((o->oBehParams >> 16) & 0xff) != 0) continue;
                struct MarioState *m = &gMarioStates[0];
                const s16 yaw = (s16)o->oMoveAngleYaw;
                m->pos[0] = o->oPosX + sins(yaw) * 40.0f; m->pos[1] = o->oPosY; m->pos[2] = o->oPosZ + coss(yaw) * 40.0f;
                m->faceAngle[1] = (s16)(yaw + 0x8000);
                m->forwardVel = 8.0f;
                set_mario_action(m, ACT_WALKING, 0);
                port_log_always("doortest: Mario at the door (%d %d %d)\n", (int)m->pos[0], (int)m->pos[1], (int)m->pos[2]);
                break;
            }
        }
    }

    { /* SM64_BURNTEST=<poll>[,<poll>..][:<n>] (test hook, 2026-10-06: Mario's burn yell in Snowman's Land's freezing water): at
       * that pad read, find the burning floor (lava, freezing water) nearest Mario (the n-th nearest, default 0) and drop
       * him onto it from 300 units up, so he lands, burns and yells the game's way (ACT_LAVA_BOOST). */
        static long sBurnAt[4] SAVESTATE_EXCLUDE = { -2 };
        static int sBurnN SAVESTATE_EXCLUDE;
        if (sBurnAt[0] == -2) { /* <poll>[,<poll>...][:<n>] */
            const char *e = getenv("SM64_BURNTEST");
            for (int i = 0; i < 4; i++) sBurnAt[i] = -1;
            for (int i = 0; e && *e && i < 4; i++) { char *end; sBurnAt[i] = strtol(e, &end, 10); e = *end == ',' ? end + 1 : end; if (*end != ',') break; }
            if (e && *e == ':') sBurnN = atoi(e + 1);
        }
        int burnNow = 0;
        for (int i = 0; i < 4; i++) if (sBurnAt[i] >= 0 && sFrame + 1ul == (unsigned long)sBurnAt[i]) burnNow = 1;
        if (burnNow) {
            extern struct MarioState gMarioStates[];
            extern f32 find_floor(f32, f32, f32, struct Surface **);
            extern u32 set_mario_action(struct MarioState *, u32, u32);
            extern void port_log_always(const char *fmt, ...);
            struct MarioState *m = &gMarioStates[0];
            float best[8][4]; int nb = 0;
            for (int gx = -40; gx <= 40; gx++) for (int gz = -40; gz <= 40; gz++) {
                const float x = m->pos[0] + gx * 200.0f, z = m->pos[2] + gz * 200.0f;
                struct Surface *fl = NULL;
                float y = m->pos[1] + 3000.0f;
                for (int layer = 0; layer < 6; layer++) { /* down through the floors: the water lies under the slopes */
                    y = find_floor(x, y, z, &fl);
                    if (fl == NULL || fl->type == SURFACE_BURNING) break;
                    y -= 20.0f;
                }
                if (fl == NULL || fl->type != SURFACE_BURNING) continue;
                const float d = (float)(gx * gx + gz * gz);
                int k = nb < 8 ? nb++ : 7;
                if (k == 7 && nb == 8 && best[7][3] <= d) continue;
                best[k][0] = x; best[k][1] = y; best[k][2] = z; best[k][3] = d;
                for (; k > 0 && best[k - 1][3] > best[k][3]; k--) for (int c = 0; c < 4; c++) { float t = best[k][c]; best[k][c] = best[k - 1][c]; best[k - 1][c] = t; }
            }
            if (nb > 0) {
                const int k = sBurnN < nb ? sBurnN : nb - 1;
                /* SM64_BURNMODE: fall (default: from 300 up), far (from 2000 up: the long-fall yell first), walk (walking on it) */
                const char *mode = getenv("SM64_BURNMODE");
                const float up = mode && !strcmp(mode, "far") ? 2000.0f : mode && !strcmp(mode, "walk") ? 0.0f : 300.0f;
                m->pos[0] = best[k][0]; m->pos[1] = best[k][1] + up; m->pos[2] = best[k][2];
                m->vel[1] = 0.0f; m->forwardVel = up == 0.0f ? 8.0f : 0.0f;
                set_mario_action(m, up == 0.0f ? ACT_WALKING : ACT_FREEFALL, 0);
                port_log_always("burntest: Mario over a burning floor at (%d %d %d)\n", (int)best[k][0], (int)best[k][1], (int)best[k][2]);
            } else {
                port_log_always("burntest: no burning floor within 8000 of Mario\n");
            }
        }
    }

    { /* SM64_VOICESWEEP=<poll> (test hook, 2026-10-06): from that pad read, one Mario voice clip every 100 frames,
       * asked for as the game asks (play_sound at Mario's camera-space position, the game's sound bits), so
       * SM64_SNDLOG shows each one queued, started and how long its layer ran. */
        static const u32 kVoices[] = { SOUND_MARIO_YAH_WAH_HOO, SOUND_MARIO_HOOHOO, SOUND_MARIO_YAHOO, SOUND_MARIO_YAHOO_WAHA_YIPPEE,
            SOUND_MARIO_WAAAOOOW, SOUND_MARIO_MAMA_MIA, SOUND_MARIO_ATTACKED, SOUND_MARIO_OOOF2, SOUND_MARIO_HERE_WE_GO,
            SOUND_MARIO_ON_FIRE, SOUND_MARIO_DROWNING, SOUND_MARIO_DYING, SOUND_MARIO_WHOA, SOUND_MARIO_UH2, SOUND_MARIO_HAHA,
            SOUND_MARIO_PUNCH_YAH, SOUND_MARIO_PUNCH_HOO, SOUND_MARIO_PUNCH_WAH, SOUND_MARIO_GROUND_POUND_WAH, SOUND_MARIO_DOH,
            SOUND_MARIO_TWIRL_BOUNCE, SOUND_MARIO_PANTING_COLD, SOUND_MARIO_COUGHING1, SOUND_MARIO_EEUH, SOUND_MARIO_HRMM };
        static long sVsAt SAVESTATE_EXCLUDE = -2;
        if (sVsAt == -2) { const char *e = getenv("SM64_VOICESWEEP"); sVsAt = e ? atol(e) : -1; }
        if (sVsAt >= 0 && sFrame + 1ul >= (unsigned long)sVsAt && (sFrame + 1ul - sVsAt) % 100 == 0) {
            const unsigned long k = (sFrame + 1ul - sVsAt) / 100;
            extern struct MarioState gMarioStates[];
            extern void play_sound(s32, f32 *);
            extern void port_log_always(const char *fmt, ...);
            if (k < sizeof(kVoices) / sizeof(kVoices[0]) && gMarioStates[0].marioObj != NULL) {
                port_log_always("voicesweep: clip %lu, %08X\n", k, (unsigned)kVoices[k]);
                play_sound(kVoices[k], gMarioStates[0].marioObj->header.gfx.cameraToObject);
            }
        }
    }

    { extern u16 port_bench2_pad(void); b |= port_bench2_pad(); } /* the level bench's START and camera taps */

    if (sAuto < 0) { const char *e = getenv("SM64_AUTO"); sAuto = (e && e[0] >= '1' && e[0] <= '2') ? e[0] - '0' : 0; }
    if (sAuto) {
        /* Tap START (4 frames every 60) until Mario exists: past the Nintendo
         * logo and Mario head, and the level select under SM64_LEVEL (which
         * preselects gCurrLevelNum, so the level number can't tell). */
        extern struct Object *gMarioObject;
        static unsigned sInLevel SAVESTATE_EXCLUDE; /* frames since Mario appeared */
        if (gMarioObject == NULL) {
            sInLevel = 0;
            if ((sFrame % 60) < 4) b |= START_BUTTON;
        } else {
            if (sInLevel == 0) {
                /* the frame the START taps stop: tools/auto2keys.py turns a run
                 * into a pad script for the emulator (M64REF_KEYS) from it */
                extern void port_log(const char *fmt, ...);
                port_log("autopilot: Mario appeared at frame %u\n", sFrame);
            }
            if (sAuto == 1) {
                sInLevel = 1;
            } else if (++sInLevel > 60) {
                /* SM64_AUTO=2, wander: run, turning a quarter every 1.5 s, jump
                 * every 0.75 s -- into walls and corners, so the camera comes close
                 * to geometry (near-plane clipping tests). Deterministic. */
                static const s8 dir[4][2] = { { 0, 80 }, { 80, 0 }, { 0, -80 }, { -80, 0 } };
                const unsigned t = sInLevel / 90;
                sx = dir[t % 4][0]; sy = dir[t % 4][1];
                if ((sInLevel % 45) < 3) b |= A_BUTTON;
            }
        }
    }
    sFrame++;

    pad->button = b;
    pad->stick_x = sx;
    pad->stick_y = sy;
    pad->errnum = 0;
}

s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status) {
    *controllerBits = 1; /* one controller */
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *mq) {
    return 0;
}

void osContGetReadData(OSContPad *pad) {
    read_pad(&pad[0]);
}

/* Some callers use the generic name. */
void port_input_poll(void) { }
