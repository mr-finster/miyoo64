/* hh_assets.c: Banjo-Kazooie's ROM assets in the handheld's byte order.
 *
 * The game reads its assets (assets.bin: models, animations, sprites, level setups, text, demos, music) straight from
 * the inflated file as C structs. On the N64 those are big-endian; here every multi-byte field is swapped to the host's
 * order once, as the asset cache loads the file (assetcache_get, decomp.patch), so the game's own code reads them
 * unchanged. Texture and palette bytes stay in the N64's order (the renderer reads them so), as do byte strings.
 * Bit-field structs inside assets are declared in reverse order on little-endian hosts (decomp.patch), so a swapped
 * storage unit gives every field its N64 value.
 *
 * The kind of each asset comes from its place in the table (the asset table's flag runs: the decomp's bk_asset_tool
 * splits it the same way) and, for the model/sprite runs, the model magic. The formats are the decomp's structs
 * (include/core2/model.h, animationfile.h, structs.h's BKSprite, demo.c's DemoInput, n_libaudio.h's ALCMidiHdr). */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savestate.h"

#define RD16(p) ((u16)((((const u8 *)(p))[0] << 8) | ((const u8 *)(p))[1]))
#define RD32(p) ((u32)((((const u8 *)(p))[0] << 24) | (((const u8 *)(p))[1] << 16) | (((const u8 *)(p))[2] << 8) | ((const u8 *)(p))[3]))

static u8 *sBase SAVESTATE_EXCLUDE;
static s32 sSize SAVESTATE_EXCLUDE;
static s32 sId SAVESTATE_EXCLUDE;
static int sErrors SAVESTATE_EXCLUDE;

static int hh_in(s32 off, s32 len) { return off >= 0 && len >= 0 && off + len <= sSize; }

static void hh_bad(const char *what, s32 off)
{
    if (sErrors++ < 40)
        fprintf(stderr, "assets: asset 0x%x: %s out of range at 0x%x (size 0x%x)\n", (unsigned)sId, what, (unsigned)off,
                (unsigned)sSize);
}

static void sw16(s32 off)
{
    u8 *p = sBase + off, t;
    if (!hh_in(off, 2)) { hh_bad("halfword", off); return; }
    t = p[0]; p[0] = p[1]; p[1] = t;
}

static void sw32(s32 off)
{
    u8 *p = sBase + off, t;
    if (!hh_in(off, 4)) { hh_bad("word", off); return; }
    t = p[0]; p[0] = p[3]; p[3] = t;
    t = p[1]; p[1] = p[2]; p[2] = t;
}

static void sw16n(s32 off, s32 n) { while (n-- > 0) { sw16(off); off += 2; } }
static void sw32n(s32 off, s32 n) { while (n-- > 0) { sw32(off); off += 4; } }

static s16 h16(s32 off) { return hh_in(off, 2) ? *(s16 *)(sBase + off) : 0; }   /* a field already swapped */
static s32 h32(s32 off) { return hh_in(off, 4) ? *(s32 *)(sBase + off) : 0; }

/* ------------------------------------------------------------------------------------ */
/* models (include/core2/model.h)                                                       */

static u8 *sGeoDone SAVESTATE_EXCLUDE; /* one bit per 4 bytes: geo commands already swapped */

