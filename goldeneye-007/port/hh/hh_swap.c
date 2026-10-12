/* hh_swap.c: data the game loads from the ROM, put into host byte order at load time.
 *
 * The N64 reads its big-endian ROM data in place; on a little-endian host every u16/u32/
 * f32/pointer field has to be swapped once, after the load and before the game's first
 * read (u8 fields and byte-oriented streams stay as they are). Each converter documents
 * the layout it assumes. The layouts were worked out with the decomp's struct definitions
 * and the GoldenEye PC port's notes (github.com/jkdansereau/goldeneye-pc-port, MIT: its
 * port/src/romdata.c and tools_pc/ emitters), minus that port's 64-bit re-layouts: on a
 * 32-bit host the N64 layouts hold as they are. */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hh_be.h"
#include "hh_os.h"
#include "hh_swap.h"

/* ------------------------------------------------------------------------------------ */
/* Animation data segment (initanitable.c alloc_load_expand_ani_table, after romCopy and
 * before expand_ani_table_entries turns the offsets into pointers).
 * Per animation, a 0x14-byte ModelAnimation header at the offset in animation_table_ptrs
 * (an entry of 1 = none):
 *   0x00 s32 ROM address of the frame data (offset in the animation_entries segment)
 *   0x04 u16 frame count, 0x06 u8, 0x07 u8 flags
 *   0x08 u32 offset of the ModelAnimBitField array, 0x0C u16, 0x0E u16
 *   0x10 u32 offset of the root-motion bitstream (the array ends there)
 * ModelAnimBitField (6 bytes): u16 bitOffset, u8 bitCount, u8 pad, u16 valueOffset.
 * The bitstreams are read a byte at a time, MSB first: no swap. The per-frame data
 * streamed from the ROM later (animation_entries) is the same kind of bitstream. */

static u8 g_anim_done[0x10000 / 32];

static void hh_swap_anim_table(u8 *blob, u32 size, const s32 *table)
{
    for (; *table != 0; table++) {
        u32 off = (u32)*table, bd, bs, p;
        u8 *rec;
        if (off == 1)
            continue;
        if (off + 0x14 > size || (off & 3)) {
            fprintf(stderr, "[hh_swap] animation record offset %x out of range\n", off);
            continue;
        }
        if (off / 4 < sizeof(g_anim_done) * 8) {
            if (g_anim_done[off / 32] & (1 << ((off / 4) & 7)))
                continue; /* listed twice: already converted */
            g_anim_done[off / 32] |= 1 << ((off / 4) & 7);
        }
        rec = blob + off;
        hh_sw32(rec, 0x00);
        hh_sw16(rec, 0x04);
        hh_sw32(rec, 0x08);
        hh_sw16(rec, 0x0C);
        hh_sw16(rec, 0x0E);
        hh_sw32(rec, 0x10);
        bd = *(u32 *)(rec + 0x08);
        bs = *(u32 *)(rec + 0x10);
        if (bd == 0 && bs == 0)
            continue;
        if (!(bd < bs && bs <= size && (bs - bd) % 6 == 0)) {
            fprintf(stderr, "[hh_swap] animation %x: bad descriptor range %x..%x\n", off, bd, bs);
            continue;
        }
        for (p = bd; p < bs; p += 6) {
            hh_sw16(blob, p);
            hh_sw16(blob, p + 4);
        }
    }
}

void hh_swap_animation_data(void *blob, u32 size, const s32 *table1, const s32 *table2)
{
    memset(g_anim_done, 0, sizeof(g_anim_done));
    hh_swap_anim_table(blob, size, table1);
    hh_swap_anim_table(blob, size, table2);
}

/* ------------------------------------------------------------------------------------ */
/* Sound banks (music.c musicSeqPlayerInit: sfx.ctl and instruments.ctl, after romCopy and
 * before alBnkfNew). libaudio's ALBankFile tree with file-relative u32 offsets where the
 * structs have pointers (the same size on a 32-bit host), walked as alBnkfNew walks it:
 *   ALBankFile   s16 revision, s16 bankCount, u32 bankArray[bankCount]
 *   ALBank       s16 instCount, u8 flags, u8 pad, s32 sampleRate, u32 percussion,
 *                u32 instArray[instCount]
 *   ALInstrument 12 x u8, s16 bendRange, s16 soundCount, u32 soundArray[soundCount]
 *   ALSound      u32 envelope, u32 keyMap, u32 wavetable, u8 pan, u8 volume, u8 flags
 *   ALEnvelope   s32 attack/decay/release times, u8, u8        ALKeyMap: 6 x u8
 *   ALWaveTable  u32 base (offset in .tbl), s32 len, u8 type, u8 flags, u32 loop,
 *                u32 book (ADPCM) -- the RAW16 union has only the loop
 *   ALADPCMloop  u32 start, end, count, s16 state[16]           ALRawLoop: 3 x u32
 *   ALADPCMBook  s32 order, s32 npredictors, s16 book[order * npredictors * 8]
 * Envelopes, key maps, books and loops can be shared: each is converted once. */

#include <stdlib.h>

typedef struct {
    u8 *base;
    u32 size;
    u8 *seen; /* one bit per byte offset: struct already converted */
} HhBank;

static int hh_bank_first(HhBank *b, u32 off, u32 len)
{
    if (off == 0 || off + len > b->size) {
        if (off != 0)
            fprintf(stderr, "[hh_swap] sound bank: offset %x+%x past the end (%x)\n", off, len, b->size);
        return 0;
    }
    if (b->seen[off >> 3] & (1 << (off & 7)))
        return 0;
    b->seen[off >> 3] |= 1 << (off & 7);
    return 1;
}

