/* Shared, small UTF-8 catalog for the Miyoo menu and Controls screen. */
#ifndef MIYOO_I18N_H
#define MIYOO_I18N_H
#include <stdint.h>

enum { MIYOO_AUTO = -1, MIYOO_EN, MIYOO_DE, MIYOO_FR, MIYOO_ES };
#define MIYOO_MANUAL_AUTO (-2)
enum miyoo_text_id {
    MT_RESUME, MT_SAVE, MT_LOAD, MT_CONTROLS, MT_LANGUAGE, MT_EXIT,
    MT_AUTO, MT_ENGLISH, MT_GERMAN, MT_FRENCH, MT_SPANISH,
    MT_SLOT, MT_AUTO_SAVE, MT_SURE, MT_USED, MT_FREE, MT_BACK_HINT,
    MT_RESUME_GAME, MT_NEW_GAME, MT_CONTROLS_TITLE, MT_N64, MT_MIYOO,
    MT_PRESS, MT_RESET_DEFAULT, MT_PRESS_NEW, MT_MENU_CANCEL, MT_SWAP_WITH,
    MT_TAKE_FROM, MT_YES_NO, MT_RESET_ALL, MT_FIXED_BACK, MT_CHANGE_BACK,
    MT_SAVED_SLOT, MT_LOADED_SLOT, MT_SAVE_FAILED, MT_LOAD_FAILED,
    MT_GAME_SAVE_FAILED, MT_RESET_HINT, MT_SAVING, MT_LOADED_AUTO,
    MT_RESET_GAME, MT_RESET_WARNING, MT_COUNT
};
int miyoo_i18n_language(void);
int miyoo_i18n_manual(void);
void miyoo_i18n_choose(int language);
void miyoo_i18n_preview(int language);
const char *miyoo_i18n(enum miyoo_text_id id);
/* Returns one decoded codepoint and advances *p. Invalid UTF-8 maps to '?'. */
unsigned miyoo_utf8_next(const char **p);
int miyoo_utf8_width(const char *s, int scale);
void miyoo_glyph(unsigned codepoint, uint8_t rows[8]);
#endif
