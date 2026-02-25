# mk/memory.mk — GameCube loader memory reservation
#
# The loader lives at the top of MEM1.  Adjust GC_LOADER_SIZE to
# resize the reservation.  Both gc-load-serial and gc-load-ip must
# fit within this region (code + data + BSS + stack).
#
# Before launching a loaded program, ArenaHi is set to the loader
# base so that libogc/devkitPPC heaps stay below the loader.

GC_MEM1_TOP    := 0x81800000
GC_LOADER_SIZE := 0x14000    # 80 KB
