/*
 * lapis Mali-G615 CSF firmware boot probe (shim alternate main, build.sh tcsf;
 * dual: Volume down). Milestone 2 of device/lapis_gpu_plan.md.
 *
 * Sequence after Linux panthor (panthor_fw.c / panthor_gpu.c / panthor_mmu.c,
 * fetched for reference, not copied): power on through the GPUEB IPI (tgpu
 * v2), GPU soft reset (GPU_CMD 0x101, wait GPU_INT_RAWSTAT bit 8), coherency
 * NONE, L2 power on; MCU address space AS0 with AArch64 4K page tables built
 * here (all memory non-cacheable: MEMATTR 0x4c per index, PTW non-cacheable);
 * mali_csffw.bin sections (header 0xc3f13a6e, IFACE entries {flags, va start
 * /end, data start/end}) copied to a DRAM pool and mapped at their MCU VAs,
 * ZERO sections cleared, PROT sections skipped; MCU_CONTROL = AUTO, wait for
 * JOB_INT_GLOBAL_IF (bit 31); then the global control block at MCU VA
 * 0x04000000: version, features, input/output VAs, group count.
 * Writes: the GPUEB mailbox, the GPU, and DRAM at POOL.
 */
#include <stdint.h>
#include "console.h"
#include "mali_fw.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPU		0x13000000UL
#define GPUEB_GPR	0x13c3fd1cUL
#define MBOX0		0x13c3fd80UL
#define MBOX0_SET	0x13c62004UL
#define MBOX0_CLR	0x13c62074UL
#define MBOX0_RECV	0x13c62078UL

#define POOL		0x58000000UL	/* 4 MiB of DRAM nobody uses in a probe boot */
#define POOL_SIZE	0x400000UL

/* GPU control (panthor_gpu_regs.h / panthor_fw_regs.h names) */
#define GPU_ID			0x000
#define GPU_MMU_FEATURES	0x014
#define GPU_AS_PRESENT		0x018
#define GPU_CSF_ID		0x01c
#define GPU_INT_RAWSTAT		0x020
#define GPU_INT_CLEAR		0x024
#define GPU_CMD			0x030
#define GPU_STATUS		0x034
#define GPU_FAULT_STATUS	0x03c
#define L2_READY		0x160
#define L2_PWRON		0x1a0
#define L2_PWRTRANS		0x220
#define GPU_COHERENCY_PROTOCOL	0x304
#define MCU_CONTROL		0x700
#define MCU_STATUS		0x704
#define JOB_INT_RAWSTAT		0x1000
#define JOB_INT_CLEAR		0x1004
#define JOB_INT_MASK		0x1008
#define AS0			0x2400
#define AS_TRANSTAB		0x00
#define AS_MEMATTR		0x08
#define AS_COMMAND		0x18
#define AS_FAULTSTATUS		0x1c
#define AS_FAULTADDRESS		0x20
#define AS_STATUS		0x28
#define AS_TRANSCFG		0x30

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t g(uint32_t off) { return rd(GPU + off); }
static inline void gw(uint32_t off, uint32_t v) { wr(GPU + off, v); }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

/* poll until (reg & mask) == want, steps of 10 us; returns steps or ~0u */
static uint32_t poll(uint32_t off, uint32_t mask, uint32_t want, uint32_t max_steps)
{
	for (uint32_t i = 0; i < max_steps; i++) {
		if ((g(off) & mask) == want)
			return i;
		udelay(10);
	}
	return ~0u;
}

/* ---- a bump allocator of 4 KiB pages in POOL ---- */
static uintptr_t pool_next = POOL;

static uintptr_t page_alloc(uint32_t n)
{
	uintptr_t p = pool_next;
	pool_next += (uintptr_t)n * 0x1000;
	if (pool_next > POOL + POOL_SIZE)
		return 0;
	volatile uint64_t *q = (volatile uint64_t *)p;
	for (uint32_t i = 0; i < n * 512u; i++)
		q[i] = 0;
	return p;
}

