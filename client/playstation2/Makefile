# client/playstation2/Makefile — builds PS2 IP loader

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

# ---------- EE Toolchain ----------

ifeq ($(wildcard $(PS2_EE_TOOLCHAIN)/mips64r5900el-ps2-elf-gcc),)
PS2_EE_TOOLCHAIN := /opt/toolchains/dc_ps2/mips64r5900el-ps2-elf/bin
endif

PREFIX  := mips64r5900el-ps2-elf-
CC      := $(PS2_EE_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(PS2_EE_TOOLCHAIN)/$(PREFIX)ar
SIZE    := $(PS2_EE_TOOLCHAIN)/$(PREFIX)size

# ---------- IOP Toolchain (MIPS R3000A, little-endian) ----------

IOP_TOOLCHAIN := $(PS2_IOP_TOOLCHAIN)
IOP_PREFIX    ?= mipsel-elf-
IOP_CC        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)gcc
IOP_NM        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)nm
IOP_OBJCOPY   := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)objcopy
IOP_STRIP     := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)strip
IOP_ABI_FLAGS :=
ELF2IRX       := $(ROOT)/host/tools/elf2irx/elf2irx

ifeq ($(wildcard $(IOP_CC)),)
IOP_TOOLCHAIN := /usr/local/ps2dev/iop/bin
IOP_PREFIX    := mipsel-none-elf-
IOP_CC        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)gcc
IOP_NM        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)nm
IOP_OBJCOPY   := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)objcopy
IOP_STRIP     := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)strip
endif

ifeq ($(wildcard $(IOP_CC)),)
IOP_TOOLCHAIN := /opt/toolchains/ps2dev/iop/bin
IOP_PREFIX    := mipsel-ps2-elf-
IOP_CC        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)gcc
IOP_NM        := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)nm
IOP_OBJCOPY   := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)objcopy
IOP_STRIP     := $(IOP_TOOLCHAIN)/$(IOP_PREFIX)strip
endif

ifeq ($(wildcard $(IOP_CC)),)
IOP_TOOLCHAIN := $(PS2_EE_TOOLCHAIN)
IOP_PREFIX    := $(PREFIX)
IOP_CC        := $(CC)
IOP_NM        := $(PS2_EE_TOOLCHAIN)/$(PREFIX)nm
IOP_OBJCOPY   := $(PS2_EE_TOOLCHAIN)/$(PREFIX)objcopy
IOP_STRIP     := $(PS2_EE_TOOLCHAIN)/$(PREFIX)strip
IOP_ABI_FLAGS := -mabi=32
endif

# ---------- Directories ----------

BUILDDIR    := build
COMMONDIR   := $(BUILDDIR)/common
IPDIR       := $(BUILDDIR)/ip
IOPDIR      := $(BUILDDIR)/iop

# ---------- Architecture flags ----------

ARCH_CFLAGS  := -G 0 -march=r5900 -mhard-float -msingle-float -mno-llsc -mno-shared -mplt -O2 -fno-builtin -nostdlib -nostartfiles -Wall -Wextra
ARCH_ASFLAGS := -G 0 -march=r5900 -mno-llsc

# ---------- Freestanding flags ----------

FREE_CFLAGS := \
    -ffreestanding \
    -fno-zero-initialized-in-bss \
    -fno-common \
    -fomit-frame-pointer \
    -fno-strict-aliasing \
    -fno-unwind-tables \
    -fno-asynchronous-unwind-tables \
    -fno-exceptions \
    -fno-delete-null-pointer-checks \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-merge-constants \
    -fno-merge-all-constants \
    -ffunction-sections \
    -fdata-sections

INCLUDES := -I$(ROOT)/include -I$(CLIENTDIR)/include -I$(CURDIR) -I$(CURDIR)/net

# ---------- Memory layout defines (passed to C and ASM) ----------

MEMORY_DEFS := -DPS2_LOADER_BASE=$(PS2_LOADER_BASE) \
               -DPS2_INNER_LOADER_BASE=$(PS2_INNER_LOADER_BASE) \
               -DPS2_RAM_TOP=$(PS2_RAM_TOP) \
               -DPS2_LOADER_SIZE=$(PS2_LOADER_SIZE)

