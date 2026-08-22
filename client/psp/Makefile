# client/psp/Makefile — builds the PSP USB loader (psp-load-usb)
#
# The USB transport reuses the shared kosload serial byte protocol, backed by
# the PSP USB bulk endpoints (serial_io.c -> usb/usb_dev.c).  Output is a static
# Two-stage boot.  Stage 2 (psp-load-usb.elf/.bin) is the real loader, linked at
# the fixed PSP_LOADER_BASE.  Stage 1 (stub/) is a small relocatable PRX that
# carries stage 2 as a payload, copies it to that address and jumps to it; that
# is what host/tools/elf2prx converts and host/tools/pbp wraps as an EBOOT.PBP.
# The fixed base and the two-stage trampoline are both required because the
# firmware loads stage 1 wherever it likes; see the header of stub/stub.S.

ROOT      ?= $(CURDIR)/../..
CLIENTDIR := $(CURDIR)/..

include $(ROOT)/mk/version.mk
include $(ROOT)/mk/memory.mk
include $(ROOT)/mk/toolchains.mk

# ---------- Toolchain ----------

PREFIX  := $(PSP_PREFIX)
CC      := $(PSP_TOOLCHAIN)/$(PREFIX)gcc
AR      := $(PSP_TOOLCHAIN)/$(PREFIX)ar
OBJCOPY := $(PSP_TOOLCHAIN)/$(PREFIX)objcopy
SIZE    := $(PSP_TOOLCHAIN)/$(PREFIX)size

HOSTCC  ?= cc
PBP     := $(ROOT)/host/tools/pbp/pbp
ELF2PRX := $(ROOT)/host/tools/elf2prx/elf2prx

# ---------- Directories ----------

BUILDDIR  := build
COMMONDIR := $(BUILDDIR)/common
USBDIR    := $(BUILDDIR)/usb
MINILZO_DIR := $(ROOT)/third_party/minilzo
MINILZO_A   := $(MINILZO_DIR)/build-psp/minilzo.a

# ---------- Flags ----------

# Allegrex: MIPS2, single-float (toolchain default; do NOT emit double-float).
# -G0 keeps small globals out of gp-relative addressing so the hand-rolled
# linker script + module descriptor stay simple.
# -mno-check-zero-division: GCC otherwise emits a `teq` divide-by-zero trap
# after every integer division.  The Allegrex does not implement the MIPS
# trap instructions -- hardware raises Reserved Instruction on `teq` (seen
# at uint_to_string+0x14 while formatting the upload progress counter), so
# any division in loader or example code would crash the console.
ARCH_CFLAGS  := -march=mips2 -mno-abicalls -fno-pic -G0 -O2 -nostdlib -nostartfiles -Wall -Wextra \
                -mno-check-zero-division
ARCH_ASFLAGS := -march=mips2 -mno-abicalls -fno-pic -G0

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
    -ffunction-sections \
    -fdata-sections

INCLUDES := -I$(ROOT)/include -I$(CLIENTDIR)/include -I$(CURDIR) -I$(CURDIR)/usb \
            -I$(ROOT)/third_party/minilzo

# The memory map lives in mk/memory.mk, which the host includes too -- see the
# note there.  The loader masks interrupts, owns both exception vectors, holds
# the Media Engine in reset and opens the low-8-MiB protection table before
# advertising the arena.
MEMORY_DEFS := -DPSP_LOADER_BASE=$(PSP_LOADER_BASE) -DPSP_RAM_TOP=$(PSP_RAM_TOP) \
               -DPSP_LOADER_SIZE=$(PSP_LOADER_SIZE) \
               -DPSP_LZO_WRKMEM_SIZE=$(PSP_LZO_WRKMEM_SIZE) \
               -DPSP_VRAM_BASE=$(PSP_VRAM_BASE) \
               -DPSP_KERNEL_BASE=$(PSP_KERNEL_BASE) \
               -DPSP_USER_BASE=$(PSP_USER_BASE) \
               -DPSP_RAM_BYTES=$(PSP_RAM_BYTES)

USB_DEFS := -DKOSLOAD_SERIAL_TRANSPORT -DLOADER_NAME=\"psp-load-usb\" \
            -DMAX_RAM_BYTES=$(PSP_RAM_BYTES)

