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
#ifdef LAPIS_DUAL
#include "dual.h"
#endif

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static void __attribute__((noreturn)) park(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
#ifdef LAPIS_DUAL
	if (!dual_pick_ios())
		dual_chain_android(dtb);
	dual_wdt_off();
#endif
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
	if (!ufs) {
		/* PWM-G1 after LINKSTARTUP is ~MB/s; switch to HS (v31: ~800 MB/s).
		 * If the change fails, redo the bring-up so xnu still gets a PWM link. */
		unsigned int gear = 0, lanes = 0;
		const char *hs = ufs_set_hs(&gear, &lanes);
		con_str("UFS", hs ? hs : "HS mode (FAST, rate B)");
		if (hs) {
			ufs = ufs_bring_up();
			con_str("UFS", ufs ? ufs : "recovered in PWM");
		}
	}
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
