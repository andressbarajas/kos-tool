# client/wii/Makefile - clean-room Wii mode wii-load-ip build

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

PREFIX  := powerpc-eabi-
CC      := $(GC_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(GC_TOOLCHAIN)/$(PREFIX)ar
OBJCOPY := $(GC_TOOLCHAIN)/$(PREFIX)objcopy
SIZE    := $(GC_TOOLCHAIN)/$(PREFIX)size
HOST_CC ?= cc

BUILDDIR  := build
COMMONDIR := $(BUILDDIR)/common
IPDIR     := $(BUILDDIR)/ip
DOLTOOL   := $(BUILDDIR)/elf2dol

WII_LOADER_BASE := $(shell printf '0x%08x' $$(($(WII_MEM1_TOP) - $(WII_LOADER_SIZE))))
WII_USE_MEM2_IPC_ARENA ?= 0

ARCH_CFLAGS  := -mcpu=750 -meabi -mhard-float -O2 -fno-builtin -nostdlib -nostartfiles -Wall -Wextra
ARCH_ASFLAGS := -mcpu=750 -meabi -Wa,-mregnames

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

INCLUDES := \
    -I$(ROOT)/include \
    -I$(CLIENTDIR)/include \
    -I$(CURDIR) \
    -I$(CURDIR)/net \
    -I$(CLIENTDIR)/gamecube \
    -I$(CLIENTDIR)/gamecube/net

MEMORY_DEFS := \
    -DWII_LOADER_BASE=$(WII_LOADER_BASE) \
    -DWII_MEM1_TOP=$(WII_MEM1_TOP) \
    -DWII_LOADER_SIZE=$(WII_LOADER_SIZE) \
    -DWII_USE_MEM2_IPC_ARENA=$(WII_USE_MEM2_IPC_ARENA) \
    -DGC_LOADER_BASE=$(WII_LOADER_BASE) \
    -DGC_MEM1_TOP=$(WII_MEM1_TOP) \
    -DGC_LOADER_SIZE=$(WII_LOADER_SIZE)

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

IP_DEFS := -DLOADER_NAME=\"wii-load-ip\" -DMAX_RAM_BYTES=92274688
# wii-load-ip does NOT support static IP: IOS owns the network stack and
# the SOSetInterfaceOpt(0x4003) runtime override path was tested and didn't
# take.  Users that need a stable IP should either configure a static IP in
# the Wii Settings UI (which IOS picks up via NCD config) or use a DHCP
# reservation on their router.  The KOSLD_IP block stays at the default
# "0.0.0.0" — the patched value would be ignored by the Wii loader anyway,
# so we don't expose a STATICIP knob here.

IP_LD  := $(IPDIR)/wii-load-ip.ld
HBC_LD := $(IPDIR)/hbc_stub.ld

.DEFAULT_GOAL := all

VERSION_H    := $(ROOT)/include/kosload/version.h
VERSION_H_IN := $(ROOT)/include/kosload/version.h.in
COMMON_CONFIG_STAMP := $(COMMONDIR)/common-config.stamp

$(VERSION_H): $(VERSION_H_IN) $(ROOT)/mk/version.mk
	@sed -e 's/@KOSLOAD_VERSION_MAJOR@/$(KOSLOAD_VERSION_MAJOR)/g' \
	     -e 's/@KOSLOAD_VERSION_MINOR@/$(KOSLOAD_VERSION_MINOR)/g' \
	     -e 's/@KOSLOAD_VERSION_PATCH@/$(KOSLOAD_VERSION_PATCH)/g' \
	     -e 's/@KOSLOAD_VERSION@/$(KOSLOAD_VERSION)/g' \
	     $< > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

$(IP_LD): wii-load-ip.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@WII_LOADER_BASE@/$(WII_LOADER_BASE)/g' \
	     -e 's/@WII_LOADER_SIZE@/$(WII_LOADER_SIZE)/g' \
	     -e 's/@WII_MEM1_TOP@/$(WII_MEM1_TOP)/g' \
	     $< > $@
	@echo "  GEN     $@"

$(HBC_LD): hbc_stub.ld.in $(ROOT)/mk/memory.mk | $(IPDIR)
	@sed -e 's/@WII_HBC_BASE@/$(WII_HBC_BASE)/g' \
	     $< > $@
	@echo "  GEN     $@"

$(COMMON_CONFIG_STAMP): | $(COMMONDIR)
	@printf '%s\n' 'WII_USE_MEM2_IPC_ARENA=$(WII_USE_MEM2_IPC_ARENA)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

COMMON_C_OBJS := \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o \
    $(COMMONDIR)/target.o \
    $(COMMONDIR)/video.o \
    $(COMMONDIR)/cache.o \
    $(COMMONDIR)/memfuncs.o \
    $(COMMONDIR)/ios_net.o

COMMON_S_OBJS := \
    $(COMMONDIR)/crt0.o \
    $(COMMONDIR)/go.o \
    $(COMMONDIR)/exception.o

COMMON_OBJS := $(COMMON_C_OBJS) $(COMMON_S_OBJS)
COMMON_A    := $(COMMONDIR)/kosload_wii_common.a

$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/target.o: target.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/video.o: $(CLIENTDIR)/gamecube/video.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/cache.o: $(CLIENTDIR)/gamecube/cache.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/memfuncs.o: $(CLIENTDIR)/gamecube/memfuncs.c $(VERSION_H) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/ios_net.o: ios_net.c ios_net.h $(VERSION_H) $(COMMON_CONFIG_STAMP) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(COMMONDIR)/crt0.o: $(CLIENTDIR)/gamecube/crt0.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/go.o: $(CLIENTDIR)/gamecube/go.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMONDIR)/exception.o: $(CLIENTDIR)/gamecube/exception.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMON_A): $(COMMON_OBJS)
	$(AR) rcs $@ $^

