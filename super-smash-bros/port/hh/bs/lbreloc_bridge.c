/* From BattleShip port/bridge/lbreloc_bridge.cpp and port/resource/RelocPointerTable.cpp
 * (MIT, see port/NOTICE.md), converted to C.
 *
 * lbreloc_bridge.c -- PORT replacement for src/lb/lbreloc.c.
 *
 * The N64 loader DMA'd the reloc file table and files from the cartridge.
 * BattleShip replaced that with libultraship's ResourceManager (an O2R
 * archive extracted by Torch). Here the files come straight from the player's
 * ROM slice through hh/romdata.h, which yields exactly the fields BattleShip's
 * RelocFile resource carried (intern/extern chain offsets, extern file id list,
 * decompressed big-endian bytes).
 *
 * Differences from the BattleShip original:
 *   - ILP32 target: a relocated pointer fits in the 4-byte data slot, so the
 *     LP64 token table (RelocPointerTable.cpp) collapses to identity functions,
 *     defined at the bottom of this file. The token API is kept because the
 *     decomp's PORT paths call it (sys/objtypes.h PORT_RESOLVE / PORT_REGISTER).
 *   - No heap: the loaded-file range table is a fixed static array, and the
 *     figatree chain-slot bitmap uses a static per-recursion-depth pool, so the
 *     whole loader state lives in .bss and survives save states.
 *   - Dropped: env-driven dumps/logs, stage audit emitters, the stale-O2R exit
 *     screen, the mod-loader byte entry points (portRelocLoadFileFromBytes*,
 *     portRelocGet/SetExternFileHeap, portRelocResetForceStatusBuffer), the
 *     packed-DL widening cache calls (LP64-only: N64 Gfx is already native size
 *     on ILP32) and port_classify_dl_ptr (diagnostic).
 */
#include <ssb_types.h>
#include <lb/lbreloc.h>
#include <lb/lbtypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "romdata.h"
#include "port_log.h"
#include "port_aobj_fixup.h"
#include "bridge/framebuffer_capture.h"

#define LBRELOC_CACHE_ALIGN(x) (((x) + 0xF) & ~0xF)
#define LBRELOC_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Byte-swap layer (hh/bs/lbreloc_byteswap.c, from BattleShip
 * port/bridge/lbreloc_byteswap.cpp). Declared here so this file does not
 * depend on that header's arrival order. */
extern void portRelocByteSwapBlob(void *data, size_t size, unsigned int file_id);
extern void portResetStructFixups(void);
extern void portEvictStructFixupsInRange(const void *begin, size_t size);
extern void portRelocNoteChainSlot(const void *slot);
extern int portRelocFixupTextureFromChain(void *file_base, size_t file_size,
                                          unsigned int slot_byte_off,
                                          unsigned int target_byte_off);

/* Renderer (hh/gfx): drop cached texture uploads keyed on addresses inside a
 * range that is about to be overwritten. */
extern void portTextureCacheDeleteRange(const void *base, size_t size);

/* Decomp. */
extern void syDebugPrintf(const char *fmt, ...);
extern void scManagerRunPrintGObjStatus(void);
extern void gmColScriptsLinkRelocTargets(void);
extern void scSubsysMotionLinkRelocTargets(void);

/* Exported below; forward declarations for the mutual recursion between the
 * chain walk and the dependency loaders. */
void *lbRelocGetExternBufferFile(u32 id);
void *lbRelocGetInternBufferFile(u32 id);
void *lbRelocGetForceExternBufferFile(u32 id);
void port_dl_range_register(const void *base, size_t size, const char *label);
void port_dl_range_unregister(const void *base);
void portRelocInvalidateRange(const void *base, size_t size);
unsigned int portRelocRegisterPointer(void *ptr);

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

static LBInternBuffer sLBRelocInternBuffer;

static u32 *sLBRelocExternFileIDs;
static s32 sLBRelocExternFileIDsNum;
static s32 sLBRelocExternFileIDsMax;
static void *sLBRelocExternFileHeap;

/* Address ranges of every loaded reloc file, for pointer -> file lookups
 * (byte-swap layer's runtime fixups, renderer segment-0x0E normalisation).
 * Ranges never overlap: a load evicts every range it overwrites first. A
 * scene holds well under a few hundred files; 1024 leaves headroom. */
#define PORT_RELOC_RANGES_MAX 1024

typedef struct PortRelocFileRange
{
	uintptr_t base;
	size_t size;
	u32 file_id;
} PortRelocFileRange;

