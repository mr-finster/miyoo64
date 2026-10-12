/* From BattleShip port/bridge/lbreloc_byteswap.cpp (MIT, see port/NOTICE.md), converted to C.
 *
 * lbreloc_byteswap.c — Big-endian to native byte-swap for reloc file blobs
 *
 * N64 reloc files are stored in big-endian byte order. On a little-endian
 * host, every multi-byte field is read incorrectly. This module performs a
 * two-pass byte-swap:
 *
 *   Pass 1: Blanket u32 swap of the entire blob. This correctly handles
 *           display list commands, float/int struct fields, reloc chain
 *           descriptors, 32bpp texture pixels, and zeros.
 *
 *   Pass 2: Parse the now-native-endian display list commands to identify
 *           vertex buffers and texture/palette regions, then apply targeted
 *           fixups for data types that u32 swap got wrong.
 *
 * Called from lbRelocLoadAndRelocFile() between memcpy and the reloc chain walk.
 *
 * handheld port notes (ILP32):
 *  - A relocated slot holds the real 32-bit pointer (BattleShip's LP64
 *    "token" is the identity here), so token resolution is a cast.
 *  - All bookkeeping lives in fixed-capacity static tables (no heap) so it is
 *    captured by save states along with the bytes it describes.
 *  - BattleShip's stage audit, texture-fixup log and TGA dump
 *    instrumentation were dropped.
 */

#include "lbreloc_byteswap.h"
#include "addrset.h"
#include "port_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>  /* decomp/include/string.h shadows libc: no memmove */

#define memmove __builtin_memmove

/* Provided by the reloc bridge (hh/bs/lbreloc_bridge.c). */
extern int  portRelocFindContainingFile(const void *ptr, uintptr_t *out_base, size_t *out_size);
extern int  portRelocFindFileIdAndBase(const void *ptr, uintptr_t *out_base);

#define BSWAP32(x) __builtin_bswap32(x)

/* ILP32: a relocated slot word IS the target pointer. */
static inline void *slot_to_ptr(uint32_t word)
{
	return (void *)(uintptr_t)word;
}

/* BattleShip asks the token table whether a chain-slot word still decodes to
 * a live token (portRelocTryResolvePointer != NULL) to tell live slots from
 * "corpse" registry entries whose address has since been reused by other
 * data. On ILP32 the slot holds a raw pointer, so the equivalent test is
 * "the word points into a currently loaded reloc file": every relocated
 * target (intern or extern) lies in one, while reused texel/struct bytes
 * almost never form such an address. */
static inline bool chain_slot_word_is_live(uint32_t word)
{
	return word != 0 && portRelocFindContainingFile(slot_to_ptr(word), NULL, NULL);
}

/* Remove a single key from an AddrSet (backward-shift deletion so linear
 * probe chains stay intact). addrset.h only offers range removal. */
static void addrset_erase_one(AddrSet *s, uintptr_t k)
{
	uint32_t slot;
	if (k == 0 || !addrset_find(s, k, &slot))
		return;
	uint32_t m = s->cap - 1, i = slot, j = slot;
	for (;;) {
		j = (j + 1) & m;
		if (s->keys[j] == 0)
			break;
		uint32_t h = addrset_hash(s->keys[j]) & m;
		/* Entry at j may stay if its home h lies cyclically in (i, j]. */
		if ((i <= j) ? (i < h && h <= j) : (i < h || h <= j))
			continue;
		s->keys[i] = s->keys[j];
		if (s->vals) s->vals[i] = s->vals[j];
		i = j;
	}
	s->keys[i] = 0;
	addrset_occ_clr(s, i); /* the slot left empty (every slot shifted into stays used) */
	if (s->vals) s->vals[i] = 0;
	s->count--;
}

// Tracks which (base + offset) regions have been fixed to prevent
// double-fixup. Used by per-struct fixups (Sprite, Bitmap, MObjSub, etc.),
// pass2 vertex fixups, the chain-walk texture fixup, and the runtime
// lazy vertex fixup. All paths share this set so they don't undo each
// other's work. Cleared at scene change via portResetStructFixups.
//
// Holds one key per fixed struct/texture base and one per fixed Vtx
// (16 bytes), so it is sized for a large stage + four fighters.
ADDRSET_DEFINE(sStructU16Fixups, 1 << 17);

// Tracks runtime texture bases that own at least one fixed word, plus the
// largest request size seen from that base. This preserves the old pass2/chain
// skip behavior for exact-base matches while sTexFixupWords handles the actual
// per-word idempotency.
ADDRMAP_DEFINE(sTexFixupExtent, 1 << 14);

// Runtime texture/TLUT fixups can overlap even when they start at different
// addresses. CSS gate palettes are adjacent 0x28-byte blocks, but their Sprite
// requests a 512-byte TLUT load from each block. Track each u32 word so the
// second overlapping load skips words fixed by the first instead of BSWAPing
// them back to the wrong byte order.
//
// handheld: stored as a sorted array of disjoint, merged [begin, end) byte
// spans instead of one hash entry per u32 word (textures are contiguous, so
// a few thousand spans cover what would be hundreds of thousands of words).
// A word key w is "fixed" iff some span has begin <= w < end.
typedef struct Span { uintptr_t begin; uintptr_t end; } Span;
#define TEX_WORD_SPAN_CAP 16384
static Span sTexFixupWords[TEX_WORD_SPAN_CAP];
static uint32_t sTexFixupWordsCount;

// Tracks memory ranges of decoded struct arrays that runtime texture/palette
// BSWAPs must NOT touch. Certain N64 sprite files intentionally overlap the
// tail of a CI8 palette (which the renderer loads as a fixed 512-byte block
// even if the actual palette is smaller) with the start of the bitmap array —
// on hardware the unused palette entries don't matter, but on the port the
// runtime-path BSWAP would corrupt the bitmap/sprite structs we already
// byte-swap-fixed up.
//
// handheld: kept sorted by begin, exact duplicates dropped (BattleShip appended
// on every sprite creation; duplicates never change the clip result).
// Cleared at scene change via portResetStructFixups.
#define PROTECTED_RANGE_CAP 4096
static Span sProtectedStructRanges[PROTECTED_RANGE_CAP];
static uint32_t sProtectedStructRangesCount;
static uintptr_t sProtectedStructMaxLen;

// Separate tracking for post-decode 4c sprite deswizzle (portDeswizzleDecodedSprite4c).
// Needs its own set because portFixupSpriteBitmapData already inserted the same buf
// addresses during the pre-decode BSWAP32 pass.
ADDRSET_DEFINE(sDeswizzle4cFixups, 1 << 13);

/* Absolute addresses of every relocated reloc chain slot currently live.
 * Texel data never contains chain slots, so a runtime texture fixup whose
 * range covers one is aimed at non-texture data (a stale or garbage
 * SETTIMG operand) and must be refused — applying it BSWAP32s live
 * descriptor and pointer words in place. Observed cascade (BattleShip): a
 * DObjDesc array in a fighter main file wholesale byte-reversed mid-scene,
 * its sentinel unrecognizable, item spawn walks running off the array into
 * float data → textureless objects and SIGSEGV in
 * gcSetupCustomDObjsWithMObj. */
ADDRSET_DEFINE(sChainSlotAddrs, 1 << 17);

void portRelocNoteChainSlot(const void *slot)
{
	addrset_insert(&sChainSlotAddrs, (uintptr_t)slot);
}

/* ---- sorted span helpers (sTexFixupWords) ---- */

/* First index whose end > addr (spans are disjoint and sorted, so ends are too). */
static uint32_t span_first_end_after(uintptr_t addr)
{
	uint32_t lo = 0, hi = sTexFixupWordsCount;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (sTexFixupWords[mid].end > addr) hi = mid;
		else lo = mid + 1;
	}
	return lo;
}

static void span_insert(uintptr_t b, uintptr_t e)
{
	if (b >= e) return;
	/* First span that overlaps or touches [b, e): end >= b. */
	uint32_t i = (b == 0) ? 0 : span_first_end_after(b - 1);
	uint32_t j = i;
	uintptr_t nb = b, ne = e;
	while (j < sTexFixupWordsCount && sTexFixupWords[j].begin <= e) {
		if (sTexFixupWords[j].begin < nb) nb = sTexFixupWords[j].begin;
		if (sTexFixupWords[j].end > ne) ne = sTexFixupWords[j].end;
		j++;
	}
	if (j == i) {
		if (sTexFixupWordsCount >= TEX_WORD_SPAN_CAP) {
			static int warned;
			if (!warned) {
				warned = 1;
				port_log("SSB64: sTexFixupWords full (%u spans); runtime texture "
				         "idempotency lost — raise TEX_WORD_SPAN_CAP\n",
				         (unsigned)TEX_WORD_SPAN_CAP);
			}
			return;
		}
		memmove(&sTexFixupWords[i + 1], &sTexFixupWords[i],
		        (sTexFixupWordsCount - i) * sizeof(Span));
		sTexFixupWordsCount++;
	} else if (j > i + 1) {
		memmove(&sTexFixupWords[i + 1], &sTexFixupWords[j],
		        (sTexFixupWordsCount - j) * sizeof(Span));
		sTexFixupWordsCount -= (j - i - 1);
	}
	sTexFixupWords[i].begin = nb;
	sTexFixupWords[i].end = ne;
}

