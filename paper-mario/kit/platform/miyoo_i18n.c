#include "miyoo_i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miyoo_font.h"

extern const char *port_save_path(const char *name) __attribute__((weak));
extern const char *miyoo_data_dir(void) __attribute__((weak));
extern int port_pal_language(void) __attribute__((weak));

/* Catalog columns: English, German, French, Spanish. Keep strings short enough for the panel;
 * format-bearing messages use the same %d placeholder in every column. */
static const char *const tr[MT_COUNT][4] = {
 [MT_RESUME]={"RESUME","FORTSETZEN","REPRENDRE","CONTINUAR"},
 [MT_SAVE]={"SAVE STATE","SPIELSTAND SPEICHERN","SAUVER L'ÉTAT","GUARDAR ESTADO"},
 [MT_LOAD]={"LOAD STATE","SPIELSTAND LADEN","CHARGER L'ÉTAT","CARGAR ESTADO"},
 [MT_CONTROLS]={"CONTROLS","STEUERUNG","COMMANDES","CONTROLES"},
 [MT_LANGUAGE]={"LANGUAGE","SPRACHE","LANGUE","IDIOMA"},
 [MT_EXIT]={"EXIT GAME","SPIEL BEENDEN","QUITTER","SALIR DEL JUEGO"},
 [MT_AUTO]={"AUTO","AUTO","AUTO","AUTO"}, [MT_ENGLISH]={"English","Englisch","Anglais","Inglés"},
 [MT_GERMAN]={"Deutsch","Deutsch","Allemand","Alemán"}, [MT_FRENCH]={"Français","Französisch","Français","Francés"},
 [MT_SPANISH]={"Español","Spanisch","Espagnol","Español"},
 [MT_SLOT]={"<   SLOT %d   >","< PLATZ %d >","< SLOT %d >","< RANURA %d >"},
 [MT_AUTO_SAVE]={"< AUTO SAVE >","< AUTOSAVE >","< SAUVEGARDE AUTO >","< GUARDADO AUTO >"},
 [MT_SURE]={"ARE YOU SURE? (A)","SICHER? (A)","CONFIRMER ? (A)","¿SEGURO? (A)"},
 [MT_USED]={"USED","BELEGT","UTILISÉ","USADO"}, [MT_FREE]={"FREE","FREI","LIBRE","LIBRE"},
 [MT_BACK_HINT]={"A: OK  B/MENU: BACK","A: OK  B/MENU: ZURÜCK","A: OK  B/MENU: RETOUR","A: OK  B/MENU: ATRÁS"},
 [MT_RESUME_GAME]={"RESUME GAME","SPIEL FORTSETZEN","REPRENDRE","CONTINUAR"}, [MT_NEW_GAME]={"NEW GAME","NEUES SPIEL","NOUVELLE PARTIE","NUEVA PARTIDA"},
 [MT_CONTROLS_TITLE]={"CONTROLS","STEUERUNG","COMMANDES","CONTROLES"}, [MT_N64]={"N64","N64","N64","N64"}, [MT_MIYOO]={"MIYOO","MIYOO","MIYOO","MIYOO"},
 [MT_PRESS]={"PRESS...","DRÜCKEN...","APPUYEZ...","PULSAR..."}, [MT_RESET_DEFAULT]={"RESET TO DEFAULT","STANDARDWERTE","PAR DÉFAUT","VALORES INICIALES"},
 [MT_PRESS_NEW]={"PRESS THE NEW BUTTON  MENU: CANCEL","NEUE TASTE DRÜCKEN  MENU: ABBRUCH","APPUYEZ  MENU : ANNULER","PULSE  MENU: CANCELAR"},
 [MT_MENU_CANCEL]={"MENU: CANCEL","MENU: ABBRUCH","MENU : ANNULER","MENU: CANCELAR"},
 [MT_SWAP_WITH]={"SWAP WITH","TAUSCHEN MIT","ÉCHANGER AVEC","CAMBIAR CON"}, [MT_TAKE_FROM]={"TAKE FROM","ÜBERNEHMEN VON","PRENDRE À","TOMAR DE"},
 [MT_YES_NO]={"A: YES  B: NO","A: JA  B: NEIN","A: OUI  B: NON","A: SI  B: NO"},
 [MT_RESET_ALL]={"RESET ALL? A: YES  B: NO","ALLE ZURÜCKSETZEN? A: JA B: NEIN","TOUT RÉINITIALISER ? A: OUI B: NON","¿REINICIAR TODO? A: SÍ B: NO"},
 [MT_FIXED_BACK]={"FIXED   B: BACK","FEST   B: ZURÜCK","FIXE   B: RETOUR","FIJO   B: ATRÁS"},
 [MT_CHANGE_BACK]={"A: CHANGE   B: BACK","A: ÄNDERN   B: ZURÜCK","A: MODIFIER   B: RETOUR","A: CAMBIAR   B: ATRÁS"},
 [MT_SAVED_SLOT]={"SAVED SLOT %d","PLATZ %d GESPEICHERT","SLOT %d SAUVE","RANURA %d GUARDADA"},
 [MT_LOADED_SLOT]={"LOADED SLOT %d","PLATZ %d GELADEN","SLOT %d CHARGE","RANURA %d CARGADA"},
 [MT_SAVE_FAILED]={"SAVE FAILED","SPEICHERN FEHLGESCHLAGEN","ECHEC SAUVEGARDE","ERROR AL GUARDAR"},
 [MT_LOAD_FAILED]={"LOAD FAILED","LADEN FEHLGESCHLAGEN","ECHEC CHARGEMENT","ERROR AL CARGAR"},
 [MT_GAME_SAVE_FAILED]={"GAME SAVE FAILED","SPIELSTAND FEHLER","ERREUR SAUVEGARDE","ERROR DE GUARDADO"},
 [MT_RESET_HINT]={"A: RESET   B: BACK","A: RESET   B: ZURÜCK","A: RÉINITIALISER   B: RETOUR","A: REINICIAR   B: ATRÁS"},
 [MT_SAVING]={"SAVING","SPEICHERN","SAUVEGARDE","GUARDANDO"},
 [MT_LOADED_AUTO]={"LOADED AUTO SAVE","AUTOSAVE GELADEN","SAUVEGARDE AUTO CHARGÉE","GUARDADO AUTO CARGADO"},
 [MT_RESET_GAME]={"RESET GAME","SPIEL ZURÜCKSETZEN","RÉINITIALISER LE JEU","REINICIAR JUEGO"},
 [MT_RESET_WARNING]={"UNSAVED PROGRESS WILL BE LOST","UNGESPEICHERTER FORTSCHRITT GEHT VERLOREN","PROGRESSION NON SAUVEGARDÉE PERDUE","SE PERDERÁ EL PROGRESO NO GUARDADO"}
};
static int manual = MIYOO_AUTO, loaded;
static int preview = MIYOO_MANUAL_AUTO;
static int clamp_lang(int l) { return l >= MIYOO_EN && l <= MIYOO_ES ? l : MIYOO_EN; }
static void load_config(void) {
    char path[640], b[32]; FILE *f;
    if (loaded) return;
    loaded = 1;
    if (port_save_path) snprintf(path,sizeof path,"%s",port_save_path("menu-language.cfg"));
    else if (miyoo_data_dir) snprintf(path,sizeof path,"%s/menu-language.cfg",miyoo_data_dir());
    else return;
    f=fopen(path,"r"); if (!f) return;
    if (fgets(b,sizeof b,f)) { b[strcspn(b,"\r\n")]=0;
        if (!strcmp(b,"AUTO")) manual=MIYOO_AUTO;
        else if (!strcmp(b,"EN")) manual=MIYOO_EN; else if (!strcmp(b,"DE")) manual=MIYOO_DE;
        else if (!strcmp(b,"FR")) manual=MIYOO_FR; else if (!strcmp(b,"ES")) manual=MIYOO_ES;
    } fclose(f);
}
void miyoo_i18n_choose(int l) {
    static const char *const code[]={"EN","DE","FR","ES"}; char path[640],tmp[672]; FILE *f;
    manual = l == MIYOO_AUTO ? MIYOO_AUTO : clamp_lang(l); loaded=1;
    if (port_save_path) snprintf(path,sizeof path,"%s",port_save_path("menu-language.cfg"));
    else if (miyoo_data_dir) snprintf(path,sizeof path,"%s/menu-language.cfg",miyoo_data_dir());
    else return;
    snprintf(tmp,sizeof tmp,"%s.tmp",path); f=fopen(tmp,"w"); if (!f) return;
    fprintf(f,"%s\n",manual==MIYOO_AUTO?"AUTO":code[manual]);
    if (fclose(f)==0) rename(tmp,path); else remove(tmp);
}
int miyoo_i18n_manual(void) { load_config(); return manual; }
int miyoo_i18n_language(void) {
    int l; load_config(); if (preview != MIYOO_MANUAL_AUTO) return preview==MIYOO_AUTO ? clamp_lang(port_pal_language?port_pal_language():-1) : preview;
    if (manual != MIYOO_AUTO) return manual;
    l=port_pal_language ? port_pal_language() : -1; return clamp_lang(l);
}
void miyoo_i18n_preview(int l) { preview=l==MIYOO_MANUAL_AUTO?MIYOO_MANUAL_AUTO:l==MIYOO_AUTO?MIYOO_AUTO:clamp_lang(l); }
const char *miyoo_i18n(enum miyoo_text_id id) {
    if ((unsigned)id>=MT_COUNT) return "?";
    if (id==MT_AUTO && miyoo_i18n_manual()==MIYOO_AUTO && preview==MIYOO_MANUAL_AUTO) return tr[id][MIYOO_EN];
    return tr[id][miyoo_i18n_language()];
}
unsigned miyoo_utf8_next(const char **pp) {
    const unsigned char *p=(const unsigned char *)*pp; unsigned c=p[0], n=0, v;
    if (!c) return 0;
    if (c<0x80) { *pp+=1; return c; }
    if ((c&0xE0)==0xC0) { n=1; v=c&31; } else if ((c&0xF0)==0xE0) { n=2; v=c&15; } else if ((c&0xF8)==0xF0) { n=3; v=c&7; } else { *pp+=1; return '?'; }
    for (unsigned i=1;i<=n;i++) { if (!p[i] || (p[i]&0xC0)!=0x80) { *pp+=1; return '?'; } v=(v<<6)|(p[i]&63); }
    if ((n==1&&v<0x80)||(n==2&&v<0x800)||(n==3&&v<0x10000)||v>0x10FFFF||(v>=0xD800&&v<=0xDFFF)) { *pp+=n+1; return '?'; }
    *pp+=n+1; return v;
}
int miyoo_utf8_width(const char *s,int k) { int n=0; while (*s) { (void)miyoo_utf8_next(&s); n+=8*k; } return n; }
void miyoo_glyph(unsigned cp, uint8_t out[8]) {
    unsigned base='?'; int mark=0;
    switch(cp) {
    case 0x00C4:case 0x00E4: base=cp==0x00C4?'A':'a';mark=1;break;
    case 0x00D6:case 0x00F6: base=cp==0x00D6?'O':'o';mark=1;break;
    case 0x00DC:
        memcpy(out,kMiyooFont['U'-0x20],8);
        out[0]=0x24; out[1]=0x18; out[2]=0x33;
        return;
    case 0x00FC: base='u';mark=1;break;
    case 0x00DF: memcpy(out,kMiyooFont['B'-0x20],8);out[0]=0;out[1]=0x3E;out[2]=0x63;out[3]=0x3E;out[4]=0x63;out[5]=0x63;out[6]=0x3E;out[7]=0;return;
    case 0x00C9:case 0x00E9: base=cp==0x00C9?'E':'e';mark=2;break;
    case 0x00C1:case 0x00E1: base=cp==0x00C1?'A':'a';mark=2;break;
    case 0x00C8:case 0x00E8: base=cp==0x00C8?'E':'e';mark=3;break;
    case 0x00CA:case 0x00EA: base=cp==0x00CA?'E':'e';mark=4;break;
    case 0x00C0:case 0x00E0: base=cp==0x00C0?'A':'a';mark=3;break;
    case 0x00C2:case 0x00E2: base=cp==0x00C2?'A':'a';mark=4;break;
    case 0x00CE:case 0x00EE: base=cp==0x00CE?'I':'i';mark=4;break;
    case 0x00D4:case 0x00F4: base=cp==0x00D4?'O':'o';mark=4;break;
    case 0x00D9:case 0x00F9: base=cp==0x00D9?'U':'u';mark=3;break;
    case 0x00D1:case 0x00F1: base=cp==0x00D1?'N':'n';mark=1;break;
    case 0x00CD:case 0x00ED: base=cp==0x00CD?'I':'i';mark=2;break;
    case 0x00DA:case 0x00FA: base=cp==0x00DA?'U':'u';mark=2;break;
    case 0x00C7:case 0x00E7: base=cp==0x00C7?'C':'c';mark=5;break;
    case 0x00BF:case 0x00A1: base='?';mark=0;memcpy(out,kMiyooFont['?'-0x20],8);out[0]=cp==0x00BF?0x18:0;out[1]=cp==0x00BF?0x18:0;out[6]=cp==0x00A1?0x18:0;out[7]=cp==0x00A1?0x18:0;return;
    default: memcpy(out,kMiyooFont[(cp>=0x20&&cp<=0x7E?cp:'?')-0x20],8);return;
    }
    memcpy(out,kMiyooFont[base-0x20],8);
    if(mark==1){out[0]=0x24;out[1]=0x18;}
    else if(mark==2){out[0]=0x18;out[1]=0x30;}
    else if(mark==3){out[0]=0x30;out[1]=0x18;}
    else if(mark==4){out[0]=0x18;out[1]=0x24;}
    else if(mark==5){out[6]=0x0C;out[7]=0x18;}
}
