# Makefile — kosload top-level build dispatcher
#
# Usage:
#   make all           Build host tool + DC + GC + Wii + PS2 firmware
#   make dc            Build Dreamcast firmware
#   make gc            Build GameCube firmware
#   make wii           Build Wii firmware
#   make ps2           Build PlayStation 2 firmware
#   make dist          Build all delivery artifacts (CDI + ISO + Wii channel WAD + PS2 ISO)
#   make dist-dc       Build Dreamcast CDI images
#   make dist-gc       Build GameCube ISO images
#   make dist-wii      Build Wii channel WAD
#   make dist-ps2      Build PlayStation 2 ISO
#   make gc-dol        Build GameCube DOL files only (no ISO)
#   make release       Build everything + package release archives (build/release/)
#   make release-host  Package the host-tool archive for this OS
#   make release-firmware  Package the firmware bundle
#   make clean         Remove all build artifacts

ROOT := $(CURDIR)
include mk/version.mk
include mk/toolchains.mk

# Default target
.DEFAULT_GOAL := all

# ---------- version.h generation ----------

VERSION_H    := $(ROOT)/include/kosload/version.h
VERSION_H_IN := $(ROOT)/include/kosload/version.h.in

# Uses a PID-unique temp so concurrent sub-makes (e.g. `make all -j` building
# several consoles at once) don't clobber each other's $@.tmp and fail the mv.
$(VERSION_H): $(VERSION_H_IN) mk/version.mk
	@t=$@.tmp.$$$$; \
	 sed -e 's/@KOSLOAD_VERSION_MAJOR@/$(KOSLOAD_VERSION_MAJOR)/g' \
	     -e 's/@KOSLOAD_VERSION_MINOR@/$(KOSLOAD_VERSION_MINOR)/g' \
	     -e 's/@KOSLOAD_VERSION_PATCH@/$(KOSLOAD_VERSION_PATCH)/g' \
	     -e 's/@KOSLOAD_VERSION@/$(KOSLOAD_VERSION)/g' \
	     $< > $$t; \
	 if ! cmp -s $$t $@ 2>/dev/null; then mv $$t $@; echo "  GEN     $@"; else rm -f $$t; fi

# ---------- Output directory ----------

BUILDDIR := build

$(BUILDDIR):
	@mkdir -p $@

# ---------- Wii channel WAD ----------
# `make dist-wii` packs the kosload Wii client into an installable channel WAD
# via make-dist's `wii` target (and is rolled into `make dist`).  The WAD defaults
# (title-id, IOS, name, title-ver) live in make-dist/Makefile; override on the
# command line, e.g.
#   make dist-wii WII_WAD_TITLE_ID=KOSL WII_WAD_IOS=58 WII_WAD_NAME="wii-load-ip"
# Bump WII_WAD_TITLE_VER to force the System Menu to overwrite an installed
# copy of the same title-id (e.g. `make dist-wii WII_WAD_TITLE_VER=2`).

# ---------- Toolchain checks ----------
# Tool prefixes (DC_PREFIX, GC_PREFIX, PS2_PREFIX, PS2_IOP_PREFIX) and bindirs
# come from mk/toolchains.mk (included above).

DC_CC        := $(DC_TOOLCHAIN)/$(DC_PREFIX)gcc
DC_AR        := $(DC_TOOLCHAIN)/$(DC_PREFIX)ar
DC_OBJCOPY   := $(DC_TOOLCHAIN)/$(DC_PREFIX)objcopy
DC_SIZE      := $(DC_TOOLCHAIN)/$(DC_PREFIX)size

GC_CC        := $(GC_TOOLCHAIN)/$(GC_PREFIX)gcc
GC_AR        := $(GC_TOOLCHAIN)/$(GC_PREFIX)ar
GC_OBJCOPY   := $(GC_TOOLCHAIN)/$(GC_PREFIX)objcopy
GC_SIZE      := $(GC_TOOLCHAIN)/$(GC_PREFIX)size

