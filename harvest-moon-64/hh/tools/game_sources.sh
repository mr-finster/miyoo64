#!/bin/sh
# The game's C files: the decomp Makefile's CODE_OBJECTS under src/ (what the N64 build links into its code segment)
grep -oE 'BUILD_DIR\)/src/[a-zA-Z0-9_/.-]+\.o' Makefile | sed 's|BUILD_DIR)/||; s/\.o$/.c/' | sort -u
# sound: libmus, the nusys audio manager, libultra's n_audio (the decomp Makefile.common's lists)
grep -oE 'BUILD_DIR\)/lib/(libmus|nusys-1/src/nualstl|libultra/src/libnaudio)/[a-zA-Z0-9_/]+\.o' Makefile.common | sed 's|BUILD_DIR)/||; s/\.o$/.c/' | sort -u