# ---------- Combined flags ----------

CFLAGS  := $(ARCH_CFLAGS) $(FREE_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\" -DKOSLOAD_GIT_REV=\"$(KOSLOAD_GIT_REV)\"
ASFLAGS := $(ARCH_ASFLAGS) $(INCLUDES) $(MEMORY_DEFS)

# Automatic header-dependency tracking.  -MMD writes a .d file alongside each
# .o listing every (non-system) header pulled in by the preprocessor; -MP adds
# phony targets for those headers so a renamed/deleted header doesn't break
# the build.  The .d files are then -include'd at the bottom, which makes
# `touch include/kosload/target.h` (or any shared header) reliably rebuild
# every TU that uses it.  Without this, editing a shared struct silently
# leaves half the binary stale (see feedback_target_ops_append_only).
CFLAGS  += -MMD -MP
ASFLAGS += -MMD -MP

# IP-additional includes
IP_INCLUDES := -I$(CURDIR)/net -I$(IOPDIR)

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

# ---------- Generated linker script (from .ld.in template) ----------

IP_LD     := $(IPDIR)/kosload-inner.ld
PROBE_LD  := $(IPDIR)/kosload-probe.ld

# Linker script for the real loader.  It is linked at the address where
# bootstrap_main.c will copy it and jump: PS2_INNER_LOADER_BASE
# (currently KSEG0 0x80000280).
$(IP_LD): kosload-ip.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@PS2_LOADER_BASE@/$(PS2_INNER_LOADER_BASE)/g' \
	     -e 's/@PS2_LOADER_SIZE@/$(PS2_INNER_LOADER_SIZE)/g' \
	     -e 's/@PS2_RAM_TOP@/$(PS2_RAM_TOP)/g' \
	     $< > $@
	@echo "  GEN     $@"

# Linker script for the outer ELF.  This one is linked at the address
# normal PS2 launchers use: PS2_LOADER_BASE = 0x00100000.
$(PROBE_LD): kosload-ip.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@PS2_LOADER_BASE@/$(PS2_LOADER_BASE)/g' \
	     -e 's/@PS2_LOADER_SIZE@/$(PS2_LOADER_SIZE)/g' \
	     -e 's/@PS2_RAM_TOP@/$(PS2_RAM_TOP)/g' \
	     $< > $@
	@echo "  GEN     $@"

# ============================================================
# Common library (kosload_common.a)
# ============================================================

COMMON_C_OBJS := \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o \
    $(COMMONDIR)/target.o \
    $(COMMONDIR)/video.o \
    $(COMMONDIR)/cache.o \
    $(COMMONDIR)/memfuncs.o

COMMON_S_OBJS := \
    $(COMMONDIR)/crt0.o \
    $(COMMONDIR)/go.o \
    $(COMMONDIR)/exception.o

COMMON_OBJS := $(COMMON_C_OBJS) $(COMMON_S_OBJS)
COMMON_A    := $(COMMONDIR)/kosload_common.a

# Common C object rules (by source directory)
$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/target.o: target.c ee_cop0.h $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/video.o: video.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/cache.o: cache.c cache.h ee_cop0.h $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/memfuncs.o: memfuncs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Common ASM object rules
$(COMMONDIR)/crt0.o: crt0.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/go.o: go.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/exception.o: exception.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMON_A): $(COMMON_OBJS)
	$(AR) rcs $@ $^

# ============================================================
# IOP DEV9 init IRX (built with IOP toolchain, embedded in EE binary)
# ============================================================

# IOP modules are relocated by the PS2 BIOS loader, which is simpler than
# a modern linker.  Two flags keep the generated code easy for it to fix up:
#   - -G 0 keeps small globals out of the special GP-relative area.
#   - -mno-explicit-relocs avoids some shared HI16 relocation patterns.
# elf2irx does an additional cleanup pass for modern GCC output.  Without
# these guards, different references to the same global can land at different
# runtime addresses and the IRX can hang with no useful error.
IOP_CFLAGS  := $(IOP_ABI_FLAGS) -march=r3000 -msoft-float -nostdlib -nostartfiles -ffreestanding -O2 -Wall -G 0 -mno-explicit-relocs -MMD -MP

