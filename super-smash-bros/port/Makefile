# Super Smash Bros. (US) for the Miyoo Mini.
#   make            desktop proxy: 32-bit x86 (-m32), SDL 1.2, software renderer
#   make PLAT=miyoo    Miyoo Mini cross build (SDL 1.2, glibc, 320x240 doubled; notes/miyoo.md)
# Game code: port/decomp = JRickey/ssb-decomp-re @ 2898d4d9 (BattleShip's
# port fork of VetriTheRetri/ssb-decomp-re), code only: no src/relocData,
# no ovl8/db debug code. ROM data never enters the build (gate 1).
PLAT     ?= desktop
BUILD    := build/$(PLAT)
# BENCH=6,1,3: a dedicated benchmark build (M3) that runs the stage chain on
# every launch, whatever arguments the launcher passes. Its build dir is set
# before INCS reads it (a clean bench build used to look for the generated
# credits in the plain build's dir).
ifneq ($(BENCH),)
  BUILD := build/$(PLAT)-bench
endif
# UBSAN=1 (desktop): report shift counts that are negative or >= the width at
# run time. MIPS and x86 use the count's low 5 bits, ARM its low byte (x >> 32
# is x on the desktop, 0 on the device), so the desktop hides what the device
# gets wrong (ocarina-of-time 33d8a76/v15; mario-kart-64 1a4bbf7). Also a float
# converted to an int it doesn't fit: x86 gives INT_MIN, ARM saturates
# (float-cast-overflow; mario-kart-64 b2dff40). Own build dir.
ifeq ($(UBSAN),1)
  BUILD := $(BUILD)-ubsan
endif
TARGET   := $(BUILD)/ssb64

ifeq ($(PLAT),miyoo)
  # Miyoo Mini family (SigmaStar SSD202D: two Cortex-A7s, NEON-VFPv4, a 640x480 panel) with the union
  # toolchain (shauninman/union-miyoomini-toolchain: gcc 8.3, glibc 2.28, the device's SDL 1.2 in the
  # sysroot), as the MK64, DKR, OoT and GoldenEye Miyoo ports. Cortex-A7 CPU flags plus the
  # hard-float ABI. HANDHELD stays defined (the soft renderer, SDL 1.2, the kit's save states);
  # MIYOO covers the differences: the panel, keys, paths, power, the menu, the second core, the clock.
  MIYOO_TOOLCHAIN ?= /opt/miyoomini-toolchain
  MM_SYSROOT := $(MIYOO_TOOLCHAIN)/usr/arm-linux-gnueabihf/sysroot
  CROSS  := $(MIYOO_TOOLCHAIN)/usr/bin/arm-linux-gnueabihf-
  ARCH   := -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-PIC
  OPT    ?= -O2
  # 64-bit file offsets: 32-bit glibc's readdir/stat otherwise fail (EOVERFLOW) on 64-bit inodes and
  # directory offsets, and the ROM search saw an empty folder (the MK64 Miyoo port).
  SDL_CFLAGS := -I$(MM_SYSROOT)/usr/include -I$(MM_SYSROOT)/usr/include/SDL -D_REENTRANT -D_FILE_OFFSET_BITS=64
  # no SDL_ttf/SDL_image: the Miyoo menu and first-start screen draw with the kit's 8x8 font
  SDL_LIBS   := -L$(MM_SYSROOT)/usr/lib -Wl,-rpath-link,$(MM_SYSROOT)/usr/lib -lSDL -lpthread -lrt
  PLATDEF := -DTARGET_HANDHELD=1 -DHANDHELD=1 -DTARGET_MIYOO=1 -DMIYOO=1
  WM_SRC := hh/gfx/gfx_sdl12.c
