/* From BattleShip port/bridge/particle_bank_bridge.cpp (MIT, see port/NOTICE.md), converted to C.
 *
 * particle_bank_bridge.c -- particle script+texture bank loader for PORT.
 *
 * On the N64, efParticleGetLoadBankID DMA'd a bank's script (.scb) and
 * texture (.txb) blobs from the cartridge into the scene heap and handed them
 * to lbParticleSetupBankID. Under PORT the decomp calls portParticleLoadBank
 * instead (src/ef/efparticle.c). BattleShip loaded the blobs by name from its
 * O2R archive; here they are read from the player's ROM (hh/romdata.h), then
 * the struct headers are byte-swapped to native order and the bank is handed
 * to lbParticleSetupBankID, which pointerizes the offset tables in place.
 *
 * Bank key: callers pass (uintptr_t)&l<Owner>ParticleScriptBankLo (or, from
 * FTData, &particles_unk{0,1,2}_scb_ROM_START). Those are distinct zero-valued
 * objects defined in hh/bs/segment_symbols.c, so their ADDRESSES identify the
 * bank; the ROM range comes from the table below.
 *
 * Differences from BattleShip: no heap. BattleShip kept a pristine
 * (swapped, un-pointerized) copy per bank in an unordered_map plus a
 * per-request working copy in std::vectors. Here each bank owns one static
 * working buffer (so bank data sits in .bss and survives save states), and
 * the pristine copy is unnecessary because re-reading from the mmap'd ROM
 * slice and re-swapping is cheap. The packed-DL cache eviction is dropped
 * (LP64-only).
 */
#include <ssb_types.h>
#include <lb/lbparticle.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "romdata.h"
#include "port_log.h"
#include "bridge/particle_bank_bridge.h"

/* Bank key objects (hh/bs/segment_symbols.c); types match the decomp. */
extern uintptr_t lEFCommonParticleScriptBankLo;
extern intptr_t  lITManagerParticleScriptBankLo;
extern intptr_t  lGRPupupuParticleScriptBankLo;
extern intptr_t  lGRHyruleParticleScriptBankLo;
extern intptr_t  lGRYosterParticleScriptBankLo;
extern uintptr_t lMNTitleParticleScriptBankLo;
extern s32 particles_unk0_scb_ROM_START;
extern s32 particles_unk1_scb_ROM_START;
extern s32 particles_unk2_scb_ROM_START;

/* Renderer / byte-swap layer: forget address-keyed state for a buffer that is
 * about to be overwritten. */
extern void portTextureCacheDeleteRange(const void *base, size_t size);
extern void portEvictStructFixupsInRange(const void *begin, size_t size);

/* ========================================================================= */
/*  Per-bank configuration                                                   */
/* ========================================================================= */

/* Each bank's .scb runs up to its .txb, and each .txb up to the next listed
 * ROM address (splat yaml, US). efcommon_txb ends where particles_unk0
 * starts; mntitle_txb ends at the audio banks. */
