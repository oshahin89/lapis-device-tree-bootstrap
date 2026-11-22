/*
 * lapis button probe (shim alternate main, build.sh tkeys).
 *
 * Runtime FDT: gpio-keys volumedown = GPIO 54 active low (pull-up, func 0);
 * mt6363-keys power (0x74) + "home" (0x73 = KEY_VOLUMEUP) on the PMIC;
 * the SoC keypad kp@1c00e000 (fixed 26 MHz clock) says hw-pwrkey = 8,
 * hw-rstkey = 0x11 (17), hw-init-map[0] = 0x72. MTK keypads keep one bit per
 * key in KP_MEM1..5 (+0x04..+0x14), clear while the key is down.
 *
 * Read-only: shows, redrawn ~20 times a second, the keypad's first registers,
 * GPIO 54's input bit, and which bits changed since start (sticky), so each
 * button can be pressed alone and its bit read off the screen.
 * Only reads MMIO (plus the TOPRGU watchdog-off in start.S).
 * Do not hold Power + Vol-down together for long: that is the PMIC reset.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define KPD		0x1c00e000UL
#define GPIO		0x10005000UL
#define GPIO_DI_32	0x210		/* pins 32..63: GPIO 54 = bit 22 */
#define GPIO_MODE_48	0x360		/* pins 48..55, 4 bits each: GPIO 54 = bits 24..27 */
#define VOLDN_BIT	(1u << 22)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tkeys v1: press each button alone, watch the bits");
	con_hex2("GPIO54", "mode48=", rd(GPIO + GPIO_MODE_48), "di32=", rd(GPIO + GPIO_DI_32));
	con_skip();

	uint32_t base[8], chg[8] = { 0 }, voldn0 = rd(GPIO + GPIO_DI_32) & VOLDN_BIT, voldn_chg = 0;
	for (int i = 0; i < 8; i++)
		base[i] = rd(KPD + 4u * i);
	con_hex2("START", "sta=", base[0], "mem1=", base[1]);
	con_hex2("START", "mem2=", base[2], "mem3=", base[3]);
	con_hex2("START", "mem4=", base[4], "mem5=", base[5]);
	con_skip();
	int live = con_row();

	for (;;) {
		uint32_t r[8];
		for (int i = 0; i < 8; i++) {
			r[i] = rd(KPD + 4u * i);
			chg[i] |= r[i] ^ base[i];
		}
		uint32_t di = rd(GPIO + GPIO_DI_32) & VOLDN_BIT;
		if (di != voldn0)
			voldn_chg = 1;
		con_set_row(live);
		con_hex2("NOW", "sta=", r[0], "mem1=", r[1]);
		con_hex2("NOW", "mem2=", r[2], "mem3=", r[3]);
		con_hex2("NOW", "mem4=", r[4], "mem5=", r[5]);
		con_hex2("NOW", "+18=", r[6], "+1c=", r[7]);
		con_hex2("VOLDN", "gpio54=", di ? 1 : 0, "ever_changed=", voldn_chg);
		con_skip();
		con_hex2("CHANGED", "sta=", chg[0], "mem1=", chg[1]);
		con_hex2("CHANGED", "mem2=", chg[2], "mem3=", chg[3]);
		con_hex2("CHANGED", "mem4=", chg[4], "mem5=", chg[5]);
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
