/*
 * lapis first-stage framebuffer probe (AArch64, position independent, no stack).
 *
 * Entered from LK's "jump to kernel" (Linux arm64 boot protocol: EL1/EL2,
 * MMU off, x0 = DTB). It never touches anything but the LK framebuffer
 * reservation and then parks the CPU. Nothing is written to storage, PMIC,
 * GPIO or any MMIO register.
 *
 * Each of the three derived LK buffers (see device/lapis_boot_contract.md)
 * gets a pattern that answers one open question when the screen is seen:
 *
 *   rows 0..255          word 0xFFFF0000: RED means ARGB8888 in memory order
 *                        B,G,R,A; BLUE means ABGR (R and B swapped)
 *   rows 256..2771       (index + 1) GREEN bands out of 8, the rest dark grey:
 *                        the band count is the buffer LK scans out (1/2/3)
 *   columns 0..63        WHITE on every row: a vertical stripe means the
 *                        5120-byte stride is right; a slanted one means wrong
 *
 * A black or unchanged screen means LK never reached this code, or scans out
 * a different address.
 */
#define FB_BASE   0xfd72f000
#define FRAME     0x00d98000      /* 1280 * 2784 * 4 */
#define STRIDE    5120
#define WIDTH     1280
#define HEIGHT    2772
#define NBUF      3
#define BAND_ROWS 314             /* (2772 - 256) / 8 */

	.text
	.globl	_start
_start:
	msr	daifset, #0xf
	mov	x20, #0                       /* buffer index */
1:	movz	x21, #(FB_BASE & 0xffff)
	movk	x21, #((FB_BASE >> 16) & 0xffff), lsl #16
	movz	x9, #(FRAME & 0xffff)
	movk	x9, #((FRAME >> 16) & 0xffff), lsl #16
	madd	x21, x20, x9, x21             /* x21 = buffer base */
	mov	x22, #0                       /* y */
2:	mov	x9, #STRIDE
	madd	x23, x22, x9, x21             /* x23 = row pointer */
	/* row colour */
	cmp	x22, #256
	b.hs	3f
	movz	w24, #0x0000
	movk	w24, #0xffff, lsl #16         /* 0xFFFF0000 */
	b	5f
3:	sub	x9, x22, #256
	mov	x10, #BAND_ROWS
	udiv	x9, x9, x10                   /* band 0..7 */
	cmp	x9, x20
	b.hi	4f
	movz	w24, #0xff00
	movk	w24, #0xff00, lsl #16         /* 0xFF00FF00 green */
	b	5f
4:	movz	w24, #0x2020
	movk	w24, #0xff20, lsl #16         /* 0xFF202020 dark grey */
5:	mov	x25, #0                       /* x */
	mov	w26, #-1                      /* white */
6:	cmp	x25, #64
	csel	w27, w26, w24, lo
	str	w27, [x23, x25, lsl #2]
	add	x25, x25, #1
	cmp	x25, #WIDTH
	b.lo	6b
	/* clean the row to the point of coherency in case LK left caches on */
	mov	x9, x23
	mov	x10, #(WIDTH * 4)
	add	x10, x23, x10
7:	dc	civac, x9
	add	x9, x9, #64
	cmp	x9, x10
	b.lo	7b
	add	x22, x22, #1
	mov	x9, #HEIGHT
	cmp	x22, x9
	b.lo	2b
	add	x20, x20, #1
	cmp	x20, #NBUF
	b.lo	1b
	dsb	sy
8:	wfe
	b	8b