#define EFCOMMON_SCB_SIZE   (ROMDATA_EFCOMMON_TXB   - ROMDATA_EFCOMMON_SCB)
#define EFCOMMON_TXB_SIZE   (ROMDATA_PARTICLES0_SCB - ROMDATA_EFCOMMON_TXB)
#define ITCOMMON_SCB_SIZE   (ROMDATA_ITCOMMON_TXB   - ROMDATA_ITCOMMON_SCB)
#define ITCOMMON_TXB_SIZE   (ROMDATA_GRPUPUPU_SCB   - ROMDATA_ITCOMMON_TXB)
#define GRPUPUPU_SCB_SIZE   (ROMDATA_GRPUPUPU_TXB   - ROMDATA_GRPUPUPU_SCB)
#define GRPUPUPU_TXB_SIZE   (ROMDATA_GRHYRULE_SCB   - ROMDATA_GRPUPUPU_TXB)
#define GRHYRULE_SCB_SIZE   (ROMDATA_GRHYRULE_TXB   - ROMDATA_GRHYRULE_SCB)
#define GRHYRULE_TXB_SIZE   (ROMDATA_GRYOSTER_SCB   - ROMDATA_GRHYRULE_TXB)
#define GRYOSTER_SCB_SIZE   (ROMDATA_GRYOSTER_TXB   - ROMDATA_GRYOSTER_SCB)
#define GRYOSTER_TXB_SIZE   (ROMDATA_MNTITLE_SCB    - ROMDATA_GRYOSTER_TXB)
#define MNTITLE_SCB_SIZE    (ROMDATA_MNTITLE_TXB    - ROMDATA_MNTITLE_SCB)
#define MNTITLE_TXB_SIZE    (ROMDATA_PARTICLES_END  - ROMDATA_MNTITLE_TXB)
#define PARTICLES0_SCB_SIZE (ROMDATA_PARTICLES0_TXB - ROMDATA_PARTICLES0_SCB)
#define PARTICLES0_TXB_SIZE (ROMDATA_PARTICLES1_SCB - ROMDATA_PARTICLES0_TXB)
#define PARTICLES1_SCB_SIZE (ROMDATA_PARTICLES1_TXB - ROMDATA_PARTICLES1_SCB)
#define PARTICLES1_TXB_SIZE (ROMDATA_PARTICLES2_SCB - ROMDATA_PARTICLES1_TXB)
#define PARTICLES2_SCB_SIZE (ROMDATA_PARTICLES2_TXB - ROMDATA_PARTICLES2_SCB)
#define PARTICLES2_TXB_SIZE (ROMDATA_ITCOMMON_SCB   - ROMDATA_PARTICLES2_TXB)

/* Working buffers: one script + one texture buffer per bank, overwritten each
 * time the bank is (re)loaded. ~390 KiB total, 83% of it EFCommon. */
#define BANK_BUF(name) \
	static u8 s##name##Scb[name##_SCB_SIZE] __attribute__((aligned(16))); \
	static u8 s##name##Txb[name##_TXB_SIZE] __attribute__((aligned(16)));
BANK_BUF(EFCOMMON)
BANK_BUF(ITCOMMON)
BANK_BUF(GRPUPUPU)
BANK_BUF(GRHYRULE)
BANK_BUF(GRYOSTER)
BANK_BUF(MNTITLE)
BANK_BUF(PARTICLES0)
BANK_BUF(PARTICLES1)
BANK_BUF(PARTICLES2)

typedef struct BankRow
{
	const void *key;        /* &l<Owner>ParticleScriptBankLo */
	u32 scb_rom, scb_size;
	u32 txb_rom, txb_size;
	u8 *scb_buf, *txb_buf;
	const char *debug_name;
} BankRow;

