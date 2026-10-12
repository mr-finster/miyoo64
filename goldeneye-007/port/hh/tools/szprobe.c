/* Build-time probe of game struct sizes (hh/tools, not linked into the game). */
#include <ultra64.h>
#include "game/ramromreplay.h"
#include "game/file.h"
extern int printf(const char *, ...);
int main(void)
{
    printf("ramromfilestructure %zx save_data %zx\n", sizeof(ramromfilestructure), sizeof(save_data));
    return 0;
}
