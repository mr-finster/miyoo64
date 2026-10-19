/* The game's entry (game side): nusys's boot would start the idle and main threads and call mainproc. */
#include "common.h"
#include "hh_port.h"

void mainproc(void *arg);

void hh_game_main(void) { mainproc(NULL); }
