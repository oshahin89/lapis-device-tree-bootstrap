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
	ps_puts("LAPIS-PSTEST v1: pstore written by the lapis shim before a warm reset\n");
	ps_puts("CNTVCT=");
	ps_hex(rsr("cntvct_el0"), 16);
	ps_puts("\nMIDR=");
	ps_hex(rsr("midr_el1"), 16);
	ps_puts("\nLAPIS-PSTEST end\n");

	con_clear(BLUE);
	con_str("LAPIS", "pstore + warm reset probe");
	con_str("PSTORE", "marker written at 0x480af000");
	con_str("REBOOT", "to fastboot in 10 s, no keys");
	wait_seconds(10);
	con_str("REBOOT", "now");
	lapis_reboot_bootloader();
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
