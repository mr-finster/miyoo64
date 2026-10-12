/* hh_romtables.c: the game's ROM data tables, filled from the player's own ROM at start.
 *
 * The game code compiles in tables the decomp commits as C from the ROM's data segment
 * (weapon stats, model headers, skeletons, the image table: port/assets). A shipped
 * executable carries them zeroed -- tools/romtables.py scrubs them and sets
 * hh_romtables_scrubbed -- and recipes.bin, next to it, says in numbers only where each
 * run comes from in the ROM's unpacked data segment and how wide its fields are. Here the
 * data segment is unpacked from the ROM (rarezip, the game's own inflate) and the runs are
 * copied back, each field swapped to host order. The desktop proxy keeps its tables. */
#define _GNU_SOURCE 1 /* dl_iterate_phdr */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>
#include "hh_hw.h"
#include "hh_os.h"
#include "hh_crc32.h"
#include "hh_romscreen.h"

/* the fill's own bar, in percent: the ROM's CRC, then the unpack (the game's inflate, one call), then the runs */
#define FILL_P_CRC 6
#define FILL_P_UNPACK 70

/* 1 in a shipped executable (romtables.py); volatile: never folded into the code */
volatile u32 hh_romtables_scrubbed = 0;

u32 decompressdata(u8 *src, u8 *dst, void *huffman_table); /* src/game/decompress.c */
extern char __executable_start[], etext[], _end[];
/* hh/romtables.ld: the filled strings' own pages, between the code and .rodata */
extern char __romtables_start[], __romtables_end[];

/* The main program's writable PT_LOADs: a filled byte is in one of them or in .romtables. */
#define HH_RW_MAX 4
static struct {
    uintptr_t lo, hi;
} sRw[HH_RW_MAX];
static int sRwCount;

static int hh_find_rw(struct dl_phdr_info *info, size_t sz, void *data)
{
    int i;
    (void)sz;
    (void)data;
    for (i = 0; i < info->dlpi_phnum && sRwCount < HH_RW_MAX; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_W)) {
            sRw[sRwCount].lo = info->dlpi_addr + p->p_vaddr;
            sRw[sRwCount].hi = sRw[sRwCount].lo + p->p_memsz;
            sRwCount++;
        }
    }
    return 1; /* the main program comes first */
}

/* The pages the fill unprotects: .romtables, rounded out to the running page size. A page that holds
 * code ([__executable_start, etext): headers, .init, .plt, .text, .fini) keeps PROT_EXEC throughout
 * (never the case with romtables.ld's page alignment; logged if a layout ever breaks that). */
static uintptr_t sPgLo, sPgHi, sCodeLo, sCodeHi;
static long sPage;

static int hh_page_has_code(uintptr_t pg) { return pg < sCodeHi && pg + (uintptr_t)sPage > sCodeLo; }

static void hh_romtables_pages(void)
{
    const uintptr_t lo = (uintptr_t)__romtables_start, hi = (uintptr_t)__romtables_end;
    uintptr_t pg;
    int code = 0;
    sPage = sysconf(_SC_PAGESIZE);
    if (sPage <= 0)
        sPage = 4096;
    sCodeLo = (uintptr_t)__executable_start & ~(uintptr_t)(sPage - 1);
    sCodeHi = (uintptr_t)etext;
    sPgLo = lo & ~(uintptr_t)(sPage - 1);
    sPgHi = (hi + (uintptr_t)sPage - 1) & ~(uintptr_t)(sPage - 1);
    for (pg = sPgLo; pg < sPgHi; pg += (uintptr_t)sPage)
        code += hh_page_has_code(pg);
    if (code == 0 && sPgLo == lo && sPgHi == hi)
        printf("romtables: pages %08lx-%08lx, code-free\n", (unsigned long)sPgLo, (unsigned long)sPgHi);
    else
        printf("romtables: pages %08lx-%08lx (.romtables %08lx-%08lx, code to %08lx): %d page(s) hold code, "
               "kept executable through the fill\n",
               (unsigned long)sPgLo, (unsigned long)sPgHi, (unsigned long)lo, (unsigned long)hi,
               (unsigned long)sCodeHi, code);
}

/* the fill's pages: writable (fill = 1), then read-only; code pages stay executable */
static int hh_romtables_protect(int fill)
{
    uintptr_t pg;
    for (pg = sPgLo; pg < sPgHi; pg += (uintptr_t)sPage) {
        int prot = PROT_READ | (fill ? PROT_WRITE : 0) | (hh_page_has_code(pg) ? PROT_EXEC : 0);
        if (mprotect((void *)pg, (size_t)sPage, prot) != 0)
            return -1;
    }
    return 0;
}

/* a run [a, a + n) the fill may write: inside .romtables or a writable segment */
static int hh_romtables_writable(uintptr_t a, uintptr_t n)
{
    int i;
    if (a >= (uintptr_t)__romtables_start && n <= (uintptr_t)__romtables_end - a)
        return 1;
    for (i = 0; i < sRwCount; i++)
        if (a >= sRw[i].lo && n <= sRw[i].hi - a)
            return 1;
    return 0;
}