static void hh_geo(s32 off, int depth)
{
    while (depth < 64) {
        u32 cmd;
        s32 next;
        if (!hh_in(off, 8) || (off & 3)) {
            hh_bad("geo command", off);
            return;
        }
        if (sGeoDone[off >> 5] & (1u << ((off >> 2) & 7)))
            return;
        sGeoDone[off >> 5] |= 1u << ((off >> 2) & 7);
        sw32(off);
        sw32(off + 4);
        cmd = (u32)h32(off);
        next = h32(off + 4);
        switch (cmd) {
        case 0x00: /* geo_cmd_0: s16 branch, s16 do_pitch_rotate, f32 position[3] */
            sw16n(off + 8, 2);
            sw32n(off + 12, 3);
            if (h16(off + 8)) hh_geo(off + h16(off + 8), depth + 1);
            break;
        case 0x01: /* SORT: f32 p1[3], p2[3], s16 flags, s16 branch1, s32 branch2 */
            sw32n(off + 8, 6);
            sw16n(off + 32, 2);
            sw32(off + 36);
            if (h16(off + 34)) hh_geo(off + h16(off + 34), depth + 1);
            if (h32(off + 36)) hh_geo(off + h32(off + 36), depth + 1);
            break;
        case 0x02: /* BONE: u8 branch, s8 anim_matrix_id */
            if (hh_in(off + 8, 1) && sBase[off + 8]) hh_geo(off + sBase[off + 8], depth + 1);
            break;
        case 0x03: /* LOADDL: s16 gfx_index (and the count after it) */
            sw16n(off + 8, 2);
            break;
        case 0x05: { /* SKINNING: s16 gfx_index[], 0-terminated after the first */
            s32 i;
            sw16(off + 8);
            for (i = 1; i < 256; i++) {
                if (next != 0 && 8 + 2 * i >= next) break;
                sw16(off + 8 + 2 * i);
                if (h16(off + 8 + 2 * i) == 0) break;
            }
            break;
        }
        case 0x06: /* CALL: s32 branch */
            sw32(off + 8);
            if (h32(off + 8)) hh_geo(off + h32(off + 8), depth + 1);
            break;
        case 0x07: /* LOADDL2: s16 pad, s16 gfx_index */
            sw16n(off + 8, 2);
            break;
        case 0x08: /* LOD: f32 max, min, position[3], s32 branch */
            sw32n(off + 8, 6);
            if (h32(off + 28)) hh_geo(off + h32(off + 28), depth + 1);
            break;
        case 0x0A: /* REFPOINT: s16 index, s16 anim_mtx_id, f32 point[3] */
            sw16n(off + 8, 2);
            sw32n(off + 12, 3);
            break;
        case 0x0C: { /* SELECTOR: s16 count, s16 index, s32 offsets[count] */
            s32 i, n;
            sw16n(off + 8, 2);
            n = h16(off + 8);
            for (i = 0; i < n && i < 256; i++) {
                sw32(off + 12 + 4 * i);
                if (h32(off + 12 + 4 * i)) hh_geo(off + h32(off + 12 + 4 * i), depth + 1);
            }
            break;
        }
        case 0x0D: /* DRAWDIST: s16 min[3], max[3], s16 branch */
            sw16n(off + 8, 7);
            if (h16(off + 20)) hh_geo(off + h16(off + 20), depth + 1);
            break;
        case 0x0E: /* E: s16 position[3], s16 distance, s16 branch, s16 anim_mtx_id */
            sw16n(off + 8, 6);
            if (h16(off + 16)) hh_geo(off + h16(off + 16), depth + 1);
            break;
        case 0x0F: /* CAMERA: s16 branch, u8 count, u8 flags, u8 ids[] */
            sw16(off + 8);
            if (h16(off + 8)) hh_geo(off + h16(off + 8), depth + 1);
            break;
        case 0x10: /* TEXWRAP: s32 mode */
            sw32(off + 8);
            break;
        case 0x04: case 0x09: case 0x0B: /* NOP */
            break;
        default:
            hh_bad("unknown geo command", off);
            return;
        }
        if (next == 0)
            return;
        off += next;
    }
}

