/* Minimal linker script for Wii freestanding example programs.
 *
 * Wii guests uploaded by wii-load-ip run from 0x80004000 (the standard
 * low-MEM1 homebrew/guest area).  This is distinct from the GameCube
 * example base (0x80003100) and well clear of the resident loader at
 * WII_LOADER_BASE (0x817c0000).  As with the GC script, this overrides
 * GCC's default bare-metal script so no bogus .stack segment is emitted.
 */

OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc)

PHDRS {
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS {
    . = 0x80004000;
    .text   : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .data   : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
