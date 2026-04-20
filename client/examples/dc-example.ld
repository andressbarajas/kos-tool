/* Minimal linker script for Dreamcast freestanding example programs.
 * Prevents GCC's default bare-metal script from creating a .stack
 * segment at 0x3fffff00 (which crashes legacy loaders). */

OUTPUT_FORMAT("elf32-shl")
OUTPUT_ARCH(sh)

PHDRS {
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS {
    . = 0x8c010000;
    .text   : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .data   : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
