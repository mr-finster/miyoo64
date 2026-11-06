/* The native port's ROM identity is fixed at compile time.  The PAL kit is
 * built with PM64_ROM_REGION_PAL by gen_build.py; the original US build keeps
 * its previous default without any additional flag.  This header is shared by
 * the streaming reader and the asset/ROM-finder path. */
#ifndef PM64_ROM_IDENTITY_H
#define PM64_ROM_IDENTITY_H
#if defined(PM64_ROM_REGION_PAL)
#define PM64_ROM_CODE "NMQP"
#define PM64_ROM_SHA1 "2111d39265a317414d359e35a7d971c4dfa5f9e1"
#define PM64_ROM_NAME "Paper Mario (Europe, PAL)"
#define PM64_ROM_WRONG_VERSION "not the PAL (Europe) version"
#define PM64_ROM_CART_HINT "It must be the European (PAL) cart's dump."
#else
#define PM64_ROM_CODE "NMQE"
#define PM64_ROM_SHA1 "3837f44cda784b466c9a2d99df70d77c322b97a0"
#define PM64_ROM_NAME "Paper Mario (USA)"
#define PM64_ROM_WRONG_VERSION "not the US (NTSC) version"
#define PM64_ROM_CART_HINT "It must be the USA cart's dump."
#endif
#endif
