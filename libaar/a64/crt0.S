/*
 * AAR app entry, profile A64 (ARMv8-A AArch64, LP64).
 *
 * On entry x0 is the aar_api table, w1 argc, x2 argv and sp the app stack top
 * (16-byte aligned), per spec 3.2. x29 and x30 are 0. The image is loaded and
 * relocated, BSS is zero, FPCR and FPSR are zero. crt0 does none of that
 * itself.
 *
 * It stores the table pointer through aar_crt0_bind, runs .preinit_array and
 * then .init_array, calls aar_main and passes the result to exit (libc
 * flavour) or aar_exit (AAR_FREESTANDING). The arrays are addressed with
 * adrp, so the code needs no relocation. Entries are called with no
 * arguments; they were relocated by the loader.
 *
 * Not run: .fini_array. Destructors registered with atexit or __cxa_atexit
 * (C++ objects) run in the libc flavour, because exit runs them.
 *
 * SPDX-License-Identifier: MIT
 */
	.arch armv8-a

/* Call every function pointer in [start, end). Clobbers x0, x21, x22. */
	.macro RUN_ARRAY start, end
	adrp	x21, \start
	add	x21, x21, :lo12:\start
	adrp	x22, \end
	add	x22, x22, :lo12:\end
1:
	cmp	x21, x22
	b.hs	2f
	ldr	x0, [x21], #8
	blr	x0
	b	1b
2:
	.endm

	.section .text.aar_start, "ax", %progbits
	.global aar_start
	.type aar_start, %function
aar_start:
	stp	x19, x20, [sp, #-32]!
	stp	x21, x22, [sp, #16]
	mov	w19, w1
	mov	x20, x2
	bl	aar_crt0_bind		/* x0 is the table */
	RUN_ARRAY __preinit_array_start, __preinit_array_end
	RUN_ARRAY __init_array_start, __init_array_end
	mov	w0, w19
	mov	x1, x20
	bl	aar_main
#ifdef AAR_FREESTANDING
	bl	aar_exit		/* w0 is the aar_main status */
#else
	bl	exit
#endif
	b	aar_exit		/* unreachable: neither call returns */
	.size aar_start, . - aar_start
