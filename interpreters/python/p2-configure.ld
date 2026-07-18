/****************************************************************************
 * apps/interpreters/python/p2-configure.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This linker script is used only by CPython's cross-configure probes.  The
 * resulting executables are never loaded or run.  The production P2 NuttX
 * image continues to use the board linker script selected by NuttX.
 ****************************************************************************/

ENTRY(main)

SECTIONS
{
  . = 0x00000a00;

  .text :
  {
    *(.text .text.*)
  }

  .rodata :
  {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  }

  .data :
  {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  .bss (NOLOAD) :
  {
    *(.bss .bss.*)
    *(.sbss .sbss.*)
    *(COMMON)
  }

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }
}
