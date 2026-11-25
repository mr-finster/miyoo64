/* Small host check for UTF-8 decoding, glyph coverage, width and catalog fallbacks. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef FLASH_INTEGRATION_TEST
#include <stdlib.h>
#include <sys/wait.h>
#include "../../game/port/port.h"
#endif
#include "../../game/port/port_flash_language.h"
#include "../../kit/platform/miyoo_text_layout.h"
#include "../../kit/platform/miyoo_i18n.c"
#ifdef PORT_HOST_LANGUAGE_TEST
int gCurrentLanguage;
static int16_t game_mode;
int16_t get_game_mode(void) { return game_mode; }
#define game_language gCurrentLanguage
#else
static int game_language=0;
#endif
#ifdef FLASH_INTEGRATION_TEST
static int use_flash;
void port_log(const char *fmt, ...) { (void)fmt; }
uint32_t port_time_us(void) { return 0; }
#endif
#ifndef PORT_HOST_LANGUAGE_TEST
int port_pal_language(void) {
#ifdef FLASH_INTEGRATION_TEST
    if (use_flash) { int l=port_flash_language(); return l >= 0 ? l : 0; }
#endif
    return game_language;
}
#endif
const char *port_save_path(const char *name) { static char p[128]; snprintf(p,sizeof p,"/tmp/miyoo-i18n-test/%s",name); return p; }
static void make_globals(uint8_t raw[0x80], uint32_t language, int valid) {
    int32_t sum=0,crc;
    memset(raw,0,sizeof(uint8_t)*0x80); memcpy(raw,"Mario Story 006",16); memcpy(raw+0x40,&language,4);
    for (unsigned i=0;i<0x80;i+=4) { if(i==0x30||i==0x34)continue; int32_t w; memcpy(&w,raw+i,4); sum+=w; }
    int32_t initial_inverse=-1; sum+=initial_inverse;
    crc=sum; memcpy(raw+0x30,&crc,4); int32_t inverse=~crc; memcpy(raw+0x34,&inverse,4);
    if (!valid) { inverse ^= 1; memcpy(raw+0x34,&inverse,4); }
}
#ifdef FLASH_INTEGRATION_TEST
/* Each child gets a fresh flash cache. Images are temporary copies: port_flash_open
 * may remove a sibling .tmp, so it must never be pointed at evidence files. */
