#ifndef _ITEM_H_
#define _ITEM_H_

#include "ittypes.h"
#include "itfunctions.h"
#ifdef PORT

/* PORT: without this include, every item .c file calls
 * ifCommonItemArrowMakeInterface without a prototype in scope. Under
 * C's implicit-function rule it then gets treated as returning `int`,
 * and the 64-bit GObj* return is truncated to 32 bits at the call site:
 *   ip->arrow_gobj = ifCommonItemArrowMakeInterface(ip);
 * On LP64 the stored pointer has its upper 32 bits zeroed, so the next
 * time gcEjectGObj(ip->arrow_gobj) runs (inside itMainDestroyItem), it
 * dereferences a bogus sub-4GiB address and segfaults on item despawn.
 * CMakeLists silences -Wimplicit-function-declaration for decomp
 * compatibility, which is what let this slip through. */
#include <if/ifcommon.h>
#endif

// Global variables declared here as extern for easy access

// 0x8018D040
extern void *gITManagerCommonData;

// 0x8018D044
extern s32 gITManagerParticleBankID;

// 0x8018D048
extern ITRandomWeights gITManagerRandomWeights;

// 0x8018D060
extern ITMonsterData gITManagerMonsterData;

// 0x8018D090
extern s32 gITManagerDisplayMode;

// 0x8018D094
extern ITStruct* gITManagerStructsAllocFree;

// 0x8018D098
extern ITAppearActor gITManagerAppearActor;

// Global data

// 0x80189450
extern s32 dITManagerForceMonsterKind;

// Linker variable, points to base of animation bank in item file? 0x00013624
extern intptr_t lITMonsterAnimBankStart;

#define itGetStruct(item_gobj) ((ITStruct*)(item_gobj)->user_data.p)

// Points to all sorts of data
#ifdef PORT
/* PORT: attr->data is a u32 relocation token; PORT_RESOLVE() converts it to a
 * real pointer at access time. Without the resolve the offset arithmetic below
 * would treat the token integer as a virtual address and dereference garbage. */
#define itGetPData(ip, off1, off2)                                                                                     \
	((void*)(((uintptr_t)PORT_RESOLVE((ip)->attr->data) - (intptr_t) (off1)) + (intptr_t) (off2)))
#else
#define itGetPData(ip, off1, off2)                                                                                     \
	((void*)(((uintptr_t)(ip)->attr->data - (intptr_t) (off1)) + (intptr_t) (off2)))
#endif

#ifdef PORT
/* PORT: same token-resolve treatment as itGetPData. The bank-start anchor is a
 * file_id constant (#define from reloc_data.h) rather than a linker symbol, so
 * we drop the address-of operator. */
#define itGetMonsterAnimNode(ip, off)                                                                                  \
	((void*)(((uintptr_t)PORT_RESOLVE((ip)->attr->data) - (intptr_t) (off)) + (intptr_t)llITCommonDataMonsterAnimBankStart))
#else
#define itGetMonsterAnimNode(ip, off)                                                                                  \
	((void*)(((uintptr_t)(ip)->attr->data - (intptr_t) (off)) + (intptr_t)&llITCommonDataMonsterAnimBankStart))
#endif

#define itGetAttackEvent(it_desc, off) ((ITAttackEvent*)((uintptr_t) * (it_desc).p_file + (intptr_t) (off)))

#define itGetMonsterEvent(it_desc, off) ((ITMonsterEvent*)((uintptr_t) * (it_desc).p_file + (intptr_t) (off)))

#endif
