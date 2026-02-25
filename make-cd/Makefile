# make-cd/Makefile — Build disc images for Dreamcast (CDI) and GameCube (ISO/DOL)
#
# Dreamcast: Assembles a 1st_read bootstrap that embeds the kosload firmware,
#            then optionally creates a CDI image using mkdcdisc.
#
# GameCube:  Assembles a bootstrap that embeds the kosload firmware,
#            converts to DOL, then optionally creates an ISO image.
#            The bootstrap loads at 0x80003100 (valid for apploader)
#            and copies the firmware to GC_LOADER_BASE at top of MEM1.
#
# Usage:
#   make dc          Build Dreamcast 1st_read ELFs + CDIs (if mkdcdisc available)
#   make gc          Build GameCube DOLs + ISOs (if mkisofs available)
#   make all         Build both
#   make clean       Remove all build artifacts
#
# Tool configuration (override on command line or via environment):
#   MKDCDISC=<path>  Path to mkdcdisc (Dreamcast CDI creator)
#   MKISOFS=<path>   Path to mkisofs/genisoimage (ISO creator)

ROOT ?= $(CURDIR)/..

include $(ROOT)/mk/memory.mk

# ---------- Output directory (same as main build) ----------

BUILDDIR := $(ROOT)/build

# ---------- Dreamcast toolchain ----------

DC_TOOLCHAIN ?= /opt/toolchains/dc/sh-elf/bin
DC_PREFIX    := sh-elf-
DC_CC        := $(DC_TOOLCHAIN)/$(DC_PREFIX)gcc
DC_OBJCOPY   := $(DC_TOOLCHAIN)/$(DC_PREFIX)objcopy

DC_ASFLAGS := -ml -m4-single-only

# ---------- GameCube toolchain ----------

GC_TOOLCHAIN ?= /opt/toolchains/gc/powerpc-eabi/bin
GC_PREFIX    := powerpc-eabi-
GC_CC        := $(GC_TOOLCHAIN)/$(GC_PREFIX)gcc

GC_ASFLAGS := -mcpu=750 -meabi -Wa,-mregnames

# GC loader memory layout (from mk/memory.mk)
GC_LOADER_BASE := $(shell printf '0x%08x' $$(($(GC_MEM1_TOP) - $(GC_LOADER_SIZE))))

# ---------- External tool auto-detection ----------
# Users can override these by setting MKDCDISC= or MKISOFS= on the command line.
# When empty, disc images are skipped and the needed command is printed instead.

MKDCDISC ?= $(shell command -v mkdcdisc 2>/dev/null || \
             (test -x /opt/toolchains/dc-utils/mkdcdisc && echo /opt/toolchains/dc-utils/mkdcdisc) || \
             (test -x /opt/toolchains/dc/bin/mkdcdisc && echo /opt/toolchains/dc/bin/mkdcdisc))

MKISOFS ?= $(shell command -v mkisofs 2>/dev/null || command -v genisoimage 2>/dev/null)

# ---------- Local build directory ----------

LOCALDIR := build

# ---------- Source paths ----------

DC_EXCEPTION_S := $(ROOT)/client/dreamcast/exception.S
DC_LOADER_S    := dreamcast/loader.S
DC_DISABLE_S   := dreamcast/disable.S

GC_LOADER_S    := gamecube/loader.S
GC_LOADER_LD   := gamecube/loader.ld

# ---------- Firmware inputs (from main build) ----------

DC_SER_BIN := $(BUILDDIR)/dc-load-ser.bin
DC_IP_BIN  := $(BUILDDIR)/dc-load-ip.bin
GC_SER_BIN := $(BUILDDIR)/gc-load-ser.bin
GC_IP_BIN  := $(BUILDDIR)/gc-load-ip.bin

# ---------- Intermediate outputs ----------

DC_EXCEPTION_BIN := $(LOCALDIR)/exception.bin

