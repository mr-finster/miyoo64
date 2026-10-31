/* hh_plat.h: the platform side (display, audio output, input, files) as seen by the
 * port layer. hh_os.h has the kernel hooks (hh_plat_rsp_task, hh_plat_vi). */
#ifndef HH_PLAT_H
#define HH_PLAT_H

#if defined(TARGET_MIYOO)
/* data/ next to the executable (App/BanjoKazooie/data: every launcher in the Miyoo release hands
 * over to App/BanjoKazooie/launch.sh, which sets BK_DATA) -- notes/miyoo.md */
const char *hh_miyoo_home(void);
#define HH_CARD_ROOT "/mnt/SDCARD"
#define HH_DEFAULT_ROM "data/banjo.z64" /* unused: hh_card_setup always names the ROM */
#define HH_DEFAULT_SAVE_DIR hh_miyoo_home()
#else
/* the desktop proxy runs in the build container, repo root at /work (dev.sh) */
#define HH_DEFAULT_ROM "baserom.us.v10.z64"
#define HH_DEFAULT_SAVE_DIR "bk_save"
#endif

void hh_plat_init(void);
const char *hh_plat_save_dir(void);

extern unsigned int hh_frames;
extern unsigned int hh_vicount; /* the last VI tick (BK_EXIT_AT, BK_PADSCRIPT) */

/* strerror(errno): for files that cannot include <errno.h> (its errno macro breaks
 * libultra's OSContPad.errno member). */
const char *hh_last_error(void);     /* gfx tasks presented */

#endif
