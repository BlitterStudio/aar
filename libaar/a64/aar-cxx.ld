/*
 * Linker script for an AAR app that contains C++, profile A64. It is aar.ld
 * with the DWARF unwind tables kept: .gcc_except_table and .eh_frame sit in
 * the RX segment after .rodata. .eh_frame ends with a zero word, the end mark
 * that crtend.o supplies in a normal link, and starts at __eh_frame_start,
 * which libaar/src/cxx.c hands to __register_frame before any constructor
 * runs. Code and libraries built with -fPIE or -fPIC encode the frame
 * descriptions PC-relative, so the sections need no relocation and the link
 * still passes -z text. aar.ld discards them; tests/cxx_ld_test.py keeps the
 * two scripts identical apart from these lines.
 *
 * mk/aar.mk and cmake/aar.cmake pick this script when the app has a C++
 * source. A C-only app keeps aar.ld and its image does not change.
 *
 * SPDX-License-Identifier: MIT
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(aar_start)

PHDRS {
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
	note PT_NOTE FLAGS(4);
	dyn  PT_DYNAMIC FLAGS(4);
}

SECTIONS {
	. = 0;

	.text : {
		*(.text.aar_start)
		*(.text .text.*)
	} :text

	.rodata : {
		*(.rodata .rodata.*)
	} :text

	/* cxx-unwind-begin */
	.gcc_except_table : {
		*(.gcc_except_table .gcc_except_table.*)
	} :text

	.eh_frame : {
		PROVIDE_HIDDEN(__eh_frame_start = .);
		KEEP(*(.eh_frame))
		LONG(0)
	} :text
	/* cxx-unwind-end */

	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.hash : { *(.hash) } :text
	.gnu.hash : { *(.gnu.hash) } :text

	/*
	 * RELA is the only table type of this profile. Keep the records so
	 * --gc-sections cannot drop what the loader reads. The output section must
	 * be named .rela.dyn: ld types a section of that name as SHT_RELA.
	 */
	.rela.dyn : {
		KEEP(*(.rela.dyn))
		KEEP(*(.rela.plt))
	} :text

	.dynamic : { *(.dynamic) } :text :dyn

	.note.aar : {
		KEEP(*(.note.aar))
	} :text :note

	. = ALIGN(4096);

	.preinit_array : {
		PROVIDE_HIDDEN(__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN(__preinit_array_end = .);
	} :data

	.init_array : {
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		PROVIDE_HIDDEN(__init_array_end = .);
	} :data

	.fini_array : {
		PROVIDE_HIDDEN(__fini_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		KEEP(*(.fini_array .dtors))
		PROVIDE_HIDDEN(__fini_array_end = .);
	} :data

	.data.rel.ro : {
		*(.data.rel.ro .data.rel.ro.*)
	} :data

	.got : {
		*(.got .got.plt .igot.plt)
	} :data

	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
	} :data

	/DISCARD/ : {
		*(.interp)
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.*)
	}
}