static PortRelocFileRange sPortRelocFileRanges[PORT_RELOC_RANGES_MAX];
static u32 sPortRelocFileRangesNum;

/* BattleShip returned the Torch path of the file; there are no paths here,
 * so every range reports this label. */
static const char sPortRelocPathLabel[] = "reloc";

static void portRelocAddFileRange(void *base, size_t size, u32 file_id)
{
	if (sPortRelocFileRangesNum >= PORT_RELOC_RANGES_MAX)
	{
		static int sWarned;

		if (!sWarned)
		{
			sWarned = 1;
			port_log("SSB64: lbReloc range table full (%u), file %u untracked\n",
			         (unsigned)PORT_RELOC_RANGES_MAX, (unsigned)file_id);
		}
		return;
	}
	sPortRelocFileRanges[sPortRelocFileRangesNum].base = (uintptr_t)base;
	sPortRelocFileRanges[sPortRelocFileRangesNum].size = size;
	sPortRelocFileRanges[sPortRelocFileRangesNum].file_id = file_id;
	sPortRelocFileRangesNum++;
}

static void portRelocEvictFileRangesInRange(const void *base, size_t size)
{
	uintptr_t begin, end;
	u32 i, j;

	if ((base == NULL) || (size == 0))
	{
		return;
	}
	begin = (uintptr_t)base;
	end = begin + size;

	/* Order-preserving compaction (BattleShip used std::remove_if). The
	 * DL-range registry is told first, while the base pointers are known. */
	for (i = j = 0; i < sPortRelocFileRangesNum; i++)
	{
		const PortRelocFileRange *r = &sPortRelocFileRanges[i];
		uintptr_t rb = r->base;
		uintptr_t re = rb + r->size;

		if ((r->size != 0) && (rb < end) && (begin < re))
		{
			port_dl_range_unregister((const void *)rb);
			continue;
		}
		if (j != i)
		{
			sPortRelocFileRanges[j] = *r;
		}
		j++;
	}
	sPortRelocFileRangesNum = j;
}

/* Walk a status-buffer array, drop every entry whose .addr falls inside
 * [lo, hi). Unordered swap-with-last erase: lookup scans the whole list, so
 * order doesn't matter. After this, lbRelocFindStatusBufferFile(id) for an
 * evicted file returns NULL and the next request reloads it into fresh memory
 * instead of handing out a pointer into a recycled arena. */
static void portStatusBufferEvictRange(LBFileNode *entries, s32 *p_num, uintptr_t lo, uintptr_t hi)
{
	s32 i = 0;

	if ((entries == NULL) || (p_num == NULL))
	{
		return;
	}
	while (i < *p_num)
	{
		uintptr_t a = (uintptr_t)entries[i].addr;

		if ((a >= lo) && (a < hi))
		{
			entries[i] = entries[*p_num - 1];
			(*p_num)--;
		}
		else i++;
	}
}

/* Called by syTaskmanStartTask before reusing the scene arena for the next
 * scene. Evicts every port-side cache that may hold a pointer into the old
 * arena's contents (texture uploads, struct fixups, reloc file ranges, status
 * buffer entries) so a stale lookup can't resolve through prior-scene state.
 *
 * The status-buffer part matters because that cache is only reset in
 * lbRelocInitSetup (mode entry), not per scene: across a transition that
 * skips a mode init, entries from the prior scene would point into the arena
 * that is about to be zeroed, and e.g. ftManagerSetupFilesKind would hand
 * those zeroed bytes to the fighter globals (untextured particles/sprites).
 * Intern-buffer entries lie outside the arena and survive. */
void port_taskman_evict_arena_caches(const void *base, size_t size)
{
	uintptr_t lo, hi;

	if ((base == NULL) || (size == 0))
	{
		return;
	}
	portTextureCacheDeleteRange(base, size);
	portEvictStructFixupsInRange(base, size);
	portRelocEvictFileRangesInRange(base, size);
	portRelocInvalidateRange(base, size);

	lo = (uintptr_t)base;
	hi = lo + size;
	portStatusBufferEvictRange(sLBRelocInternBuffer.status_buffer,
	                           &sLBRelocInternBuffer.status_buffer_num, lo, hi);
	portStatusBufferEvictRange(sLBRelocInternBuffer.force_status_buffer,
	                           &sLBRelocInternBuffer.force_status_buffer_num, lo, hi);
}

