# Makefile — kosload top-level build dispatcher
#
# Usage:
#   make all           Build host tool + DC + GC + Wii + PS2 + Xbox + PSP firmware
#   make dc            Build Dreamcast firmware
#   make gc            Build GameCube firmware
#   make wii           Build Wii firmware
#   make ps2           Build PlayStation 2 firmware
#   make xbox          Build Xbox firmware
#   make psp           Build PSP firmware
#   make dist          Build all delivery artifacts (CDI + ISO + WAD + XISO + PBP)
#   make dist-dc       Build Dreamcast CDI images
#   make dist-gc       Build GameCube ISO images
#   make dist-wii      Build Wii channel WAD
#   make dist-ps2      Build PlayStation 2 ISO
#   make dist-xbox     Build Xbox XISO
#   make dist-psp      Build PSP EBOOT.PBP
#   make gc-dol        Build GameCube DOL files only (no ISO)
#   make release       Build everything + package release archives (build/release/)
#   make release-host  Package the host-tool archive for this OS
#   make release-firmware  Package the firmware bundle
#   make clean         Remove all build artifacts
#
# Output layout: build/kos-tool plus one directory per console,
# build/<dc|gc|wii|ps2|xbox|psp>/ holding that console's loader binaries and
# disc images with its examples/ alongside.  See the BUILDDIR block below.

ROOT := $(CURDIR)
include mk/version.mk
include mk/toolchains.mk

# Default target
.DEFAULT_GOAL := all

# Each console target (dc/gc/wii/ps2/xbox) re-invokes `$(MAKE) host` so the host
# tool embeds that console's freshly built firmware.  Under `make -j` the
# auto-* console builds run concurrently, so several `make host` invocations
# race on build-host/minilzo.a and build/kos-tool (intermittent link failures
# like "undefined reference to lzo1x_*").  Serialize THIS makefile's targets;
# the recursive sub-makes (client/*, host/) still compile their objects in
# parallel, so per-console build time is unaffected.
.NOTPARALLEL:

# ---------- Output directory ----------
#
#   build/kos-tool             host tool (console-independent)
#   build/<console>/           loader binaries + disc images
#   build/<console>/examples/  example programs
#
# <console> is the make target name (dc, gc, wii, ps2, xbox, psp); make-dist and
# package-release.sh derive their paths from the same names.  Each examples/ is
# wiped before staging — the copy is a plain `cp *.elf`, so otherwise an example
# deleted from client/examples/ lingers and ships in the release bundle.

BUILDDIR := build

DC_OUT   := $(BUILDDIR)/dc
GC_OUT   := $(BUILDDIR)/gc
WII_OUT  := $(BUILDDIR)/wii
PS2_OUT  := $(BUILDDIR)/ps2
XBOX_OUT := $(BUILDDIR)/xbox
PSP_OUT  := $(BUILDDIR)/psp

$(BUILDDIR) $(DC_OUT) $(GC_OUT) $(WII_OUT) $(PS2_OUT) $(XBOX_OUT) $(PSP_OUT):
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

XBOX_CC      := $(XBOX_TOOLCHAIN)/$(XBOX_PREFIX)gcc
XBOX_AR      := $(XBOX_TOOLCHAIN)/$(XBOX_PREFIX)ar
XBOX_OBJCOPY := $(XBOX_TOOLCHAIN)/$(XBOX_PREFIX)objcopy
XBOX_SIZE    := $(XBOX_TOOLCHAIN)/$(XBOX_PREFIX)size

PSP_CC       := $(PSP_TOOLCHAIN)/$(PSP_PREFIX)gcc
PSP_AR       := $(PSP_TOOLCHAIN)/$(PSP_PREFIX)ar
PSP_OBJCOPY  := $(PSP_TOOLCHAIN)/$(PSP_PREFIX)objcopy
PSP_SIZE     := $(PSP_TOOLCHAIN)/$(PSP_PREFIX)size

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

.PHONY: all host dc gc wii ps2 xbox psp dist dist-dc dist-gc dist-wii dist-ps2 dist-xbox dist-psp gc-dol \
        auto-dc auto-gc auto-wii auto-ps2 auto-xbox auto-psp \
        dist-auto-dc dist-auto-gc dist-auto-wii dist-auto-ps2 dist-auto-xbox \
        dist-auto-psp \
        release release-host release-firmware print-version clean \
        check-dc-toolchain check-gc-toolchain check-wii-toolchain check-ps2-toolchain \
        check-xbox-toolchain check-psp-toolchain

# Single source of truth for the version string (e.g. for release tooling/CI).
# `make -s print-version` -> 3.1.0
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

check-xbox-toolchain:
	$(call require_host_tool,$(XBOX_CC),Xbox compiler (i686-pc-xbox-gcc),Xbox,XBOX_TOOLCHAIN)
	$(call require_host_tool,$(XBOX_AR),Xbox archiver (i686-pc-xbox-ar),Xbox,XBOX_TOOLCHAIN)
	$(call require_host_tool,$(XBOX_OBJCOPY),Xbox objcopy (i686-pc-xbox-objcopy),Xbox,XBOX_TOOLCHAIN)
	$(call require_host_tool,$(XBOX_SIZE),Xbox size tool (i686-pc-xbox-size),Xbox,XBOX_TOOLCHAIN)

