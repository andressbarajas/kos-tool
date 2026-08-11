# client/xbox/Makefile — builds the Xbox IP loader (xbox-load-ip)
#
# Single-stage: link a PE/COFF image with the i686-pc-xbox toolchain, then
# convert it to a bootable default.xbe with host/tools/pe2xbe.  Also emits a
# raw .bin for kos-tool firmware embedding.

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

# ---------- Toolchain ----------

PREFIX  := $(XBOX_PREFIX)
CC      := $(XBOX_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(XBOX_TOOLCHAIN)/$(PREFIX)ar
OBJCOPY := $(XBOX_TOOLCHAIN)/$(PREFIX)objcopy
SIZE    := $(XBOX_TOOLCHAIN)/$(PREFIX)size

# Host C compiler for the pe2xbe converter.
HOSTCC  ?= cc
PE2XBE  := $(ROOT)/host/tools/pe2xbe/pe2xbe

# ---------- Directories ----------

BUILDDIR  := build
COMMONDIR := $(BUILDDIR)/common
IPDIR     := $(BUILDDIR)/ip

# ---------- Flags ----------

# Pentium III (Coppermine): x87 + MMX + SSE, no SSE2.  Freestanding, no PIC so
# the kosload header's absolute pointers resolve at the fixed XBE base.
ARCH_CFLAGS := -march=pentium3 -m32 -O2 -fno-pic -fno-builtin -nostdlib \
               -nostartfiles -Wall -Wextra

FREE_CFLAGS := \
    -ffreestanding \
    -fno-common \
    -fomit-frame-pointer \
    -fno-strict-aliasing \
    -fno-asynchronous-unwind-tables \
    -fno-unwind-tables \
    -fno-exceptions \
    -fno-stack-protector \
    -fno-stack-check \
    -ffunction-sections \
    -fdata-sections

INCLUDES := -I$(ROOT)/include -I$(CLIENTDIR)/include -I$(CURDIR) -I$(CURDIR)/net

MEMORY_DEFS := -DXBOX_LOADER_BASE=$(XBOX_LOADER_BASE) \
               -DXBOX_RAM_TOP=$(XBOX_RAM_TOP) \
               -DXBOX_KOSLOAD_BASE=$(XBOX_KOSLOAD_BASE) \
               -DXBOX_DEFAULT_LOAD_ADDR=$(XBOX_DEFAULT_LOAD_ADDR)

IP_DEFS := -DLOADER_NAME=\"xbox-load-ip\" -DMAX_RAM_BYTES=67108864
# Optional static IP (matches DC/GC/PS2): make xbox STATICIP=192.168.1.50
# Omit for DHCP (the loader's 0.0.0.0 default).
ifdef STATICIP
IP_DEFS += -DKOSLOAD_IP=\"$(STATICIP)\"
endif

CFLAGS  := $(ARCH_CFLAGS) $(FREE_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) $(IP_DEFS) \
           -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\" -DKOSLOAD_GIT_REV=\"$(KOSLOAD_GIT_REV)\" \
           -MMD -MP
ASFLAGS := $(ARCH_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) -MMD -MP

# PE image layout: base + 0x20 alignment reach the linker script via ld's
# __image_base__ / __section_alignment__ variables.
LDFLAGS := -nostdlib -nostartfiles \
           -Wl,--image-base,$(XBOX_LOADER_BASE) \
           -Wl,--section-alignment,0x20 \
           -Wl,--file-alignment,0x20 \
           -Wl,--no-undefined \
           -Wl,--gc-sections \
           -Wl,-e,_start

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

# ---------- Generated linker script ----------

IP_LD := $(IPDIR)/kosload-ip.ld

$(IP_LD): kosload-ip.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@XBOX_LOADER_BASE@/$(XBOX_LOADER_BASE)/g' \
	     -e 's/@XBOX_KOSLOAD_BASE@/$(XBOX_KOSLOAD_BASE)/g' \
	     -e 's/@XBOX_LOADER_SIZE@/$(XBOX_LOADER_SIZE)/g' \
	     -e 's/@XBOX_DEFAULT_LOAD_ADDR@/$(XBOX_DEFAULT_LOAD_ADDR)/g' \
	     $< > $@
	@echo "  GEN     $@"

# ---------- Shared common objects ----------

COMMON_OBJS := \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o

$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- IP (network) objects ----------

# Shared client/common network + core objects.
IP_COMMON_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/commands.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/net.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

# Xbox-specific objects.
IP_PLATFORM_OBJS := \
    $(IPDIR)/crt0.o \
    $(IPDIR)/go.o \
    $(IPDIR)/exception_stubs.o \
    $(IPDIR)/exception.o \
    $(IPDIR)/target.o \
    $(IPDIR)/rtc.o \
    $(IPDIR)/video.o \
    $(IPDIR)/memfuncs.o \
    $(IPDIR)/syscall.o \
    $(IPDIR)/kosload_header.o \
    $(IPDIR)/adapter.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/nvnet.o

IP_OBJS := $(IP_COMMON_OBJS) $(IP_PLATFORM_OBJS)

# Shared source rules
$(IPDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/commands.o: $(CLIENTDIR)/common/core/commands.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/entry.o: $(CLIENTDIR)/common/network/entry.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/network_transport.o: $(CLIENTDIR)/common/network/network_transport.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/common/network/network_syscalls.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/net.o: $(CLIENTDIR)/common/network/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Xbox source rules
$(IPDIR)/crt0.o: crt0.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/go.o: go.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/exception_stubs.o: exception.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/exception.o: exception.c video.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/target.o: target.c video.h cache.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/rtc.o: rtc.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/video.o: video.c video.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/memfuncs.o: memfuncs.c | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/syscall.o: syscall.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/kosload_header.o: kosload_header.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/adapter.o: net/adapter.c net/nvnet.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/packet.o: net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/nvnet.o: net/nvnet.c net/nvnet.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Link + package ----------

IP_EXE := $(IPDIR)/xbox-load-ip.exe
IP_BIN := $(IPDIR)/xbox-load-ip.bin
IP_XBE := $(IPDIR)/default.xbe

$(IP_EXE): $(IP_OBJS) $(COMMON_OBJS) $(IP_LD)
	$(CC) $(ARCH_CFLAGS) $(LDFLAGS) -T $(IP_LD) \
	    $(IP_OBJS) $(COMMON_OBJS) -lgcc -o $@
	@$(SIZE) $@ 2>/dev/null || true

$(IP_BIN): $(IP_EXE)
	@$(OBJCOPY) -O binary $< $@
	@echo "  OBJCOPY $@"

$(PE2XBE): $(ROOT)/host/tools/pe2xbe/pe2xbe.c $(ROOT)/host/tools/pe2xbe/Makefile
	@$(MAKE) -C $(ROOT)/host/tools/pe2xbe CC=$(HOSTCC)

$(IP_XBE): $(IP_EXE) $(PE2XBE)
	@$(PE2XBE) -o $@ -t "xbox-load-ip" $(IP_EXE)

# ---------- Freestanding Xbox guest examples ----------

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

XBOX_EXAMPLES := \
    console-test download-test exception-test exception-frame-test exit-test \
    file-test gdb-test ip-calibrator syscall-test xbox-video-test xbox-reboot
XBOX_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(XBOX_EXAMPLES)))

$(EXBUILD)/%.exe: $(EXDIR)/%/main.c $(ROOT)/mk/memory.mk | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -g -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -DXBOX_KOSLOAD_BASE=$(XBOX_KOSLOAD_BASE) \
	    -Wl,--image-base,$(XBOX_DEFAULT_LOAD_ADDR) \
	    -Wl,--section-alignment,0x20 -Wl,--file-alignment,0x20 \
	    -Wl,-e,_start $< -o $@

$(EXBUILD)/%.elf: $(EXBUILD)/%.exe
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O elf32-i386 $< $@

examples: $(XBOX_EXAMPLE_ELFS)

# ---------- Phony ----------

.PHONY: all ip examples clean

all: ip examples

ip: $(IP_EXE) $(IP_BIN) $(IP_XBE)

$(COMMONDIR) $(IPDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

-include $(COMMON_OBJS:.o=.d) $(IP_OBJS:.o=.d)