DC_SER_1ST_READ := $(LOCALDIR)/1st_read-serial.elf
DC_IP_1ST_READ  := $(LOCALDIR)/1st_read-ip.elf

GC_SER_BOOTSTRAP := $(LOCALDIR)/gc-bootstrap-serial.elf
GC_IP_BOOTSTRAP  := $(LOCALDIR)/gc-bootstrap-ip.elf

ELF2DOL := $(LOCALDIR)/elf2dol
GBI_HDR := tools/gbi.hdr

# ---------- Final outputs ----------

DC_SER_CDI := $(BUILDDIR)/dc-load-ser.cdi
DC_IP_CDI  := $(BUILDDIR)/dc-load-ip.cdi

GC_SER_DOL := $(BUILDDIR)/gc-load-ser.dol
GC_IP_DOL  := $(BUILDDIR)/gc-load-ip.dol
GC_SER_ISO := $(BUILDDIR)/gc-load-ser.iso
GC_IP_ISO  := $(BUILDDIR)/gc-load-ip.iso

# ============================================================
# Phony targets
# ============================================================

.DEFAULT_GOAL := all
.PHONY: all dc gc dc-serial dc-ip gc-serial gc-ip clean

all: dc gc

dc: dc-serial dc-ip

gc: gc-serial gc-ip

# ============================================================
# Directory creation
# ============================================================

$(LOCALDIR) $(BUILDDIR):
	@mkdir -p $@

# ============================================================
# Dreamcast: exception.bin (from client/dreamcast/exception.S)
# ============================================================

$(LOCALDIR)/exception.o: $(DC_EXCEPTION_S) | $(LOCALDIR)
	@echo "  AS      $@"
	@$(DC_CC) $(DC_ASFLAGS) -c $< -o $@

$(DC_EXCEPTION_BIN): $(LOCALDIR)/exception.o
	@echo "  OBJCOPY $@"
	@$(DC_OBJCOPY) -O binary -j .text $< $@

# ============================================================
# Dreamcast: 1st_read bootstrap (embeds firmware + exception handler)
# ============================================================
#
# loader.S uses .incbin to pull in "kosload.bin" and "exception.bin"
# from the assembler include path (-Wa,-I<dir>).
#
# We stage these files in per-variant directories so both serial
# and IP variants can be built without filename conflicts.

$(LOCALDIR)/dc-serial $(LOCALDIR)/dc-ip:
	@mkdir -p $@

# Stage kosload.bin for each variant
$(LOCALDIR)/dc-serial/kosload.bin: $(DC_SER_BIN) | $(LOCALDIR)/dc-serial
	@cp $< $@

$(LOCALDIR)/dc-ip/kosload.bin: $(DC_IP_BIN) | $(LOCALDIR)/dc-ip
	@cp $< $@

# Stage exception.bin (same for both variants)
$(LOCALDIR)/dc-serial/exception.bin: $(DC_EXCEPTION_BIN) | $(LOCALDIR)/dc-serial
	@cp $< $@

$(LOCALDIR)/dc-ip/exception.bin: $(DC_EXCEPTION_BIN) | $(LOCALDIR)/dc-ip
	@cp $< $@

# Assemble 1st_read bootstrap
$(DC_SER_1ST_READ): $(DC_LOADER_S) $(DC_DISABLE_S) $(LOCALDIR)/dc-serial/kosload.bin $(LOCALDIR)/dc-serial/exception.bin
	@echo "  AS      $@ (serial)"
	@$(DC_CC) $(DC_ASFLAGS) -nostartfiles -nostdlib -Ttext=0x8c010000 \
	    -Wa,-I$(LOCALDIR)/dc-serial \
	    $(DC_LOADER_S) $(DC_DISABLE_S) -o $@

$(DC_IP_1ST_READ): $(DC_LOADER_S) $(DC_DISABLE_S) $(LOCALDIR)/dc-ip/kosload.bin $(LOCALDIR)/dc-ip/exception.bin
	@echo "  AS      $@ (ip)"
	@$(DC_CC) $(DC_ASFLAGS) -nostartfiles -nostdlib -Ttext=0x8c010000 \
	    -Wa,-I$(LOCALDIR)/dc-ip \
	    $(DC_LOADER_S) $(DC_DISABLE_S) -o $@

