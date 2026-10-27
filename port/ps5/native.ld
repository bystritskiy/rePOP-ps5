/* reSOT PS5, MIT. Four page-aligned LOAD segments for native conversion. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
PHDRS {
  code PT_LOAD FLAGS(5);
  const PT_LOAD FLAGS(4);
  relro PT_LOAD FLAGS(6);
  writable PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
}
SECTIONS {
  .text 0 : ALIGN(0x4000) { QUAD(0) QUAD(0) *(.text .text.*) *(.plt .plt.*) } :code
  .eh_frame_hdr : ALIGN(0x4000) { KEEP(*(.eh_frame_hdr)) } :const
  .eh_frame : { KEEP(*(.eh_frame)) } :const
  .rodata : { *(.rodata .rodata.*) } :const
  . = ALIGN(0x4000);
  .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :relro
  .got : { *(.got .got.plt) } :relro
  .init_array : {
    __init_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
  } :relro
  .fini_array : {
    __fini_array_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .;
  } :relro
  .dynsym : { *(.dynsym) } :relro
  .gnu.hash : { *(.gnu.hash) } :relro
  .dynstr : { *(.dynstr) } :relro
  .rela.dyn : { *(.rela.dyn) } :relro
  .rela.plt : { *(.rela.plt) } :relro
  .dynamic : ALIGN(0x4000) { *(.dynamic) } :writable :dynamic
  .data : { *(.data .data.*) } :writable
  .bss : { *(.bss .bss.*) *(COMMON) } :writable
}
