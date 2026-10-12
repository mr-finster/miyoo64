/* hh_stubs.c: symbols the game references that the port has no hardware for. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>

/* Microcode images: the game only stores their addresses (and size differences) in its
 * OSTasks; the port executes tasks by type (hh_plat_rsp_task). Distinct addresses keep
 * those sizes positive. */
long long int rspbootTextStart[2];
long long int rspbootTextEnd[1];
long long int gsp3DTextStart[2];
long long int gsp3DDataStart[2];
long long int aspMainTextStart[2];
long long int aspMainDataStart[2];

/* crash.c is the N64 exception handler and its debug screen */
void crashInit(void) {}
void crashRenderFrame(u16 *buffer) { (void)buffer; }

/* rmon.c's osSyncPrintf formats with the game's own _Printf into the crash screen's
 * text buffer, one character at a time. GE_SYNCPRINTF=1 copies it to stderr. */
void crashAppendChar(char c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("GE_SYNCPRINTF") != NULL;
    if (on)
        fputc(c, stderr);
}