typedef struct {
    char magic[8];
    u32 count, rom_start, rom_end, vaddr, seg_size, crc_packed, crc_result, total;
} HhRecipeHeader;

typedef struct {
    u32 host, n64, len, width;
} HhRecipe;

static void hh_romtables_fail(const char *why)
{
    fprintf(stderr, "FATAL: the game data tables cannot be filled: %s\n", why);
    fflush(stderr);
    exit(1);
}

/* recipes.bin: $GE_RECIPES, else next to the executable */
static u8 *hh_romtables_read(u32 *size)
{
    char path[600];
    const char *e = getenv("GE_RECIPES");
    FILE *f;
    long n;
    u8 *buf;
    if (e != NULL) {
        snprintf(path, sizeof(path), "%s", e);
    } else {
        ssize_t k = readlink("/proc/self/exe", path, sizeof(path) - 16);
        char *slash;
        if (k <= 0)
            return NULL;
        path[k] = 0;
        slash = strrchr(path, '/');
        strcpy(slash ? slash + 1 : path, "recipes.bin");
    }
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *size = (u32)n;
    return buf;
}

static long hh_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void hh_romtables_load(void)
{
    const uintptr_t lo = (uintptr_t)__executable_start, hi = (uintptr_t)_end;
    HhRecipeHeader h;
    const HhRecipe *r;
    u8 *file, *seg, *huft;
    u32 size, i, crc = 0;
    long t0;

    hh_romtables_pages(); /* the self-check line, every start */
    if (!hh_romtables_scrubbed)
        return;
    /* The first-start screen (hh_romscreen.c) is still up from the card work, if there was any, and
     * gets the steps; a later start shows nothing unless this runs past its HH_LAZY_MS. */
    t0 = hh_ms();
    hh_romscreen_fill_begin();
    hh_romscreen_fill("Checking the ROM data...", 0);
    file = hh_romtables_read(&size);
    if (file == NULL)
        hh_romtables_fail("recipes.bin is missing");
    if (size < sizeof(h))
        hh_romtables_fail("recipes.bin is truncated");
    memcpy(&h, file, sizeof(h));
    if (memcmp(h.magic, "GERCP1\0\0", 8) != 0 || size != sizeof(h) + h.count * sizeof(HhRecipe))
        hh_romtables_fail("recipes.bin does not belong to this build");
    if (h.rom_end <= h.rom_start || h.rom_end > hh_rom_size ||
        hh_crc32(0, hh_rom + h.rom_start, h.rom_end - h.rom_start) != h.crc_packed)
        hh_romtables_fail("this ROM is not GoldenEye 007 (USA) as the tables expect");
    hh_romscreen_fill("Unpacking game data...", FILL_P_CRC);

    seg = malloc(h.seg_size + 0x100);
    huft = malloc(0x10000);
    if (seg == NULL || huft == NULL)
        hh_romtables_fail("out of memory");
    if (decompressdata((u8 *)hh_rom + h.rom_start, seg, huft) != h.seg_size)
        hh_romtables_fail("the ROM's data segment did not unpack");
    hh_romscreen_fill("Filling the tables...", FILL_P_UNPACK);

    /* the strings some tables point at: .romtables (romtables.ld), read-only outside the fill */
    dl_iterate_phdr(hh_find_rw, NULL);
    if (hh_romtables_protect(1) != 0)
        hh_romtables_fail("cannot unprotect the tables' pages");

    r = (const HhRecipe *)(file + sizeof(h));
    for (i = 0; i < h.count; i++) {
        u8 *dst = (u8 *)(uintptr_t)r[i].host;
        const u8 *src = seg + (r[i].n64 - h.vaddr);
        u32 k, w = r[i].width, len = r[i].len;
        if (r[i].host < lo || r[i].host + len > hi || !hh_romtables_writable(r[i].host, len) || r[i].n64 < h.vaddr || r[i].n64 - h.vaddr + len > h.seg_size ||
            (w != 1 && w != 2 && w != 4 && w != 8) || len % w != 0)
            hh_romtables_fail("recipes.bin is damaged");
        for (k = 0; k < len; k += w) {
            u32 b;
            for (b = 0; b < w; b++)
                dst[k + b] = src[k + w - 1 - b];
        }
        crc = hh_crc32(crc, dst, len);
        if ((i & 63) == 63)
            hh_romscreen_fill("Filling the tables...", FILL_P_UNPACK + (int)((100 - FILL_P_UNPACK) * (u64)(i + 1) / h.count));
    }
    if (crc != h.crc_result)
        hh_romtables_fail("the filled tables do not match the build");
    if (hh_romtables_protect(0) != 0)
        hh_romtables_fail("cannot protect the tables' pages again");
    free(seg);
    free(huft);
    free(file);
    printf("[romtables] %u runs, %u bytes of game tables filled from the ROM in %ld ms\n", h.count, h.total,
           hh_ms() - t0);
}
