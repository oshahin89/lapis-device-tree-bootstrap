/*
 * lapis PMIC key probe (shim alternate main, build.sh tpmic).
 *
 * Power and Volume up are MT6363 keys (SPMI slave 4); stock mtk-pmic-keys.ko
 * reads them from PMIC TOPSTATUS 0x1e. The PMIC is reached through PMIF
 * (mediatek,mt6878-spmi, 0x1cc04000), as the stock spmi-mtk-pmif.ko does
 * (pmif_spmi_read_cmd, mt6xxx_regs, soc_chan 2):
 *   SWINF channel c at 0x800 + 0x40*c: ACC +0x00, RDATA +0x14, VLD_CLR +0x24,
 *   STA +0x28 (FSM = bits 1..3: 0 idle, 6 = data valid, wait for clear);
 *   command = type<<30 | sid<<24 | (len-1)<<16 | addr, type 3 = extended
 *   register long (16-bit address), read.
 * Shows the four channels' STA, the PMIC's ID bytes (0x8/0x9) as a check, then
 * 0x1e live with the bits that ever changed. Only PMIC reads are sent.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define PMIF		0x1cc04000UL
#define CH		2u
#define SW(c)		(PMIF + 0x800u + 0x40u * (c))
#define SW_ACC		0x00
#define SW_RDATA	0x14
#define SW_VLDCLR	0x24
#define SW_STA		0x28
#define FSM(sta)	(((sta) >> 1) & 7u)
#define PMIC_SID	4u

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

static uint32_t n_err;

static int wait_fsm(uint32_t want)
{
	for (int i = 0; i < 20000; i++) {	/* ~20 ms */
		if (FSM(rd(SW(CH) + SW_STA)) == want)
			return 1;
		udelay(1);
	}
	return 0;
}

/* One byte from a PMIC register; -1 on timeout. */
static int pmic_read(uint32_t addr)
{
	if (!wait_fsm(0)) {
		n_err++;
		return -1;
	}
	wr(SW(CH) + SW_ACC, (3u << 30) | (PMIC_SID << 24) | (0u << 16) | (addr & 0xffffu));
	if (!wait_fsm(6)) {
		n_err++;
		return -1;
	}
	uint32_t v = rd(SW(CH) + SW_RDATA);
	wr(SW(CH) + SW_VLDCLR, 1);
	return (int)(v & 0xff);
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tpmic v1: press Power / Vol-up alone, watch 0x1e");
	con_hex2("STA", "ch0=", rd(SW(0) + SW_STA), "ch1=", rd(SW(1) + SW_STA));
	con_hex2("STA", "ch2=", rd(SW(2) + SW_STA), "ch3=", rd(SW(3) + SW_STA));
	int id0 = pmic_read(0x8), id1 = pmic_read(0x9);
	con_hex2("PMIC ID", "0x8=", (uint32_t)id0, "0x9=", (uint32_t)id1);
	int base = pmic_read(0x1e);
	con_hex2("START", "0x1e=", (uint32_t)base, "errors=", n_err);
	con_skip();
	int live = con_row();
	uint32_t chg = 0, gpio_chg = 0;
	uint32_t g0 = rd(0x10005210UL) & (1u << 22);	/* Vol-down, GPIO 54 */
	for (;;) {
		int v = pmic_read(0x1e);
		uint32_t g = rd(0x10005210UL) & (1u << 22);
		if (v >= 0)
			chg |= (uint32_t)(v ^ base);
		if (g != g0)
			gpio_chg = 1;
		con_set_row(live);
		con_hex2("NOW", "0x1e=", (uint32_t)v, "errors=", n_err);
		con_hex2("CHANGED", "0x1e bits=", chg, "voldn_gpio=", gpio_chg);
		con_hex2("VOLDN", "gpio54=", g ? 1 : 0, 0, 0);
		udelay(50000);
	}
}

void exc_report(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	con_skip();
	con_hex("EXC slot", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	for (;;)
		__asm__ volatile("wfe");
}
