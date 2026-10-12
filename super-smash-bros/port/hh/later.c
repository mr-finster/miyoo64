/* later.c -- fork hooks whose features come in later milestones. Each one
 * returns the value that makes the game fall back to its own ROM path. */
#include <stddef.h>

typedef struct Sprite Sprite;

/* BattleShip's PNG stage-select art; NULL = the ROM sprites (mnmaps.c). */
Sprite *portCSSGetStageIconSprite(int gkind) { (void)gkind; return NULL; }
Sprite *portCSSGetStageBackgroundSprite(int gkind) { (void)gkind; return NULL; }
Sprite *portCSSGetStageNameSprite(int gkind) { (void)gkind; return NULL; }
Sprite *portCSSGetStageEmblemSprite(int gkind) { (void)gkind; return NULL; }
Sprite *portCSSGetScrollArrowSprite(void) { return NULL; }
Sprite *portCSSGetScrollArrowLeftSprite(void) { return NULL; }

/* BattleShip's GPU snapshot bridge for the framebuffer photos (1P stage-clear wallpaper, results photo
 * wipe): unused here. The handheld build copies the software renderer's last frame instead
 * (gfx_pc.c port_capture_fb_rgba16, called from sc1pstageclear.c and lbtransition.c); these keep the
 * bridge's other entry points (lbreloc_bridge.c releases at each scene change) as no-ops. */
int port_capture_register_fb_for_subrect(const void *base, unsigned int size_bytes,
                                         float u0, float v0, float u1, float v1)
{
    (void)base; (void)size_bytes; (void)u0; (void)v0; (void)u1; (void)v1;
    return 0;
}
void port_capture_release_range(const void *base) { (void)base; }
void port_capture_release_all(void) {}