static void hh_bank_wavetable(HhBank *b, u32 off)
{
    u8 *w = b->base + off;
    u32 loop, book;
    if (!hh_bank_first(b, off, 20))
        return;
    hh_sw32(w, 0);
    hh_sw32(w, 4);
    hh_sw32(w, 12);
    loop = *(u32 *)(w + 12);
    if (w[8] == AL_ADPCM_WAVE) {
        hh_sw32(w, 16);
        book = *(u32 *)(w + 16);
        if (hh_bank_first(b, book, 8)) {
            u8 *p = b->base + book;
            u32 i, n;
            hh_sw32(p, 0);
            hh_sw32(p, 4);
            n = *(s32 *)(p + 0) * *(s32 *)(p + 4) * 8;
            if (8 + n * 2 > b->size - book)
                fprintf(stderr, "[hh_swap] sound bank: ADPCM book at %x too long\n", book);
            else
                for (i = 0; i < n; i++)
                    hh_sw16(p, 8 + i * 2);
        }
        if (hh_bank_first(b, loop, 44)) {
            u8 *p = b->base + loop;
            int i;
            hh_sw32(p, 0);
            hh_sw32(p, 4);
            hh_sw32(p, 8);
            for (i = 0; i < 16; i++)
                hh_sw16(p, 12 + i * 2);
        }
    } else if (w[8] == AL_RAW16_WAVE) {
        if (hh_bank_first(b, loop, 12)) {
            u8 *p = b->base + loop;
            hh_sw32(p, 0);
            hh_sw32(p, 4);
            hh_sw32(p, 8);
        }
    }
}

static void hh_bank_sound(HhBank *b, u32 off)
{
    u8 *s = b->base + off;
    if (!hh_bank_first(b, off, 15))
        return;
    hh_sw32(s, 0);
    hh_sw32(s, 4);
    hh_sw32(s, 8);
    if (hh_bank_first(b, *(u32 *)(s + 0), 14)) {
        u8 *e = b->base + *(u32 *)(s + 0);
        hh_sw32(e, 0);
        hh_sw32(e, 4);
        hh_sw32(e, 8);
    }
    hh_bank_wavetable(b, *(u32 *)(s + 8));
}

static void hh_bank_instrument(HhBank *b, u32 off)
{
    u8 *in = b->base + off;
    s32 i, n;
    if (!hh_bank_first(b, off, 16))
        return;
    hh_sw16(in, 12);
    hh_sw16(in, 14);
    n = *(s16 *)(in + 14);
    if (n < 0 || off + 16 + (u32)n * 4 > b->size) {
        fprintf(stderr, "[hh_swap] sound bank: instrument at %x has %d sounds\n", off, n);
        return;
    }
    for (i = 0; i < n; i++) {
        hh_sw32(in, 16 + i * 4);
        hh_bank_sound(b, *(u32 *)(in + 16 + i * 4));
    }
}

void hh_swap_audio_bank(void *ctl, u32 size)
{
    HhBank b;
    s32 i, j, nbanks;
    u8 *f = ctl;
    b.base = ctl;
    b.size = size;
    b.seen = calloc((size + 7) / 8, 1);
    hh_sw16(f, 0);
    hh_sw16(f, 2);
    nbanks = *(s16 *)(f + 2);
    for (i = 0; i < nbanks && 4 + (u32)i * 4 + 4 <= size; i++) {
        u32 boff;
        u8 *bank;
        s32 ninst;
        hh_sw32(f, 4 + i * 4);
        boff = *(u32 *)(f + 4 + i * 4);
        if (!hh_bank_first(&b, boff, 12))
            continue;
        bank = f + boff;
        hh_sw16(bank, 0);
        hh_sw32(bank, 4);
        hh_sw32(bank, 8);
        ninst = *(s16 *)(bank + 0);
        hh_bank_instrument(&b, *(u32 *)(bank + 8));
        for (j = 0; j < ninst && boff + 12 + (u32)j * 4 + 4 <= size; j++) {
            hh_sw32(bank, 12 + j * 4);
            hh_bank_instrument(&b, *(u32 *)(bank + 12 + j * 4));
        }
    }
    free(b.seen);
}

/* ------------------------------------------------------------------------------------ */
/* Music sequence table (music.c: RareALSeqBankFile, after each romCopy): u16 seqCount,
 * u16 unused, then 8-byte entries {u32 offset, u16 uncompressed_len, u16 len}. The
 * header-only copy converts the entries that fit. */
void hh_swap_music_table(void *blob, u32 size)
{
    u8 *p = blob;
    u32 i, n;
    if (size < 4)
        return;
    hh_sw16(p, 0);
    hh_sw16(p, 2);
    n = *(u16 *)p;
    for (i = 0; i < n && 4 + i * 8 + 8 <= size; i++) {
        hh_sw32(p, 4 + i * 8);
        hh_sw16(p, 4 + i * 8 + 4);
        hh_sw16(p, 4 + i * 8 + 6);
    }
}

/* Compressed MIDI sequence (music.c, after decompressdata, before alCSeqNew): the
 * ALCMidiHdr is 16 u32 track offsets + u32 division; the rest is a byte stream. */
void hh_swap_cseq_header(void *seq)
{
    int i;
    for (i = 0; i < 17; i++)
        hh_sw32(seq, i * 4);
}

/* ------------------------------------------------------------------------------------ */
/* Display lists and vertices loaded from the ROM (F3D plus Rare's G_TRI4 and G_SETTEX).
 * Every command is two u32 words, swapped in place, so the renderer and the game read
 * host-order Gfx; game code that inspected command bytes uses hh_gbi_le.h accessors
 * instead. A list is walked to G_ENDDL or a branch (G_DL without push); vertices named
 * by G_VTX and lists called with G_DL inside the same blob are converted as they are
 * found. Vtx (16 bytes): s16 ob[3], u16 flag, s16 tc[2], u8 cn[4]. Everything is
 * converted once (the blob keeps a bitmap per 8 bytes). */

#define HH_G_VTX 0x04
#define HH_G_DL 0x06
#define HH_G_ENDDL 0xB8

