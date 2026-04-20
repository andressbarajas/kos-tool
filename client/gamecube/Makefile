# client/gamecube/Makefile — builds GC serial + IP loaders

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk

# ---------- Toolchain ----------

GC_TOOLCHAIN ?= /opt/toolchains/gc/powerpc-eabi/bin

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
$(COMMONDIR)/info.o: $(CLIENTDIR)/common/info.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/screensaver.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/syscall_table.c $(VERSION_H) | $(COMMONDIR)
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
$(SERIALDIR)/main.o: $(CLIENTDIR)/common/main.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/entry.o: $(CLIENTDIR)/serial/entry.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_transport.o: $(CLIENTDIR)/serial/serial_transport.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_syscalls.o: $(CLIENTDIR)/serial/serial_syscalls.c $(VERSION_H) | $(SERIALDIR)
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

IP_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/commands.o \
    $(IPDIR)/adapter.o \
    $(IPDIR)/bba.o \
    $(IPDIR)/enc28j60.o \
    $(IPDIR)/w5500.o \
    $(IPDIR)/w5500_spi_gc.o \
    $(IPDIR)/net.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

IP_ELF := $(IPDIR)/gc-load-ip.elf
IP_BIN := $(IPDIR)/gc-load-ip.bin

# IP C object rules
$(IPDIR)/main.o: $(CLIENTDIR)/common/main.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/commands.o: $(CLIENTDIR)/common/commands.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/entry.o: $(CLIENTDIR)/network/entry.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_transport.o: $(CLIENTDIR)/network/network_transport.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/network/network_syscalls.c $(VERSION_H) | $(IPDIR)
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

$(IPDIR)/net.o: net/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet.o: net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/dhcp.o: net/dhcp.c $(VERSION_H) | $(IPDIR)
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
    console-test download-test exception-test exception-frame-test \
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