/* ---- AArch64 4K stage-1 tables, levels 0..3 ---- */
static uintptr_t pt_root;
static uint32_t pt_levels;		/* from va_bits */

#define PTE_TABLE	3ull
#define PTE_PAGE	3ull
#define PTE_AF		(1ull << 10)
#define PTE_NG		(1ull << 11)
#define PTE_SH_OS	(2ull << 8)
#define PTE_AP_UNPRIV	(1ull << 6)
#define PTE_AP_RDONLY	(1ull << 7)
#define PTE_XN		(1ull << 54)

static int map_page(uint64_t va, uint64_t pa, uint64_t attrs)
{
	uintptr_t table = pt_root;
	for (uint32_t lvl = 4 - pt_levels; lvl < 3; lvl++) {
		uint32_t shift = 12 + 9 * (3 - lvl);
		volatile uint64_t *e = (volatile uint64_t *)(table + ((va >> shift) & 511) * 8);
		if (!(*e & 1)) {
			uintptr_t t = page_alloc(1);
			if (!t)
				return -1;
			*e = (uint64_t)t | PTE_TABLE;
		}
		table = (uintptr_t)(*e & 0x0000fffffffff000ull);
	}
	volatile uint64_t *e = (volatile uint64_t *)(table + ((va >> 12) & 511) * 8);
	*e = pa | PTE_PAGE | PTE_AF | PTE_NG | PTE_SH_OS | PTE_AP_UNPRIV | attrs;
	return 0;
}

static uintptr_t shared_pa;

static int load_fw(uint32_t *nsec)
{
	const uint8_t *d = mali_fw;
	if (*(const uint32_t *)d != 0xc3f13a6eu)
		return -1;
	uint32_t size = *(const uint32_t *)(d + 16), off = 20;
	*nsec = 0;
	while (off < size) {
		uint32_t eh = *(const uint32_t *)(d + off), type = eh & 0xff, esz = (eh >> 8) & 0xff;
		if (!esz)
			return -2;
		if (type == 0) {
			const uint32_t *h = (const uint32_t *)(d + off + 4);
			uint32_t fl = h[0], vs = h[1], ve = h[2], ds = h[3], de = h[4];
			if (!(fl & (1u << 5)) && ve > vs) {	/* PROT sections: skipped */
				uint32_t pages = (ve - vs) >> 12;
				uintptr_t pa = page_alloc(pages);	/* zeroed */
				if (!pa)
					return -3;
				volatile uint8_t *dst = (volatile uint8_t *)pa;
				for (uint32_t i = 0; i < de - ds; i++)
					dst[i] = d[ds + i];
				uint64_t attrs = 0;
				if (!(fl & 2u))
					attrs |= PTE_AP_RDONLY;
				if (!(fl & 4u))
					attrs |= PTE_XN;
				for (uint32_t i = 0; i < pages; i++)
					if (map_page(vs + i * 0x1000ull, pa + i * 0x1000ull, attrs))
						return -4;
				if (vs == 0x04000000u)
					shared_pa = pa;
				(*nsec)++;
			}
		}
		off += esz;
	}
	return shared_pa ? 0 : -5;
}

