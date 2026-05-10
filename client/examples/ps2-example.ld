/* Minimal linker script for PS2 freestanding example programs.
 * Prevents GCC's default bare-metal script from placing sections
 * at invalid addresses for the PS2 EE environment. */

OUTPUT_FORMAT("elf32-nlittlemips", "elf32-nbigmips", "elf32-nlittlemips")
OUTPUT_ARCH(mips)

SECTIONS {
    /* Programs land at 0x00100000.  Once bootstrap_main has copied the
     * inner loader to 0x80000280 (KSEG0) and ERET'd there, the outer
     * bootstrap's text/data at 0x00100000..0x001FFFFF is no longer
     * referenced — examples and any future loadable program can reuse
     * that region. */
    . = 0x00100000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) *(.MIPS.abiflags) *(.reginfo) }
}