/* Fighter animation (reloc_animations/FT*) and submotion (reloc_submotions/FT*)
 * files hold u16 animation streams; after the blanket u32 swap each u16 pair
 * is position-swapped, so every non-chain word gets a 16-bit rotate. BattleShip
 * matched Torch path prefixes; in the US table those are exactly file ids
 * 357..2131 (the last file). SCExplainMain (id 252, llSCExplainMainFileID)
 * joins them: it holds the tutorial's FTKeyEvent u16 arrays, which read the
 * wrong half of each pair without the halfswap and end the button script. */
#define PORT_RELOC_FIGATREE_FIRST_ID 357u
#define PORT_RELOC_SCEXPLAINMAIN_ID  252u

static int portRelocIsFighterFigatreeFile(u32 file_id)
{
	return (file_id == PORT_RELOC_SCEXPLAINMAIN_ID) ||
	       ((file_id >= PORT_RELOC_FIGATREE_FIRST_ID) && (file_id < ROMDATA_RELOC_COUNT));
}

/* Chain-slot bitmap for the figatree halfswap: one bit per file word, set for
 * words the chain walks overwrote with pointers (those must not be rotated).
 * A file is at most 0xFFFF words (u16 size field), so 8 KiB covers any file.
 * The extern walk can recurse into dependency loads, so each nesting level
 * that loads a figatree file takes its own bitmap. Figatree files are leaves
 * in practice; 4 levels is ample. */
#define PORT_RELOC_MAX_WORDS      0x10000u
#define PORT_RELOC_FIGBITS_DEPTH  4

static u8 sFigatreeChainBits[PORT_RELOC_FIGBITS_DEPTH][PORT_RELOC_MAX_WORDS / 8];
static s32 sFigatreeChainBitsDepth;

static void portRelocFixupFighterFigatree(void *ram_dst, size_t copy_size, const u8 *chain_bits)
{
	u32 *words = (u32 *)ram_dst;
	size_t word_count, i;

	if ((ram_dst == NULL) || (copy_size < sizeof(u32)))
	{
		return;
	}
	word_count = copy_size / sizeof(u32);

	for (i = 0; i < word_count; i++)
	{
		if (!(chain_bits[i >> 3] & (1u << (i & 7))))
		{
			words[i] = (words[i] << 16) | (words[i] >> 16);
		}
	}
}

// // // // // // // // // // // //
//                               //
//      STATUS BUFFER FUNCS      //
//                               //
// // // // // // // // // // // //

void *lbRelocFindStatusBufferFile(u32 id)
{
	s32 i;

	if (sLBRelocInternBuffer.status_buffer_num == 0)
	{
		return NULL;
	}
	else for (i = 0; i < sLBRelocInternBuffer.status_buffer_num; i++)
	{
		if (id == sLBRelocInternBuffer.status_buffer[i].id)
		{
			return sLBRelocInternBuffer.status_buffer[i].addr;
		}
	}
	return NULL;
}

void *lbRelocGetStatusBufferFile(u32 id)
{
	return lbRelocFindStatusBufferFile(id);
}

void *lbRelocFindForceStatusBufferFile(u32 id)
{
	s32 i;

	if (sLBRelocInternBuffer.force_status_buffer_num != 0)
	{
		for (i = 0; i < sLBRelocInternBuffer.force_status_buffer_num; i++)
		{
			if (id == sLBRelocInternBuffer.force_status_buffer[i].id)
			{
				return sLBRelocInternBuffer.force_status_buffer[i].addr;
			}
		}
	}
	return lbRelocFindStatusBufferFile(id);
}

void *lbRelocGetForceStatusBufferFile(u32 id)
{
	return lbRelocFindForceStatusBufferFile(id);
}

void lbRelocAddStatusBufferFile(u32 id, void *addr)
{
	u32 num = sLBRelocInternBuffer.status_buffer_num;

	if (num >= (u32)sLBRelocInternBuffer.status_buffer_max)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Status Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.status_buffer[num].id = id;
	sLBRelocInternBuffer.status_buffer[num].addr = addr;
	sLBRelocInternBuffer.status_buffer_num++;
}

void lbRelocAddForceStatusBufferFile(u32 id, void *addr)
{
	u32 num = sLBRelocInternBuffer.force_status_buffer_num;

	if (num >= (u32)sLBRelocInternBuffer.force_status_buffer_max)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Force Status Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.force_status_buffer[num].id = id;
	sLBRelocInternBuffer.force_status_buffer[num].addr = addr;
	sLBRelocInternBuffer.force_status_buffer_num++;
}

// // // // // // // // // // // //
//                               //
//   BRIDGE: LOAD & RELOCATE     //
//                               //
// // // // // // // // // // // //

