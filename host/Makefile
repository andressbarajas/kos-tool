# host/Makefile — builds kos-tool

ROOT ?= $(CURDIR)/..
include $(ROOT)/mk/version.mk
include $(ROOT)/mk/toolchains.mk
include $(ROOT)/mk/memory.mk

BUILDDIR    := build
MINILZO_DIR := $(ROOT)/third_party/minilzo
MINILZO_A   := $(MINILZO_DIR)/build-host/minilzo.a

# ---------- Platform detection ----------

UNAME_S := $(shell uname -s)
HOST_CC_DEFAULT := cc

ifeq ($(UNAME_S),Darwin)
  PLATFORM_SRCS := src/platform/posix.c src/platform/macos_serial.c
  ifeq ($(shell uname -m),arm64)
    # libelf.h falls through to `#include <libelf/sys_elf.h>` for non-internal
    # users, so the bare /opt/homebrew/include path has to be on -I too — the
    # /libelf-suffixed path alone wasn't enough to resolve that include.
    LIBELF_CFLAGS  := -I/opt/homebrew/include -I/opt/homebrew/include/libelf
    LIBELF_LDFLAGS := -L/opt/homebrew/lib -lelf
  else
    LIBELF_CFLAGS  := -I/usr/local/include -I/usr/local/include/libelf
    LIBELF_LDFLAGS := -L/usr/local/lib -lelf
  endif
  EXTRA_DEFS :=
else ifeq ($(UNAME_S),Linux)
  PLATFORM_SRCS := src/platform/posix.c src/platform/linux_serial.c
  LIBELF_CFLAGS  := $(shell pkg-config --cflags libelf 2>/dev/null)
  LIBELF_LDFLAGS := $(shell pkg-config --libs libelf 2>/dev/null || echo "-lelf")
  EXTRA_DEFS :=
else
  # Windows (MSYS2 / MinGW)
  PLATFORM_SRCS := src/platform/windows.c src/platform/windows_serial.c
  LIBELF_CFLAGS  :=
  LIBELF_LDFLAGS := -lelf
  EXTRA_DEFS     :=
  EXTRA_LDLIBS   := -lws2_32 -liphlpapi
  HOST_CC_DEFAULT := gcc
endif

# ---------- Compiler flags ----------

ifeq ($(origin CC),default)
CC      := $(HOST_CC_DEFAULT)
else ifeq ($(origin CC),undefined)
CC      := $(HOST_CC_DEFAULT)
endif

CFLAGS  := -O2 -Wall -Wextra -std=c11 \
    -Iinclude \
    -I$(ROOT)/include \
    -I$(ROOT)/third_party/minilzo \
    $(LIBELF_CFLAGS) \
    -D_GNU_SOURCE \
    -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\" \
    -DKOSLOAD_GIT_REV=\"$(KOSLOAD_GIT_REV)\"

# Automatic header-dependency tracking.  -MMD writes a .d file alongside each
# .o listing every (non-system) header pulled in by the preprocessor; -MP adds
# phony targets for those headers so a renamed/deleted header doesn't break
# the build.  The .d files are then -include'd at the bottom.  Supersedes the
# coarse $(HOST_HEADERS) catch-all dep on the compile rule, which forces a
# full rebuild on any header touch — kept for now because it's harmless once
# .d files exist (auto-deps are a strict subset).
CFLAGS  += -MMD -MP

# addr2line defaults derived from the cross-toolchain config (mk/toolchains.mk),
# so they track DC_TOOLCHAIN/GC_TOOLCHAIN/PS2_EE_TOOLCHAIN (CLI > env > default).
# The *_TOOL_PREFIX defines let config.c rebuild the path from $*_TOOLCHAIN set
# in the tool's own runtime environment.
CFLAGS  += -DDEFAULT_SH4_ADDR2LINE=\"$(DC_TOOLCHAIN)/$(DC_PREFIX)addr2line\" \
           -DDEFAULT_PPC_ADDR2LINE=\"$(GC_TOOLCHAIN)/$(GC_PREFIX)addr2line\" \
           -DDEFAULT_MIPS_ADDR2LINE=\"$(PS2_EE_TOOLCHAIN)/$(PS2_PREFIX)addr2line\" \
           -DSH4_TOOL_PREFIX=\"$(DC_PREFIX)\" \
           -DPPC_TOOL_PREFIX=\"$(GC_PREFIX)\" \
           -DMIPS_TOOL_PREFIX=\"$(PS2_PREFIX)\"

