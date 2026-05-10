/* client/playstation2/iop/kosload_iop.ld
 *
 * Linker script for clean-room kosload IOP IRX modules.  Used by any
 * IRX whose source uses kosload_iop.h's KOSLOAD_IRX_ID() macro, to
 * provide the section-size symbols the macro references and to
 * arrange sections in the layout BIOS LOADCORE expects.
 *
 * Origin 0x00001000 is above the IOP scratch / BIOS LOADCORE area.
 * The IOP loader patches in the actual load address at module load
 * time, so 0x00001000 is just a placeholder — addresses in the
 * resulting ELF are link-time relative and the loader applies a
 * runtime offset.
 *
 * 0x4000 (16 KB) is enough headroom for our SMAP / HDD / etc. IRXs
 * with their stage-2 chip-driver code.  Bump if needed.
 */

OUTPUT_FORMAT("elf32-littlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY
{
    IOP_RAM (rwx) : ORIGIN = 0x00001000, LENGTH = 0x4000
}

SECTIONS
{
    /* The .iopmod section is NOT emitted by KOSLOAD_IRX_ID — elf2irx
     * synthesizes it at IRX-conversion time from the `_irx_id` and
     * `__bss_start` symbols.  No .iopmod section ordering needed in
     * this script. */

    .text : {
        *(.text.start)
        *(.text .text.*)
    } > IOP_RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > IOP_RAM

    .data : { *(.data .data.*) } > IOP_RAM

    /* Keep .sdata as its own section. KOSLOAD_IRX_ID applies
     * `__attribute__((section(".sdata")))` to `_irx_id`, matching the
     * convention observed via `readelf -S` on working IRXs. Sony BIOS
     * IRXs use .data instead (no .sdata section at all) and load fine
     * — LOADCORE reads `iopmod[+0x00]` as an absolute address
     * regardless of which section the target lives in.
     *
     * This layout keeps .sdata in the +/-32 KB window around
     * `__bss_start + 0x7fe8` that LOADCORE installs into `$gp`,
     * enabling single-instruction `gp`-relative access
     * (`lw $reg, off($gp)`) to any small globals an IRX adds. The
     * 4-byte modinfo pointer alone doesn't justify it, but the layout
     * extends to driver-state structs in SMAP / HDD / etc. */
    .sdata : { *(.sdata .sdata.*) } > IOP_RAM

    /* `__bss_start` is referenced by elf2irx to compute the GP
     * register value (gp = __bss_start + 0x7fe8) written into
     * iopmod[+0x08].  IRXs are compiled with `-G 0` (see
     * IOP_CFLAGS in the Makefile) so we don't get
     * compiler-emitted gp-relative loads that would require a
     * matching `_gp` symbol here.  All globals go through HI16/LO16
     * pairs that LOADCORE relocates correctly.  ALIGN(4) ensures
     * __bss_start lands on a 4-byte boundary regardless of preceding
     * .sdata size — without it, `__bss_start + 0x7fe8` can drift
     * and confuse elf2irx's gp computation. */
    . = ALIGN(4);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } > IOP_RAM


    /DISCARD/ : {
        *(.comment)
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.pdr)
        *(.gnu.attributes)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note*)
    }
}
