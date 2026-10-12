#include "malloc.h"

#include <sys/debug.h>
#include <ssb_types.h>
#include <PR/ultratypes.h>

void syMallocReset(SYMallocRegion *bp)
{
    bp->ptr = bp->start;
}

void* syMallocSet(SYMallocRegion *bp, size_t size, u32 alignment)
{
    u8 *aligned;
#ifdef PORT
    uintptr_t aligned_addr;
    uintptr_t offset = 0;
#else
    u32 offset;
#endif

    if (alignment != 0) 
    {
#ifdef PORT
        // alignment must be a power of two for mask-based rounding
        if ((alignment & (alignment - 1)) != 0)
        {
            syDebugPrintf("ml : invalid alignment #%d (%d)\n", bp->id, alignment);
            while (TRUE);
        }
        offset       = (uintptr_t)(alignment - 1);
        aligned_addr = ((uintptr_t)bp->ptr + offset) & ~offset;
        aligned      = (u8*)aligned_addr;
#else
        offset  = alignment - 1;
        aligned = (u8*) (((uintptr_t)bp->ptr + (offset)) & ~(offset));
#endif
    } 
#ifdef PORT
    else
    {
        aligned_addr = (uintptr_t)bp->ptr;
        aligned      = (u8*)bp->ptr;
    }
#else
    else aligned = (u8*) bp->ptr;
#endif

#ifdef PORT
    bp->ptr = (void*)(aligned_addr + size);
#else
    bp->ptr = (void*) (aligned + size);
#endif

#ifdef PORT
    if (((uintptr_t)bp->end < (uintptr_t)bp->ptr) || ((uintptr_t)bp->ptr < (uintptr_t)bp->start))
#else
    if (bp->end < bp->ptr)
#endif
    {
        syDebugPrintf("ml : alloc overflow #%d\n", bp->id);

        while (TRUE);
    }
    return (void*) aligned;
}

void syMallocInit(SYMallocRegion *bp, u32 id, void *start, size_t size)
{
    bp->id    = id;
    bp->ptr   = start;
    bp->start = start;
    bp->end   = (void*) ((uintptr_t)start + size);
}