void hh_gfx_blob_init(HhGfxBlob *b, void *base, u32 size, s32 seg, u32 segoff)
{
    b->base = base;
    b->size = size;
    b->seg = seg;
    b->segoff = segoff;
    b->seen = calloc((size + 63) / 64, 1);
}

void hh_gfx_blob_free(HhGfxBlob *b)
{
    free(b->seen);
    b->seen = NULL;
}

/* Blob offset of a segment address (or -1 if it points elsewhere). */
static s32 hh_gfx_blob_off(HhGfxBlob *b, u32 addr)
{
    u32 off;
    if (b->seg < 0 || ((addr >> 24) & 0x0F) != (u32)b->seg)
        return -1;
    off = (addr & 0x00FFFFFF) - b->segoff;
    return off < b->size ? (s32)off : -1;
}

static int hh_gfx_first(HhGfxBlob *b, u32 off)
{
    u32 u = off >> 3;
    if (b->seen[u >> 3] & (1 << (u & 7)))
        return 0;
    b->seen[u >> 3] |= 1 << (u & 7);
    return 1;
}

void hh_gfx_blob_vtx(HhGfxBlob *b, u32 off, u32 n)
{
    u32 i;
    for (i = 0; i < n && off + 16 <= b->size; i++, off += 16) {
        u8 *v = b->base + off;
        if (!hh_gfx_first(b, off))
            continue;
        hh_sw16(v, 0);
        hh_sw16(v, 2);
        hh_sw16(v, 4);
        hh_sw16(v, 6);
        hh_sw16(v, 8);
        hh_sw16(v, 10);
        /* the second 8-byte unit of the vertex: mark it too */
        hh_gfx_first(b, off + 8);
    }
}

void hh_gfx_blob_dl(HhGfxBlob *b, u32 off)
{
    int depth = 0;
    while (off + 8 <= b->size && (off & 7) == 0) {
        u32 *w = (u32 *)(b->base + off);
        u32 cmd;
        if (!hh_gfx_first(b, off))
            return; /* already converted from here on */
        w[0] = hh_bswap32(w[0]);
        w[1] = hh_bswap32(w[1]);
        cmd = w[0] >> 24;
        if (cmd == HH_G_ENDDL)
            return;
        if (cmd == HH_G_VTX) {
            s32 v = hh_gfx_blob_off(b, w[1]);
            if (v >= 0)
                hh_gfx_blob_vtx(b, (u32)v, ((w[0] >> 20) & 0xF) + 1);
        } else if (cmd == HH_G_DL) {
            s32 t = hh_gfx_blob_off(b, w[1]);
            if (t >= 0 && depth < 16) {
                depth++;
                hh_gfx_blob_dl(b, (u32)t);
                depth--;
            }
            if (((w[0] >> 16) & 0xFF) == 1) /* G_DL_NOPUSH: a branch ends this list */
                return;
        }
        off += 8;
    }
}

/* ------------------------------------------------------------------------------------ */
/* The global image segment (image_bank.c texReset, after romCopy): 17 display lists,
 * then the s_*image tables of sImageTableEntry {u32 index; 8 x u8}. The segment is
 * linked at 0x02000000 and its lists use segment 2 addresses. */
