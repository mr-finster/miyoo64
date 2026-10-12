/**
 * Rare's "rzip" (official names rzipUncompress / rzipUncompressSize*): a
 * 4-byte little-endian uncompressed size, one more header byte, then a raw
 * DEFLATE stream. The decomp's gzip.c keeps the inflate loop only as MIPS asm
 * (src/hasm/gzip_asm.s), so the port replaces the module with zlib.
 */
#include <string.h>
#include <zlib.h>

#include <ultra64.h>
#include "memory.h"
#include "asset_loading.h"
#include "gzip.h"
#include "port.h"

static s32 *sPackedHeader;
/* gzip.c's global: one past the last byte written by the latest inflate
 * (tracks.c sizes the level model with it). */
u8 *gzip_inflate_output;

void gzip_init(void) {
    /* Same pool allocations as the N64 (0x2800 huft table + 16-byte header), so
     * the pool layout -- and everything allocated after it -- matches the original. */
    (void) mempool_alloc_safe(0x2800, COLOUR_TAG_BLACK);
    sPackedHeader = (s32 *) mempool_alloc_safe(0x10, COLOUR_TAG_BLACK);
}

s32 byteswap32(u8 *p) {
    return (s32) ((u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24));
}

s32 gzip_size_uncompressed(s32 assetIndex, s32 assetOffset) {
    asset_load(assetIndex, (u32) sPackedHeader, assetOffset, 8);
    return byteswap32((u8 *) sPackedHeader);
}

u8 *gzip_inflate(u8 *compressedInput, u8 *decompressedOutput) {
    const u32 outSize = (u32) byteswap32(compressedInput);
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = compressedInput + 5;
    zs.avail_in = 0x7FFFFFFF; /* the stream is self-terminating */
    zs.next_out = decompressedOutput;
    zs.avail_out = outSize;
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        port_log_always("gzip: inflateInit2 failed\n");
        return decompressedOutput;
    }
    const int r = inflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END) {
        port_log_always("gzip: inflate %d at %p (out %u/%u)\n", r, compressedInput, (unsigned) zs.total_out, (unsigned) outSize);
    }
    inflateEnd(&zs);
    gzip_inflate_output = decompressedOutput + zs.total_out;
    return decompressedOutput;
}
