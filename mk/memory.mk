# mk/memory.mk — loader memory reservations
#
# Dreamcast: the loader lives at a fixed low address, just above the 16 KB
# reserved at the bottom of SH4 RAM (IP.BIN / boot area). Both dc-load-serial
# and dc-load-ip must fit within DC_LOADER_SIZE (code + data + BSS + stack).
# The stack top is derived in the linker script as ORIGIN(ram) + LENGTH(ram).
DC_LOADER_BASE := 0x8c004000
DC_LOADER_SIZE := 0xb400        # ~45 KB
#
# GameCube: the loader lives at the top of MEM1. Adjust GC_LOADER_SIZE to
# resize the reservation. Both gc-load-serial and gc-load-ip must fit within
# this region (code + data + BSS + stack).
#
# Before launching a loaded program, ArenaHi is set to the loader base so
# guest-side heap allocators stay below the loader.

GC_MEM1_TOP    := 0x81800000
GC_LOADER_SIZE := 0x14000      # 80 KB
GC_LOADER_BASE := 0x817ec000   # = GC_MEM1_TOP - GC_LOADER_SIZE

# Wii mode has the same 24 MB MEM1 window as GameCube plus MEM2.  Keep the
# initial clean-room loader in high MEM1 so uploaded Wii DOLs can use the usual
# low MEM1 area while the IOS socket shim is being brought up.
#
# NOTE: the top of MEM1 (the real 0x81800000) is NOT usable when launched as a
# System-Menu channel — ES_LaunchTitle leaves only ~0x81380000 mapped/executable
# (the very top is the SM's XFB/reserved region; jumping to 0x817c0000 there
# faults with an ISI). Retail channel executables run their payload at
# 0x81330000, so we put the loader there. This stays well clear of HBC's top
# reservation too, so the same base works for both HBC and channel boot.
WII_MEM1_TOP     := 0x81370000  # channel-safe usable top
WII_LOADER_SIZE  := 0x40000     # 256 KB
WII_LOADER_BASE  := 0x81330000  # = WII_MEM1_TOP - WII_LOADER_SIZE

# HBC/System-Menu will only launch a DOL whose load addresses sit in the
# standard homebrew area of low MEM1 (the top of MEM1 is reserved for HBC + the
# XFB).  The DOL has two low PT_LOAD segments: a tiny stub at WII_BOOTSTRAP_BASE
# (the fixed DOL entry) and the embedded loader blob at WII_HBC_BASE.  The stub
# relocates that loader up to WII_LOADER_BASE and jumps (cf. PS2 -F trampoline).
WII_BOOTSTRAP_BASE := 0x80003400  # fixed ES_LaunchTitle/HBC channel DOL entry (.stub)
WII_HBC_BASE       := 0x80004000  # canonical devkitPPC/HBC homebrew base (.blob)

# The XFB must not sit in the area an uploaded payload occupies.  GameCube is
# safe with the default 0xC0050000 because its payloads load at 0x80100000,
# above the framebuffer.  The Wii loads at WII_HBC_BASE (0x80004000), which is
# BELOW it, so the default XFB is only ~311 KB clear of the load address and any
# larger upload is silently overwritten by the loader's own status drawing.
# Move it high instead, but keep the physical address under 16 MB so VI_TFBL can
# stay on the POFF=0 (raw physical) encoding — see gc_video_init().
# 0xC0F60000 = phys 0x00F60000, + 640*480*2 (0x96000) ends at 0x00FF6000.
WII_XFB_BASE       := 0xC0F60000  # phys 0x00F60000, must stay < 16 MB

