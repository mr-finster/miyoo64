/* vanilla_hooks.c -- the fork's PC-enhancement and mod hooks, answered with
 * the original game's behaviour. BattleShip wires these to its menu and mod
 * system; this port plays the N64 game as shipped. The controls layer may
 * later drive the input hooks (see notes/controls.md). */
#include <stdbool.h>
#include <ssb_types.h>

int port_cheat_unlock_all(void) { return 0; }
int port_cheat_unlock_captain(void) { return 0; }
int port_cheat_unlock_inishie(void) { return 0; }
int port_cheat_unlock_itemswitch(void) { return 0; }
int port_cheat_unlock_luigi(void) { return 0; }
int port_cheat_unlock_ness(void) { return 0; }
int port_cheat_unlock_purin(void) { return 0; }
int port_cheat_unlock_soundtest(void) { return 0; }

int port_classic_coop_context(void) { return 0; }
int port_classic_coop_friendly_fire(void) { return 0; }
void port_classic_coop_set_context(int active) { (void)active; }
int port_enhancement_classic_coop(void) { return 0; }

int port_enhancement_IsAutoZCancelEnabled(void) { return 0; }
int port_enhancement_IsFailedZCancelFlashEnabled(void) { return 0; }
int port_enhancement_bonus_stages_enabled(void) { return 0; }
int port_enhancement_boot_to_vs_css(void) { return 0; }
int port_enhancement_cpu_level_9(void) { return 0; }
int port_enhancement_neutral_spawns(void) { return 0; }
int port_enhancement_skip_results_screen(void) { return 0; }
int port_enhancement_tap_jump_disabled(int player_index) { (void)player_index; return 0; }
int port_enhancement_hitbox_display_override(int current_mode) { return current_mode; }
bool port_enhancement_is_hud_disabled(void) { return false; }
int port_get_comp_ruleset(void) { return 0; }

void port_enhancement_analog_remap(int player_index, signed char *stick_x, signed char *stick_y)
{ (void)player_index; (void)stick_x; (void)stick_y; }
void port_enhancement_c_stick_smash(int player_index, unsigned short *button_hold, unsigned short *button_tap,
                                    signed char *stick_x, signed char *stick_y, unsigned short tap_pre_remap)
{ (void)player_index; (void)button_hold; (void)button_tap; (void)stick_x; (void)stick_y; (void)tap_pre_remap; }
void port_enhancement_dpad_jump(int player_index, unsigned short *button_hold, unsigned short *button_tap,
                                unsigned short tap_pre_remap)
{ (void)player_index; (void)button_hold; (void)button_tap; (void)tap_pre_remap; }

/* 0 = the original A/START handling on the stage-select screen. */
s32 port_enhancement_music_select_handle_a(u32 bgm_id) { (void)bgm_id; return 0; }
s32 port_enhancement_music_select_handle_b(void) { return 0; }
void port_enhancement_music_select_reset(void) {}
u32 port_enhancement_shuffle_music(u32 requested_bgm) { return requested_bgm; }

float port_get_master_volume(void) { return 1.0f; }
float port_get_music_volume(void) { return 1.0f; }
float port_get_sfx_volume(void) { return 1.0f; }
float port_get_voice_volume(void) { return 1.0f; }
float port_widescreen_clip_x_scale(void) { return 1.0f; }