# kosdev9 EEPROM bit-bang variant matrix.  Default 0 = baseline
# protocol (DIR=0xe0, no dummy-zero, fast udelay).  Set on the
# command line with `make ip KOSDEV9_VARIANT=N` to A/B test
# protocol tweaks against hardware:
#   0  baseline
#   1  +DIR=0xe1                 (Sony's bit 0)
#   2  +consume dummy zero       (extra clock between addr & data)
#   3  +4x slower udelay         (in case timing is off)
# See iop/kosdev9_irx.c near KOSDEV9_VARIANT for full description.
KOSDEV9_VARIANT ?= 0
IOP_CFLAGS += -DKOSDEV9_VARIANT=$(KOSDEV9_VARIANT)

KOSDEV9_IRX_ELF      := $(IOPDIR)/kosdev9_irx.elf
KOSDEV9_IRX_STRIPPED := $(IOPDIR)/kosdev9_irx.stripped.elf
KOSDEV9_IRX          := $(IOPDIR)/kosdev9.irx
KOSDEV9_IRX_HDR      := $(IOPDIR)/kosdev9_irx_bin.h

$(IOPDIR)/dev9_init.o: iop/dev9_init.c iop/dev9_init_shared.h | $(IOPDIR)
	@echo "  IOP-CC  $@"
	@$(IOP_CC) $(IOP_CFLAGS) -c $< -o $@

# Stamp file ensures the .o rebuilds when KOSDEV9_VARIANT changes.
# Each variant has a distinct stamp filename; switching variants
# leaves the new stamp missing, triggering this recipe which deletes
# the old .o (forcing recompile with the new -DKOSDEV9_VARIANT=N).
# Without the rm, mtime resolution can be too coarse to detect the
# stamp-file change, leaving the .o "up-to-date" with stale flags.
KOSDEV9_VARIANT_STAMP := $(IOPDIR)/.kosdev9_variant_$(KOSDEV9_VARIANT)

$(KOSDEV9_VARIANT_STAMP): | $(IOPDIR)
	@rm -f $(IOPDIR)/.kosdev9_variant_* $(IOPDIR)/kosdev9_irx.o 2>/dev/null
	@touch $@

$(IOPDIR)/kosdev9_irx.o: iop/kosdev9_irx.c iop/dev9_init.c iop/dev9_init_shared.h iop/kosload_iop.h $(KOSDEV9_VARIANT_STAMP) | $(IOPDIR)
	@echo "  IOP-CC  $@  [KOSDEV9_VARIANT=$(KOSDEV9_VARIANT)]"
	@$(IOP_CC) $(IOP_CFLAGS) -c $< -o $@

# Link with our own kosload_iop.ld; -r produces a relocatable object
# that elf2irx then converts to the final .irx.
$(KOSDEV9_IRX_ELF): $(IOPDIR)/kosdev9_irx.o $(KOSLOAD_IOP_LD)
	@echo "  IOP-LD  $@"
	@$(IOP_CC) $(IOP_ABI_FLAGS) -nostdlib -nostartfiles -Wl,-r \
	    -T $(KOSLOAD_IOP_LD) -Os -o $@ $(IOPDIR)/kosdev9_irx.o

$(KOSDEV9_IRX_STRIPPED): $(KOSDEV9_IRX_ELF)
	@echo "  IOP-STRIP $@"
	@$(IOP_STRIP) --strip-unneeded --remove-section=.pdr --remove-section=.comment \
	    --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes \
	    -o $@ $<

$(ELF2IRX): $(ROOT)/host/tools/elf2irx/elf2irx.c $(ROOT)/host/tools/elf2irx/Makefile
	@$(MAKE) -C $(ROOT)/host/tools/elf2irx

$(KOSDEV9_IRX): $(KOSDEV9_IRX_STRIPPED) $(ELF2IRX)
	@echo "  ELF2IRX $@"
	@$(ELF2IRX) $< $@