# Loader base/load addresses for the firmware-update trampolines, from
# mk/memory.mk (the source of truth).  These override the protocol.h fallbacks.
CFLAGS  += -DDC_LOADER_BASE=$(DC_LOADER_BASE) \
           -DGC_LOADER_BASE=$(GC_LOADER_BASE) \
           -DWII_LOADER_BASE=$(WII_LOADER_BASE) \
           -DPS2_LOADER_BASE=$(PS2_LOADER_BASE)

LDFLAGS := $(LIBELF_LDFLAGS) $(EXTRA_LDLIBS)

# ---------- Firmware embedding ----------
# If client firmware .bin files exist, embed them into kos-tool for
# auto-update support. When bins are missing (e.g. standalone host build),
# firmware_update.c provides zero-length stubs.

BIN2C := $(CURDIR)/bin2c.sh

DC_SER_BIN := $(ROOT)/client/dreamcast/build/serial/dc-load-ser.bin
DC_IP_BIN  := $(ROOT)/client/dreamcast/build/ip/dc-load-ip.bin
GC_SER_BIN := $(ROOT)/client/gamecube/build/serial/gc-load-ser.bin
GC_IP_BIN  := $(ROOT)/client/gamecube/build/ip/gc-load-ip.bin
PS2_IP_BIN := $(ROOT)/client/playstation2/build/ip/ps2-load-ip-image.bin
WII_IP_BIN := $(ROOT)/client/wii/build/ip/wii-load-ip.bin

FW_OBJS :=
FW_DEFS :=

