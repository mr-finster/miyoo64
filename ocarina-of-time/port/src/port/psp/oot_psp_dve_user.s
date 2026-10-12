    .set noreorder

#include "pspstub.s"

    .section .lib.stub,"a",@progbits
    .global ootPspDveImport
ootPspDveImport:
    /* Weak import: the application must still start on a PSP-1000, where the
     * kernel DVE module is intentionally never loaded. */
    STUB_START "pspDveManager",0x40090000,0x00030005
    STUB_FUNC 0x2ACFCB6D,pspDveMgrCheckVideoOut
    STUB_FUNC 0xF9C86C73,pspDveMgrSetVideoOut
    STUB_FUNC 0xC8CCE483,pspDveMgrSetEdramSize
    STUB_END
