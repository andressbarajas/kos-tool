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