# ============================================================
# Dreamcast: CDI creation (requires mkdcdisc)
# ============================================================

dc-serial: $(DC_SER_1ST_READ) | $(BUILDDIR)
	@echo "  1ST_READ $(DC_SER_1ST_READ)"
ifneq ($(MKDCDISC),)
	@echo "  CDI     $(DC_SER_CDI)"
	@$(MKDCDISC) -e $< -o $(DC_SER_CDI) -N -n "kosload-serial"
else
	@echo ""
	@echo "  mkdcdisc not found — skipping CDI creation."
	@echo "  Recommended: https://gitlab.com/simulant/mkdcdisc"
	@echo ""
	@echo "  To create the CDI manually:"
	@echo "    mkdcdisc -e $(DC_SER_1ST_READ) -o $(DC_SER_CDI) -N -n \"kosload-serial\""
	@echo ""
endif

dc-ip: $(DC_IP_1ST_READ) | $(BUILDDIR)
	@echo "  1ST_READ $(DC_IP_1ST_READ)"
ifneq ($(MKDCDISC),)
	@echo "  CDI     $(DC_IP_CDI)"
	@$(MKDCDISC) -e $< -o $(DC_IP_CDI) -N -n "kosload-ip"
else
	@echo ""
	@echo "  mkdcdisc not found — skipping CDI creation."
	@echo "  Recommended: https://gitlab.com/simulant/mkdcdisc"
	@echo ""
	@echo "  To create the CDI manually:"
	@echo "    mkdcdisc -e $(DC_IP_1ST_READ) -o $(DC_IP_CDI) -N -n \"kosload-ip\""
	@echo ""
endif

# ============================================================
# GameCube: elf2dol host tool
# ============================================================

$(ELF2DOL): tools/elf2dol.c | $(LOCALDIR)
	@echo "  HOSTCC  $@"
	@cc -std=c11 -O2 -Wall -o $@ $<

# ============================================================
# GameCube: Bootstrap (embeds firmware, copies to high memory)
# ============================================================
#
# loader.S uses .incbin to pull in "kosload.bin" from the
# assembler include path.  The bootstrap loads at 0x80003100
# (standard GC entry, valid for apploader) and copies the
# embedded firmware to GC_LOADER_BASE at top of MEM1.
#
# Same staging pattern as the Dreamcast bootstrap.

$(LOCALDIR)/gc-serial $(LOCALDIR)/gc-ip:
	@mkdir -p $@

# Stage kosload.bin for each variant
$(LOCALDIR)/gc-serial/kosload.bin: $(GC_SER_BIN) | $(LOCALDIR)/gc-serial
	@cp $< $@

$(LOCALDIR)/gc-ip/kosload.bin: $(GC_IP_BIN) | $(LOCALDIR)/gc-ip
	@cp $< $@

# Assemble bootstrap
$(GC_SER_BOOTSTRAP): $(GC_LOADER_S) $(GC_LOADER_LD) $(LOCALDIR)/gc-serial/kosload.bin
	@echo "  AS      $@ (serial)"
	@$(GC_CC) $(GC_ASFLAGS) -DGC_LOADER_BASE=$(GC_LOADER_BASE) \
	    -nostartfiles -nostdlib -Wl,-n -T $(GC_LOADER_LD) \
	    -Wa,-I$(LOCALDIR)/gc-serial \
	    $(GC_LOADER_S) -o $@

$(GC_IP_BOOTSTRAP): $(GC_LOADER_S) $(GC_LOADER_LD) $(LOCALDIR)/gc-ip/kosload.bin
	@echo "  AS      $@ (ip)"
	@$(GC_CC) $(GC_ASFLAGS) -DGC_LOADER_BASE=$(GC_LOADER_BASE) \
	    -nostartfiles -nostdlib -Wl,-n -T $(GC_LOADER_LD) \
	    -Wa,-I$(LOCALDIR)/gc-ip \
	    $(GC_LOADER_S) -o $@

