/*
 * hh_recipes: derive the first-start recipes (M3b). Host tool, run by Makefile.handheld
 * after the link, on the developer's machine (it needs the ROM-built files to compare).
 *
 *   hh_recipes <asset_segments.c> <baserom dir> <oot_psp_assets.bin> <embedded.bin>
 *              <region.syms> <region base> <ptr lo> <ptr hi> <stamp> <out recipes.bin>
 *
 * The recipes say how each byte of the two game data files comes from the player's
 * own ROM; they never carry the ROM's bytes (the leak gate scans them against the ROM).
 *
 *  oot_psp_assets.bin: raw entries are ROM files as they are (Yaz0-decompressed);
 *    a native entry is its ROM file reshaped block by block: per 8 bytes, one of the 56
 *    byte-order permutations (oot-PSP's OPZ4 set: every split of 8 into 8/4/2/1-byte
 *    fields, each reversed), zero, or two 4-byte word ops.
 *  embedded.bin (the NOLOAD .hhassets region): per symbol, the offset of its bytes in a
 *    raw pack entry (code, ovl_*), then the same block/word ops.
 *
 * Word ops: one of the 24 permutations of the word's 4 source bytes; zero; VROM (the
 * ROM's big-endian word is a ROM file's start/end, rewritten to its place in the pack:
 * scene room lists); a literal word. Literals are the only numbers the recipes carry:
 * pointers into this executable (display lists that point at their own vertices), and
 * GBI command words the port's compiler encodes differently from the ROM's
 * (z_select_static is F3DEX on the cartridge). Each kind is counted and printed; the
 * build fails on any literal that is neither a pointer nor inside the known budget.
 *
 * Output (little endian): see hh_convert.c, which replays it on the device.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../../src/port/hh/hh_recipe_ops.h"

#define MAX_ENTRIES 2048
#define MAX_SYMS 4096
#define LITERAL_BUDGET 4096 /* non-pointer literal words allowed in all (currently ~2.9k) */

typedef struct {
    uint32_t vs, ve, ovs, ove, flags, fo;
    char name[64];
} Entry;
typedef struct {
    uint32_t addr, size;
    char name[96];
} Sym;

static Entry sE[MAX_ENTRIES];
static int sNE;
static Sym sS[MAX_SYMS];
static int sNS;
static int sP8[64][8], sNP8;
static int sP4[24][4];
static uint32_t sPtrLo, sPtrHi;
static long sNoSrc, sLitPtr, sLitOther, sVrom, sBlocks, sSplit, sZero;

