# mk/memory.mk — loader memory reservations
#
# GameCube: the loader lives at the top of MEM1. Adjust GC_LOADER_SIZE to
# resize the reservation. Both gc-load-serial and gc-load-ip must fit within
# this region (code + data + BSS + stack).
#
# Before launching a loaded program, ArenaHi is set to the loader base so
# that libogc/devkitPPC heaps stay below the loader.

GC_MEM1_TOP    := 0x81800000
GC_LOADER_SIZE := 0x14000    # 80 KB

# PS2 EE memory layout.  If you change these values, keep the matching
# constants in include/kosload/protocol.h and the PS2 linker templates in sync.
#
# The file users launch is a small outer ELF at PS2_LOADER_BASE because common
# PS2 launchers expect an ELF at 0x00100000.  It copies the real loader down
# to PS2_INNER_LOADER_BASE, flushes caches, and jumps there.  After that
# handoff, the original 0x00100000 launcher region is no longer used by
# kosload and is the default load address for programs.
#
# Physical layout:
#   phys 0x000000..0x00027F  EE exception vectors + low globals
#   phys 0x000280..0x0FFFFF  inner loader
#   phys 0x100000..0x1FFFFF  outer/bootstrap ELF landing zone
#                             reused as the default program load area
#   phys 0x200000..0x7FFFFF  continuation of program RAM
#   phys 0x800000..          remainder of 32 MB RAM
PS2_RAM_TOP              := 0x02000000
PS2_LOADER_SIZE          := 0x100000    # 1 MB landing zone
PS2_LOADER_BASE          := 0x00100000  # where the launcher loads the outer ELF
PS2_INNER_LOADER_SIZE    := 0xFFD80     # 1 MB minus the 0x280 vector area
PS2_INNER_LOADER_BASE    := 0x80000280  # cached view of physical 0x280
PS2_BOOTSTRAP_BASE       := 0x00100000  # alias of PS2_LOADER_BASE
PS2_BOOTSTRAP_SIZE       := 0x100000
