/* qemu-user TCG plugin: executed guest instructions per translation block,
 * keyed by the block's start address. At exit writes "<hex vaddr> <count>"
 * lines to out=<file> (default /tmp/insnprof.txt) plus a "total" line; a
 * separate script maps addresses to function symbols (nm -n).
 * Counting is an inline add of the block's instruction count on every
 * execution (cheap; races between guest threads can lose a few counts). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qemu-plugin.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define TBL_BITS 21
#define TBL_SIZE (1u << TBL_BITS)
typedef struct { uint64_t vaddr; uint64_t count; } Ent;
static Ent *tbl;
static char out_path[512] = "/tmp/insnprof.txt";

static Ent *lookup(uint64_t vaddr) {
    uint64_t h = (vaddr * 0x9E3779B97F4A7C15ull) >> (64 - TBL_BITS);
    for (;;) {
        Ent *e = &tbl[h];
        if (e->vaddr == vaddr) return e;
        if (e->vaddr == 0) { e->vaddr = vaddr; return e; }
        h = (h + 1) & (TBL_SIZE - 1);
    }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    (void) id;
    uint64_t va = qemu_plugin_tb_vaddr(tb);
    size_t n = qemu_plugin_tb_n_insns(tb);
    Ent *e = lookup(va ? va : 1);
    qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &e->count, n);
}

static void at_exit(qemu_plugin_id_t id, void *p) {
    (void) id; (void) p;
    FILE *f = fopen(out_path, "w");
    uint64_t total = 0;
    if (!f) return;
    for (uint32_t i = 0; i < TBL_SIZE; i++) {
        if (tbl[i].vaddr && tbl[i].count) {
            fprintf(f, "%" PRIx64 " %" PRIu64 "\n", tbl[i].vaddr, tbl[i].count);
            total += tbl[i].count;
        }
    }
    fprintf(f, "total %" PRIu64 "\n", total);
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    (void) info;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "out=", 4) == 0) snprintf(out_path, sizeof(out_path), "%s", argv[i] + 4);
    }
    tbl = calloc(TBL_SIZE, sizeof(Ent));
    if (!tbl) return -1;
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