static void check_image(const uint8_t *image, int preference, int flash, int expected) {
    char path[]="/tmp/pm64-flash-test-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0);
    FILE *f=fdopen(fd,"wb"); assert(f);
    assert(fwrite(image,1,0x20000,f)==0x20000); assert(fclose(f)==0);
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) {
        assert(setenv("PM64_SAVE",path,1)==0);
        use_flash=1; loaded=1; manual=preference; preview=MIYOO_MANUAL_AUTO;
#ifdef PORT_HOST_LANGUAGE_TEST
        game_mode=0; game_language=MIYOO_EN;
#endif
        assert(port_flash_language()==flash);
        assert(miyoo_i18n_language()==expected);
        if (expected==MIYOO_DE) {
            assert(!strcmp(miyoo_i18n(MT_RESUME_GAME),"SPIEL FORTSETZEN"));
            assert(!strcmp(miyoo_i18n(MT_NEW_GAME),"NEUES SPIEL"));
        }
#ifdef PORT_HOST_LANGUAGE_TEST
        /* Simulate startup completion and restoring states with different live
         * languages while the flash language remains unchanged. */
        game_mode=1;
        for (int live=0;live<4;live++) {
            game_language=live;
            assert(miyoo_i18n_language()==(preference==MIYOO_AUTO ? live : preference));
        }
        game_mode=0; game_language=MIYOO_EN;
        assert(miyoo_i18n_language()==expected);
#endif
        _exit(0);
    }
    int status; assert(waitpid(pid,&status,0)==pid);
    assert(unlink(path)==0);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void check_flash_images(int argc, char **argv) {
    uint8_t *image=malloc(0x20000); assert(image);
    for (int lang=0;lang<4;lang++) {
        memset(image,0xff,0x20000);
        /* Deliberately misleading D4 positions must never determine language. */
        make_globals(image+0x300,(lang+1)%4,1);
        make_globals(image+0x18000,lang,1);
        check_image(image,MIYOO_AUTO,lang,lang);
    }
    memset(image,0xff,0x20000);
    make_globals(image+0x300,MIYOO_DE,1);
    check_image(image,MIYOO_AUTO,-1,MIYOO_EN);
    make_globals(image+0x18000,MIYOO_DE,0);
    check_image(image,MIYOO_AUTO,-1,MIYOO_EN);
    make_globals(image+0x1c000,MIYOO_FR,1);
    check_image(image,MIYOO_AUTO,MIYOO_FR,MIYOO_FR);
    make_globals(image+0x18000,MIYOO_DE,1);
    check_image(image,MIYOO_AUTO,MIYOO_DE,MIYOO_DE);
    check_image(image,MIYOO_EN,MIYOO_DE,MIYOO_EN);
    for (int i=1;i<argc;i++) {
        FILE *f=fopen(argv[i],"rb"); assert(f);
        assert(fread(image,1,0x20000,f)==0x20000 && fgetc(f)==EOF);
        assert(!ferror(f) && fclose(f)==0);
        check_image(image,MIYOO_AUTO,MIYOO_DE,MIYOO_DE);
        printf("snapshot %s: AUTO DE PASS\n",argv[i]);
    }
    free(image);
    puts("flash offsets, priority, fallback and manual preference PASS");
}
#endif
int main(int argc, char **argv) {
#ifndef FLASH_INTEGRATION_TEST
    (void)argc; (void)argv;
#endif
    const char *p="ÄöÜß éèê ç ñ ¡¿"; unsigned n=0; uint8_t g[8];
    while (*p) { unsigned cp=miyoo_utf8_next(&p); miyoo_glyph(cp,g); n++; }
    assert(n==15 && miyoo_utf8_width("éñ¿",2)==48);
    /* The localized action word and Spanish acute-A each decode as one glyph;
     * Ü and Á must differ from their unaccented base glyphs without losing it. */
    p="BESTÄTIGEN"; n=0; while (*p) { unsigned cp=miyoo_utf8_next(&p); miyoo_glyph(cp,g); n++; }
    assert(n==10 && miyoo_utf8_width("BESTÄTIGEN",3)==240);
    assert(miyoo_menu_label_scale(600,0,364)==1);
    assert(miyoo_menu_label_scale(600,1,364)==3);
    assert(miyoo_menu_label_scrolls(600,364) && !miyoo_menu_label_scrolls(300,364));
    { uint8_t base[8], accented[8]; miyoo_glyph('U',base); miyoo_glyph(0x00DC,accented);
      assert(memcmp(base,accented,8)!=0 && accented[0]==0x24 && accented[1]==0x18);
      assert(memcmp(base+2,accented+2,6)==0 && accented[7]==0); }
    { uint8_t base[8], accented[8]; miyoo_glyph('A',base); miyoo_glyph(0x00C1,accented);
      assert(memcmp(base,accented,8)!=0 && memcmp(base+2,accented+2,6)==0); }
    { uint8_t globals[0x80];
      make_globals(globals,3,1); assert(port_flash_globals_language(globals)==3);
      make_globals(globals,2,0); assert(port_flash_globals_language(globals)==-1);
      make_globals(globals,4,1); assert(port_flash_globals_language(globals)==-1);
      memset(globals,0xFF,sizeof globals); assert(port_flash_globals_language(globals)==-1); }
    mkdir("/tmp/miyoo-i18n-test",0755);
#ifdef PORT_HOST_LANGUAGE_TEST
    game_mode=1;
#endif
    game_language=2; miyoo_i18n_choose(MIYOO_AUTO); assert(miyoo_i18n_language()==2);
    game_language=99; assert(miyoo_i18n_language()==MIYOO_EN);
    miyoo_i18n_choose(MIYOO_ES); assert(!strcmp(miyoo_i18n(MT_LANGUAGE),"IDIOMA"));
    loaded=0; manual=MIYOO_AUTO; assert(miyoo_i18n_manual()==MIYOO_ES);
    miyoo_i18n_choose(MIYOO_AUTO); loaded=0; manual=MIYOO_EN; assert(miyoo_i18n_manual()==MIYOO_AUTO);
    { FILE *f=fopen("/tmp/miyoo-i18n-test/menu-language.cfg","w"); assert(f); fputs("BAD\n",f); fclose(f); loaded=0; manual=MIYOO_AUTO; assert(miyoo_i18n_manual()==MIYOO_AUTO); }
    puts("i18n UTF-8/glyph/width/AUTO fallback/catalog PASS");
#ifdef FLASH_INTEGRATION_TEST
    check_flash_images(argc,argv);
#endif
    return 0;
}
