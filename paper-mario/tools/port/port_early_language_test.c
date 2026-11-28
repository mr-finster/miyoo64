/* Actual host language resolver and catalog, with synthetic boot/flash state. */
#define MIYOO 1
#define PM64 1
#define PM64_ROM_REGION_PAL 1
#define main unused_port_main
#include "../../game/port/port_host.c"
#undef main
#include "../../kit/platform/miyoo_i18n.c"
#include <assert.h>
static int mode, pending, flash=1;
int gCurrentLanguage;
int16_t get_game_mode(void) { return mode; }
int port_flash_language(void) { return flash; }
int savestate_boot_load_pending(void) { return pending; }
const char *miyoo_data_dir(void) { return "/tmp"; }
static void raw(const char *v) { FILE *f=fopen(port_save_path("menu-last-language.cfg"),"wb"); assert(f); assert(fwrite(v,1,strlen(v),f)==strlen(v)); assert(!fclose(f)); sEarlyLanguage=-2; }
int main(void) {
    char dir[]="/tmp/pm64-d7-language-XXXXXX"; assert(mkdtemp(dir)); assert(!setenv("PM64_DATA",dir,1));
    mode=0; gCurrentLanguage=0; flash=1;
    assert(port_pal_language()==1); assert(miyoo_i18n_language()==1);
    raw("BAD\n"); assert(port_pal_language()==1);
    raw("FR\nJUNK"); assert(port_pal_language()==1);
    raw("FR\n"); assert(port_pal_language()==2 && gCurrentLanguage==0);
    { FILE *f=fopen(port_save_path("menu-language.cfg"),"w"); assert(f); assert(fputs("INVALID\n",f)>=0); assert(!fclose(f)); }
    loaded=0; manual=MIYOO_AUTO; assert(miyoo_i18n_language()==MIYOO_FR);
    for (int manual=0;manual<4;manual++) { miyoo_i18n_choose(manual); assert(miyoo_i18n_language()==manual); }
    miyoo_i18n_choose(MIYOO_AUTO); assert(miyoo_i18n_language()==MIYOO_FR);
    mode=1; pending=1; gCurrentLanguage=MIYOO_EN; port_pal_language_remember();
    mode=0; assert(port_pal_language()==MIYOO_FR); /* pending resume never replaced hint */
    pending=0; mode=1;
    for (int lang=0;lang<4;lang++) {
        gCurrentLanguage=lang; assert(miyoo_i18n_language()==lang); port_pal_language_remember();
        mode=0; sEarlyLanguage=-2; assert(port_pal_language()==lang); mode=1;
    }
    gCurrentLanguage=MIYOO_DE; port_pal_language_remember();
    gCurrentLanguage=MIYOO_FR; port_pal_language_remember();
    mode=0; sEarlyLanguage=-2; assert(miyoo_i18n_language()==MIYOO_FR);
    mode=1; gCurrentLanguage=MIYOO_DE; assert(miyoo_i18n_language()==MIYOO_DE); /* loaded state */
    port_pal_language_remember(); mode=0; sEarlyLanguage=-2; assert(miyoo_i18n_language()==MIYOO_DE);
    flash=-1; assert(!unlink(port_save_path("menu-last-language.cfg"))); sEarlyLanguage=-2; gCurrentLanguage=0; assert(port_pal_language()==0);
    assert(!unlink(port_save_path("menu-language.cfg"))); assert(!rmdir(dir));
    puts("D7 early cache, AUTO/manual, four languages and pending/loaded state PASS"); return 0;
}
