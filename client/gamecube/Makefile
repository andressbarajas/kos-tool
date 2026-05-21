# client/gamecube/Makefile — builds GC serial + IP loaders

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

# ---------- Toolchain ----------

PREFIX  := powerpc-eabi-
CC      := $(GC_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(GC_TOOLCHAIN)/$(PREFIX)ar
OBJCOPY := $(GC_TOOLCHAIN)/$(PREFIX)objcopy
SIZE    := $(GC_TOOLCHAIN)/$(PREFIX)size

# ---------- Loader memory layout (from mk/memory.mk) ----------
# GC_MEM1_TOP, GC_LOADER_SIZE are defined in mk/memory.mk.
# GC_LOADER_BASE = GC_MEM1_TOP - GC_LOADER_SIZE.

GC_LOADER_BASE := $(shell printf '0x%08x' $$(($(GC_MEM1_TOP) - $(GC_LOADER_SIZE))))

# ---------- Directories ----------

BUILDDIR    := build
COMMONDIR   := $(BUILDDIR)/common
SERIALDIR   := $(BUILDDIR)/serial
IPDIR       := $(BUILDDIR)/ip
MINILZO_DIR := $(ROOT)/third_party/minilzo
MINILZO_A   := $(MINILZO_DIR)/build-gc/minilzo.a

# ---------- Architecture flags ----------

ARCH_CFLAGS  := -mcpu=750 -meabi -mhard-float -O2 -fno-builtin -nostdlib -nostartfiles -Wall -Wextra
ARCH_ASFLAGS := -mcpu=750 -meabi -Wa,-mregnames

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

# ---------- Include paths ----------

INCLUDES := -I$(ROOT)/include -I$(CLIENTDIR)/include -I$(CURDIR) -I$(CURDIR)/serial -I$(ROOT)/third_party/minilzo

# ---------- Memory layout defines (passed to C and ASM) ----------

MEMORY_DEFS := -DGC_LOADER_BASE=$(GC_LOADER_BASE) \
               -DGC_MEM1_TOP=$(GC_MEM1_TOP) \
               -DGC_LOADER_SIZE=$(GC_LOADER_SIZE)

# ---------- Combined flags ----------

CFLAGS  := $(ARCH_CFLAGS) $(FREE_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\"
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
IP_INCLUDES := -I$(CURDIR)/net

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

# ---------- Generated linker scripts (from .ld.in templates) ----------

SERIAL_LD := $(SERIALDIR)/kosload-serial.ld
IP_LD     := $(IPDIR)/kosload-ip.ld

$(SERIAL_LD): kosload-serial.ld.in $(ROOT)/mk/memory.mk | $(SERIALDIR)
	@sed -e 's/@GC_LOADER_BASE@/$(GC_LOADER_BASE)/g' \
	     -e 's/@GC_LOADER_SIZE@/$(GC_LOADER_SIZE)/g' \
	     -e 's/@GC_MEM1_TOP@/$(GC_MEM1_TOP)/g' \
	     $< > $@
	@echo "  GEN     $@"

$(IP_LD): kosload-ip.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@GC_LOADER_BASE@/$(GC_LOADER_BASE)/g' \
	     -e 's/@GC_LOADER_SIZE@/$(GC_LOADER_SIZE)/g' \
	     -e 's/@GC_MEM1_TOP@/$(GC_MEM1_TOP)/g' \
	     $< > $@
	@echo "  GEN     $@"

# ============================================================
# Common library (kosload_common.a) — shared by serial and IP
# ============================================================
# Note: On GC, crt0.S and go.S are in the common lib (no
# serial/IP-specific #ifdefs unlike DC).

COMMON_C_OBJS := \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o \
    $(COMMONDIR)/target.o \
    $(COMMONDIR)/exi.o \
    $(COMMONDIR)/video.o \
    $(COMMONDIR)/cache.o \
    $(COMMONDIR)/memfuncs.o \
    $(COMMONDIR)/usbgecko.o \
    $(COMMONDIR)/serial_io.o

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

$(COMMONDIR)/target.o: target.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/exi.o: exi.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/video.o: video.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/cache.o: cache.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/memfuncs.o: memfuncs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/usbgecko.o: serial/usbgecko.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/serial_io.o: serial/serial_io.c $(VERSION_H) | $(COMMONDIR)
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
# Serial build (gc-load-ser)
# ============================================================

SERIAL_DEFS := -DKOSLOAD_SERIAL_TRANSPORT -DLOADER_NAME=\"gc-load-serial\"

SERIAL_OBJS := \
    $(SERIALDIR)/main.o \
    $(SERIALDIR)/entry.o \
    $(SERIALDIR)/serial_transport.o \
    $(SERIALDIR)/serial_syscalls.o

SERIAL_ELF := $(SERIALDIR)/gc-load-ser.elf
SERIAL_BIN := $(SERIALDIR)/gc-load-ser.bin

GC_SERIAL_LDFLAGS := -nostdlib -Wl,-n -Wl,--gc-sections

# Serial C object rules
$(SERIALDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/entry.o: $(CLIENTDIR)/common/serial/entry.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_transport.o: $(CLIENTDIR)/common/serial/serial_transport.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_syscalls.o: $(CLIENTDIR)/common/serial/serial_syscalls.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIAL_ELF): $(SERIAL_OBJS) $(COMMON_A) $(MINILZO_A) $(SERIAL_LD)
	$(CC) $(ARCH_CFLAGS) $(GC_SERIAL_LDFLAGS) -T $(SERIAL_LD) \
	    $(SERIAL_OBJS) -Wl,--start-group $(COMMON_A) $(MINILZO_A) -lgcc -Wl,--end-group \
	    -o $@

$(SERIAL_BIN): $(SERIAL_ELF)
	$(OBJCOPY) -O binary $< $@

# ============================================================
# IP build (gc-load-ip)
# ============================================================

IP_DEFS := -DLOADER_NAME=\"gc-load-ip\" -DMAX_RAM_BYTES=25165824
ifdef STATICIP
IP_DEFS += -DKOSLOAD_IP=\"$(STATICIP)\"
endif

IP_CFLAGS := $(CFLAGS) $(IP_INCLUDES)

GC_IP_LDFLAGS := -nostdlib -Wl,-n -Wl,--gc-sections

# IP object groups keep the network layering visible:
# - platform adapters and bus bindings live under client/gamecube/net/
# - shared chip drivers live under client/common/drivers/
# - shared transport/stack code lives under client/common/network/
IP_CLIENT_TRANSPORT_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/commands.o

IP_PLATFORM_NET_OBJS := \
    $(IPDIR)/adapter.o \
    $(IPDIR)/bba.o \
    $(IPDIR)/enc28j60.o

IP_SHARED_CHIP_DRIVER_OBJS := \
    $(IPDIR)/w5500.o

IP_PLATFORM_BUS_BINDING_OBJS := \
    $(IPDIR)/w5500_spi_gc.o

IP_NETWORK_STACK_OBJS := \
    $(IPDIR)/net.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

IP_OBJS := \
    $(IP_CLIENT_TRANSPORT_OBJS) \
    $(IP_PLATFORM_NET_OBJS) \
    $(IP_SHARED_CHIP_DRIVER_OBJS) \
    $(IP_PLATFORM_BUS_BINDING_OBJS) \
    $(IP_NETWORK_STACK_OBJS)

IP_ELF := $(IPDIR)/gc-load-ip.elf
IP_BIN := $(IPDIR)/gc-load-ip.bin
IP_CONFIG_STAMP := $(IPDIR)/ip-config.stamp

$(IP_CONFIG_STAMP): | $(IPDIR)
	@printf '%s\n' 'STATICIP=$(if $(STATICIP),$(STATICIP),DHCP)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

# IP C object rules
$(IPDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/commands.o: $(CLIENTDIR)/common/core/commands.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/entry.o: $(CLIENTDIR)/common/network/entry.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_transport.o: $(CLIENTDIR)/common/network/network_transport.c $(VERSION_H) $(IP_CONFIG_STAMP) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/common/network/network_syscalls.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/adapter.o: net/adapter.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/bba.o: net/bba.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/enc28j60.o: net/enc28j60.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/w5500.o: $(CLIENTDIR)/common/drivers/w5500.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/w5500_spi_gc.o: net/w5500_spi_gc.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/net.o: $(CLIENTDIR)/common/network/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet.o: net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IP_ELF): $(IP_OBJS) $(COMMON_A) $(IP_LD)
	$(CC) $(ARCH_CFLAGS) $(GC_IP_LDFLAGS) -T $(IP_LD) \
	    $(IP_OBJS) -Wl,--start-group $(COMMON_A) -lgcc -Wl,--end-group \
	    -o $@

$(IP_BIN): $(IP_ELF)
	$(OBJCOPY) -O binary $< $@

# ============================================================
# minilzo (cross-compiled for PPC)
# ============================================================

$(MINILZO_A):
	$(MAKE) -C $(MINILZO_DIR) CC="$(CC)" AR="$(AR)" \
	    CFLAGS="$(ARCH_CFLAGS) $(FREE_CFLAGS)" BUILDDIR=build-gc

# ============================================================
# Examples (freestanding test programs)
# ============================================================

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

# All examples with a main.c that compile for GC (maple-test is DC-only)
GC_EXAMPLES := \
    bba-test console-test download-test exception-test exception-frame-test \
    enc28j60-reset-test exit-test exi-probe file-test gdb-test \
    font-test rainbow-test syscall-test

GC_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(GC_EXAMPLES)))

$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(EXDIR)/gc-example.ld | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -DGC_KOSLOAD_BASE=$(GC_LOADER_BASE) \
	    -T $(EXDIR)/gc-example.ld $< -o $@

examples: $(GC_EXAMPLE_ELFS)

# ============================================================
# Phony targets
# ============================================================

.PHONY: all serial ip examples clean

all: serial ip examples

serial: $(SERIAL_ELF) $(SERIAL_BIN)
	@$(SIZE) $(SERIAL_ELF)

ip: $(IP_ELF) $(IP_BIN)
	@$(SIZE) $(IP_ELF)

# Directory creation
$(COMMONDIR) $(SERIALDIR) $(IPDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C $(MINILZO_DIR) BUILDDIR=build-gc clean

# Auto-generated header deps (-MMD).  `-include` (leading dash) is silent on
# missing files, so the first build after `clean` works without any .d files.
-include $(COMMON_OBJS:.o=.d) $(SERIAL_OBJS:.o=.d) $(IP_OBJS:.o=.d)
