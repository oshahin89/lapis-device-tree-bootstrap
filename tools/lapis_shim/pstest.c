/*
 * pstore + warm-reset probe (alternative main for the shim; build.sh pstest).
 *
 * Writes a marker log into Android's ramoops console zone, shows a countdown
 * on screen, then warm-resets into LK fastboot (reboot.c). If both work, the
 * phone reaches fastboot without keys and, after restoring boot_b, Android's
 * /sys/fs/pstore/console-ramoops-0 contains the "LAPIS-PSTEST" lines.
 * MMIO writes: TOPRGU only (watchdog off in start.S, reboot mode, SWRST).
 */
#include <stdint.h>
#include "console.h"
#include "pstore.h"
#include "reboot.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static void wait_seconds(unsigned s)
{
	uint64_t frq = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < frq * s)
		;
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	ps_init();
	ps_puts("LAPIS-PSTEST v4: pstore written by the lapis shim before a warm reset\n");
	ps_puts("CNTVCT=");
	ps_hex(rsr("cntvct_el0"), 16);
	ps_puts("\nMIDR=");
	ps_hex(rsr("midr_el1"), 16);
	ps_puts("\nLAPIS-PSTEST end\n");

	con_clear(BLUE);
	con_str("LAPIS", "pstore + warm reset probe v4");
	con_str("PSTORE", "marker written at 0x480af000");
	/*
	 * v3 diagnostic: v1/v2 reset the SoC but the preloader still saw
	 * NONRST_REG2 = 0x2C002820. Show the register before and after our
	 * store, and WDT_MODE, so the screen tells whether the store sticks.
	 */
	volatile uint32_t *rgu = (volatile uint32_t *)0x1c00a000UL;
	uint32_t before = rgu[0x24 / 4];
	rgu[0x24 / 4] = (before & ~0x82fU) | 0x3U;	/* repeated in the PSCI path */
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t after = rgu[0x24 / 4];
	con_hex2("NONRST2", "before=", before, "after=", after);
	con_hex2("RGU", "mode=", rgu[0x00 / 4], "+20=", rgu[0x20 / 4]);
	ps_puts("NONRST2 before=");
	ps_hex(before, 8);
	ps_puts(" after=");
	ps_hex(after, 8);
	ps_puts("\n");
	con_str("REBOOT", "PSCI SYSTEM_RESET in 20 s, no keys");
	wait_seconds(20);
	con_str("REBOOT", "now (PSCI)");
	int64_t rc = lapis_reboot_bootloader_psci();
	con_hex("PSCI rc", (uint64_t)rc);
	con_str("REBOOT", "PSCI returned; Power+Vol-down");
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