/* Read file `file_id` from the ROM into ram_dst, byte-swap it to native order
 * and resolve its internal and external pointer chains.
 *
 * Chain word layout (both chains), after the blanket u32 swap:
 *   bits [31:16] = next descriptor offset (words), 0xFFFF = end
 *   bits [15:0]  = target offset (words) within this file (intern) or
 *                  within the dependency file (extern)
 * The descriptor word is overwritten with the target pointer, exactly as on
 * the N64 (ILP32: the pointer is the token). */
static void lbRelocLoadAndRelocFileImpl(u32 file_id, void *ram_dst, u32 bytes_num, s32 loc)
{
	RomRelocInfo info;
	size_t copySize;
	int is_fighter_figatree;
	u8 *chain_bits = NULL;
	u16 reloc_intern, reloc_extern;
	u32 intern_steps, extern_steps, extern_idx;

	if (romdata_reloc_info(file_id, &info) != 0)
	{
		port_log("SSB64: lbReloc: invalid file_id %u\n", (unsigned)file_id);
		return;
	}
	if (info.size == 0)
	{
		port_log("SSB64: lbReloc: empty file_id %u\n", (unsigned)file_id);
		return;
	}
	copySize = info.size;

	/* BattleShip truncated an oversize file to bytes_num. Decompression here
	 * goes straight into ram_dst and can't stop part-way, and no caller passes
	 * less than the file size (they all size from lbRelocGetFileSize or
	 * info.size), so refuse rather than keep a scratch buffer for it. */
	if ((bytes_num != 0) && (copySize > bytes_num))
	{
		port_log("SSB64: lbReloc: file_id %u (%u bytes) exceeds buffer (%u bytes), not loaded\n",
		         (unsigned)file_id, (unsigned)copySize, (unsigned)bytes_num);
		return;
	}
	is_fighter_figatree = portRelocIsFighterFigatreeFile(file_id);

	/* Invalidate address-keyed state for the region about to be overwritten.
	 * Bump-reset heaps (e.g. the stage-select wallpaper heaps rewound by
	 * lbRelocGetForceExternHeapFile on every cursor tick) reuse addresses
	 * across files: without this the struct-fixup tracker skips the new
	 * file's sprites/bitmaps and the texture cache serves the previous
	 * file's texels for a same-shape texture at the same address. */
	portEvictStructFixupsInRange(ram_dst, copySize);
	portTextureCacheDeleteRange(ram_dst, copySize);
	portRelocEvictFileRangesInRange(ram_dst, copySize);

	if (romdata_reloc_read(file_id, ram_dst, (u32)copySize) != 0)
	{
		port_log("SSB64: lbReloc: failed to read file_id %u from ROM\n", (unsigned)file_id);
		return;
	}

	/* Dependencies loaded by the extern walk below bump-allocate from
	 * sLBRelocExternFileHeap. lbRelocGet*BufferFile already advanced it past
	 * this file; doing it here as well keeps a direct caller of this function
	 * from having dependencies land on top of the file being walked. */
	if ((loc == nLBFileLocationExtern) || (loc == nLBFileLocationForce))
	{
		sLBRelocExternFileHeap = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)ram_dst + copySize);
	}

	/* N64 big-endian -> native. Must precede the chain walks, which read the
	 * descriptor u16s out of native u32 words. */
	portRelocByteSwapBlob(ram_dst, copySize, (unsigned int)file_id);

	if (loc == nLBFileLocationForce)
	{
		lbRelocAddForceStatusBufferFile(file_id, ram_dst);
	}
	else lbRelocAddStatusBufferFile(file_id, ram_dst);

	portRelocAddFileRange(ram_dst, copySize, file_id);
	port_dl_range_register(ram_dst, copySize, sPortRelocPathLabel);

	if (is_fighter_figatree)
	{
		if (sFigatreeChainBitsDepth < PORT_RELOC_FIGBITS_DEPTH)
		{
			chain_bits = sFigatreeChainBits[sFigatreeChainBitsDepth++];
			memset(chain_bits, 0, (copySize / sizeof(u32) + 7) / 8);
		}
		else
		{
			port_log("SSB64: lbReloc: figatree nesting too deep at file_id %u, halfswap skipped\n",
			         (unsigned)file_id);
			is_fighter_figatree = FALSE;
		}
	}

	// --- Internal pointer relocation ---
	reloc_intern = info.reloc_intern;
	intern_steps = 0;

	while (reloc_intern != 0xFFFF)
	{
		u32 *slot;
		u16 next_reloc, words_num;

		/* Containment: a pristine chain only references word offsets inside
		 * its own file and visits each slot once, so a legit walk can't take
		 * more steps than the file has words nor step outside it. Stop on a
		 * corrupt chain word instead of wandering through memory; the file
		 * keeps every fixup applied up to that point. */
		if (((size_t)reloc_intern * sizeof(u32) + sizeof(u32) > copySize) ||
		    (++intern_steps > (u32)(copySize / sizeof(u32))))
		{
			port_log("SSB64: lbReloc: intern chain walk stopped file=%u off=0x%x steps=%u size=0x%x\n",
			         (unsigned)file_id, (unsigned)reloc_intern, (unsigned)intern_steps, (unsigned)copySize);
			break;
		}
		slot = (u32 *)((uintptr_t)ram_dst + (reloc_intern * sizeof(u32)));

		next_reloc = (u16)(*slot >> 16);
		words_num  = (u16)(*slot & 0xFFFF);

		/* An intern target outside the file means this is not a chain word. */
		if ((size_t)words_num * sizeof(u32) >= copySize)
		{
			port_log("SSB64: lbReloc: intern chain target OOB file=%u slot=0x%x tgt=0x%x size=0x%x\n",
			         (unsigned)file_id, (unsigned)reloc_intern, (unsigned)words_num, (unsigned)copySize);
			break;
		}
		/* If this slot is the w1 of a G_SETTIMG (or G_VTX), the chain tells
		 * us where the texture (vertex) bytes live; the byte-swap layer
		 * restores their byte order here because its static DL scan can't
		 * see chain-encoded addresses. Idempotent. */
		portRelocFixupTextureFromChain(ram_dst, copySize,
		                               (unsigned int)(reloc_intern * sizeof(u32)),
		                               (unsigned int)(words_num * sizeof(u32)));

		if (chain_bits != NULL)
		{
			chain_bits[reloc_intern >> 3] |= (u8)(1u << (reloc_intern & 7));
		}
		*slot = portRelocRegisterPointer((void *)((uintptr_t)ram_dst + (words_num * sizeof(u32))));
		portRelocNoteChainSlot(slot);

		reloc_intern = next_reloc;
	}

	// --- External pointer relocation ---
	reloc_extern = info.reloc_extern;
	extern_idx = 0;
	extern_steps = 0;

	while (reloc_extern != 0xFFFF)
	{
		u32 *slot;
		u16 next_reloc, words_num, dep_file_id;
		void *vaddr_extern;

		/* Same containment as the intern walk. words_num indexes the
		 * dependency file, so it can't be bounds-checked here. */
		if (((size_t)reloc_extern * sizeof(u32) + sizeof(u32) > copySize) ||
		    (++extern_steps > (u32)(copySize / sizeof(u32))))
		{
			port_log("SSB64: lbReloc: extern chain walk stopped file=%u off=0x%x steps=%u size=0x%x\n",
			         (unsigned)file_id, (unsigned)reloc_extern, (unsigned)extern_steps, (unsigned)copySize);
			break;
		}
		slot = (u32 *)((uintptr_t)ram_dst + (reloc_extern * sizeof(u32)));

		next_reloc = (u16)(*slot >> 16);
		words_num  = (u16)(*slot & 0xFFFF);

		if (extern_idx >= info.extern_count)
		{
			port_log("SSB64: lbReloc: file_id %u extern reloc overrun (idx=%u count=%u)\n",
			         (unsigned)file_id, (unsigned)extern_idx, (unsigned)info.extern_count);
			break;
		}
		dep_file_id = romdata_extern_id(&info, extern_idx);

		if (loc == nLBFileLocationForce)
		{
			vaddr_extern = lbRelocFindForceStatusBufferFile(dep_file_id);
		}
		else vaddr_extern = lbRelocFindStatusBufferFile(dep_file_id);

		if (vaddr_extern == NULL)
		{
			switch (loc)
			{
			case nLBFileLocationExtern:
				vaddr_extern = lbRelocGetExternBufferFile(dep_file_id);
				break;

			case nLBFileLocationDefault:
				vaddr_extern = lbRelocGetInternBufferFile(dep_file_id);
				break;

			case nLBFileLocationForce:
				vaddr_extern = lbRelocGetForceExternBufferFile(dep_file_id);
				break;
			}
		}
		if (chain_bits != NULL)
		{
			chain_bits[reloc_extern >> 3] |= (u8)(1u << (reloc_extern & 7));
		}
		*slot = portRelocRegisterPointer((void *)((uintptr_t)vaddr_extern + (words_num * sizeof(u32))));
		portRelocNoteChainSlot(slot);

		extern_idx++;
		reloc_extern = next_reloc;
	}

	if (chain_bits != NULL)
	{
		portRelocFixupFighterFigatree(ram_dst, copySize, chain_bits);
		sFigatreeChainBitsDepth--;

		/* Tell the AObj EVENT32 un-halfswapper this range was halfswapped, so
		 * it only ever touches streams inside such files. */
		port_aobj_register_halfswapped_range(ram_dst, (unsigned long)copySize);
	}
}

