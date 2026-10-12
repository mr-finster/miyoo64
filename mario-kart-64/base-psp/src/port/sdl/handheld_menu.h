/* The in-game menu, resume prompt and notices (miyoo_menu.c on the Miyoo Mini); no-op stubs
 * elsewhere. */
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
void handheld_menu_notify(const char *msg);          /* the handheld OS notification bar */
void handheld_menu_notify_wait(const char *msg);     /* the same, returns once it is up (quitting) */
void handheld_menu_test_dump(const char *dir);       /* headless: render zones to PPM */
int  handheld_menu_resume_prompt(void);              /* launch: 1 = resume, 0 = new, -1 = power-off */
/* gfx_sdl12.c: the OS's power-off signal (SIGTERM). */
int  handheld_power_pending(void);     /* power-off seen, not yet taken by the game loop */
#else
#define handheld_menu_init(s)   ((void)0)
#define handheld_menu_run(s)    ((void)0)
#define handheld_menu_notify(m) ((void)0)
#define handheld_menu_notify_wait(m) ((void)0)
#define handheld_power_pending()     0
#endif

#endif
