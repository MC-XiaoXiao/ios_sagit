/* SPDX-License-Identifier: BSD-2-Clause */
OUTPUT_ARCH(aarch64)
ENTRY(raw_start)
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.boot)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .rela.dyn : { raw_rela_start = .; *(.rela.dyn) raw_rela_end = .; }
    .data : { *(.data .data.*) *(.got .got.*) }
    .dynamic : { *(.dynamic) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .bss : { *(.bss .bss.*) *(COMMON) BYTE(0); }
    . = ALIGN(4096);
    raw_image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