$(KOSDEV9_IRX_HDR): $(KOSDEV9_IRX)
	@echo "  GEN     $@"
	@xxd -i $< | \
	    sed 's/unsigned char .*\[\]/static const unsigned char kosdev9_irx_bin[]/' | \
	    sed 's/unsigned int .*/static const unsigned int kosdev9_irx_bin_len = '"$$(wc -c < $<)"';/' \
	    > $@

KOSLOAD_IOP_LD             := iop/kosload_iop.ld

# ============================================================
# SMAP IRX — Stage 1 skeleton.
# ============================================================

SMAP_IRX_OBJ          := $(IOPDIR)/smap_irx.o
SMAP_IRX_ELF          := $(IOPDIR)/smap_irx.elf
SMAP_IRX_STRIPPED_ELF := $(IOPDIR)/smap_irx.stripped.elf
SMAP_IRX              := $(IOPDIR)/smap.irx
SMAP_IRX_HDR          := $(IOPDIR)/smap_irx_bin.h
SMAP_IRX_CFLAGS       := $(IOP_CFLAGS)

$(SMAP_IRX_OBJ): iop/smap_irx.c iop/kosload_iop.h iop/smap_protocol.h | $(IOPDIR)
	@echo "  IOP-CC  $@"
	@$(IOP_CC) $(SMAP_IRX_CFLAGS) -c $< -o $@

$(SMAP_IRX_ELF): $(SMAP_IRX_OBJ) $(KOSLOAD_IOP_LD)
	@echo "  IOP-LD  $@"
	@$(IOP_CC) $(IOP_ABI_FLAGS) -nostdlib -nostartfiles -Wl,-r \
	    -T $(KOSLOAD_IOP_LD) -Os -o $@ $(SMAP_IRX_OBJ)

$(SMAP_IRX_STRIPPED_ELF): $(SMAP_IRX_ELF)
	@echo "  IOP-STRIP $@"
	@$(IOP_STRIP) --strip-unneeded --remove-section=.pdr --remove-section=.comment \
	    --remove-section=.mdebug.abi32 --remove-section=.gnu.attributes \
	    -o $@ $<

$(SMAP_IRX): $(SMAP_IRX_STRIPPED_ELF) $(ELF2IRX)
	@echo "  ELF2IRX $@"
	@$(ELF2IRX) $< $@

$(SMAP_IRX_HDR): $(SMAP_IRX)
	@echo "  GEN     $@"
	@xxd -i $< | \
	    sed 's/unsigned char .*\[\]/static const unsigned char smap_irx_bin[]/' | \
	    sed 's/unsigned int .*/static const unsigned int smap_irx_bin_len = '"$$(wc -c < $<)"';/' \
	    > $@

.PHONY: smap-irx
smap-irx: $(SMAP_IRX)

# ============================================================
# IP build (ps2-load-ip)
# ============================================================

IP_DEFS := -DLOADER_NAME=\"ps2-load-ip\" -DMAX_RAM_BYTES=33554432
ifdef STATICIP
IP_DEFS += -DKOSLOAD_IP=\"$(STATICIP)\"
endif
# SMAP_DIAG=1 enables on-screen IRX-side hot-diag dump (rx/tx counts,
# head/tail cursors, link state, last RPC op/result, TX-slot states)
# at y=174/198.  Off by default in production builds.
ifdef SMAP_DIAG
IP_DEFS += -DSMAP_DIAG
endif
IP_CFLAGS := $(CFLAGS) $(IP_INCLUDES)

PS2_IP_LDFLAGS := -nostdlib -Wl,-n -Wl,--gc-sections

EE_SIF_LIB_OBJ := $(IPDIR)/ee_sif.o

$(EE_SIF_LIB_OBJ): ee_sif.c ee_sif.h ps2_memory_map.h cache.h $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

.PHONY: ee-sif-lib
ee-sif-lib: $(EE_SIF_LIB_OBJ)

# IP object groups keep the network layering visible:
# - EE-side loader transport and common command handling stay shared
# - PS2-specific DEV9/SMAP plumbing lives under client/playstation2/
# - bootstrap code loads the IOP-side DEV9/SMAP IRXs before networking starts
# - the PS2 packet/DHCP stack remains grouped as platform networking code
IP_CLIENT_TRANSPORT_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/commands.o

