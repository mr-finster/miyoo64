/* The native port's fixups for what the game reads from the ROM (M2, game/include/port_fix.h). Game side. */
#include "common.h"
#include "sprite.h"
#include "model.h"
#include "port_fix.h"
#include "port.h"

void port_fix_words(void *data, u32 bytes) {
    u32 *w = data;
    for (u32 i = 0; i < bytes / 4; i++) w[i] = __builtin_bswap32(w[i]);
}

typedef struct {
    char name[16];
    u32 offset, compressedLength, decompressedLength;
} PortAssetHeader;

void port_fix_asset_table(void *table, u32 bytes) {
    PortAssetHeader *e = table;
    for (u32 i = 0; i < bytes / sizeof(PortAssetHeader); i++) {
        PORT_SWAP32(e[i].offset);
        PORT_SWAP32(e[i].compressedLength);
        PORT_SWAP32(e[i].decompressedLength);
    }
}

/* a background's raster and palette follow its header; the N64 decodes it to 0x80200000 */
void port_fix_background(void *data) {
    BackgroundHeader *bg = data;
    PORT_SWAP32(bg->raster);
    PORT_SWAP32(bg->palette);
    PORT_SWAP16(bg->startX);
    PORT_SWAP16(bg->startY);
    PORT_SWAP16(bg->width);
    PORT_SWAP16(bg->height);
    PORT_RELOC(bg->raster, 0x80200000, bg);
    PORT_RELOC(bg->palette, 0x80200000, bg);
}

/* ---------------------------------------------------------------- sprites */
/* A sprite after decode_yay0 (sprite.h, the decomp's tools/splat_ext/pm_sprites.py): four header words, then lists
 * of offsets ending in -1 (animations -> components, rasters, palettes); a component points at its u16 command
 * list. Images and palettes stay in the N64's byte order (the renderer reads them as bytes / be16). Parts can be
 * shared, so each is swapped once: a bit per halfword offset marks what is done. */
#define SPR_MAX (1u << 20)
static u32 sSprSeen[SPR_MAX / 2 / 32];
static u32 sSprSize;

static int spr_claim(u32 off, u32 len) {
    if ((off & 1) || off + len > sSprSize || off + len < off) {
        port_log("sprite: part at 0x%X (+%u) is outside its %u bytes\n", off, len, sSprSize);
        return 0;
    }
    const u32 bit = off / 2;
    if (sSprSeen[bit >> 5] & (1u << (bit & 31))) return 0;
    sSprSeen[bit >> 5] |= 1u << (bit & 31);
    return 1;
}

static void spr_list(u8 *d, u32 off, void (*each)(u8 *, u32)) {
    if (!spr_claim(off, 4)) return;
    for (u32 *w = (u32 *) (d + off); (u8 *) (w + 1) <= d + sSprSize && *w != 0xFFFFFFFFu; w++) {
        *w = __builtin_bswap32(*w);
        if (each) each(d, *w);
    }
}

static void spr_comp(u8 *d, u32 off) {
    if (!spr_claim(off, sizeof(SpriteAnimComponent))) return;
    SpriteAnimComponent *c = (SpriteAnimComponent *) (d + off);
    PORT_SWAP32(c->cmdList);
    PORT_SWAP16(c->cmdListSize);
    PORT_SWAP16(c->compOffset.x);
    PORT_SWAP16(c->compOffset.y);
    PORT_SWAP16(c->compOffset.z);
    const u32 cmds = (u32) c->cmdList, bytes = (u16) c->cmdListSize;
    if (bytes && spr_claim(cmds, bytes))
        for (u16 *h = (u16 *) (d + cmds), *e = h + bytes / 2; h < e; h++) *h = __builtin_bswap16(*h);
}
static void spr_anim(u8 *d, u32 off) { spr_list(d, off, spr_comp); }
static void spr_raster(u8 *d, u32 off) {
    if (spr_claim(off, sizeof(SpriteRasterCacheEntry))) PORT_SWAP32(((SpriteRasterCacheEntry *) (d + off))->image);
}

