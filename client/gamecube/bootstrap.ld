/* Bootstrap linker script for kosload (GameCube)
 *
 * Loads at 0x81300000 — below the loader's target region at the
 * top of MEM1 (GC_LOADER_BASE, set by mk/memory.mk).
 * The bootstrap copies the embedded loader binary to GC_LOADER_BASE
 * and jumps there.
 */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(start)

MEMORY
{
  /* 512KB region starting at 0x81300000 — enough for bootstrap code
   * plus the embedded loader binary (~50KB).  Well below MEM1 top
   * (0x81800000) so it won't conflict with anything. */
  bootstrap (rwx) : ORIGIN = 0x81300000, LENGTH = 0x80000
}

SECTIONS
{
  .text : {
    *(.text.bootstrap)
    *(.text)
    *(.text.*)
  } > bootstrap

  /* Embedded loader binary (from objcopy -I binary).
   * 32-byte aligned for cache line operations. */
  .loader_bin ALIGN(32) : {
    *(.data)
    *(.data.*)
  } > bootstrap

  _end = .;

  /* Discard everything we don't need */
  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame)
    *(.eh_frame_hdr)
  }
}