#define BANK_ROW(keysym, name, dbg) \
	{ &keysym, ROMDATA_##name##_SCB, name##_SCB_SIZE, ROMDATA_##name##_TXB, name##_TXB_SIZE, \
	  s##name##Scb, s##name##Txb, dbg }

static const BankRow sBankTable[] =
{
	BANK_ROW(lEFCommonParticleScriptBankLo,  EFCOMMON,   "EFCommon"),
	BANK_ROW(lITManagerParticleScriptBankLo, ITCOMMON,   "ITManager"),
	BANK_ROW(lGRPupupuParticleScriptBankLo,  GRPUPUPU,   "GRPupupu"),
	BANK_ROW(lGRHyruleParticleScriptBankLo,  GRHYRULE,   "GRHyrule"),
	BANK_ROW(lGRYosterParticleScriptBankLo,  GRYOSTER,   "GRYoster"),
	BANK_ROW(lMNTitleParticleScriptBankLo,   MNTITLE,    "MNTitle"),
	/* Fighter banks: FTData passes &particles_unk{0,1,2}_scb_ROM_START. */
	BANK_ROW(particles_unk0_scb_ROM_START,   PARTICLES0, "unk0"),
	BANK_ROW(particles_unk1_scb_ROM_START,   PARTICLES1, "unk1"),
	BANK_ROW(particles_unk2_scb_ROM_START,   PARTICLES2, "unk2"),
};

/* ========================================================================= */
/*  Byte-swap helpers                                                        */
/* ========================================================================= */

static inline void bswap16At(u8 *p, size_t off)
{
	u8 a = p[off];

	p[off] = p[off + 1];
	p[off + 1] = a;
}

static inline void bswap32At(u8 *p, size_t off)
{
	u8 a = p[off], b = p[off + 1];

	p[off] = p[off + 3];
	p[off + 1] = p[off + 2];
	p[off + 2] = b;
	p[off + 3] = a;
}

static inline u32 readNativeU32(const u8 *p, size_t off)
{
	u32 v;

	memcpy(&v, p + off, 4);
	return v;
}

/* Script bank layout (big-endian in ROM):
 *   +0x00  s32    scripts_num
 *   +0x04  u32[]  offset table (scripts_num entries, offsets from blob base)
 *   +...   LBScript records at each offset:
 *          +0x00  u16 kind, texture_id, generator_lifetime, particle_lifetime
 *          +0x08  u32 flags
 *          +0x0C  f32 gravity, friction, vel[3], unk_0x20, unk_0x24,
 *                     update_rate, size
 *          +0x30  u8[] bytecode (byte stream; left as is)
 */
static void byteSwapScriptBank(u8 *data, size_t size, const char *dbg)
{
	s32 scripts_num, i;
	size_t k, table_end;

	if (size < 4)
	{
		port_log("particle_bank[%s]: script blob too small (%u bytes)\n", dbg, (unsigned)size);
		return;
	}
	bswap32At(data, 0);
	scripts_num = (s32)readNativeU32(data, 0);

	if ((scripts_num <= 0) || (scripts_num > 1024))
	{
		port_log("particle_bank[%s]: absurd scripts_num=%d\n", dbg, (int)scripts_num);
		return;
	}
	table_end = 4 + (size_t)scripts_num * 4;

	if (table_end > size)
	{
		port_log("particle_bank[%s]: script offset table (%u) overflows blob (%u)\n",
		         dbg, (unsigned)table_end, (unsigned)size);
		return;
	}
	for (i = 0; i < scripts_num; i++)
	{
		bswap32At(data, 4 + i * 4);
	}
	for (i = 0; i < scripts_num; i++)
	{
		u32 script_off = readNativeU32(data, 4 + i * 4);

		if ((size_t)script_off + 0x30 > size)
		{
			port_log("particle_bank[%s]: script %d header at 0x%X runs past end\n",
			         dbg, (int)i, (unsigned)script_off);
			continue;
		}
		bswap16At(data, script_off + 0x00);
		bswap16At(data, script_off + 0x02);
		bswap16At(data, script_off + 0x04);
		bswap16At(data, script_off + 0x06);

		for (k = 0; k < 10; k++)
		{
			bswap32At(data, script_off + 0x08 + k * 4);
		}
	}
}

/* Texture bank layout (big-endian in ROM):
 *   +0x00  s32    textures_num
 *   +0x04  u32[]  offset table (textures_num entries)
 *   +...   LBTexture records:
 *          +0x00  u32 count, s32 fmt, s32 siz, s32 width, s32 height, u32 flags
 *          +0x18  u32 data[] (count image offsets, then 1 palette if
 *                 fmt == CI and flags & 1, else count palettes for CI)
 *   Pixel and palette bytes stay big-endian: the texture pipeline reads them
 *   in N64 byte order.
 */
static void byteSwapTextureBank(u8 *data, size_t size, const char *dbg)
{
	const s32 kImFmtCi = 2; /* G_IM_FMT_CI */
	s32 textures_num, i;
	size_t k, table_end;

	if (size < 4)
	{
		port_log("particle_bank[%s]: texture blob too small (%u bytes)\n", dbg, (unsigned)size);
		return;
	}
	bswap32At(data, 0);
	textures_num = (s32)readNativeU32(data, 0);

	if ((textures_num <= 0) || (textures_num > 1024))
	{
		port_log("particle_bank[%s]: absurd textures_num=%d\n", dbg, (int)textures_num);
		return;
	}
	table_end = 4 + (size_t)textures_num * 4;

	if (table_end > size)
	{
		port_log("particle_bank[%s]: texture offset table (%u) overflows blob (%u)\n",
		         dbg, (unsigned)table_end, (unsigned)size);
		return;
	}
	for (i = 0; i < textures_num; i++)
	{
		bswap32At(data, 4 + i * 4);
	}
	for (i = 0; i < textures_num; i++)
	{
		u32 tex_off = readNativeU32(data, 4 + i * 4);
		u32 count, flags, total_data_entries;
		s32 fmt;
		size_t data_end;

		if ((size_t)tex_off + 0x18 > size)
		{
			port_log("particle_bank[%s]: texture %d header at 0x%X runs past end\n",
			         dbg, (int)i, (unsigned)tex_off);
			continue;
		}
		for (k = 0; k < 6; k++)
		{
			bswap32At(data, tex_off + k * 4);
		}
		count = readNativeU32(data, tex_off + 0x00);
		fmt   = (s32)readNativeU32(data, tex_off + 0x04);
		flags = readNativeU32(data, tex_off + 0x14);

		if (count > 4096)
		{
			port_log("particle_bank[%s]: texture %d absurd count=%u\n", dbg, (int)i, (unsigned)count);
			continue;
		}
		total_data_entries = count;

		if (fmt == kImFmtCi)
		{
			total_data_entries += (flags & 1) ? 1u : count;
		}
		data_end = tex_off + 0x18 + (size_t)total_data_entries * 4;

		if (data_end > size)
		{
			port_log("particle_bank[%s]: texture %d data[] (%u) overflows blob (%u)\n",
			         dbg, (int)i, (unsigned)data_end, (unsigned)size);
			continue;
		}
		for (k = 0; k < total_data_entries; k++)
		{
			bswap32At(data, tex_off + 0x18 + k * 4);
		}
	}
}

/* ========================================================================= */
/*  Public entry point                                                       */
/* ========================================================================= */

int portParticleLoadBank(uintptr_t scripts_lo, int bank_id)
{
	const BankRow *match = NULL;
	size_t i;

	for (i = 0; i < sizeof(sBankTable) / sizeof(sBankTable[0]); i++)
	{
		if ((uintptr_t)sBankTable[i].key == scripts_lo)
		{
			match = &sBankTable[i];
			break;
		}
	}
	if (match == NULL)
	{
		port_log("particle_bank: no table entry for scripts_lo=0x%lX (bank_id=%d)\n",
		         (unsigned long)scripts_lo, bank_id);
		return -1;
	}

	/* The bank's buffer is about to be overwritten. Purge every cache keyed
	 * on raw addresses inside it -- the same lockstep eviction the reloc
	 * loader does -- or a texture cache entry for the previous load could be
	 * hit by a same-shape texture at the same address and render stale
	 * texels. */
	portTextureCacheDeleteRange(match->scb_buf, match->scb_size);
	portEvictStructFixupsInRange(match->scb_buf, match->scb_size);
	portTextureCacheDeleteRange(match->txb_buf, match->txb_size);
	portEvictStructFixupsInRange(match->txb_buf, match->txb_size);

	if (romdata_read(match->scb_rom, match->scb_buf, match->scb_size) != 0)
	{
		port_log("particle_bank[%s]: script bank not in ROM data\n", match->debug_name);
		return -2;
	}
	if (romdata_read(match->txb_rom, match->txb_buf, match->txb_size) != 0)
	{
		port_log("particle_bank[%s]: texture bank not in ROM data\n", match->debug_name);
		return -3;
	}
	byteSwapScriptBank(match->scb_buf, match->scb_size, match->debug_name);
	byteSwapTextureBank(match->txb_buf, match->txb_size, match->debug_name);

	lbParticleSetupBankID(bank_id, (LBScriptDesc *)match->scb_buf, (LBTextureDesc *)match->txb_buf);

	return 0;
}
