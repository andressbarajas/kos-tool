/* Dreamcast linker script for kosload
 *
 * Based on dcload-serial: dcload-serial/target-src/dcload/dcload.x
 * Used by both kosload-serial and kosload-ip.
 */

OUTPUT_FORMAT("elf32-shl", "elf32-shl", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(start)

MEMORY
{
  ram (rwx) : ORIGIN = 0x8c004000, LENGTH = 0xb400
}

SECTIONS
{
  .interp     : { *(.interp) }
  .hash          : { *(.hash) }
  .dynsym        : { *(.dynsym) }
  .dynstr        : { *(.dynstr) }
  .gnu.version   : { *(.gnu.version) }
  .gnu.version_d : { *(.gnu.version_d) }
  .gnu.version_r : { *(.gnu.version_r) }
  .rel.init      : { *(.rel.init) }
  .rela.init     : { *(.rela.init) }
  .rel.text      : { *(.rel.text) *(.rel.text.*) *(.rel.gnu.linkonce.t*) }
  .rela.text     : { *(.rela.text) *(.rela.text.*) *(.rela.gnu.linkonce.t*) }
  .rel.fini      : { *(.rel.fini) }
  .rela.fini     : { *(.rela.fini) }
  .rel.rodata    : { *(.rel.rodata) *(.rel.rodata.*) *(.rel.gnu.linkonce.r*) }
  .rela.rodata   : { *(.rela.rodata) *(.rela.rodata.*) *(.rela.gnu.linkonce.r*) }
  .rel.data      : { *(.rel.data) *(.rel.data.*) *(.rel.gnu.linkonce.d*) }
  .rela.data     : { *(.rela.data) *(.rela.data.*) *(.rela.gnu.linkonce.d*) }
  .rel.ctors     : { *(.rel.ctors) }
  .rela.ctors    : { *(.rela.ctors) }
  .rel.dtors     : { *(.rel.dtors) }
  .rela.dtors    : { *(.rela.dtors) }
  .rel.got       : { *(.rel.got) }
  .rela.got      : { *(.rela.got) }
  .rel.sdata     : { *(.rel.sdata) *(.rel.sdata.*) *(.rel.gnu.linkonce.s*) }
  .rela.sdata    : { *(.rela.sdata) *(.rela.sdata.*) *(.rela.gnu.linkonce.s*) }
  .rel.sbss      : { *(.rel.sbss) }
  .rela.sbss     : { *(.rela.sbss) }
  .rel.bss       : { *(.rel.bss) }
  .rela.bss      : { *(.rela.bss) }
  .rel.plt       : { *(.rel.plt) }
  .rela.plt      : { *(.rela.plt) }

  .init : { KEEP (*(.init)) } =0
  .plt  : { *(.plt) }

  .text : {
    *(.text.crt0)
    *(.text)
    *(.text.*)
    *(.stub)
    *(.gnu.warning)
    *(.gnu.linkonce.t*)
  } =0
  _etext = .;
  PROVIDE (etext = .);

  .fini   ALIGN(4) : { KEEP (*(.fini)) } =0
  .rodata ALIGN(4) : { *(.rodata) *(.rodata.*) *(.gnu.linkonce.r*) }
  .rodata1 ALIGN(4) : { *(.rodata1) }

  .data ALIGN(4) : {
    *(.data)
    *(.data.*)
    *(.gnu.linkonce.d*)
    SORT(CONSTRUCTORS)
  }
  .data1 ALIGN(4) : { *(.data1) }
  .eh_frame ALIGN(4) : { *(.eh_frame) }
  .gcc_except_table ALIGN(4) : { *(.gcc_except_table) }

  .ctors ALIGN(4) : {
    ___ctors = .;
    KEEP (*crtbegin.o(.ctors))
    KEEP (*(EXCLUDE_FILE (*crtend.o ) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    ___ctors_end = .;
  }

  .dtors ALIGN(4) : {
    ___dtors = .;
    KEEP (*crtbegin.o(.dtors))
    KEEP (*(EXCLUDE_FILE (*crtend.o ) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    ___dtors_end = .;
  }

  .got ALIGN(4) : { *(.got.plt) *(.got) }
  .dynamic ALIGN(4) : { *(.dynamic) }

  .sdata ALIGN(4) : {
    *(.sdata)
    *(.sdata.*)
    *(.gnu.linkonce.s.*)
  }

  . = ALIGN(32 / 8);
  _edata = .;
  PROVIDE (edata = .);
  __bss_start = .;

  .sbss ALIGN(4) : {
    *(.dynsbss)
    *(.sbss)
    *(.sbss.*)
    *(.scommon)
  }

  .bss ALIGN(4) : {
    *(.dynbss)
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(32 / 8);
  }
  . = ALIGN(32 / 8);
  _end = .;
  PROVIDE (end = .);

  /* Debug sections */
  .stab 0 : { *(.stab) }
  .stabstr 0 : { *(.stabstr) }
  .stab.excl 0 : { *(.stab.excl) }
  .stab.exclstr 0 : { *(.stab.exclstr) }
  .stab.index 0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment 0 : { *(.comment) }
  .debug          0 : { *(.debug) }
  .line           0 : { *(.line) }
  .debug_srcinfo  0 : { *(.debug_srcinfo) }
  .debug_sfnames  0 : { *(.debug_sfnames) }
  .debug_aranges  0 : { *(.debug_aranges) }
  .debug_pubnames 0 : { *(.debug_pubnames) }
  .debug_info     0 : { *(.debug_info) }
  .debug_abbrev   0 : { *(.debug_abbrev) }
  .debug_line     0 : { *(.debug_line) }
  .debug_frame    0 : { *(.debug_frame) }
  .debug_str      0 : { *(.debug_str) }
  .debug_loc      0 : { *(.debug_loc) }
  .debug_macinfo  0 : { *(.debug_macinfo) }
  .debug_weaknames 0 : { *(.debug_weaknames) }
  .debug_funcnames 0 : { *(.debug_funcnames) }
  .debug_typenames 0 : { *(.debug_typenames) }
  .debug_varnames  0 : { *(.debug_varnames) }

  _stack = 0x8c00f400;

  ASSERT(( (_stack - _end) > 800 ), "Error: Not enough stack space: need at least 800 bytes")
  ASSERT(( _end <= (ORIGIN(ram) + LENGTH(ram)) ), "Error: Region 'ram' overflowed")
}
