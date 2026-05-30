# client/dreamcast/Makefile — builds DC serial + IP loaders

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

# ---------- Toolchain ----------

PREFIX  := sh-elf-
CC      := $(DC_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(DC_TOOLCHAIN)/$(PREFIX)ar
OBJCOPY := $(DC_TOOLCHAIN)/$(PREFIX)objcopy
SIZE    := $(DC_TOOLCHAIN)/$(PREFIX)size
HOSTCC  ?= cc

# ---------- Directories ----------

BUILDDIR    := build
COMMONDIR   := $(BUILDDIR)/common
SERIALDIR   := $(BUILDDIR)/serial
IPDIR       := $(BUILDDIR)/ip
HOSTDIR     := $(BUILDDIR)/host
MINILZO_DIR := $(ROOT)/third_party/minilzo
MINILZO_A   := $(MINILZO_DIR)/build-dc/minilzo.a

# ---------- Architecture flags ----------

ARCH_CFLAGS  := -ml -m4-single-only -std=gnu11 -O2 -nostdlib -nostartfiles -Wall -Wextra
ARCH_ASFLAGS := -ml -m4-single-only

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

# ---------- Memory layout defines (from mk/memory.mk; passed to C and ASM) ----------

MEMORY_DEFS := -DDC_LOADER_BASE=$(DC_LOADER_BASE) \
               -DDC_LOADER_SIZE=$(DC_LOADER_SIZE)

# ---------- Combined flags ----------

CFLAGS  := $(ARCH_CFLAGS) $(FREE_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\" -DKOSLOAD_GIT_REV=\"$(KOSLOAD_GIT_REV)\"
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

# Optional VGA-only (Naomi / System SP)
ifdef VGAONLY
CFLAGS += -DFORCE_VGA
endif

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

# ---------- Linker script (generated from mk/memory.mk) ----------

KOSLOAD_X := $(BUILDDIR)/kosload.x

$(KOSLOAD_X): kosload.x.in $(ROOT)/mk/memory.mk | $(BUILDDIR)
	@sed -e 's/@DC_LOADER_BASE@/$(DC_LOADER_BASE)/g' \
	     -e 's/@DC_LOADER_SIZE@/$(DC_LOADER_SIZE)/g' \
	     $< > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

# ============================================================
# Common library (kosload_common.a) — shared by serial and IP
# ============================================================

COMMON_C_OBJS := \
    $(COMMONDIR)/cdfs.o \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o \
    $(COMMONDIR)/target.o \
    $(COMMONDIR)/video.o \
    $(COMMONDIR)/maple.o \
    $(COMMONDIR)/perfctr.o \
    $(COMMONDIR)/memfuncs.o \
    $(COMMONDIR)/stdlib_funcs.o \
    $(COMMONDIR)/scif.o \
    $(COMMONDIR)/serial_io.o

COMMON_S_OBJS := \
    $(COMMONDIR)/disable.o \
    $(COMMONDIR)/cdfs_redir.o \
    $(COMMONDIR)/exception.o \
    $(COMMONDIR)/video_asm.o \
    $(COMMONDIR)/memcpy.o

COMMON_OBJS := $(COMMON_C_OBJS) $(COMMON_S_OBJS)
COMMON_A    := $(COMMONDIR)/kosload_common.a

# Common C object rules (by source directory)
$(COMMONDIR)/cdfs.o: $(CLIENTDIR)/common/core/cdfs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/target.o: target.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/video.o: video.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/maple.o: maple.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/perfctr.o: perfctr.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/memfuncs.o: memfuncs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/stdlib_funcs.o: stdlib_funcs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/scif.o: serial/scif.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/serial_io.o: serial/serial_io.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Common ASM object rules
$(COMMONDIR)/disable.o: disable.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/cdfs_redir.o: cdfs_redir.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/exception.o: exception.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/video_asm.o: video.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/memcpy.o: memcpy.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMON_A): $(COMMON_OBJS)
	$(AR) rcs $@ $^

# ============================================================
# Serial build (dc-load-ser)
# ============================================================

SERIAL_DEFS := -DKOSLOAD_SERIAL_TRANSPORT -DLOADER_NAME=\"dc-load-serial\"

SERIAL_OBJS := \
    $(SERIALDIR)/main.o \
    $(SERIALDIR)/entry.o \
    $(SERIALDIR)/serial_transport.o \
    $(SERIALDIR)/serial_syscalls.o \
    $(SERIALDIR)/cdfs_syscalls.o \
    $(SERIALDIR)/crt0.o \
    $(SERIALDIR)/go.o

SERIAL_ELF := $(SERIALDIR)/dc-load-ser.elf
SERIAL_BIN := $(SERIALDIR)/dc-load-ser.bin

DC_LDFLAGS := -nostartfiles -nostdlib -static -Wl,--warn-common -Wl,--no-undefined -Wl,-z,now -Wl,--gc-sections

# Serial C object rules
$(SERIALDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/entry.o: $(CLIENTDIR)/common/serial/entry.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_transport.o: $(CLIENTDIR)/common/serial/serial_transport.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/serial_syscalls.o: $(CLIENTDIR)/common/serial/serial_syscalls.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/cdfs_syscalls.o: $(CLIENTDIR)/common/serial/cdfs_syscalls.c $(VERSION_H) | $(SERIALDIR)
	$(CC) $(CFLAGS) $(SERIAL_DEFS) -c $< -o $@

# Serial ASM rules (need SERIAL_DEFS for crt0.S/go.S #ifdef KOSLOAD_SERIAL_TRANSPORT)
$(SERIALDIR)/crt0.o: crt0.S | $(SERIALDIR)
	$(CC) $(ASFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIALDIR)/go.o: go.S | $(SERIALDIR)
	$(CC) $(ASFLAGS) $(SERIAL_DEFS) -c $< -o $@

$(SERIAL_ELF): $(SERIAL_OBJS) $(COMMON_A) $(MINILZO_A) $(KOSLOAD_X)
	$(CC) $(ARCH_CFLAGS) $(DC_LDFLAGS) -T $(KOSLOAD_X) \
	    $(SERIAL_OBJS) -Wl,--start-group $(COMMON_A) $(MINILZO_A) -lgcc -Wl,--end-group \
	    -o $@

$(SERIAL_BIN): $(SERIAL_ELF)
	$(OBJCOPY) -O binary $< $@

# ============================================================
# IP build (dc-load-ip)
# ============================================================

IP_DEFS := -DLOADER_NAME=\"dc-load-ip\" -DMAX_RAM_BYTES=33554432
ifdef STATICIP
IP_DEFS += -DKOSLOAD_IP=\"$(STATICIP)\"
endif

IP_CFLAGS := $(CFLAGS) $(IP_INCLUDES)

# IP object groups keep the network layering visible:
# - platform adapters and bus bindings live under client/dreamcast/net/
# - shared chip drivers live under client/common/drivers/
# - shared transport/stack code lives under client/common/network/
IP_CLIENT_TRANSPORT_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/cdfs_syscalls.o \
    $(IPDIR)/commands.o

IP_PLATFORM_NET_OBJS := \
    $(IPDIR)/adapter.o \
    $(IPDIR)/rtl8139.o \
    $(IPDIR)/lan_adapter.o

IP_SHARED_CHIP_DRIVER_OBJS := \
    $(IPDIR)/w5500.o

IP_PLATFORM_BUS_BINDING_OBJS := \
    $(IPDIR)/w5500_spi_dc.o

IP_NETWORK_STACK_OBJS := \
    $(IPDIR)/net.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

IP_STARTUP_OBJS := \
    $(IPDIR)/crt0.o \
    $(IPDIR)/go.o

IP_OBJS := \
    $(IP_CLIENT_TRANSPORT_OBJS) \
    $(IP_PLATFORM_NET_OBJS) \
    $(IP_SHARED_CHIP_DRIVER_OBJS) \
    $(IP_PLATFORM_BUS_BINDING_OBJS) \
    $(IP_NETWORK_STACK_OBJS) \
    $(IP_STARTUP_OBJS)

IP_ELF := $(IPDIR)/dc-load-ip.elf
IP_BIN := $(IPDIR)/dc-load-ip.bin
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

$(IPDIR)/cdfs_syscalls.o: $(CLIENTDIR)/common/network/cdfs_syscalls.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/adapter.o: net/adapter.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/rtl8139.o: net/rtl8139.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/lan_adapter.o: net/lan_adapter.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/w5500.o: $(CLIENTDIR)/common/drivers/w5500.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/w5500_spi_dc.o: net/w5500_spi_dc.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/net.o: net/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet.o: net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(VERSION_H) | $(IPDIR)
	$(CC) $(IP_CFLAGS) $(IP_DEFS) -c $< -o $@

# IP ASM rules (no KOSLOAD_SERIAL_TRANSPORT define)
$(IPDIR)/crt0.o: crt0.S | $(IPDIR)
	$(CC) $(ASFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/go.o: go.S | $(IPDIR)
	$(CC) $(ASFLAGS) $(IP_DEFS) -c $< -o $@

$(IP_ELF): $(IP_OBJS) $(COMMON_A) $(KOSLOAD_X)
	$(CC) $(ARCH_CFLAGS) $(DC_LDFLAGS) -T $(KOSLOAD_X) \
	    $(IP_OBJS) -Wl,--start-group $(COMMON_A) -lgcc -Wl,--end-group \
	    -o $@

$(IP_BIN): $(IP_ELF)
	$(OBJCOPY) -O binary $< $@

# ============================================================
# minilzo (cross-compiled for SH4)
# ============================================================

$(MINILZO_A):
	$(MAKE) -C $(MINILZO_DIR) CC="$(CC)" AR="$(AR)" \
	    CFLAGS="$(ARCH_CFLAGS) $(FREE_CFLAGS)" BUILDDIR=build-dc

# ============================================================
# Examples (freestanding test programs)
# ============================================================

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples
MKISO_LITE := $(HOSTDIR)/mkiso-lite

# All examples with a main.c that compile for DC
DC_EXAMPLES := \
    cdfs-test console-test dc-probe download-test exception-test \
    exception-frame-test exit-test file-test gdb-test maple-test \
    rainbow-test syscall-test ip-calibrator

DC_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(DC_EXAMPLES)))

$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(EXDIR)/dc-example.ld | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -g -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -T $(EXDIR)/dc-example.ld $< -o $@

# Host helper for the cdfs-test ISO
$(MKISO_LITE): $(ROOT)/make-dist/tools/mkiso-lite.c \
               $(ROOT)/make-dist/tools/iso9660-lite.c \
               $(ROOT)/make-dist/tools/iso9660-lite.h | $(HOSTDIR)
	@echo "  HOSTCC  $@"
	@$(HOSTCC) -std=c11 -O2 -Wall -Wextra -o $@ \
	    $(ROOT)/make-dist/tools/mkiso-lite.c \
	    $(ROOT)/make-dist/tools/iso9660-lite.c

CDFS_TEST_ISO := $(EXBUILD)/cdfs-test.iso

$(CDFS_TEST_ISO): $(MKISO_LITE) | $(EXBUILD)
	@echo "  ISO     $@"
	@mkdir -p $(EXBUILD)/cdfs-test-data
	@echo "kosload cdfs-test" > $(EXBUILD)/cdfs-test-data/readme.txt
	@$(MKISO_LITE) --quiet --output $@ \
	    --source-dir $(EXBUILD)/cdfs-test-data --volume-id CDFS_TEST
	@rm -rf $(EXBUILD)/cdfs-test-data

examples: $(DC_EXAMPLE_ELFS) $(CDFS_TEST_ISO)

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
$(BUILDDIR) $(COMMONDIR) $(SERIALDIR) $(IPDIR) $(HOSTDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C $(MINILZO_DIR) BUILDDIR=build-dc clean

# Auto-generated header deps (-MMD).  `-include` (leading dash) is silent on
# missing files, so the first build after `clean` works without any .d files.
-include $(COMMON_OBJS:.o=.d) $(SERIAL_OBJS:.o=.d) $(IP_OBJS:.o=.d)
