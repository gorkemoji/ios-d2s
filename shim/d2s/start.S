/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S8 (SM-G950F, Exynos 8895) XNU loader entry.
 *
 * arm64 Linux Image header so Samsung S-Boot treats us like a kernel. S-Boot
 * enters at EL1 with the MMU off, CNTFRQ_EL0 = 0 and x0 = DTB (s8-probe
 * v2-v7). Position independent: the header asks for text_offset 0x80000 like
 * the stock kernel, but S-Boot has been seen loading us at 0x800b4000.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset, as the stock kernel */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0
	.quad	0
	.quad	0x00000000ecefecef	/* Exynos 9825 S-Boot header marker */
	.ascii	"ARM\x64"		/* magic */
	.long	0

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB */
	mrs	x20, CurrentEL
	mrs	x21, sctlr_el1
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	lsr	x1, x20, #2
	mov	x2, x21
	bl	dl_main			/* returns XNU entry PA, or 0 */
	cbz	x0, 4f
	mov	x16, x0
	adr	x1, dl_boot_args
	ldr	x0, [x1]		/* boot_args PA for start_first_cpu */
	/* XNU's entry expects the MMU and caches off. Data was written with the
	 * MMU off (Device memory) and the copies were cleaned to PoC anyway. */
	mrs	x14, sctlr_el1
	bic	x14, x14, #1		/* M */
	bic	x14, x14, #4		/* C */
	bic	x14, x14, #0x1000	/* I */
	msr	sctlr_el1, x14
	ic	iallu
	dsb	sy
	isb
	br	x16
4:	wfe
	b	4b

/*
 * smp_probe.c target: a started secondary marks smp_flags[(Aff1 & 1) << 2 |
 * Aff0] (independent of the PSCI context id) and powers itself off again.
 */
	.globl smp_entry
smp_entry:
	msr	daifset, #0xf
	mrs	x10, mpidr_el1
	ubfx	x11, x10, #8, #1
	and	x12, x10, #3
	orr	x12, x12, x11, lsl #2
	adr	x13, smp_flags
	mov	x14, #1
	str	x14, [x13, x12, lsl #3]
	dsb	sy
	movz	x0, #0x8400, lsl #16	/* PSCI CPU_OFF 0x84000002 */
	movk	x0, #0x0002
	smc	#0
6:	wfe
	b	6b

	.data
	.balign	8
	.globl smp_flags
smp_flags:
	.space	64
	.text

.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	mov	x1, sp
	b	exc_common
.endm

	.balign	0x800
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	bl	dl_exception		/* never returns */
5:	wfe
	b	5b