static unsigned char* slurp(const char* p, long* n) {
    FILE* f = fopen(p, "rb");
    unsigned char* b;
    if (!f) { fprintf(stderr, "hh_recipes: cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(*n + 16);
    if (fread(b, 1, *n, f) != (size_t)*n) { fprintf(stderr, "hh_recipes: short read %s\n", p); exit(1); }
    fclose(f);
    return b;
}

static HhrPerms sT;
static void load_perms(void) {
    int i, k;
    if (!hhr_perms(&sT)) { fprintf(stderr, "hh_recipes: permutation tables\n"); exit(1); }
    for (i = 0; i < HHR_NP8; i++) for (k = 0; k < 8; k++) sP8[i][k] = sT.p8[i][k];
    for (i = 0; i < 24; i++) for (k = 0; k < 4; k++) sP4[i][k] = sT.p4[i][k];
    sNP8 = HHR_NP8;
}

/* ---- op stream ---- */
static unsigned char* sOps;
static size_t sOpsN, sOpsCap;
static void op(unsigned v) {
    if (sOpsN == sOpsCap) { sOpsCap = sOpsCap ? sOpsCap * 2 : 1 << 20; sOps = realloc(sOps, sOpsCap); }
    sOps[sOpsN++] = (unsigned char)v;
}
static void op32(uint32_t v) { op(v & 0xFF); op((v >> 8) & 0xFF); op((v >> 16) & 0xFF); op(v >> 24); }

static uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char* p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

static int vrom_map(uint32_t v, uint32_t* out) {
    int i;
    if (v == 0) return 0;
    for (i = 0; i < sNE; i++) {
        if (sE[i].ovs == v) { *out = sE[i].vs; return 1; }
        if (sE[i].ove == v) { *out = sE[i].ve; return 1; }
    }
    return 0;
}

static int vrom_cb(void* ctx, uint32_t v, uint32_t* out) { (void)ctx; return vrom_map(v, out); }

static int sCurLiteralOk; /* 1 while encoding z_select_static / region symbols */

static void word_op(const unsigned char* R, const unsigned char* N, const char* who, long at) {
    int i;
    uint32_t w = le32(N), m;
    for (i = 0; i < 24; i++)
        if (R[sP4[i][0]] == N[0] && R[sP4[i][1]] == N[1] && R[sP4[i][2]] == N[2] && R[sP4[i][3]] == N[3]) { op(i); return; }
    if (w == 0) { op(24); return; }
    if (vrom_map(be32(R), &m) && m == w) { op(25); sVrom++; return; }
    op(26); op32(w);
    if (w >= sPtrLo && w < sPtrHi) sLitPtr++;
    else {
        sLitOther++;
        if (sLitOther <= 12 || getenv("HHR_ALL")) fprintf(stderr, "  literal %s+0x%lX: rom %08X -> %08X\n", who, at, be32(R), w);
    }
    (void)sCurLiteralOk;
}

/* Encode n bytes N (target) from R (source, same length). */
static void encode(const unsigned char* R, const unsigned char* N, long n, const char* who) {
    long o;
    static const unsigned char z[8];
    for (o = 0; o + 8 <= n; o += 8) {
        int s = -1, i, k;
        sBlocks++;
        for (i = 0; i < sNP8 && s < 0; i++) {
            for (k = 0; k < 8; k++) if (R[o + sP8[i][k]] != N[o + k]) break;
            if (k == 8) s = i;
        }
        if (s >= 0) { op(s); continue; }
        if (!memcmp(N + o, z, 8)) { op(56); sZero++; continue; }
        op(57); sSplit++;
        word_op(R + o, N + o, who, o);
        word_op(R + o + 4, N + o + 4, who, o + 4);
    }
    /* tail (< 8 bytes): a whole word as one word op (op 0x80 | word op), then per byte an
     * index into the tail's source bytes, zero, or a literal */
    if (n - o >= 4) {
        op(0x80);
        word_op(R + o, N + o, who, o);
        o += 4;
    }
    for (; o < n; o++) {
        long t0 = n - n % 4, j;
        int done = 0;
        for (j = t0; j < n && !done; j++) if (R[j] == N[o]) { op((unsigned)(j - t0)); done = 1; }
        if (done) continue;
        if (N[o] == 0) { op(0xFE); continue; }
        op(0xFF); op(N[o]); sLitOther++;
        if (getenv("HHR_ALL")) fprintf(stderr, "  literal byte %s+0x%lX: %02X\n", who, o, N[o]);
    }
}

/* ---- symbol source search: hash of 8-byte keys at 2-aligned offsets of raw entries ---- */
typedef struct { uint64_t key; uint32_t pos; } HE;
static HE* sH;
static size_t sHN;
static int he_cmp(const void* a, const void* b) {
    const HE *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}
static uint64_t key8(const unsigned char* p) { uint64_t k; memcpy(&k, p, 8); return k; }
static size_t h_lower(uint64_t k) {
    size_t lo = 0, hi = sHN;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (sH[m].key < k) lo = m + 1; else hi = m; }
    return lo;
}
static int is_src_entry(const Entry* e) { return e->flags == 0 && (!strcmp(e->name, "code") || !strncmp(e->name, "ovl_", 4)); }

static long score(const unsigned char* pack, long packN, long src, const unsigned char* N, long n) {
    long o, good = 0;
    static const unsigned char z[8];
    if (src < 0 || src + n > packN) return -1;
    for (o = 0; o + 8 <= n; o += 8) {
        int i, k;
        for (i = 0; i < sNP8; i++) {
            for (k = 0; k < 8; k++) if (pack[src + o + sP8[i][k]] != N[o + k]) break;
            if (k == 8) break;
        }
        if (i < sNP8 && memcmp(N + o, z, 8)) good++;
    }
    return good;
}

/* word-level score: target words (non-zero) that are a byte permutation of the source word */
static long score_words(const unsigned char* pack, long packN, long src, const unsigned char* N, long n) {
    long o, good = 0;
    if (src < 0 || src + n > packN) return -1;
    for (o = 0; o + 4 <= n; o += 4) {
        const unsigned char* R = pack + src + o;
        int i;
        if (!N[o] && !N[o + 1] && !N[o + 2] && !N[o + 3]) continue;
        for (i = 0; i < 24; i++)
            if (R[sP4[i][0]] == N[o] && R[sP4[i][1]] == N[o + 1] && R[sP4[i][2]] == N[o + 2] && R[sP4[i][3]] == N[o + 3]) { good++; break; }
    }
    return good;
}

typedef struct { uint32_t key; uint32_t pos; } HE4;
static HE4* sH4;
static size_t sH4N;
static int he4_cmp(const void* a, const void* b) {
    const HE4 *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}
static size_t h4_lower(uint32_t k) {
    size_t lo = 0, hi = sH4N;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (sH4[m].key < k) lo = m + 1; else hi = m; }
    return lo;
}

