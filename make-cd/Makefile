# make-cd/Makefile — Build disc images for Dreamcast (CDI) and GameCube (ISO/DOL)
#
# Dreamcast: Assembles a 1st_read bootstrap that embeds the kosload firmware,
#            objcopies it to a boot binary, then creates a repo-local CDI.
#
# GameCube:  Assembles a bootstrap that embeds the kosload firmware,
#            converts to DOL, then creates a repo-local ISO image.
#            The bootstrap loads at 0x80003100 (valid for apploader)
#            and copies the firmware to GC_LOADER_BASE at top of MEM1.
#
# Usage:
#   make dc          Build Dreamcast 1st_read ELFs/BINs + CDIs
#   make gc          Build GameCube DOLs + ISOs
#   make all         Build both
#   make clean       Remove all build artifacts

ROOT ?= $(CURDIR)/..

include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

UNAME_S := $(shell uname -s)
HOST_CC_DEFAULT := cc

ifneq (,$(findstring MINGW,$(UNAME_S)))
  HOST_CC_DEFAULT := gcc
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  HOST_CC_DEFAULT := gcc
else ifneq (,$(findstring CYGWIN,$(UNAME_S)))
  HOST_CC_DEFAULT := gcc
endif

# ---------- Output directory (same as main build) ----------

BUILDDIR := $(ROOT)/build

# ---------- Dreamcast toolchain ----------

DC_PREFIX    := sh-elf-
DC_CC        := $(DC_TOOLCHAIN)/$(DC_PREFIX)gcc
DC_OBJCOPY   := $(DC_TOOLCHAIN)/$(DC_PREFIX)objcopy

DC_ASFLAGS := -ml -m4-single-only

# ---------- GameCube toolchain ----------

GC_PREFIX    := powerpc-eabi-
GC_CC        := $(GC_TOOLCHAIN)/$(GC_PREFIX)gcc

GC_ASFLAGS := -mcpu=750 -meabi -Wa,-mregnames

# ---------- Host toolchain ----------

HOSTCC ?= $(HOST_CC_DEFAULT)
HOST_CFLAGS ?= -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra

# GC loader memory layout (from mk/memory.mk)
GC_LOADER_BASE := $(shell printf '0x%08x' $$(($(GC_MEM1_TOP) - $(GC_LOADER_SIZE))))

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
DC_SER_1ST_BIN  := $(LOCALDIR)/1st_read-serial.bin
DC_IP_1ST_BIN   := $(LOCALDIR)/1st_read-ip.bin

GC_SER_BOOTSTRAP := $(LOCALDIR)/gc-bootstrap-serial.elf
GC_IP_BOOTSTRAP  := $(LOCALDIR)/gc-bootstrap-ip.elf

ELF2DOL := $(LOCALDIR)/elf2dol
MKISO_LITE := $(LOCALDIR)/mkiso-lite
MKCDI_LITE := $(LOCALDIR)/mkcdi-lite
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
.PHONY: all dc gc dc-serial dc-ip gc-serial gc-ip gc-dol clean

all: dc gc

dc: dc-serial dc-ip

gc: gc-serial gc-ip

gc-dol: $(GC_SER_DOL) $(GC_IP_DOL)

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
# Dreamcast: raw boot binary + CDI creation
# ============================================================

$(DC_SER_1ST_BIN): $(DC_SER_1ST_READ) | $(LOCALDIR)
	@echo "  OBJCOPY $@"
	@$(DC_OBJCOPY) -O binary -j .text -j .rodata $< $@

$(DC_IP_1ST_BIN): $(DC_IP_1ST_READ) | $(LOCALDIR)
	@echo "  OBJCOPY $@"
	@$(DC_OBJCOPY) -O binary -j .text -j .rodata $< $@

dc-serial: $(MKCDI_LITE) $(DC_SER_1ST_READ) $(DC_SER_1ST_BIN) | $(BUILDDIR)
	@echo "  1ST_READ $(DC_SER_1ST_READ)"
	@echo "  CDI     $(DC_SER_CDI)"
	@$(MKCDI_LITE) --input-bin $(DC_SER_1ST_BIN) --output $(DC_SER_CDI) --name "kosload-serial"

