/* miyoo_options.h: MENU > OPTIONS, a list of the game's settings (miyoo_options.c; docs/OPTIONS.md).
 *
 * Each setting is a key=value line of the game's debug.cfg, which the game reads at start as it always has.
 * The page shows the game's table of rows (miyoo_options[], one per-game file: hh_options.c), lets the player
 * change them, and on leaving writes the changed keys back into debug.cfg (the other lines and comments kept,
 * a temp file renamed over it). A row either applies at once (its apply() is called with the new value, its
 * variable set) or needs a restart (the page marks it). Saves and save states are never touched.
 *
 * The canonical copy lives in kit-options/ at the repository root; each port that has the page vendors an
 * identical copy next to its miyoo_menu.c (kit-options/sync.sh copies and checks them). */
#ifndef MIYOO_OPTIONS_H
#define MIYOO_OPTIONS_H

/* One choice of a row: the value written to debug.cfg ("1") and the name the page shows ("ON"). */
struct miyoo_option_value {
    const char *value;
    const char *name;
};

enum { MIYOO_OPT_RESTART, MIYOO_OPT_NOW };

/* One row. key NULL: a section header (label only). env: the variable the game reads the setting from; NULL =
 * the game's prefix and the key in capitals (hh_game.h HH_ENV: fineaim -> OOT_FINEAIM). def: the value the
 * game uses without a line (one of values[].value). help: one line for the bottom of the page (or NULL).
 * when: MIYOO_OPT_NOW (apply, if any, is called on each change, after the variable is set) or
 * MIYOO_OPT_RESTART (the game reads it at start only). */
struct miyoo_option {
    const char *label;
    const char *key;
    const char *env;
    const struct miyoo_option_value *values;
    int nvalues;
    const char *def;
    const char *help;
    int when;
    void (*apply)(const char *value);
};

/* A row's choices: .values and .nvalues from a list of { value, name } pairs. */
#define MIYOO_OPT_VALUES(...)                                                                                     \
    .values = (const struct miyoo_option_value[]){ __VA_ARGS__ },                                               \
    .nvalues = (int)(sizeof((const struct miyoo_option_value[]){ __VA_ARGS__ }) / sizeof(struct miyoo_option_value))
#define MIYOO_OPT_ONOFF MIYOO_OPT_VALUES({ "1", "ON" }, { "0", "OFF" })

/* The game's table (its hh_options.c). */
extern const struct miyoo_option miyoo_options[];
extern const int miyoo_options_count;

/* Optional, in the game's file: the folder that holds debug.cfg. Without it: port_save_dir(), else
 * miyoo_data_dir(). */
extern const char *miyoo_options_dir(void) __attribute__((weak));

/* For a game whose start does not read debug.cfg already: each table row's line, as its variable (setenv,
 * the file winning over the environment, as Ocarina of Time's loader). Call it first thing in main().
 * Returns the number of variables set. */
int miyoo_options_load(void);

/* debug.cfg's text with the n keys set to the values: each line "key=..." of those keys rewritten, a key with
 * no line appended, every other line (comments, other keys, blank lines, CR LF endings) kept as it was.
 * Returns a malloc'd string (the caller frees it), NULL if out of memory. */
char *miyoo_options_rewrite(const char *text, const char *const *keys, const char *const *values, int n);

/* The same, on the file at path, atomically (path.new, then renamed over it). 0 written or nothing to
 * change, -1 an error (the file is left as it was). */
int miyoo_options_write_file(const char *path, const char *const *keys, const char *const *values, int n);

struct SDL_Surface;
/* The OPTIONS page, modal, as miyoo_remap_screen: draw is the menu's compose surface (640x480 or the panel's
 * size), backdrop the dimmed frame of the same size, show() puts draw on the panel, key(sym, 0) lets go of a
 * key in the game (may be NULL), should_close() ends the page at once (power off, sleep, SIGTERM); the
 * changes are written either way. */
void miyoo_options_screen(struct SDL_Surface *draw, struct SDL_Surface *backdrop, void (*show)(void),
                          void (*key)(int sym, int down), int (*should_close)(void));

/* Headless check (<GAME>_MENU_TEST): the page's states to <dir>/options_*.ppm (nothing is changed). */
void miyoo_options_test_dump(struct SDL_Surface *draw, struct SDL_Surface *backdrop, const char *dir);

#endif
