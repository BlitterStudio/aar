/*
 * Linker script for an AAR app, profile A64. PIE: virtual addresses are
 * offsets from 0 and the loader adds the load base and applies the
 * relocations listed in PT_DYNAMIC (spec 2.2). Two PT_LOAD segments, one RX
 * and one RW, so no segment is writable and executable, plus PT_NOTE for
 * .note.aar and PT_DYNAMIC.
 *
 * Everything the loader relocates sits in the RW segment: the init arrays,
 * .data.rel.ro and the GOT. The RX segment holds code, read-only data and the
 * dynamic tables, and needs no relocation (link with -z text). The RW segment
 * starts on a 4 KiB boundary so a platform that maps pages can give the two
 * segments different permissions.
 *
 * Link with -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 (mk/aar.mk
 * and cmake/aar.cmake do).
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
		/* Apps disable unwinding; unwinder archives carry absolute relocs. */
		*(.eh_frame*)
	}
}