static uint32_t gpueb_power_on(void)
{
	uint32_t magic = rd(GPUEB_GPR + 13 * 4);
	if (!magic)
		return ~0u;
	uint32_t msg[8] = { magic, 6, 0, 0, 1, 0, 0, 0 };
	for (int i = 0; i < 8; i++)
		wr(MBOX0 + 16 + i * 4, msg[i]);
	wr(MBOX0_SET, 0x2);
	for (uint32_t t = 0; t < 1000; t++) {
		uint32_t r = rd(MBOX0_RECV);
		if (r) {
			wr(MBOX0_CLR, r);
			return t;
		}
		udelay(100);
	}
	return ~1u;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_skip();		/* the camera hole covers the first lines */
	con_str("LAPIS", "tcsf v1: Mali CSF firmware boot");
	if ((rd(0x1c001eb4) & 0xc0000000u) != 0xc0000000u) {
		con_str("FAIL", "mfg0 domain off");
		goto out;
	}
	con_hex2("POWER", "gpueb ipi x100us=", gpueb_power_on(), "gpu_id=", g(GPU_ID));
	uint32_t mmuf = g(GPU_MMU_FEATURES), va_bits = mmuf & 0xff;
	con_hex2("GPU", "mmu_features=", mmuf, "as_present=", g(GPU_AS_PRESENT));
	con_hex2("GPU", "csf_id=", g(GPU_CSF_ID), "mcu_status=", g(MCU_STATUS));
	if (g(GPU_ID) == 0) {
		con_str("FAIL", "GPU not powered");
		goto out;
	}

	/* soft reset, coherency off, L2 on */
	gw(GPU_INT_CLEAR, 0xffffffff);
	gw(GPU_CMD, 0x101);
	uint32_t rs = poll(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000);
	gw(GPU_INT_CLEAR, 0xffffffff);
	gw(GPU_COHERENCY_PROTOCOL, 31);
	(void)poll(L2_PWRTRANS, 1, 0, 2000);
	gw(L2_PWRON, 1);
	uint32_t l2 = poll(L2_READY, 1, 1, 2000);
	con_hex2("RESET", "done x10us=", rs, "l2 on x10us=", l2);

	/* MCU address space: 4 GiB of VA, va_bits from MMU_FEATURES */
	pt_levels = va_bits > 39 ? 4 : 3;
	pt_root = page_alloc(1);
	uint32_t nsec = 0;
	int lr = load_fw(&nsec);
	con_hex2("FW", "load result=", (uint32_t)lr, "sections=", nsec);
	con_hex2("FW", "shared pa=", (uint32_t)shared_pa, "pool used=", (uint32_t)(pool_next - POOL));
	if (lr)
		goto out;
	__asm__ volatile("dsb sy" ::: "memory");
	uint64_t transcfg = 6ull | ((uint64_t)(55 - va_bits) << 6) | (1ull << 24);
	gw(AS0 + AS_TRANSTAB, (uint32_t)pt_root);
	gw(AS0 + AS_TRANSTAB + 4, 0);
	gw(AS0 + AS_MEMATTR, 0x4c4c4c4c);
	gw(AS0 + AS_MEMATTR + 4, 0x4c4c4c4c);
	gw(AS0 + AS_TRANSCFG, (uint32_t)transcfg);
	gw(AS0 + AS_TRANSCFG + 4, (uint32_t)(transcfg >> 32));
	(void)poll(AS0 + AS_STATUS, 1, 0, 10000);
	gw(AS0 + AS_COMMAND, 1);	/* UPDATE */
	uint32_t asw = poll(AS0 + AS_STATUS, 1, 0, 10000);
	con_hex2("MMU", "as0 update x10us=", asw, "levels=", pt_levels);

	/* boot the MCU */
	gw(JOB_INT_CLEAR, 0xffffffff);
	gw(JOB_INT_MASK, 0xffffffff);
	gw(MCU_CONTROL, 2);		/* AUTO */
	uint32_t boot = poll(JOB_INT_RAWSTAT, 1u << 31, 1u << 31, 100000);	/* 1 s */
	con_hex2("MCU", "glb irq x10us=", boot, "mcu_status=", g(MCU_STATUS));
	con_hex2("MCU", "job_rawstat=", g(JOB_INT_RAWSTAT), "gpu_fault=", g(GPU_FAULT_STATUS));
	con_hex2("MMU", "as0 fault=", g(AS0 + AS_FAULTSTATUS), "addr=", g(AS0 + AS_FAULTADDRESS));
	volatile uint32_t *glb = (volatile uint32_t *)shared_pa;
	con_hex2("GLB", "version=", glb[0], "features=", glb[1]);
	con_hex2("GLB", "input_va=", glb[2], "output_va=", glb[3]);
	con_hex2("GLB", "group_num=", glb[4], "group_stride=", glb[5]);
	if (glb[0])
		con_str("DONE", "CSF firmware booted");
	else
		con_str("DONE", "firmware did not publish its interface");
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
