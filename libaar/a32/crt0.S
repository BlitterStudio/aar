/*
 * AAR app entry, profile A32 (ARMv7-A, Cortex-A9, hard-float, NEON).
 *
 * On entry r0 is the aar_api table, r1 argc, r2 argv and sp the app stack top
 * (8-byte aligned), per spec 3.2. The image is loaded and relocated, BSS is
 * zero, the FPSCR is zero. crt0 does none of that itself: no vectors, no BSS
 * clear, no MMU.
 *
 * It stores the table pointer through aar_crt0_bind, runs .preinit_array and
 * then .init_array, calls aar_main and passes the result to exit (libc
 * flavour) or aar_exit (AAR_FREESTANDING). The arrays are addressed
 * pc-relative, so the code needs no relocation. Entries are called with no
 * arguments; they were relocated by the loader.
 *
 * Not run: .fini_array. Destructors registered with atexit or __cxa_atexit
 * (C++ objects) run in the libc flavour, because exit runs them.
 *
 * SPDX-License-Identifier: MIT
 */
	.syntax unified
	.arch armv7-a
	.cpu cortex-a9
	.fpu neon

/* Call every function pointer in [start, end). Clobbers r0, r6, r7. */
	.macro RUN_ARRAY start, end
	movw	r6, #:lower16:(\start - (.Lstart\@ + 8))
	movt	r6, #:upper16:(\start - (.Lstart\@ + 8))
.Lstart\@:
	add	r6, pc, r6
	movw	r7, #:lower16:(\end - (.Lend\@ + 8))
	movt	r7, #:upper16:(\end - (.Lend\@ + 8))
.Lend\@:
	add	r7, pc, r7
.Lloop\@:
	cmp	r6, r7
	bhs	.Ldone\@
	ldr	r0, [r6], #4
	blx	r0
	b	.Lloop\@
.Ldone\@:
	.endm

	.section .text.aar_start, "ax", %progbits
	.arm
	.global aar_start
	.type aar_start, %function
aar_start:
	/* Six registers keep sp 8-byte aligned. r8 is only padding. */
	push	{r4, r5, r6, r7, r8, lr}
	mov	r4, r1
	mov	r5, r2
	bl	aar_crt0_bind		/* r0 is the table */
	RUN_ARRAY __preinit_array_start, __preinit_array_end
	RUN_ARRAY __init_array_start, __init_array_end
	mov	r0, r4
	mov	r1, r5
	bl	aar_main
#ifdef AAR_FREESTANDING
	bl	aar_exit		/* r0 is the aar_main status */
#else
	bl	exit
#endif
	b	aar_exit		/* unreachable: neither call returns */
	.size aar_start, . - aar_start