/* Timed for the PERF2 line (main.c): the loading work inside the logic tick. */
extern uint32_t gPortRelocUs, gPortRelocLoads;
extern uint32_t port_time_us(void);
void lbRelocLoadAndRelocFile(u32 file_id, void *ram_dst, u32 bytes_num, s32 loc)
{
	const uint32_t t0 = port_time_us();
	lbRelocLoadAndRelocFileImpl(file_id, ram_dst, bytes_num, loc);
	gPortRelocUs += port_time_us() - t0;
	gPortRelocLoads++;
}

// // // // // // // // // // // //
//                               //
//    BRIDGE: SIZE CALCULATION   //
//                               //
// // // // // // // // // // // //

size_t lbRelocGetExternBytesNum(u32 file_id)
{
	RomRelocInfo info;
	size_t bytes_read;
	u32 i;
	s32 j;

	if (lbRelocFindStatusBufferFile(file_id) != NULL)
	{
		return 0;
	}
	for (j = 0; j < sLBRelocExternFileIDsNum; j++)
	{
		if (file_id == sLBRelocExternFileIDs[j])
		{
			return 0;
		}
	}
	if (sLBRelocExternFileIDsNum >= sLBRelocExternFileIDsMax)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: External Data is over %d!!\n", sLBRelocExternFileIDsMax);
			scManagerRunPrintGObjStatus();
		}
	}
	if (romdata_reloc_info(file_id, &info) != 0)
	{
		port_log("SSB64: lbReloc: invalid file_id %u\n", (unsigned)file_id);
		return 0;
	}
	bytes_read = LBRELOC_CACHE_ALIGN((size_t)info.size);
	sLBRelocExternFileIDs[sLBRelocExternFileIDsNum++] = file_id;

	for (i = 0; i < info.extern_count; i++)
	{
		bytes_read += lbRelocGetExternBytesNum(romdata_extern_id(&info, i));
	}
	return bytes_read;
}

