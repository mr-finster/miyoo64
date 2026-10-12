# Diddy Kong Racing -> Miyoo Mini port build.
#
#   make PLAT=pc      -j$(nproc)   # desktop 32-bit x86 proxy (SDL2), the debug/regression target
#   make PLAT=miyoo   -j$(nproc)   # Miyoo Mini cross build (SDL 1.2, glibc, 320x240 doubled)
#
# Game code: decomp/ (DavidSM64/Diddy-Kong-Racing, CC0, see decomp/UPSTREAM),
# compiled NON_MATCHING for us.v80 with the port's edits under #ifdef PORT.
# Platform: port/ (libultra shim, F3DDKR display-list front end, software
# rasterizer, SDL backends -- the MK64 port's platform kit, adapted).
#
# The executable carries no game data. Assets are read at run time from
# dkr_assets.bin, which the device extracts from the player's own ROM at first
# boot (port/sdl/assets_dkr.c). The trig tables are generated here from their
# closed forms (tools/gen_trig_tables.py), not copied from the ROM.

PLAT     ?= pc
VERSION  := us_v80
BUILD    := build/$(PLAT)$(if $(filter 1,$(UBSAN)),-ubsan)
ELF      := $(BUILD)/dkr
PYTHON   ?= python3

# Asset ROM offsets (us.v80): the LUT and the start of the asset data. The PI
# shim serves these offsets from dkr_assets.bin, extracted from the player's ROM.
ASSETS_LUT_START := 0xED0E0
ASSETS_LUT_END   := 0xED1B0
ASSETS_END       := 0xB8CFD0

V := @
ifeq ($(VERBOSE),1)
  V :=
endif

ifeq ($(PLAT),miyoo)
  # Miyoo Mini / Mini Plus (SigmaStar SSD202D: dual Cortex-A7, NEON-VFPv4, 640x480 panel) with the
  # union toolchain (shauninman/union-miyoomini-toolchain: GCC 8.3, glibc 2.28, the device's SDL 1.2
  # in the sysroot). armv7ve: the Cortex-A7 has hardware SDIV/UDIV. HANDHELD selects the shared
  # platform layer (SDL 1.2 backend, NEON, save states), MIYOO the Mini's own bits (keys, paths,
  # power, menu), rendering 320x240, doubled onto the panel (port/sdl/miyoo_video.c). As MK64's.
  MIYOO_TOOLCHAIN ?= /opt/miyoomini-toolchain
  MIYOO_CROSS     ?= $(MIYOO_TOOLCHAIN)/usr/bin/arm-linux-gnueabihf-
  MIYOO_SYSROOT   ?= $(MIYOO_TOOLCHAIN)/usr/arm-linux-gnueabihf/sysroot
  CC      := $(MIYOO_CROSS)gcc
  OBJCOPY := $(MIYOO_CROSS)objcopy
  STRIP   := $(MIYOO_CROSS)strip
  READELF := $(MIYOO_CROSS)readelf
  MARCH   := -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
  OPT     := -O2 -fno-math-errno -fomit-frame-pointer -ffp-contract=off
  # 64-bit file offsets: 32-bit glibc's readdir/stat otherwise fail (EOVERFLOW) on filesystems with
  # 64-bit inodes or directory offsets, and the ROM search sees an empty folder (MK64's Miyoo port).
  SDL_CFLAGS := -I$(MIYOO_SYSROOT)/usr/include -D_GNU_SOURCE=1 -D_REENTRANT -D_FILE_OFFSET_BITS=64
  # no SDL_ttf/SDL_image: the Miyoo menu and first-start screen draw with a built-in font
  SDL_LIBS   := -L$(MIYOO_SYSROOT)/usr/lib -lSDL
  PLAT_DEFS  := -DTARGET_HANDHELD=1 -DHANDHELD=1 -DMIYOO=1
  PLAT_SRC   := port/sdl/gfx_sdl12.c port/sdl/miyoo_video.c port/sdl/miyoo_menu.c port/sdl/miyoo_worker.c port/sdl/miyoo_watchdog.c port/sdl/miyoo_clock.c
else
  CC      := gcc
  OBJCOPY := objcopy
  STRIP   := strip
  READELF := readelf
  # 32-bit x86 so pointer and struct sizes match the device; SSE float (not
  # x87) and no contraction so float results match the ARM build.
  MARCH   := -m32 -fno-pie -malign-data=abi -msse2 -mfpmath=sse
  OPT     := -O2 -g -fno-math-errno -ffp-contract=off
  SDL_CFLAGS := $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2)
  SDL_LIBS   := $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2)
  PLAT_DEFS  := -DTARGET_HANDHELD=1 -DTARGET_PC=1
  PLAT_SRC   := port/sdl/gfx_sdl.c
