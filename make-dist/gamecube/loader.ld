/* Linker script for kosload GameCube bootstrap (disc boot).
 *
 * Loads at 0x80003100 — the standard GameCube DOL entry point,
 * just after the exception vectors.  The apploader copies this
 * from disc and jumps here.
 *
 * The bootstrap then copies the embedded kosload binary to
 * GC_LOADER_BASE at the top of MEM1 and jumps there.
 */

OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc)
ENTRY(start)

SECTIONS {
    . = 0x80003100;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata ALIGN(32) : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