IP_OBJS := \
    $(IPDIR)/main.o \
    $(IPDIR)/entry.o \
    $(IPDIR)/network_transport.o \
    $(IPDIR)/network_syscalls.o \
    $(IPDIR)/commands.o \
    $(IPDIR)/adapter.o \
    $(IPDIR)/net.o \
    $(IPDIR)/packet.o \
    $(IPDIR)/packet_helpers.o \
    $(IPDIR)/dhcp.o

IP_ELF := $(IPDIR)/wii-load-ip.elf
IP_BIN := $(IPDIR)/wii-load-ip.bin
IP_DOL := $(IPDIR)/wii-load-ip.dol
IP_CONFIG_STAMP := $(IPDIR)/ip-config.stamp

$(IP_CONFIG_STAMP): | $(IPDIR)
	@printf '%s\n' 'WII_IOS_SOCKET_SHIM=clean-room-experimental-ipc' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; echo "  GEN     $@"; else rm $@.tmp; fi

$(IPDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/entry.o: $(CLIENTDIR)/common/network/entry.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_transport.o: $(CLIENTDIR)/common/network/network_transport.c $(VERSION_H) $(IP_CONFIG_STAMP) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/network_syscalls.o: $(CLIENTDIR)/common/network/network_syscalls.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/commands.o: $(CLIENTDIR)/common/core/commands.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/adapter.o: net/adapter.c ios_net.h $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/net.o: $(CLIENTDIR)/common/network/net.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet.o: $(CLIENTDIR)/gamecube/net/packet.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/packet_helpers.o: $(CLIENTDIR)/common/network/packet_helpers.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

$(IPDIR)/dhcp.o: $(CLIENTDIR)/common/network/dhcp.c $(VERSION_H) | $(IPDIR)
	$(CC) $(CFLAGS) $(IP_DEFS) -c $< -o $@

WII_IP_LDFLAGS := -nostdlib -Wl,-n -Wl,--gc-sections

$(IP_ELF): $(IP_OBJS) $(COMMON_A) $(IP_LD)
	$(CC) $(ARCH_CFLAGS) $(WII_IP_LDFLAGS) -T $(IP_LD) \
	    $(IP_OBJS) -Wl,--start-group $(COMMON_A) -lgcc -Wl,--end-group \
	    -o $@

$(IP_BIN): $(IP_ELF)
	$(OBJCOPY) -O binary $< $@

$(DOLTOOL): $(ROOT)/make-cd/tools/elf2dol.c | $(BUILDDIR)
	$(HOST_CC) $< -o $@

# ---- HBC-launch stub --------------------------------------------------
# Tiny DOL HBC will actually accept (links at WII_HBC_BASE); embeds the
# Stage-2 loader image ($(IP_BIN), linked at WII_LOADER_BASE) via .incbin,
# relocates it up and jumps.  -I$(IPDIR) lets .incbin find wii-load-ip.bin.
HBC_OBJ := $(IPDIR)/hbc_stub.o
HBC_ELF := $(IPDIR)/wii-load-ip-hbc.elf

$(HBC_OBJ): hbc_stub.S $(IP_BIN) | $(IPDIR)
	$(CC) $(ASFLAGS) -I$(IPDIR) -Wa,-I,$(IPDIR) -c $< -o $@

$(HBC_ELF): $(HBC_OBJ) $(HBC_LD)
	$(CC) $(ARCH_CFLAGS) -nostdlib -Wl,-n -Wl,--gc-sections -T $(HBC_LD) \
	    $(HBC_OBJ) -o $@

# The DOL handed to HBC is the stub, not the high-linked loader.
$(IP_DOL): $(HBC_ELF) $(DOLTOOL)
	$(DOLTOOL) $< $@

# ---- Wii example guests -----------------------------------------------
# Same client/examples/*/main.c sources as the other consoles, but linked
# at the Wii guest base (0x80004000 via wii-example.ld) and told the
# loader/syscall base is the *Wii* one (WII_LOADER_BASE 0x817c0000, NOT
# GC's 0x817ec000) through -DWII_KOSLOAD_BASE.  These give wii-load-ip
# something known-good to `-x` for exercising the LOADBIN/PARTBIN path.
EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

# Platform-independent set (Wii has no EXI/BBA/ENC/Maple hardware, so the
# GC NIC/probe tests are excluded).
WII_EXAMPLES := \
    console-test download-test exit-test file-test syscall-test \
    exception-test exception-frame-test gdb-test rainbow-test font-test \
    ip-calibrator

WII_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(WII_EXAMPLES)))

$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(EXDIR)/wii-example.ld | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) -O2 -ffreestanding -fno-zero-initialized-in-bss -nostdlib \
	    -DWII_KOSLOAD_BASE=$(WII_LOADER_BASE) \
	    -T $(EXDIR)/wii-example.ld $< -o $@

examples: $(WII_EXAMPLE_ELFS)

.PHONY: all ip examples clean

all: ip examples

ip: $(IP_ELF) $(IP_BIN) $(HBC_ELF) $(IP_DOL)
	@$(SIZE) $(IP_ELF)
	@$(SIZE) $(HBC_ELF)

$(COMMONDIR) $(IPDIR) $(EXBUILD) $(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

# Auto-generated header deps (-MMD).  `-include` (leading dash) is silent on
# missing files, so the first build after `clean` works without any .d files.
-include $(COMMON_OBJS:.o=.d) $(IP_OBJS:.o=.d)
