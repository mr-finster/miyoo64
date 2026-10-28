# Makefile to rebuild SM64 split image

include util.mk

# Default target
default: all

# Preprocessor definitions
DEFINES :=

#==============================================================================#
# Build Options                                                                #
#==============================================================================#

# These options can either be set by building with 'make SETTING=value'.
# 'make clean' may be required first.

# Build for the N64 (turn this off for ports)
TARGET_N64 ?= 0
# Build for Emscripten/WebGL
TARGET_WEB ?= 0
# Handheld port (src/hh, the Mario Kart 64 port's kit): PLAT=pc builds the -m32 SDL2 desktop
# proxy, PLAT=miyoo the Miyoo Mini family's binary (union toolchain, SDL 1.2; HANDHELD and MIYOO).
PLAT ?=
ifneq ($(PLAT),)
  $(eval $(call validate-option,PLAT,pc miyoo))
  TARGET_N64 := 0
  TARGET_HANDHELD := 1
endif
# Compiler to use (ido or gcc)


# COMPILER - selects the C compiler to use
#   ido - uses the SGI IRIS Development Option compiler, which is used to build
#         an original matching N64 ROM
#   gcc - uses the GNU C Compiler
COMPILER ?= ido
$(eval $(call validate-option,COMPILER,ido gcc))

# Automatic settings only for ports
ifeq ($(TARGET_N64),0)

  COMPILER := gcc
  NON_MATCHING := 1
ifneq ($(TARGET_HANDHELD),1)
  GRUCODE := f3dex2e
  TARGET_WINDOWS := 0
  ifeq ($(TARGET_WEB),0)
    ifeq ($(OS),Windows_NT)
      TARGET_WINDOWS := 1
    else
      # TODO: Detect Mac OS X, BSD, etc. For now, assume Linux
      TARGET_LINUX := 1
    endif
  endif

  ifeq ($(TARGET_WINDOWS),1)
    # On Windows, default to DirectX 11
    ifneq ($(ENABLE_OPENGL),1)
      ifneq ($(ENABLE_DX12),1)
        ENABLE_DX11 ?= 1
      endif
    endif
  else
    # On others, default to OpenGL
    ENABLE_OPENGL ?= 1
  endif

  # Sanity checks
  ifeq ($(ENABLE_DX11),1)
    ifneq ($(TARGET_WINDOWS),1)
      $(error The DirectX 11 backend is only supported on Windows)
    endif
    ifeq ($(ENABLE_OPENGL),1)
      $(error Cannot specify multiple graphics backends)
    endif
    ifeq ($(ENABLE_DX12),1)
      $(error Cannot specify multiple graphics backends)
    endif
  endif
  ifeq ($(ENABLE_DX12),1)
    ifneq ($(TARGET_WINDOWS),1)
      $(error The DirectX 12 backend is only supported on Windows)
    endif
    ifeq ($(ENABLE_OPENGL),1)
      $(error Cannot specify multiple graphics backends)
    endif
    ifeq ($(ENABLE_DX11),1)
      $(error Cannot specify multiple graphics backends)
    endif
  endif
endif # !TARGET_HANDHELD

endif

ifeq ($(COMPILER),gcc)
  NON_MATCHING := 1
endif

# VERSION - selects the version of the game to build
#   jp - builds the 1996 Japanese version
#   us - builds the 1996 North American version
#   eu - builds the 1997 PAL version
#   sh - builds the 1997 Japanese Shindou version, with rumble pak support
VERSION ?= us
$(eval $(call validate-option,VERSION,jp us eu sh))

ifeq      ($(VERSION),jp)
  DEFINES   += VERSION_JP=1
  OPT_FLAGS := -g
  GRUCODE   ?= f3d_old
  VERSION_JP_US  ?= true
else ifeq ($(VERSION),us)
  DEFINES   += VERSION_US=1
  OPT_FLAGS := -g
  GRUCODE   ?= f3d_old
  VERSION_JP_US  ?= true
else ifeq ($(VERSION),eu)
  DEFINES   += VERSION_EU=1
  OPT_FLAGS := -O2
  GRUCODE   ?= f3d_new
  VERSION_JP_US  ?= false
else ifeq ($(VERSION),sh)
  DEFINES   += VERSION_SH=1
  OPT_FLAGS := -O2
  GRUCODE   ?= f3d_new
  VERSION_JP_US  ?= false
endif

TARGET := sm64.$(VERSION)


# GRUCODE - selects which RSP microcode to use.
#   f3d_old - default for JP and US versions
#   f3d_new - default for EU and Shindou versions
#   f3dex   -
#   f3dex2  -
#   f3dzex  - newer, experimental microcode used in Animal Crossing
$(eval $(call validate-option,GRUCODE,f3d_old f3dex f3dex2 f3dex2e f3d_new f3dzex))

ifeq      ($(GRUCODE),f3d_old)
  DEFINES += F3D_OLD=1
else ifeq ($(GRUCODE),f3d_new) # Fast3D 2.0H
  DEFINES += F3D_NEW=1
else ifeq ($(GRUCODE),f3dex) # Fast3DEX
  DEFINES += F3DEX_GBI=1 F3DEX_GBI_SHARED=1
else ifeq ($(GRUCODE), f3dex2) # Fast3DEX2
  DEFINES += F3DEX_GBI_2=1 F3DEX_GBI_SHARED=1
else ifeq ($(GRUCODE), f3dex2e) # Fast3DEX2 Extended (for PC)
  DEFINES += F3DEX_GBI_2E=1
else ifeq ($(GRUCODE),f3dzex) # Fast3DZEX (2.0J / Animal Forest - Dōbutsu no Mori)
  $(warning Fast3DZEX is experimental. Try at your own risk.)
  DEFINES += F3DZEX_GBI_2=1 F3DEX_GBI_2=1 F3DEX_GBI_SHARED=1
endif


# USE_QEMU_IRIX - when ido is selected, select which way to emulate IRIX programs
#   1 - use qemu-irix
#   0 - statically recompile the IRIX programs
USE_QEMU_IRIX ?= 0
$(eval $(call validate-option,USE_QEMU_IRIX,0 1))

ifeq      ($(COMPILER),ido)
  ifeq ($(USE_QEMU_IRIX),1)
    # Verify that qemu-irix exists
    QEMU_IRIX ?= $(call find-command,qemu-irix)
    ifeq (, $(QEMU_IRIX))
      $(error Using the IDO compiler requires qemu-irix. Please install qemu-irix package or set the QEMU_IRIX environment variable to the full qemu-irix binary path)
    endif
  endif

  MIPSISET := -mips2
else ifeq ($(COMPILER),gcc)
  NON_MATCHING := 1
  MIPSISET     := -mips3
  OPT_FLAGS    := -O2
endif

# OPT_FLAGS - for ports
ifeq ($(TARGET_N64),0)
  OPT_FLAGS := -O2
  ifeq ($(TARGET_WEB),1)
    OPT_FLAGS += -g4 --source-map-base http://localhost:8080/
  endif
endif


# NON_MATCHING - whether to build a matching, identical copy of the ROM
#   1 - enable some alternate, more portable code that does not produce a matching ROM
#   0 - build a matching ROM
NON_MATCHING ?= 0
$(eval $(call validate-option,NON_MATCHING,0 1))

ifeq ($(NON_MATCHING),1)
  DEFINES += NON_MATCHING=1 AVOID_UB=1
  COMPARE := 0
endif


# COMPARE - whether to verify the SHA-1 hash of the ROM after building
#   1 - verifies the SHA-1 hash of the selected version of the game
#   0 - does not verify the hash
COMPARE ?= 1
$(eval $(call validate-option,COMPARE,0 1))

TARGET_STRING := sm64.$(VERSION).$(GRUCODE)
# If non-default settings were chosen, disable COMPARE
ifeq ($(filter $(TARGET_STRING), sm64.jp.f3d_old sm64.us.f3d_old sm64.eu.f3d_new sm64.sh.f3d_new),)
  COMPARE := 0
endif

# Whether to hide commands or not
VERBOSE ?= 0
ifeq ($(VERBOSE),0)
  V := @
endif

# Whether to colorize build messages
COLOR ?= 1

# display selected options unless 'make clean' or 'make distclean' is run
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
  $(info ==== Build Options ====)
  $(info Version:        $(VERSION))
  $(info Microcode:      $(GRUCODE))
  $(info Target:         $(TARGET))
  ifeq ($(COMPARE),1)
    $(info Compare ROM:    yes)
  else
    $(info Compare ROM:    no)
  endif
  ifeq ($(NON_MATCHING),1)
    $(info Build Matching: no)
  else
    $(info Build Matching: yes)
  endif
  $(info =======================)
