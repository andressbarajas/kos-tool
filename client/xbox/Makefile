# client/xbox/Makefile — builds the Xbox IP loader (xbox-load-ip)
#
# Single-stage: link an ELF32 image with the i686-pc-xbox toolchain, then
# convert it to a bootable default.xbe with host/tools/elf2xbe.  Also emits a
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

# Host C compiler for the elf2xbe converter.
HOSTCC  ?= cc
ELF2XBE := $(ROOT)/host/tools/elf2xbe/elf2xbe

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
           $(KOSLOAD_VERSION_DEFS) \
           -MMD -MP
ASFLAGS := $(ARCH_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) -MMD -MP

# ELF image layout: the image base is set in the linker script, which needs the
# literal anyway for its ASSERTs (--section-alignment / --file-alignment have no
# elf_i386 equivalent).  ELF would otherwise page-align every PT_LOAD to 0x1000;
# -z max-page-size=0x20 keeps the window as tight as PE's 0x20 alignment did.
LDFLAGS := -nostdlib -nostartfiles \
           -Wl,-z,max-page-size=0x20 \
           -Wl,--build-id=none \
           -Wl,--no-warn-rwx-segments \
           -Wl,--no-undefined \
           -Wl,--gc-sections \
           -Wl,-e,_start

.DEFAULT_GOAL := all

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
    $(COMMONDIR)/display.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o

$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/display.o: $(CLIENTDIR)/common/core/display.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
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
$(IPDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/commands.o: $(CLIENTDIR)/common/core/commands.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/entry.o: $(CLIENTDIR)/common/network/entry.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/network_transport.o: $(CLIENTDIR)/common/network/network_transport.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/common/network/network_syscalls.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/net.o: $(CLIENTDIR)/common/network/net.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Xbox source rules
$(IPDIR)/crt0.o: crt0.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/go.o: go.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/exception_stubs.o: exception.S | $(IPDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(IPDIR)/exception.o: exception.c video.h $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/target.o: target.c video.h cache.h $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/rtc.o: rtc.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/video.o: video.c video.h $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/memfuncs.o: memfuncs.c | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/syscall.o: syscall.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/kosload_header.o: kosload_header.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/adapter.o: net/adapter.c net/nvnet.h $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/packet.o: net/packet.c $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(IPDIR)/nvnet.o: net/nvnet.c net/nvnet.h $(KOSLOAD_VERSION_MK) | $(IPDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- Link + package ----------

IP_ELF := $(IPDIR)/xbox-load-ip.elf
IP_BIN := $(IPDIR)/xbox-load-ip.bin
IP_XBE := $(IPDIR)/default.xbe

$(IP_ELF): $(IP_OBJS) $(COMMON_OBJS) $(IP_LD)
	$(CC) $(ARCH_CFLAGS) $(LDFLAGS) -T $(IP_LD) \
	    $(IP_OBJS) $(COMMON_OBJS) -lgcc -o $@
	@$(SIZE) $@ 2>/dev/null || true

$(IP_BIN): $(IP_ELF)
	@$(OBJCOPY) -O binary $< $@
	@echo "  OBJCOPY $@"

$(ELF2XBE): $(ROOT)/host/tools/elf2xbe/elf2xbe.c $(ROOT)/host/tools/elf2xbe/Makefile
	@$(MAKE) -C $(ROOT)/host/tools/elf2xbe CC=$(HOSTCC)

# -b is the XBE image base: the ELF has no image-base field, and the page below
# the first PT_LOAD is the reserve the generated XBE headers occupy.
$(IP_XBE): $(IP_ELF) $(ELF2XBE)
	@$(ELF2XBE) -o $@ -b $(XBOX_LOADER_BASE) -t "xbox-load-ip" $(IP_ELF)

# ---------- Freestanding Xbox guest examples ----------

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

XBOX_EXAMPLES := \
    console-test download-test exception-test exception-frame-test exit-test \
    file-test gdb-test ip-calibrator syscall-test xbox-video-test xbox-reboot
XBOX_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(XBOX_EXAMPLES)))

$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(ROOT)/mk/memory.mk | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -g -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -DXBOX_KOSLOAD_BASE=$(XBOX_KOSLOAD_BASE) \
	    -Wl,-z,max-page-size=0x20 -Wl,--build-id=none \
	    -Wl,-Ttext-segment=$(XBOX_DEFAULT_LOAD_ADDR) \
	    -Wl,-e,start $< -o $@

examples: $(XBOX_EXAMPLE_ELFS)

# ---------- Phony ----------

.PHONY: all ip examples clean

all: ip examples

ip: $(IP_ELF) $(IP_BIN) $(IP_XBE)

$(COMMONDIR) $(IPDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

-include $(COMMON_OBJS:.o=.d) $(IP_OBJS:.o=.d)
