/* SPDX-License-Identifier: MPL-2.0 */

ENTRY(_start)
SECTIONS {
    . = 0x80200000;
    .text : { *(.text.init) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.sdata*) *(.data*) }
    .bss : { *(.sbss*) *(.bss*) }
}