void port_fix_sprite(void *data, u32 size) {
    u8 *d = data;
    u32 *h = data;
    sSprSize = size < SPR_MAX ? size : SPR_MAX;
    __builtin_memset(sSprSeen, 0, (sSprSize / 2 + 31) / 32 * 4);
    for (int i = 0; i < 4; i++) h[i] = __builtin_bswap32(h[i]);
    spr_list(d, 0x10, spr_anim);
    spr_list(d, h[0], spr_raster);
    spr_list(d, h[1], NULL);
}

/* ---------------------------------------------------------------- map shapes */
/* A map's shape file (model.h; the decomp's tools/build/mapfs/shape.py) is decoded where the N64 keeps it, at
 * 0x80210000, and every pointer in it is an address there. Walked from the header: the name lists (pointers to
 * strings, up to "db"), the model tree (nodes, group data, child lists, display data, property lists), each display
 * list (only G_VTX, G_TRI1/2, G_MTX, G_DL, G_POPMTX, G_GEOMETRYMODE, G_RDPPIPESYNC, G_ENDDL occur) with the vertices
 * and matrices it loads. Pointers are moved to the host's copy and every word or halfword is swapped once. */
#define SHAPE_N64_BASE 0x80210000u
#define SHAPE_MAX 0x30000u /* the N64's room for it: 0x80210000..0x80240000 (the map overlay) */
#define SHP_NONE 0xFFFFFFFFu
static u8 *sShp;
static u32 sShpSize;
static u32 sShpSeen[SHAPE_MAX / 2 / 32];
static int sShpBad;

static int shp_claim(u32 off, u32 len) {
    if ((off & 1) || off + len > sShpSize || off + len < off) { sShpBad++; return 0; }
    const u32 bit = off / 2;
    if (sShpSeen[bit >> 5] & (1u << (bit & 31))) return 0;
    sShpSeen[bit >> 5] |= 1u << (bit & 31);
    return 1;
}

/* a big-endian N64 pointer field -> a host pointer; returns the offset in the file (SHP_NONE for null or outside) */
static u32 shp_reloc(void *field) {
    u32 *f = field;
    const u32 a = __builtin_bswap32(*f);
    if (a == 0) { *f = 0; return SHP_NONE; }
    if (a < SHAPE_N64_BASE || a - SHAPE_N64_BASE >= sShpSize) {
        if (sShpBad++ < 8) port_log("shape: pointer 0x%08X is outside the file (%u bytes)\n", a, sShpSize);
        *f = 0;
        return SHP_NONE;
    }
    *f = a - SHAPE_N64_BASE + (u32) sShp;
    return a - SHAPE_N64_BASE;
}

static void shp_mtx(u32 off) {
    if (shp_claim(off, sizeof(Mtx))) port_fix_words(sShp + off, sizeof(Mtx));
}

static void shp_vtx(u32 off, u32 n) { /* Vtx: ob[3], flag, tc[2] are halfwords; the colour bytes stay */
    for (u32 i = 0; i < n; i++, off += sizeof(Vtx)) {
        if (!shp_claim(off, sizeof(Vtx))) continue;
        u16 *h = (u16 *) (sShp + off);
        for (int k = 0; k < 6; k++) h[k] = __builtin_bswap16(h[k]);
    }
}

static void shp_dl(u32 off, int depth) {
    if (depth > 16) return;
    for (u32 guard = 0; guard < 65536; guard++, off += 8) {
        if (!shp_claim(off, 8)) return; /* done before (or outside the file) */
        u32 *w = (u32 *) (sShp + off);
        w[0] = __builtin_bswap32(w[0]);
        const u8 op = w[0] >> 24;
        u32 t;
        switch (op) {
            case 0x01: /* G_VTX */
                if ((t = shp_reloc(&w[1])) != SHP_NONE) shp_vtx(t, (w[0] >> 12) & 0xFF);
                break;
            case 0xDA: /* G_MTX */
                if ((t = shp_reloc(&w[1])) != SHP_NONE) shp_mtx(t);
                break;
            case 0xDE: /* G_DL (call or branch) */
                if ((t = shp_reloc(&w[1])) != SHP_NONE) shp_dl(t, depth + 1);
                if (w[0] & 0x00010000) return; /* a branch does not come back */
                break;
            case 0xDF: /* G_ENDDL */
                w[1] = __builtin_bswap32(w[1]);
                return;
            default:
                w[1] = __builtin_bswap32(w[1]);
                break;
        }
    }
}

