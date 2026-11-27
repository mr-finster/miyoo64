/* Exercise the public menu's actual event loop with a dummy SDL display. */
#define PM64_PUBLIC_RELEASE 1
#define PM64 1
#define PM64_ROM_REGION_PAL 1
enum { MT_DIAGNOSTIC = 1000 };
#include "miyoo_menu.c"
#include "miyoo_i18n.c"
#include <assert.h>
static int saved,loads,controls,exited,muted,valid,resets,cancel_boot;
int gfx_soft_bilinear, gfx_soft_magbilin;
void gfx_sdl12_set_display_mode(int v){(void)v;}
void gfx_sdl12_set_res_dynamic(int v){(void)v;}
void gfx_sdl12_set_res_pct(int v){(void)v;}
void port_request_exit(void){}
void port_audio_out_set_muted(int v){muted=v;}
uint32_t *gfx_soft_get_framebuffer(int *w,int *h){*w=0;*h=0;return NULL;}
int miyoo_panel_backdrop_size(int *w,int *h){(void)w;(void)h;return 0;}
int miyoo_panel_backdrop(uint32_t *p,int w,int h,int pitch){(void)p;(void)w;(void)h;(void)pitch;return 0;}
void miyoo_panel_show_full(SDL_Surface *s){(void)s;}
int handheld_power_pending(void){return 0;}
int miyoo_sleepsave_menu_should_close(void){return 0;}
int savestate_slot_valid(int slot){return slot==1 && valid;}
void savestate_request_save(int slot){assert(slot==1);saved++;}
void savestate_request_load(int slot){assert(slot==1);loads++;}
void savestate_request_exit_save(void){exited++;}
void savestate_cancel_boot_load(void){cancel_boot++;}
void port_game_soft_reset_request(void){resets++;}
void miyoo_remap_screen(SDL_Surface *a,SDL_Surface *b,void(*c)(void),void(*d)(int,int),int(*e)(void)){(void)a;(void)b;(void)c;(void)d;(void)e;controls++;}
static void key(SDLKey sym){SDL_Event e;memset(&e,0,sizeof e);e.type=SDL_KEYDOWN;e.key.keysym.sym=sym;assert(SDL_PushEvent(&e)==0);}
static void run(SDL_Surface *screen,int down,int confirm,int close){for(int i=0;i<down;i++)key(SDLK_DOWN);key(SDLK_SPACE);if(confirm)key(SDLK_SPACE);if(close)key(SDLK_ESCAPE);handheld_menu_run(screen);}
static void assert_row_in_panel(SDL_Surface *s,int y0,int y1){
 const int left=(s->w-420)/2+28,right=left+364;int min=9999,max=-1;
 for(int y=y0;y<y1;y++)for(int x=0;x<s->w;x++)if(((uint32_t*)((uint8_t*)s->pixels+y*s->pitch))[x]&0x00FFFFFFu){if(x<min)min=x;if(x>max)max=x;}
 assert(max>=0 && min>=left && max<right);
}
int main(void){
 assert(I_COUNT==7 && ITEM[0]==MT_RESUME && ITEM[1]==MT_SAVE && ITEM[2]==MT_LOAD && ITEM[3]==MT_CONTROLS && ITEM[4]==MT_LANGUAGE && ITEM[5]==MT_RESET_GAME && ITEM[6]==MT_EXIT);
 for (int i=0;i<I_COUNT;i++) assert(ITEM[i]!=MT_DIAGNOSTIC);
 miyoo_i18n_choose(MIYOO_DE);
 assert(!strcmp(miyoo_i18n(MT_BACK_HINT),"A: OK  B/MENU: ZURÜCK"));
 miyoo_i18n_choose(MIYOO_AUTO);
 assert(SDL_Init(SDL_INIT_VIDEO)==0);SDL_Surface *screen=SDL_SetVideoMode(640,480,32,SDL_SWSURFACE);assert(screen);handheld_menu_init(screen);
 run(screen,0,0,0);assert(!muted);
 valid=0;run(screen,1,0,0);assert(saved==1 && !muted);
 valid=1;run(screen,1,1,0);assert(saved==2 && !muted);
 run(screen,2,1,0);assert(loads==1 && !muted);
 run(screen,3,0,1);assert(controls==1 && !muted);
 /* Language: open on fifth item, move to German, confirm and back out. */
 key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_SPACE);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_SPACE);key(SDLK_ESCAPE);handheld_menu_run(screen);
 assert(miyoo_i18n_manual()==MIYOO_DE);
 /* Back closes language screen without changing preference. */
 key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_SPACE);key(SDLK_LCTRL);key(SDLK_ESCAPE);handheld_menu_run(screen);assert(miyoo_i18n_manual()==MIYOO_DE);
 /* Exercise every manual language and AUTO through the actual menu. */
 for(int choice=0;choice<5;choice++) {
   miyoo_i18n_choose(MIYOO_AUTO);
   for(int i=0;i<4;i++) key(SDLK_DOWN);
   key(SDLK_SPACE);
   for(int i=0;i<choice;i++) key(SDLK_DOWN);
   key(SDLK_SPACE); key(SDLK_ESCAPE); handheld_menu_run(screen);
   assert(miyoo_i18n_manual()==(choice ? choice-1 : MIYOO_AUTO));
 }
 key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_DOWN);key(SDLK_SPACE);key(SDLK_LCTRL);key(SDLK_ESCAPE);handheld_menu_run(screen);
 assert(!resets && !cancel_boot);
 run(screen,5,1,0);assert(resets==1 && cancel_boot==1 && !muted);
 /* Confirm defaults to off: B and MENU both cancel an armed exit. */
 for(int lang=MIYOO_EN;lang<=MIYOO_ES;lang++) {
   miyoo_i18n_preview(lang);
   assert(miyoo_text_w(miyoo_i18n(MT_BACK_HINT),2)<=380);
   assert(miyoo_text_w(miyoo_i18n(MT_SURE),2)<=380);
   for(int cancel=0;cancel<2;cancel++) {
     for(int i=0;i<6;i++) key(SDLK_DOWN);
     key(SDLK_SPACE); key(cancel ? SDLK_ESCAPE : SDLK_LCTRL);
     key(SDLK_ESCAPE); handheld_menu_run(screen);
     assert(exited==0 && !muted);
   }
 }
 miyoo_i18n_preview(MIYOO_MANUAL_AUTO);
 run(screen,6,1,0);assert(exited==1 && muted);
 /* Compare the real composed non-selected rows with a same-scale reference,
  * for every selection. Language changes recompute the common scale. */
 SDL_Surface *expected=SDL_CreateRGBSurface(SDL_SWSURFACE,640,480,32,0x00FF0000,0x0000FF00,0x000000FF,0); assert(expected);
 for(int lang=MIYOO_EN;lang<=MIYOO_ES;lang++) {
   miyoo_i18n_preview(lang); int common=menu_unselected_scale(364), minimum=3;
   for(int i=0;i<I_COUNT;i++) {
     int fit=miyoo_menu_label_scale(miyoo_text_w(miyoo_i18n(ITEM[i]),3),0,364);
     if(fit<minimum)minimum=fit;
   }
   assert(common==minimum); printf("language %d common bitmap scale %d (cell %d px)\n",lang,common,8*common);
   for(int selected=0;selected<I_COUNT;selected++) {
     compose_menu(selected,0); assert(menu_unselected_scale(364)==common);
     for(int i=0;i<I_COUNT;i++) if(i!=selected) {
       const char *label=miyoo_i18n(ITEM[i]); int cy=(480-374)/2+92+i*31;
       assert(miyoo_text_w(label,common)<=364);
       SDL_FillRect(expected,NULL,0); text_centered(expected,cy,common,label,C_TEXT);
       Uint32 white=SDL_MapRGB(sDraw->format,0xEC,0xEC,0xEC);
       for(int y=cy-13;y<cy+13;y++) for(int x=138;x<502;x++) {
         Uint32 actual=((Uint32*)((Uint8*)sDraw->pixels+y*sDraw->pitch))[x];
         Uint32 ref=((Uint32*)((Uint8*)expected->pixels+y*expected->pitch))[x];
         assert((actual==white)==(ref==white));
       }
     }
     assert(miyoo_menu_label_scale(miyoo_text_w(miyoo_i18n(ITEM[selected]),3),1,364)==3);
   }
 }
 SDL_FreeSurface(expected); miyoo_i18n_preview(MIYOO_MANUAL_AUTO);
 /* Render every localized label selected and unselected and verify actual lit pixels stay in the text lane. */
 for(int lang=MIYOO_EN;lang<=MIYOO_ES;lang++){
   miyoo_i18n_preview(lang);
   for(int i=0;i<I_COUNT;i++){
     int cy=(480-374)/2+92+i*31; int left=(sDraw->w-420)/2+28;
     SDL_FillRect(sDraw,NULL,0); SDL_Rect lane={(Sint16)left,(Sint16)(cy-13),364,26};
     SDL_SetClipRect(sDraw,&lane); text_centered(sDraw,cy,1,miyoo_i18n(ITEM[i]),0xFFFFFFFF); SDL_SetClipRect(sDraw,NULL);
     assert_row_in_panel(sDraw,cy-13,cy+13);
   }
   miyoo_i18n_preview(MIYOO_MANUAL_AUTO);
 }
 SDL_Quit();puts("public menu: Resume, Save, Load, Controls, Language, Reset and Exit dispatch PASS");return 0;
}