check-psp-toolchain:
	$(call require_host_tool,$(PSP_CC),PSP compiler (mipsel-psp-elf-gcc),PSP,PSP_TOOLCHAIN)
	$(call require_host_tool,$(PSP_AR),PSP archiver (mipsel-psp-elf-ar),PSP,PSP_TOOLCHAIN)
	$(call require_host_tool,$(PSP_OBJCOPY),PSP objcopy (mipsel-psp-elf-objcopy),PSP,PSP_TOOLCHAIN)
	$(call require_host_tool,$(PSP_SIZE),PSP size tool (mipsel-psp-elf-size),PSP,PSP_TOOLCHAIN)

# `make all` builds every console whose cross-toolchain is installed, skipping
# the rest with a SKIP notice, then always builds the host tool.  Use the
# explicit per-console targets (make dc/gc/wii/ps2/xbox) for a hard error when a
# toolchain is missing.  Real compile failures still abort — only a missing
# toolchain is skipped.
all: auto-dc auto-gc auto-wii auto-ps2 auto-xbox auto-psp host

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

auto-xbox:
	@if $(call has_host_tool,$(XBOX_CC)) && \
	    $(call has_host_tool,$(XBOX_AR)) && \
	    $(call has_host_tool,$(XBOX_OBJCOPY)) && \
	    $(call has_host_tool,$(XBOX_SIZE)); then \
		$(MAKE) xbox; \
	else \
		echo "  SKIP    Xbox firmware (toolchain not found)"; \
	fi

auto-psp:
	@if $(call has_host_tool,$(PSP_CC)) && \
	    $(call has_host_tool,$(PSP_AR)) && \
	    $(call has_host_tool,$(PSP_OBJCOPY)) && \
	    $(call has_host_tool,$(PSP_SIZE)); then \
		$(MAKE) psp; \
	else \
		echo "  SKIP    PSP firmware (toolchain not found)"; \
	fi

host: | $(BUILDDIR)
	$(MAKE) -C host ROOT=$(ROOT)
	@cp host/build/kos-tool $(BUILDDIR)/ 2>/dev/null || cp host/build/kos-tool.exe $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/kos-tool"