static void hh_model(void)
{
    s32 o;
    sw32n(0x00, 2);
    sw16n(0x08, 2);
    sw32n(0x0C, 9);
    sw16n(0x30, 2);
    sw32(0x34);

    if ((o = h16(0x08)) != 0) { /* texture list: s32 size, s16 count, u16 pad, BKTextureInfo[count] (16 bytes) */
        s32 i, n;
        sw32(o);
        sw16n(o + 4, 2);
        n = h16(o + 4);
        for (i = 0; i < n; i++) {
            sw32(o + 8 + 16 * i);
            sw16(o + 8 + 16 * i + 4);
        }
    }
    if ((o = h32(0x0C)) != 0) { /* gfx list: u32 count, u32 pad, Gfx[count] */
        sw32n(o, 2);
        sw32n(o + 8, 2 * h32(o));
    }
    if ((o = h32(0x10)) != 0) { /* vertex list: s16 header[12], Vtx[count] (s16 x6 then 4 colour bytes) */
        s32 i, n;
        sw16n(o, 12);
        n = (u16)h16(o + 20);
        for (i = 0; i < n; i++)
            sw16n(o + 24 + 16 * i, 6);
    }
    if ((o = h32(0x14)) != 0) { /* unk14 list: s16 counts[4], [0] 24-byte, [1] 16-byte, [2] 12-byte entries */
        s32 i, n0, n1, n2, p;
        sw16n(o, 4);
        n0 = h16(o); n1 = h16(o + 2); n2 = h16(o + 4);
        p = o + 8;
        for (i = 0; i < n0; i++, p += 24) sw16n(p, 9);
        for (i = 0; i < n1; i++, p += 16) sw16n(p, 5);
        for (i = 0; i < n2; i++, p += 12) sw16n(p, 4);
    }
    if ((o = h32(0x18)) != 0) { /* animation list: f32 unk0, s16 count, BKAnimation[count] at +8 (f32[3], s16, s16) */
        s32 i, n;
        sw32(o);
        sw16(o + 4);
        n = h16(o + 4);
        for (i = 0; i < n; i++) {
            sw32n(o + 8 + 16 * i, 3);
            sw16n(o + 8 + 16 * i + 12, 2);
        }
    }
    if ((o = h32(0x1C)) != 0) { /* collision list: s16 header[11] + pad, geometry[geo] (s16 x2), tris[tri] (s16 x4, s32) */
        s32 i, ng, nt, p;
        sw16n(o, 11);
        ng = h16(o + 16);
        nt = h16(o + 20);
        p = o + 24;
        sw16n(p, 2 * ng);
        p += 4 * ng;
        for (i = 0; i < nt; i++, p += 12) {
            sw16n(p, 4);
            sw32(p + 8);
        }
    }
    if ((o = h32(0x20)) != 0) { /* camera areas: u8 count, u8 pad, BKCameraArea[count] (s16 x6, u8, u8) */
        s32 i, n = hh_in(o, 1) ? sBase[o] : 0;
        for (i = 0; i < n; i++)
            sw16n(o + 2 + 14 * i, 6);
    }
    if ((o = h32(0x24)) != 0) { /* mesh list: s16 count, BKMesh[count] (s16 uid, s16 n, s16 vertices[n]) */
        s32 i, n, p;
        sw16(o);
        n = h16(o);
        p = o + 2;
        for (i = 0; i < n; i++) {
            s32 k;
            sw16n(p, 2);
            k = h16(p + 2);
            sw16n(p + 4, k);
            p += 4 + 2 * k;
        }
    }
    if ((o = h32(0x28)) != 0) { /* animated vertices: s16 count, u16 pad, BKAnimVertices[count] (s16[3], s8, u8 n, s16[n]) */
        s32 i, n, p;
        sw16n(o, 2);
        n = h16(o);
        p = o + 4;
        for (i = 0; i < n; i++) {
            s32 k;
            sw16n(p, 3);
            k = hh_in(p + 7, 1) ? sBase[p + 7] : 0;
            sw16n(p + 8, k);
            p += 8 + 2 * k;
        }
    }
    if ((o = h32(0x2C)) != 0) { /* animated textures: BKAnimTexture[4] (s16, s16, f32) */
        s32 i;
        for (i = 0; i < 4; i++) {
            sw16n(o + 8 * i, 2);
            sw32(o + 8 * i + 4);
        }
    }
    if ((o = h32(0x04)) != 0) { /* the geo command tree last: its commands are found by following it */
        sGeoDone = calloc((size_t)sSize / 32 + 1, 1);
        if (sGeoDone != NULL) {
            hh_geo(o, 0);
            free(sGeoDone);
            sGeoDone = NULL;
        }
    }
}