IP_PLATFORM_NET_OBJS := \
    $(IPDIR)/adapter.o \
    $(IPDIR)/smap.o \
    $(IPDIR)/iop_dev9.o \
    $(IPDIR)/iop_smap.o

IP_BOOTSTRAP_OBJS := \
    $(IPDIR)/iop_bootstrap.o \
    $(EE_SIF_LIB_OBJ)

IP_NETWORK_STACK_OBJS := \
    $(IPDIR)/net.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

IP_OBJS := \
    $(IP_CLIENT_TRANSPORT_OBJS) \
    $(IP_PLATFORM_NET_OBJS) \
    $(IP_BOOTSTRAP_OBJS) \
    $(IP_NETWORK_STACK_OBJS)

# Two-stage outputs:
#   IP_LOADER_IMAGE_ELF    -> inner loader, linked at PS2_INNER_LOADER_BASE
#   IP_LOADER_IMAGE_BIN    -> raw binary of the inner loader
#   IP_LOADER_BLOB_O       -> the .bin wrapped as an .o for linking
#   IP_BOOTSTRAP_MAIN_OBJ  -> bootstrap_main.c compiled
#   IP_ELF                 -> bootstrap_main + embedded blob (the deliverable)
IP_LOADER_IMAGE_ELF    := $(IPDIR)/ps2-load-ip-image.elf
IP_LOADER_IMAGE_BIN    := $(IPDIR)/ps2-load-ip-image.bin
IP_LOADER_BLOB_O       := $(IPDIR)/ps2-load-ip-image-blob.o
IP_BOOTSTRAP_MAIN_OBJ  := $(IPDIR)/bootstrap_main.o
IP_ELF                 := $(IPDIR)/ps2-load-ip.elf
IP_BIN                 := $(IPDIR)/ps2-load-ip.bin

# IP C object rules
$(IPDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/commands.o: $(CLIENTDIR)/common/core/commands.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/entry.o: $(CLIENTDIR)/common/network/entry.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_transport.o: $(CLIENTDIR)/common/network/network_transport.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/common/network/network_syscalls.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/adapter.o: net/adapter.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/iop_dev9.o: iop_dev9.c iop_dev9.h bootstrap/iop_bootstrap.h $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/iop_smap.o: iop_smap.c iop_smap.h iop/smap_protocol.h bootstrap/iop_bootstrap.h $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/iop_bootstrap.o: bootstrap/iop_bootstrap.c bootstrap/iop_bootstrap.h ee_sif.h ps2_memory_map.h iop_dev9.h iop_smap.h iop/dev9_init_shared.h iop/smap_protocol.h $(KOSDEV9_IRX_HDR) $(SMAP_IRX_HDR) $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/smap.o: net/smap.c iop_dev9.h $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/net.o: $(CLIENTDIR)/common/network/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet.o: net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

# Stage 1: link the real loader at PS2_INNER_LOADER_BASE.
$(IP_LOADER_IMAGE_ELF): $(IP_OBJS) $(COMMON_A) $(IP_LD)
	$(CC) $(ARCH_CFLAGS) $(PS2_IP_LDFLAGS) -T $(IP_LD) \
	    $(IP_OBJS) -Wl,--start-group $(COMMON_A) -lgcc -Wl,--end-group \
	    -o $@

# Stage 1b: convert the inner loader ELF to a flat binary.
$(IP_LOADER_IMAGE_BIN): $(IP_LOADER_IMAGE_ELF)
	@$(PS2_EE_TOOLCHAIN)/$(PREFIX)objcopy -O binary $< $@
	@echo "  OBJCOPY $@"

# Stage 1c: wrap the flat binary in an .o so the outer ELF can embed it.
# bootstrap_main.c uses the _binary_* symbols from loader_blob.S to find
# the bytes at runtime.
$(IP_LOADER_BLOB_O): $(IP_LOADER_IMAGE_BIN) loader_blob.S
	$(CC) $(ARCH_CFLAGS) $(MEMORY_DEFS) -c loader_blob.S -o $@
	@echo "  BLOB    $@"

# Stage 2: compile bootstrap_main.c, the tiny outer program that copies
# the embedded inner loader to PS2_INNER_LOADER_BASE.
$(IP_BOOTSTRAP_MAIN_OBJ): bootstrap_main.c cache.h $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

# Stage 3: build the final launchable ELF.  PS2 launchers load this at
# 0x00100000.  It copies the embedded inner loader to physical 0x280 and
# jumps to the cached address PS2_INNER_LOADER_BASE (0x80000280).
$(IP_ELF): $(IP_BOOTSTRAP_MAIN_OBJ) $(IP_LOADER_BLOB_O) $(COMMON_A) $(PROBE_LD)
	$(CC) $(ARCH_CFLAGS) $(PS2_IP_LDFLAGS) -T $(PROBE_LD) \
	    $(IP_BOOTSTRAP_MAIN_OBJ) $(IP_LOADER_BLOB_O) \
	    -Wl,--start-group $(COMMON_A) -lgcc -Wl,--end-group \
	    -o $@

# Stage 4: flat binary for kos-tool firmware embedding.
$(IP_BIN): $(IP_ELF)
	@$(PS2_EE_TOOLCHAIN)/$(PREFIX)objcopy -O binary $< $@
	@echo "  OBJCOPY $@"

# ============================================================
# Examples (freestanding test programs)
# ============================================================

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

PS2_EXAMPLES := \
    console-test download-test exception-test \
    exit-test file-test gdb-test syscall-test ip-calibrator
# exception-frame-test is excluded — the host's frame parser
# (handle_dc_exception / handle_gc_exception in host/src/console.c) has
# no R5900 case yet, so a synthetic PS2 frame would fail to render.

PS2_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(PS2_EXAMPLES)))