PS2_CC       := $(PS2_EE_TOOLCHAIN)/$(PS2_PREFIX)gcc
PS2_AR       := $(PS2_EE_TOOLCHAIN)/$(PS2_PREFIX)ar
PS2_OBJCOPY  := $(PS2_EE_TOOLCHAIN)/$(PS2_PREFIX)objcopy
PS2_SIZE     := $(PS2_EE_TOOLCHAIN)/$(PS2_PREFIX)size

PS2_IOP_CC        := $(PS2_IOP_TOOLCHAIN)/$(PS2_IOP_PREFIX)gcc

define require_host_tool
	@if [ ! -x "$(1)" ] && [ ! -x "$(1).exe" ]; then \
		echo "Error: missing $(2): $(1)"; \
		echo "Hint: install the $(3) toolchain or pass $(4)=<toolchain-bin-dir>."; \
		exit 1; \
	fi
endef

define has_host_tool
( [ -x "$(1)" ] || [ -x "$(1).exe" ] )
endef

# ---------- Targets ----------

.PHONY: all host dc gc wii ps2 dist dist-dc dist-gc dist-wii dist-ps2 gc-dol \
        auto-dc auto-gc auto-wii auto-ps2 \
        dist-auto-dc dist-auto-gc dist-auto-wii dist-auto-ps2 \
        release release-host release-firmware print-version clean \
        check-dc-toolchain check-gc-toolchain check-wii-toolchain check-ps2-toolchain

# Single source of truth for the version string (e.g. for release tooling/CI).
# `make -s print-version` -> 3.0.0
print-version:
	@echo $(KOSLOAD_VERSION)

check-dc-toolchain:
	$(call require_host_tool,$(DC_CC),Dreamcast compiler (sh-elf-gcc),Dreamcast,DC_TOOLCHAIN)
	$(call require_host_tool,$(DC_AR),Dreamcast archiver (sh-elf-ar),Dreamcast,DC_TOOLCHAIN)
	$(call require_host_tool,$(DC_OBJCOPY),Dreamcast objcopy (sh-elf-objcopy),Dreamcast,DC_TOOLCHAIN)
	$(call require_host_tool,$(DC_SIZE),Dreamcast size tool (sh-elf-size),Dreamcast,DC_TOOLCHAIN)

check-gc-toolchain:
	$(call require_host_tool,$(GC_CC),GameCube compiler (powerpc-eabi-gcc),GameCube,GC_TOOLCHAIN)
	$(call require_host_tool,$(GC_AR),GameCube archiver (powerpc-eabi-ar),GameCube,GC_TOOLCHAIN)
	$(call require_host_tool,$(GC_OBJCOPY),GameCube objcopy (powerpc-eabi-objcopy),GameCube,GC_TOOLCHAIN)
	$(call require_host_tool,$(GC_SIZE),GameCube size tool (powerpc-eabi-size),GameCube,GC_TOOLCHAIN)

check-wii-toolchain: check-gc-toolchain

check-ps2-toolchain:
	$(call require_host_tool,$(PS2_CC),PS2 EE compiler (mips64r5900el-ps2-elf-gcc),PlayStation 2,PS2_EE_TOOLCHAIN)
	$(call require_host_tool,$(PS2_IOP_CC),PS2 IOP compiler (mipsel-elf-gcc),PlayStation 2,PS2_IOP_TOOLCHAIN)

# `make all` builds every console whose cross-toolchain is installed, skipping
# the rest with a SKIP notice, then always builds the host tool.  Use the
# explicit per-console targets (make dc/gc/wii/ps2) for a hard error when a
# toolchain is missing.  Real compile failures still abort — only a missing
# toolchain is skipped.
all: auto-dc auto-gc auto-wii auto-ps2 host

# Skip-if-missing wrappers (mirror dist-auto-*).  Wii uses the GameCube
# (powerpc-eabi) toolchain; PS2 needs both the EE and IOP compilers.
auto-dc:
	@if $(call has_host_tool,$(DC_CC)) && \
	    $(call has_host_tool,$(DC_AR)) && \
	    $(call has_host_tool,$(DC_OBJCOPY)) && \
	    $(call has_host_tool,$(DC_SIZE)); then \
		$(MAKE) dc; \
	else \
		echo "  SKIP    Dreamcast firmware (toolchain not found)"; \
	fi

