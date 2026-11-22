/*
 * lapis dual boot (build.sh dual): Android by default, iOS on Volume up.
 *
 * The same idea as hakios-mt6768's mtkloader (MIT): the boot image's kernel is
 * this shim, the SWEETXNU payload behind it as usual, then Android's own stock
 * Image. lapis_boot_pack.py --android puts the Image's offset from _head in
 * the header's res2 (0x20), its length in res3 (0x28) and the offset of a
 * free 64 KiB area for chain_stub in res4 (0x30).
 *
 * dual_pick_ios() shows one line and watches Volume up (MT6363 TOPSTATUS 0x1e
 * bit 3, clear while down; read through PMIF SWINF channel 2 like tpmic.c)
 * for about two seconds. Only PMIC reads are sent; any PMIF error counts as
 * "not pressed". Before XNU, dual_wdt_off() stops LK's watchdog the way
 * start.S does for the other builds; Android gets it armed, as LK left it.
 */
#include <stdint.h>
#include "console.h"
#include "dual.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define PMIF		0x1cc04000UL
#define SW		(PMIF + 0x800u + 0x40u * 2u)
#define SW_ACC		0x00
#define SW_RDATA	0x14
#define SW_VLDCLR	0x24
#define SW_STA		0x28
#define FSM(sta)	(((sta) >> 1) & 7u)
#define PMIC_SID	4u
#define TOPSTATUS	0x1eu
#define VOLUP_BIT	(1u << 3)

#define WINDOW_MS	2000u
#define POLL_MS		20u
#define NEED_DOWN	3u		/* consecutive "down" reads */

extern char _head[];
extern char chain_stub[], chain_stub_end[];

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static int wait_fsm(uint32_t want)
{
	for (int i = 0; i < 2000; i++) {	/* ~2 ms */
		if (FSM(rd(SW + SW_STA)) == want)
			return 1;
		udelay(1);
	}
	return 0;
}

static int pmic_read(uint32_t addr)
{
	if (!wait_fsm(0))
		return -1;
	wr(SW + SW_ACC, (3u << 30) | (PMIC_SID << 24) | (addr & 0xffffu));
	if (!wait_fsm(6))
		return -1;
	uint32_t v = rd(SW + SW_RDATA);
	wr(SW + SW_VLDCLR, 1);
	return (int)(v & 0xff);
}

static int volup_down(void)
{
	int v = pmic_read(TOPSTATUS);
	return v >= 0 && !(v & VOLUP_BIT);
}

int dual_pick_ios(void)
{
#ifdef LAPIS_DUAL_TEST_IOS
	return 1;	/* QEMU test of the XNU path only */
#endif
	/* No screen clear: one line over LK's logo, which Android replaces. */
	con_str("LAPIS", "Vol+ now = iOS   (else Android)");
	uint32_t run = 0;
	for (uint32_t t = 0; t < WINDOW_MS; t += POLL_MS) {
		run = volup_down() ? run + 1 : 0;
		if (run >= NEED_DOWN)
			return 1;
		udelay(POLL_MS * 1000u);
	}
	return 0;
}

void dual_wdt_off(void)
{
	wr(0x1c00a000UL, 0x22000000u);	/* TOPRGU WDT_MODE = key, all off */
}

void dual_chain_android(uint64_t dtb)
{
	const volatile uint64_t *h = (const volatile uint64_t *)_head;
	uint64_t off = h[4], len = h[5], stub_off = h[6];
	uintptr_t base = (uintptr_t)_head;
	const volatile uint32_t *img = (const volatile uint32_t *)(base + off);

	if (!off || !len || !stub_off || (len & 15) || img[14] != 0x644d5241u) {
		con_clear(RED);
		con_str("DUAL", "no Android kernel in this image");
		con_hex2("HDR", "off=", (uint32_t)off, "len=", (uint32_t)len);
		for (;;)
			__asm__ volatile("wfe");
	}
	volatile uint32_t *dst = (volatile uint32_t *)(base + stub_off);
	const uint32_t *src = (const uint32_t *)chain_stub;
	for (uintptr_t i = 0; i < (uintptr_t)(chain_stub_end - chain_stub) / 4; i++)
		dst[i] = src[i];
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t) = (void (*)(uint64_t, uint64_t, uint64_t, uint64_t))(base + stub_off);
	stub(dtb, base, base + off, len);
	__builtin_unreachable();
}
