/* oot-PSP's VFPU block copy; plain memcpy here (libc's is NEON-optimised on ARM). */
#include <string.h>

void* OotPsp_MemcpyVfpu(void* dst, const void* src, size_t size) {
    return memcpy(dst, src, size);
}