auto-gc:
	@if $(call has_host_tool,$(GC_CC)) && \
	    $(call has_host_tool,$(GC_AR)) && \
	    $(call has_host_tool,$(GC_OBJCOPY)) && \
	    $(call has_host_tool,$(GC_SIZE)); then \
		$(MAKE) gc; \
	else \
		echo "  SKIP    GameCube firmware (toolchain not found)"; \
	fi

auto-wii:
	@if $(call has_host_tool,$(GC_CC)) && \
	    $(call has_host_tool,$(GC_AR)) && \
	    $(call has_host_tool,$(GC_OBJCOPY)) && \
	    $(call has_host_tool,$(GC_SIZE)); then \
		$(MAKE) wii; \
	else \
		echo "  SKIP    Wii firmware (toolchain not found)"; \
	fi

auto-ps2:
	@if $(call has_host_tool,$(PS2_CC)) && \
	    $(call has_host_tool,$(PS2_IOP_CC)); then \
		$(MAKE) ps2; \
	else \
		echo "  SKIP    PlayStation 2 firmware (toolchain not found)"; \
	fi

host: $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C host ROOT=$(ROOT)
	@cp host/build/kos-tool $(BUILDDIR)/ 2>/dev/null || cp host/build/kos-tool.exe $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/kos-tool"

