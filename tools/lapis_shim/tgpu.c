/*
 * lapis GPU probe (shim alternate main, build.sh tgpu; dual: Volume down).
 * Read-only. Mali-G615 MC2 r1p3 (CSF) at 0x13000000 (Android gpuinfo).
 *
 * Power, from the stock mtk-scpsys-mt6878.ko: domain "mfg0-shutdown" PWR_CON
 * 0x1c001000 + 0xeb4 (SRAM bit 8 / ack 12; bus protect infracfg_ao
 * 0xca0..0xcac bit 4). GPU-side blocks from the runtime FDT gpufreq node:
 * mfg_top_config 0x13fbf000, mfg_pll 0x13fa0000, mfgsc_pll 0x13fa0c00,
 * mfg_rpc 0x13f90000 (core power), gpueb 0x13c00000; GPUEB owns power/DVFS.
 *
 * Shows those first; then, only if mfg0 reports both power acks, the Mali's
 * GPU_ID (0x0), L2/shader/tiler PRESENT and READY registers (Valhall layout:
 * SHADER_PRESENT 0x100, TILER 0x110, L2 0x120, SHADER_READY 0x140, TILER 0x150,
 * L2 0x160). A stall on that last read means the core domain is off.
 */
#include <stdint.h>
#include "console.h"

#define SCPSYS		0x1c001000UL
#define MFG0_PWR_CON	(SCPSYS + 0xeb4)
#define INFRA		0x10001000UL
#define MFG_TOP		0x13fbf000UL
#define MFG_PLL		0x13fa0000UL
#define MFGSC_PLL	0x13fa0c00UL
#define MFG_RPC		0x13f90000UL
#define GPUEB		0x13c00000UL
#define GPU		0x13000000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static void pair(const char *label, uintptr_t base, uint32_t a, uint32_t b)
{
	char la[8] = "+000=", lb[8] = " +000=";
	static const char hx[] = "0123456789abcdef";
	la[1] = hx[(a >> 8) & 0xf]; la[2] = hx[(a >> 4) & 0xf]; la[3] = hx[a & 0xf];
	lb[2] = hx[(b >> 8) & 0xf]; lb[3] = hx[(b >> 4) & 0xf]; lb[4] = hx[b & 0xf];
	con_hex2(label, la, rd(base + a), lb, rd(base + b));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tgpu v1: Mali-G615 power state + GPU_ID");
	uint32_t pwr = rd(MFG0_PWR_CON);
	con_hex2("MFG0", "pwr_con eb4=", pwr, "busprot cac=", rd(INFRA + 0xcac));
	pair("MFG_PLL", MFG_PLL, 0x008, 0x00c);
	pair("MFG_PLL", MFG_PLL, 0x010, 0x014);
	pair("SC_PLL", MFGSC_PLL, 0x008, 0x00c);
	pair("MFG_RPC", MFG_RPC, 0x000, 0x004);
	pair("MFG_RPC", MFG_RPC, 0x008, 0x00c);
	pair("MFG_RPC", MFG_RPC, 0x100, 0x104);
	pair("MFG_RPC", MFG_RPC, 0x500, 0x504);
	pair("MFG_TOP", MFG_TOP, 0x000, 0x004);
	pair("MFG_TOP", MFG_TOP, 0x008, 0x00c);
	pair("GPUEB", GPUEB, 0x000, 0x004);
	con_skip();
	if ((pwr & 0xc0000000u) != 0xc0000000u) {
		con_str("GPU", "mfg0 is off: not touching the Mali");
		goto out;
	}
	con_str("GPU", "mfg0 on: reading the Mali next (a stall = cores off)");
	con_hex2("GPU", "gpu_id=", rd(GPU + 0x000), "l2_features=", rd(GPU + 0x004));
	con_hex2("GPU", "shader_present=", rd(GPU + 0x100), "tiler_present=", rd(GPU + 0x110));
	con_hex2("GPU", "l2_present=", rd(GPU + 0x120), "shader_ready=", rd(GPU + 0x140));
	con_hex2("GPU", "tiler_ready=", rd(GPU + 0x150), "l2_ready=", rd(GPU + 0x160));
	con_hex2("GPU", "gpu_status=", rd(GPU + 0x034), "mcu_status=", rd(GPU + 0x704));
	con_str("DONE", "Mali registers readable");
out:
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
