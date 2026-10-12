/* reloc_dump.c -- build-machine helper for the leak scan (tools/leak_scan.py):
 * writes every relocData file, decompressed, from the player's ROM into a
 * scratch directory, so the scan can index the game data in the form the port
 * holds it in memory. Never shipped; the shipped game has no dump feature.
 *
 *   cc -O2 -Iport/hh -o reloc_dump tools/reloc_dump.c port/hh/romdata.c port/hh/bs/vpk0.c
 *   SSB64_ROM=<rom> ./reloc_dump <scratch dir>        (writes <dir>/romdata.bin too) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "romdata.h"

void port_log(const char *fmt, ...) { (void)fmt; }
void port_log_always(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv)
{
    static unsigned char buf[1 << 20];
    char err[256], path[512];
    if (argc != 2) { fprintf(stderr, "usage: SSB64_ROM=<rom> reloc_dump <dir>\n"); return 2; }
    if (romdata_init(argv[1], err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    for (unsigned id = 0; id < ROMDATA_RELOC_COUNT; id++) {
        RomRelocInfo in;
        FILE *f;
        if (romdata_reloc_info(id, &in) != 0 || in.size > sizeof buf || romdata_reloc_read(id, buf, sizeof buf) != 0) {
            fprintf(stderr, "reloc %u: read failed\n", id);
            return 1;
        }
        snprintf(path, sizeof path, "%s/reloc_%04u.bin", argv[1], id);
        if ((f = fopen(path, "wb")) == NULL || fwrite(buf, 1, in.size, f) != in.size) return 1;
        fclose(f);
    }
    printf("reloc_dump: %u files\n", ROMDATA_RELOC_COUNT);
    return 0;
}