typedef struct { long src; int votes; } Cand;
static int cand_cmp(const void* a, const void* b) {
    const Cand *x = a, *y = b;
    if (x->votes != y->votes) return y->votes - x->votes;
    return x->src < y->src ? -1 : x->src > y->src;
}

static int src_cmp(const void* a, const void* b) {
    const Cand *x = a, *y = b;
    return x->src < y->src ? -1 : x->src > y->src;
}

/* in-source-entry check: the whole symbol must lie inside one source entry */
static int src_ok(long src, long n) {
    int i;
    for (i = 0; i < sNE; i++)
        if (is_src_entry(&sE[i]) && src >= (long)sE[i].fo && src + n <= (long)(sE[i].fo + (sE[i].ve - sE[i].vs))) return 1;
    return 0;
}

static long find_source(const unsigned char* pack, long packN, const unsigned char* N, long n, long* bestScore) {
    Cand* c = NULL;
    size_t nc = 0, cap = 0, i;
    long o, best = -1, bs = -1;
    static const unsigned char z[8];
    int used = 0;
    for (o = 0; o + 8 <= n && used < 48; o += 8) {
        int p;
        if (!memcmp(N + o, z, 8)) continue;
        used++;
        for (p = 0; p < sNP8; p++) {
            unsigned char R[8];
            size_t h;
            int k;
            for (k = 0; k < 8; k++) R[sP8[p][k]] = N[o + k];
            for (h = h_lower(key8(R)); h < sHN && sH[h].key == key8(R); h++) {
                long s = (long)sH[h].pos - o;
                if (nc == cap) { cap = cap ? cap * 2 : 4096; c = realloc(c, cap * sizeof(*c)); }
                c[nc].src = s; c[nc].votes = 1; nc++;
                if (nc > 2000000) break;
            }
        }
    }
    if (nc) {
        size_t w = 0;
        qsort(c, nc, sizeof(*c), src_cmp);
        for (i = 0; i < nc; i++) {
            if (w && c[w - 1].src == c[i].src) c[w - 1].votes++;
            else c[w++] = c[i];
        }
        nc = w;
        qsort(c, nc, sizeof(*c), cand_cmp);
        for (i = 0; i < nc && i < 64; i++) {
            long s;
            if (!src_ok(c[i].src, n)) continue;
            s = score(pack, packN, c[i].src, N, n);
            if (s > bs || (s == bs && c[i].src < best)) { bs = s; best = c[i].src; }
        }
    }
    free(c);
    c = NULL; nc = cap = 0;
    {
        long nz = 0, bw = best >= 0 ? score_words(pack, packN, best, N, n) : -1;
        for (o = 0; o + 4 <= n; o += 4) if (N[o] | N[o + 1] | N[o + 2] | N[o + 3]) nz++;
        if (bw < nz) {
            /* 4-byte keys from the words that are not pointers into this executable */
            int words = 0;
            for (o = 0; o + 4 <= n && words < 24; o += 4) {
                uint32_t w = le32(N + o);
                int p;
                if (w == 0 || (w >= sPtrLo && w < sPtrHi)) continue;
                words++;
                for (p = 0; p < 24; p++) {
                    unsigned char R[4];
                    uint32_t k;
                    size_t h, hits = 0;
                    int q;
                    for (q = 0; q < 4; q++) R[sP4[p][q]] = N[o + q];
                    memcpy(&k, R, 4);
                    for (h = h4_lower(k); h < sH4N && sH4[h].key == k && hits < 4096; h++, hits++) {
                        if (nc == cap) { cap = cap ? cap * 2 : 4096; c = realloc(c, cap * sizeof(*c)); }
                        c[nc].src = (long)sH4[h].pos - o; c[nc].votes = 1; nc++;
                    }
                }
            }
            if (nc) {
                size_t w = 0;
                qsort(c, nc, sizeof(*c), src_cmp);
                for (i = 0; i < nc; i++) {
                    if (w && c[w - 1].src == c[i].src) c[w - 1].votes++;
                    else c[w++] = c[i];
                }
                nc = w;
                qsort(c, nc, sizeof(*c), cand_cmp);
                for (i = 0; i < nc && i < 256; i++) {
                    long s2;
                    if (!src_ok(c[i].src, n)) continue;
                    s2 = score_words(pack, packN, c[i].src, N, n);
                    if (s2 > bw || (s2 == bw && best >= 0 && c[i].src < best && 0)) { bw = s2; best = c[i].src; bs = s2; }
                }
            }
            free(c);
        }
    }
    *bestScore = bs;
    return best;
}