# PS2_KOSLOAD_BASE points at the start of the *header* fields the
# example reads (magic at +4, syscall ptr at +8), NOT the actual
# entry-instruction at PS2_INNER_LOADER_BASE.  MIPS forces a delay
# slot after the leading `j realstart`, so the layout is:
#   PS2_INNER_LOADER_BASE+0: j realstart
#   PS2_INNER_LOADER_BASE+4: nop (delay slot)
#   PS2_INNER_LOADER_BASE+8: 0xdeadbeef magic
#   PS2_INNER_LOADER_BASE+12: kosloadsyscall ptr
# Pretending KOSLOAD_BASE is +4 from the real entry lets the shared
# DC/GC example pattern (BASE+4 = magic, BASE+8 = syscall) work
# unchanged on PS2 — BASE+4 → +8, BASE+8 → +12.
$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(EXDIR)/ps2-example.ld | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -DPS2_KOSLOAD_BASE=0x80000284 \
	    -T $(EXDIR)/ps2-example.ld $< -o $@

examples: $(PS2_EXAMPLE_ELFS)

# ============================================================
# Phony targets
# ============================================================

.PHONY: all ip examples clean

all: ip examples

ip: $(IP_ELF) $(IP_BIN)
	@$(SIZE) $(IP_ELF)

# Directory creation
$(COMMONDIR) $(IPDIR) $(IOPDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR) $(KOSDEV9_IRX_HDR)

# Auto-generated header deps (-MMD).  `-include` (leading dash) is silent on
# missing files, so the first build after `clean` works without any .d files.
# Covers EE-side .o (loader + bootstrap + ee_sif), IOP-side .o (DEV9 + SMAP
# IRXs), and shared common objects.  loader_blob.o is assembled from .S with
# no #include and is intentionally excluded.
IOP_DEP_OBJS := $(IOPDIR)/dev9_init.o $(IOPDIR)/kosdev9_irx.o $(SMAP_IRX_OBJ)
-include $(COMMON_OBJS:.o=.d) $(IP_OBJS:.o=.d) \
         $(IP_BOOTSTRAP_MAIN_OBJ:.o=.d) $(EE_SIF_LIB_OBJ:.o=.d) \
         $(IOP_DEP_OBJS:.o=.d)
