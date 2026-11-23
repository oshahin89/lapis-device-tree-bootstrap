/*
 * lapis CPU frequency probe (shim alternate main, build.sh tcpu; dual: Volume
 * down).
 *
 * Runtime FDT: performance-controller@11bc00, "mediatek,cpufreq-hw", two
 * performance domains: domain 0 at 0x11bc10 (cpu0..3, little, capacity 267)
 * and domain 1 at 0x11bd30 (cpu4..7, big, capacity 1024), 0x120 bytes each.
 * Layout as upstream drivers/cpufreq/mediatek-cpufreq-hw.c: LUT at 0x00
 * (32 x u32, [11:0] = MHz), FREQ_ENABLE 0x84, FREQ_PERF_STATE 0x88 (requested
 * LUT index), FREQ_HW_STATE 0x8c, FREQ_LATENCY 0x110. MCUPM firmware does the
 * switching (cpuhvfs@114400, "mediatek,cpufreq-hybrid").
 *
 * Shows both LUTs and states, times a fixed loop on cpu0 (loops per us is
 * proportional to the clock). v2: the MCU PLL block reads 0 from here. v3
 * starts cpu4 (big) with PSCI CPU_ON and times the same loop there, then
 * writes big FREQ_PERF = 0 and times again. v1: requests LUT index 0 (the top entry) on both
 * domains, waits, and times again. Writes: FREQ_PERF_STATE only.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define DOM0		0x0011bc10UL
#define DOM1		0x0011bd30UL
#define FREQ_ENABLE	0x84
#define FREQ_PERF	0x88
#define FREQ_HW		0x8c

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

/* iterations of a 4-instruction dependent loop per 10 ms of counter time */
static uint32_t speed(void)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0"), end = t0 + f / 100;
	uint32_t n = 0;
	while (rsr("cntvct_el0") < end) {
		uint64_t x = 1;
		for (int i = 0; i < 1000; i++)
			__asm__ volatile("add %0, %0, #1\n\teor %0, %0, %0, lsl #1" : "+r"(x));
		n++;
	}
	return n;
}

#define MCU_APMIX	0x0c030000UL	/* cpuhvfs apmixedsys: pll-con 0x208/0x218/0x228 */
#define MCU_CKDIV	0x0c000000UL	/* cpuhvfs clk-div-base: 0xa2a0/0xa2a4/0xa2e0 */

/* MHz from a MediaTek CPU PLL CON1: postdiv [26:24] (log2), pcw [21:0]
 * with 14 fractional bits, 26 MHz reference. */
static uint32_t pll_mhz(uint32_t con1)
{
	uint64_t pcw = con1 & 0x3fffff;
	uint32_t pd = (con1 >> 24) & 7;
	return (uint32_t)((26ull * pcw >> 14) >> pd);
}

static void pll(const char *label, uint32_t con, uint32_t div)
{
	uint32_t c0 = rd(MCU_APMIX + con), c1 = rd(MCU_APMIX + con + 4);
	con_hex2(label, "con0=", c0, "con1=", c1);
	con_hex2(label, "pll mhz=", pll_mhz(c1), "clkdiv=", rd(MCU_CKDIV + div));
}

static void dom(const char *label, uintptr_t d)
{
	con_hex2(label, "enable=", rd(d + FREQ_ENABLE), "perf=", rd(d + FREQ_PERF));
	con_hex2(label, "hw_state=", rd(d + FREQ_HW), "lut0=", rd(d + 0x00));
	/* the LUT as MHz, 4 entries a line */
	for (int i = 0; i < 16; i += 4) {
		uint32_t a = rd(d + i * 4) & 0xfff, b = rd(d + (i + 1) * 4) & 0xfff;
		uint32_t c = rd(d + (i + 2) * 4) & 0xfff, e = rd(d + (i + 3) * 4) & 0xfff;
		con_hex2(label, "mhz=", a << 16 | b, "", c << 16 | e);
	}
}

/* v3: run the speed loop on cpu4 (big) through PSCI CPU_ON */
extern char sec_entry[];
void sec_main(void);
static uint8_t sec_stack[4096] __attribute__((aligned(16)));
static volatile uint32_t sec_req, sec_done, sec_loops[2];

void sec_main(void)
{
	for (uint32_t n = 0; n < 2; n++) {
		while (sec_req <= n)
			__asm__ volatile("dsb sy" ::: "memory");
		sec_loops[n] = speed();
		__asm__ volatile("dsb sy" ::: "memory");
		sec_done = n + 1;
		__asm__ volatile("dsb sy" ::: "memory");
	}
}

static int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
	register uint64_t x0 __asm__("x0") = 0xc4000003ULL;
	register uint64_t x1 __asm__("x1") = mpidr;
	register uint64_t x2 __asm__("x2") = entry;
	register uint64_t x3 __asm__("x3") = ctx;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
	    "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

static int wait_done(uint32_t n)
{
	for (int i = 0; i < 300; i++) {	/* up to 300 ms */
		if (sec_done >= n)
			return 1;
		udelay(1000);
	}
	return 0;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tcpu v3: big core speed via PSCI cpu4");
	con_hex2("LITTLE", "enable=", rd(DOM0 + FREQ_ENABLE), "perf=", rd(DOM0 + FREQ_PERF));
	con_hex2("BIG", "enable=", rd(DOM1 + FREQ_ENABLE), "perf=", rd(DOM1 + FREQ_PERF));
	/* v2: the PLLs themselves (v1: little at LUT 0 = 2000 MHz already, big
	 * domain not enabled, perf 12) */
	(void)pll;		/* v2 on the phone: the MCU PLL block reads 0 */
	uint32_t s0 = speed();
	con_hex2("SPEED", "before kloops/10ms=", s0, 0, 0);
	con_skip();
	int64_t r = psci_cpu_on(0x400, (uint64_t)(uintptr_t)sec_entry,
	    (uint64_t)(uintptr_t)(sec_stack + sizeof(sec_stack)));
	con_hex2("CPU4", "psci cpu_on=", (uint32_t)r, 0, 0);
	if (r == 0) {
		sec_req = 1;
		__asm__ volatile("dsb sy" ::: "memory");
		if (wait_done(1)) {
			con_hex2("CPU4", "big kloops/10ms=", sec_loops[0], "x100 vs cpu0=", s0 ? sec_loops[0] * 100 / s0 : 0);
			/* ask for the top big OPP (LUT 0 = 2600 MHz) even with the domain off */
			wr(DOM1 + FREQ_PERF, 0);
			udelay(50000);
			con_hex2("BIG", "perf=", rd(DOM1 + FREQ_PERF), "hw_state=", rd(DOM1 + FREQ_HW));
			sec_req = 2;
			__asm__ volatile("dsb sy" ::: "memory");
			if (wait_done(2))
				con_hex2("CPU4", "after perf 0 kloops=", sec_loops[1], "x100 vs cpu0=", s0 ? sec_loops[1] * 100 / s0 : 0);
			else
				con_str("CPU4", "no second result");
		} else {
			con_str("CPU4", "started but no result");
		}
	}
	(void)dom;
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
