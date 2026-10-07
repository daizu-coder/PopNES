# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
# PORT NOTE: this Makefile follows the pattern worked out in
# the sister CE ports (PicoDrive CE, snes9x2002 CE, gpsp CE), but the
# core here is unlike any of them: nes_emu/*.cpp is C++ (see the
# rationale below), while every prior CE port deliberately stayed
# pure C to avoid a runtime dependency on libstdc++-6.dll (which does not
# exist on this device). A feasibility spike (minimal new/delete + pure
# virtual + global-ctor program, arm-mingw32ce-g++, linked with
# -static-libgcc -static-libstdc++) confirmed the resulting binary's only
# imported DLL is COREDLL.dll, same as every C-only port - so this
# Makefile links the whole app (CE shell .c files + nes_emu/libretro.cpp
# .cpp files) as one static-libstdc++ binary via $(CXX), not $(CC).
#
# Windows CE (SHARP Brain PW-G5300, ARM, no HW integer divide) port of
# QuickNES via its libretro core. Built with the cegcc cross toolchain.
#
# Run from inside this directory:
#   make clean && make && make strip
#
# Scope/architecture decisions:
#  - Core is NOT patched (nes_emu/*.cpp and libretro/libretro.cpp are
#    compiled as-is). QuickNES's own libretro.cpp already outputs
#    RETRO_PIXEL_FORMAT_RGB565 unconditionally on this build (no PSP/PS2
#    branch taken, since PSP/RENDER_GSKIT_PS2 are never defined here),
#    which matches ce_display.c's RGB565 blit path exactly.
#  - Video output is plain GDI (ce_display.c), not GAPI (gx.dll) - see
#    that file's own header comment for why (a licensing concern shared
#    with the sister gnuboy CE port on this same device, not a
#    technical one).
#  - No zlib/unzip needed: confirmed by grep that nothing under nes_emu/
#    or libretro/ references zlib.h/zip.h/gzip. ce_fileopen.c's ROM list
#    therefore should NOT offer .zip (unlike the PicoDrive port, which
#    has real unzip.c support) - see ce_fileopen.c's own IsRomExtension().
#  - No JIT/dynamic recompiler: QuickNES's CPU core is a straightforward
#    6502 switch-based interpreter (no _arm.S variant), which is already
#    the "portable C implementation" the sister ports recommend
#    starting with. At NES's ~1.79MHz 6502 (vs. e.g.
#    GBA's ~16.78MHz ARM7 that motivated gpsp CE's real dynarec), a
#    dynarec is unlikely to ever be needed - measure with the
#    "perf: retro_run" logging already built into ce_main.c's main loop
#    before considering one.
#  - ce_pprof.c/h and ce_strerror_stub.c from the template are NOT used:
#    both exist solely for PicoDrive's own core-internal pprof injection
#    and zlib/vgm strerror() calls respectively, neither of which apply
#    here (no core patching, no zlib).

ROOT := ..

TOOLPREFIX := /opt/cegcc/bin/arm-mingw32ce-
CC      := $(TOOLPREFIX)gcc
CXX     := $(TOOLPREFIX)g++
STRIP   := $(TOOLPREFIX)strip
OBJDUMP := $(TOOLPREFIX)objdump
NM      := $(TOOLPREFIX)nm
WINDRES := $(TOOLPREFIX)windres

# armv5te: matches the target device (has BLX/BLX-reg and other v5T
# instructions). mno-unaligned-access: this device's CPU does not
# tolerate unaligned word accesses (confirmed the hard way by every
# sister CE port) - let the compiler emit safe byte/half accesses
# instead of faulting.
ARCH_FLAGS := -march=armv5te -mno-unaligned-access

# === Core sources (unmodified upstream code, C++) ===

NES_EMU_DIR  := $(ROOT)/nes_emu
LIBRETRO_DIR := $(ROOT)/libretro

SOURCES_CORE := \
	$(LIBRETRO_DIR)/libretro.cpp \
	$(NES_EMU_DIR)/abstract_file.cpp \
	$(NES_EMU_DIR)/apu_state.cpp \
	$(NES_EMU_DIR)/Blip_Buffer.cpp \
	$(NES_EMU_DIR)/Effects_Buffer.cpp \
	$(NES_EMU_DIR)/Multi_Buffer.cpp \
	$(NES_EMU_DIR)/Nes_Apu.cpp \
	$(NES_EMU_DIR)/Nes_Buffer.cpp \
	$(NES_EMU_DIR)/Nes_Cart.cpp \
	$(NES_EMU_DIR)/Nes_Core.cpp \
	$(NES_EMU_DIR)/Nes_Cpu.cpp \
	$(NES_EMU_DIR)/nes_data.cpp \
	$(NES_EMU_DIR)/Nes_Effects_Buffer.cpp \
	$(NES_EMU_DIR)/Nes_Emu.cpp \
	$(NES_EMU_DIR)/Nes_File.cpp \
	$(NES_EMU_DIR)/Nes_Fme7_Apu.cpp \
	$(NES_EMU_DIR)/Nes_Mapper.cpp \
	$(NES_EMU_DIR)/Nes_Vrc7.cpp \
	$(NES_EMU_DIR)/emu2413.cpp \
	$(NES_EMU_DIR)/emu2413_state.cpp \
	$(NES_EMU_DIR)/Nes_Namco_Apu.cpp \
	$(NES_EMU_DIR)/Nes_Oscs.cpp \
	$(NES_EMU_DIR)/Nes_Ppu.cpp \
	$(NES_EMU_DIR)/Nes_Ppu_Impl.cpp \
	$(NES_EMU_DIR)/Nes_Ppu_Rendering.cpp \
	$(NES_EMU_DIR)/Nes_State.cpp \
	$(NES_EMU_DIR)/nes_util.cpp \
	$(NES_EMU_DIR)/Nes_Vrc6_Apu.cpp \
	$(NES_EMU_DIR)/Data_Reader.cpp \
	$(NES_EMU_DIR)/nes_ntsc.cpp