else
  # 32-bit x86 so pointer/size_t widths and struct layouts match the device;
  # SSE (not x87) and no FMA contraction so floats match the device's VFP.
  CROSS  :=
  ARCH   := -m32 -fno-pie -malign-data=abi -msse2 -mfpmath=sse
  OPT    ?= -O1 -g
  SDL_CFLAGS := $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2)
  SDL_LIBS   := $(shell PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2)
  PLATDEF := -DTARGET_HANDHELD=1 -DTARGET_PC=1
  WM_SRC := hh/gfx/gfx_sdl.c
endif
ifeq ($(UBSAN),1)
  ARCH += -fsanitize=shift-exponent,float-cast-overflow
endif
CC := $(CROSS)gcc

DEFS := -DPORT=1 -DNON_MATCHING=1 -DNON_EQUIVALENT=1 -DAVOID_UB=1 \
        -DREGION_US=1 -DVERSION_US=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C -DN_MICRO=1 \
        -DHANDHELD_PORT=1
INCS := -I$(BUILD)/gen -Ihh/include -Idecomp/include -Idecomp/src -Ihh -Ihh/gfx
WARN := -Werror=implicit-function-declaration -Werror=int-conversion \
        -Werror=incompatible-pointer-types -Werror=return-type \
        -Wno-implicit-int -Wno-pointer-sign -Wno-unused-value -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-unused-function -Wno-shift-negative-value \
        -Wno-multichar -Wno-builtin-declaration-mismatch
BASEFLAGS := $(ARCH) $(OPT) -std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common \
             -fno-math-errno -ffp-contract=off -fno-builtin-sinf -fno-builtin-cosf $(DEFS) $(PLATDEF) $(WARN)
# Game code and the BattleShip-derived stubs see the decomp's headers first
# (it ships its own libc/ultra64 headers). Our own layer and the renderer see
# the system headers first and the decomp's only after them.
ifneq ($(BENCH),)
  BASEFLAGS += -DHH_BENCH_DEFAULT='"$(BENCH)"'
endif
CFLAGS := $(BASEFLAGS) $(INCS)
PORT_CFLAGS := $(BASEFLAGS) -I$(BUILD)/gen -Ihh -Ikit/platform -Ihh/gfx -Ihh/include -idirafter decomp/include -idirafter decomp/src $(SDL_CFLAGS)

GAME_DIRS := credits ef ft gm gr if it lb mn mp mv sc sys wp
# sorted: find lists files in the filesystem's order, which differs between
# machines and changes the link order (and every address) of the same code
GAME_SRC  := $(filter-out %.inc.c,$(foreach d,$(GAME_DIRS),$(sort $(shell find decomp/src/$(d) -name '*.c'))))
ULTRA_SRC := $(addprefix decomp/src/libultra/,gu/mtxcatf.c gu/mtxutil.c gu/mtxxfmf.c gu/normalize.c \
             gu/sinf.c gu/cosf.c audio/cents2ratio.c \
             $(addprefix n_audio/,n_env.c n_synaddplayer.c n_synallocfx.c n_synallocvoice.c \
             n_synfreevoice.c n_syndelete.c n_synsetfxmix.c n_synsetpan.c n_synsetpitch.c \
             n_synsetpriority.c n_synsetvol.c n_synstartvoiceparam.c n_synstopvoice.c n_cspplay.c \
             n_cspsetbank.c n_cspsetfxmix.c n_cspsetpriority.c n_cspsetseq.c n_cspsetvol.c \
             n_cspstop.c n_seq.c n_seqplayer.c n_seqpgetchlvol.c n_seqpgetvol.c))
# the shared kit (port/kit, vendored from ocarina-of-time's k1.7 copy): the menu and save states.
# hh/priority.c stays (the kit's hh_priority.c is the same k1.6 code).
KIT_SRC   := kit/platform/savestate.c
# the Miyoo Mini's own (the shared Miyoo kit KM1-KM3): the panel, the menu, the second core, the clock
ifeq ($(PLAT),miyoo)
  KIT_SRC += kit/platform/miyoo_panel.c kit/platform/miyoo_menu.c kit/platform/miyoo_remap.c kit/platform/miyoo_worker.c kit/platform/miyoo_clock.c kit/platform/miyoo_sleepsave.c
