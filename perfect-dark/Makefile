# Perfect Dark -> Miyoo Mini (armhf, SDL 1.2, software renderer).
#
#   tools/gen_headers.sh                 # once: asset ID headers from src/assets/<romid> (local, gitignored)
#   make -j$(nproc)                      # build/miyoo/pd.arm
#   qemu-arm -L $(SYSROOT) build/miyoo/pd.arm
#
# Game code + port layer: fgsfdsfgs/perfect_dark (MIT), 32-bit layout (no PLATFORM_64BIT).
# Renderer + platform kit: the GoldenEye Miyoo port (port/miyoo/gfx, port/miyoo/kit), in C.

ROMID   ?= ntsc-final
VERSION_ntsc-final := 2
VERSION_pal-final  := 4
VERSION_jpn-final  := 5
PAL_pal-final := 1

TOOLCHAIN ?= /opt/miyoomini-toolchain
SYSROOT   := $(TOOLCHAIN)/usr/arm-linux-gnueabihf/sysroot
CC        := $(TOOLCHAIN)/usr/bin/arm-linux-gnueabihf-gcc
MARCH     := -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
OPT       ?= -O2

BUILD_DIR := build/miyoo
# `make SAN="-fsanitize=bounds,object-size"` (into build/miyoo-san, run under qemu-arm): the crash sweep's build
# (an index past a fixed-size array; the toolchain's libubsan reports file:line).
ifneq ($(SAN),)
  MARCH     += $(SAN) -fno-omit-frame-pointer
  BUILD_DIR := build/miyoo-san
endif
ELF       := $(BUILD_DIR)/pd.arm

V ?= @

SDL_CFLAGS := -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL -D_REENTRANT
SDL_LIBS   := -L$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib -lSDL -lrt

PLAT_DEFS := -DTARGET_HANDHELD=1 -DHANDHELD=1 -DTARGET_MIYOO=1 -DMIYOO=1 -DPLATFORM_MIYOO=1 \
             -D_FILE_OFFSET_BITS=64

GAME_DEFS := -DMATCHING=0 -DPIRACYCHECKS=0 -DROM_SIZE=32 -DAVOID_UB=1 -D_LANGUAGE_C=1 \
             -DPAL=$(or $(PAL_$(ROMID)),0) -DVERSION=$(VERSION_$(ROMID))

GEN_DIR  := src/generated/$(ROMID)
INCLUDES := -Iport/miyoo -Iinclude -Iinclude/PR -Isrc/include -I$(GEN_DIR) -Isrc/lib/ultra/audio \
            -Iport/include -I$(BUILD_DIR)/include

CFLAGS := -std=gnu11 $(MARCH) $(OPT) -g -fno-inline-functions -fno-strict-aliasing -funsigned-char -fwrapv \
          -ffp-contract=off -fno-pie -include versioninfo.h $(GAME_DEFS) $(PLAT_DEFS) $(INCLUDES) \
          $(SDL_CFLAGS) -MMD -MP -Wall -Wno-address -Wno-int-in-bool-context -Wno-misleading-indentation \
          -Wno-missing-braces -Wno-multichar -Wno-tautological-compare -Wno-unused-but-set-variable \
          -Wno-unused-value -Wno-unused-variable -Wno-pointer-sign -Wno-format-truncation \
          -Wno-unused-function -Werror=implicit-function-declaration $(EXTRA_CFLAGS)

# The GoldenEye kit renderer: C, signed plain char, its own include shims first.
GFX_CFLAGS := -std=gnu11 $(MARCH) -O2 -g -fsigned-char -fno-strict-aliasing -fwrapv -fno-common \
              -ffp-contract=off -fno-pie -D_LANGUAGE_C -DHH_PORT_LAYER=1 -DPORT_GE_RAWCC=1 -DPORT_REAL_PTR_MIN=0x1000u \
              $(GAME_DEFS) $(PLAT_DEFS) -Iport/miyoo/gfx -Iport/miyoo/kit -Iport/miyoo \
              -Iinclude -Iinclude/PR -Isrc/include -Iport/include -I$(BUILD_DIR)/include \
              $(SDL_CFLAGS) -include versioninfo.h -MMD -MP -w $(EXTRA_CFLAGS)

