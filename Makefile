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

# ---------- Targets ----------

.PHONY: all host dc gc disc disc-dc disc-gc clean

all: dc gc host

host: $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C host ROOT=$(ROOT)
	@cp host/build/kos-tool $(BUILDDIR)/
	@echo "  COPY    $(BUILDDIR)/kos-tool"

dc: $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client dc ROOT=$(ROOT)
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

gc: $(VERSION_H) | $(BUILDDIR)
	$(MAKE) -C client gc ROOT=$(ROOT)
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

disc: disc-dc disc-gc

disc-dc: dc
	$(MAKE) -C make-cd dc ROOT=$(ROOT)

disc-gc: gc
	$(MAKE) -C make-cd gc ROOT=$(ROOT)

# ---------- Clean ----------

clean:
	$(MAKE) -C host ROOT=$(ROOT) clean
	$(MAKE) -C client ROOT=$(ROOT) clean
	$(MAKE) -C make-cd clean
	$(MAKE) -C third_party/minilzo clean
	rm -f $(VERSION_H)
	rm -rf $(BUILDDIR)