# PS2 EE memory layout.  If you change these values, keep the matching
# constants in include/kosload/protocol.h and the PS2 linker templates in sync.
#
# The file users launch is a small outer/bootstrap ELF at PS2_BOOTSTRAP_BASE
# because common PS2 launchers expect an ELF at 0x00100000.  It copies the real
# loader down to PS2_LOADER_BASE (where the loader runs), flushes caches, and
# jumps there.  After that handoff, the original 0x00100000 launcher region is
# no longer used by kosload and is the default program load area
# (PS2_DEFAULT_LOAD_ADDR).
#
# Physical layout:
#   phys 0x000000..0x00027F  EE exception vectors + low globals
#   phys 0x000280..0x0FFFFF  inner loader        (PS2_LOADER_BASE, cached 0x80000280)
#   phys 0x100000..0x1FFFFF  outer/bootstrap ELF (PS2_BOOTSTRAP_BASE)
#                             reused as the default program load area
#   phys 0x200000..0x7FFFFF  continuation of program RAM
#   phys 0x800000..          remainder of 32 MB RAM
PS2_RAM_TOP        := 0x02000000
PS2_BOOTSTRAP_BASE := 0x00100000  # outer ELF landing zone (= PS2_DEFAULT_LOAD_ADDR)
PS2_BOOTSTRAP_SIZE := 0x100000    # 1 MB landing zone
PS2_LOADER_BASE    := 0x80000280  # where the real loader runs (cached phys 0x280)
PS2_LOADER_SIZE    := 0xFFD80     # 1 MB minus the 0x280 vector area

# Original Microsoft Xbox (Pentium III / nForce).  Xbox titles run in ring 0
# with paging on and flat segments.  The addresses declared by an XBE are
# VIRTUAL addresses: when the kernel first loads the image, its sections are
# backed by pages allocated from the physical free-page pool, so VA != PA.
#
# xbox-load-ip keeps the conventional homebrew XBE base (0x00010000).  After
# taking over from the kernel, it copies its runtime into the former resident
# kernel pages and rewrites its PTEs so VA == PA from 0x00011000 through the
# loader image.  The linker caps that identity-mapped image below 0x0003C000.
# The XBE header page at VA 0x00010000 is not part of the runtime relocation.
#
# Virtual layout:
#   VA 0x00010000..0x00010FFF  generated XBE headers (launch-time only)
#   VA 0x00011000..0x0003BFFF  loader identity window (maximum)
#   VA 0x0003C000..0x0203BFFF  mapped 32 MiB guest arena
#
# After takeover, only the loader window above has the same physical addresses.
# The guest arena remains kernel-paged and can be backed by scattered physical
# pages; its virtual addresses do not identify a corresponding physical portion
# of the Xbox's 64 MiB UMA RAM.  The remaining physical RAM also includes the
# framebuffer and other reservations.
#
# XBOX_KOSLOAD_BASE is the first byte after the generated XBE header page and
# is where the loader publishes its guest-facing header (magic + syscall entry
# + info-block pointer).  The linker forces .text.kosload_header there.
XBOX_RAM_TOP           := 0x04000000  # physical top of 64 MiB retail RAM
XBOX_LOADER_BASE       := 0x00010000  # XBE image base (conventional homebrew)
XBOX_LOADER_SIZE       := 0x0002C000  # max XBE header + loader window, ending at 0x0003C000
XBOX_HEADER_RESERVE    := 0x1000      # XBE header page (must match elf2xbe HEADER_RESERVE)
XBOX_KOSLOAD_BASE      := 0x00011000  # = XBOX_LOADER_BASE + XBOX_HEADER_RESERVE
XBOX_DEFAULT_LOAD_ADDR := 0x0003C000  # first guest VA, immediately after loader window

# Sony PSP (Allegrex / MIPS32-ish, single-float).  The PSP main RAM window is
# 0x08000000..0x09FFFFFF (32 MB on PSP-1000).  VRAM is the separate 2 MB block
# at 0x04000000 (used by the framebuffer console).
#
# There is one supported layout.  Stage 1 takes both exception vectors and opens
# the Tachyon protection table before its first low store, reclaiming the
# firmware's kernel + volatile partitions, so stage 2 lives at the very bottom
# of RAM and the guest gets ONE contiguous extent:
#
#   0x08000000..0x0801FFFF  psp-load-usb loader image (~125 KiB)
#   0x08020000..0x080EFFFF  slack inside the loader reservation
#   0x080F0000..0x080FFFFF  LZO work memory (top of the loader window)
#   0x08100000..0x09FFFFFF  guest extent (31.00 MiB contiguous, to the top of RAM)
#
# Putting the loader at the bottom rather than in the user partition does not
# change the total free byte count -- it removes the split.  Largest single
# allocation goes from 23.94 MiB to 31.00 MiB.  Folding the LZO work memory into
# the loader's own reservation removes the last carve-out: the guest extent runs
# unbroken to PSP_RAM_TOP and no download has to be checked against it.
#
# PSP_DEFAULT_LOAD_ADDR is 0x08804000, the address the firmware itself hands a
# user module (user partition + the 16 KB it reserves for usersystemlib).  The
# reservation is meaningless once stage 1 has reclaimed the partitions -- the
# whole extent is ours -- but keeping the convention means homebrew linked with
# a stock PSP script lands where it expects.  A guest that wants the entire
# 31 MiB extent asks for 0x08100000 explicitly.
PSP_RAM_TOP           := 0x0A000000  # end of 32 MB main RAM
PSP_DEFAULT_LOAD_ADDR := 0x08804000  # uploaded guest programs load here
PSP_VRAM_BASE         := 0x04000000  # framebuffer VRAM (uncached alias 0x44000000)