ifneq ($(wildcard $(DC_SER_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_dc_serial_data.o
  FW_DEFS += -DHAS_FIRMWARE_DC_SERIAL
endif
ifneq ($(wildcard $(DC_IP_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_dc_ip_data.o
  FW_DEFS += -DHAS_FIRMWARE_DC_IP
endif
ifneq ($(wildcard $(GC_SER_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_gc_serial_data.o
  FW_DEFS += -DHAS_FIRMWARE_GC_SERIAL
endif
ifneq ($(wildcard $(GC_IP_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_gc_ip_data.o
  FW_DEFS += -DHAS_FIRMWARE_GC_IP
endif
ifneq ($(wildcard $(PS2_IP_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_ps2_ip_data.o
  FW_DEFS += -DHAS_FIRMWARE_PS2_IP
endif
ifneq ($(wildcard $(WII_IP_BIN)),)
  FW_OBJS += $(BUILDDIR)/firmware_wii_ip_data.o
  FW_DEFS += -DHAS_FIRMWARE_WII_IP
endif

CFLAGS += $(FW_DEFS)

# ---------- Sources ----------

HOST_HEADERS := \
    $(wildcard include/kostool/*.h) \
    $(wildcard $(ROOT)/include/kosload/*.h)

CORE_SRCS := \
    src/main.c \
    src/discover.c \
    src/upload.c \
    src/download.c \
    src/execute.c \
    src/console.c \
    src/config.c \
    src/gdb.c \
    src/cdfs.c \
    src/syscalls.c \
    src/dedup.c \
    src/firmware_update.c \
    src/transport/serial.c \
    src/transport/network.c \
    src/binary/elf.c \
    src/binary/bfd.c \
    src/binary/srec.c \
    src/binary/dol.c \
    src/binary/raw.c

ALL_SRCS := $(CORE_SRCS) $(PLATFORM_SRCS)
OBJS     := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(ALL_SRCS))

TARGET   := $(BUILDDIR)/kos-tool

# Default target (must appear before version.h rule)
.DEFAULT_GOAL := all

# ---------- version.h ----------

VERSION_H    := $(ROOT)/include/kosload/version.h
VERSION_H_IN := $(ROOT)/include/kosload/version.h.in

$(VERSION_H): $(VERSION_H_IN) $(ROOT)/mk/version.mk
	@sed -e 's/@KOSLOAD_VERSION_MAJOR@/$(KOSLOAD_VERSION_MAJOR)/g' \
	     -e 's/@KOSLOAD_VERSION_MINOR@/$(KOSLOAD_VERSION_MINOR)/g' \
	     -e 's/@KOSLOAD_VERSION_PATCH@/$(KOSLOAD_VERSION_PATCH)/g' \
	     -e 's/@KOSLOAD_VERSION@/$(KOSLOAD_VERSION)/g' \
	     $< > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

# ---------- Rules ----------

.PHONY: all clean dev

all: $(TARGET)

# Development build: re-flash firmware even when the version is unchanged.
dev: CFLAGS += -DKOS_TOOL_DEV_FORCE_REFLASH
dev: all

$(TARGET): $(OBJS) $(FW_OBJS) $(MINILZO_A) | $(BUILDDIR)
	$(CC) $(OBJS) $(FW_OBJS) $(MINILZO_A) $(LDFLAGS) -o $@
	@echo "  Built: $@"

$(BUILDDIR)/%.o: src/%.c $(VERSION_H) $(HOST_HEADERS) | $(BUILDDIR)/transport $(BUILDDIR)/binary $(BUILDDIR)/platform
	$(CC) $(CFLAGS) -c $< -o $@

# main.o carries the git stamp (KOSLOAD_GIT_REV via -D); recompile it every
# build so `kos-tool` always reports the current revision. (One small file; the
# relink is instant.)
.PHONY: gitrev-stamp
gitrev-stamp:
$(BUILDDIR)/main.o: gitrev-stamp

# firmware_update.o uses #ifdef HAS_FIRMWARE_* to emit stubs for missing
# firmware.  When firmware availability changes (e.g. dc built before gc),
# FW_OBJS changes and firmware_update.o must be recompiled with the new
# FW_DEFS to avoid duplicate symbol errors at link time.
$(BUILDDIR)/firmware_update.o: $(FW_OBJS)

# KOS_TOOL_DEV_FORCE_REFLASH setting.  Recompile it every build.
.PHONY: fwupdate-stamp
fwupdate-stamp:
$(BUILDDIR)/firmware_update.o: fwupdate-stamp

$(MINILZO_A):
	$(MAKE) -C $(MINILZO_DIR) CC="$(CC)" AR=ar CFLAGS="-O2" BUILDDIR=build-host

$(BUILDDIR) $(BUILDDIR)/transport $(BUILDDIR)/binary $(BUILDDIR)/platform:
	@mkdir -p $@

# ---------- Firmware bin-to-C rules ----------

$(BUILDDIR)/firmware_dc_serial_data.c: $(DC_SER_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_dc_serial $@

$(BUILDDIR)/firmware_dc_ip_data.c: $(DC_IP_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_dc_ip $@

$(BUILDDIR)/firmware_gc_serial_data.c: $(GC_SER_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_gc_serial $@

$(BUILDDIR)/firmware_gc_ip_data.c: $(GC_IP_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_gc_ip $@

$(BUILDDIR)/firmware_ps2_ip_data.c: $(PS2_IP_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_ps2_ip $@

$(BUILDDIR)/firmware_wii_ip_data.c: $(WII_IP_BIN) $(BIN2C) | $(BUILDDIR)
	@echo "  BIN2C   $<"
	@$(BIN2C) $< firmware_wii_ip $@

$(BUILDDIR)/firmware_%_data.o: $(BUILDDIR)/firmware_%_data.c
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C $(MINILZO_DIR) BUILDDIR=build-host clean

# Auto-generated header deps (-MMD).  `-include` (leading dash) is silent on
# missing files, so the first build after `clean` works without any .d files.
-include $(OBJS:.o=.d) $(FW_OBJS:.o=.d)
