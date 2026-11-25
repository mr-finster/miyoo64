/* Pixel-level text-lane and localization checks without an SDL dependency. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../kit/platform/miyoo_font.h"
#include "../../kit/platform/miyoo_i18n.h"
#include "../../kit/platform/miyoo_text_layout.h"

static uint8_t pixels[480][640];
static void draw_clipped(const char *s, int scale, int x, int y, int clip_l, int clip_r) {
    while (*s) {
        uint8_t g[8]; unsigned cp=miyoo_utf8_next(&s); miyoo_glyph(cp,g);
        for (int gy=0;gy<8;gy++) for(int gx=0;gx<8;gx++) if(g[gy]&(1u<<gx))
            for(int dy=0;dy<scale;dy++) for(int dx=0;dx<scale;dx++) {
                int px=x+gx*scale+dx,py=y+gy*scale+dy;
                if(px>=clip_l&&px<clip_r&&px>=0&&px<640&&py>=0&&py<480)pixels[py][px]=1;
            }
        x+=8*scale;
    }
}
static void check_bounds(int y0,int y1,int left,int right) {
    for(int y=y0;y<y1;y++)for(int x=0;x<640;x++)if(pixels[y][x])assert(x>=left&&x<right);
}
static void check_catalog(const char *label) {
    const int left=138,right=502,lane=right-left;
    const int width=miyoo_utf8_width(label,3);
    int scale=miyoo_menu_label_scale(width,0,lane);
    assert(scale>=1&&scale<=3);
    assert(miyoo_utf8_width(label,scale)<=lane);
    memset(pixels,0,sizeof pixels);
    draw_clipped(label,scale,(640-miyoo_utf8_width(label,scale))/2,200,left,right);
    check_bounds(200,200+8*scale,left,right);
    assert(miyoo_menu_label_scrolls(width,lane)==(width>lane));
    /* Selected long labels retain scale 3 and are clipped to the marquee lane. */
    memset(pixels,0,sizeof pixels);
    if(width>lane)draw_clipped(label,3,left-(width-lane),200,left,right);
    else draw_clipped(label,3,(640-width)/2,200,left,right);
    check_bounds(200,224,left,right);
}
int main(void) {
    static const char *labels[4][7]={
      {"RESUME","SAVE STATE","LOAD STATE","CONTROLS","LANGUAGE","RESET GAME","EXIT GAME"},
      {"FORTSETZEN","SPIELSTAND SPEICHERN","SPIELSTAND LADEN","STEUERUNG","SPRACHE","SPIEL ZURÜCKSETZEN","SPIEL BEENDEN"},
      {"REPRENDRE","SAUVER L'ÉTAT","CHARGER L'ÉTAT","COMMANDES","LANGUE","RÉINITIALISER LE JEU","QUITTER"},
      {"CONTINUAR","GUARDAR ESTADO","CARGAR ESTADO","CONTROLES","IDIOMA","REINICIAR JUEGO","SALIR DEL JUEGO"}
    };
    for(unsigned lang=0;lang<4;lang++)for(unsigned i=0;i<7;i++)check_catalog(labels[lang][i]);
    puts("menu text layout pixels EN/DE/FR/ES lane bounds PASS");
    return 0;
}
