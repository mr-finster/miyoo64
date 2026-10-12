/**
 * errno for files that include the N64 headers (the lead's matrix X4: savestate.c names the
 * failing save stage and its reason). <errno.h> defines errno as a macro, and the N64 headers use
 * errno as a field name, so only this file includes it. port_errno_clear() before a call that may
 * not set errno itself (zlib), port_errno() right after it fails.
 */
#include <errno.h>

int port_errno(void) {
    return errno;
}

void port_errno_clear(void) {
    errno = 0;
}

/* 1 if err is "no such file" (port_misc.c: a missing fzerox.sram is a new save, any other open error
 * is a save that is there but can't be read). */
int port_errno_missing(int err) {
    return err == ENOENT;
}