/* ------------------------------------------------------------------------------------ */
/* sprites (structs.h BKSprite / BKSpriteFrame / BKSpriteTextureBlock, code_AEDA0.c)    */

static void hh_sprite(void)
{
    s32 i, n, type, nib, frames;
    sw16n(0, 6);
    sw32(12);
    n = h16(0);
    type = h16(2);
    sw32n(16, n);
    frames = 16 + 4 * n;
    nib = (type & 0x001) ? 1 : (type & 0x004) ? 2 : (type & 0x020) ? 1 : (type & 0x040) ? 2 : (type & 0x080) ? 1 :
          (type & 0x100) ? 2 : (type & 0x400) ? 4 : (type & 0x800) ? 8 : 0;
    for (i = 0; i < n; i++) {
        s32 f = frames + h32(16 + 4 * i), b, k, chunks;
        if (!hh_in(f, 20)) { hh_bad("sprite frame", f); continue; }
        sw16n(f, 10);
        chunks = h16(f + 8);
        b = f + 20;
        if (type & 0x001)
            b = ((b + 7) & ~7) + 0x20; /* CI4 palette, 16 entries (N64 order) */
        else if (type & 0x004)
            b = ((b + 7) & ~7) + 0x200; /* CI8 palette, 256 entries */
        for (k = 0; k < chunks && nib; k++) {
            s32 w, h;
            if (!hh_in(b, 8)) { hh_bad("sprite chunk", b); break; }
            sw16n(b, 4);
            w = h16(b + 4);
            h = h16(b + 6);
            b = ((b + 8 + 7) & ~7) + w * h * nib / 2;
        }
    }
}

/* ------------------------------------------------------------------------------------ */

enum { HH_A_NONE, HH_A_ANIM, HH_A_MODEL_SPRITE, HH_A_SETUP, HH_A_TEXT, HH_A_DEMO, HH_A_MIDI };

/* the table's runs: each starts where the flag's bit 1 changes (bk_asset_tool AssetFolder::from_bytes) */
static u8 *sKind SAVESTATE_EXCLUDE;
static s32 sCount SAVESTATE_EXCLUDE;

typedef struct { s32 offset; s16 compFlag; s16 flag; } HhAssetMeta;

/* code_B3A80.c assetCache_init, after the table's read: the header and entries to host order, and the runs */
void hh_asset_table_loaded(void *head, void *meta)
{
    static const u8 kRunKind[] = { HH_A_ANIM, HH_A_MODEL_SPRITE, HH_A_SETUP, HH_A_MODEL_SPRITE, HH_A_TEXT, HH_A_MODEL_SPRITE,
                                   HH_A_MIDI };
    HhAssetMeta *m = meta;
    s32 i, run = 0, prev = 3;
    sCount = (s32) * (u32 *)head; /* hh_asset_head_loaded swapped it */
    sKind = calloc((size_t)sCount + 1, 1);
    for (i = 0; i < sCount; i++) {
        u8 *p = (u8 *)&m[i];
        m[i].offset = (s32)RD32(p);
        m[i].compFlag = (s16)RD16(p + 4);
        m[i].flag = (s16)RD16(p + 6);
    }
    for (i = 0; i + 1 < sCount; i++) {
        if (m[i].flag == 4)
            continue;
        if (m[i].flag != 2 && (prev & 2) != (m[i].flag & 2)) {
            run++;
            prev = m[i].flag;
        }
        sKind[i] = run < (s32)sizeof(kRunKind) ? kRunKind[run] : HH_A_NONE;
    }
    fprintf(stderr, "assets: %d entries, %d runs\n", (int)sCount, (int)run + 1);
}