# Loader window: the bottom megabyte of main RAM.
#
# This overlaps the firmware's exception-handler BODIES at the base of the
# kernel partition, which is safe only because stage 1 points COP0 $25 and
# control $9 at its own stub before its first low store.  Masking Status.IE is
# not sufficient on its own: IE masks interrupts, not exceptions, and NMI
# through control $9 is not maskable at all, so without that takeover a bus or
# address error during the copy would vector into memory stage 1 had just
# overwritten.  Do not move the loader here without keeping that ordering.
#
# Stage 1's own PRX is loaded in the user partition, so source, copier and
# destination are disjoint; the guard below rejects any base that would put
# them back in the same region.
#
# The host needs the same answer: its -F trampoline copies to PSP_LOADER_BASE.
# Both makefiles include this file, and perform_update() re-verifies the
# embedded image against the base, so a stale pairing is refused, not executed.
PSP_LOADER_BASE       := 0x08000000
PSP_LOADER_SIZE       := 0x00100000

# Lowest address the firmware can hand a user module.  Stage 1 runs from
# wherever the firmware put the PRX, which is at or above this, so keeping the
# loader window strictly below it is what makes source, copier and destination
# three disjoint intervals -- the copy can then never overwrite the code running
# it, whatever base the firmware picked.  stub.S still re-derives its own
# address and re-checks before the first store, because only the runtime knows
# the real base.
PSP_USER_MODULE_MIN   := 0x08800000

ifneq ($(shell test $$(($(PSP_LOADER_BASE) + $(PSP_LOADER_SIZE))) -le $$(($(PSP_USER_MODULE_MIN))) && echo ok),ok)
  $(error PSP loader window $(PSP_LOADER_BASE)+$(PSP_LOADER_SIZE) reaches the user partition at $(PSP_USER_MODULE_MIN); stage 1 would overwrite the range it runs from)
endif

# LZO work memory for compressed downloads lives in the TOP of the loader
# window, not below the top of RAM.  The image is ~125 KiB of a 1 MiB
# reservation, so it is free real estate, and it leaves the guest extent
# unbroken all the way to PSP_RAM_TOP.  Must equal LZO_WRKMEM_SIZE in
# include/kosload/protocol.h; target.c static-asserts that.
PSP_LZO_WRKMEM_SIZE   := 0x10000

# Firmware partition boundaries (verified against the PSP kernel memory manager).
PSP_KERNEL_BASE       := 0x08000000  # firmware kernel partition, 4 MB
PSP_VOLATILE_BASE     := 0x08400000  # firmware volatile partition, 4 MB
PSP_USER_BASE         := 0x08800000  # firmware user partition

# Guest arena: the full 0x08000000..0x0A000000 main-RAM window = 32 MiB.
# Stage 1 (client/psp/stub/stub.S) performs the unlock before its first low
# store -- ME reset, four F writes, four readbacks, then a whole-8-MiB cache
# purge -- because it copies stage 2 into the reclaimed range.  Stage 2's
# exception_init() re-verifies the same state idempotently and fails closed.
#
# PSP-1000 hardware proof (2026-08-10): inherited protection was C,C,0,0.
# After the four F writes, KU0/KU1/K0/K1 all responded for both partitions.  A
# deterministic 8-MiB image was uploaded through K1, downloaded byte-identically
# through KU1 and K1, and the original 8 MiB was restored and verified through
# both aliases.
PSP_RAM_BYTES         := 33554432