endif

# `make PLAT=pc UBSAN=1` -> build/pc-ubsan/dkr reports shift counts that are
# negative or >= the width at run time. MIPS and x86 mask the count to 5 bits,
# ARM uses its low byte (x >> 32 is 0 on the device, x on the desktop), so the
# desktop can hide what the device gets wrong (the OoT port 33d8a76, the MK64 port
# 1a4bbf7). The PC build already has -g, so each report names file:line.
# Likewise a float converted to an int it doesn't fit: x86 gives INT_MIN, the
# device's vcvt saturates (float-cast-overflow; the MK64 port b2dff40).
ifeq ($(UBSAN),1)
  MARCH += -fsanitize=shift-exponent,float-cast-overflow
endif

# IDO treats plain char as unsigned; so does ARM. -fwrapv and
# -fno-strict-aliasing: the decomp relies on MIPS wraparound and type punning.
COMMON_FLAGS := $(MARCH) $(OPT) -std=gnu99 -funsigned-char -fno-strict-aliasing -fwrapv -fno-common \
                -D_LANGUAGE_C -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 -D_FINALROM -DNDEBUG -DF3DDKR_GBI \
                -DVERSION_$(VERSION) -D_MIPS_SZLONG=32 -DPORT_ASSETS_LUT_START=$(ASSETS_LUT_START) -DPORT_ASSETS_END=$(ASSETS_END) -DCIC_ID=6103 -DPORT=1 $(PLAT_DEFS)

DECOMP_INC := -Idecomp -Idecomp/include -Idecomp/include/libc -Idecomp/include/PR -Idecomp/include/sys \
              -Idecomp/src -Idecomp/libultra -Idecomp/libultra/src/gu -Idecomp/libultra/src/libc \
              -Idecomp/libultra/src/io -Idecomp/libultra/src/sc -Idecomp/libultra/src/audio \
              -Idecomp/libultra/src/os -Idecomp/src/hasm -Iport

# Game code sees only the decomp's own libc headers, as on the N64.
DECOMP_CFLAGS := $(COMMON_FLAGS) -nostdinc -isystem $(shell $(CC) -print-file-name=include) $(DECOMP_INC) \
                 -include port/port_decomp.h -w -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-sincosf
# Float constants: the game keeps C/IDO semantics (a double constant promotes
# the expression to double, as on the N64); the port's renderer and backends
# use single-precision constants (the MK64 kit's ARM speed-up) -- on BOTH
# builds, so the x86 proxy computes exactly what the device computes.
# Port code sees the host libc first, then the decomp headers.
PORT_CFLAGS   := $(COMMON_FLAGS) -fsingle-precision-constant -DMODERN_CC $(SDL_CFLAGS) -Iport -Iport/gfx -Iport/sdl -Iport/audio -Iport/ultra \
                 -iquote decomp/src -iquote decomp/include -idirafter decomp/include -idirafter decomp/include/PR \
                 -idirafter decomp/libultra/src/audio -Wall -Wno-unused-parameter -Wno-missing-braces

# Hardware-only translation units, replaced by port/ultra and port/sdl.
DECOMP_EXCLUDE := decomp/src/main.c decomp/src/gzip.c decomp/src/os_yield_data.c \
                  decomp/src/thread0_epc.c decomp/src/get_stack_pointer.c
