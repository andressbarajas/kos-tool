/* Minimal linker script for GameCube freestanding example programs.
 * Prevents GCC's default bare-metal script from creating a .stack
 * segment at an invalid address (which crashes legacy loaders). */

OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc)

PHDRS {
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS {
    . = 0x80003100;
    .text   : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .data   : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) }
}
