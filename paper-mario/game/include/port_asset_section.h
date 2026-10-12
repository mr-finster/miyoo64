/* The native port: the data defined from here on is ROM-derived (a file the decomp's split generates from the ROM:
 * vertices, display lists, image-effect keyframes), so it goes to .assetdata, the region the device rebuilds from the
 * player's ROM at first boot (tools/assets). Included through the wrappers gen_build.py writes for the split's
 * generated .inc.c files, and with -include for whole generated files. */
#pragma clang section data = ".assetdata" rodata = ".assetrodata"