#------------------------------------------------------------------------------
GAME_C := $(sort $(shell find src/game -name "*.c"))
LIB_C  := $(sort $(wildcard src/lib/mp3/*.c) $(wildcard src/lib/naudio/*.c) \
          $(wildcard src/lib/ultra/audio/*.c) $(wildcard src/lib/ultra/gu/*.c)) \
  src/lib/ultra/io/vimodentsclan1.c src/lib/ultra/io/vimodepallan1.c src/lib/ultra/io/vimodempallan1.c \
  src/lib/ultra/io/vitbl.c src/lib/ailist.c src/lib/anim.c src/lib/args.c src/lib/audiodma.c \
  src/lib/audiomgr.c src/lib/base.c src/lib/collision.c src/lib/debughud.c src/lib/dma.c \
  src/lib/fault.c src/lib/joy.c src/lib/lib_2f490_c.c src/lib/lib_2fc60.c src/lib/lib_3e3e0.c \
  src/lib/lib_04f60nb.c src/lib/lib_17ce0.c src/lib/lib_034d0.c src/lib/lib_39c80.c \
  src/lib/lib_15850.c src/lib/mema.c src/lib/memp.c src/lib/model.c src/lib/modelasm_c.c \
  src/lib/mp3.c src/lib/mtx_c.c src/lib/mtx.c src/lib/music.c src/lib/path.c src/lib/profile.c \
  src/lib/rdp.c src/lib/rmon.c src/lib/rng_c.c src/lib/rzip_c.c src/lib/snd.c src/lib/speaker.c \
  src/lib/varsinit.c src/lib/vi.c src/textureconfig.c
# The PC port's platform layer, minus its SDL2/OpenGL video, input and audio (port/miyoo/ has those).
PORT_C := $(filter-out port/src/video.c port/src/input.c port/src/audio.c, \
            $(sort $(shell find port/src -name "*.c")))
MIYOO_C := $(sort $(wildcard port/miyoo/*.c))
GFX_C   := port/miyoo/gfx/gfx_pc.c port/miyoo/gfx/gfx_cc.c port/miyoo/gfx/gfx_soft.c port/miyoo/gfx/gfx_sdl12.c
KIT_C   := port/miyoo/kit/miyoo_worker.c port/miyoo/kit/miyoo_clock.c port/miyoo/kit/miyoo_sleepsave.c port/miyoo/kit/miyoo_menu.c port/miyoo/kit/miyoo_remap.c port/miyoo/kit/savestate.c port/miyoo/kit/hh_priority.c

C_FILES := $(GAME_C) $(LIB_C) $(PORT_C) $(MIYOO_C) $(GFX_C) $(KIT_C)
O_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_FILES))
DEP_FILES := $(O_FILES:.o=.d)

VERSIONINFO := $(BUILD_DIR)/include/versioninfo.h

all: $(ELF)

$(VERSIONINFO): port/include/versioninfo.h.in Makefile
	@mkdir -p $(dir $@)
	$(V)sed -e 's|@VERSION_HASH@|$(shell git rev-parse --short HEAD 2>/dev/null || echo none)|' \
	    -e 's|@VERSION_BRANCH@|miyoo|' -e 's|@VERSION_ROMID@|$(ROMID)|' \
	    -e 's|@VERSION_TARGET@|armhf-miyoo|' -e 's|@VERSION_ARCH@|armhf|' \
	    -e 's|@VERSION_BUILD@|Release|' $< > $@

$(GEN_DIR)/textures.h:
	@echo "missing $(GEN_DIR): run tools/gen_headers.sh (needs src/assets/$(ROMID))"; false

$(O_FILES): | $(VERSIONINFO) $(GEN_DIR)/textures.h

# Per-link build stamp: a save state loads only into the exact binary that wrote it (kit savestate.c).
STAMP_C := $(BUILD_DIR)/build_stamp.c
$(STAMP_C): $(O_FILES)
	$(V)printf '#include <stdint.h>\nconst uint32_t port_build_stamp = 0x%s;\n' "$$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')" > $@
$(STAMP_C:.c=.o): $(STAMP_C)
	$(V)$(CC) -c $(GFX_CFLAGS) -o $@ $<

$(ELF): $(O_FILES) $(STAMP_C:.c=.o)
	@echo "LD  $@"
	$(V)$(CC) $(MARCH) -no-pie -Wl,-Map,$(BUILD_DIR)/pd.map -Wl,--defsym=_ftext=0 -Wl,--defsym=_fbss=0 -Wl,--defsym=__assets_start=0 -Wl,--defsym=__assets_end=0 -o $@ $(O_FILES) $(STAMP_C:.c=.o) $(SDL_LIBS) -lz -lm -lpthread -ldl

$(BUILD_DIR)/port/miyoo/gfx/%.o: port/miyoo/gfx/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	$(V)$(CC) $(GFX_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/port/miyoo/kit/%.o: port/miyoo/kit/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	$(V)$(CC) $(GFX_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	$(V)$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP_FILES)
.PHONY: all clean
