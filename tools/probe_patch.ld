/* Flat blob at the gap between the end of .text and .init (see src/probe_patch.c). */
ENTRY(_probe_wrap_decode)
SECTIONS
{
  . = 0x8c10fa00;
  .blob : {
    *(.text .text.*)
    *(.rodata .rodata.* .rodata1)
    *(.data .data.*)
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) *(.gnu*) }
}