static void span_remove_range(uintptr_t lo, uintptr_t hi)
{
	if (lo >= hi) return;
	uint32_t i = span_first_end_after(lo);
	if (i >= sTexFixupWordsCount || sTexFixupWords[i].begin >= hi)
		return;
	if (sTexFixupWords[i].begin < lo && sTexFixupWords[i].end > hi) {
		/* Split one span into two. */
		if (sTexFixupWordsCount >= TEX_WORD_SPAN_CAP) {
			/* No room: dropping the tail only loses idempotency marks,
			 * which is the safe direction for an eviction. */
			sTexFixupWords[i].end = lo;
			return;
		}
		memmove(&sTexFixupWords[i + 1], &sTexFixupWords[i],
		        (sTexFixupWordsCount - i) * sizeof(Span));
		sTexFixupWordsCount++;
		sTexFixupWords[i].end = lo;
		sTexFixupWords[i + 1].begin = hi;
		return;
	}
	if (sTexFixupWords[i].begin < lo) {
		sTexFixupWords[i].end = lo;
		i++;
	}
	uint32_t k = i;
	while (k < sTexFixupWordsCount && sTexFixupWords[k].end <= hi)
		k++;
	if (k < sTexFixupWordsCount && sTexFixupWords[k].begin < hi)
		sTexFixupWords[k].begin = hi;
	if (k > i) {
		memmove(&sTexFixupWords[i], &sTexFixupWords[k],
		        (sTexFixupWordsCount - k) * sizeof(Span));
		sTexFixupWordsCount -= (k - i);
	}
}

/* ---- protected struct ranges ---- */