DECOMP_C := $(filter-out $(DECOMP_EXCLUDE),$(wildcard decomp/src/*.c)) \
            $(wildcard decomp/src/hasm/*.c) \
            $(wildcard decomp/libultra/src/audio/*.c) $(wildcard decomp/libultra/src/audio/mips1/*.c) \
            $(wildcard decomp/libultra/src/gu/*.c)

PORT_C := $(wildcard port/*.c) $(wildcard port/ultra/*.c) $(wildcard port/gfx/*.c) $(wildcard port/audio/*.c) \
          port/sdl/main.c port/sdl/gfx_soft.c port/sdl/audio_sdl.c port/sdl/controller_sdl.c \
          port/sdl/assets_dkr.c port/sdl/savestate.c port/sdl/pmu.c port/sdl/handheld_screen.c port/sdl/diag.c port/sdl/priority.c \
          $(PLAT_SRC)

GEN_C := $(BUILD)/gen/trig_tables.c

# Port files that hold game state: they join the save-state region.
GAMESTATE_PORT_C := port/game_pool.c port/gzip_port.c

DECOMP_O := $(patsubst %.c,$(BUILD)/%.o,$(DECOMP_C))
PORT_O   := $(patsubst %.c,$(BUILD)/%.o,$(PORT_C))
GEN_O    := $(patsubst %.c,%.o,$(GEN_C))
ALL_O    := $(DECOMP_O) $(PORT_O) $(GEN_O)

LDFLAGS := $(MARCH) -no-pie -Wl,-Ttext-segment=0x10000000 -Wl,-T,port/gamestate.ld \
           -Wl,--defsym=__ASSETS_LUT_START=$(ASSETS_LUT_START) -Wl,--defsym=__ASSETS_LUT_END=$(ASSETS_LUT_END) \
           -Wl,--defsym=_ftext=__executable_start -Wl,--defsym=_fbss=__data_start
LIBS    := $(SDL_LIBS) -lz -lm -lpthread

all: $(ELF)

# A fresh random stamp on every link: save states record it and refuse any
# other build (their fixed addresses belong to one exact executable).
STAMP_C := $(BUILD)/gen/build_stamp.c
$(ELF): $(ALL_O) port/gamestate.ld
	@echo "LINK $@"
	@mkdir -p $(BUILD)/gen
	$(V)echo "const unsigned int port_build_stamp = 0x$$(od -An -N4 -tx4 /dev/urandom | tr -d ' ');" > $(STAMP_C)
	$(V)echo "const char port_build_rev[] = \"$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$$(git diff --quiet HEAD -- decomp port Makefile 2>/dev/null || echo +)\";" >> $(STAMP_C)
	$(V)$(CC) -c $(PORT_CFLAGS) -o $(STAMP_C:.c=.o) $(STAMP_C)
	$(V)$(CC) $(LDFLAGS) -o $@ $(ALL_O) $(STAMP_C:.c=.o) $(LIBS)

# Rebuild everything when the compile flags change (a flag added after a build
# once left stale objects that hid the libultra sinf bug on x86).
FLAGS_STAMP := $(BUILD)/flags.stamp
FLAGS_NOW := $(strip $(DECOMP_CFLAGS) | $(PORT_CFLAGS))
$(shell mkdir -p $(BUILD); [ -f $(FLAGS_STAMP) ] && [ "$$(cat $(FLAGS_STAMP))" = "$(FLAGS_NOW)" ] || echo "$(FLAGS_NOW)" > $(FLAGS_STAMP))

# Game objects: .data/.bss renamed into the save-state include-list (port/gamestate.ld).
TO_GAMESTATE = $(OBJCOPY) --rename-section .data=.game_data --rename-section .bss=.game_bss,alloc $@

$(BUILD)/decomp/%.o: decomp/%.c port/port_decomp.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	@echo "CC   $<"
	$(V)$(CC) -c $(DECOMP_CFLAGS) -MMD -MP -o $@ $<
	$(V)$(TO_GAMESTATE)

$(patsubst %.c,$(BUILD)/%.o,$(GAMESTATE_PORT_C)): $(BUILD)/port/%.o: port/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	@echo "CC   $< (game state)"
	$(V)$(CC) -c $(PORT_CFLAGS) -MMD -MP -o $@ $<
	$(V)$(TO_GAMESTATE)

$(BUILD)/port/%.o: port/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	@echo "CC   $<"
	$(V)$(CC) -c $(PORT_CFLAGS) -MMD -MP -o $@ $<

# miyoo_worker.c (the shared miyoo/kit, the Mini's second core) names its A/B switch after the port
$(BUILD)/port/sdl/miyoo_worker.o: PORT_CFLAGS += -U_GNU_SOURCE '-DMIYOO_WORKER_ENV="DKR_THREADS"'
$(BUILD)/port/sdl/miyoo_watchdog.o: PORT_CFLAGS += -U_GNU_SOURCE '-DMIYOO_WD_ENV="DKR_WATCHDOG"'
$(BUILD)/port/sdl/miyoo_clock.o: PORT_CFLAGS += '-DMIYOO_CLOCK_ENV="DKR_CPUCLOCK"'

$(BUILD)/gen/trig_tables.c: tools/gen_trig_tables.py
	@mkdir -p $(dir $@)
	$(V)$(PYTHON) $< > $@

$(BUILD)/gen/%.o: $(BUILD)/gen/%.c $(FLAGS_STAMP)
	$(V)$(CC) -c $(DECOMP_CFLAGS) -o $@ $<
	$(V)$(TO_GAMESTATE)

clean:
	rm -rf $(BUILD)

-include $(ALL_O:.o=.d)

.PHONY: all clean
decomp-objs: $(DECOMP_O) $(GEN_O)

# Sources compiled into the executable (tools/build_miyoo.sh provenance gate).
print-sources:
	@echo $(DECOMP_C) $(PORT_C) $(GEN_C)
.PHONY: print-sources decomp-objs