/* the table's header word as the game reads it (count): swapped in place too */
void hh_asset_head_loaded(void *head)
{
    u32 *w = head;
    w[0] = RD32(&w[0]);
    w[1] = RD32(&w[1]);
}

/* assetcache_get (and the cache's load-into variant), once per load: the file to host order */
void hh_asset_loaded(s32 id, void *data, s32 size)
{
    if (data == NULL || id < 0 || id >= sCount)
        return;
    sBase = data;
    sSize = size;
    sId = id;
    switch (sKind[id]) {
    case HH_A_ANIM: /* every field is a halfword (animationfile.h) */
        sw16n(0, size / 2);
        break;
    case HH_A_MODEL_SPRITE:
        if (size >= 0x38 && RD32(data) == 0x0000000Bu)
            hh_model();
        else if (size >= 16)
            hh_sprite();
        break;
    case HH_A_DEMO: /* DemoInput[] after a 4-byte header: u8 x, u8 y, u16 buttons, u8 frames, u8 pad */
        break;
    case HH_A_MIDI: /* ALCMidiHdr: s32 trackOffset[16], s32 division; the tracks are bytes */
        sw32n(0, 17);
        break;
    case HH_A_TEXT: /* dialog, quiz and Grunty questions read byte by byte; demos (the first run of it) have halfwords */
        if (id >= 0x9A3 && id <= 0xA0A) {
            s32 i;
            for (i = 4; i + 6 <= size; i += 6)
                sw16(i + 2);
        }
        break;
    default: /* level setups: read through file.c, which swaps each field it reads */
        break;
    }
}

/* the asset's kind, for file.c (level setups are swapped as they are read) */
int hh_asset_is_setup(s32 id) { return id >= 0 && id < sCount && sKind[id] == HH_A_SETUP; }

/* ------------------------------------------------------------------------------------ */
/* sound banks (libaudio.h: ALBankFile, ALBank, ALInstrument, ALSound, ALEnvelope, ALWaveTable, ALADPCMBook/loop)  */

/* alBnkfNew (libultra bnkf.c) turns the bank file's offsets into pointers; first, every multi-byte field to host
 * order. The same envelope, key map, wave table or book can be shared: each is swapped once (sBnkDone). The samples
 * (the .tbl) are bytes. Linked with --wrap=alBnkfNew. */
static u8 *sBnkDone SAVESTATE_EXCLUDE;
static int hh_bnk_once(s32 off)
{
    if (off <= 0 || !hh_in(off, 1))
        return 0;
    if (sBnkDone[off >> 3] & (1u << (off & 7)))
        return 0;
    sBnkDone[off >> 3] |= 1u << (off & 7);
    return 1;
}

static void hh_bnk_wave(s32 o)
{
    s32 book, loop;
    if (!hh_bnk_once(o))
        return;
    sw32n(o, 2); /* base, len; u8 type, flags at +8 */
    sw32n(o + 12, 2); /* waveInfo: adpcm {loop, book} / raw {loop} */
    loop = h32(o + 12);
    book = h32(o + 16);
    if (sBase[o + 8] == 0 /* AL_ADPCM_WAVE */) {
        if (hh_bnk_once(book)) { /* s32 order, s32 npredictors, s16 book[order * npredictors * 8] */
            sw32n(book, 2);
            sw16n(book + 8, h32(book) * h32(book + 4) * 8);
        }
        if (hh_bnk_once(loop)) { /* u32 start, end, count; ADPCM_STATE (s16[16]) */
            sw32n(loop, 3);
            sw16n(loop + 12, 16);
        }
    } else if (hh_bnk_once(loop)) { /* ALRawLoop: u32 start, end, count */
        sw32n(loop, 3);
    }
}