dc: check-dc-toolchain $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client/dreamcast ROOT=$(ROOT) all
	@cp client/dreamcast/build/serial/dc-load-ser.elf $(BUILDDIR)/
	@cp client/dreamcast/build/serial/dc-load-ser.bin $(BUILDDIR)/
	@cp client/dreamcast/build/ip/dc-load-ip.elf $(BUILDDIR)/
	@cp client/dreamcast/build/ip/dc-load-ip.bin $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/dc-load-ser.{elf,bin} dc-load-ip.{elf,bin}"
	@mkdir -p $(BUILDDIR)/examples/dc
	@cp client/dreamcast/build/examples/*.elf $(BUILDDIR)/examples/dc/
	@if ls client/dreamcast/build/examples/*.iso >/dev/null 2>&1; then \
	    cp client/dreamcast/build/examples/*.iso $(BUILDDIR)/examples/dc/; \
	fi
	@echo "  COPY    $(BUILDDIR)/examples/dc/"
	$(MAKE) host

gc: check-gc-toolchain $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client/gamecube ROOT=$(ROOT) all
	@cp client/gamecube/build/serial/gc-load-ser.elf $(BUILDDIR)/
	@cp client/gamecube/build/serial/gc-load-ser.bin $(BUILDDIR)/
	@cp client/gamecube/build/ip/gc-load-ip.elf $(BUILDDIR)/
	@cp client/gamecube/build/ip/gc-load-ip.bin $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/gc-load-ser.{elf,bin} gc-load-ip.{elf,bin}"
	@mkdir -p $(BUILDDIR)/examples/gc
	@cp client/gamecube/build/examples/*.elf $(BUILDDIR)/examples/gc/
	@echo "  COPY    $(BUILDDIR)/examples/gc/*.elf"
	$(MAKE) host

wii: check-wii-toolchain $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client/wii ROOT=$(ROOT) all
	@cp client/wii/build/ip/wii-load-ip.elf $(BUILDDIR)/
	@cp client/wii/build/ip/wii-load-ip.bin $(BUILDDIR)/
	@cp client/wii/build/ip/wii-load-ip.dol $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/wii-load-ip.{elf,bin,dol}"
	@mkdir -p $(BUILDDIR)/examples/wii
	@cp client/wii/build/examples/*.elf $(BUILDDIR)/examples/wii/
	@echo "  COPY    $(BUILDDIR)/examples/wii/*.elf"
	$(MAKE) host

ps2: check-ps2-toolchain $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client/playstation2 ROOT=$(ROOT) all
	@cp client/playstation2/build/ip/ps2-load-ip.elf $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/ps2-load-ip.elf"
	@mkdir -p $(BUILDDIR)/examples/ps2
	@cp client/playstation2/build/examples/*.elf $(BUILDDIR)/examples/ps2/
	@echo "  COPY    $(BUILDDIR)/examples/ps2/*.elf"
	$(MAKE) host

# ---------- Distribution artifact targets ----------

dist: dist-auto-dc dist-auto-gc dist-auto-wii dist-auto-ps2

dist-auto-dc:
	@if $(call has_host_tool,$(DC_CC)) && \
	    $(call has_host_tool,$(DC_AR)) && \
	    $(call has_host_tool,$(DC_OBJCOPY)) && \
	    $(call has_host_tool,$(DC_SIZE)); then \
		$(MAKE) dist-dc; \
	else \
		echo "  SKIP    Dreamcast disc images (toolchain not found)"; \
	fi

dist-auto-gc:
	@if $(call has_host_tool,$(GC_CC)) && \
	    $(call has_host_tool,$(GC_AR)) && \
	    $(call has_host_tool,$(GC_OBJCOPY)) && \
	    $(call has_host_tool,$(GC_SIZE)); then \
		$(MAKE) dist-gc; \
	else \
		echo "  SKIP    GameCube disc images (toolchain not found)"; \
	fi

# Wii uses the GameCube (powerpc-eabi) toolchain — same skip-if-missing logic.
dist-auto-wii:
	@if $(call has_host_tool,$(GC_CC)) && \
	    $(call has_host_tool,$(GC_AR)) && \
	    $(call has_host_tool,$(GC_OBJCOPY)) && \
	    $(call has_host_tool,$(GC_SIZE)); then \
		$(MAKE) dist-wii; \
	else \
		echo "  SKIP    Wii channel WAD (toolchain not found)"; \
	fi

# PS2 needs both the EE and IOP compilers (same gate as auto-ps2).
dist-auto-ps2:
	@if $(call has_host_tool,$(PS2_CC)) && \
	    $(call has_host_tool,$(PS2_IOP_CC)); then \
		$(MAKE) dist-ps2; \
	else \
		echo "  SKIP    PlayStation 2 ISO (toolchain not found)"; \
	fi

dist-dc: check-dc-toolchain dc
	$(MAKE) -C make-dist dc ROOT=$(ROOT)

dist-gc: check-gc-toolchain gc
	$(MAKE) -C make-dist gc ROOT=$(ROOT)

dist-wii: check-wii-toolchain wii
	$(MAKE) -C make-dist wii ROOT=$(ROOT)

dist-ps2: check-ps2-toolchain ps2
	$(MAKE) -C make-dist ps2 ROOT=$(ROOT)

gc-dol: check-gc-toolchain gc
	$(MAKE) -C make-dist gc-dol ROOT=$(ROOT)

# ---------- Release packaging ----------
# `make release` builds everything the installed toolchains allow, then packages
# per-OS host archives + the firmware bundle into build/release/.  The split
# targets exist for CI: `release-host` on each OS runner, `release-firmware`
# once.  The script only packages what is already in build/; the dependencies
# below build it.  KOSLOAD_VERSION (from mk/version.mk) feeds the archive names.
#
# Note: the host tool embeds the console firmware for -F, so a release-quality
# kos-tool must be linked after the firmware .bin files exist.  `make release`
# guarantees that via the `all` prerequisite; `release-host` embeds whatever
# firmware is already present when `host` links.
RELEASE_SCRIPT := scripts/package-release.sh

release: all dist
	@KOSLOAD_VERSION=$(KOSLOAD_VERSION) $(RELEASE_SCRIPT) --all

release-host: host
	@KOSLOAD_VERSION=$(KOSLOAD_VERSION) $(RELEASE_SCRIPT) --host

release-firmware: dist
	@KOSLOAD_VERSION=$(KOSLOAD_VERSION) $(RELEASE_SCRIPT) --firmware

# ---------- Clean ----------

clean:
	$(MAKE) -C host ROOT=$(ROOT) clean
	$(MAKE) -C client ROOT=$(ROOT) clean
	$(MAKE) -C make-dist clean
	$(MAKE) -C third_party/minilzo clean
	rm -f $(VERSION_H)
	rm -rf $(BUILDDIR)
