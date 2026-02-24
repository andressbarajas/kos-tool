/* Minimal linker script for GameCube freestanding example programs.
 * Prevents GCC's default bare-metal script from creating a .stack
 * segment at an invalid address (which crashes legacy loaders). */

OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc)

SECTIONS {
    . = 0x80100000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