static void shp_node(u32 off, int depth);

static void shp_props(u32 off, s32 n) {
    if (n <= 0 || !shp_claim(off, (u32) n * sizeof(ModelNodeProperty))) return;
    ModelNodeProperty *p = (ModelNodeProperty *) (sShp + off);
    for (s32 i = 0; i < n; i++) {
        PORT_SWAP32(p[i].key);
        PORT_SWAP32(p[i].dataType);
        if (p[i].key == MODEL_PROP_KEY_TEXTURE_NAME || (p[i].dataType == 2 && p[i].key != MODEL_PROP_KEY_SPECIAL))
            shp_reloc(&p[i].data.p);
        else
            PORT_SWAP32(p[i].data.s);
    }
}

static void shp_group(u32 off, int depth) {
    if (!shp_claim(off, sizeof(ModelGroupData))) return;
    ModelGroupData *g = (ModelGroupData *) (sShp + off);
    u32 t;
    PORT_SWAP32(g->numLights);
    PORT_SWAP32(g->numChildren);
    if ((t = shp_reloc(&g->transformMatrix)) != SHP_NONE) shp_mtx(t);
    shp_reloc(&g->lightingGroup); /* Light_t is bytes */
    if ((t = shp_reloc(&g->childList)) != SHP_NONE && g->numChildren > 0 &&
        shp_claim(t, (u32) g->numChildren * 4)) {
        u32 *c = (u32 *) (sShp + t);
        for (s32 i = 0; i < g->numChildren; i++) {
            const u32 child = shp_reloc(&c[i]);
            if (child != SHP_NONE) shp_node(child, depth + 1);
        }
    }
}

static void shp_node(u32 off, int depth) {
    if (depth > 64 || !shp_claim(off, sizeof(ModelNode))) return;
    ModelNode *n = (ModelNode *) (sShp + off);
    u32 t;
    PORT_SWAP32(n->type);
    PORT_SWAP32(n->numProperties);
    if ((t = shp_reloc(&n->displayData)) != SHP_NONE && shp_claim(t, sizeof(ModelDisplayData))) {
        const u32 dl = shp_reloc(&((ModelDisplayData *) (sShp + t))->displayList);
        if (dl != SHP_NONE) shp_dl(dl, 0);
    }
    if ((t = shp_reloc(&n->propertyList)) != SHP_NONE) shp_props(t, n->numProperties);
    if ((t = shp_reloc(&n->groupData)) != SHP_NONE) shp_group(t, depth);
}

static void shp_names(void *field) {
    const u32 off = shp_reloc(field);
    if (off == SHP_NONE || !shp_claim(off, 4)) return;
    for (u32 *p = (u32 *) (sShp + off); (u8 *) (p + 1) <= sShp + sShpSize; p++) {
        const u32 s = shp_reloc(p);
        if (s == SHP_NONE) break;
        const char *str = (const char *) (sShp + s);
        if (str[0] == 'd' && str[1] == 'b' && str[2] == 0) break;
    }
}

void port_fix_shape(void *data, u32 size) {
    ShapeFile *f = data;
    sShp = data;
    sShpSize = size < SHAPE_MAX ? size : SHAPE_MAX;
    sShpBad = 0;
    if (size > SHAPE_MAX) port_log("shape: %u bytes is more than the N64's %u\n", size, SHAPE_MAX);
    __builtin_memset(sShpSeen, 0, (sShpSize / 2 + 31) / 32 * 4);
    shp_claim(0, sizeof(ShapeFileHeader));
    const u32 root = shp_reloc(&f->header.root);
    shp_reloc(&f->header.vertexTable);
    shp_names(&f->header.modelNames);
    shp_names(&f->header.colliderNames);
    shp_names(&f->header.zoneNames);
    if (root != SHP_NONE) shp_node(root, 0);
    if (sShpBad) port_log("shape: %d parts outside the file\n", sShpBad);
}