static void hh_bnk_inst(s32 o)
{
    s32 i, n;
    if (!hh_bnk_once(o))
        return;
    sw16n(o + 12, 2); /* bendRange, soundCount (after 12 bytes) */
    n = h16(o + 14);
    sw32n(o + 16, n);
    for (i = 0; i < n; i++) {
        s32 s = h32(o + 16 + 4 * i);
        if (!hh_bnk_once(s))
            continue;
        sw32n(s, 3); /* envelope, keyMap, wavetable; u8 samplePan, sampleVolume, flags */
        if (hh_bnk_once(h32(s))) /* ALEnvelope: s32 attackTime, decayTime, releaseTime; u8 volumes */
            sw32n(h32(s), 3);
        hh_bnk_wave(h32(s + 8));
    }
}

extern void __real_alBnkfNew(void *file, u8 *table);
void __wrap_alBnkfNew(void *file, u8 *table)
{
    s32 i, n, size = 0x40000;
    sBase = file;
    sSize = size;
    sId = -1;
    sBnkDone = calloc((size_t)size / 8 + 1, 1);
    sw16n(0, 2); /* revision, bankCount */
    n = h16(2);
    sw32n(4, n);
    for (i = 0; i < n; i++) {
        s32 b = h32(4 + 4 * i), k, ni;
        if (!hh_bnk_once(b))
            continue;
        sw16(b); /* instCount; u8 flags, pad */
        sw32n(b + 4, 2); /* sampleRate, percussion */
        ni = h16(b);
        sw32n(b + 12, ni);
        if (h32(b + 8))
            hh_bnk_inst(h32(b + 8));
        for (k = 0; k < ni; k++)
            hh_bnk_inst(h32(b + 12 + 4 * k));
    }
    free(sBnkDone);
    sBnkDone = NULL;
    __real_alBnkfNew(file, table);
}

/* ------------------------------------------------------------------------------------ */
/* level setups (prop.h): the props a cube's list holds, read as bytes (code_A5BC0.c)    */

/* NodeProp / OtherNode (20 bytes): s16 position[3], u16 bits, u16 actorId, u8, u8, u32 bits, u32 bits */
void hh_setup_nodeprops(void *p, s32 n)
{
    s32 i;
    sBase = p;
    sSize = n * 20;
    sId = -2;
    for (i = 0; i < n; i++) {
        if (getenv("BK_NODELOG") && (sBase[20 * i + 7] & 1))
            fprintf(stderr, "node %d: %08x %08x %08x %08x %08x\n", (int)i, RD32(sBase + 20 * i), RD32(sBase + 20 * i + 4),
                    RD32(sBase + 20 * i + 8), RD32(sBase + 20 * i + 12), RD32(sBase + 20 * i + 16));
        if (sBase[20 * i + 7] & 1) {
            /* bit0 set: a spline node (code_B9770.c reads its 20 bytes as Union_glspline: f32 at 0, u32 units at 8, 0xC
             * and 0x10; the unit at 4 is two u16 halves there, decomp.patch, as NodeProp's position_z and bits) */
            sw32(20 * i);
            sw16n(20 * i + 4, 2);
            sw32n(20 * i + 8, 3);
        } else {
            sw16n(20 * i, 5);
            sw32n(20 * i + 12, 2);
        }
    }
}

/* Prop (12 bytes): a model prop (bit 1 of byte 0xB) starts with a u16 and two bytes, a sprite prop with a u32; then
 * s16 position[3] and the u16 of flags (ModelProp's scale and flags bytes, swapped with it: prop.h) */
void hh_setup_props(void *p, s32 n)
{
    s32 i;
    sBase = p;
    sSize = n * 12;
    sId = -3;
    for (i = 0; i < n; i++) {
        sw32(12 * i); /* one u32 unit for sprites and models alike (prop.h's little-endian ModelProp) */
        sw16n(12 * i + 4, 4);
    }
}