# ============================================================
# GameCube: DOL conversion (from bootstrap ELF)
# ============================================================

$(GC_SER_DOL): $(ELF2DOL) $(GC_SER_BOOTSTRAP) | $(BUILDDIR)
	@echo "  ELF2DOL $@"
	@$(ELF2DOL) $(GC_SER_BOOTSTRAP) $@

$(GC_IP_DOL): $(ELF2DOL) $(GC_IP_BOOTSTRAP) | $(BUILDDIR)
	@echo "  ELF2DOL $@"
	@$(ELF2DOL) $(GC_IP_BOOTSTRAP) $@

# ============================================================
# GameCube: ISO creation (requires mkisofs/genisoimage)
#
# Uses gbi.hdr (GameCube Boot Image header with El Torito
# apploader) at sector 0 so the GameCube IPL recognizes the disc.
# The DOL is staged as bootldr.dol in a temporary directory
# and referenced via El Torito boot catalog (-b flag).
# ============================================================

$(LOCALDIR)/gc-serial-disc $(LOCALDIR)/gc-ip-disc:
	@mkdir -p $@

# Stage DOLs as bootldr.dol for mkisofs
$(LOCALDIR)/gc-serial-disc/bootldr.dol: $(GC_SER_DOL) | $(LOCALDIR)/gc-serial-disc
	@cp $< $@

$(LOCALDIR)/gc-ip-disc/bootldr.dol: $(GC_IP_DOL) | $(LOCALDIR)/gc-ip-disc
	@cp $< $@

gc-serial: $(LOCALDIR)/gc-serial-disc/bootldr.dol $(GBI_HDR) | $(BUILDDIR)
ifneq ($(MKISOFS),)
	@echo "  ISO     $(GC_SER_ISO)"
	@$(MKISOFS) -quiet -R -J -G $(GBI_HDR) -no-emul-boot \
	    -b bootldr.dol -o $(GC_SER_ISO) $(LOCALDIR)/gc-serial-disc/
else
	@echo ""
	@echo "  mkisofs not found — skipping ISO creation."
	@echo "  Install: brew install cdrtools  (macOS)"
	@echo "           apt install genisoimage  (Debian/Ubuntu)"
	@echo ""
	@echo "  To create the ISO manually:"
	@echo "    mkisofs -R -J -G $(GBI_HDR) -no-emul-boot -b bootldr.dol -o $(GC_SER_ISO) <dir-with-dol>/"
	@echo ""
endif

gc-ip: $(LOCALDIR)/gc-ip-disc/bootldr.dol $(GBI_HDR) | $(BUILDDIR)
ifneq ($(MKISOFS),)
	@echo "  ISO     $(GC_IP_ISO)"
	@$(MKISOFS) -quiet -R -J -G $(GBI_HDR) -no-emul-boot \
	    -b bootldr.dol -o $(GC_IP_ISO) $(LOCALDIR)/gc-ip-disc/
else
	@echo ""
	@echo "  mkisofs not found — skipping ISO creation."
	@echo "  Install: brew install cdrtools  (macOS)"
	@echo "           apt install genisoimage  (Debian/Ubuntu)"
	@echo ""
	@echo "  To create the ISO manually:"
	@echo "    mkisofs -R -J -G $(GBI_HDR) -no-emul-boot -b bootldr.dol -o $(GC_IP_ISO) <dir-with-dol>/"
	@echo ""
endif

# ============================================================
# Clean
# ============================================================

clean:
	rm -rf $(LOCALDIR)
	rm -f $(DC_SER_CDI) $(DC_IP_CDI)
	rm -f $(GC_SER_DOL) $(GC_IP_DOL)
	rm -f $(GC_SER_ISO) $(GC_IP_ISO)
