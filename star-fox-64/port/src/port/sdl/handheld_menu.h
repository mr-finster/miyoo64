/* The in-game menu (MENU button; miyoo_menu.c): resume, save / load state, exit; the
 * resume prompt and the notices. No-op stubs off the device. */
#ifndef HANDHELD_MENU_H
#define HANDHELD_MENU_H

/* Display modes -- see gfx_sdl12.c. */
#define HH_DISPLAY_CONVERT 0 /* render at the output size (default) */
#define HH_DISPLAY_STRETCH 1 /* internal 320x240, squashed to the square size */
#define HH_DISPLAY_CROP    2 /* internal 320x240, the centred square 1:1 */

#if defined(HANDHELD)
struct SDL_Surface;
void handheld_menu_init(struct SDL_Surface *screen); /* loads saved settings */
void handheld_menu_run(struct SDL_Surface *screen);  /* modal until resume/exit */
void handheld_menu_notify(const char *msg);          /* a notice in the corner for two seconds */
void handheld_menu_notify_wait(const char *msg);     /* the same, returns once it is up (quitting) */
void handheld_menu_test_dump(const char *dir);       /* headless: render zones to PPM */
int  handheld_menu_resume_prompt(void);              /* launch: 1 = resume, 0 = new, -1 = the OS ended the game */
int  handheld_power_pending(void);     /* SIGTERM seen: the menu and the resume prompt step aside */
#else
#define handheld_menu_init(s)   ((void)0)
#define handheld_menu_run(s)    ((void)0)
#define handheld_menu_notify(m) ((void)0)
#define handheld_menu_notify_wait(m) ((void)0)
#define handheld_power_pending()     0
#endif

#endif
