# pm64bench: M1 renderer gate. `make` = desktop proxy (x86 -m32, strict float, headless);
# `make PLAT=miyoo` = ARM build (the Miyoo Mini toolchain at MIYOO_TOOLCHAIN, SDL 1.2), for qemu-arm runs.
PLAT     ?= pc
PM_INC   ?= ../refs/papermario/include
BUILD    := build/$(PLAT)$(if $(filter 1,$(UBSAN)),-ubsan)
SRCS     := src/bench.c kit/gfx_pc.c kit/gfx_cc.c kit/gfx_soft.c
DEFINES  := -DTARGET_HANDHELD=1 -DPM64=1 -DPM64_REPLAY=1 -DF3DEX_GBI_2=1 -D_LANGUAGE_C=1 -DPORT_NO_FPS=1 -DNON_MATCHING=1 -DAVOID_UB=1
WARN     := -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-unused-parameter \
            -Wno-missing-braces -Wno-sign-compare -Wno-unknown-pragmas
COMMON   := -std=gnu11 -fsigned-char -fno-strict-aliasing -fwrapv -fno-common $(DEFINES) -Ikit -I$(PM_INC) $(WARN)
ifeq ($(PLAT),miyoo)
  MIYOO_TOOLCHAIN ?= /opt/miyoomini-toolchain
  MM_SYSROOT := $(MIYOO_TOOLCHAIN)/usr/arm-linux-gnueabihf/sysroot
  CC       := $(MIYOO_TOOLCHAIN)/usr/bin/arm-linux-gnueabihf-gcc
  CFLAGS   := -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fsingle-precision-constant \
              -O3 -fno-math-errno -fomit-frame-pointer -ffp-contract=off $(COMMON) -DPM64_SDL12=1 \
              -I$(MM_SYSROOT)/usr/include/SDL -D_GNU_SOURCE=1 -D_REENTRANT
  LDFLAGS  := -no-pie -Wl,--gc-sections -Wl,-Ttext-segment=0x40000000 -s
  LIBS     := -L$(MM_SYSROOT)/usr/lib -lSDL -lSDL_ttf -lm -lpthread
else
  CC       := gcc
  # -fsingle-precision-constant as the device has it (B8, as mario-kart-64 977e5a2): without it x * 0.5 is worked in
  # double here and in float there, and the proxy is not the device. Game code (M2) must build with
  # -fno-single-precision-constant on BOTH, to keep the decomp's double constants as the N64 compiler did.
  CFLAGS   := -m32 -fno-pie -malign-data=abi -fsingle-precision-constant -O2 -fno-math-errno -msse2 -mfpmath=sse -ffp-contract=off $(COMMON)
  LDFLAGS  := -m32 -no-pie -Wl,-Ttext-segment=0x40000000
  LIBS     := -lm
  # make UBSAN=1 (desktop only, build/pc-ubsan): shift counts and float->int casts checked at run time -- the
  # MIPS/x86-vs-ARM differences (a shift by 32 or more; a cast out of int range, INT_MIN on x86, saturated on ARM)
  ifeq ($(UBSAN),1)
    CFLAGS  += -g -fsanitize=shift-exponent,float-cast-overflow
    LDFLAGS += -fsanitize=shift-exponent,float-cast-overflow
  endif
endif
LDFLAGS  += -Wl,--defsym=_ftext=__executable_start -Wl,--defsym=_fbss=__bss_start
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

# The header's build stamp (bench.c: "pm64bench M1 (<stamp>)"), written at each link: the time and the commit, so a
# log names its build. __DATE__ only changed when bench.c itself recompiled: round 16's device log carried round
# 15's date (2026-09-25, the lead).
STAMP = @printf 'const char pm64bench_stamp[] = "%s %s";\n' "$$(date -u +%Y-%m-%dT%H:%MZ)" \
            "$$(git describe --always --dirty 2>/dev/null || echo nogit)" > $(BUILD)/stamp.c && \
        $(CC) $(CFLAGS) -c -o $(BUILD)/stamp.o $(BUILD)/stamp.c
$(BUILD)/pm64bench: $(OBJS) ; $(STAMP) && $(CC) $(LDFLAGS) -o $@ $^ $(BUILD)/stamp.o $(LIBS)
# Unstripped twin for instruction profiles (tools/qprof.sh); never shipped.
$(BUILD)/pm64bench.sym: $(OBJS) ; $(STAMP) && $(CC) $(filter-out -s,$(LDFLAGS)) -o $@ $^ $(BUILD)/stamp.o $(LIBS)
$(BUILD)/%.o: %.c $(wildcard kit/*.h) ; @mkdir -p $(dir $@); $(CC) $(CFLAGS) -c -o $@ $<
clean: ; rm -rf build
.PHONY: clean
