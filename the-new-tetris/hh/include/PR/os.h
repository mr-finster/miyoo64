/* Port shim: libultra's os.h declares bcopy/bcmp/bzero with int sizes, which clashes
 * with the host's <strings.h> in port-layer files that include both. There the libultra
 * prototypes are declared under throwaway names; game files see os.h unchanged. */
#if defined(HH_PORT_LAYER)
#define bcopy hh_n64_bcopy_decl
#define bcmp hh_n64_bcmp_decl
#define bzero hh_n64_bzero_decl
#include_next <PR/os.h>
#undef bcopy
#undef bcmp
#undef bzero
#else
#include_next <PR/os.h>
#endif
