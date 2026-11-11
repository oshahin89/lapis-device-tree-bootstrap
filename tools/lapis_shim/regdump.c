/*
 * lapis register-dump probe (alternative main for the shim; build.sh regdump).
 *
 * Reads, never writes, the UFS host and UFS clock/reset controller registers
 * at LK's kernel jump and records them twice: a summary on the framebuffer,
 * the full dump in Android's ramoops console zone (pstore.c), which Android
 * shows as /sys/fs/pstore/console-ramoops-0 after the restore boot if DRAM
 * survives the Power + Vol-down reset. The only MMIO store is the TOPRGU
 * watchdog disable in start.S. A "> region" marker is written before each
 * block, so a bus hang shows where it happened.
 */
#include <stdint.h>
#include "console.h"
#include "pstore.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define UFSHCI		0x112b0000UL	/* ufshci@112b0000 */
#define UFSCFG_AO	0x112b8000UL	/* mediatek,mt6878-ufscfg_ao */
#define UFSCFG_PDN	0x112bb000UL	/* mediatek,mt6878-ufscfg_pdn */

static inline uint32_t rd32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static void __attribute__((noreturn)) park(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

static void ps_kv(const char *k, uint64_t v)
{
	ps_puts(k);
	ps_puts("=");
	ps_hex(v, 16);
	ps_puts("\n");
}

/* Dump [base+lo, base+hi) as rows of four words. */
static void dump(const char *name, uintptr_t base, uint32_t lo, uint32_t hi)
{
	ps_puts("> ");
	ps_puts(name);
	ps_puts("\n");
	for (uint32_t off = lo; off < hi; off += 16) {
		ps_hex(base + off, 8);
		ps_puts(":");
		for (uint32_t i = 0; i < 16 && off + i < hi; i += 4) {
			ps_puts(" ");
			ps_hex(rd32(base + off + i), 8);
		}
		ps_puts("\n");
	}
}

void shim_main(uint64_t dtb)
{
	ps_init();
	ps_puts("LAPIS-REGDUMP v1 (lapis shim probe; read-only MMIO except TOPRGU off)\n");
	ps_kv("CurrentEL", rsr("CurrentEL"));
	ps_kv("MIDR", rsr("midr_el1"));
	ps_kv("MPIDR", rsr("mpidr_el1"));
	ps_kv("SCTLR_EL1", rsr("sctlr_el1"));
	ps_kv("ID_AA64MMFR0", rsr("id_aa64mmfr0_el1"));
	ps_kv("CNTFRQ", rsr("cntfrq_el0"));
	ps_kv("DTB", dtb);

	con_clear(BLUE);
	con_str("LAPIS", "register dump probe");
	con_str("PSTORE", "log at 0x480af000");
	con_skip();

	dump("UFSHCI std 0x00-0xa0", UFSHCI, 0x00, 0xa0);
	con_hex2("HCI", "cap=", rd32(UFSHCI + 0x00), "ver=", rd32(UFSHCI + 0x08));
	con_hex2("HCI", "hcs=", rd32(UFSHCI + 0x30), "hce=", rd32(UFSHCI + 0x34));
	dump("UFSHCI mtk 0x140-0x170", UFSHCI, 0x140, 0x170);
	con_hex2("XOUFS", "ctl=", rd32(UFSHCI + 0x140), "refclk=", rd32(UFSHCI + 0x144));
	dump("UFSHCI mtk 0x2100-0x2110", UFSHCI, 0x2100, 0x2110);
	dump("UFSHCI mtk 0x2200-0x2250", UFSHCI, 0x2200, 0x2250);
	con_hex2("IPVER", "v=", rd32(UFSHCI + 0x2240), "probe=", rd32(UFSHCI + 0x22c8));

	dump("UFSCFG_AO 0x000-0x100", UFSCFG_AO, 0x000, 0x100);
	con_hex2("CFG_AO", "+00=", rd32(UFSCFG_AO + 0x00), "+48=", rd32(UFSCFG_AO + 0x48));
	dump("UFSCFG_PDN 0x000-0x100", UFSCFG_PDN, 0x000, 0x100);
	con_hex2("CFG_PDN", "+00=", rd32(UFSCFG_PDN + 0x00), "+48=", rd32(UFSCFG_PDN + 0x48));

	ps_puts("LAPIS-REGDUMP done\n");
	con_skip();
	con_str("DONE", "Power+Vol-down, restore boot_b");
	park();
}

void exc_report(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	ps_kv("EXC slot", slot);
	ps_kv("ESR", esr);
	ps_kv("ELR", elr);
	ps_kv("FAR", far);
	con_skip();
	con_hex("EXC slot", slot);
	con_hex("ESR", esr);
	con_hex("FAR", far);
	park();
}
