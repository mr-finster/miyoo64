/**
 * Code overlays on the port (ovl_i1..i6, ovl_menu, ovl_ending, ovl_unused).
 *
 * The N64 DMAs an overlay's code and data into shared RAM when a scene needs
 * it and zeroes its .bss, so every (re)load starts from the overlay's initial
 * data. Here all overlays are linked into the executable; to keep the reload
 * semantics each overlay object's .data/.bss are renamed into their own
 * sections (Makefile.handheld, ovl_sections.ld) and port_overlay_load() puts the
 * .data back to its boot-time image and zeroes the .bss.
 *
 * The scene RAM that follows the overlay on the N64 (the scene's asset banks,
 * Load_SceneFiles) is gPortSceneRam.
 */
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "port.h"
#include "savestate.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Largest per-scene sum of asset banks is ~1 MB (measured: see BUILDLOG);
 * 3 MB leaves room and Load_SceneFiles logs an overflow. */
#define PORT_SCENE_RAM_SIZE 0x300000
__attribute__((aligned(16))) u8 gPortSceneRam[PORT_SCENE_RAM_SIZE];
u32 gPortSceneRamSize = PORT_SCENE_RAM_SIZE;

#define OVL(name)                                                                       \
    extern u8 name##_ROM_START[], name##_DATA_START[], name##_DATA_END[], name##_BSS_START[], \
        name##_BSS_END[];
OVL(ovl_i1) OVL(ovl_i2) OVL(ovl_i3) OVL(ovl_i4) OVL(ovl_i5) OVL(ovl_i6) OVL(ovl_menu) OVL(ovl_ending)
OVL(ovl_unused)
#undef OVL

typedef struct {
    const char* name;
    u8* romStart;
    u8 *data, *dataEnd, *bss, *bssEnd;
    u8* pristine; /* heap copy of the boot-time .data: never saved */
} PortOverlay;

#define OVL(name) { #name, name##_ROM_START, name##_DATA_START, name##_DATA_END, name##_BSS_START, name##_BSS_END, NULL },
static PortOverlay sOverlays[] SAVESTATE_EXCLUDE = {
    OVL(ovl_i1) OVL(ovl_i2) OVL(ovl_i3) OVL(ovl_i4) OVL(ovl_i5) OVL(ovl_i6) OVL(ovl_menu) OVL(ovl_ending)
    OVL(ovl_unused)
};
#undef OVL

/* Before the game runs: snapshot every overlay's initial .data. */
void port_overlay_init(void) {
    size_t i, total = 0;

    for (i = 0; i < ARRAY_COUNT(sOverlays); i++) {
        PortOverlay* o = &sOverlays[i];
        size_t n = (size_t) (o->dataEnd - o->data);

        o->pristine = n ? malloc(n) : NULL;
        if (n) memcpy(o->pristine, o->data, n);
        total += n;
    }
    port_log("overlays: %u bytes of initial data kept\n", (unsigned) total);
}

void port_overlay_load(void* romStart) {
    size_t i;

    for (i = 0; i < ARRAY_COUNT(sOverlays); i++) {
        PortOverlay* o = &sOverlays[i];

        if (o->romStart == (u8*) romStart) {
            if (o->pristine) memcpy(o->data, o->pristine, (size_t) (o->dataEnd - o->data));
            memset(o->bss, 0, (size_t) (o->bssEnd - o->bss));
            { /* its data that lives in .hhasset (rebuilt from the ROM) comes back too */
                extern void port_embedded_refill(u32 ovl);
                port_embedded_refill((u32) i);
            }
            port_log("overlay %s reloaded\n", o->name);
            return;
        }
    }
    port_log_always("overlay load: no overlay at ROM %p\n", romStart);
}