int main(int argc, char** argv) {
    long packN, embN, romN;
    unsigned char *pack, *emb, *zbuf;
    uint32_t base, stamp, packId, embOpsStart;
    FILE *f, *out;
    char line[512];
    int i;
    int parts[8];
    uLongf zn;
    long totalNative = 0;
    static unsigned char hdr[64];
    (void)romN;

    if (argc != 11) { fprintf(stderr, "usage: see the header of hh_recipes.c\n"); return 2; }
    load_perms();
    (void)parts;

    /* entries */
    f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    while (fgets(line, sizeof(line), f)) {
        Entry e;
        memset(&e, 0, sizeof(e));
        if (sscanf(line, " { 0x%X, 0x%X, 0x%X, 0x%X, 0x%X, 0x%X, \"%63[^\"]\" }", &e.vs, &e.ve, &e.ovs, &e.ove, &e.flags, &e.fo, e.name) == 7) {
            if (sNE == MAX_ENTRIES) { fprintf(stderr, "too many entries\n"); return 1; }
            sE[sNE++] = e;
        }
    }
    fclose(f);
    pack = slurp(argv[3], &packN);
    emb = slurp(argv[4], &embN);
    base = strtoul(argv[6], NULL, 0);
    sPtrLo = strtoul(argv[7], NULL, 0);
    sPtrHi = strtoul(argv[8], NULL, 0);
    stamp = strtoul(argv[9], NULL, 0);
    embN -= 8; /* stamp + HHEB trailer */
    if (le32(emb + embN) != stamp || (memcmp(emb + embN + 4, "HHEB", 4) && memcmp(emb + embN + 4, "FKEB", 4))) { fprintf(stderr, "hh_recipes: embedded.bin stamp != %08X\n", stamp); return 1; }
    if (packN != (long)(sE[sNE - 1].fo + sE[sNE - 1].ve - sE[sNE - 1].vs)) { fprintf(stderr, "hh_recipes: pack size mismatch\n"); return 1; }

    /* symbols in the region */
    f = fopen(argv[5], "r");
    if (!f) { perror(argv[5]); return 1; }
    while (fgets(line, sizeof(line), f)) {
        Sym s;
        char t[8];
        if (sscanf(line, "%x %x %7s %95s", &s.addr, &s.size, t, s.name) == 4 && s.addr >= base && s.addr + s.size <= base + (uint32_t)embN && s.size) {
            if (sNS == MAX_SYMS) { fprintf(stderr, "too many symbols\n"); return 1; }
            sS[sNS++] = s;
        }
    }
    fclose(f);

    /* ---- pack: every raw entry is its ROM file; ops for native entries ---- */
    for (i = 0; i < sNE; i++) {
        Entry* e = &sE[i];
        long n = e->ve - e->vs, rn;
        char p[600];
        unsigned char* raw;
        snprintf(p, sizeof(p), "%s/%s", argv[2], e->name);
        raw = slurp(p, &rn);
        if (rn != n) { fprintf(stderr, "hh_recipes: %s is %ld bytes, pack entry %ld\n", e->name, rn, n); return 1; }
        if (e->flags == 0) {
            if (memcmp(raw, pack + e->fo, n)) { fprintf(stderr, "hh_recipes: raw entry %s differs from its ROM file\n", e->name); return 1; }
        } else {
            encode(raw, pack + e->fo, n, e->name);
            totalNative += n;
        }
        free(raw);
        /* the gaps between entries (16-byte alignment) must be zero */
        if (i + 1 < sNE) {
            long g;
            for (g = e->fo + n; g < (long)sE[i + 1].fo; g++) if (pack[g]) { fprintf(stderr, "hh_recipes: nonzero pad after %s\n", e->name); return 1; }
        }
    }
    fprintf(stderr, "hh_recipes: pack %d entries, %ld native bytes: %ld blocks, %ld zero, %ld split (%ld vrom, %ld ptr, %ld other literal)\n",
            sNE, totalNative, sBlocks, sZero, sSplit, sVrom, sLitPtr, sLitOther);
    packId = (uint32_t)crc32(crc32(0, sOps, sOpsN), (const unsigned char*)sE, sizeof(Entry) * sNE);
    embOpsStart = sOpsN;

    /* ---- embedded region ---- */
    {
        /* hash raw source entries */
        size_t cap = 0;
        long litBefore = sLitOther, ptrBefore = sLitPtr, covered = 0, o;
        unsigned char* cov = calloc(embN, 1);
        for (i = 0; i < sNE; i++) {
            long n = sE[i].ve - sE[i].vs, o2;
            if (!is_src_entry(&sE[i])) continue;
            for (o2 = 0; o2 + 8 <= n; o2 += 2) {
                if (sHN == cap) { cap = cap ? cap * 2 : 1 << 20; sH = realloc(sH, cap * sizeof(*sH)); }
                sH[sHN].key = key8(pack + sE[i].fo + o2);
                sH[sHN].pos = sE[i].fo + o2;
                sHN++;
            }
        }
        qsort(sH, sHN, sizeof(*sH), he_cmp);
        {
            size_t cap4 = 0;
            for (i = 0; i < sNE; i++) {
                long n = sE[i].ve - sE[i].vs, o2;
                if (!is_src_entry(&sE[i])) continue;
                for (o2 = 0; o2 + 4 <= n; o2 += 2) {
                    if (sH4N == cap4) { cap4 = cap4 ? cap4 * 2 : 1 << 20; sH4 = realloc(sH4, cap4 * sizeof(*sH4)); }
                    memcpy(&sH4[sH4N].key, pack + sE[i].fo + o2, 4);
                    sH4[sH4N].pos = sE[i].fo + o2;
                    sH4N++;
                }
            }
            qsort(sH4, sH4N, sizeof(*sH4), he4_cmp);
        }
        for (i = 0; i < sNS; i++) {
            Sym* s = &sS[i];
            long off = s->addr - base, sc, src;
            src = find_source(pack, packN, emb + off, s->size, &sc);
            if (src < 0 || sc == 0) {
                fprintf(stderr, "  nosource %s %u bytes score %ld: ", s->name, s->size, sc);
                { long q; for (q = 0; q < (long)s->size && q < 32; q += 4) fprintf(stderr, "%08X ", le32(emb + off + q)); }
                fprintf(stderr, "\n");
                sNoSrc++;
            }
            if (src < 0) {
                /* no ROM match (a struct of pointers and small counts: animation and
                 * skeleton headers whose ROM copy points at N64 addresses): every word
                 * from a zero source, so each one is a pointer or a counted literal */
                unsigned char* zsrc = calloc(s->size + 8, 1);
                op32(off); op32(s->size); op32(0xFFFFFFFFu);
                encode(zsrc, emb + off, s->size, s->name);
                free(zsrc);
            } else {
                op32(off); op32(s->size); op32(src);
                encode(pack + src, emb + off, s->size, s->name);
            }
            memset(cov + off, 1, s->size);
        }
        for (o = 0; o < embN; o++) {
            if (cov[o]) covered++;
            else if (emb[o]) { fprintf(stderr, "hh_recipes: region byte +0x%lX is outside every symbol and not zero\n", o); return 1; }
        }
        fprintf(stderr, "hh_recipes: region %ld bytes, %d symbols cover %ld; literals %ld ptr, %ld other\n",
                embN, sNS, covered, sLitPtr - ptrBefore, sLitOther - litBefore);
        free(cov);
    }
    if (sLitOther > LITERAL_BUDGET) { fprintf(stderr, "hh_recipes: %ld non-pointer literals > budget %d\n", sLitOther, LITERAL_BUDGET); return 1; }

    /* ---- round trip: decode every unit with the device's decoder and compare ---- */
    {
        const unsigned char* c = sOps;
        const unsigned char* end = sOps + sOpsN;
        long bad = 0;
        for (i = 0; i < sNE; i++) {
            long n = sE[i].ve - sE[i].vs, rn;
            char p[600];
            unsigned char *raw, *dst;
            if (sE[i].flags == 0) continue;
            snprintf(p, sizeof(p), "%s/%s", argv[2], sE[i].name);
            raw = slurp(p, &rn);
            dst = malloc(n + 8);
            if (!hhr_decode(&sT, raw, dst, n, &c, end, vrom_cb, NULL) || memcmp(dst, pack + sE[i].fo, n)) { fprintf(stderr, "hh_recipes: round trip failed for %s\n", sE[i].name); bad++; }
            free(raw); free(dst);
        }
        if ((uint32_t)(c - sOps) != embOpsStart) { fprintf(stderr, "hh_recipes: pack ops end at %ld, expected %u\n", (long)(c - sOps), embOpsStart); bad++; }
        for (i = 0; i < sNS && !bad; i++) {
            uint32_t off = hhr_le32(c), size = hhr_le32(c + 4), src = hhr_le32(c + 8);
            unsigned char *zsrc = NULL, *dst = malloc(size + 8);
            const unsigned char* R;
            c += 12;
            if (src == 0xFFFFFFFFu) { zsrc = calloc(size + 8, 1); R = zsrc; } else R = pack + src;
            if (!hhr_decode(&sT, R, dst, size, &c, end, vrom_cb, NULL) || memcmp(dst, emb + off, size)) { fprintf(stderr, "hh_recipes: round trip failed for %s\n", sS[i].name); bad++; }
            free(zsrc); free(dst);
        }
        if (!bad && c != end) { fprintf(stderr, "hh_recipes: %ld op bytes left over\n", (long)(end - c)); bad++; }
        if (bad) return 1;
        fprintf(stderr, "hh_recipes: round trip OK (pack natives + %d symbols)\n", sNS);
    }

    /* ---- write ---- */
    zn = compressBound(sOpsN);
    zbuf = malloc(zn);
    if (compress2(zbuf, &zn, sOps, sOpsN, 9) != Z_OK) { fprintf(stderr, "compress failed\n"); return 1; }
    out = fopen(argv[10], "wb");
    if (!out) { perror(argv[10]); return 1; }
    memcpy(hdr, "HHR1", 4);
    {
        uint32_t v[] = { 1, stamp, packId, (uint32_t)sNE, (uint32_t)packN, (uint32_t)embN, (uint32_t)sNS,
                         (uint32_t)sOpsN, embOpsStart, (uint32_t)zn };
        int k;
        for (k = 0; k < 10; k++) { hdr[4 + k * 4] = v[k] & 0xFF; hdr[5 + k * 4] = (v[k] >> 8) & 0xFF; hdr[6 + k * 4] = (v[k] >> 16) & 0xFF; hdr[7 + k * 4] = v[k] >> 24; }
    }
    fwrite(hdr, 1, 44, out);
    for (i = 0; i < sNE; i++) {
        uint32_t v[5] = { sE[i].ovs, sE[i].ove, sE[i].vs, sE[i].fo, sE[i].flags };
        unsigned char b[20];
        int k;
        for (k = 0; k < 5; k++) { b[k * 4] = v[k] & 0xFF; b[k * 4 + 1] = (v[k] >> 8) & 0xFF; b[k * 4 + 2] = (v[k] >> 16) & 0xFF; b[k * 4 + 3] = v[k] >> 24; }
        fwrite(b, 1, 20, out);
        {
            uint32_t sz = sE[i].ve - sE[i].vs;
            unsigned char c4[4] = { sz & 0xFF, (sz >> 8) & 0xFF, (sz >> 16) & 0xFF, sz >> 24 };
            fwrite(c4, 1, 4, out);
        }
    }
    fwrite(zbuf, 1, zn, out);
    {
        unsigned char t[8] = { stamp & 0xFF, (stamp >> 8) & 0xFF, (stamp >> 16) & 0xFF, stamp >> 24, 'H', 'H', 'R', 'C' };
        fwrite(t, 1, 8, out);
    }
    fclose(out);
    {
        /* the same content uncompressed, for the release gates' leak scan (zlib would
         * hide any ROM window from it): header, file table, ops */
        char p2[600];
        FILE* o2;
        snprintf(p2, sizeof(p2), "%s.ops", argv[10]);
        o2 = fopen(p2, "wb");
        if (!o2) { perror(p2); return 1; }
        fwrite(hdr, 1, 44, o2);
        for (i = 0; i < sNE; i++) {
            uint32_t v[6] = { sE[i].ovs, sE[i].ove, sE[i].vs, sE[i].fo, sE[i].flags, sE[i].ve - sE[i].vs };
            fwrite(v, 4, 6, o2);
        }
        fwrite(sOps, 1, sOpsN, o2);
        fclose(o2);
    }
    fprintf(stderr, "hh_recipes: %s: %zu op bytes -> %lu compressed, pack id %08X, stamp %08X\n", argv[10], sOpsN, (unsigned long)zn, packId, stamp);
    return 0;
}