size_t lbRelocGetFileSize(u32 id)
{
	u32 file_ids_extern_buf[50];

	sLBRelocExternFileIDs = file_ids_extern_buf;
	sLBRelocExternFileIDsNum = 0;
	sLBRelocExternFileIDsMax = LBRELOC_ARRAY_COUNT(file_ids_extern_buf);

	return lbRelocGetExternBytesNum(id);
}

// // // // // // // // // // // //
//                               //
//     BRIDGE: FILE LOADING      //
//                               //
// // // // // // // // // // // //

/* Decompressed size of a file, or 0 (after logging) for a bad id. */
static u32 portRelocFileSize(u32 id)
{
	RomRelocInfo info;

	if (romdata_reloc_info(id, &info) != 0)
	{
		port_log("SSB64: lbReloc: invalid file_id %u\n", (unsigned)id);
		return 0;
	}
	return info.size;
}

void *lbRelocGetExternBufferFile(u32 id)
{
	void *file = lbRelocFindStatusBufferFile(id);
	void *file_alloc;
	u32 file_size;

	if (file != NULL)
	{
		return file;
	}
	file_size = portRelocFileSize(id);

	if (file_size == 0)
	{
		return NULL;
	}
	file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocExternFileHeap);
	sLBRelocExternFileHeap = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, file_size, nLBFileLocationExtern);

	return file_alloc;
}

void *lbRelocGetExternHeapFile(u32 id, void *heap)
{
	sLBRelocExternFileHeap = heap;

	return lbRelocGetExternBufferFile(id);
}

void *lbRelocGetInternBufferFile(u32 id)
{
	void *file = lbRelocFindStatusBufferFile(id);
	void *file_alloc;
	u32 file_size;

	if (file != NULL)
	{
		return file;
	}
	file_size = portRelocFileSize(id);

	if (file_size == 0)
	{
		return NULL;
	}
	file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocInternBuffer.heap_ptr);

	if (((uintptr_t)file_alloc + file_size) > (uintptr_t)sLBRelocInternBuffer.heap_end)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.heap_ptr = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, file_size, nLBFileLocationDefault);

	return file_alloc;
}