CFLAGS  := $(ARCH_CFLAGS) $(FREE_CFLAGS) $(INCLUDES) $(MEMORY_DEFS) $(USB_DEFS) \
           $(KOSLOAD_VERSION_DEFS) \
           -MMD -MP
ASFLAGS := $(ARCH_ASFLAGS) $(INCLUDES) $(MEMORY_DEFS) $(USB_DEFS) -MMD -MP

# -u module_info: module_info.o lives in kosload_common.a and nothing
# references its symbol, so without this the linker never extracts the member
# and the ELF/PBP ships WITHOUT the sceModuleInfo descriptor (KEEP() in the
# linker script cannot retain a section that was never pulled in).
LDFLAGS := -nostartfiles -nostdlib -static -Wl,--gc-sections -Wl,--no-undefined \
           -Wl,-u,module_info

# The stage-1 stub is the only image the firmware relocates.  --emit-relocs
# keeps the resolved relocation records so elf2prx can rewrite them into PRX
# relocations; without it the PRX has no relocations at all and every absolute
# reference still points at the base-0 link addresses.
STUB_LDFLAGS := -nostartfiles -nostdlib -static -Wl,--no-undefined \
                -Wl,-u,module_info -Wl,--emit-relocs

.DEFAULT_GOAL := all

# ---------- Linker script ----------

USB_LD := $(USBDIR)/kosload-usb.ld

.PHONY: FORCE_PSP_IMAGE
FORCE_PSP_IMAGE:

$(USB_LD): kosload-usb.ld.in $(ROOT)/mk/memory.mk FORCE_PSP_IMAGE | $(USBDIR)
	@sed -e 's/@PSP_LOADER_BASE@/$(PSP_LOADER_BASE)/g' \
	     -e 's/@PSP_LOADER_SIZE@/$(PSP_LOADER_SIZE)/g' \
	     -e 's/@PSP_LZO_WRKMEM_SIZE@/$(PSP_LZO_WRKMEM_SIZE)/g' \
	     $< > $@
	@echo "  GEN     $@"

# ---------- Common objects ----------

COMMON_OBJS := \
    $(COMMONDIR)/info.o \
    $(COMMONDIR)/display.o \
    $(COMMONDIR)/screensaver.o \
    $(COMMONDIR)/syscall_table.o \
    $(COMMONDIR)/target.o \
    $(COMMONDIR)/video.o \
    $(COMMONDIR)/syscon.o \
    $(COMMONDIR)/power.o \
    $(COMMONDIR)/memfuncs.o \
    $(COMMONDIR)/module_info.o \
    $(COMMONDIR)/exception.o \
    $(COMMONDIR)/exception_entry.o \
    $(COMMONDIR)/serial_io.o \
    $(COMMONDIR)/usb_dev.o \
    $(COMMONDIR)/crt0.o \
    $(COMMONDIR)/go.o
COMMON_A := $(COMMONDIR)/kosload_common.a

