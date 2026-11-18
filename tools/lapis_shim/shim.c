/*
 * lapis XNU boot shim: report the entry state on the LK framebuffer, then
 * hand over to loader_boot(). On any error the reason stays on screen and
 * the CPU parks; the watchdog is already off, so recovery is
 * Power + Vol-down into LK fastboot.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"
#include "ufsinit.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static void __attribute__((noreturn)) park(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	con_clear(BLUE);
	con_str("LAPIS", "XNU boot shim");
	con_skip();
	con_hex2("EL", "cur=", (uint32_t)(rsr("CurrentEL") >> 2), "midr=", (uint32_t)rsr("midr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("SCTLR1", rsr("sctlr_el1"));
	con_hex2("MMFR0", "tg16=", (uint32_t)((rsr("id_aa64mmfr0_el1") >> 20) & 0xf),
		 "pa=", (uint32_t)(rsr("id_aa64mmfr0_el1") & 0xf));
	con_hex2("DTB", "at=", (uint32_t)dtb,
		 "magic=", dtb ? __builtin_bswap32(*(volatile uint32_t *)dtb) : 0);
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_skip();

	/* LK leaves the UFS host off; bring it up so xnu's driver can adopt it.
	 * A failure is reported but not fatal: RAM-disk roots don't need UFS. */
	const char *ufs = ufs_bring_up();
	con_str("UFS", ufs ? ufs : "host up, device initialised");
	con_skip();

	const char *why = loader_boot();
	con_skip();
	con_str("ERROR", why);
	park();
}

void exc_report(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	con_skip();
	con_hex("EXC slot", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	park();
}