void *lbRelocGetForceExternBufferFile(u32 id)
{
	void *file = lbRelocFindForceStatusBufferFile(id);
	void *file_alloc;
	u32 file_size;

	if (file != NULL)
	{
		return file;
	}
	file_size = portRelocFileSize(id);

	if (file_size == 0)
	{
		return NULL;
	}
	file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocExternFileHeap);
	sLBRelocExternFileHeap = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, file_size, nLBFileLocationForce);

	return file_alloc;
}

void *lbRelocGetForceExternHeapFile(u32 id, void *heap)
{
	/* Rewinding abandons prior loads above `heap`; their range/fixup entries
	 * linger until a load over the same addresses evicts them. Do NOT evict
	 * [heap, sLBRelocExternFileHeap) here: that cursor is shared with the
	 * extern-heap bump path, so the span can contain live extern files
	 * (BattleShip verified this crashes scene 28). */
	sLBRelocExternFileHeap = heap;
	sLBRelocInternBuffer.force_status_buffer_num = 0;

	return lbRelocGetForceExternBufferFile(id);
}

// // // // // // // // // // // //
//                               //
//     BRIDGE: BATCH LOADING     //
//                               //
// // // // // // // // // // // //

size_t lbRelocLoadFilesExtern(u32 *ids, u32 len, void **files, void *heap)
{
	sLBRelocExternFileHeap = heap;

	while (len != 0)
	{
		*files = lbRelocGetExternBufferFile(*ids);
		ids++, files++, len--;
	}
	return (size_t)((uintptr_t)sLBRelocExternFileHeap - (uintptr_t)heap);
}

size_t lbRelocLoadFilesIntern(u32 *ids, u32 len, void **files)
{
	void *heap = sLBRelocInternBuffer.heap_ptr;

	while (len != 0)
	{
		*files = lbRelocGetInternBufferFile(*ids);
		ids++, files++, len--;
	}
	return (size_t)((uintptr_t)sLBRelocInternBuffer.heap_ptr - (uintptr_t)heap);
}

size_t lbRelocGetAllocSize(u32 *ids, u32 len)
{
	u32 file_ids[50];
	size_t allocated = 0;

	sLBRelocExternFileIDs = file_ids;
	sLBRelocExternFileIDsNum = 0;
	sLBRelocExternFileIDsMax = LBRELOC_ARRAY_COUNT(file_ids);

	while (len != 0)
	{
		allocated = LBRELOC_CACHE_ALIGN(allocated);
		allocated += lbRelocGetExternBytesNum(*ids);
		ids++, len--;
	}
	return allocated;
}

// // // // // // // // // // // //
//                               //
//    BRIDGE: RANGE LOOKUPS      //
//                               //
// // // // // // // // // // // //

static const PortRelocFileRange *portRelocFindRange(const void *ptr)
{
	uintptr_t addr = (uintptr_t)ptr;
	u32 i;

	for (i = 0; i < sPortRelocFileRangesNum; i++)
	{
		const PortRelocFileRange *r = &sPortRelocFileRanges[i];

		if ((addr >= r->base) && (r->size != 0) && ((addr - r->base) < r->size))
		{
			return r;
		}
	}
	return NULL;
}

/* Returns 1 if ptr lies inside a loaded reloc file (and reports its range). */
int portRelocFindContainingFile(const void *ptr, uintptr_t *out_base, size_t *out_size)
{
	const PortRelocFileRange *r = portRelocFindRange(ptr);

	if (r == NULL)
	{
		return FALSE;
	}
	if (out_base != NULL)
	{
		*out_base = r->base;
	}
	if (out_size != NULL)
	{
		*out_size = r->size;
	}
	return TRUE;
}

/* File id and base of the file containing ptr, or -1. */
int portRelocFindFileIdAndBase(const void *ptr, uintptr_t *out_base)
{
	const PortRelocFileRange *r = portRelocFindRange(ptr);

	if (r == NULL)
	{
		return -1;
	}
	if (out_base != NULL)
	{
		*out_base = r->base;
	}
	return (int)r->file_id;
}

/* Returns int (0/1); the decomp declares it bool, which reads the same. */
int portRelocDescribePointer(const void *ptr, uintptr_t *out_base, size_t *out_size, u32 *out_file_id, const char **out_path)
{
	const PortRelocFileRange *r = portRelocFindRange(ptr);

	if (r == NULL)
	{
		return FALSE;
	}
	if (out_base != NULL)
	{
		*out_base = r->base;
	}
	if (out_size != NULL)
	{
		*out_size = r->size;
	}
	if (out_file_id != NULL)
	{
		*out_file_id = r->file_id;
	}
	if (out_path != NULL)
	{
		*out_path = sPortRelocPathLabel;
	}
	return TRUE;
}