$(COMMONDIR)/info.o: $(CLIENTDIR)/common/core/info.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/display.o: $(CLIENTDIR)/common/core/display.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/screensaver.o: $(CLIENTDIR)/common/core/screensaver.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/syscall_table.o: $(CLIENTDIR)/common/core/syscall_table.c $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
# These sources consume command-line PSP configuration without changing their
# object names.  Rebuild them whenever an image is requested so mode/reclaim/
# memory-map switches cannot reuse objects from a previous invocation.
$(COMMONDIR)/target.o: target.c video.h cache.h $(KOSLOAD_VERSION_MK) FORCE_PSP_IMAGE | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/video.o: video.c video.h syscon.h $(KOSLOAD_VERSION_MK) FORCE_PSP_IMAGE | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/syscon.o: syscon.c syscon.h $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/power.o: power.c power.h syscon.h video.h $(KOSLOAD_VERSION_MK) | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/memfuncs.o: memfuncs.c | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/module_info.o: module_info.c | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/exception.o: exception.c video.h cache.h hw_trace.h $(KOSLOAD_VERSION_MK) FORCE_PSP_IMAGE | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/exception_entry.o: exception.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(COMMONDIR)/serial_io.o: serial_io.c usb/usb_dev.h power.h $(KOSLOAD_VERSION_MK) FORCE_PSP_IMAGE | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/usb_dev.o: usb/usb_dev.c usb/usb_dev.h cache.h hw_trace.h $(KOSLOAD_VERSION_MK) FORCE_PSP_IMAGE | $(COMMONDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(COMMONDIR)/crt0.o: crt0.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@
$(COMMONDIR)/go.o: go.S | $(COMMONDIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(COMMON_A): $(COMMON_OBJS) FORCE_PSP_IMAGE
	$(AR) rcs $@ $(COMMON_OBJS)

# ---------- Serial-over-USB transport objects ----------

USB_OBJS := \
    $(USBDIR)/main.o \
    $(USBDIR)/entry.o \
    $(USBDIR)/serial_transport.o \
    $(USBDIR)/serial_syscalls.o \

$(USBDIR)/main.o: $(CLIENTDIR)/common/core/main.c $(KOSLOAD_VERSION_MK) | $(USBDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(USBDIR)/entry.o: $(CLIENTDIR)/common/serial/entry.c $(KOSLOAD_VERSION_MK) | $(USBDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(USBDIR)/serial_transport.o: $(CLIENTDIR)/common/serial/serial_transport.c $(KOSLOAD_VERSION_MK) | $(USBDIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(USBDIR)/serial_syscalls.o: $(CLIENTDIR)/common/serial/serial_syscalls.c $(KOSLOAD_VERSION_MK) | $(USBDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---------- minilzo (cross-compiled for PSP) ----------

$(MINILZO_A):
	$(MAKE) -C $(MINILZO_DIR) CC="$(CC)" AR="$(AR)" \
	    CFLAGS="$(ARCH_CFLAGS) $(FREE_CFLAGS)" BUILDDIR=build-psp

# ---------- Link + package ----------

USB_ELF := $(USBDIR)/psp-load-usb.elf
USB_BIN := $(USBDIR)/psp-load-usb.bin
USB_PBP := $(USBDIR)/EBOOT.PBP

$(USB_ELF): $(USB_OBJS) $(COMMON_A) $(MINILZO_A) $(USB_LD) FORCE_PSP_IMAGE
	$(CC) $(ARCH_CFLAGS) $(LDFLAGS) -T $(USB_LD) \
	    $(USB_OBJS) -Wl,--start-group $(COMMON_A) $(MINILZO_A) -lgcc -Wl,--end-group \
	    -o $@
	@$(SIZE) $@ 2>/dev/null || true

$(USB_BIN): $(USB_ELF) FORCE_PSP_IMAGE
	@$(OBJCOPY) -O binary $< $@
	@echo "  OBJCOPY $@"

# ---------- Stage 1: relocating PRX stub ----------

STUBDIR  := $(BUILDDIR)/stub
STUB_LD  := $(STUBDIR)/kosload-stub.ld
STUB_ELF := $(STUBDIR)/psp-load-usb-stub.elf
STUB_PRX := $(STUBDIR)/psp-load-usb-stub.prx

# The stub must reserve the whole stage-2 footprint (see the .ld.in), so this is
# generated only once stage 2 has been linked and its _end is known.
$(STUB_LD): stub/kosload-stub.ld.in $(USB_ELF) FORCE_PSP_IMAGE | $(STUBDIR)
	@end=$$($(PSP_TOOLCHAIN)/$(PREFIX)nm $(USB_ELF) | awk '$$3 == "_end" { print $$1 }'); \
	 memsz=$$(( 0x$$end - $(PSP_LOADER_BASE) )); \
	 sed -e 's/@PSP_LOADER_BASE@/$(PSP_LOADER_BASE)/g' \
	     -e "s/@PSP_STAGE2_MEMSZ@/$$memsz/g" $< > $@; \
	 echo "  GEN     $@ (stage-2 footprint $$memsz bytes)"

# stub.S bakes the memory map into its instructions without that showing up in
# the target name, so a command-line PSP_LOADER_BASE override would otherwise
# reuse the previous build's stub.  Rebuild this tiny object every invocation.
.PHONY: FORCE_STUB
FORCE_STUB:

$(STUBDIR)/stub.o: stub/stub.S $(USB_BIN) FORCE_STUB | $(STUBDIR)
	$(CC) $(ASFLAGS) -DPAYLOAD_FILE=\"$(CURDIR)/$(USB_BIN)\" -c $< -o $@

$(STUBDIR)/module_info.o: module_info.c $(KOSLOAD_VERSION_MK) | $(STUBDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STUBDIR)/syscon.o: syscon.c syscon.h | $(STUBDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# syscon.o provides the watchdog disable stage 1 performs before its first low
# store.  --gc-sections is not used on the relocatable stub link.
STUB_OBJS := $(STUBDIR)/stub.o $(STUBDIR)/module_info.o $(STUBDIR)/syscon.o

$(STUB_ELF): $(STUB_OBJS) $(STUB_LD) FORCE_PSP_IMAGE
	$(CC) $(ARCH_CFLAGS) $(STUB_LDFLAGS) -T $(STUB_LD) $(STUB_OBJS) -o $@
	@$(SIZE) $@ 2>/dev/null || true

$(STUB_PRX): $(STUB_ELF) $(ELF2PRX) FORCE_PSP_IMAGE
	@$(ELF2PRX) -o $@ $(STUB_ELF)
	@echo "  PRX     $@"

$(ELF2PRX): $(ROOT)/host/tools/elf2prx/elf2prx.c $(ROOT)/host/tools/elf2prx/Makefile
	@$(MAKE) -C $(ROOT)/host/tools/elf2prx CC=$(HOSTCC)

$(PBP): $(ROOT)/host/tools/pbp/pbp.c $(ROOT)/host/tools/pbp/Makefile
	@$(MAKE) -C $(ROOT)/host/tools/pbp CC=$(HOSTCC)

$(USB_PBP): $(STUB_PRX) $(PBP) FORCE_PSP_IMAGE
	@$(PBP) -o $@ -t "psp-load-usb" $(STUB_PRX)
	@echo "  PBP     $@"

# ---------- Examples (freestanding test programs) ----------

EXDIR   := $(CLIENTDIR)/examples
EXBUILD := $(BUILDDIR)/examples

PSP_EXAMPLES := \
    console-test download-test exception-test exception-frame-test \
    exit-test file-test gdb-test syscall-test font-test

PSP_EXAMPLE_ELFS := $(addprefix $(EXBUILD)/,$(addsuffix .elf,$(PSP_EXAMPLES)))

# PSP_KOSLOAD_BASE points at the start of the *header* fields the example
# reads (magic at +4, syscall ptr at +8), NOT the entry instruction at
# PSP_LOADER_BASE.  Same trick as PS2: crt0 is `j realstart` (+0), delay-slot
# nop (+4), magic (+8), syscall ptr (+12), so the shared DC/GC example
# pattern works with BASE = PSP_LOADER_BASE + 4.
#
# FORCE_PSP_IMAGE for the same reason the loader objects use it: a command-line
# PSP_LOADER_BASE override changes the header address without changing any
# object name, and a stale example would hunt for the magic at an address the
# loader no longer occupies.
$(EXBUILD)/%.elf: $(EXDIR)/%/main.c $(EXDIR)/psp-example.ld FORCE_PSP_IMAGE | $(EXBUILD)
	@echo "  CC      $@"
	@$(CC) $(ARCH_CFLAGS) $(FREE_CFLAGS) -g \
	    -DPSP_KOSLOAD_BASE=$$(($(PSP_LOADER_BASE) + 4)) \
	    -DPSP_LOADER_BASE=$(PSP_LOADER_BASE) \
	    -DPSP_LOADER_SIZE=$(PSP_LOADER_SIZE) \
	    -T $(EXDIR)/psp-example.ld $< -o $@

examples: $(PSP_EXAMPLE_ELFS)

# ---------- Phony ----------

.PHONY: all usb examples clean

all: usb examples

usb: $(USB_ELF) $(USB_BIN) $(STUB_PRX) $(USB_PBP)

$(COMMONDIR) $(USBDIR) $(STUBDIR) $(EXBUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C $(MINILZO_DIR) BUILDDIR=build-psp clean

-include $(COMMON_OBJS:.o=.d) $(USB_OBJS:.o=.d)
