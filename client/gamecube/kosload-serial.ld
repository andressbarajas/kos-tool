/* GameCube linker script for kosload-serial */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(start)

MEMORY
{
  /* Loader region in MEM1, after system area + exception vectors.
   * 0x80003100 to 0x80013100 = 64KB for code + data + BSS.
   * Stack grows down from 0x80013100. */
  loader (rwx) : ORIGIN = 0x80003100, LENGTH = 0x10000
}

SECTIONS
{
  .text : {
    *(.text.crt0)
    *(.text)
    *(.text.*)
    *(.stub)
    *(.gnu.warning)
    *(.gnu.linkonce.t*)
  } > loader =0
  _etext = .;
  PROVIDE (etext = .);

  .rodata ALIGN(4) : { *(.rodata) *(.rodata.*) *(.gnu.linkonce.r*) } > loader
  .rodata1 ALIGN(4) : { *(.rodata1) } > loader

  .data ALIGN(4) : {
    *(.data)
    *(.data.*)
    *(.gnu.linkonce.d*)
    SORT(CONSTRUCTORS)
  } > loader
  .data1 ALIGN(4) : { *(.data1) } > loader

  .eh_frame ALIGN(4) : { *(.eh_frame) } > loader
  .gcc_except_table ALIGN(4) : { *(.gcc_except_table) } > loader

  .ctors ALIGN(4) : {
    ___ctors = .;
    KEEP (*crtbegin.o(.ctors))
    KEEP (*(EXCLUDE_FILE (*crtend.o ) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    ___ctors_end = .;
  } > loader

  .dtors ALIGN(4) : {
    ___dtors = .;
    KEEP (*crtbegin.o(.dtors))
    KEEP (*(EXCLUDE_FILE (*crtend.o ) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
    ___dtors_end = .;
  } > loader

  .got ALIGN(4) : { *(.got.plt) *(.got) } > loader

  .sdata ALIGN(4) : {
    *(.sdata)
    *(.sdata.*)
    *(.gnu.linkonce.s.*)
  } > loader
  .sdata2 ALIGN(4) : { *(.sdata2) *(.sdata2.*) } > loader

  . = ALIGN(4);
  _edata = .;
  PROVIDE (edata = .);
  __bss_start = .;

  .sbss ALIGN(4) : {
    *(.dynsbss)
    *(.sbss)
    *(.sbss.*)
    *(.scommon)
  } > loader
  .sbss2 ALIGN(4) : { *(.sbss2) *(.sbss2.*) } > loader

  .bss ALIGN(4) : {
    *(.dynbss)
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4);
  } > loader
  . = ALIGN(4);
  _end = .;
  PROVIDE (end = .);

  /* Debug sections */
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

  _stack = 0x80013100;

  ASSERT(( (_stack - _end) > 800 ), "Error: Not enough stack space: need at least 800 bytes")
  ASSERT(( _end <= (ORIGIN(loader) + LENGTH(loader)) ), "Error: Region 'loader' overflowed")
}