/* First index whose begin >= addr. */
static uint32_t protected_lower_bound(uintptr_t addr)
{
	uint32_t lo = 0, hi = sProtectedStructRangesCount;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (sProtectedStructRanges[mid].begin < addr) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

static void portRegisterProtectedStructRange(const void *begin, size_t size)
{
	if (begin == NULL || size == 0) return;
	uintptr_t b = (uintptr_t)begin;
	uintptr_t e = b + size;
	uint32_t i = protected_lower_bound(b);
	for (uint32_t k = i; k < sProtectedStructRangesCount &&
	                     sProtectedStructRanges[k].begin == b; k++) {
		if (sProtectedStructRanges[k].end == e)
			return; /* already registered */
	}
	if (sProtectedStructRangesCount >= PROTECTED_RANGE_CAP) {
		static int warned;
		if (!warned) {
			warned = 1;
			port_log("SSB64: sProtectedStructRanges full (%u); raise PROTECTED_RANGE_CAP\n",
			         (unsigned)PROTECTED_RANGE_CAP);
		}
		return;
	}
	memmove(&sProtectedStructRanges[i + 1], &sProtectedStructRanges[i],
	        (sProtectedStructRangesCount - i) * sizeof(Span));
	sProtectedStructRanges[i].begin = b;
	sProtectedStructRanges[i].end = e;
	sProtectedStructRangesCount++;
	if (size > sProtectedStructMaxLen)
		sProtectedStructMaxLen = size;
}

// F3DEX2 GBI opcodes (from gbi.h with F3DEX_GBI_2 defined)
#define GBI_VTX         0x01
#define GBI_DL          0xDE
#define GBI_ENDDL       0xDF
#define GBI_SETTIMG     0xFD
#define GBI_LOADBLOCK   0xF3
#define GBI_LOADTLUT    0xF0

// Texture size constants (bits per pixel)
#define SIZ_4b     0
#define SIZ_8b     1
#define SIZ_16b    2
#define SIZ_32b    3

// Segment ID used for intra-file display list references
#define FILE_SEGMENT_ID 0x0E

// Fixup types for marked regions
typedef enum RegionFixup
{
	FIXUP_VERTEX,       // Per-Vtx: rotate16 words 0-2, bswap32 word 3
	FIXUP_TEX_BYTES,    // 4bpp/8bpp texture: undo u32 swap (bswap32 back)
	FIXUP_TEX_U16,      // 16bpp texture or palette: rotate16 each word
	// 32bpp: no fixup needed (u32 swap was correct)
} RegionFixup;

typedef struct FixupRegion
{
	uint32_t byte_offset;
	uint32_t byte_size;
	RegionFixup type;
} FixupRegion;

/* Pass2 scratch: regions found in one blob (the whole blob is scanned before
 * any region is modified, as in BattleShip). */
#define PASS2_REGION_CAP 8192
static FixupRegion sPass2Regions[PASS2_REGION_CAP];
static uint32_t sPass2RegionCount;

static void push_region(uint32_t off, uint32_t size, RegionFixup type)
{
	if (sPass2RegionCount >= PASS2_REGION_CAP) {
		static int warned;
		if (!warned) {
			warned = 1;
			port_log("SSB64: pass2 region list full (%u); raise PASS2_REGION_CAP\n",
			         (unsigned)PASS2_REGION_CAP);
		}
		return;
	}
	sPass2Regions[sPass2RegionCount].byte_offset = off;
	sPass2Regions[sPass2RegionCount].byte_size = size;
	sPass2Regions[sPass2RegionCount].type = type;
	sPass2RegionCount++;
}

// ============================================================
//  Pass 1: Blanket u32 swap
// ============================================================

static void pass1_swap_u32(void *data, size_t size)
{
	uint32_t *words = (uint32_t *)data;
	size_t count = size / 4;

	for (size_t i = 0; i < count; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

// ============================================================
//  Pass 2: DL-guided fixup scan
// ============================================================

static void scan_display_lists(const uint32_t *words, size_t num_words, size_t file_size)
{
	// Pending texture image state from the most recent gDPSetTextureImage
	bool has_pending_tex = false;
	uint32_t pending_tex_offset = 0;
	uint32_t pending_tex_siz = 0;

	for (size_t i = 0; i + 1 < num_words; i += 2)
	{
		uint32_t w0 = words[i];
		uint32_t w1 = words[i + 1];
		uint8_t opcode = (w0 >> 24) & 0xFF;

		switch (opcode)
		{
		case GBI_VTX:
		{
			uint32_t num_vtx = (w0 >> 12) & 0xFF;
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t offset = w1 & 0x00FFFFFF;
			size_t offset_sz = (size_t)offset;
			size_t vtx_bytes = (size_t)num_vtx * 16;

			if (seg == FILE_SEGMENT_ID && num_vtx > 0 &&
			    offset_sz <= file_size && vtx_bytes <= (file_size - offset_sz))
			{
				push_region(offset, num_vtx * 16, FIXUP_VERTEX);
			}
			break;
		}

		case GBI_SETTIMG:
		{
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t siz = (w0 >> 19) & 0x03;

			if (seg == FILE_SEGMENT_ID)
			{
				pending_tex_offset = w1 & 0x00FFFFFF;
				pending_tex_siz = siz;
				has_pending_tex = true;
			}
			else
			{
				has_pending_tex = false;
			}
			break;
		}

		case GBI_LOADBLOCK:
		{
			if (!has_pending_tex)
				break;

			uint32_t lrs = (w1 >> 12) & 0xFFF;
			uint32_t num_texels = lrs + 1;
			uint32_t bpp = 0;

			switch (pending_tex_siz)
			{
			case SIZ_4b:  bpp = 4;  break;
			case SIZ_8b:  bpp = 8;  break;
			case SIZ_16b: bpp = 16; break;
			case SIZ_32b: bpp = 32; break;
			}

			if (bpp == 0)
				break;

			uint32_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Align to 4 bytes for u32 word processing
			tex_bytes = (tex_bytes + 3) & ~3u;

			size_t pending_offset_sz = (size_t)pending_tex_offset;
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if ((size_t)tex_bytes > (file_size - pending_offset_sz))
				tex_bytes = (uint32_t)(file_size - pending_offset_sz);

			switch (pending_tex_siz)
			{
			case SIZ_4b:
			case SIZ_8b:
				push_region(pending_tex_offset, tex_bytes, FIXUP_TEX_BYTES);
				break;
			case SIZ_16b:
				push_region(pending_tex_offset, tex_bytes, FIXUP_TEX_U16);
				break;
			default:
				// 32b: u32 swap was correct, no fixup
				break;
			}
			has_pending_tex = false;
			break;
		}

		case GBI_LOADTLUT:
		{
			if (!has_pending_tex)
				break;

			uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
			uint32_t palette_bytes = count * 2;
			// Align to 4 bytes
			palette_bytes = (palette_bytes + 3) & ~3u;

			size_t pending_offset_sz = (size_t)pending_tex_offset;
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if ((size_t)palette_bytes > (file_size - pending_offset_sz))
				palette_bytes = (uint32_t)(file_size - pending_offset_sz);

			push_region(pending_tex_offset, palette_bytes, FIXUP_TEX_U16);
			has_pending_tex = false;
			break;
		}

		default:
			break;
		}
	}
}

// ============================================================
//  Fixup application
// ============================================================

// Rotate each u32 word by 16 bits: converts u32-swapped u16 pairs
// to correctly u16-swapped data.
// After u32 swap, BE bytes [A B C D] became [D C B A].
// rotate16 produces [B A D C] which is correct u16 LE for (AB, CD).
static inline uint32_t rotate16(uint32_t w)
{
	return (w << 16) | (w >> 16);
}

static void apply_fixup_vertex(uint32_t *words, size_t num_words)
{
	// Vtx is 4 u32 words (16 bytes):
	//   Word 0: s16 ob[0] | s16 ob[1]   -> rotate16
	//   Word 1: s16 ob[2] | u16 flag     -> rotate16
	//   Word 2: s16 tc[0] | s16 tc[1]    -> rotate16
	//   Word 3: u8 cn[0-3]               -> bswap32 (restore byte order)
	for (size_t i = 0; i + 3 < num_words; i += 4)
	{
		words[i + 0] = rotate16(words[i + 0]);
		words[i + 1] = rotate16(words[i + 1]);
		words[i + 2] = rotate16(words[i + 2]);
		words[i + 3] = BSWAP32(words[i + 3]);
	}
}

static void apply_fixup_tex_bytes(uint32_t *words, size_t num_words)
{
	// 4bpp/8bpp texture data: byte-granular pixels.
	// u32 swap reversed byte order within each 4-byte group.
	// Undo by swapping back to original byte order.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixup_tex_u16(uint32_t *words, size_t num_words)
{
	// 16bpp texture/palette data: u16 pixel values stored in N64 BE byte order.
	// Pass1 BSWAP32 reversed the byte order within each u32; the renderer's
	// RGBA16 importer reads texels as `(addr[0] << 8) | addr[1]` (BE u16),
	// so we must restore the original BE byte layout.  BSWAP32 undoes pass1.
	//
	// Earlier this used rotate16 which only swapped the two 16-bit halves —
	// that's correct for `[s16][s16]` vertex pair fields but wrong for a stream
	// of BE-read u16 texels.  Most non-sprite RGBA16 textures (room walls, etc.)
	// surfaced as all-near-black pixels until this was switched to BSWAP32.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixups(void *data, size_t file_size)
{
	uint8_t *bytes = (uint8_t *)data;

	for (uint32_t r = 0; r < sPass2RegionCount; r++)
	{
		const FixupRegion *region = &sPass2Regions[r];
		size_t start = (size_t)region->byte_offset;
		size_t len = (size_t)region->byte_size;

		// Bounds check using size_t math to prevent 32-bit overflow.
		if (start > file_size || len > (file_size - start))
			continue;

		// We reinterpret as u32 words, so both start and length must be word-aligned.
		if ((start & 3) != 0)
			continue;
		len &= ~(size_t)3;
		if (len == 0)
			continue;

		uint32_t *region_words = (uint32_t *)(bytes + start);
		size_t num_words = len / 4;

		switch (region->type)
		{
		case FIXUP_VERTEX:
			apply_fixup_vertex(region_words, num_words);
			// Per-vertex registration so the runtime lazy fixup
			// (portRelocFixupVertexAtRuntime) skips each Vtx
			// individually — handles overlapping sub-loads.
			{
				uintptr_t base = (uintptr_t)region_words;
				size_t n_vtx = num_words / 4;  // 4 u32 words per Vtx
				for (size_t v = 0; v < n_vtx; v++) {
					addrset_insert(&sStructU16Fixups, base + v * 16);
				}
			}
			break;
		case FIXUP_TEX_BYTES:
			apply_fixup_tex_bytes(region_words, num_words);
			break;
		case FIXUP_TEX_U16:
			apply_fixup_tex_u16(region_words, num_words);
			break;
		}
	}
}

// ============================================================
//  Public API
// ============================================================

void portRelocByteSwapBlob(void *data, size_t size, unsigned int file_id)
{
	(void)file_id;
	if (data == NULL || size < 4)
		return;

	// Pass 1: blanket u32 swap
	pass1_swap_u32(data, size);

	// Pass 2: DL-guided fixup
	size_t num_words = size / 4;
	const uint32_t *words = (const uint32_t *)data;

	sPass2RegionCount = 0;
	scan_display_lists(words, num_words, size);

	if (sPass2RegionCount != 0)
	{
		apply_fixups(data, size);
	}
	sPass2RegionCount = 0;
}

// ============================================================
//  Struct u16 fixup — rotate16 for u16 fields in ROM structs
// ============================================================

void portFixupStructU16(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = (uintptr_t)base + byte_offset;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *words = (uint32_t *)((uint8_t *)base + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = (words[i] << 16) | (words[i] >> 16);
	}
}

// Undo the blanket pass1 u32 byteswap for a single-u8-inside-a-u32 field.
// After pass1, a BE ROM u8 at struct offset N lives at native offset (N|3);
// reversing the containing u32 word puts the u8 back at offset N where the
// C struct definition expects to read it.  Idempotent via sStructU16Fixups.
void portFixupStructU32(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = (uintptr_t)base + byte_offset;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *words = (uint32_t *)((uint8_t *)base + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

void portResetStructFixups(void)
{
	addrset_clear(&sStructU16Fixups);
	addrset_clear(&sTexFixupExtent);
	sTexFixupWordsCount = 0;
	sProtectedStructRangesCount = 0;
	sProtectedStructMaxLen = 0;
	addrset_clear(&sDeswizzle4cFixups);
	addrset_clear(&sChainSlotAddrs);
}

void portEvictStructFixupsInRange(const void *begin, size_t size)
{
	if (begin == NULL || size == 0)
		return;

	uintptr_t lo = (uintptr_t)begin;
	uintptr_t hi = lo + size;

	addrset_remove_range(&sStructU16Fixups, lo, hi);
	span_remove_range(lo, hi);
	addrset_remove_range(&sDeswizzle4cFixups, lo, hi);
	addrset_remove_range(&sChainSlotAddrs, lo, hi);
	addrset_remove_range(&sTexFixupExtent, lo, hi);

	uint32_t out = 0;
	uintptr_t maxlen = 0;
	for (uint32_t i = 0; i < sProtectedStructRangesCount; i++)
	{
		Span r = sProtectedStructRanges[i];
		if (r.begin < hi && r.end > lo)
			continue;
		sProtectedStructRanges[out++] = r;
		if (r.end - r.begin > maxlen)
			maxlen = r.end - r.begin;
	}
	sProtectedStructRangesCount = out;
	sProtectedStructMaxLen = maxlen;
}

// For raw texel blobs loaded via a runtime-built SETTIMG (where pass2's
// DL scan never saw the SETTIMG inside a stored display list) — apply
// BSWAP32 again to restore N64 BE byte order that pass1 destroyed.
// Tracked by base so repeat calls on the same blob are no-ops.
void portFixupRawTextureBSWAP32(void *base, size_t bytes)
{
	if (base == NULL || bytes == 0)
		return;

	uintptr_t key = (uintptr_t)base;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	// Round down to full u32s — pass1 only touched 4-byte-aligned words,
	// so any trailing tail bytes were never swapped and shouldn't be now.
	size_t num_words = bytes / 4;
	uint32_t *words = (uint32_t *)base;
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

// ============================================================
//  Chain-walk fixup (textures and vertices)
// ============================================================
//
// Pass2's seg==0x0E filter only catches a tiny minority of in-file textures
// and vertices.  SSB64 model DLs reference these via real-pointer slots that
// the reloc chain rewrites at load time.  Pass2 sees those slots BEFORE
// rewrite — they contain chain encoding (random-looking high bytes), not
// seg=0x0E references, so they get missed.
//
// Workaround: during the chain walk, for each chain entry whose preceding
// cmd is a G_SETTIMG or G_VTX, apply the appropriate fixup using the chain
// encoding's words_num field as the in-file target offset.  The chain knows
// the truth because it's an explicit list of "slots that need fixup" —
// no false positives possible.
//
// Args:
//   file_base       — start of the file in memory (post-pass1)
//   file_size       — size of the file blob
//   slot_byte_off   — byte offset of the cmd's w1 slot (the slot the
//                     chain entry refers to)
//   target_byte_off — byte offset of the target data within the same file
//                     (computed from chain entry's words_num * 4)
//
// Returns 1 if the slot was at a G_SETTIMG/G_VTX and a fixup was applied;
// 0 otherwise.

static int chain_fixup_settimg(void *file_base, size_t file_size,
                               unsigned int slot_byte_off,
                               unsigned int target_byte_off,
                               uint32_t w0)
{
	const uint8_t *file_bytes = (const uint8_t *)file_base;

	uint32_t siz = (w0 >> 19) & 0x03;

	// Walk forward up to 8 cmds (64 bytes) to find G_LOADBLOCK or G_LOADTLUT.
	uint32_t loadblock_w1 = 0;
	uint32_t loadtlut_w1  = 0;
	int      found_load   = 0;
	for (int step = 1; step <= 8; step++)
	{
		size_t walk_off = (size_t)slot_byte_off - 4 + (size_t)(step * 8);
		if (walk_off + 8 > file_size) break;
		uint32_t walk_w0 = *(const uint32_t *)(file_bytes + walk_off);
		uint32_t walk_w1 = *(const uint32_t *)(file_bytes + walk_off + 4);
		uint8_t  walk_op = (uint8_t)(walk_w0 >> 24);
		if (walk_op == GBI_LOADBLOCK)
		{
			loadblock_w1 = walk_w1;
			found_load = 1;
			break;
		}
		if (walk_op == GBI_LOADTLUT)
		{
			loadtlut_w1 = walk_w1;
			found_load = 2;
			break;
		}
		if (walk_op == GBI_SETTIMG) return 0;
	}
	if (!found_load) return 0;

	uint32_t tex_bytes = 0;
	if (found_load == 1)
	{
		uint32_t lrs = (loadblock_w1 >> 12) & 0xFFF;
		uint32_t num_texels = lrs + 1;
		uint32_t bpp = (siz == SIZ_4b)  ? 4
		             : (siz == SIZ_8b)  ? 8
		             : (siz == SIZ_16b) ? 16
		             : (siz == SIZ_32b) ? 32
		             : 0;
		if (bpp == 0) return 0;
		tex_bytes = (num_texels * bpp + 7) / 8;
	}
	else
	{
		uint32_t count = ((loadtlut_w1 >> 14) & 0x3FF) + 1;
		tex_bytes = count * 2;
	}
	tex_bytes = (tex_bytes + 3) & ~3u;
	if (tex_bytes == 0) return 0;
	if ((size_t)target_byte_off + tex_bytes > file_size)
	{
		tex_bytes = (uint32_t)(file_size - target_byte_off);
		tex_bytes &= ~3u;
		if (tex_bytes == 0) return 0;
	}

	uintptr_t fixup_key = (uintptr_t)file_base + (uintptr_t)target_byte_off;
	if (addrset_contains(&sStructU16Fixups, fixup_key)) return 1;
	addrset_insert(&sStructU16Fixups, fixup_key);

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);
	size_t num_words = tex_bytes / 4;

	// All formats need BSWAP32 to restore N64 BE byte order:
	//  - 4b/8b: per-byte data; the renderer reads bytes in their original order
	//  - 16b: pixels are BE u16; read as `(addr[0] << 8) | addr[1]`
	//  - 32b: pixels are BE [R,G,B,A]; read addr[0]=R, addr[1]=G, addr[2]=B,
	//         addr[3]=A
	// Pass1 BSWAP32 reversed the byte order within each u32, so a second
	// BSWAP32 restores the original layout for ALL of these formats.
	//
	// BTT/BTP arrow palette: we must record this fix in sTexFixupExtent and
	// sTexFixupWords too. Without those, the runtime LOADTLUT path's
	// early-skip — which fires when a target appears in sStructU16Fixups but
	// NOT sTexFixupExtent — locks out runtime fixup on this target. That
	// matters when the runtime LOADTLUT loads MORE bytes than chain just
	// fixed (e.g. a count=3 partial palette where chain only saw a count=1
	// LOADTLUT in its 8-cmd window): bytes beyond the chain-fixed range stay
	// in pass1-BSWAP state and the arrow's palette[2] reads as 0xC17B
	// (magenta) instead of 0x0001 (black). With these two records, the
	// runtime path now passes the early-skip and falls through to the
	// per-word loop, which skips already-fixed words via sTexFixupWords and
	// BSWAPs the rest.
	uintptr_t reg_base = (uintptr_t)region;
	for (size_t i = 0; i < num_words; i++) {
		region[i] = BSWAP32(region[i]);
	}
	span_insert(reg_base, reg_base + num_words * 4);
	if (addrmap_get(&sTexFixupExtent, reg_base) < tex_bytes)
		addrmap_set(&sTexFixupExtent, reg_base, tex_bytes);

	return 1;
}

static int chain_fixup_vertex(void *file_base, size_t file_size,
                              unsigned int slot_byte_off,
                              unsigned int target_byte_off,
                              uint32_t w0)
{
	(void)slot_byte_off;
	// G_VTX layout in F3DEX2:
	//   w0[31:24] = 0x01  (G_VTX opcode)
	//   w0[23:20] = 0     (reserved)
	//   w0[19:12] = num_vtx (n), 8 bits, practically 1..32
	//   w0[11:1]  = (v0 + n) << 1, end vertex slot index in TMEM
	//   w0[0]     = 0     (must be even because of << 1)
	//   w1        = vertex array address
	//
	// Each Vtx is 16 bytes:
	//   word 0: s16 ob[0]  | s16 ob[1]   → rotate16
	//   word 1: s16 ob[2]  | u16 flag    → rotate16
	//   word 2: s16 tc[0]  | s16 tc[1]   → rotate16
	//   word 3: u8  cn[0..3]              → bswap32 (restore byte order)
	//
	// STRICT VALIDATION: opcode 0x01 is too common to trust by itself.
	// Many non-cmd u32 words start with 0x01.  Reject anything where the
	// reserved/structural bits don't match the spec.

	// bits [23:20] must be zero (reserved)
	if ((w0 & 0x00F00000) != 0) return 0;
	// bit [0] must be zero (the count is multiplied by 2)
	if ((w0 & 0x1) != 0) return 0;

	uint32_t num_vtx = (w0 >> 12) & 0xFF;
	uint32_t end_idx = (w0 & 0xFFE) >> 1;

	// Sanity-check num_vtx and the destination range.  Real game DLs use
	// 1..32 vertices per G_VTX (Vtx buffer is 32 entries on N64).
	if (num_vtx == 0 || num_vtx > 32) return 0;
	if (end_idx > 32 || end_idx < num_vtx) return 0;

	size_t total_bytes = (size_t)num_vtx * 16;
	if ((size_t)target_byte_off + total_bytes > file_size) return 0;

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);

	// Content-based sanity check on the FIRST vertex.
	// Post-pass1 word 0 (high 16 bits) = original BE ob[0] (s16),
	// (low 16 bits) = original BE ob[1] (s16). Word 1 likewise has
	// ob[2] and flag. Real game vertex coords observed up to ±4200,
	// flag is always 0. Reject anything with much larger coords or
	// nonzero flag — those are false-positive struct-field chain
	// entries whose preceding 4 bytes happen to look like a G_VTX cmd.
	{
		int16_t ob0 = (int16_t)((region[0] >> 16) & 0xFFFF);
		int16_t ob1 = (int16_t)(region[0] & 0xFFFF);
		int16_t ob2 = (int16_t)((region[1] >> 16) & 0xFFFF);
		uint16_t flag = (uint16_t)(region[1] & 0xFFFF);
		const int kMaxCoord = 16384;
		if (ob0 < -kMaxCoord || ob0 > kMaxCoord) return 0;
		if (ob1 < -kMaxCoord || ob1 > kMaxCoord) return 0;
		if (ob2 < -kMaxCoord || ob2 > kMaxCoord) return 0;
		if (flag != 0) return 0;
	}

	// Per-vertex idempotency (matches portRelocFixupVertexAtRuntime).
	uintptr_t target_addr = (uintptr_t)file_base + (uintptr_t)target_byte_off;
	for (uint32_t i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target_addr + (uintptr_t)i * 16;
		if (addrset_contains(&sStructU16Fixups, vtx_key)) continue;
		addrset_insert(&sStructU16Fixups, vtx_key);

		region[i * 4 + 0] = rotate16(region[i * 4 + 0]);
		region[i * 4 + 1] = rotate16(region[i * 4 + 1]);
		region[i * 4 + 2] = rotate16(region[i * 4 + 2]);
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
	return 1;
}

int portRelocFixupTextureFromChain(void *file_base, size_t file_size,
                                   unsigned int slot_byte_off,
                                   unsigned int target_byte_off)
{
	if (file_base == NULL || slot_byte_off < 4 ||
	    (size_t)(slot_byte_off + 4) > file_size ||
	    (size_t)target_byte_off >= file_size)
	{
		return 0;
	}

	const uint8_t *file_bytes = (const uint8_t *)file_base;

	/* The w0 candidate at slot-4 may be a slot this same walk already
	 * relocated: chain entries are frequently adjacent words, so the
	 * "command" preceding this slot is the previous entry's freshly-written
	 * pointer. Such a word can pass the opcode and structural checks (top
	 * byte 0x01 = G_VTX or 0xFD = SETTIMG, reserved bits zero, plausible
	 * num_vtx). Parsing it as a command applies vertex/texture byte
	 * permutations over live chain descriptors — observed in BattleShip as
	 * a misparse that rot16'd five descriptors in
	 * reloc_effects/EFCommonEffects2 and derailed the chain walk. A live
	 * relocated slot is never a real command word — skip. Corpse entries
	 * (address reused, word no longer a live pointer) are pruned so a stale
	 * registry can't suppress real fixups. */
	{
		uintptr_t w0_addr = (uintptr_t)(file_bytes + slot_byte_off) - 4;
		if (addrset_contains(&sChainSlotAddrs, w0_addr))
		{
			uint32_t w0_word = *(const uint32_t *)w0_addr;
			if (chain_slot_word_is_live(w0_word))
				return 0;
			addrset_erase_one(&sChainSlotAddrs, w0_addr);
		}
	}

	// The cmd's w0 is at slot_byte_off - 4, w1 is at slot_byte_off.
	uint32_t w0 = *(const uint32_t *)(file_bytes + slot_byte_off - 4);
	uint8_t opcode = (uint8_t)(w0 >> 24);

	if (opcode == GBI_SETTIMG)
	{
		return chain_fixup_settimg(file_base, file_size, slot_byte_off,
		                           target_byte_off, w0);
	}
	if (opcode == GBI_VTX)
	{
		// Filter: real packed cmd w1 must be at 8*N+4 alignment.
		if ((slot_byte_off & 0x7) != 4) return 0;
		return chain_fixup_vertex(file_base, file_size, slot_byte_off, target_byte_off, w0);
	}
	return 0;
}

// ============================================================
//  Lazy runtime fixups
// ============================================================

// Lazy texture byte-order fixup at G_LOADBLOCK / G_LOADTLUT execute time.
// Called by the renderer once both the texture address (set by G_SETTIMG)
// and the texel count are known.
//
// This catches textures referenced by RUNTIME-BUILT DLs that pass2 doesn't
// see (no in-file SETTIMG/LOADBLOCK pair) and that the chain walk also
// doesn't see (the slot was looked up via an MObjSub field, not a chain
// entry whose preceding cmd is SETTIMG).  Fighter material textures are
// the most common example: lbCommonAddMObjForFighterPartsDObj walks the
// fighter's MObjSub array at draw time and builds the SETTIMG/LOADBLOCK
// pairs in heap, never embedding them in any file DL.
//
// The texel readers expect bytes in N64 BE byte order:
//   4b/8b: per-byte data, addr[0] is the first pixel
//   16b: pixels are BE u16, read via `(addr[0] << 8) | addr[1]`
//   32b: pixels are BE [R,G,B,A], read addr[0]=R, addr[1]=G, ...
// Pass1 BSWAP32 reversed the byte order within each u32 word, so we apply
// BSWAP32 again to restore the original layout.
//
// Idempotent at word granularity so overlapping runtime loads from different
// starts cannot undo each other.
void portRelocFixupTextureAtRuntime(const void *addr, unsigned int num_bytes)
{
	if (addr == NULL || num_bytes == 0) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built textures
	// (procedurally generated, copied from elsewhere, etc.) are already in
	// correct host byte order.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	if (!portRelocFindContainingFile(addr, &fileBase, &fileSize))
		return;

	uintptr_t target = (uintptr_t)addr;
	size_t    target_offset = target - fileBase;
	if (target_offset >= fileSize)
	{
		return;
	}
	size_t available_bytes = fileSize - target_offset;
	if ((size_t)num_bytes > available_bytes)
	{
		num_bytes = (unsigned int)available_bytes;
	}

	// If pass2 or the chain walk already fixed this exact base and runtime has
	// never seen it, keep the old conservative skip. Runtime-owned fixups below
	// are tracked per word to handle overlapping TLUT/LOADBLOCK ranges.
	// (handheld: hoisted above the side-effect-free clipping below; same result.)
	if (addrmap_get(&sTexFixupExtent, target) == 0 &&
	    addrset_contains(&sStructU16Fixups, target))
	{
		return;
	}

	// Clip the fix range so it can't enter a previously-registered protected
	// struct array (bitmap array, sprite, mobjsub, ...). These are already in
	// native byte order and a blind BSWAP32 pass would corrupt them. The
	// typical offender is a CI8 palette load that the renderer treats as a
	// fixed 512-byte read even when the actual palette is smaller; the extra
	// bytes happen to overlap the bitmap/sprite struct region on sprite files.
	//
	// Overlap rule: if a protected range starts *after* target, clip so we
	// still fix the prefix that lives outside the struct. If a protected
	// range starts at or before target, the whole range overlaps a struct and
	// we skip it.
	if (sProtectedStructRangesCount != 0)
	{
		uintptr_t clip_end = target + num_bytes;
		uintptr_t scan_from = (target > sProtectedStructMaxLen) ? target - sProtectedStructMaxLen : 0;
		for (uint32_t i = protected_lower_bound(scan_from);
		     i < sProtectedStructRangesCount; i++)
		{
			const Span *r = &sProtectedStructRanges[i];
			if (r->begin >= clip_end) break;
			if (r->end <= target) continue;
			if (r->begin > target)
				clip_end = r->begin;   /* sorted: first such is the minimum */
			else
				clip_end = target;
			break;
		}
		num_bytes = (clip_end > target) ? (unsigned int)(clip_end - target) : 0u;
	}

	// Round UP to next 4-byte boundary, then clamp to file bounds. A CI4
	// LOADTLUT with count=3 calls in with num_bytes=6, which under a
	// round-DOWN became 4 — leaving the second 4-byte word (containing
	// palette[2]'s low byte) in pass1-BSWAP state. The N64 LOADTLUT only
	// copies 6 bytes into TMEM, but pass1 mangled the whole containing
	// word, so we must un-mangle the whole word for the loaded portion to
	// read correctly. Bounds-clamped because rounding up can't validly
	// extend past file end.
	num_bytes = (unsigned int)(((size_t)num_bytes + 3u) & ~(size_t)3u);
	if ((size_t)num_bytes > available_bytes)
		num_bytes = (unsigned int)(available_bytes & ~(size_t)3u);
	if (num_bytes == 0) return;

	// Fast path (handheld): every word already fixed → nothing below would
	// change (the per-word loop would skip all of them).
	{
		uint32_t i = span_first_end_after(target);
		if (i < sTexFixupWordsCount && sTexFixupWords[i].begin <= target &&
		    sTexFixupWords[i].end >= target + num_bytes)
			return;
	}

	/* Clamp the swap at the first relocated chain slot in the range.
	 *
	 * N64 LoadBlock/LoadTLUT legitimately over-read past the true texel
	 * span (fixed block sizes); on hardware the extra bytes only landed in
	 * TMEM and were never written back. This fixup MUTATES source memory,
	 * so swapping the over-read tail byte-reverses live pointers and struct
	 * words in place — observed as fighter-file DObjDesc arrays wholesale
	 * BSWAP32'd, cascading into textureless objects and SIGSEGV in the desc
	 * walk.
	 *
	 * Texel data never contains chain slots, so the first slot marks the
	 * end of trustworthy texture bytes: swap up to it, leave the rest.
	 * The clamped tail stays in its live (already correct) byte order and
	 * the GPU merely samples a few nonsense over-read texels the original
	 * hardware also displayed as garbage-in-TMEM. */
	if (sChainSlotAddrs.count != 0)
	{
		uintptr_t scan_end = target + num_bytes;
		for (uintptr_t w = target; w < scan_end; w += 4)
		{
			if (!addrset_contains(&sChainSlotAddrs, w))
				continue;
			/* Registry entries can outlive their file epoch: a reload at
			 * the same address (force-heap rewind, bump reuse) evicts only
			 * the NEW file's smaller range, leaving corpse addresses from
			 * the old file's tail. A live slot must hold a pointer into a
			 * loaded file; anything else is a corpse — drop it and keep
			 * the texture swap intact (clamping on a corpse truncates
			 * real texels and renders the texture garbled). */
			uint32_t slot_word = *(const uint32_t *)w;
			if (!chain_slot_word_is_live(slot_word))
			{
				addrset_erase_one(&sChainSlotAddrs, w);
				continue;
			}
			static unsigned int sClampLogCount = 0;
			if (sClampLogCount < 64)
			{
				int cl_file_id = portRelocFindFileIdAndBase(addr, NULL);
				port_log("SSB64: runtimeTexFix CLAMPED addr=%p num=0x%x->0x%x file=%d — "
				         "live reloc chain slot at %p bounds the texel span\n",
				         addr, num_bytes, (unsigned int)(w - target), cl_file_id, (void *)w);
				sClampLogCount++;
			}
			num_bytes = (unsigned int)(w - target);
			break;
		}
		if (num_bytes == 0)
			return;
	}

	/* Per-word loop, walking the span list instead of one lookup per word. */
	uint32_t *region = (uint32_t *)target;
	size_t num_words = num_bytes / 4;
	uintptr_t end = target + (uintptr_t)num_words * 4;
	size_t fixed_words = 0;
	uintptr_t w = target;
	while (w < end)
	{
		uint32_t si = span_first_end_after(w);
		if (si < sTexFixupWordsCount && sTexFixupWords[si].begin <= w)
		{
			/* Skip words already fixed: advance to the first word key at
			 * or past the span end. */
			uintptr_t se = sTexFixupWords[si].end;
			w = target + ((se - target + 3) & ~(uintptr_t)3);
			continue;
		}
		uintptr_t stop = (si < sTexFixupWordsCount && sTexFixupWords[si].begin < end)
		               ? sTexFixupWords[si].begin : end;
		for (; w < stop; w += 4)
		{
			size_t idx = (w - target) / 4;
			region[idx] = BSWAP32(region[idx]);
			fixed_words++;
		}
	}

	if (fixed_words == 0)
		return;

	span_insert(target, end);
	addrset_insert(&sStructU16Fixups, target);
	if (addrmap_get(&sTexFixupExtent, target) < num_bytes)
		addrmap_set(&sTexFixupExtent, target, num_bytes);
}

// Called from the renderer's G_VTX handler with the resolved vertex array
// address and the cmd's num_vtx.  If the address is inside a reloc file
// (i.e., not heap-built data), apply the per-Vtx byte permutation
// idempotently.
//
// Why this is correct: only data the running interpreter actually treats as
// vertices reaches this function — there's no guessing.  Each target's "type"
// is unambiguously vertex because a real G_VTX cmd just dispatched it.
void portRelocFixupVertexAtRuntime(const void *addr, unsigned int num_vtx)
{
	if (addr == NULL || num_vtx == 0 || num_vtx > 32) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built vertex
	// arrays (e.g., dynamic UI sprites) are constructed by game code with
	// already-correct byte order on the host LE side.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	if (!portRelocFindContainingFile(addr, &fileBase, &fileSize))
		return;

	uintptr_t target = (uintptr_t)addr;
	size_t target_offset = target - fileBase;
	size_t total_bytes = (size_t)num_vtx * 16;
	if (target_offset + total_bytes > fileSize) return;

	// PER-VERTEX idempotency.  Game code can re-load OVERLAPPING sub-ranges
	// of the same vertex array (e.g. cmd 1 loads vertices 0..15 at addr A,
	// cmd 2 loads vertices 1..14 at addr A+16).  Address-keyed tracking
	// would treat these as distinct keys and double-fix the overlap region.
	// Instead we mark each individual 16-byte Vtx; only un-marked vertices
	// get fixed.
	uint32_t *region = (uint32_t *)target;
	for (unsigned int i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target + (uintptr_t)i * 16;
		if (addrset_contains(&sStructU16Fixups, vtx_key)) continue;
		addrset_insert(&sStructU16Fixups, vtx_key);

		// word 0: s16 ob[0] | s16 ob[1] → rotate16
		region[i * 4 + 0] = rotate16(region[i * 4 + 0]);
		// word 1: s16 ob[2] | u16 flag → rotate16
		region[i * 4 + 1] = rotate16(region[i * 4 + 1]);
		// word 2: s16 tc[0] | s16 tc[1] → rotate16
		region[i * 4 + 2] = rotate16(region[i * 4 + 2]);
		// word 3: u8 cn[0..3] → bswap32 to restore byte order
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
}

// ============================================================
//  Sprite / Bitmap / MObjSub — struct-level byte-swap fixups
// ============================================================
//
// After the blanket u32 swap, u32 and f32 fields are correct but
// u16-pair words and u8-quad words are garbled.
//
// rotate16: fixes two u16 values packed in one u32 word
// bswap32:  undoes the blanket swap for a u8-quad word
//
// Each function is idempotent: tracked by the same sStructU16Fixups set.

static void fixup_rotate16(uint32_t *word)
{
	*word = (*word << 16) | (*word >> 16);
}

static void fixup_bswap32(uint32_t *word)
{
	*word = BSWAP32(*word);
}

// Fixup for a u32 word laid out as [u16 a][u8 b][u8 c] in original BE memory.
// Pass1's blanket BSWAP32 leaves the bytes as [c, b, a_lo, a_hi] which makes
// a_lo/a_hi appear in the wrong slots: reading `u16 a` from offset 0 yields
// (b << 8) | c, and reading `u8 b`/`u8 c` from offsets 2/3 yields a_lo/a_hi.
// Neither rotate16 nor a second bswap32 produces the correct LE layout for
// all three fields simultaneously, so we permute the bytes directly.
static void fixup_u16_u8u8(uint32_t *word)
{
	uint8_t *p = (uint8_t *)word;
	uint8_t b0 = p[0];
	uint8_t b1 = p[1];
	uint8_t b2 = p[2];
	uint8_t b3 = p[3];
	// Have (post-pass1): [c, b, a_lo, a_hi] = [b0, b1, b2, b3]
	// Want (LE struct):  [a_lo, a_hi, b, c] = [b2, b3, b1, b0]
	p[0] = b2;
	p[1] = b3;
	p[2] = b1;
	p[3] = b0;
}

void portFixupSprite(void *sprite)
{
	if (sprite == NULL)
		return;

	uintptr_t key = (uintptr_t)sprite;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *w = (uint32_t *)sprite;

	// Sprite layout (17 words = 68 bytes):
	//  Word  Offset  Fields                   Fixup
	//  0     0x00    s16 x, s16 y             rotate16
	//  1     0x04    s16 width, s16 height    rotate16
	//  2     0x08    f32 scalex               (ok)
	//  3     0x0C    f32 scaley               (ok)
	//  4     0x10    s16 expx, s16 expy       rotate16
	//  5     0x14    u16 attr, s16 zdepth     rotate16
	//  6     0x18    u8 r,g,b,a               bswap32
	//  7     0x1C    s16 startTLUT, s16 nTLUT rotate16
	//  8     0x20    u32 LUT (pointer)        (ok)
	//  9     0x24    s16 istart, s16 istep    rotate16
	//  10    0x28    s16 nbitmaps, s16 ndisplist rotate16
	//  11    0x2C    s16 bmheight, s16 bmHreal rotate16
	//  12    0x30    u8 bmfmt, u8 bmsiz, pad  bswap32
	//  13    0x34    u32 bitmap (pointer)      (ok)
	//  14    0x38    u32 rsp_dl (pointer)      (ok)
	//  15    0x3C    u32 rsp_dl_next (pointer) (ok)
	//  16    0x40    s16 frac_s, s16 frac_t   rotate16

	fixup_rotate16(&w[0]);   // x, y
	fixup_rotate16(&w[1]);   // width, height
	// w[2], w[3]: f32 scalex, scaley — ok
	fixup_rotate16(&w[4]);   // expx, expy
	fixup_rotate16(&w[5]);   // attr, zdepth
	fixup_bswap32(&w[6]);    // rgba
	fixup_rotate16(&w[7]);   // startTLUT, nTLUT
	// w[8]: u32 LUT — ok
	fixup_rotate16(&w[9]);   // istart, istep
	fixup_rotate16(&w[10]);  // nbitmaps, ndisplist
	fixup_rotate16(&w[11]);  // bmheight, bmHreal
	fixup_bswap32(&w[12]);   // bmfmt, bmsiz, pad
	// w[13..15]: u32 pointers — ok
	fixup_rotate16(&w[16]);  // frac_s, frac_t
}

void portFixupBitmap(void *bitmap)
{
	if (bitmap == NULL)
		return;

	uintptr_t key = (uintptr_t)bitmap;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *w = (uint32_t *)bitmap;

	// Bitmap layout (4 words = 16 bytes):
	//  Word  Offset  Fields                        Fixup
	//  0     0x00    s16 width, s16 width_img      rotate16
	//  1     0x04    s16 s, s16 t                  rotate16
	//  2     0x08    u32 buf (pointer)             (ok)
	//  3     0x0C    s16 actualHeight, s16 LUToffset rotate16

	fixup_rotate16(&w[0]);
	fixup_rotate16(&w[1]);
	// w[2]: u32 buf — ok
	fixup_rotate16(&w[3]);
}

void portFixupBitmapArray(void *bitmaps, unsigned int count)
{
	if (bitmaps == NULL || count == 0)
		return;

	// Protect the bitmap array bytes from runtime texture/palette BSWAP
	// over-reads. N64 sprite files sometimes sit a full 512-byte CI8 palette
	// load on top of the bitmap array (the useful palette entries fit before
	// the array; the tail past the array start is effectively garbage). See
	// the wallpaper handling in mvOpeningRun for the concrete case.
	portRegisterProtectedStructRange(bitmaps, (size_t)count * 16);

	uint8_t *ptr = (uint8_t *)bitmaps;
	for (unsigned int i = 0; i < count; i++)
	{
		portFixupBitmap(ptr + i * 16);
	}
}

// Sprite layout offsets used by the texel-data fixup. We do not include
// PR/sp.h here (to keep this translation unit lean), so we hard-code
// the field offsets matching sizeof(Sprite) == 68.
//
//   sprite[0x28]  s16 nbitmaps
//   sprite[0x31]  u8  bmsiz                (within the u8 quad word at 0x30)
//   sprite[0x34]  u32 bitmap (pointer)
//
//   bitmap[0x00]  s16 width        (unused for size — see width_img)
//   bitmap[0x02]  s16 width_img
//   bitmap[0x08]  u32 buf (pointer)
//   bitmap[0x0C]  s16 actualHeight
//
// Sizes match decomp Sprite/Bitmap structs after portFixupSprite/portFixupBitmap
// have run (i.e. fields readable as native LE).
void portFixupSpriteBitmapData(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	// Also protect the 68-byte Sprite struct from runtime-texture over-reads
	// for the same reason as the bitmap array above.
	portRegisterProtectedStructRange(sprite_v, 68);

	uint8_t *sprite = (uint8_t *)sprite_v;
	uint8_t *bitmaps = (uint8_t *)bitmaps_v;

	int16_t nbitmaps = *(int16_t *)(sprite + 0x28);
	uint8_t bmsiz    = *(uint8_t *)(sprite + 0x31);

	if (nbitmaps <= 0)
		return;

	// G_IM_SIZ_ values: 4b=0, 8b=1, 16b=2, 32b=3, 4c=4 (compressed-4b)
	//
	// The renderer's texel readers all read texture bytes in N64 big-endian
	// order — for example
	//   RGBA16: `(addr[0] << 8) | addr[1]`
	//   RGBA32: `r=addr[0], g=addr[1], b=addr[2], a=addr[3]`
	// So texel data must be left in the original BE byte order from the file.
	// Pass1 BSWAP32 destroys that order; another BSWAP32 restores it.
	//
	// 32bpp used to be skipped here on the assumption "pass1 byteswap is
	// already correct because the reader is per-channel byte-by-byte".  That
	// reasoning is wrong: pass1's per-u32 byte reverse turns each pixel's
	// `[R G B A]` into `[A B G R]` in memory, so the per-byte reader sees
	// channel-reversed RGBA32.  Symptom: HAL Labs dog (a 32bpp sprite) renders
	// with R↔A and G↔B swapped.
	int bpp;
	switch (bmsiz)
	{
	case 0: bpp = 4;  break;   // 4b
	case 1: bpp = 8;  break;   // 8b
	case 2: bpp = 16; break;   // 16b
	case 3: bpp = 32; break;   // 32b
	case 4: bpp = 4;  break;   // 4c (compressed-4b: byte-granular like 4b)
	default:
		return;
	}

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  width_img    = *(int16_t *)(bm + 0x02);
		uint32_t buf_word     = *(uint32_t *)(bm + 0x08);
		int16_t  actualHeight = *(int16_t *)(bm + 0x0C);

		void *buf = slot_to_ptr(buf_word);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		// Idempotency for the BSWAP32 pass: skip if pass2 (or a prior
		// sprite fixup call) already restored the byte order.
		uintptr_t key = (uintptr_t)buf;
		if (!addrset_contains(&sStructU16Fixups, key))
		{
			addrset_insert(&sStructU16Fixups, key);

			size_t num_texels = (size_t)width_img * (size_t)actualHeight;
			size_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Word-align so we can iterate as u32
			tex_bytes = (tex_bytes + 3) & ~(size_t)3;

			uint32_t *words = (uint32_t *)buf;
			size_t num_words = tex_bytes / 4;

			// Undo pass1 BSWAP32 to restore N64 BE byte order.
			for (size_t j = 0; j < num_words; j++)
				words[j] = BSWAP32(words[j]);
		}

		// N64 RDP TMEM line swizzle: textures stored in DRAM are pre-swizzled
		// to avoid TMEM bank conflicts when sampled. lbCommonDrawSObjBitmap
		// LOAD_BLOCKs the sprite and relies on the RDP's odd-row TMEM
		// address XOR to unscramble at sample time; the renderer reads DRAM
		// linearly, so we have to un-swizzle in software.
		//
		// The swap granularity is bit-depth dependent:
		//
		//   4b/8b/16b: LoadBlock uses G_IM_SIZ_16b (per the *_LOAD_BLOCK
		//              macros in gbi.h), so the sampler's XOR-0x4 on the
		//              byte address lands as "swap the two 4-byte halves
		//              of each 8-byte qword" on odd rows.
		//
		//   32b:       LoadBlock uses G_IM_SIZ_32b. The LOAD_BLOCK with
		//              dxt=0 interleaves each 4-byte pixel across the
		//              low/high TMEM banks (2 bytes per bank), so each
		//              texel consumes 2 TMEM words instead of one. The
		//              sampler's odd-row XOR still acts at the same TMEM
		//              word granularity, which at the DRAM level means
		//              "swap the two 8-byte halves of each 16-byte group"
		//              — i.e. swap 2 RGBA32 pixels with the next 2.
		//              Empirically verified in BattleShip against the
		//              original artwork (SMASH logo, fighter-select
		//              portraits): XOR-4 (1-pixel swap) scrambled 32bpp
		//              sprites by swapping adjacent pixels on every odd
		//              row; XOR-16 (2-pixel pair swap) produces clean images.
		//
		// 4c (compressed) is excluded because the on-disk data is 2bpp
		// compressed source; the decoder runs AFTER this fixup and
		// produces 4bpp data that needs its own post-decode deswizzle
		// (portDeswizzleDecodedSprite4c below).
		//
		// Uses sDeswizzle4cFixups (separate from the BSWAP set) so that
		// textures whose byte-swap was already handled by pass2 still get
		// the deswizzle applied.
		if (bmsiz != 4 && !addrset_contains(&sDeswizzle4cFixups, key))
		{
			addrset_insert(&sDeswizzle4cFixups, key);

			size_t group_size = (bpp == 32) ? 16 : 8;
			size_t half       = group_size / 2;

			// row_bytes = DRAM bytes per pixel row.
			size_t row_bytes = ((size_t)width_img * (size_t)bpp + 7) / 8;

			// Require the row to hold at least one full group. The inner
			// loop only processes complete groups (`g + group_size <=
			// row_bytes`), so any trailing bytes past the last full group
			// are left untouched.
			if (row_bytes >= group_size)
			{
				uint8_t *bytes = (uint8_t *)buf;
				for (int row = 1; row < actualHeight; row += 2)
				{
					uint8_t *row_p = bytes + (size_t)row * row_bytes;
					for (size_t g = 0; g + group_size <= row_bytes; g += group_size)
					{
						uint8_t tmp[8]; // max half is 8 (for group_size=16)
						memcpy(tmp, row_p + g, half);
						memcpy(row_p + g, row_p + g + half, half);
						memcpy(row_p + g + half, tmp, half);
					}
				}
			}
		}

		// Note: the fighter-intro "MARIO" title-card smear and the
		// title-screen border right-edge slice used to be patched here in
		// BattleShip. Both were workarounds for a renderer upload bug where
		// the texture was uploaded at the TMEM stride (width_img) but the UV
		// normalization used the SetTileSize clamp (width). The real fix
		// belongs in the renderer (clamp texture import to the tile's lrs),
		// so the trailing columns are not touched here.
	}
}

void portDeswizzleDecodedSprite4c(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	uint8_t *sprite = (uint8_t *)sprite_v;
	uint8_t *bitmaps = (uint8_t *)bitmaps_v;

	int16_t nbitmaps = *(int16_t *)(sprite + 0x28);
	if (nbitmaps <= 0)
		return;

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  width_img    = *(int16_t *)(bm + 0x02);
		uint32_t buf_word     = *(uint32_t *)(bm + 0x08);
		int16_t  actualHeight = *(int16_t *)(bm + 0x0C);

		void *buf = slot_to_ptr(buf_word);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		uintptr_t key = (uintptr_t)buf;
		if (addrset_contains(&sDeswizzle4cFixups, key))
			continue;
		addrset_insert(&sDeswizzle4cFixups, key);

		// After decode, data is 4bpp: row_bytes = width_img / 2
		size_t row_bytes = (size_t)width_img / 2;
		if (row_bytes < 8)
			continue;

		uint8_t *bytes = (uint8_t *)buf;
		for (int row = 1; row < actualHeight; row += 2)
		{
			uint8_t *row_p = bytes + (size_t)row * row_bytes;
			for (size_t qw = 0; qw + 8 <= row_bytes; qw += 8)
			{
				uint8_t tmp[4];
				memcpy(tmp, row_p + qw, 4);
				memcpy(row_p + qw, row_p + qw + 4, 4);
				memcpy(row_p + qw + 4, tmp, 4);
			}
		}
	}
}

void portFixupMObjSub(void *mobjsub)
{
	if (mobjsub == NULL)
		return;

	uintptr_t key = (uintptr_t)mobjsub;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *w = (uint32_t *)mobjsub;

	// MObjSub layout (30 words = 0x78 bytes):
	//  Word  Offset  Fields                           Fixup
	//  0     0x00    u16 pad00, u8 fmt, u8 siz        bswap32 (mixed u16+u8)
	//  1     0x04    u32 sprites (pointer)             (ok)
	//  2     0x08    u16 unk08, u16 unk0A              rotate16
	//  3     0x0C    u16 unk0C, u16 unk0E              rotate16
	//  4     0x10    s32 unk10                          (ok)
	//  5-10  0x14    f32 trau..unk28                    (ok)
	//  11    0x2C    u32 palettes (pointer)             (ok)
	//  12    0x30    u16 flags, u8 block_fmt, u8 block_siz  u16/u8/u8 permute
	//  13    0x34    u16 block_dxt, u16 unk36           rotate16
	//  14    0x38    u16 unk38, u16 unk3A               rotate16
	//  15-18 0x3C    f32 scrollu..unk48                 (ok)
	//  19    0x4C    u32 unk4C                          (ok)
	//  20    0x50    SYColorPack primcolor (u8 rgba)    bswap32
	//  21    0x54    u8 prim_l, u8 prim_m, u8[2] pad   bswap32
	//  22    0x58    SYColorPack envcolor               bswap32
	//  23    0x5C    SYColorPack blendcolor             bswap32
	//  24    0x60    SYColorPack light1color            bswap32
	//  25    0x64    SYColorPack light2color            bswap32
	//  26-29 0x68    s32 unk68..unk74                   (ok)

	fixup_bswap32(&w[0]);    // pad00 + fmt + siz (pad16 unused, u8 fields ok)
	// w[1]: sprites pointer — ok
	fixup_rotate16(&w[2]);   // unk08, unk0A
	fixup_rotate16(&w[3]);   // unk0C, unk0E
	// w[4..11]: s32/f32/u32 — ok
	fixup_u16_u8u8(&w[12]);  // u16 flags + u8 block_fmt + u8 block_siz
	fixup_rotate16(&w[13]);  // block_dxt, unk36
	fixup_rotate16(&w[14]);  // unk38, unk3A
	// w[15..19]: f32/u32 — ok
	fixup_bswap32(&w[20]);   // primcolor
	fixup_bswap32(&w[21]);   // prim_l, prim_m, pad
	fixup_bswap32(&w[22]);   // envcolor
	fixup_bswap32(&w[23]);   // blendcolor
	fixup_bswap32(&w[24]);   // light1color
	fixup_bswap32(&w[25]);   // light2color
	// w[26..29]: s32 — ok
}

void *portFixupFTTexturePartContainer(void *container)
{
	/* FTTexturePartContainer = `FTTexturePart textureparts[2]` where each
	 * entry is `{u8 joint_id; u8 detail[2];}` = 3 bytes; total sizeof = 6.
	 * After pass1 BSWAP32 the u8 fields read as the wrong byte (post-pass1
	 * byte 0 == original byte 3, etc.).
	 *
	 * Earlier attempts at a fix wrote *into* the file image. That broke
	 * for fighters whose `attr->textureparts_container` points at memory
	 * shared with adjacent relocated pointers (the C struct claim of
	 * always-6 container bytes does NOT hold for every fighter — some have
	 * pointer-shaped data at offsets 4-7 of the resolved address). Writing
	 * the "fixed" bytes back over those pointers crashed `ftMainSetStatus`
	 * downstream.
	 *
	 * Non-destructive approach: return a static-storage corrected COPY of
	 * the 6 container bytes. The original file image is never modified, so
	 * adjacent pointer data is untouched. Caller dereferences the returned
	 * pointer for `textureparts[0..1]` reads — that data is correct.
	 *
	 * The static copy is single-instance: callers must not interleave
	 * lookups for two different containers without consuming the result of
	 * the first. ftparam.c's call sites each take a single snapshot
	 * inside one function — safe. */
	if (container == NULL)
		return NULL;

	static uint8_t sFixed[8] __attribute__((aligned(4)));   /* 8 bytes for container alignment safety */
	uint8_t *src = (uint8_t *)container;
	sFixed[0] = src[3];   /* bswap of word 0 (bytes 0..3) */
	sFixed[1] = src[2];
	sFixed[2] = src[1];
	sFixed[3] = src[0];
	sFixed[4] = src[7];   /* pass1 moved original byte 4 → position 7 */
	sFixed[5] = src[6];   /* pass1 moved original byte 5 → position 6 */
	sFixed[6] = 0;        /* trailing pad (not read by C struct) */
	sFixed[7] = 0;
	return sFixed;
}

void portFixupFTAttributes(void *attr)
{
	if (attr == NULL)
		return;

	uintptr_t key = (uintptr_t)attr;
	if (addrset_contains(&sStructU16Fixups, key))
		return;
	addrset_insert(&sStructU16Fixups, key);

	uint32_t *w = (uint32_t *)attr;

	// FTAttributes layout (0x348 bytes = 210 words):
	// Words 0x00..0x2C: f32/s32 physics fields — ok
	// Words 0x27..0x2B: MPObjectColl (4 f32) + Vec2f (2 f32) — ok
	//
	//  Word  Offset  Fields                               Fixup
	//  0x2D  0x0B4   u16 dead_fgm_ids[0], [1]            rotate16
	//  0x2E  0x0B8   u16 deadup_sfx, u16 damage_sfx      rotate16
	//  0x2F  0x0BC   u16 smash_sfx[0], smash_sfx[1]      rotate16
	//  0x30  0x0C0   u16 smash_sfx[2], pad                rotate16
	// Words 0x31..0x38: FTItemPickup (8 f32) — ok
	//  0x39  0x0E4   u16 itemthrow_vel_scale, damage_scale rotate16
	//  0x3A  0x0E8   u16 heavyget_sfx, pad                rotate16
	// Word 0x3B: f32 halo_size — ok
	//  0x3C  0x0F0   SYColorRGBA shade_color[0]           bswap32
	//  0x3D  0x0F4   SYColorRGBA shade_color[1]           bswap32
	//  0x3E  0x0F8   SYColorRGBA shade_color[2]           bswap32
	//  0x3F  0x0FC   SYColorRGBA fog_color                bswap32
	// Word 0x40: is_have_* bitfields — handled by #if IS_BIG_ENDIAN in FTAttributes decl
	// Words 0x41..end: DamageCollDescs (s32/f32), pointers (u32) — ok

	fixup_rotate16(&w[0x2D]);  // dead_fgm_ids[0], [1]
	fixup_rotate16(&w[0x2E]);  // deadup_sfx, damage_sfx
	fixup_rotate16(&w[0x2F]);  // smash_sfx[0], smash_sfx[1]
	fixup_rotate16(&w[0x30]);  // smash_sfx[2], pad
	fixup_rotate16(&w[0x39]);  // itemthrow_vel_scale, itemthrow_damage_scale
	fixup_rotate16(&w[0x3A]);  // heavyget_sfx, pad
	fixup_bswap32(&w[0x3C]);   // shade_color[0] rgba
	fixup_bswap32(&w[0x3D]);   // shade_color[1] rgba
	fixup_bswap32(&w[0x3E]);   // shade_color[2] rgba
	fixup_bswap32(&w[0x3F]);   // fog_color rgba
}

/* =========================================================================
 * portMarkSyntheticSprite — pre-register a port-built Sprite so that all
 * byteswap/fixup passes become no-ops when the game calls
 * lbCommonMakeSObjForGObj → portFixupSprite / portFixupBitmapArray /
 * portFixupSpriteBitmapData on it.
 *
 * The caller must have already set all struct fields in native LE order and
 * stored pixel buffers in big-endian RGBA16 linear (no TMEM odd-row swizzle).
 * ========================================================================= */
void portMarkSyntheticSprite(void *sprite, void *bitmaps,
                             unsigned int nbitmaps,
                             void *const *buf_ptrs)
{
	if (sprite)
		addrset_insert(&sStructU16Fixups, (uintptr_t)sprite);

	if (bitmaps)
	{
		// Register each Bitmap's base address so portFixupBitmapArray's
		// per-entry portFixupBitmap calls are no-ops.
		// portFixupBitmapArray's portRegisterProtectedStructRange call will
		// still execute — it adds the bitmap array to the BSWAP-protection
		// list, which is harmless for synthetic sprites.
		uint8_t *bm = (uint8_t *)bitmaps;
		for (unsigned int i = 0; i < nbitmaps; i++)
			addrset_insert(&sStructU16Fixups, (uintptr_t)(bm + i * 16));
	}

	// Register each pixel buffer in both tracking sets so:
	//   sStructU16Fixups → portFixupSpriteBitmapData skips BSWAP32 pass
	//   sDeswizzle4cFixups → portFixupSpriteBitmapData skips deswizzle pass
	if (buf_ptrs)
	{
		for (unsigned int i = 0; i < nbitmaps; i++)
		{
			if (buf_ptrs[i] == NULL) continue;
			uintptr_t key = (uintptr_t)buf_ptrs[i];
			addrset_insert(&sStructU16Fixups, key);
			addrset_insert(&sDeswizzle4cFixups, key);
		}
	}
}
