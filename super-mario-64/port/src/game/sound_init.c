#include <ultra64.h>

#include "area.h"
#include "audio/external.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "level_table.h"
#include "level_update.h"
#include "main.h"
#include "paintings.h"
#include "print.h"
#include "profiler.h"
#include "save_file.h"
#include "seq_ids.h"
#include "sm64.h"
#include "sound_init.h"
#include "rumble_init.h"
#ifdef TARGET_HANDHELD
#include "hh/port.h"
#endif

#define MUSIC_NONE 0xFFFF

static Vec3f unused80339DC0;
static OSMesgQueue sSoundMesgQueue;
static OSMesg sSoundMesgBuf[1];
static struct VblankHandler sSoundVblankHandler;

// Only written to, never read.
static u8 sMusicVolume = 0;

static u8 sBgMusicDisabled = FALSE;
static u16 sCurrentMusic = MUSIC_NONE;
static u16 sCurrentShellMusic = MUSIC_NONE;
static u16 sCurrentCapMusic = MUSIC_NONE;
static u8 sPlayingInfiniteStairs = FALSE;
UNUSED static u8 unused8032C6D8[16] = { 0 };
static s16 sSoundMenuModeToSoundMode[] = { SOUND_MODE_STEREO, SOUND_MODE_MONO, SOUND_MODE_HEADSET };
// Only the 20th array element is used.
static u32 sMenuSoundsExtra[] = {
    SOUND_MOVING_TERRAIN_SLIDE + (0 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (1 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (2 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (3 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (4 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (5 << 16),
    SOUND_MOVING_TERRAIN_SLIDE + (6 << 16),
    SOUND_MOVING_LAVA_BURN,
    SOUND_MOVING_SLIDE_DOWN_TREE,
    SOUND_MOVING_SLIDE_DOWN_POLE,
    SOUND_MOVING_QUICKSAND_DEATH,
    SOUND_MOVING_TERRAIN_RIDING_SHELL,
    NO_SOUND,
    SOUND_ENV_BOAT_ROCKING1,
    SOUND_ENV_ELEVATOR3,
    SOUND_ENV_UNKNOWN2,
    SOUND_ENV_WATERFALL1,
    SOUND_ENV_WATERFALL2,
    SOUND_ENV_ELEVATOR1,
    SOUND_ENV_DRONING1,
    SOUND_ENV_DRONING2,
    SOUND_ENV_ELEVATOR2,
    SOUND_ENV_WIND1,
    SOUND_ENV_WATER,
    SOUND_AIR_BOWSER_SPIT_FIRE,
    SOUND_MOVING_AIM_CANNON,
    SOUND_AIR_LAKITU_FLY,
    SOUND_AIR_AMP_BUZZ,
    SOUND_AIR_CASTLE_OUTDOORS_AMBIENT,
    SOUND_OBJ2_BIRD_CHIRP1,
    SOUND_GENERAL2_BIRD_CHIRP2,
    SOUND_OBJ_BIRD_CHIRP3,
    SOUND_ENV_ELEVATOR2,
    SOUND_AIR_BLOW_WIND,
    SOUND_AIR_BLOW_FIRE,
    SOUND_ENV_ELEVATOR4,
};
static s8 sPaintingEjectSoundPlayed = FALSE;

void play_menu_sounds_extra(s32 a, void *b);

/**
 * Called from threads: thread5_game_loop
 */
void reset_volume(void) {
    sMusicVolume = 0;
}

/**
 * Called from threads: thread5_game_loop
 */
void lower_background_noise(s32 a) {
    switch (a) {
        case 1:
            set_audio_muted(TRUE);
            break;
        case 2:
            seq_player_lower_volume(SEQ_PLAYER_LEVEL, 60, 40);
            break;
    }
    sMusicVolume |= a;
}

/**
 * Called from threads: thread5_game_loop
 */
void raise_background_noise(s32 a) {
    switch (a) {
        case 1:
            set_audio_muted(FALSE);
            break;
        case 2:
            seq_player_unlower_volume(SEQ_PLAYER_LEVEL, 60);
            break;
    }
    sMusicVolume &= ~a;
}

/**
 * Called from threads: thread5_game_loop
 */
void disable_background_sound(void) {
    if (sBgMusicDisabled == FALSE) {
        sBgMusicDisabled = TRUE;
        sound_banks_disable(SEQ_PLAYER_SFX, SOUND_BANKS_BACKGROUND);
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void enable_background_sound(void) {
    if (sBgMusicDisabled == TRUE) {
        sBgMusicDisabled = FALSE;
        sound_banks_enable(SEQ_PLAYER_SFX, SOUND_BANKS_BACKGROUND);
    }
}

/**
 * Sets the sound mode
 *
 * Called from threads: thread5_game_loop
 */
void set_sound_mode(u16 soundMode) {
    if (soundMode < 3) {
        audio_set_sound_mode(sSoundMenuModeToSoundMode[soundMode]);
    }
}

/**
 * Wrapper method by menu used to set the sound via flags.
 *
 * Called from threads: thread5_game_loop
 */
void play_menu_sounds(s16 soundMenuFlags) {
    if (soundMenuFlags & SOUND_MENU_FLAG_HANDAPPEAR) {
        play_sound(SOUND_MENU_HAND_APPEAR, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_HANDISAPPEAR) {
        play_sound(SOUND_MENU_HAND_DISAPPEAR, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_UNKNOWN1) {
        play_sound(SOUND_MENU_UNK0C, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_PINCHMARIOFACE) {
        play_sound(SOUND_MENU_PINCH_MARIO_FACE, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_PINCHMARIOFACE2) {
        play_sound(SOUND_MENU_PINCH_MARIO_FACE, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_LETGOMARIOFACE) {
        play_sound(SOUND_MENU_LET_GO_MARIO_FACE, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_CAMERAZOOMIN) {
        play_sound(SOUND_MENU_CAMERA_ZOOM_IN, gGlobalSoundSource);
    } else if (soundMenuFlags & SOUND_MENU_FLAG_CAMERAZOOMOUT) {
        play_sound(SOUND_MENU_CAMERA_ZOOM_OUT, gGlobalSoundSource);
    }

    if (soundMenuFlags & 0x100) {
        play_menu_sounds_extra(20, NULL);
    }
#if ENABLE_RUMBLE
    if (soundMenuFlags & SOUND_MENU_FLAG_LETGOMARIOFACE) {
        queue_rumble_data(10, 60);
    }
#endif
}

/**
 * Plays the painting eject sound effect if it has not already been played
 *
 * Called from threads: thread5_game_loop
 */
void play_painting_eject_sound(void) {
    if (gRipplingPainting != NULL && gRipplingPainting->state == PAINTING_ENTERED) {
        // ripple when Mario enters painting
        if (!sPaintingEjectSoundPlayed) {
            play_sound(SOUND_GENERAL_PAINTING_EJECT,
                       gMarioStates[0].marioObj->header.gfx.cameraToObject);
        }
        sPaintingEjectSoundPlayed = TRUE;
    } else {
        sPaintingEjectSoundPlayed = FALSE;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void play_infinite_stairs_music(void) {
    u8 shouldPlay = FALSE;

    /* Infinite stairs? */
    if (gCurrLevelNum == LEVEL_CASTLE && gCurrAreaIndex == 2 && gMarioState->numStars < 70) {
        if (gMarioState->floor != NULL && gMarioState->floor->room == 6) {
            if (gMarioState->pos[2] < 2540.0f) {
                shouldPlay = TRUE;
            }
        }
    }

    if (sPlayingInfiniteStairs ^ shouldPlay) {
        sPlayingInfiniteStairs = shouldPlay;
        if (shouldPlay) {
            play_secondary_music(SEQ_EVENT_ENDLESS_STAIRS, 0, 255, 1000);
        } else {
            func_80321080(500);
        }
    }
}

/**
 * Called from threads: thread5_game_loop
 */
#ifdef TARGET_HANDHELD
/* The reset while the last scene's sound plays on (the user, 2026-09-29: "there's a pause between star
 * select and level starting, if it's to let 'let's-a-go' ring out can it happen during the transition
 * instead of forcing a wait?"). sound_reset held the whole game until the voice or the music's fade
 * ended (up to ~1.5 s of a frozen screen). Now the music change that resets the audio waits instead:
 * the level loads and its transition plays while the sound finishes, and the reset and the new music
 * come in the first frame nothing would be cut, 45 frames (1.5 s) at most. The sounds the new scene
 * starts meanwhile play in the old session; the reset clears them, as it always did. SM64_RESETDEFER=0:
 * the hold as before. */
extern int sound_reset_would_cut(int holdSfx);
extern int gSoundResetNoHold;
static int sDeferOn = -1, sDeferPending, sDeferFrames;
static u16 sDeferPreset, sDeferSeq;
static s16 sDeferFade;
static void background_music_now(u16 a, u16 seqArgs, s16 fadeTimer, int force);
int gHhLetsAGo = 0; /* star_select.c: an act was picked; its "let's-a-go" plays on */
static int defer_music(u16 a, u16 seqArgs, s16 fadeTimer) {
    if (sDeferOn < 0) { extern char *getenv(const char *); const char *e = getenv("SM64_RESETDEFER"); sDeferOn = !(e && e[0] == '0'); }
    const int letsago = gHhLetsAGo; /* read once, for this music change only */
    gHhLetsAGo = 0;
    if (!sDeferOn || !sound_reset_would_cut(1)) return 0;
    /* Leaving the act select with the same audio preset (both 0): nothing to rebuild, so no reset at
     * all. The level's music takes the level player at once and "let's-a-go" rings out on its own
     * channel (the user: with the wait "the music is delayed"). Other scene changes keep the reset. */
    {
        extern u8 D_80332108;
        if (letsago && gCurrCreditsEntry == NULL && (D_80332108 & 0xf) == (a < 8 ? a : 0)) {
            extern void port_log(const char *, ...);
            extern int port_music_queue_clear(void), port_music_level_seq(void);
            const int queued = port_music_queue_clear();
            if (!gNeverEnteredCastle || seqArgs != SEQ_LEVEL_INSIDE_CASTLE) play_music(SEQ_PLAYER_LEVEL, seqArgs, fadeTimer);
            { extern void port_sfx_restore_after_skipped_reset(void); port_sfx_restore_after_skipped_reset(); } /* external.c */
            port_log("audio reset: skipped (same preset %d) for a fade or a sound; music queue had %d, level player now seq %d (want %d)\n",
                     a, queued, port_music_level_seq(), seqArgs & 0xff);
            return 1;
        }
        /* A course with another preset (most of them: water, snow, lava, haunt, the Bowser stages) needs
         * the reset. Do it now with no hold: the music starts with the level (the user, 2026-09-30: "music
         * on time", picked over waiting for the voice to go quiet). Waiting made it 39 frames late; the
         * reset now cuts the last ~0.8 s of "let's-a-go", as the level appears. */
        static int sLetsAGoCut = -1; /* SM64_LETSAGOCUT=0: wait for the voice, as before */
        if (sLetsAGoCut < 0) { extern char *getenv(const char *); const char *e = getenv("SM64_LETSAGOCUT"); sLetsAGoCut = !(e && e[0] == '0'); }
        if (letsago && gCurrCreditsEntry == NULL && sLetsAGoCut) {
            extern void port_log(const char *, ...);
            port_log("audio reset: act select into preset %d, no wait\n", a);
            gSoundResetNoHold = 1;
            background_music_now(a, seqArgs, fadeTimer, 1);
            gSoundResetNoHold = 0;
            return 1;
        }
    }
    sDeferPending = 1; sDeferFrames = 0;
    sDeferPreset = a; sDeferSeq = seqArgs; sDeferFade = fadeTimer;
    return 1;
}
static void defer_tick(void) {
    if (!sDeferPending) return;
    if (sound_reset_would_cut(1) && ++sDeferFrames < 45) return;
    sDeferPending = 0;
    { extern void port_log(const char *, ...); port_log("audio reset: deferred %d frames for a fade or a sound\n", sDeferFrames); }
    gSoundResetNoHold = 1; /* waited enough: no hold of its own */
    background_music_now(sDeferPreset, sDeferSeq, sDeferFade, 1);
    gSoundResetNoHold = 0;
}
#endif
#ifdef TARGET_HANDHELD
static void set_background_music_inner(u16 a, u16 seqArgs, s16 fadeTimer);
void set_background_music(u16 a, u16 seqArgs, s16 fadeTimer) { /* STALLWHY: music changes (hh/port.h) */
    const u32 t = port_time_us();
    set_background_music_inner(a, seqArgs, fadeTimer);
    PORT_WHY_ADD(WHY_MUSIC, port_time_us() - t);
}
static void set_background_music_inner(u16 a, u16 seqArgs, s16 fadeTimer) {
    if (gResetTimer == 0 && seqArgs != sCurrentMusic) {
        if (sDeferPending) { sDeferPreset = a; sDeferSeq = seqArgs; sDeferFade = fadeTimer; sCurrentMusic = seqArgs; return; }
        if (defer_music(a, seqArgs, fadeTimer)) { sCurrentMusic = seqArgs; return; }
    }
    background_music_now(a, seqArgs, fadeTimer, 0);
}
static void background_music_now(u16 a, u16 seqArgs, s16 fadeTimer, int force) {
    if (gResetTimer == 0 && (force || seqArgs != sCurrentMusic)) {
#else
    if (gResetTimer == 0 && seqArgs != sCurrentMusic) {
#endif
        if (gCurrCreditsEntry != NULL) {
            sound_reset(7);
        } else {
            sound_reset(a);
        }

        if (!gNeverEnteredCastle || seqArgs != SEQ_LEVEL_INSIDE_CASTLE) {
            play_music(SEQ_PLAYER_LEVEL, seqArgs, fadeTimer);
            sCurrentMusic = seqArgs;
        }
    }
}

/**
 * Called from threads: thread3_main, thread5_game_loop
 */
void fadeout_music(s16 fadeOutTime) {
    func_803210D4(fadeOutTime);
    sCurrentMusic = MUSIC_NONE;
    sCurrentShellMusic = MUSIC_NONE;
    sCurrentCapMusic = MUSIC_NONE;
}

/**
 * Called from threads: thread5_game_loop
 */
void fadeout_level_music(s16 fadeTimer) {
    seq_player_fade_out(SEQ_PLAYER_LEVEL, fadeTimer);
    sCurrentMusic = MUSIC_NONE;
    sCurrentShellMusic = MUSIC_NONE;
    sCurrentCapMusic = MUSIC_NONE;
}

/**
 * Called from threads: thread5_game_loop
 */
void play_cutscene_music(u16 seqArgs) {
    play_music(SEQ_PLAYER_LEVEL, seqArgs, 0);
    sCurrentMusic = seqArgs;
}

/**
 * Called from threads: thread5_game_loop
 */
void play_shell_music(void) {
    play_music(SEQ_PLAYER_LEVEL, SEQUENCE_ARGS(4, SEQ_EVENT_POWERUP | SEQ_VARIATION), 0);
    sCurrentShellMusic = SEQUENCE_ARGS(4, SEQ_EVENT_POWERUP | SEQ_VARIATION);
}

/**
 * Called from threads: thread5_game_loop
 */
void stop_shell_music(void) {
    if (sCurrentShellMusic != MUSIC_NONE) {
        stop_background_music(sCurrentShellMusic);
        sCurrentShellMusic = MUSIC_NONE;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void play_cap_music(u16 seqArgs) {
    play_music(SEQ_PLAYER_LEVEL, seqArgs, 0);
    if (sCurrentCapMusic != MUSIC_NONE && sCurrentCapMusic != seqArgs) {
        stop_background_music(sCurrentCapMusic);
    }
    sCurrentCapMusic = seqArgs;
}

/**
 * Called from threads: thread5_game_loop
 */
void fadeout_cap_music(void) {
    if (sCurrentCapMusic != MUSIC_NONE) {
        fadeout_background_music(sCurrentCapMusic, 600);
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void stop_cap_music(void) {
    if (sCurrentCapMusic != MUSIC_NONE) {
        stop_background_music(sCurrentCapMusic);
        sCurrentCapMusic = MUSIC_NONE;
    }
}

/**
 * Called from threads: thread5_game_loop
 */
void play_menu_sounds_extra(s32 a, void *b) {
    play_sound(sMenuSoundsExtra[a], b);
}

/**
 * Called from threads: thread5_game_loop
 */
void audio_game_loop_tick(void) {
#ifdef TARGET_HANDHELD
    defer_tick();
#endif
    audio_signal_game_loop_tick();
}

/**
 * Sound processing thread. Runs at 60 FPS.
 */
void thread4_sound(UNUSED void *arg) {
    audio_init();
    sound_init();

    // Zero-out unused vector
    vec3f_copy(unused80339DC0, gVec3fZero);

    osCreateMesgQueue(&sSoundMesgQueue, sSoundMesgBuf, ARRAY_COUNT(sSoundMesgBuf));
    set_vblank_handler(1, &sSoundVblankHandler, &sSoundMesgQueue, (OSMesg) 512);

    while (TRUE) {
        OSMesg msg;

        osRecvMesg(&sSoundMesgQueue, &msg, OS_MESG_BLOCK);
        if (gResetTimer < 25) {
            struct SPTask *spTask;
            profiler_log_thread4_time();
            spTask = create_next_audio_frame_task(); 
            if (spTask != NULL) {
                dispatch_audio_sptask(spTask);
            }
            profiler_log_thread4_time();
        }
    }
}
