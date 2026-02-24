/* Minimal linker script for Dreamcast freestanding example programs.
 * Prevents GCC's default bare-metal script from creating a .stack
 * segment at 0x3fffff00 (which crashes legacy loaders). */

OUTPUT_FORMAT("elf32-shl")
OUTPUT_ARCH(sh)

SECTIONS {
    . = 0x8c010000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
