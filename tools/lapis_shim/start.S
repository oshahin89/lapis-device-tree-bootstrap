/*
 * lapis XNU boot shim: entry.
 *
 * LK decompresses boot_b's kernel to 0x40000000 and the EL2 hypervisor
 * (GenieZone) erets here at EL1 with the MMU and caches off, x0 = DTB
 * (device/lapis_boot_contract.md). The Linux arm64 Image header keeps LK
 * happy; lapis_boot_pack.py --image appends the SWEETXNU payload at the first
 * 4 KiB boundary after _end and patches image_size to cover it.
 *
 * Layout of the header follows Documentation/arch/arm64/booting.rst; the
 * structure (header, stack, vectors, bss) mirrors ios-sweet's shim/start.S
 * (MIT, see LICENSE.ios-sweet).
 */
	.section .text.head, "ax"
	.globl	_head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size; the packer grows it to cover the payload */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0, 0, 0			/* res2..res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

	.text
	.globl	_start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB from LK */
	mrs	x20, CurrentEL		/* entry EL (callee-saved); passed to shim_main */

	/*
	 * LK leaves the MT6878 TOPRGU watchdog armed; clear WDT_MODE with its
	 * 0x22 key first (confirmed on the device with the stage 1 probe).
	 * XNU patch 0082 repeats the store.
	 */
#ifndef LAPIS_DUAL
	movz	x0, #0xa000
	movk	x0, #0x1c00, lsl #16
	movz	w1, #0
	movk	w1, #0x2200, lsl #16
	str	w1, [x0]
	dsb	sy
#endif
	/* LAPIS_DUAL: dual.c leaves it armed for Android and stops it for XNU */

	/*
	 * lapis enters at EL1 (GenieZone owns EL2). QEMU virt with
	 * virtualization=on (needed for its smc PSCI conduit) enters at EL2:
	 * drop to EL1h the way ios-sweet's secondary entry does. Not taken on
	 * the phone.
	 */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.ne	.Lat_el1
	mov	x1, #(1 << 31)		/* HCR_EL2.RW: AArch64 EL1, no stage 2 */
	msr	hcr_el2, x1
	msr	vttbr_el2, xzr
	msr	cptr_el2, xzr
	msr	mdcr_el2, xzr
	mov	x1, #3			/* EL1 physical timer/counter access */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	mrs	x1, ICC_SRE_EL2		/* allow ICC_SRE_EL1, sysreg GIC interface */
	orr	x1, x1, #1
	orr	x1, x1, #8
	msr	ICC_SRE_EL2, x1
	movz	x1, #0x0800		/* SCTLR_EL1 RES1 bits, MMU/caches off */
	movk	x1, #0x30d0, lsl #16
	msr	sctlr_el1, x1
	adr	x1, .Lat_el1
	msr	elr_el2, x1
	mov	x1, #0x3c5		/* EL1h, DAIF masked */
	msr	spsr_el2, x1
	isb
	eret
.Lat_el1:
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
	mov	x1, x20			/* entry EL (CurrentEL value, EL<<2) */
	bl	shim_main
3:	wfe
	b	3b

/*
 * Exception vectors: report slot, ESR, ELR, FAR on screen and park. The shim
 * never expects an exception; with the watchdog off, recovery is
 * Power + Vol-down into LK fastboot.
 */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	b	exc_report
.endm

	.balign	0x800
	.globl	vectors
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

/*
 * chain_stub(dtb, dst, src, len): LAPIS_DUAL's way into Android's kernel.
 * dual.c copies these instructions past the end of everything the shim
 * needs and calls the copy, which moves Android's Image down onto the shim's
 * own load address (dst < src: an ascending copy is safe), and enters it
 * there the way LK would have: x0 = DTB, x1..x3 = 0, MMU and caches off.
 */
	.globl	chain_stub
	.globl	chain_stub_end
	.balign	16
chain_stub:
	mov	x4, x1
1:	ldp	x5, x6, [x2], #16
	stp	x5, x6, [x1], #16
	subs	x3, x3, #16
	b.hi	1b
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x4
chain_stub_end:

/*
 * sec_entry: a secondary core started by PSCI CPU_ON with context_id = its
 * stack top (tcpu: big-core speed). MMU and caches off like the boot core.
 */
	.globl	sec_entry
	.balign	16
sec_entry:
	msr	daifset, #0xf
	mov	sp, x0
	bl	sec_main
1:	wfe
	b	1b
