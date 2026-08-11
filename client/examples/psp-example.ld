/* Minimal linker script for PSP freestanding example programs.
 * Programs land at PSP_DEFAULT_LOAD_ADDR (0x08804000), the address the firmware
 * itself hands a user module, and well clear of the loader window at
 * 0x08000000..0x080FFFFF (mk/memory.mk).  Examples find the loader's guest-facing
 * header through the PSP_KOSLOAD_BASE the build passes in rather than a
 * hardcoded address, so a PSP_LOADER_BASE override still works.  No ENTRY(): the
 * default entry equals the start of .text.start, which is what the
 * host's upload() returns as the execute address. */

OUTPUT_FORMAT("elf32-littlemips", "elf32-bigmips", "elf32-littlemips")
OUTPUT_ARCH(mips)

SECTIONS {
    . = 0x08804000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.stack) *(.eh_frame) *(.eh_frame_hdr) *(.MIPS.abiflags) *(.reginfo) *(.gnu.attributes) *(.pdr) *(.mdebug.*) }
}