endif
PORT_SRC  := $(wildcard hh/*.c hh/bs/*.c hh/n64/*.c) hh/gfx/gfx_pc.c hh/gfx/gfx_cc.c hh/gfx/gfx_soft.c $(WM_SRC) $(KIT_SRC)
SRC       := $(GAME_SRC) $(ULTRA_SRC) $(PORT_SRC)
OBJ       := $(SRC:%.c=$(BUILD)/%.o)

# SGI trig must evaluate exactly as on the N64 (no FMA contraction).
$(BUILD)/decomp/src/libultra/gu/sinf.o $(BUILD)/decomp/src/libultra/gu/cosf.o: CFLAGS += -ffp-contract=off

# Real pointers stay above the N64 segment window (0x01000000..0x0F000000),
# which the renderer tells apart from segmented addresses. There is no asset
# region: all game data comes from the ROM slice at run time.
LDFLAGS := -no-pie -Wl,-Ttext-segment=0x40000000 \
           -Wl,--defsym=_ftext=__executable_start -Wl,--defsym=_fbss=__bss_start \
           -Wl,--defsym=__assets_start=__bss_start -Wl,--defsym=__assets_end=__bss_start \
           -Wl,-T,gamestate.ld

all: $(TARGET)

# A fresh random stamp on every link (DKR's): save states record it and refuse any other
# build, since their fixed addresses belong to one exact executable.
STAMP_C := $(BUILD)/stamp/build_stamp.c
$(TARGET): $(OBJ) gamestate.ld
	@mkdir -p $(BUILD)/stamp
	@echo "const unsigned int port_build_stamp = 0x$$(od -An -N4 -tx4 /dev/urandom | tr -d ' ');" > $(STAMP_C)
	@$(CC) $(PORT_CFLAGS) -c $(STAMP_C) -o $(STAMP_C:.c=.o)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(OBJ) $(STAMP_C:.c=.o) $(SDL_LIBS) -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/hh/%.o: hh/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(PORT_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kit/%.o: kit/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(PORT_CFLAGS) -MMD -MP -c $< -o $@

# BattleShip-derived stubs and our libultra layer (hh/n64) are written against
# the decomp headers.
$(BUILD)/hh/n64/%.o: hh/n64/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/hh/bs/%.o: hh/bs/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# the kit's Miyoo switches carry the port's prefix: SSB64_THREADS=1 (one core), SSB64_CPUCLOCK=<MHz>
$(BUILD)/kit/platform/miyoo_worker.o: PORT_CFLAGS += '-DMIYOO_WORKER_ENV="SSB64_THREADS"'
$(BUILD)/kit/platform/miyoo_clock.o: PORT_CFLAGS += '-DMIYOO_CLOCK_ENV="SSB64_CPUCLOCK"'

objs: $(OBJ)

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
.PHONY: all objs clean

# Credits text -> encoded tables (generated from decomp source text, never ROM).
CREDITS := staff titles info companies
CREDITS_OUT := $(foreach c,$(CREDITS),$(BUILD)/gen/credits/$(c).credits.encoded)
$(BUILD)/decomp/src/sc/sccommon/scstaffroll.o: $(CREDITS_OUT)
$(BUILD)/gen/credits/%.credits.encoded: decomp/src/credits/%.credits.us.txt tools/creditsTextConverter.py
	@mkdir -p $(dir $@)
	cd $(dir $@) && cp $(abspath $<) $*.credits.us.txt && python3 $(abspath tools/creditsTextConverter.py) $(CREDITS_FLAGS_$*) $*.credits.us.txt
CREDITS_FLAGS_info := -paragraphFont -multiline
CREDITS_FLAGS_companies := -paragraphFont

# Release gate 1 (tools/check_provenance.sh): every source compiled into the
# executable, one per line.
print-sources:
	@for f in $(SRC); do echo $$f; done
.PHONY: print-sources