dc: check-dc-toolchain | $(DC_OUT)
	$(MAKE) -C client/dreamcast ROOT=$(ROOT) all
	@cp client/dreamcast/build/serial/dc-load-ser.elf $(DC_OUT)/
	@cp client/dreamcast/build/serial/dc-load-ser.bin $(DC_OUT)/
	@cp client/dreamcast/build/ip/dc-load-ip.elf $(DC_OUT)/
	@cp client/dreamcast/build/ip/dc-load-ip.bin $(DC_OUT)/
	@echo "  COPY    $(DC_OUT)/dc-load-ser.{elf,bin} dc-load-ip.{elf,bin}"
	@rm -rf $(DC_OUT)/examples && mkdir -p $(DC_OUT)/examples
	@cp client/dreamcast/build/examples/*.elf $(DC_OUT)/examples/
	@if ls client/dreamcast/build/examples/*.iso >/dev/null 2>&1; then \
	    cp client/dreamcast/build/examples/*.iso $(DC_OUT)/examples/; \
	fi
	@echo "  COPY    $(DC_OUT)/examples/"
	$(MAKE) host

gc: check-gc-toolchain | $(GC_OUT)
	$(MAKE) -C client/gamecube ROOT=$(ROOT) all
	@cp client/gamecube/build/serial/gc-load-ser.elf $(GC_OUT)/
	@cp client/gamecube/build/serial/gc-load-ser.bin $(GC_OUT)/
	@cp client/gamecube/build/ip/gc-load-ip.elf $(GC_OUT)/
	@cp client/gamecube/build/ip/gc-load-ip.bin $(GC_OUT)/
	@echo "  COPY    $(GC_OUT)/gc-load-ser.{elf,bin} gc-load-ip.{elf,bin}"
	@rm -rf $(GC_OUT)/examples && mkdir -p $(GC_OUT)/examples
	@cp client/gamecube/build/examples/*.elf $(GC_OUT)/examples/
	@echo "  COPY    $(GC_OUT)/examples/*.elf"
	$(MAKE) host

wii: check-wii-toolchain | $(WII_OUT)
	$(MAKE) -C client/wii ROOT=$(ROOT) all
	@cp client/wii/build/ip/wii-load-ip.elf $(WII_OUT)/
	@cp client/wii/build/ip/wii-load-ip.bin $(WII_OUT)/
	@cp client/wii/build/ip/wii-load-ip.dol $(WII_OUT)/
	@echo "  COPY    $(WII_OUT)/wii-load-ip.{elf,bin,dol}"
	@rm -rf $(WII_OUT)/examples && mkdir -p $(WII_OUT)/examples
	@cp client/wii/build/examples/*.elf $(WII_OUT)/examples/
	@echo "  COPY    $(WII_OUT)/examples/*.elf"
	$(MAKE) host

ps2: check-ps2-toolchain | $(PS2_OUT)
	$(MAKE) -C client/playstation2 ROOT=$(ROOT) all
	@cp client/playstation2/build/ip/ps2-load-ip.elf $(PS2_OUT)/
	@echo "  COPY    $(PS2_OUT)/ps2-load-ip.elf"
	@rm -rf $(PS2_OUT)/examples && mkdir -p $(PS2_OUT)/examples
	@cp client/playstation2/build/examples/*.elf $(PS2_OUT)/examples/
	@echo "  COPY    $(PS2_OUT)/examples/*.elf"
	$(MAKE) host

xbox: check-xbox-toolchain | $(XBOX_OUT)
	$(MAKE) -C client/xbox ROOT=$(ROOT) all
	@cp client/xbox/build/ip/xbox-load-ip.elf $(XBOX_OUT)/
	@cp client/xbox/build/ip/xbox-load-ip.bin $(XBOX_OUT)/
	@cp client/xbox/build/ip/default.xbe $(XBOX_OUT)/default.xbe
	@echo "  COPY    $(XBOX_OUT)/xbox-load-ip.{elf,bin} $(XBOX_OUT)/default.xbe"
	@rm -rf $(XBOX_OUT)/examples && mkdir -p $(XBOX_OUT)/examples
	@cp client/xbox/build/examples/*.elf $(XBOX_OUT)/examples/
	@echo "  COPY    $(XBOX_OUT)/examples/*.elf"
	$(MAKE) host

# EBOOT.PBP keeps its exact name: the PSP firmware only boots
# ms0:/PSP/GAME/<dir>/EBOOT.PBP, so it can be moved but never renamed.
psp: check-psp-toolchain | $(PSP_OUT)
	$(MAKE) -C client/psp ROOT=$(ROOT) all
	@cp client/psp/build/usb/psp-load-usb.elf $(PSP_OUT)/
	@cp client/psp/build/usb/psp-load-usb.bin $(PSP_OUT)/
	@cp client/psp/build/usb/EBOOT.PBP $(PSP_OUT)/EBOOT.PBP
	@echo "  COPY    $(PSP_OUT)/psp-load-usb.{elf,bin} $(PSP_OUT)/EBOOT.PBP"
	@rm -rf $(PSP_OUT)/examples && mkdir -p $(PSP_OUT)/examples
	@cp client/psp/build/examples/*.elf $(PSP_OUT)/examples/
	@echo "  COPY    $(PSP_OUT)/examples/*.elf"
	$(MAKE) host

# ---------- Distribution artifact targets ----------

dist: dist-auto-dc dist-auto-gc dist-auto-wii dist-auto-ps2 dist-auto-xbox \
      dist-auto-psp

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

dist-auto-psp:
	@if $(call has_host_tool,$(PSP_CC)) && \
	    $(call has_host_tool,$(PSP_AR)) && \
	    $(call has_host_tool,$(PSP_OBJCOPY)) && \
	    $(call has_host_tool,$(PSP_SIZE)); then \
		$(MAKE) dist-psp; \
	else \
		echo "  SKIP    PSP EBOOT.PBP (toolchain not found)"; \
	fi

dist-auto-xbox:
	@if $(call has_host_tool,$(XBOX_CC)) && \
	    $(call has_host_tool,$(XBOX_AR)) && \
	    $(call has_host_tool,$(XBOX_OBJCOPY)) && \
	    $(call has_host_tool,$(XBOX_SIZE)); then \
		$(MAKE) dist-xbox; \
	else \
		echo "  SKIP    Xbox XISO (toolchain not found)"; \
	fi

dist-dc: check-dc-toolchain dc
	$(MAKE) -C make-dist dc ROOT=$(ROOT)

dist-gc: check-gc-toolchain gc
	$(MAKE) -C make-dist gc ROOT=$(ROOT)

dist-wii: check-wii-toolchain wii
	$(MAKE) -C make-dist wii ROOT=$(ROOT)

dist-ps2: check-ps2-toolchain ps2
	$(MAKE) -C make-dist ps2 ROOT=$(ROOT)

dist-xbox: check-xbox-toolchain xbox
	$(MAKE) -C make-dist xbox ROOT=$(ROOT)

# PSP has no make-dist step: the distributable artifact IS the EBOOT.PBP, which
# the `psp` target already builds and copies into build/.  There is no disc
# image to author, so this exists purely so `make dist` produces the PSP
# artifact like every other console.  Without it, only `make all` (and hence
# `make release`) built the EBOOT, and a bare `make dist` silently omitted it.
dist-psp: check-psp-toolchain psp

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
	rm -f
	rm -rf $(BUILDDIR)
