/*
 * Linker script for an AAR app that contains C++, profile A32. It is aar.ld
 * with the ARM exception tables kept: .ARM.extab and .ARM.exidx sit in the RX
 * segment after .rodata, between __exidx_start and __exidx_end, which is where
 * the unwinder of libgcc_eh looks for them. Both sections hold PC-relative
 * (prel31) words, so they need no relocation and the link still passes
 * -z text. aar.ld discards them; tests/cxx_ld_test.py keeps the two scripts
 * identical apart from these lines.
 *
 * mk/aar.mk and cmake/aar.cmake pick this script when the app has a C++
 * source. A C-only app keeps aar.ld and its image does not change.
 *
 * SPDX-License-Identifier: MIT
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
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
	.ARM.extab : {
		*(.ARM.extab* .gnu.linkonce.armextab.*)
	} :text

	.ARM.exidx : {
		PROVIDE_HIDDEN(__exidx_start = .);
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		PROVIDE_HIDDEN(__exidx_end = .);
	} :text
	/* cxx-unwind-end */

	.dynsym : { *(.dynsym) } :text
	.dynstr : { *(.dynstr) } :text
	.hash : { *(.hash) } :text
	.gnu.hash : { *(.gnu.hash) } :text

	/*
	 * Keep either GNU ld's .rel.dyn or a toolchain's .rela.dyn so
	 * --gc-sections cannot drop the loader-visible relocation records.
	 */
	.rel.dyn : {
		KEEP(*(.rel.dyn))
		KEEP(*(.rel.plt))
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
		/* ARM code unwinds through .ARM.exidx; a .eh_frame is not used. */
		*(.eh_frame*)
	}
}