void hh_swap_global_images(void *seg, u32 size, const u32 *dls, s32 ndls, const u32 *tables, s32 ntables)
{
    HhGfxBlob b;
    s32 i;
    hh_gfx_blob_init(&b, seg, size, 2, 0);
    for (i = 0; i < ndls; i++)
        hh_gfx_blob_dl(&b, dls[i]);
    hh_gfx_blob_free(&b);
    for (i = 0; i < ntables; i++) {
        u32 start = tables[i], stop = (i + 1 < ntables) ? tables[i + 1] : size, p;
        for (p = start; p + 12 <= stop; p += 12)
            hh_sw32(seg, p);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Decoded textures (image.c): the decoders build texels as host u16/u32 values, and mip
 * levels are averaged from them; each finished level is then stored as the N64 has it
 * in memory (big-endian), which is what the renderer's importers read. Rows are padded
 * to 4 texels for 16- and 32-bit formats. 4- and 8-bit texels are bytes already. */
void hh_tex_level_to_be(u8 *p, s32 width, s32 height, s32 format)
{
    u32 n = (u32)((width + 3) & ~3) * (u32)height, i;
    switch (format) {
    case 0: /* TEXFORMAT_RGBA32 */
    case 2: /* TEXFORMAT_RGB24 */
        for (i = 0; i < n; i++)
            hh_sw32(p, i * 4);
        break;
    case 1: /* TEXFORMAT_RGBA16 */
    case 3: /* TEXFORMAT_RGB15 */
    case 4: /* TEXFORMAT_IA16 */
        for (i = 0; i < n; i++)
            hh_sw16(p, i * 2);
        break;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Model files (objecthandler_2.c load_object_fill_header, after the file is inflated and
 * before sub_GAME_7F075A90 turns its segment-5 offsets into pointers). Layout:
 *   u32 Switches[numSwitches]            node offsets
 *   ModelFileTextures[numTextures]       u32 TextureID + 8 bytes
 *   ModelNode tree from here             u16 Opcode, pad, u32 Data/Parent/Next/Prev/Child
 * plus the records, display lists, vertices and embedded images they point at. The tree
 * is walked as the game walks it: LOD (8) and switch (18) nodes lead on through their
 * Affects/Controls node, not Child. Record layouts per opcode are the decomp's
 * ModelRoData_* structs; fields read through a union get the type the game reads them
 * as (the header record's +8 is GroupsAsF32 in getjointsize). Display lists and their
 * vertices go through the shared Gfx converter; collision vertices have their own
 * layout: s16 x[3], s16 index, u32 LinkedTo, s16, s16. Embedded images stay bytes. */

typedef struct {
    HhGfxBlob g;       /* display lists, display vertices (8-byte bitmap) */
    u8 *claimed;       /* records/tables converted (1 bit per byte) */
    u32 size;
    const char *name;
    int errors;
} HhModel;

static int g_mtrace; /* GE_SWAPTRACE (set in hh_swap_model) */
static u32 hh_moff(u32 ptr) { return ptr & 0x00FFFFFF; }

static int hh_mclaim(HhModel *m, u32 off, u32 len)
{
    u32 i;
    if (off + len > m->size) {
        if (m->errors++ < 8)
            fprintf(stderr, "[hh_swap] model %s: %x+%x out of range (%x)\n", m->name, off, len, m->size);
        return 0;
    }
    for (i = off; i < off + len; i++)
        if (m->claimed[i >> 3] & (1 << (i & 7)))
            return 0;
    for (i = off; i < off + len; i++)
        m->claimed[i >> 3] |= 1 << (i & 7);
    return 1;
}

/* fields: "2" = u16/s16, "4" = u32/s32/f32/pointer, "1" = byte, each consecutive */
static void hh_mfields(u8 *p, const char *f)
{
    u32 o = 0;
    for (; *f; f++) {
        if (*f == '2')
            hh_sw16(p, o), o += 2;
        else if (*f == '4')
            hh_sw32(p, o), o += 4;
        else
            o += 1;
    }
}

static void hh_mcollision_vtx(HhModel *m, u32 off, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++, off += 16) {
        u8 *v;
        if (off + 16 > m->size || !hh_gfx_first(&m->g, off))
            continue;
        hh_gfx_first(&m->g, off + 8);
        v = m->g.base + off;
        hh_mfields(v, "2222422");
    }
}

static void hh_mdl(HhModel *m, u32 ptr)
{
    if (ptr != 0 && hh_moff(ptr) < m->size)
        hh_gfx_blob_dl(&m->g, hh_moff(ptr));
}

static void hh_mvtx(HhModel *m, u32 ptr, s32 n)
{
    if (ptr != 0 && n > 0)
        hh_gfx_blob_vtx(&m->g, hh_moff(ptr), (u32)n);
}

/* Convert the record of a node; returns the extra node to walk into (LOD/switch), or 0. */
static u32 hh_mrecord(HhModel *m, u32 op, u32 d, u32 *extra2, u32 *extra3)
{
    u8 *r = m->g.base + d;
    *extra2 = *extra3 = 0;
    switch (op) {
    case 1: /* header */
        if (hh_mclaim(m, d, 0x10)) hh_mfields(r, "224422");
        return 0;
    case 2: case 3: /* group */
        if (hh_mclaim(m, d, 0x1C)) hh_mfields(r, "444222244");
        return 0;
    case 4: /* display list */
        if (hh_mclaim(m, d, 0x14)) {
            hh_mfields(r, "44442");
            hh_mdl(m, *(u32 *)(r + 0));
            hh_mdl(m, *(u32 *)(r + 4));
            hh_mvtx(m, *(u32 *)(r + 0xC), *(u16 *)(r + 0x10));
        }
        return 0;
    case 8: /* LOD */
        if (hh_mclaim(m, d, 0x10)) hh_mfields(r, "44422");
        return *(u32 *)(r + 8);
    case 9: /* BSP */
        if (hh_mclaim(m, d, 0x24)) hh_mfields(r, "4444444422");
        *extra2 = *(u32 *)(r + 0x18);
        *extra3 = *(u32 *)(r + 0x1C);
        return 0;
    case 10: /* bounding box */
        if (hh_mclaim(m, d, 0x1C)) hh_mfields(r, "4444444");
        return 0;
    case 11:
        if (hh_mclaim(m, d, 0x4C)) hh_mfields(r, "44444444444444444224");
        return 0;
    case 12: /* gunfire */
        if (hh_mclaim(m, d, 0x28)) hh_mfields(r, "44444444224");
        return 0;
    case 13: /* shadow */
        if (hh_mclaim(m, d, 0x20)) hh_mfields(r, "44444444");
        return 0;
    case 14:
        if (hh_mclaim(m, d, 0x10)) hh_mfields(r, "4444");
        return 0;
    case 15: /* interlinkage */
        if (hh_mclaim(m, d, 0x1C)) hh_mfields(r, "4444444");
        return 0;
    case 16:
        if (hh_mclaim(m, d, 0x18)) hh_mfields(r, "44422224");
        return 0;
    case 17:
        if (hh_mclaim(m, d, 0x20)) hh_mfields(r, "44444444");
        return 0;
    case 18: /* switch */
        if (g_mtrace)
            fprintf(stderr, "[hh_swap]   switch rodata %x: %02x%02x%02x%02x %02x%02x%02x%02x\n", d, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        if (hh_mclaim(m, d, 0x08)) hh_mfields(r, "422");
        return *(u32 *)(r + 0);
    case 21: /* group simple */
        if (hh_mclaim(m, d, 0x14)) hh_mfields(r, "444224");
        return 0;
    case 22: /* primary display list: a muzzle flash's rotating quads (model.c dorottex) */
        if (hh_mclaim(m, d, 0x10)) {
            hh_mfields(r, "4444");
            hh_mdl(m, *(u32 *)(r + 8));
            /* numVertices counts quads: dorottex copies 4 vertices each. Only the first quarter
             * was converted, the rest read big-endian -- flash quads thousands of units wide,
             * the AR33's (and every multi-quad flash) a yellow wash over the screen (round 17). */
            hh_mvtx(m, *(u32 *)(r + 4), *(s32 *)(r + 0) * 4);
        }
        return 0;
    case 23: /* head placeholder */
        if (hh_mclaim(m, d, 0x02)) hh_mfields(r, "2");
        return 0;
    case 24: /* display list + collision */
        if (hh_mclaim(m, d, 0x20)) {
            s32 nv, ncv, i;
            hh_mfields(r, "4442244224");
            hh_mdl(m, *(u32 *)(r + 0));
            hh_mdl(m, *(u32 *)(r + 4));
            nv = *(s16 *)(r + 0xC);
            ncv = *(s16 *)(r + 0xE);
            hh_mvtx(m, *(u32 *)(r + 8), nv);
            if (*(u32 *)(r + 0x10) != 0 && ncv > 0)
                hh_mcollision_vtx(m, hh_moff(*(u32 *)(r + 0x10)), (u32)ncv);
            if (*(u32 *)(r + 0x14) != 0 && nv > 0 && hh_mclaim(m, hh_moff(*(u32 *)(r + 0x14)), nv * 2)) {
                u8 *pu = m->g.base + hh_moff(*(u32 *)(r + 0x14));
                for (i = 0; i < nv; i++)
                    hh_sw16(pu, i * 2);
            }
        }
        return 0;
    default:
        if (m->errors++ < 8)
            fprintf(stderr, "[hh_swap] model %s: unknown node opcode %u (record %x)\n", m->name, op, d);
        return 0;
    }
}

/* The game's walk (modelPromoteNodeOffsetsToPointers): each node, then its Child (LOD
 * nodes: their record's Affects), else the next sibling, else back up through parents.
 * Nodes come first so their fields are host order when the walk reads them. */
static u32 g_mroots[256];
static u32 g_mnroots;

static int g_mtrace = -1;

static void hh_mnode(HhModel *m, u32 o)
{
    u8 *n = m->g.base + o;
    u32 op, d, e1, e2, e3;
    if (!hh_mclaim(m, o, 24))
        return;
    if (g_mtrace)
        fprintf(stderr, "[hh_swap]   node %x: %02x %02x %02x %02x | data %02x%02x%02x%02x child %02x%02x%02x%02x next %02x%02x%02x%02x\n", o,
                n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[0x14], n[0x15], n[0x16], n[0x17], n[0xC], n[0xD], n[0xE], n[0xF]);
    hh_mfields(n, "2244444");
    op = *(u16 *)n & 0xFF;
    d = hh_moff(*(u32 *)(n + 4));
    e1 = hh_mrecord(m, op, d, &e2, &e3);
    if (op == 8) {
        /* the game replaces a LOD node's Child with Affects before following it */
        *(u32 *)(n + 0x14) = e1;
    } else if (op == 18 && e1 && g_mnroots < 256) {
        g_mroots[g_mnroots++] = hh_moff(e1);
    }
    if (e2 && g_mnroots < 256)
        g_mroots[g_mnroots++] = hh_moff(e2);
    if (e3 && g_mnroots < 256)
        g_mroots[g_mnroots++] = hh_moff(e3);
}

static void hh_mwalk(HhModel *m, u32 o)
{
    u32 guard = 0, limit = m->size / 4;
    for (;;) {
        u8 *n;
        u32 child;
        if (o + 24 > m->size || (o & 3) || guard++ > limit)
            return;
        hh_mnode(m, o);
        n = m->g.base + o;
        child = *(u32 *)(n + 0x14);
        if (child) {
            o = hh_moff(child);
            continue;
        }
        for (;;) {
            u32 next, parent;
            n = m->g.base + o;
            next = *(u32 *)(n + 0x0C);
            parent = *(u32 *)(n + 0x08);
            if (next) {
                o = hh_moff(next);
                break;
            }
            if (!parent)
                return;
            o = hh_moff(parent);
            if (o + 24 > m->size || guard++ > limit)
                return;
        }
    }
}

void hh_swap_model(void *file, u32 size, s32 numswitches, s32 numtextures, const char *name)
{
    HhModel m;
    u32 i, root, r;
    u8 *f = file;
    memset(&m, 0, sizeof(m));
    hh_gfx_blob_init(&m.g, file, size, 5, 0);
    m.claimed = calloc((size + 7) / 8, 1);
    m.size = size;
    m.name = name ? name : "?";
    g_mnroots = 0;
    if (g_mtrace < 0)
        g_mtrace = getenv("GE_SWAPTRACE") != NULL;
    root = 4 * (u32)numswitches + 12 * (u32)numtextures;
    if (g_mtrace)
        fprintf(stderr, "[hh_swap] model %s: %x bytes, %d switches, %d textures, root %x\n", m.name, size,
                (int)numswitches, (int)numtextures, root);
    if (root + 24 > size) {
        fprintf(stderr, "[hh_swap] model %s: no node table (%d switches, %d textures, %x bytes)\n",
                m.name, (int)numswitches, (int)numtextures, size);
        goto done;
    }
    for (i = 0; i < (u32)numswitches; i++)
        hh_sw32(f, i * 4);
    for (i = 0; i < (u32)numtextures; i++)
        hh_sw32(f, 4 * numswitches + 12 * i);
    hh_mwalk(&m, root);
    /* switch tables, switch Controls and BSP children not reached from the root */
    for (i = 0; i < (u32)numswitches; i++)
        if (*(u32 *)(f + i * 4) && g_mnroots < 256)
            g_mroots[g_mnroots++] = hh_moff(*(u32 *)(f + i * 4));
    for (r = 0; r < g_mnroots; r++) {
        u32 o = g_mroots[r];
        if (o + 24 <= size && !(m.claimed[o >> 3] & (1 << (o & 7))))
            hh_mwalk(&m, o);
    }
done:
    free(m.claimed);
    hh_gfx_blob_free(&m.g);
}

/* ------------------------------------------------------------------------------------ */
/* Fonts (textrelated.c load_font_tables, after romCopy and before the pixeldata offsets
 * become pointers): struct font = s32 kerning[13 * 13] + fontchar[94] {s32 index,
 * baseline, height, width, kerningindex; u8 *pixeldata}, all 32-bit; the glyph pixels
 * after them are 4-bit texels (bytes). */
void hh_swap_font(void *font, u32 size)
{
    u32 i, n = 13 * 13 + 94 * 6;
    if (n * 4 > size) {
        fprintf(stderr, "[hh_swap] font of %x bytes is too small\n", size);
        return;
    }
    for (i = 0; i < n; i++)
        hh_sw32(font, i * 4);
}

/* ------------------------------------------------------------------------------------ */
/* Text banks (language.c, after each bank load): u32 string offsets from the bank start,
 * then the strings (bytes). The table has no count: it ends where the first string
 * begins, i.e. at the smallest non-zero offset; a zero entry is a string not in this
 * version ("not loaded"). */
void hh_swap_text_bank(void *bank, u32 size)
{
    u32 i, first = size;
    u8 *b = bank;
    if (bank == NULL)
        return;
    for (i = 0; i * 4 + 4 <= size && i * 4 < first; i++) {
        u32 v;
        hh_sw32(b, i * 4);
        v = *(u32 *)(b + i * 4);
        if (v != 0 && v < first)
            first = v;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Mission briefing files (Ubrief*Z, front.c load_briefing_text_for_stage): u16 brief[4]
 * (the text ids of the overview, M, Q and Moneypenny pages), then OBJECTIVES_MAX pairs of
 * u16 text id, u16 lowest difficulty. All u16: the first 'size' bytes, up to 48. */
void hh_swap_briefing(void *file, u32 size)
{
    u32 o;
    if (file == NULL)
        return;
    if (size > 48)
        size = 48;
    for (o = 0; o + 2 <= size; o += 2)
        hh_sw16(file, o);
}

/* ------------------------------------------------------------------------------------ */
/* A blob of display lists at known offsets, addressed through segment 'seg' (the setup
 * lists in the font_dl segment: dlBasicGeometry 0x00, dlZBufferGeometry 0x20,
 * dlFastPipelineSetup 0x40, called through segment 1). */
void hh_swap_dl_blob(void *base, u32 size, s32 seg, const u32 *starts, s32 n)
{
    HhGfxBlob b;
    s32 i;
    hh_gfx_blob_init(&b, base, size, seg, 0);
    for (i = 0; i < n; i++)
        hh_gfx_blob_dl(&b, starts[i]);
    hh_gfx_blob_free(&b);
}

/* ------------------------------------------------------------------------------------ */
/* Level geometry (bg.c). Whole words / Gfx commands: a plain 32-bit swap of each word. */
void hh_swap_u32_array(void *p, u32 bytes)
{
    u32 i;
    for (i = 0; i + 4 <= bytes; i += 4)
        hh_sw32(p, i);
}

/* Vtx arrays (a room's point table after bgDecompress): s16 x, y, z, u16 flag, s16 s, t,
 * then the four colour bytes, which stay as they are. */
void hh_swap_vtx_array(void *p, u32 bytes)
{
    u32 i, k;
    for (i = 0; i + 16 <= bytes; i += 16)
        for (k = 0; k < 12; k += 2)
            hh_sw16(p, i + k);
}

#define HH_BG_OFF(v) ((v) - 0x0F000000u) /* bg file addresses are segment 0x0F offsets */

/* The bg file's header block, [0, first point table), as load_bg_file loads it (layout:
 * the decomp's tools/bg_binary_to_c.py and bg.h):
 *   0x00 u32 0; 0x04 room table; 0x08 portal table; 0x0C global visibility commands;
 *   0x10 a float table (none in the NTSC-U levels seen so far: fatal if present).
 *   Room table: 24-byte {u32 point table, u32 primary DL, u32 secondary DL, f32 x, y, z};
 *     entry 0 is all zero, the next all-zero entry ends it.
 *   Portal table: 8-byte {u32 portal, u8 room1, u8 room2, u8 ctrl1, u8 ctrl2} (bg.c pokes
 *     ctrl1 by byte offset: the bytes stay), ending with portal 0.
 *   Portal: {u8 numPoints, pad[3], f32 x, y, z per point}, shared between entries.
 *   Visibility commands: 8-byte records {u8 type, u8 length, u8 pad[2], s32 arg}; a command
 *     spans `length` records (its extra args in their `arg`), type 0 ends the list. */
void hh_swap_bg_header(void *data, u32 size)
{
    u8 *b = data, *seen;
    u32 i, off, rooms, portals, vis;
    if (size < 0x20)
        return;
    for (i = 0; i < 5; i++)
        hh_sw32(b, i * 4);
    rooms = *(u32 *)(b + 4);
    portals = *(u32 *)(b + 8);
    vis = *(u32 *)(b + 12);
    if (*(u32 *)(b + 16) != 0)
        hh_fatal("bg header: float table at %08x (not handled yet)", *(u32 *)(b + 16));
    for (i = 0, off = HH_BG_OFF(rooms); off + 24 <= size; i++, off += 24) {
        u32 k, any = 0;
        for (k = 0; k < 24; k += 4) {
            hh_sw32(b, off + k);
            any |= *(u32 *)(b + off + k);
        }
        if (i > 0 && any == 0)
            break;
    }
    seen = calloc(size / 4 + 1, 1);
    for (off = HH_BG_OFF(portals); off + 8 <= size; off += 8) {
        u32 p, n, k;
        hh_sw32(b, off);
        p = *(u32 *)(b + off);
        if (p == 0)
            break;
        p = HH_BG_OFF(p);
        if (p + 4 > size || seen == NULL || seen[p / 4])
            continue;
        seen[p / 4] = 1;
        n = b[p];
        for (k = 0; k < n * 3 && p + 4 + k * 4 + 4 <= size; k++)
            hh_sw32(b, p + 4 + k * 4);
    }
    free(seen);
    if (vis != 0) {
        off = HH_BG_OFF(vis);
        while (off + 8 <= size) {
            u32 type = b[off], len = b[off + 1] ? b[off + 1] : 1, k;
            for (k = 0; k < len && off + k * 8 + 8 <= size; k++)
                hh_sw32(b, off + k * 8 + 4);
            if (type == 0)
                break;
            off += len * 8;
        }
    }
}

/* Stan (collision tile) files, after the load and before stanDetermineEOF:
 *   u32 0, then the rooms' first-tile pointers up to a NULL, then tiles until a zero word.
 *   Tile: u32 id:24 + u8 room (room at byte 3 on both targets), s16 mid, s16 tail (bits
 *   12-15 = point count), then 8-byte points {s16 x, y, z, u16 link}; the size comes from
 *   list_of_tilesizes (at least 3 point slots). The id is only read by stanMatchTileName,
 *   through the tile seen as a point: a u16 at bytes 0-1 (id bits 23-8) and the byte at 2
 *   (bits 7-0). So bytes 0-1 swap as a u16 and bytes 2-3 stay; the `id` bit-field itself
 *   is never read. */
void hh_swap_stan(void *file, u32 size)
{
    static const u8 sizes[16] = { 0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58 };
    u8 *b = file;
    u32 off, p;
    if (file == NULL || size < 8)
        return;
    hh_sw32(b, 0);
    for (off = 4;; off += 4) {
        if (off + 4 > size)
            return;
        hh_sw32(b, off);
        if (*(u32 *)(b + off) == 0)
            break;
    }
    for (off += 4; off + 8 <= size;) {
        u32 n, tsz;
        if (*(u32 *)(b + off) == 0)
            break;
        hh_sw16(b, off); /* stanMatchTileName reads the id as a u16 (bytes 0-1) + a byte (2) */
        hh_sw16(b, off + 4);
        hh_sw16(b, off + 6);
        n = (*(u16 *)(b + off + 6) >> 12) & 0xF;
        tsz = sizes[n];
        if (tsz == 0 || off + tsz > size)
            hh_fatal("stan: bad tile at +%x (%u points)", off, n);
        for (p = off + 8; p < off + tsz; p += 2)
            hh_sw16(b, p);
        off += tsz;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Stage setup files (Usetup*Z; prop.c proplvreset2, right after the load). Layouts from
 * bondtypes.h and the GoldenEye PC port's d88_emit.py / d88_propdefs.py (ROM-verified
 * field kinds), without that port's 64-bit re-layout. Offsets are plain file offsets.
 *   header: 10 u32 offsets (pathwaypoints, waypointgroups, intro, propDefs, patrolpaths,
 *     ailists, pads, boundpads, padnames, boundpadnames).
 *   waypoint 16 B {s32 padID (<0 ends), u32 neighbours, s32 group, s32 dist};
 *   waygroup 12 B {u32 neighbours (0 ends), u32 waypoints, s32 dist};
 *   the lists they point to: s32 ids ending with -1.
 *   PathRecord 8 B {u32 waypoints (0 ends), u8 id, u8 loop, u16 len};
 *   AIListRecord 8 B {u32 ailist (0 ends), s32 id}: the AI byte code stays as it is.
 *   PadRecord 44 B {f32 pos, up, look [3], u32 plink (0 ends), u32 stan} and
 *   BoundPadRecord 68 B (+ f32 bbox[6]): plink names are strings.
 *   pad names: u32 offsets ending with 0.
 *   intro: s32-typed records (SetupIntro*), END = 9; the camera's lang1c is u16[2];
 *   the credits table (12 B entries of u16/s16, ending with two zero ids).
 *   propDefs: hh_swap_propdef per record, sizes from the game's sizepropdef. */
s32 sizepropdef(void *pdef); /* loadobjectmodel.c */

typedef struct {
    u8 *b;
    u32 size;
    u8 *seen; /* s32 lists already converted (shared between records) */
} HhSetup;

static int hh_setup_in(HhSetup *s, u32 off, u32 len) { return off >= 40 && off + len <= s->size; }

static void hh_setup_idlist(HhSetup *s, u32 off)
{
    if (off == 0 || (off & 3) || !hh_setup_in(s, off, 4) || s->seen[off / 4])
        return;
    for (; hh_setup_in(s, off, 4) && !s->seen[off / 4]; off += 4) {
        s->seen[off / 4] = 1;
        hh_sw32(s->b, off);
        if (*(s32 *)(s->b + off) < 0)
            break;
    }
}

/* Word kinds: 'W' 32-bit, 'H' two 16-bit halves, 'V' bytes, 'D' header ([u16][u8][u8]),
 * 'X' [s8 s8 s16], 'Y' [s16 s8 s8]. */
static void hh_word(u8 *p, char k)
{
    switch (k) {
    case 'W': hh_sw32(p, 0); break;
    case 'H': hh_sw16(p, 0); hh_sw16(p, 2); break;
    case 'D': case 'Y': hh_sw16(p, 0); break;
    case 'X': hh_sw16(p, 2); break;
    default: break;
    }
}

static char hh_propdef_kind(u32 type, u32 i)
{
    int obj = 0;
    if (i == 0)
        return 'D';
    switch (type) {
    case 1: case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 12: case 13:
    case 17: case 20: case 21: case 36: case 39: case 40: case 42: case 43: case 45: case 47:
        obj = 1; /* ObjectRecord first: [s16 obj][s16 pad], shadecol/nextcol rgba */
        break;
    }
    if (obj && i < 32)
        return i == 1 ? 'H' : (i == 30 || i == 31) ? 'V' : 'W';
    switch (type) {
    case 1: return i == 38 ? 'H' : i == 47 ? 'X' : i == 49 ? 'Y' : 'W';      /* door */
    case 8: return i == 32 ? 'X' : 'W';                                       /* weapon: s8 num, s8 linked, s16 timer */
    case 11: return i == 148 ? 'V' : 'W';                                     /* multi monitor: image nums u8[4] */
    case 20: return (i >= 32 && i <= 44) ? 'H' : 'W';                         /* ammo box: [u16 model][u16 quantity] */
    case 39: case 40: return i == 33 ? 'H' : 'W';                             /* vehicle/aircraft [u16][s16] */
    case 9: return i <= 5 ? 'H' : 'W';                                        /* guard: u16 pairs */
    case 18: return i == 2 ? 'Y' : 'W';                                       /* guard attribute */
    case 22: return i == 1 ? 'H' : 'W';                                       /* tag: [u16 id][s16 offset] */
    /* objective start: [u16 reserved][u16 text], [u16][u8][s8 difficulty]; watch-menu text:
     * [u16 reserved][u16 text]. Swapped as words, every objective's text read 0 (langGet(0):
     * the unloaded bank 0 -- the watch's objectives page crashed on the device, round 17) and
     * its difficulty 0 (every objective listed on every difficulty). */
    case 23: return i == 2 ? 'H' : i == 3 ? 'D' : 'W';
    case 35: return i == 2 ? 'H' : 'W';
    default: return 'W';
    }
}

static void hh_setup_propdefs(HhSetup *s, u32 off)
{
    u32 n = 0;
    while (hh_setup_in(s, off, 4) && n++ < 20000) {
        u32 type, words, i;
        hh_word(s->b + off, 'D');
        type = s->b[off + 3];
        words = (u32)sizepropdef(s->b + off);
        if (words == 0 || !hh_setup_in(s, off, words * 4))
            hh_fatal("setup: propdef type %u at +%x runs past the file", type, off);
        for (i = 1; i < words; i++)
            hh_word(s->b + off + i * 4, hh_propdef_kind(type, i));
        off += words * 4;
        if (type == 48) /* PROPDEF_END */
            break;
    }
}

static void hh_setup_intro(HhSetup *s, u32 off)
{
    static const u8 bytes[10] = { 0x0C, 0x10, 0x10, 0x20, 0x08, 0x08, 0x28, 0x0C, 0x08, 0x04 };
    u32 n = 0;
    while (hh_setup_in(s, off, 4) && n++ < 1000) {
        u32 type, len, i;
        hh_sw32(s->b, off);
        type = *(u32 *)(s->b + off);
        if (type > 9)
            hh_fatal("setup: intro record type %u at +%x", type, off);
        len = bytes[type];
        if (!hh_setup_in(s, off, len))
            break;
        for (i = 4; i < len; i += 4)
            hh_word(s->b + off + i, (type == 6 && i == 0x1C) ? 'H' : 'W'); /* camera lang1c: u16[2] */
        if (type == 8) { /* credits: 12-byte entries of halfwords */
            u32 c = *(u32 *)(s->b + off + 4), k;
            for (; hh_setup_in(s, c, 12); c += 12) {
                for (k = 0; k < 12; k += 2)
                    hh_sw16(s->b, c + k);
                if (*(u16 *)(s->b + c) == 0 && *(u16 *)(s->b + c + 2) == 0)
                    break;
            }
        }
        if (type == 9)
            break;
        off += len;
    }
}

void hh_swap_setup(void *file, u32 size)
{
    HhSetup s;
    u32 h[10], i, off;
    if (file == NULL || size < 40)
        return;
    s.b = file;
    s.size = size;
    s.seen = calloc(size / 4 + 1, 1);
    if (s.seen == NULL)
        hh_fatal("setup: out of memory");
    for (i = 0; i < 10; i++) {
        hh_sw32(s.b, i * 4);
        h[i] = *(u32 *)(s.b + i * 4);
    }
    for (off = h[0]; h[0] && hh_setup_in(&s, off, 16); off += 16) {   /* waypoints */
        hh_sw32(s.b, off);
        if (*(s32 *)(s.b + off) < 0)
            break;
        hh_sw32(s.b, off + 4); hh_sw32(s.b, off + 8); hh_sw32(s.b, off + 12);
        hh_setup_idlist(&s, *(u32 *)(s.b + off + 4));
    }
    for (off = h[1]; h[1] && hh_setup_in(&s, off, 12); off += 12) {   /* waypoint groups */
        hh_sw32(s.b, off);
        if (*(u32 *)(s.b + off) == 0)
            break;
        hh_sw32(s.b, off + 4); hh_sw32(s.b, off + 8);
        hh_setup_idlist(&s, *(u32 *)(s.b + off));
        hh_setup_idlist(&s, *(u32 *)(s.b + off + 4));
    }
    if (h[2])
        hh_setup_intro(&s, h[2]);
    if (h[3])
        hh_setup_propdefs(&s, h[3]);
    for (off = h[4]; h[4] && hh_setup_in(&s, off, 8); off += 8) {     /* patrol paths */
        hh_sw32(s.b, off);
        if (*(u32 *)(s.b + off) == 0)
            break;
        hh_sw16(s.b, off + 6);
        hh_setup_idlist(&s, *(u32 *)(s.b + off));
    }
    for (off = h[5]; h[5] && hh_setup_in(&s, off, 8); off += 8) {     /* AI lists */
        hh_sw32(s.b, off);
        if (*(u32 *)(s.b + off) == 0)
            break;
        hh_sw32(s.b, off + 4);
    }
    for (i = 6; i <= 7; i++) {                                         /* pads, bound pads */
        u32 rec = i == 6 ? 44 : 68, k;
        for (off = h[i]; h[i] && hh_setup_in(&s, off, rec); off += rec) {
            hh_sw32(s.b, off + 36);
            if (*(u32 *)(s.b + off + 36) == 0)
                break;
            for (k = 0; k < rec; k += 4)
                if (k != 36)
                    hh_sw32(s.b, off + k);
        }
    }
    for (i = 8; i <= 9; i++)                                           /* pad names */
        for (off = h[i]; h[i] && hh_setup_in(&s, off, 4); off += 4) {
            hh_sw32(s.b, off);
            if (*(u32 *)(s.b + off) == 0)
                break;
        }
    free(s.seen);
}
