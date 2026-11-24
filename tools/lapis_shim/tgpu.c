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
/* GPUEB IPI (runtime FDT gpueb@13c00000; captured on Android with kprobes):
 * gpufreq sends IPI 1 (IPI_ID_GPUFREQ) as mtk_mbox_write(mbox 0, slot 4,
 * 8 words) + mtk_mbox_trigger_irq(mbox 0, 0x2). Words: [0] magic (read by the
 * driver from GPUEB GPR 13), [1] command (6 = CMD_POWER_CONTROL), [4] 1 = on. */
#define GPUEB_GPR	0x13c3fd1cUL
#define MBOX0		0x13c3fd80UL	/* 0x280 bytes of slots */
#define MBOX0_SET	0x13c62004UL
#define MBOX0_CLR	0x13c62074UL
#define MBOX0_SEND	0x13c62000UL
#define MBOX0_RECV	0x13c62078UL
#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

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

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr(uintptr_t a, uint32_t v);
static void udelay(uint32_t us);

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
	con_skip();		/* the camera hole covers the first lines */
	con_str("LAPIS", "tgpu v2: GPUEB power-on IPI + GPU_ID");
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
	con_hex2("GPU", "before gpu_id=", rd(GPU + 0x000), "shader_ready=", rd(GPU + 0x140));

	/* v2: ask GPUEB to power the GPU on, as Android's gpufreq does */
	uint32_t magic = rd(GPUEB_GPR + 13 * 4);
	con_hex2("GPUEB", "magic gpr13=", magic, "recv=", rd(MBOX0_RECV));
	con_hex2("GPUEB", "send=", rd(MBOX0_SEND), "slot4=", rd(MBOX0 + 16));
	if (magic == 0) {
		con_str("GPUEB", "no magic: GPUEB not running?");
		goto out;
	}
	uint32_t msg[8] = { magic, 6, 0, 0, 1, 0, 0, 0 };
	uint32_t before[40];
	for (int i = 0; i < 40; i++)
		before[i] = rd(MBOX0 + i * 4);
	for (int i = 0; i < 8; i++)
		wr(MBOX0 + 16 + i * 4, msg[i]);
	wr(MBOX0_SET, 0x2);
	uint32_t recv = 0, t;
	for (t = 0; t < 1000; t++) {	/* up to 100 ms */
		recv = rd(MBOX0_RECV);
		if (recv)
			break;
		udelay(100);
	}
	con_hex2("IPI", "recv=", recv, "after x100us=", t);
	int shown = 0;
	for (int i = 0; i < 40 && shown < 6; i++) {
		uint32_t v = rd(MBOX0 + i * 4);
		if (v != before[i] && (i < 4 || i >= 12)) {
			con_hex2("SLOT", "index=", (uint32_t)i, "value=", v);
			shown++;
		}
	}
	if (recv)
		wr(MBOX0_CLR, recv);
	udelay(5000);
	con_hex2("GPU", "gpu_id=", rd(GPU + 0x000), "l2_features=", rd(GPU + 0x004));
	con_hex2("GPU", "shader_present=", rd(GPU + 0x100), "tiler_present=", rd(GPU + 0x110));
	con_hex2("GPU", "l2_present=", rd(GPU + 0x120), "gpu_status=", rd(GPU + 0x034));
	con_hex2("MFG_RPC", "+000=", rd(MFG_RPC), "+00c=", rd(MFG_RPC + 0xc));
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