/* Entry `index` of a pointer array (PORT_RESOLVE_ARRAY). Inside a reloc file
 * the entry is bounds-checked against the file, as BattleShip did; on ILP32
 * the stored word is the pointer in either case. */
void *portRelocResolveArrayEntry(const void *array_ptr, unsigned int index)
{
	uintptr_t base = 0;
	size_t size = 0;

	if (array_ptr == NULL)
	{
		return NULL;
	}
	if (portRelocFindContainingFile(array_ptr, &base, &size))
	{
		size_t entry_offset = ((uintptr_t)array_ptr - base) + ((size_t)index * sizeof(u32));

		if ((entry_offset > size) || ((size - entry_offset) < sizeof(u32)))
		{
			return NULL;
		}
		return (void *)(uintptr_t)((const u32 *)array_ptr)[index];
	}
	return ((void *const *)array_ptr)[index];
}

/* DL-range registry (BattleShip port/port_dl_ranges.cpp) existed so the
 * libultraship interpreter could stop a runaway DL walk at the end of a
 * registered range. The handheld renderer has no such check, so registration is
 * a no-op; the calls stay so the load/evict sequence matches BattleShip. */
void port_dl_range_register(const void *base, size_t size, const char *label)
{
	(void)base, (void)size, (void)label;
}

void port_dl_range_unregister(const void *base)
{
	(void)base;
}

// // // // // // // // // // // //
//                               //
//    BRIDGE: INITIALIZATION     //
//                               //
// // // // // // // // // // // //

void lbRelocInitSetup(LBRelocSetup *setup)
{
	/* Compiled-in collision / demo scripts carry pointers into reloc data
	 * that the decomp re-links on every setup. */
	gmColScriptsLinkRelocTargets();
	scSubsysMotionLinkRelocTargets();

	/* All file heaps are being reset: every range, fixup mark and
	 * framebuffer-mirror registration keyed on their addresses is stale.
	 * Without the capture reset a fresh asset landing at a prior mirror's
	 * address would render the old snapshot instead of its own pixels. */
	sPortRelocFileRangesNum = 0;
	portResetStructFixups();
	port_capture_release_all();

	sLBRelocInternBuffer.rom_table_lo = setup->table_addr;
	sLBRelocInternBuffer.total_files_num = setup->table_files_num;
	sLBRelocInternBuffer.rom_table_hi = setup->table_addr + ((setup->table_files_num + 1) * sizeof(LBTableEntry));

	sLBRelocInternBuffer.heap_start = sLBRelocInternBuffer.heap_ptr = setup->file_heap;
	sLBRelocInternBuffer.heap_end = (void *)((uintptr_t)setup->file_heap + setup->file_heap_size);

	sLBRelocInternBuffer.status_buffer_num = 0;
	sLBRelocInternBuffer.status_buffer_max = setup->status_buffer_size;
	sLBRelocInternBuffer.status_buffer = setup->status_buffer;

	sLBRelocInternBuffer.force_status_buffer_max = setup->force_status_buffer_size;
	sLBRelocInternBuffer.force_status_buffer_num = 0;
	sLBRelocInternBuffer.force_status_buffer = setup->force_status_buffer;

	sFigatreeChainBitsDepth = 0;
}

// // // // // // // // // // // //
//                               //
//   POINTER TOKENS (identity)   //
//                               //
// // // // // // // // // // // //

/* BattleShip's RelocPointerTable maps 64-bit pointers to 32-bit tokens so they
 * fit the N64's 4-byte slots. On ILP32 the pointer already fits: a token IS
 * the pointer value, there is no table, and nothing can go stale except the
 * memory itself (which the eviction calls above handle). */
unsigned int portRelocRegisterPointer(void *ptr)
{
	return (unsigned int)(uintptr_t)ptr;
}

void *portRelocResolvePointer(unsigned int token)
{
	return (void *)(uintptr_t)token;
}

void *portRelocResolvePointerDebug(unsigned int token, const char *file, int line)
{
	(void)file, (void)line;
	return (void *)(uintptr_t)token;
}

void *portRelocTryResolvePointer(unsigned int token)
{
	return (void *)(uintptr_t)token;
}

void portRelocResetPointerTable(void)
{
}

void portRelocInvalidateRange(const void *base, size_t size)
{
	(void)base, (void)size;
}