dc-ip: $(MKCDI_LITE) $(DC_IP_1ST_READ) $(DC_IP_1ST_BIN) | $(BUILDDIR)
	@echo "  1ST_READ $(DC_IP_1ST_READ)"
	@echo "  CDI     $(DC_IP_CDI)"
	@$(MKCDI_LITE) --input-bin $(DC_IP_1ST_BIN) --output $(DC_IP_CDI) --name "kosload-ip"

# ============================================================
# GameCube: elf2dol host tool
# ============================================================

$(ELF2DOL): tools/elf2dol.c | $(LOCALDIR)
	@echo "  HOSTCC  $@"
	@$(HOSTCC) $(HOST_CFLAGS) -I$(ROOT)/include -o $@ $<

$(MKISO_LITE): tools/mkiso-lite.c tools/iso9660-lite.c tools/iso9660-lite.h tools/time-compat.h $(ROOT)/include/kosload/strutil.h | $(LOCALDIR)
	@echo "  HOSTCC  $@"
	@$(HOSTCC) $(HOST_CFLAGS) -I$(ROOT)/include -o $@ \
	    tools/mkiso-lite.c tools/iso9660-lite.c

$(MKCDI_LITE): tools/mkcdi-lite.c tools/iso9660-lite.c tools/iso9660-lite.h tools/time-compat.h tools/IP.BIN $(ROOT)/include/kosload/strutil.h | $(LOCALDIR)
	@echo "  HOSTCC  $@"
	@$(HOSTCC) $(HOST_CFLAGS) -I$(ROOT)/include \
	    -DMKCDI_LITE_IP_TEMPLATE=\"$(CURDIR)/tools/IP.BIN\" \
	    -o $@ tools/mkcdi-lite.c tools/iso9660-lite.c

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
# GameCube: ISO creation
#
# Uses gbi.hdr (GameCube Boot Image header with El Torito
# apploader) at sector 0 so the GameCube IPL recognizes the disc.
# The DOL is staged as bootldr.dol in a temporary directory
# and referenced via El Torito boot catalog (-b flag).
# ============================================================

$(LOCALDIR)/gc-serial-disc $(LOCALDIR)/gc-ip-disc:
	@mkdir -p $@

# Stage DOLs as bootldr.dol for mkiso-lite
$(LOCALDIR)/gc-serial-disc/bootldr.dol: $(GC_SER_DOL) | $(LOCALDIR)/gc-serial-disc
	@cp $< $@

$(LOCALDIR)/gc-ip-disc/bootldr.dol: $(GC_IP_DOL) | $(LOCALDIR)/gc-ip-disc
	@cp $< $@

gc-serial: $(MKISO_LITE) $(LOCALDIR)/gc-serial-disc/bootldr.dol $(GBI_HDR) | $(BUILDDIR)
	@echo "  ISO     $(GC_SER_ISO)"
	@$(MKISO_LITE) --quiet --output $(GC_SER_ISO) \
	    --source-dir $(LOCALDIR)/gc-serial-disc \
	    --system-area $(GBI_HDR) --eltorito-boot bootldr.dol \
	    --volume-id KOSLOAD_SER

gc-ip: $(MKISO_LITE) $(LOCALDIR)/gc-ip-disc/bootldr.dol $(GBI_HDR) | $(BUILDDIR)
	@echo "  ISO     $(GC_IP_ISO)"
	@$(MKISO_LITE) --quiet --output $(GC_IP_ISO) \
	    --source-dir $(LOCALDIR)/gc-ip-disc \
	    --system-area $(GBI_HDR) --eltorito-boot bootldr.dol \
	    --volume-id KOSLOAD_IP

# ============================================================
# Clean
# ============================================================

clean:
	rm -rf $(LOCALDIR)
	rm -f $(DC_SER_CDI) $(DC_IP_CDI)
	rm -f $(GC_SER_DOL) $(GC_IP_DOL)
	rm -f $(GC_SER_ISO) $(GC_IP_ISO)