SOURCES_CE := ce_main.c ce_log.c ce_display.c ce_input.c ce_audio.c ce_video.c \
	ce_config.c ce_lang.c ce_fileopen.c ce_bmpfont.c

DEFINES  += -D__LIBRETRO__ -DWINCE -D_WIN32_WCE=0x0500 -DNO_UNALIGNED_ACCESS
INCLUDES += -I. -Icompat -I$(NES_EMU_DIR) -I$(LIBRETRO_DIR) \
	-I$(LIBRETRO_DIR)/libretro-common/include

# -O3: both sister CE ports measured a real gain over -O2 on this device
# with no regressions; -flto is NOT used - this cegcc ld doesn't support
# LTO plugin objects (confirmed by prior ports).
CFLAGS   := $(ARCH_FLAGS) -O3 -fomit-frame-pointer $(DEFINES) $(INCLUDES)
CXXFLAGS := $(ARCH_FLAGS) -O3 -fomit-frame-pointer -Wno-multichar $(DEFINES) $(INCLUDES)

# -static-libgcc -static-libstdc++: this device has no libstdc++-6.dll
# (confirmed absent, same as the C-only ports' original libgcc concern) -
# statically linking both keeps the only runtime DLL dependency at
# COREDLL.dll, verified for the "new/delete + pure virtual + global ctor"
# subset nes_emu/ actually uses via the feasibility spike above.
# -lm: nes_emu/Blip_Buffer.cpp, nes_emu/emu2413.cpp and libretro.cpp all
# use libm (sqrt/floor/pow-family calls in the resampling/EQ/FM-synth
# math), so this must be linked in explicitly like the sister ports do.
LDFLAGS := -static-libgcc -static-libstdc++ -lm

RESOURCES := ce_res.o

OBJECTS := $(patsubst %.cpp,%.o,$(SOURCES_CORE)) \
	$(SOURCES_CE:.c=.o) $(RESOURCES)

TARGET := AppMain.exe

# UI bitmap font (ce_bmpfont.c) - same switch as PopPCE (NitroGrafx-main,
# 2026-09-27). Galmuri14 is the default; the others stay buildable:
#   make                     Galmuri14 (default)  -> AppMain.exe
#   make CE_FONT=shinonome   Shinonome 16dot      -> AppMain_shinonome.exe
#   make CE_FONT=galmuri11   GalmuriMono11        -> AppMain_galmuri11.exe
# ce_galmuri14.h / ce_galmuri11.h are copies of PopPCE's
# tools/galmuri2c.py output. Only ce_bmpfont.o and ce_fileopen.o see the
# define; the .font-<name> stamp makes them rebuild when CE_FONT changes.
CE_FONT ?= galmuri14
ifeq ($(CE_FONT),galmuri14)
FONT_DEFS := -DCE_FONT_GALMURI14
else ifeq ($(CE_FONT),shinonome)
FONT_DEFS :=
TARGET := AppMain_shinonome.exe
else ifeq ($(CE_FONT),galmuri11)
FONT_DEFS := -DCE_FONT_GALMURI11
TARGET := AppMain_galmuri11.exe
else
$(error CE_FONT must be galmuri14, shinonome or galmuri11)
endif
FONT_STAMP := .font-$(CE_FONT)

.PHONY: all clean strip

all: $(TARGET)

$(FONT_STAMP):
	rm -f .font-*
	touch $@

ce_bmpfont.o ce_fileopen.o: $(FONT_STAMP) ce_bmpfont.h ce_galmuri14.h ce_galmuri11.h ce_shinonome16.h
ce_bmpfont.o ce_fileopen.o: CFLAGS += $(FONT_DEFS)

# Link with $(CXX), not $(CC): pulls in the C++ runtime startup path
# (needed for the file-scope Mono_Buffer/Effects_Buffer/etc. global
# objects' constructors in libretro.cpp) alongside the static libstdc++
# named in LDFLAGS above.
$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# windres doesn't track what the .rc pulls in - list it by hand so a
# changed icon, mascot bitmap or resource ID actually triggers a rebuild.
ce_res.o: ce_resource.h icon/popnes.ico icon/popnes_mascot.bmp

%.o: %.rc
	$(WINDRES) -F pe-arm-wince-little -I$(ROOT) -o $@ $<

strip: all
	$(STRIP) $(TARGET)

clean:
	rm -f .font-* AppMain_shinonome.exe AppMain_galmuri11.exe
	rm -f $(OBJECTS) $(TARGET)
