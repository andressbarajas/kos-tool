# Makefile — kosload top-level build dispatcher

ROOT := $(CURDIR)
include mk/version.mk

# Default target
.DEFAULT_GOAL := all

# ---------- version.h generation ----------

VERSION_H    := $(ROOT)/include/kosload/version.h
VERSION_H_IN := $(ROOT)/include/kosload/version.h.in

$(VERSION_H): $(VERSION_H_IN) mk/version.mk
	@sed -e 's/@KOSLOAD_VERSION_MAJOR@/$(KOSLOAD_VERSION_MAJOR)/g' \
	     -e 's/@KOSLOAD_VERSION_MINOR@/$(KOSLOAD_VERSION_MINOR)/g' \
	     -e 's/@KOSLOAD_VERSION_PATCH@/$(KOSLOAD_VERSION_PATCH)/g' \
	     -e 's/@KOSLOAD_VERSION@/$(KOSLOAD_VERSION)/g' \
	     $< > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

# ---------- Output directory ----------

BUILDDIR := build

$(BUILDDIR):
	@mkdir -p $@

# ---------- Toolchain checks ----------

DC_TOOLCHAIN ?= /opt/toolchains/dc/sh-elf/bin
DC_PREFIX    := sh-elf-
DC_CC        := $(DC_TOOLCHAIN)/$(DC_PREFIX)gcc
DC_AR        := $(DC_TOOLCHAIN)/$(DC_PREFIX)ar
DC_OBJCOPY   := $(DC_TOOLCHAIN)/$(DC_PREFIX)objcopy
DC_SIZE      := $(DC_TOOLCHAIN)/$(DC_PREFIX)size

GC_TOOLCHAIN ?= /opt/toolchains/gc/powerpc-eabi/bin
GC_PREFIX    := powerpc-eabi-
GC_CC        := $(GC_TOOLCHAIN)/$(GC_PREFIX)gcc
GC_AR        := $(GC_TOOLCHAIN)/$(GC_PREFIX)ar
GC_OBJCOPY   := $(GC_TOOLCHAIN)/$(GC_PREFIX)objcopy
GC_SIZE      := $(GC_TOOLCHAIN)/$(GC_PREFIX)size

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

.PHONY: all host dc gc disc disc-dc disc-gc disc-auto-dc disc-auto-gc clean \
        check-dc-toolchain check-gc-toolchain

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

all: dc gc host

host: $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C host ROOT=$(ROOT)
	@cp host/build/kos-tool $(BUILDDIR)/
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

# ---------- Disc image targets ----------

disc: disc-auto-dc disc-auto-gc

disc-auto-dc:
	@if $(call has_host_tool,$(DC_CC)) && \
	    $(call has_host_tool,$(DC_AR)) && \
	    $(call has_host_tool,$(DC_OBJCOPY)) && \
	    $(call has_host_tool,$(DC_SIZE)); then \
		$(MAKE) disc-dc; \
	else \
		echo "  SKIP    Dreamcast disc images (toolchain not found)"; \
	fi

disc-auto-gc:
	@if $(call has_host_tool,$(GC_CC)) && \
	    $(call has_host_tool,$(GC_AR)) && \
	    $(call has_host_tool,$(GC_OBJCOPY)) && \
	    $(call has_host_tool,$(GC_SIZE)); then \
		$(MAKE) disc-gc; \
	else \
		echo "  SKIP    GameCube disc images (toolchain not found)"; \
	fi

disc-dc: check-dc-toolchain dc
	$(MAKE) -C make-cd dc ROOT=$(ROOT)

disc-gc: check-gc-toolchain gc
	$(MAKE) -C make-cd gc ROOT=$(ROOT)

# ---------- Clean ----------

clean:
	$(MAKE) -C host ROOT=$(ROOT) clean
	$(MAKE) -C client ROOT=$(ROOT) clean
	$(MAKE) -C make-cd clean
	$(MAKE) -C third_party/minilzo clean
	rm -f $(VERSION_H)
	rm -rf $(BUILDDIR)
