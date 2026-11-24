/*
 * lapis audio power-up probe (shim alternate main, build.sh taud; dual:
 * Volume down). Step 1 of a speaker tone: power and clock the AFE, read it.
 *
 * From the stock modules (vendor_boot):
 *  - mtk-scpsys-mt6878.ko domain 2 "audio": PWR_CON 0x1c001000 + 0xe18,
 *    SRAM_PDN bit 8, SRAM ack bit 12; bus protect in infracfg_ao 0x10001000:
 *    set 0xc84 / clr 0xc88 / sta 0xc8c, mask 0x40 (Android writes 0xc88 = 0x40
 *    at playback start, device/captures/android_audio_regmap_trace2_*).
 *    PWR_CON bits as the MediaTek scpsys driver: RST_B 0, ISO 1, ON 2, ON_2ND 3,
 *    CLK_DIS 4, ACK 30, ACK_2ND 31.
 *  - clk-mt6878-peri.ko peraop_audio0/1/2: pericfg_ao 0x11036000 bits 0..2,
 *    sta 0x18, set (gate) 0x34, clr (ungate) 0x38.
 *  - clk-mt6878.ko apll2: apmixedsys 0x1000c000 + 0x34c (CON0), pcw 0x350,
 *    tuner 0x44; shown only.
 * Topckgen audio muxes as Android sets them (CLK_CFG set/clr pairs, update
 * register 0x8): aud_engen2 <- apll2_d4 (0xb4/0xb8), aud_2 <- apll2 (0xc4/0xc8),
 * audio_h <- apll2 (0xd4/0xd8): shown only here (APLL2 is not running yet).
 *
 * The AFE registers are read last; if the domain did not come up the read
 * can stall, so everything before it is already on screen.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define SCPSYS		0x1c001000UL
#define AUD_PWR_CON	(SCPSYS + 0xe18)
#define INFRA		0x10001000UL
#define PERI		0x11036000UL
#define TOPCK		0x10000000UL
#define APMIX		0x1000c000UL
#define AFE		0x11050000UL

#define PWR_RST_B	(1u << 0)
#define PWR_ISO		(1u << 1)
#define PWR_ON		(1u << 2)
#define PWR_ON_2ND	(1u << 3)
#define PWR_CLK_DIS	(1u << 4)
#define SRAM_PDN	(1u << 8)
#define SRAM_ACK	(1u << 12)
#define PWR_ACK		(1u << 30)
#define PWR_ACK_2ND	(1u << 31)

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

/* poll until (rd(a) & mask) == want; returns the 10 us steps or ~0u */
static uint32_t wait_bits(uintptr_t a, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0; i < 10000; i++) {
		if ((rd(a) & mask) == want)
			return i;
		udelay(10);
	}
	return ~0u;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "taud v1: audio power domain + AFE read");
	con_hex2("PWRCON", "audio e18=", rd(AUD_PWR_CON), "md e00=", rd(SCPSYS + 0xe00));
	con_hex2("BUSPROT", "infra c80=", rd(INFRA + 0xc80), "c8c=", rd(INFRA + 0xc8c));
	con_hex2("PERI", "audio gates sta 18=", rd(PERI + 0x18), 0, 0);
	con_hex2("TOPCK", "b0=", rd(TOPCK + 0xb0), "c0=", rd(TOPCK + 0xc0));
	con_hex2("TOPCK", "d0=", rd(TOPCK + 0xd0), "320=", rd(TOPCK + 0x320));
	con_hex2("APLL2", "con0 34c=", rd(APMIX + 0x34c), "350=", rd(APMIX + 0x350));
	con_hex2("APLL2", "354=", rd(APMIX + 0x354), "358=", rd(APMIX + 0x358));
	con_hex2("APLL1", "con0 338=", rd(APMIX + 0x338), "tuner 44=", rd(APMIX + 0x44));
	con_skip();

	uint32_t v = rd(AUD_PWR_CON);
	if (!(v & PWR_ACK)) {
		v |= PWR_ON;
		wr(AUD_PWR_CON, v);
		v |= PWR_ON_2ND;
		wr(AUD_PWR_CON, v);
		uint32_t n = wait_bits(AUD_PWR_CON, PWR_ACK | PWR_ACK_2ND, PWR_ACK | PWR_ACK_2ND);
		con_hex2("PWR ON", "acks after=", n, "pwr_con=", rd(AUD_PWR_CON));
		v = rd(AUD_PWR_CON) & ~PWR_CLK_DIS;
		wr(AUD_PWR_CON, v);
		v &= ~PWR_ISO;
		wr(AUD_PWR_CON, v);
		v |= PWR_RST_B;
		wr(AUD_PWR_CON, v);
	}
	wr(PERI + 0x38, 0x7);				/* peraop_audio0..2 on */
	v = rd(AUD_PWR_CON) & ~SRAM_PDN;
	wr(AUD_PWR_CON, v);
	uint32_t ns = wait_bits(AUD_PWR_CON, SRAM_ACK, 0);
	con_hex2("SRAM", "ack clear after=", ns, "pwr_con=", rd(AUD_PWR_CON));
	wr(INFRA + 0xc88, 0x40);			/* bus protect release */
	uint32_t nb = wait_bits(INFRA + 0xc8c, 0x40, 0);
	con_hex2("BUSPROT", "release after=", nb, "c8c=", rd(INFRA + 0xc8c));
	con_hex2("PERI", "audio gates sta 18=", rd(PERI + 0x18), 0, 0);
	con_skip();
	con_str("AFE", "reading AFE 0x11050000 next (a stall = no power/clock)");
	con_hex2("AFE", "00=", rd(AFE + 0x00), "04=", rd(AFE + 0x04));
	con_hex2("AFE", "10=", rd(AFE + 0x10), "14=", rd(AFE + 0x14));
	con_hex2("AFE", "38=", rd(AFE + 0x38), "ba0=", rd(AFE + 0xba0));
	con_hex2("AFE", "i2sout4 13c0=", rd(AFE + 0x13c0), "13c4=", rd(AFE + 0x13c4));
	con_hex2("AFE", "memif 4640=", rd(AFE + 0x4640), "4e10=", rd(AFE + 0x4e10));
	con_str("DONE", "AFE reachable");
#ifdef LAPIS_DUAL_PROBE
	return;			/* shim.c: Power reboots to Android */
#endif
	con_str("END", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
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
