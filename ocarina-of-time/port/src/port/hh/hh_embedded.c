/* The ROM data compiled into game code (actor overlays, transitions) lives in a
 * NOLOAD region of the shipped executable (Makefile.handheld, "Embedded ROM data").
 * Fill it at boot from <assets>/embedded.bin: the region's bytes followed by the
 * per-link build stamp and "HHEB" ("FKEB" from earlier builds is taken too). A blob from another build is refused. (M3 builds
 * this blob on the device from the player's ROM.) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned char __hhassets_start[];
extern unsigned char __hhassets_end[];
extern const uint32_t hh_build_stamp;

int HhEmbedded_Load(const char* dir) {
    char path[512];
    size_t size = (size_t)(__hhassets_end - __hhassets_start);
    unsigned char trailer[8];
    FILE* f;
    long fileSize;

    if (size == 0) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/embedded.bin", dir);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("hh: missing %s (embedded ROM data, %u bytes)\n", path, (unsigned)size);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize != (long)(size + sizeof(trailer))) {
        printf("hh: %s is %ld bytes, this build needs %u\n", path, fileSize, (unsigned)(size + sizeof(trailer)));
        fclose(f);
        return 0;
    }
    if (fread(__hhassets_start, 1, size, f) != size || fread(trailer, 1, sizeof(trailer), f) != sizeof(trailer)) {
        fclose(f);
        memset(__hhassets_start, 0, size);
        return 0;
    }
    fclose(f);
    if ((memcmp(trailer + 4, "HHEB", 4) != 0 && memcmp(trailer + 4, "FKEB", 4) != 0) ||
        (uint32_t)(trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) | ((uint32_t)trailer[3] << 24)) != hh_build_stamp) {
        printf("hh: %s is from another build\n", path);
        memset(__hhassets_start, 0, size);
        return 0;
    }
    printf("hh: embedded ROM data loaded (%u bytes)\n", (unsigned)size);
    return 1;
}