endif


#==============================================================================#
# Universal Dependencies                                                       #
#==============================================================================#

TOOLS_DIR := tools

# (This is a bit hacky, but a lot of rules implicitly depend
# on tools and assets, and we use directory globs further down
# in the makefile that we want should cover assets.)

PYTHON := python3

ifeq ($(filter clean distclean print-%,$(MAKECMDGOALS)),)

  # Make sure assets exist
  NOEXTRACT ?= 0
  ifeq ($(NOEXTRACT),0)
    DUMMY != $(PYTHON) extract_assets.py $(VERSION) >&2 || echo FAIL
    ifeq ($(DUMMY),FAIL)
      $(error Failed to extract assets)
    endif
  endif

  # Make tools if out of date
  $(info Building tools...)
  DUMMY != $(MAKE) -s -C $(TOOLS_DIR) $(if $(filter-out ido0,$(COMPILER)$(USE_QEMU_IRIX)),all-except-recomp,) >&2 || echo FAIL
    ifeq ($(DUMMY),FAIL)
      $(error Failed to build tools)
    endif
  $(info Building ROM...)

endif


#==============================================================================#
# Target Executable and Sources                                                #
#==============================================================================#

BUILD_DIR_BASE := build
# BUILD_DIR is the location where all build artifacts are placed
ifeq ($(TARGET_N64),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)
else ifeq ($(TARGET_HANDHELD),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_$(PLAT)$(if $(filter 1,$(UBSAN)),_ubsan)$(if $(SAN),_san)
else ifeq ($(TARGET_WEB),1)
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_web
else
  BUILD_DIR := $(BUILD_DIR_BASE)/$(VERSION)_pc
endif

ifeq ($(TARGET_HANDHELD),1)
  EXE := $(BUILD_DIR)/sm64
else ifeq ($(TARGET_WEB),1)
  EXE := $(BUILD_DIR)/$(TARGET).html
else ifeq ($(TARGET_WINDOWS),1)
  EXE := $(BUILD_DIR)/$(TARGET).exe
else
  EXE := $(BUILD_DIR)/$(TARGET)
endif
ROM            := $(BUILD_DIR)/$(TARGET).z64
ELF            := $(BUILD_DIR)/$(TARGET).elf
LIBULTRA       := $(BUILD_DIR)/libultra.a
LD_SCRIPT      := sm64.ld
MIO0_DIR       := $(BUILD_DIR)/bin
SOUND_BIN_DIR  := $(BUILD_DIR)/sound
TEXTURE_DIR    := textures
ACTOR_DIR      := actors
LEVEL_DIRS     := $(patsubst levels/%,%,$(dir $(wildcard levels/*/header.h)))

# Directories containing source files
SRC_DIRS := src src/engine src/game src/audio src/menu src/buffers actors levels bin bin/$(VERSION) data assets sound
ifeq ($(TARGET_N64),1)
  SRC_DIRS += asm lib
else ifeq ($(TARGET_HANDHELD),1)
  # the menu and save states come from the shared kit (port/kit, vendored at
  # tag k1: never edited here; changes go to the kit)
  SRC_DIRS += src/hh src/hh/gfx kit/platform
else
  SRC_DIRS += src/pc src/pc/gfx src/pc/audio src/pc/controller
endif
BIN_DIRS := bin bin/$(VERSION)

ULTRA_SRC_DIRS := lib/src lib/src/math lib/asm lib/data
ULTRA_BIN_DIRS := lib/bin

GODDARD_SRC_DIRS := src/goddard src/goddard/dynlists

# File dependencies and variables for specific files
include Makefile.split

# Source code files
LEVEL_C_FILES     := $(wildcard levels/*/leveldata.c) $(wildcard levels/*/script.c) $(wildcard levels/*/geo.c)
C_FILES           := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c)) $(LEVEL_C_FILES)
CXX_FILES         := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.cpp))
S_FILES           := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.s))
ULTRA_C_FILES     := $(foreach dir,$(ULTRA_SRC_DIRS),$(wildcard $(dir)/*.c))
GODDARD_C_FILES   := $(foreach dir,$(GODDARD_SRC_DIRS),$(wildcard $(dir)/*.c))
ifeq ($(TARGET_N64),1)
  ULTRA_S_FILES   := $(foreach dir,$(ULTRA_SRC_DIRS),$(wildcard $(dir)/*.s))
endif
GENERATED_C_FILES := $(BUILD_DIR)/assets/mario_anim_data.c $(BUILD_DIR)/assets/demo_data.c \
  $(addprefix $(BUILD_DIR)/bin/,$(addsuffix _skybox.c,$(notdir $(basename $(wildcard textures/skyboxes/*.png)))))

ifneq ($(TARGET_N64),1)
  ULTRA_C_FILES := \
    alBnkfNew.c \
    guLookAtRef.c \
    guMtxF2L.c \
    guNormalize.c \
    guOrthoF.c \
    guPerspectiveF.c \
    guRotateF.c \
    guScaleF.c \
    guTranslateF.c
  ifeq ($(TARGET_HANDHELD),1)
    # libultra's own sinf and cosf (and the NaN they return), the N64's: the handheld SDK's musl
    # 1.2.1 (the device's libc) returns garbage from |x| ~ 7 up (goldeneye-007 round 12), and
    # the paintings' ripple (paintings.c) passes cosf thousands of radians. Every sinf/cosf in
    # the game now links to these on both targets, so the proxy still equals the device.
    ULTRA_C_FILES += NaN.c math/sinf.c math/cosf.c
  endif

  C_FILES := $(filter-out src/game/main.c,$(C_FILES))
  ifneq ($(PLAT),miyoo)
    # the Miyoo Mini's platform files (kit/platform/miyoo_*.c): the Miyoo build's only
    C_FILES := $(filter-out kit/platform/miyoo_%.c,$(C_FILES))
  endif
  ULTRA_C_FILES := $(addprefix lib/src/,$(ULTRA_C_FILES))
  ifeq ($(TARGET_HANDHELD),1)
    # from sm64-port's own layer: the libultra shims and the audio microcode in C
    C_FILES += src/pc/ultra_reimplementation.c src/pc/mixer.c
  endif
endif

# Sound files
SOUND_BANK_FILES    := $(wildcard sound/sound_banks/*.json)
SOUND_SAMPLE_DIRS   := $(wildcard sound/samples/*)
SOUND_SAMPLE_AIFFS  := $(foreach dir,$(SOUND_SAMPLE_DIRS),$(wildcard $(dir)/*.aiff))
SOUND_SAMPLE_TABLES := $(foreach file,$(SOUND_SAMPLE_AIFFS),$(BUILD_DIR)/$(file:.aiff=.table))
SOUND_SAMPLE_AIFCS  := $(foreach file,$(SOUND_SAMPLE_AIFFS),$(BUILD_DIR)/$(file:.aiff=.aifc))
SOUND_SEQUENCE_DIRS := sound/sequences sound/sequences/$(VERSION)
# all .m64 files in SOUND_SEQUENCE_DIRS, plus all .m64 files that are generated from .s files in SOUND_SEQUENCE_DIRS
SOUND_SEQUENCE_FILES := \
  $(foreach dir,$(SOUND_SEQUENCE_DIRS),\
    $(wildcard $(dir)/*.m64) \
    $(foreach file,$(wildcard $(dir)/*.s),$(BUILD_DIR)/$(file:.s=.m64)) \
  )

# Object files
O_FILES := $(foreach file,$(C_FILES),$(BUILD_DIR)/$(file:.c=.o)) \
           $(foreach file,$(CXX_FILES),$(BUILD_DIR)/$(file:.cpp=.o)) \
           $(foreach file,$(S_FILES),$(BUILD_DIR)/$(file:.s=.o)) \
           $(foreach file,$(GENERATED_C_FILES),$(file:.c=.o))

ULTRA_O_FILES := $(foreach file,$(ULTRA_S_FILES),$(BUILD_DIR)/$(file:.s=.o)) \
                 $(foreach file,$(ULTRA_C_FILES),$(BUILD_DIR)/$(file:.c=.o))

GODDARD_O_FILES := $(foreach file,$(GODDARD_C_FILES),$(BUILD_DIR)/$(file:.c=.o))

# Automatic dependency files
DEP_FILES := $(O_FILES:.o=.d) $(ULTRA_O_FILES:.o=.d) $(GODDARD_O_FILES:.o=.d) $(BUILD_DIR)/$(LD_SCRIPT).d

# Files with GLOBAL_ASM blocks
ifeq ($(NON_MATCHING),0)
  ifeq ($(VERSION),sh)
    GLOBAL_ASM_C_FILES != grep -rl 'GLOBAL_ASM(' $(wildcard src/**/*.c) $(wildcard lib/src/*.c)
  else
    GLOBAL_ASM_C_FILES != grep -rl 'GLOBAL_ASM(' $(wildcard src/**/*.c)
  endif
GLOBAL_ASM_O_FILES = $(foreach file,$(GLOBAL_ASM_C_FILES),$(BUILD_DIR)/$(file:.c=.o))
GLOBAL_ASM_DEP = $(BUILD_DIR)/src/audio/non_matching_dep
endif


#==============================================================================#
# Compiler Options                                                             #
#==============================================================================#

INCLUDE_DIRS := include $(BUILD_DIR) $(BUILD_DIR)/include src .
ifeq ($(TARGET_N64),1)
  INCLUDE_DIRS += include/libc
endif

C_DEFINES := $(foreach d,$(DEFINES),-D$(d))
DEF_INC_CFLAGS := $(foreach i,$(INCLUDE_DIRS),-I$(i)) $(C_DEFINES)

ifeq ($(TARGET_N64),1)

# detect prefix for MIPS toolchain
ifneq      ($(call find-command,mips-linux-gnu-ld),)
  CROSS := mips-linux-gnu-
else ifneq ($(call find-command,mips64-linux-gnu-ld),)
  CROSS := mips64-linux-gnu-
else ifneq ($(call find-command,mips64-elf-ld),)
  CROSS := mips64-elf-
else
  $(error Unable to detect a suitable MIPS toolchain installed)
endif

AS        := $(CROSS)as
ifeq ($(COMPILER),gcc)
  CC      := $(CROSS)gcc
else
  ifeq ($(USE_QEMU_IRIX),1)
    IRIX_ROOT := $(TOOLS_DIR)/ido5.3_compiler
    CC      := $(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/bin/cc
    ACPP    := $(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/lib/acpp
    COPT    := $(QEMU_IRIX) -silent -L $(IRIX_ROOT) $(IRIX_ROOT)/usr/lib/copt
  else
    IDO_ROOT := $(TOOLS_DIR)/ido5.3_recomp
    CC      := $(IDO_ROOT)/cc
    ACPP    := $(IDO_ROOT)/acpp
    COPT    := $(IDO_ROOT)/copt
  endif
endif
LD        := $(CROSS)ld
AR        := $(CROSS)ar
OBJDUMP   := $(CROSS)objdump
OBJCOPY   := $(CROSS)objcopy

TARGET_CFLAGS := -nostdinc -DTARGET_N64 -D_LANGUAGE_C
CC_CFLAGS := -fno-builtin

# Check code syntax with host compiler
CC_CHECK := gcc
CC_CHECK_CFLAGS := -fsyntax-only -fsigned-char $(CC_CFLAGS) $(TARGET_CFLAGS) -std=gnu90 -Wall -Wextra -Wno-format-security -Wno-main -DNON_MATCHING -DAVOID_UB $(DEF_INC_CFLAGS)

# C compiler options
CFLAGS = -G 0 $(OPT_FLAGS) $(TARGET_CFLAGS) $(MIPSISET) $(DEF_INC_CFLAGS)
ifeq ($(COMPILER),gcc)
  CFLAGS += -mno-shared -march=vr4300 -mfix4300 -mabi=32 -mhard-float -mdivide-breaks -fno-stack-protector -fno-common -fno-zero-initialized-in-bss -fno-PIC -mno-abicalls -fno-strict-aliasing -fno-inline-functions -ffreestanding -fwrapv -Wall -Wextra
else
  CFLAGS += -non_shared -Wab,-r4300_mul -Xcpluscomm -Xfullwarn -signed -32
endif

ASFLAGS     := -march=vr4300 -mabi=32 $(foreach i,$(INCLUDE_DIRS),-I$(i)) $(foreach d,$(DEFINES),--defsym $(d))
RSPASMFLAGS := $(foreach d,$(DEFINES),-definelabel $(subst =, ,$(d)))

ifeq ($(shell getconf LONG_BIT), 32)
  # Work around memory allocation bug in QEMU
  export QEMU_GUEST_BASE := 1
else
  # Ensure that gcc treats the code as 32-bit
  CC_CHECK_CFLAGS += -m32
endif

# Prevent a crash with -sopt
export LANG := C

else # TARGET_N64

AS := as
ifneq ($(TARGET_WEB),1)
  CC := gcc
  CXX := g++
else
  CC := emcc
endif
ifeq ($(CXX_FILES),"")
  LD := $(CC)
else
  LD := $(CXX)
endif
OBJDUMP := objdump
OBJCOPY := objcopy
PYTHON := python3

# Platform-specific compiler and linker flags
ifeq ($(TARGET_WINDOWS),1)
  PLATFORM_CFLAGS  := -DTARGET_WINDOWS
  PLATFORM_LDFLAGS := -lm -lxinput9_1_0 -lole32 -no-pie -mwindows
endif
ifeq ($(TARGET_LINUX),1)
  PLATFORM_CFLAGS  := -DTARGET_LINUX `pkg-config --cflags libusb-1.0`
  PLATFORM_LDFLAGS := -lm -lpthread `pkg-config --libs libusb-1.0` -lasound -lpulse -no-pie
endif
ifeq ($(TARGET_WEB),1)
  PLATFORM_CFLAGS  := -DTARGET_WEB
  PLATFORM_LDFLAGS := -lm -no-pie -s TOTAL_MEMORY=20MB -g4 --source-map-base http://localhost:8080/ -s "EXTRA_EXPORTED_RUNTIME_METHODS=['callMain']"
endif

PLATFORM_CFLAGS += -DNO_SEGMENTED_MEMORY -DUSE_SYSTEM_MALLOC

# Compiler and linker flags for graphics backend
ifeq ($(ENABLE_OPENGL),1)
  GFX_CFLAGS  := -DENABLE_OPENGL
  GFX_LDFLAGS :=
  ifeq ($(TARGET_WINDOWS),1)
    GFX_CFLAGS  += $(shell sdl2-config --cflags) -DGLEW_STATIC
    GFX_LDFLAGS += $(shell sdl2-config --libs) -lglew32 -lopengl32 -lwinmm -limm32 -lversion -loleaut32 -lsetupapi
  endif
  ifeq ($(TARGET_LINUX),1)
    GFX_CFLAGS  += $(shell sdl2-config --cflags)
    GFX_LDFLAGS += -lGL $(shell sdl2-config --libs) -lX11 -lXrandr
  endif
  ifeq ($(TARGET_WEB),1)
    GFX_CFLAGS  += -s USE_SDL=2
    GFX_LDFLAGS += -lGL -lSDL2
  endif
endif
ifeq ($(ENABLE_DX11),1)
  GFX_CFLAGS := -DENABLE_DX11
  PLATFORM_LDFLAGS += -lgdi32 -static
endif
ifeq ($(ENABLE_DX12),1)
  GFX_CFLAGS := -DENABLE_DX12
  PLATFORM_LDFLAGS += -lgdi32 -static
endif

GFX_CFLAGS += -DWIDESCREEN

CC_CHECK := $(CC) -fsyntax-only -fsigned-char -Wall -Wextra -Wno-format-security -D_LANGUAGE_C $(DEF_INC_CFLAGS) $(PLATFORM_CFLAGS) $(GFX_CFLAGS)
CFLAGS := $(OPT_FLAGS) -D_LANGUAGE_C $(DEF_INC_CFLAGS) $(PLATFORM_CFLAGS) $(GFX_CFLAGS) -fno-strict-aliasing -fwrapv -march=native

ASFLAGS := -I include -I $(BUILD_DIR) $(foreach d,$(DEFINES),--defsym $(d))

LDFLAGS := $(PLATFORM_LDFLAGS) $(GFX_LDFLAGS)

ifeq ($(TARGET_HANDHELD),1)
  # Same float and char semantics as the N64 build on both targets: IDO builds
  # with -signed; no -Ofast / -fsingle-precision-constant (they change the game's
  # physics); no fused multiply-add (the VR4300 has none, and the x86 proxy can't
  # fuse, so bit-exact sweeps on the proxy transfer to the device).
  # No -DUSE_SYSTEM_MALLOC: the game keeps its static N64 pool, so no heap pointer
  # ends up in game state (save states survive reboots). No -DWIDESCREEN.
  ifeq ($(PLAT),miyoo)
    # Miyoo Mini / Mini Plus / v4 / Flip (SigmaStar SSD202D: two Cortex-A7s, the handheld's core):
    # the union toolchain (shauninman/miyoomini-toolchain-buildroot v0.0.3: GCC 8.3, glibc 2.28,
    # the device's SDL 1.2 in the sysroot). The handheld's CPU flags plus the hard-float ABI; the same
    # float rules as the handheld (no fast-math, no contraction: the flags below). The game renders
    # its own 320x240, doubled onto the 640x480 panel (kit/platform/miyoo_panel.c).
    MIYOO_TOOLCHAIN ?= /opt/miyoomini-toolchain
    MIYOO_SYSROOT   ?= $(MIYOO_TOOLCHAIN)/usr/arm-linux-gnueabihf/sysroot
    HH_BIN  := $(MIYOO_TOOLCHAIN)/usr/bin/arm-linux-gnueabihf-
    CC      := $(HH_BIN)gcc
    CXX     := $(HH_BIN)g++
    AS      := $(HH_BIN)as
    OBJDUMP := $(HH_BIN)objdump
    OBJCOPY := $(HH_BIN)objcopy
    HH_ARCH := -march=armv7ve+neon-vfpv4 -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
    # 64-bit file offsets: 32-bit glibc's readdir/stat otherwise fail (EOVERFLOW) on a card with
    # 64-bit inodes, and the ROM search saw an empty folder (the MK64 Miyoo port).
    HH_SDL_CFLAGS := -I$(MIYOO_SYSROOT)/usr/include/SDL -D_GNU_SOURCE=1 -D_REENTRANT -D_FILE_OFFSET_BITS=64
    # SDL only: the Miyoo menu and first-start screen draw with the kit's bitmap font
    HH_SDL_LIBS   := -L$(MIYOO_SYSROOT)/usr/lib -lSDL
    HH_DEFINES    := -DTARGET_HANDHELD -DHANDHELD -DMIYOO -DTARGET_MIYOO
    HH_OPT ?= -O3 -fomit-frame-pointer
    HH_LDDEVICE := -Wl,--gc-sections
  else
    CC      := gcc
    CXX     := g++
    AS      := as
    OBJCOPY := objcopy
    # -m32: pointer and struct widths as on the device; SSE math, not x87, so
    # frames are bit-comparable with ARM; -malign-data=abi: ARM's array alignment.
    HH_ARCH := -m32 -fno-pie -malign-data=abi -msse2 -mfpmath=sse
    HH_PKG  := PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig /usr/bin/pkg-config
    HH_SDL_CFLAGS := $(shell $(HH_PKG) --cflags sdl2)
    HH_SDL_LIBS   := $(shell $(HH_PKG) --libs sdl2)
    HH_DEFINES    := -DTARGET_HANDHELD -DTARGET_PC
  endif
  LD := $(CC)
  # TEX32=2: 16-bit textures of <= 4096 texels stored as 8888 (MK64 v32 default)
  PLATFORM_CFLAGS := $(HH_DEFINES) -DNO_SEGMENTED_MEMORY -DPORT_TEX32=2
  HH_OPT ?= -O2
  CFLAGS := $(HH_OPT) -g -MMD -MP -D_LANGUAGE_C $(DEF_INC_CFLAGS) -Isrc/hh -Isrc/hh/gfx -Ikit/platform $(PLATFORM_CFLAGS) $(HH_SDL_CFLAGS) \
            $(HH_ARCH) -fsigned-char -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off \
            -fno-builtin-sinf -fno-builtin-cosf
  # (no builtins: GCC would fold a sinf/cosf pair into libc's sincosf, the same broken code)
  CC_CHECK := true
  # Linked at 0x10000000 like MK64: the kit treats lower addresses as N64 segments.
  HH_LDEXTRA ?=
  LDFLAGS := $(HH_ARCH) $(HH_LDEXTRA) $(HH_LDDEVICE) -no-pie -Wl,-Ttext-segment=0x10000000 $(HH_SDL_LIBS) -lm -lpthread
endif

endif

# Prefer clang as C preprocessor if installed on the system
ifneq (,$(call find-command,clang))
  CPP      := clang
  CPPFLAGS := -E -P -x c -Wno-trigraphs $(DEF_INC_CFLAGS)
else
  CPP      := cpp
  CPPFLAGS := -P -Wno-trigraphs $(DEF_INC_CFLAGS)
endif

#==============================================================================#
# Miscellaneous Tools                                                          #
#==============================================================================#

# N64 tools
MIO0TOOL              := $(TOOLS_DIR)/mio0
N64CKSUM              := $(TOOLS_DIR)/n64cksum
N64GRAPHICS           := $(TOOLS_DIR)/n64graphics
N64GRAPHICS_CI        := $(TOOLS_DIR)/n64graphics_ci
TEXTCONV              := $(TOOLS_DIR)/textconv
AIFF_EXTRACT_CODEBOOK := $(TOOLS_DIR)/aiff_extract_codebook
VADPCM_ENC            := $(TOOLS_DIR)/vadpcm_enc
EXTRACT_DATA_FOR_MIO  := $(TOOLS_DIR)/extract_data_for_mio
SKYCONV               := $(TOOLS_DIR)/skyconv
# Use the system installed armips if available. Otherwise use the one provided with this repository.
ifneq (,$(call find-command,armips))
  RSPASM              := armips
else
  RSPASM              := $(TOOLS_DIR)/armips
endif
ENDIAN_BITWIDTH       := $(BUILD_DIR)/endian-and-bitwidth
EMULATOR = mupen64plus
EMU_FLAGS = --noosd
LOADER = loader64
LOADER_FLAGS = -vwf
SHA1SUM = sha1sum
PRINT = printf

ifeq ($(COLOR),1)
NO_COL  := \033[0m
RED     := \033[0;31m
GREEN   := \033[0;32m
BLUE    := \033[0;34m
YELLOW  := \033[0;33m
BLINK   := \033[33;5m
endif

# Use Objcopy instead of extract_data_for_mio
ifeq ($(COMPILER),gcc)
  EXTRACT_DATA_FOR_MIO := $(OBJCOPY) -O binary --only-section=.data
endif

# Common build print status function
define print
  @$(PRINT) "$(GREEN)$(1) $(YELLOW)$(2)$(GREEN) -> $(BLUE)$(3)$(NO_COL)\n"
endef

#==============================================================================#
# Main Targets                                                                 #
#==============================================================================#

ifeq ($(TARGET_N64),1)
all: $(ROM)
ifeq ($(COMPARE),1)
	@$(PRINT) "$(GREEN)Checking if ROM matches.. $(NO_COL)\n"
	@$(SHA1SUM) --quiet -c $(TARGET).sha1 && $(PRINT) "$(TARGET): $(GREEN)OK$(NO_COL)\n" || ($(PRINT) "$(YELLOW)Building the ROM file has succeeded, but does not match the original ROM.\nThis is expected, and not an error, if you are making modifications.\nTo silence this message, use 'make COMPARE=0.' $(NO_COL)\n" && false)
endif
else
all: $(EXE)
endif

clean:
	$(RM) -r $(BUILD_DIR_BASE)

distclean: clean
	$(PYTHON) extract_assets.py --clean
	$(MAKE) -C $(TOOLS_DIR) clean

test: $(ROM)
	$(EMULATOR) $(EMU_FLAGS) $<

load: $(ROM)
	$(LOADER) $(LOADER_FLAGS) $<

libultra: $(BUILD_DIR)/libultra.a

# Extra object file dependencies
$(BUILD_DIR)/asm/boot.o:              $(IPL3_RAW_FILES)
$(BUILD_DIR)/src/game/crash_screen.o: $(CRASH_TEXTURE_C_FILES)
$(BUILD_DIR)/lib/rsp.o:               $(BUILD_DIR)/rsp/rspboot.bin $(BUILD_DIR)/rsp/fast3d.bin $(BUILD_DIR)/rsp/audio.bin
$(SOUND_BIN_DIR)/sound_data.o:        $(SOUND_BIN_DIR)/sound_data.ctl.inc.c $(SOUND_BIN_DIR)/sound_data.tbl.inc.c $(SOUND_BIN_DIR)/sequences.bin.inc.c $(SOUND_BIN_DIR)/bank_sets.inc.c
$(BUILD_DIR)/levels/scripts.o:        $(BUILD_DIR)/include/level_headers.h

ifeq ($(VERSION),sh)
  $(BUILD_DIR)/src/audio/load_sh.o: $(SOUND_BIN_DIR)/bank_sets.inc.c $(SOUND_BIN_DIR)/sequences_header.inc.c $(SOUND_BIN_DIR)/ctl_header.inc.c $(SOUND_BIN_DIR)/tbl_header.inc.c
endif

$(CRASH_TEXTURE_C_FILES): TEXTURE_ENCODING := u32

ifeq ($(TARGET_N64),0)
  $(BUILD_DIR)/src/pc/dlmalloc.o: CFLAGS += -fno-builtin
endif

ifeq ($(COMPILER),gcc)
  $(BUILD_DIR)/lib/src/math/%.o: CFLAGS += -fno-builtin
endif

ifeq ($(VERSION),eu)
  TEXT_DIRS := text/de text/us text/fr

  # EU encoded text inserted into individual segment 0x19 files,
  # and course data also duplicated in leveldata.c
  $(BUILD_DIR)/bin/eu/translation_en.o: $(BUILD_DIR)/text/us/define_text.inc.c
  $(BUILD_DIR)/bin/eu/translation_de.o: $(BUILD_DIR)/text/de/define_text.inc.c
  $(BUILD_DIR)/bin/eu/translation_fr.o: $(BUILD_DIR)/text/fr/define_text.inc.c
  $(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/include/text_strings.h
  $(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/us/define_courses.inc.c
  $(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/de/define_courses.inc.c
  $(BUILD_DIR)/levels/menu/leveldata.o: $(BUILD_DIR)/text/fr/define_courses.inc.c
else
  ifeq ($(VERSION),sh)
    TEXT_DIRS := text/jp
    $(BUILD_DIR)/bin/segment2.o: $(BUILD_DIR)/text/jp/define_text.inc.c
  else
    TEXT_DIRS := text/$(VERSION)
    # non-EU encoded text inserted into segment 0x02
    $(BUILD_DIR)/bin/segment2.o: $(BUILD_DIR)/text/$(VERSION)/define_text.inc.c
  endif
endif

ALL_DIRS := $(BUILD_DIR) $(addprefix $(BUILD_DIR)/,$(SRC_DIRS) $(GODDARD_SRC_DIRS) $(ULTRA_SRC_DIRS) $(ULTRA_BIN_DIRS) $(BIN_DIRS) $(TEXTURE_DIRS) $(TEXT_DIRS) $(SOUND_SAMPLE_DIRS) $(addprefix levels/,$(LEVEL_DIRS)) rsp include) $(MIO0_DIR) $(addprefix $(MIO0_DIR)/,$(VERSION)) $(SOUND_BIN_DIR) $(SOUND_BIN_DIR)/sequences/$(VERSION)

# Make sure build directory exists before compiling anything
DUMMY != mkdir -p $(ALL_DIRS)
ifeq ($(TARGET_HANDHELD),1)
  DUMMY != mkdir -p $(BUILD_DIR)/src/pc
endif
ifeq ($(UBSAN),1)
  # Shift counts that are negative or >= 32 (MK64 1a4bbf7, `make PLAT=pc UBSAN=1`):
  # MIPS and x86 use the count's low 5 bits, ARM its low byte (-> 0), so only the
  # device would go wrong. Likewise a float converted to an int it doesn't fit:
  # x86 gives INT_MIN, ARM saturates (float-cast-overflow; MK64 b2dff40). Reports
  # print file:line; into a build dir of its own.
  CFLAGS += -fsanitize=shift-exponent,float-cast-overflow -fsanitize-recover=all
  LDFLAGS += -fsanitize=shift-exponent,float-cast-overflow
endif
# `make PLAT=pc SAN="-fsanitize=address,bounds,object-size --param asan-globals=0"` (into build/us_pc_san): the
# crash sweep's build (an index past a fixed-size array, stack and heap overruns); asan-globals=0 keeps the data layout.
ifneq ($(SAN),)
  CFLAGS += $(SAN) -fno-omit-frame-pointer -g
  LDFLAGS += $(filter -fsanitize=%,$(SAN))
endif
ifneq ($(filter miyoo,$(PLAT)),)
  # the audio mixer's inner loops (MK64: mixer.o, synthesis.o)
  $(BUILD_DIR)/src/pc/mixer.o $(BUILD_DIR)/src/audio/synthesis.o: CFLAGS += -O3 -funroll-loops
endif

ifeq ($(PLAT),miyoo)
  # the kit's Miyoo modules name their switches after the port: SM64_THREADS=1 (one core),
  # SM64_CPUCLOCK=<MHz> (0: the clock as the OS set it)
  $(BUILD_DIR)/kit/platform/miyoo_worker.o: CFLAGS += -U_GNU_SOURCE '-DMIYOO_WORKER_ENV="SM64_THREADS"'
  $(BUILD_DIR)/kit/platform/miyoo_clock.o: CFLAGS += '-DMIYOO_CLOCK_ENV="SM64_CPUCLOCK"'
endif

$(BUILD_DIR)/include/text_strings.h: $(BUILD_DIR)/include/text_menu_strings.h
$(BUILD_DIR)/src/menu/file_select.o: $(BUILD_DIR)/include/text_strings.h
$(BUILD_DIR)/src/menu/star_select.o: $(BUILD_DIR)/include/text_strings.h
$(BUILD_DIR)/src/game/ingame_menu.o: $(BUILD_DIR)/include/text_strings.h


#==============================================================================#
# Texture Generation                                                           #
#==============================================================================#
TEXTURE_ENCODING := u8

# Convert PNGs to RGBA32, RGBA16, IA16, IA8, IA4, IA1, I8, I4 binary files
$(BUILD_DIR)/%: %.png
	$(call print,Converting:,$<,$@)
	$(V)$(N64GRAPHICS) -s raw -i $@ -g $< -f $(lastword $(subst ., ,$@))

$(BUILD_DIR)/%.inc.c: %.png
	$(call print,Converting:,$<,$@)
	$(V)$(N64GRAPHICS) -s $(TEXTURE_ENCODING) -i $@ -g $< -f $(lastword ,$(subst ., ,$(basename $<)))

# Color Index CI8
$(BUILD_DIR)/%.ci8: %.ci8.png
	$(call print,Converting:,$<,$@)
	$(V)$(N64GRAPHICS_CI) -i $@ -g $< -f ci8

# Color Index CI4
$(BUILD_DIR)/%.ci4: %.ci4.png
	$(call print,Converting:,$<,$@)
	$(V)$(N64GRAPHICS_CI) -i $@ -g $< -f ci4


#==============================================================================#
# Compressed Segment Generation                                                #
#==============================================================================#

ifeq ($(TARGET_N64),1)
# Link segment file to resolve external labels
# TODO: ideally this would be `-Trodata-segment=0x07000000` but that doesn't set the address
$(BUILD_DIR)/%.elf: $(BUILD_DIR)/%.o
	$(call print,Linking ELF file:,$<,$@)
	$(V)$(LD) -e 0 -Ttext=$(SEGMENT_ADDRESS) -Map $@.map -o $@ $<
# Override for leveldata.elf, which otherwise matches the above pattern
.SECONDEXPANSION:
$(BUILD_DIR)/levels/%/leveldata.elf: $(BUILD_DIR)/levels/%/leveldata.o $(BUILD_DIR)/bin/$$(TEXTURE_BIN).elf
	$(call print,Linking ELF file:,$<,$@)
	$(V)$(LD) -e 0 -Ttext=$(SEGMENT_ADDRESS) -Map $@.map --just-symbols=$(BUILD_DIR)/bin/$(TEXTURE_BIN).elf -o $@ $<

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(call print,Extracting compressible data from:,$<,$@)
	$(V)$(EXTRACT_DATA_FOR_MIO) $< $@

$(BUILD_DIR)/levels/%/leveldata.bin: $(BUILD_DIR)/levels/%/leveldata.elf
	$(call print,Extracting compressible data from:,$<,$@)
	$(V)$(EXTRACT_DATA_FOR_MIO) $< $@

# Compress binary file
$(BUILD_DIR)/%.mio0: $(BUILD_DIR)/%.bin
	$(call print,Compressing:,$<,$@)
	$(V)$(MIO0TOOL) $< $@

# convert binary mio0 to object file
$(BUILD_DIR)/%.mio0.o: $(BUILD_DIR)/%.mio0
	$(call print,Converting MIO0 to ELF:,$<,$@)
	$(V)$(LD) -r -b binary $< -o $@
endif

#==============================================================================#
# Sound File Generation                                                        #
#==============================================================================#

$(BUILD_DIR)/%.table: %.aiff
	$(call print,Extracting codebook:,$<,$@)
	$(V)$(AIFF_EXTRACT_CODEBOOK) $< >$@

$(BUILD_DIR)/%.aifc: $(BUILD_DIR)/%.table %.aiff
	$(call print,Encoding ADPCM:,$(word 2,$^),$@)
	$(V)$(VADPCM_ENC) -c $^ $@

$(ENDIAN_BITWIDTH): $(TOOLS_DIR)/determine-endian-bitwidth.c
	@$(PRINT) "$(GREEN)Generating endian-bitwidth $(NO_COL)\n"
	$(V)$(CC) -c $(CFLAGS) -o $@.dummy2 $< 2>$@.dummy1; true
	$(V)grep -o 'msgbegin --endian .* --bitwidth .* msgend' $@.dummy1 > $@.dummy2
	$(V)head -n1 <$@.dummy2 | cut -d' ' -f2-5 > $@
	$(V)$(RM) $@.dummy1
	$(V)$(RM) $@.dummy2

$(SOUND_BIN_DIR)/sound_data.ctl: sound/sound_banks/ $(SOUND_BANK_FILES) $(SOUND_SAMPLE_AIFCS) $(ENDIAN_BITWIDTH)
	@$(PRINT) "$(GREEN)Generating:  $(BLUE)$@ $(NO_COL)\n"
	$(V)$(PYTHON) $(TOOLS_DIR)/assemble_sound.py $(BUILD_DIR)/sound/samples/ sound/sound_banks/ $(SOUND_BIN_DIR)/sound_data.ctl $(SOUND_BIN_DIR)/ctl_header $(SOUND_BIN_DIR)/sound_data.tbl $(SOUND_BIN_DIR)/tbl_header $(C_DEFINES) $$(cat $(ENDIAN_BITWIDTH))

$(SOUND_BIN_DIR)/sound_data.tbl: $(SOUND_BIN_DIR)/sound_data.ctl
	@true

$(SOUND_BIN_DIR)/ctl_header: $(SOUND_BIN_DIR)/sound_data.ctl
	@true

$(SOUND_BIN_DIR)/tbl_header: $(SOUND_BIN_DIR)/sound_data.ctl
	@true

$(SOUND_BIN_DIR)/sequences.bin: $(SOUND_BANK_FILES) sound/sequences.json $(SOUND_SEQUENCE_DIRS) $(SOUND_SEQUENCE_FILES) $(ENDIAN_BITWIDTH)
	@$(PRINT) "$(GREEN)Generating:  $(BLUE)$@ $(NO_COL)\n"
	$(V)$(PYTHON) $(TOOLS_DIR)/assemble_sound.py --sequences $@ $(SOUND_BIN_DIR)/sequences_header $(SOUND_BIN_DIR)/bank_sets sound/sound_banks/ sound/sequences.json $(SOUND_SEQUENCE_FILES) $(C_DEFINES) $$(cat $(ENDIAN_BITWIDTH))

$(SOUND_BIN_DIR)/bank_sets: $(SOUND_BIN_DIR)/sequences.bin
	@true

$(SOUND_BIN_DIR)/sequences_header: $(SOUND_BIN_DIR)/sequences.bin
	@true

$(SOUND_BIN_DIR)/%.m64: $(SOUND_BIN_DIR)/%.o
	$(call print,Converting to M64:,$<,$@)
	$(V)$(OBJCOPY) -j .rodata $< -O binary $@


#==============================================================================#
# Generated Source Code Files                                                  #
#==============================================================================#

# Convert binary file to a comma-separated list of byte values for inclusion in C code
$(BUILD_DIR)/%.inc.c: $(BUILD_DIR)/%
	$(call print,Converting to C:,$<,$@)
	$(V)hexdump -v -e '1/1 "0x%X,"' $< > $@
	$(V)echo >> $@

# Generate animation data
$(BUILD_DIR)/assets/mario_anim_data.c: $(wildcard assets/anims/*.inc.c)
	@$(PRINT) "$(GREEN)Generating animation data $(NO_COL)\n"
	$(V)$(PYTHON) $(TOOLS_DIR)/mario_anims_converter.py > $@

# Generate demo input data
$(BUILD_DIR)/assets/demo_data.c: assets/demo_data.json $(wildcard assets/demos/*.bin)
	@$(PRINT) "$(GREEN)Generating demo data $(NO_COL)\n"
	$(V)$(PYTHON) $(TOOLS_DIR)/demo_data_converter.py assets/demo_data.json $(DEF_INC_CFLAGS) > $@

# Encode in-game text strings
$(BUILD_DIR)/include/text_strings.h: include/text_strings.h.in
	$(call print,Encoding:,$<,$@)
	$(V)$(TEXTCONV) charmap.txt $< $@
$(BUILD_DIR)/include/text_menu_strings.h: include/text_menu_strings.h.in
	$(call print,Encoding:,$<,$@)
	$(V)$(TEXTCONV) charmap_menu.txt $< $@
$(BUILD_DIR)/text/%/define_courses.inc.c: text/define_courses.inc.c text/%/courses.h
	@$(PRINT) "$(GREEN)Preprocessing: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(CPP) $(CPPFLAGS) $< -o - -I text/$*/ | $(TEXTCONV) charmap.txt - $@
$(BUILD_DIR)/text/%/define_text.inc.c: text/define_text.inc.c text/%/courses.h text/%/dialogs.h
	@$(PRINT) "$(GREEN)Preprocessing: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(CPP) $(CPPFLAGS) $< -o - -I text/$*/ | $(TEXTCONV) charmap.txt - $@

# Level headers
$(BUILD_DIR)/include/level_headers.h: levels/level_headers.h.in
	$(call print,Preprocessing level headers:,$<,$@)
	$(V)$(CPP) $(CPPFLAGS) -I . $< | sed -E 's|(.+)|#include "\1"|' > $@

# Run asm_processor on files that have NON_MATCHING code
ifeq ($(NON_MATCHING),0)
$(GLOBAL_ASM_O_FILES): CC := $(V)$(PYTHON) $(TOOLS_DIR)/asm_processor/build.py $(CC) -- $(AS) $(ASFLAGS) --
endif

# Rebuild files with 'GLOBAL_ASM' if the NON_MATCHING flag changes.
$(GLOBAL_ASM_O_FILES): $(GLOBAL_ASM_DEP).$(NON_MATCHING)
$(GLOBAL_ASM_DEP).$(NON_MATCHING):
	@$(RM) $(GLOBAL_ASM_DEP).*
	$(V)touch $@


#==============================================================================#
# Compilation Recipes                                                          #
#==============================================================================#

# Compile C/C++ code
$(BUILD_DIR)/%.o: %.cpp
	$(call print,Compiling:,$<,$@)
	@$(CXX) -fsyntax-only $(CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	$(V)$(CXX) -c $(CFLAGS) -o $@ $<
$(BUILD_DIR)/%.o: %.c
	$(call print,Compiling:,$<,$@)
	@$(CC_CHECK) $(CC_CHECK_CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	$(V)$(CC) -c $(CFLAGS) -o $@ $<
$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.c
	$(call print,Compiling:,$<,$@)
	@$(CC_CHECK) $(CC_CHECK_CFLAGS) -MMD -MP -MT $@ -MF $(BUILD_DIR)/$*.d $<
	$(V)$(CC) -c $(CFLAGS) -o $@ $<

# Alternate compiler flags needed for matching
ifeq ($(COMPILER),ido)
  $(BUILD_DIR)/levels/%/leveldata.o: OPT_FLAGS := -g
  $(BUILD_DIR)/actors/%.o:           OPT_FLAGS := -g
  $(BUILD_DIR)/bin/%.o:              OPT_FLAGS := -g
  $(BUILD_DIR)/src/goddard/%.o:      OPT_FLAGS := -g
  $(BUILD_DIR)/src/goddard/%.o:      MIPSISET := -mips1
  $(BUILD_DIR)/lib/src/%.o:          OPT_FLAGS :=
  $(BUILD_DIR)/lib/src/math/%.o:     OPT_FLAGS := -O2
  $(BUILD_DIR)/lib/src/math/ll%.o:   OPT_FLAGS :=
  $(BUILD_DIR)/lib/src/math/ll%.o:   MIPSISET := -mips3 -32
  $(BUILD_DIR)/lib/src/ldiv.o:       OPT_FLAGS := -O2
  $(BUILD_DIR)/lib/src/string.o:     OPT_FLAGS := -O2
  $(BUILD_DIR)/lib/src/gu%.o:        OPT_FLAGS := -O3
  $(BUILD_DIR)/lib/src/al%.o:        OPT_FLAGS := -O3

  ifeq ($(VERSION),sh)
    $(BUILD_DIR)/lib/src/_Ldtob.o:   OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/_Litob.o:   OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/_Printf.o:  OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/sprintf.o:  OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/osDriveRomInit.o: OPT_FLAGS := -g
  endif
  ifeq ($(VERSION),eu)
    $(BUILD_DIR)/lib/src/_Ldtob.o:   OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/_Litob.o:   OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/_Printf.o:  OPT_FLAGS := -O3
    $(BUILD_DIR)/lib/src/sprintf.o:  OPT_FLAGS := -O3

    # For all audio files other than external.c and port_eu.c, put string literals
    # in .data. (In Shindou, the port_eu.c string literals also moved to .data.)
    $(BUILD_DIR)/src/audio/%.o:        OPT_FLAGS := -O2 -use_readwrite_const
    $(BUILD_DIR)/src/audio/port_eu.o:  OPT_FLAGS := -O2
  endif
  ifeq ($(VERSION_JP_US),true)
    $(BUILD_DIR)/src/audio/%.o:        OPT_FLAGS := -O2 -Wo,-loopunroll,0
    $(BUILD_DIR)/src/audio/load.o:     OPT_FLAGS := -O2 -Wo,-loopunroll,0 -framepointer
    # The source-to-source optimizer copt is enabled for audio. This makes it use
    # acpp, which needs -Wp,-+ to handle C++-style comments.
    # All other files than external.c should really use copt, but only a few have
    # been matched so far.
    $(BUILD_DIR)/src/audio/effects.o:   OPT_FLAGS := -O2 -Wo,-loopunroll,0 -sopt,-inline=sequence_channel_process_sound,-scalaroptimize=1 -Wp,-+
    $(BUILD_DIR)/src/audio/synthesis.o: OPT_FLAGS := -O2 -Wo,-loopunroll,0 -sopt,-scalaroptimize=1 -Wp,-+
  endif
  $(BUILD_DIR)/src/audio/external.o: OPT_FLAGS := -O2 -Wo,-loopunroll,0

# Add a target for build/eu/src/audio/*.copt to make it easier to see debug
$(BUILD_DIR)/src/audio/%.acpp: src/audio/%.c
	$(ACPP) $(TARGET_CFLAGS) $(DEF_INC_CFLAGS) -D__sgi -+ $< > $@
$(BUILD_DIR)/src/audio/%.copt: $(BUILD_DIR)/src/audio/%.acpp
	$(COPT) -signed -I=$< -CMP=$@ -cp=i -scalaroptimize=1 $(COPTFLAGS)
$(BUILD_DIR)/src/audio/seqplayer.copt: COPTFLAGS := -inline_manual

endif

# Assemble assembly code
$(BUILD_DIR)/%.o: %.s
	$(call print,Assembling:,$<,$@)
	$(V)$(CPP) $(CPPFLAGS) $< | $(AS) $(ASFLAGS) -MD $(BUILD_DIR)/$*.d -o $@

# Assemble RSP assembly code
$(BUILD_DIR)/rsp/%.bin $(BUILD_DIR)/rsp/%_data.bin: rsp/%.s
	$(call print,Assembling:,$<,$@)
	$(V)$(RSPASM) -sym $@.sym $(RSPASMFLAGS) -strequ CODE_FILE $(BUILD_DIR)/rsp/$*.bin -strequ DATA_FILE $(BUILD_DIR)/rsp/$*_data.bin $<

ifeq ($(TARGET_N64),1)
# Run linker script through the C preprocessor
$(BUILD_DIR)/$(LD_SCRIPT): $(LD_SCRIPT)
	$(call print,Preprocessing linker script:,$<,$@)
	$(V)$(CPP) $(CPPFLAGS) -DBUILD_DIR=$(BUILD_DIR) -MMD -MP -MT $@ -MF $@.d -o $@ $<

# Link libultra
$(BUILD_DIR)/libultra.a: $(ULTRA_O_FILES)
	@$(PRINT) "$(GREEN)Linking libultra:  $(BLUE)$@ $(NO_COL)\n"
	$(V)$(AR) rcs -o $@ $(ULTRA_O_FILES)
	$(V)$(TOOLS_DIR)/patch_elf_32bit $@

# Link libgoddard
$(BUILD_DIR)/libgoddard.a: $(GODDARD_O_FILES)
	@$(PRINT) "$(GREEN)Linking libgoddard:  $(BLUE)$@ $(NO_COL)\n"
	$(V)$(AR) rcs -o $@ $(GODDARD_O_FILES)

# Link SM64 ELF file
$(ELF): $(O_FILES) $(MIO0_OBJ_FILES) $(SEG_FILES) $(BUILD_DIR)/$(LD_SCRIPT) undefined_syms.txt $(BUILD_DIR)/libultra.a $(BUILD_DIR)/libgoddard.a
	@$(PRINT) "$(GREEN)Linking ELF file:  $(BLUE)$@ $(NO_COL)\n"
	$(V)$(LD) -L $(BUILD_DIR) -T undefined_syms.txt -T $(BUILD_DIR)/$(LD_SCRIPT) -Map $(BUILD_DIR)/sm64.$(VERSION).map --no-check-sections $(addprefix -R ,$(SEG_FILES)) -o $@ $(O_FILES) -lultra -lgoddard

# Build ROM
$(ROM): $(ELF)
	$(call print,Building ROM:,$<,$@)
	$(V)$(OBJCOPY) --pad-to=0x800000 --gap-fill=0xFF $< $(@:.z64=.bin) -O binary
	$(V)$(N64CKSUM) $(@:.z64=.bin) $@

$(BUILD_DIR)/$(TARGET).objdump: $(ELF)
	$(OBJDUMP) -D $< > $@

else ifeq ($(TARGET_HANDHELD),1)
#==============================================================================#
# Asset split (M3; the MK64 port's M6, from beckerd's PSP asset split)   #
#==============================================================================#
# The shipped executable carries none of the game's ROM-derived data. The data
# objects' .data/.rodata are renamed to .assetdata/.assetrodata and linked into
# one region after .bss: NOLOAD in the executable (addresses, no bytes), filled
# at boot from the player's ROM by recipes.bin (numbers only), then cached as
# assets.bin; PROGBITS in a second identical link, whose bytes make the local
# assets.bin the recipes are derived against (tools/assets/, local only).
HH_ALL_OBJS := $(O_FILES) $(ULTRA_O_FILES) $(GODDARD_O_FILES)
# The decomp's data (restored untracked by tools/decomp_data.sh) + the trig tables.
ASSET_OBJS := $(filter $(BUILD_DIR)/actors/% $(BUILD_DIR)/levels/% $(BUILD_DIR)/bin/% $(BUILD_DIR)/sound/% \
                       $(BUILD_DIR)/assets/% $(BUILD_DIR)/data/% $(BUILD_DIR)/src/goddard/dynlists/% \
                       $(BUILD_DIR)/src/hh/hh_trig_asset.o,$(HH_ALL_OBJS))
CODE_OBJS := $(filter-out $(ASSET_OBJS),$(HH_ALL_OBJS))
ASSET_RENAMED := $(patsubst $(BUILD_DIR)/%,$(BUILD_DIR)/asset_objs/%,$(ASSET_OBJS))
# Every initialised-data section GCC can emit (plain, pointer-bearing, merged
# string/constant pools) -> .asset*; names that don't occur are skipped.
ASSET_RENAMES := .data=.assetdata .data.rel=.assetdata.rel .data.rel.local=.assetdata.rel.local \
                 .data.rel.ro=.assetdata.relro .data.rel.ro.local=.assetdata.relro.local \
                 .rodata=.assetrodata .rodata.str1.1=.assetrodata.str1 .rodata.str1.4=.assetrodata.str4 \
                 .rodata.str1.8=.assetrodata.str8 .rodata.cst4=.assetrodata.cst4 .rodata.cst8=.assetrodata.cst8 \
                 .rodata.cst16=.assetrodata.cst16
ASSET_RENAME_FLAGS := $(foreach r,$(ASSET_RENAMES),--rename-section $(r))
$(BUILD_DIR)/asset_objs/%.o: $(BUILD_DIR)/%.o
	@mkdir -p $(dir $@)
	$(V)$(OBJCOPY) $(ASSET_RENAME_FLAGS) $< $@
HH_LINK_OBJS := $(CODE_OBJS) $(ASSET_RENAMED)
# Page-aligned region (a cache could be mmapped over it; today it is read in).
# The levels' writable data leads it: the paintings and moving textures, which
# the game changes as it runs -- game state, so save states include
# [__assets_start, __assets_state_end) (savestate.c). The rest is never written
# (checked with SM64_ASSET_DIFF, tools/assets/asset_writes.py).
define HH_ASSET_LD
SECTIONS\n{\n  .assetdata $(1) : ALIGN(4096) { __assets_start = .; *levels/*(.assetdata .assetdata.*) __assets_state_end = .; *(.assetdata .assetdata.*) *(.assetrodata .assetrodata.*) . = ALIGN(4096); __assets_end = .; }\n}\nINSERT AFTER .bss;\n
endef
HH_LD_NOLOAD := $(BUILD_DIR)/assets_noload.ld
HH_LD_PROGBITS := $(BUILD_DIR)/assets_progbits.ld
$(HH_LD_NOLOAD): Makefile
	$(V)printf '$(call HH_ASSET_LD,(NOLOAD))' > $@
$(HH_LD_PROGBITS): Makefile
	$(V)printf '$(call HH_ASSET_LD,)' > $@
# Build stamp: a random word compiled into each link, recorded in every save
# state, the recipe table and the asset cache, so the game only restores a
# state of, extracts with, or loads a cache from its own build. Same inputs as
# the link, so nothing relinks without a new stamp.
HH_STAMP_C := $(BUILD_DIR)/build_stamp.c
HH_STAMP_O := $(HH_STAMP_C:.c=.o)
$(HH_STAMP_C): $(HH_LINK_OBJS) $(HH_LD_NOLOAD)
	$(V)printf '/* generated per link: recipes.bin and assets.bin must carry this value */\n#include <stdint.h>\nconst uint32_t port_build_stamp = 0x%s;\n' "$$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')" > $@
$(HH_STAMP_O): $(HH_STAMP_C)
	$(V)$(CC) -c $(CFLAGS) -o $@ $<
HH_LINK_TAIL := $(HH_LINK_OBJS) $(HH_STAMP_O) $(LDFLAGS) -Wl,--defsym=_ftext=__executable_start -Wl,--defsym=_fbss=__bss_start
ifeq ($(PLAT),pc)
# The desktop proxy (a dev tool) links the region's bytes in: same layout, no
# ROM step. The shipped path -- extraction from the ROM -- is the device
# build's, tested on it under qemu.
HH_EXE_LD := $(HH_LD_PROGBITS)
$(BUILD_DIR)/src/hh/assets_handheld.o: CFLAGS += -DHH_ASSETS_LINKED
else
HH_EXE_LD := $(HH_LD_NOLOAD)
endif
# The rasterizer without GCC's first (pre-register-allocation) scheduling pass: it stretched the
# NEON kernels' live ranges past the 16 q registers, and the water's row function spent 22.5% of
# its executed instructions on stack spills. qemu frames 400-500: JRB -5.4%, BoB -3.4%; the post-RA
# pass still schedules. Same code otherwise (no float contraction), same pixels. -fsched-pressure
# changed nothing; the flag on every file gains nothing more.
# -g1 (line tables only, for the profilers' addr2line): the full -g's variable tracking took the compile past 1.6 GB
# (2026-10-07); the code is the same at every -g level.
ifneq ($(filter miyoo,$(PLAT)),)
HH_RASTER_OBJS := $(addprefix $(BUILD_DIR)/src/hh/,gfx_soft.o gfx_soft_rows_lean1.o gfx_soft_rows_lean2.o gfx_soft_rows_lean3.o)
$(HH_RASTER_OBJS): CFLAGS += -fno-schedule-insns -g1
endif
$(EXE): $(HH_LINK_OBJS) $(HH_STAMP_O) $(HH_EXE_LD)
	$(call print,Linking:,$(BUILD_DIR),$@)
	$(V)$(LD) -L $(BUILD_DIR) -Wl,-T,$(HH_EXE_LD) -o $@ $(HH_LINK_TAIL)
HH_ELF_ASSETS := $(BUILD_DIR)/sm64_assets.elf
$(HH_ELF_ASSETS): $(HH_LINK_OBJS) $(HH_STAMP_O) $(HH_LD_PROGBITS)
	$(V)$(LD) -L $(BUILD_DIR) -Wl,-q -Wl,-T,$(HH_LD_PROGBITS) -o $@ $(HH_LINK_TAIL)
HH_TOOLS := ../tools/assets
HH_ASSETS_BIN := $(BUILD_DIR)/assets.bin
$(HH_ASSETS_BIN): $(HH_ELF_ASSETS) $(EXE) $(HH_TOOLS)/make_assets.py
	$(V)python3 $(HH_TOOLS)/make_assets.py $(HH_ELF_ASSETS) $(EXE) -o $@ --page 4096
# Slow (tens of minutes): find, for every asset symbol, how it derives from the
# ROM. Needed whenever the asset LAYOUT changes (any asset source, compiler or
# flag change), not for code-only changes: `make PLAT=miyoo assets-recipes`.
# The JSON is numbers only and tracked; emit_recipes.py refuses a stale one.
HH_RECIPES_OUT := $(HH_TOOLS)/recipes_$(PLAT).json
HH_RECIPES_JSON := $(HH_RECIPES_OUT)
HH_RECIPES_BIN := $(BUILD_DIR)/recipes.bin
assets-recipes: $(HH_ASSETS_BIN)
	python3 $(HH_TOOLS)/derive_recipes.py $(HH_ASSETS_BIN) baserom.$(VERSION).z64 -o $(HH_RECIPES_OUT) --cache $(BUILD_DIR)/mio0.cache --report $(BUILD_DIR)/recipes_report.txt
$(HH_RECIPES_BIN): $(wildcard $(HH_RECIPES_JSON)) $(HH_ASSETS_BIN) $(HH_STAMP_C) $(HH_TOOLS)/emit_recipes.py
	$(V)if [ -f $(HH_RECIPES_JSON) ]; then python3 $(HH_TOOLS)/emit_recipes.py $(HH_RECIPES_JSON) $(HH_ASSETS_BIN) -o $@ --stamp $$(sed -n 's/.*= 0x\([0-9a-fA-F]*\);.*/\1/p' $(HH_STAMP_C)); \
	else echo "NOTE: $(HH_RECIPES_JSON) missing - this build cannot extract from a ROM (run: make PLAT=$(PLAT) assets-recipes)"; printf 'SM64RCP0' > $@; fi
ifneq ($(filter miyoo,$(PLAT)),)
all: $(HH_ASSETS_BIN) $(HH_RECIPES_BIN)
endif
# Sources whose bytes stay in the executable (tools/assets/check_provenance.sh
# checks each is tracked in git).
HH_OBJ_SRC = $(firstword $(wildcard $(patsubst $(BUILD_DIR)/%.o,%.c,$(1)) $(patsubst $(BUILD_DIR)/%.o,%.s,$(1)) $(1:.o=.c)))
print-code-sources:
	@printf '%s\n' $(foreach o,$(filter-out $(HH_STAMP_O),$(CODE_OBJS)),$(call HH_OBJ_SRC,$(o)))
print-asset-objects:
	@printf '%s\n' $(ASSET_OBJS)
.PHONY: assets-recipes print-code-sources print-asset-objects
else
$(EXE): $(O_FILES) $(MIO0_FILES:.mio0=.o) $(ULTRA_O_FILES) $(GODDARD_O_FILES)
	$(LD) -L $(BUILD_DIR) -o $@ $(O_FILES) $(ULTRA_O_FILES) $(GODDARD_O_FILES) $(LDFLAGS)
endif



.PHONY: all clean distclean default diff test load libultra
# with no prerequisites, .SECONDARY causes no intermediate target to be removed
.SECONDARY:

# Remove built-in rules, to improve performance
MAKEFLAGS += --no-builtin-rules

-include $(DEP_FILES)

print-% : ; $(info $* is a $(flavor $*) variable set to [$($*)]) @true
