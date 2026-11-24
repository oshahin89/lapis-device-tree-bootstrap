/*
 * Lapis GPU: Mali-G615 MC2 (CSF) bring-up from xnu, step 1 of the kernel
 * driver (device/lapis_gpu_plan.md): what tools/lapis_shim/tcsf.c v7 proved
 * from the boot shim, in a kernel thread at boot:
 *  - power on through MediaTek's GPUEB: IPI 1 (gpufreq) over the mailbox,
 *    [magic from GPUEB GPR 13, 6 = CMD_POWER_CONTROL, 0, 0, 1, 0, 0, 0];
 *  - GPU soft reset, coherency NONE, L2 power on;
 *  - the CSF firmware mali_csffw.bin, carried by the dual boot image at
 *    physical 0x43800000 (tools/lapis_boot_pack.py --gpufw; Arm's binary is
 *    not in git), its sections copied into a pool at 0x58000000 (outside the
 *    RAM xnu manages, which starts at 0x140000000) and mapped at their MCU
 *    VAs in AArch64 4K tables built here (all non-cacheable), AS0 for the MCU,
 *    AS1 (same tables) for our group;
 *  - MCU_CONTROL = AUTO, wait for the global interface, GLB setup and PING;
 *  - group 0 / stream 0 with the user I/O and suspend buffers in the MCU
 *    shared region (0x04000000..0x08000000, as Android's kbase does), and a
 *    4-instruction command stream that stores 0xc0ffee42.
 * Step 2 keeps it up (see lg_service_locked); lapisgpu=2 stops it after the
 * self-test as step 1 did.
 * Register layout and sequence after Linux panthor (GPL, read for reference,
 * not copied) and Android's kbase (kprobed on this phone). lapisgpu=0 skips.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

void sweet_lapis_gpu_init(void);

#define LG_GPU_PA       0x13000000UL
#define LG_GPU_SIZE     0x110000UL      /* up to doorbell page 8 */
#define LG_GPUEB_PA     0x13c3f000UL    /* GPR at +0xd1c, mailbox slots at +0xd80 */
#define LG_MBOXREG_PA   0x13c62000UL    /* send +0, set +4, clr +0x74, recv +0x78 */
#define LG_SCPSYS_PA    0x1c001000UL
#define LG_FW_PA        0x43800000UL
#define LG_FW_MAX       0x48000UL
#define LG_POOL_PA      0x58000000UL
#define LG_POOL_SIZE    0x400000UL

#define GPU_ID          0x000
#define GPU_MMU_FEATURES 0x014
#define GPU_INT_RAWSTAT 0x020
#define GPU_INT_CLEAR   0x024
#define GPU_CMD         0x030
#define GPU_TILER_PRESENT 0x110
#define L2_READY        0x160
#define L2_PWRON        0x1a0
#define L2_PWRTRANS     0x220
#define GPU_COHERENCY   0x304
#define MCU_CONTROL     0x700
#define MCU_STATUS      0x704
#define MMU_INT_RAWSTAT 0x2000
#define MMU_INT_CLEAR   0x2004
#define JOB_INT_RAWSTAT 0x1000
#define JOB_INT_CLEAR   0x1004
#define JOB_INT_MASK    0x1008
#define AS(n)           (0x2400u + (n) * 0x40u)
#define AS_TRANSTAB     0x00
#define AS_MEMATTR      0x08
#define AS_LOCKADDR     0x10
#define AS_COMMAND      0x18
#define AS_FAULTSTATUS  0x1c
#define AS_STATUS       0x28
#define AS_TRANSCFG     0x30
#define LG_RING         0x1000u
#define GLB_EVT_MASK    0x07f00000u     /* panthor GLB_EVT_MASK */
#define CSG_EVT_MASK    0xb0000000u     /* SYNC_UPDATE | IDLE | PROGRESS_TIMER */
#define CS_EVT_MASK     0xc4000000u     /* FAULT | FATAL | TILER_OOM */
#define DOORBELL(n)     (0x80000u + (n) * 0x10000u)

#define VA_SHARED       0x04000000u
#define VA_RING         0x10000000u
#define VA_TARGET       0x10003000u
#define VA_UIO_IN       0x04100000u
#define VA_UIO_OUT      0x04101000u
#define VA_SUSPEND      0x04110000u
#define VA_PSUSPEND     0x04120000u

#define PTE_VALID3      3ull
#define PTE_AF          (1ull << 10)
#define PTE_NG          (1ull << 11)
#define PTE_SH_OS       (2ull << 8)
#define PTE_AP_UNPRIV   (1ull << 6)
#define PTE_AP_RDONLY   (1ull << 7)
#define PTE_XN          (1ull << 54)

static vm_offset_t lg_gpu, lg_gpueb, lg_mbox, lg_scp, lg_fw, lg_pool;
static uint64_t lg_pool_next = LG_POOL_PA, lg_root;
static uint64_t lg_shared_pa;
static unsigned lg_levels;
static int lg_sleep_chan;
static uint64_t lg_transcfg;
static uint32_t lg_cs_features, lg_ngroups, lg_group_stride;
/* the command stream groups by CSG slot; 0 is the kernel's own test queue */
struct lg_slot {
	volatile uint32_t *csgin, *csgout, *csin, *csout;
	int active, fatal;
};
static struct lg_slot lg_slots[8];
static uint32_t lg_as_faulty;           /* AS disabled after a fault, set up again by the next group */
static int lg_slot_vm(unsigned n);
static volatile uint32_t *lg_gin, *lg_gout, *lg_csgin, *lg_csgout, *lg_csin, *lg_csout;
static volatile uint64_t *lg_ring, *lg_uin, *lg_uout;
static volatile uint32_t *lg_tgt;

static uint32_t
lg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
lg_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint32_t g(uint32_t off) { return lg_rd(lg_gpu, off); }
static void gw(uint32_t off, uint32_t v) { lg_wr(lg_gpu, off, v); }

/* (reg & mask) == want, steps of 10 us; the steps taken, or ~0u */
static uint32_t
lg_poll(uint32_t off, uint32_t mask, uint32_t want, uint32_t steps)
{
	for (uint32_t i = 0; i < steps; i++) {
		if ((g(off) & mask) == want) {
			return i;
		}
		delay(10);
	}
	return ~0u;
}

static volatile void *
lg_pool_va(uint64_t pa)
{
	return (volatile void *)(lg_pool + (pa - LG_POOL_PA));
}

static uint64_t lg_free_list;           /* single freed pages, first word = next */

static uint64_t
lg_alloc(uint32_t pages)
{
	if (pages == 1 && lg_free_list) {
		uint64_t fp = lg_free_list;
		volatile uint64_t *q = lg_pool_va(fp);
		lg_free_list = q[0];
		for (int i = 0; i < 512; i++) {
			q[i] = 0;
		}
		return fp;
	}
	uint64_t pa = lg_pool_next;
	if (pa + (uint64_t)pages * 0x1000 > LG_POOL_PA + LG_POOL_SIZE) {
		return 0;
	}
	lg_pool_next += (uint64_t)pages * 0x1000;
	volatile uint64_t *q = lg_pool_va(pa);
	for (uint32_t i = 0; i < pages * 512u; i++) {
		q[i] = 0;
	}
	return pa;
}

static void
lg_free(uint64_t pa, uint32_t pages)
{
	for (uint32_t i = 0; i < pages; i++) {
		volatile uint64_t *q = lg_pool_va(pa + i * 0x1000ull);
		q[0] = lg_free_list;
		lg_free_list = pa + i * 0x1000ull;
	}
}

static int
lg_map_in(uint64_t root, uint64_t va, uint64_t pa, uint64_t attrs)
{
	uint64_t table = root;
	for (unsigned lvl = 4 - lg_levels; lvl < 3; lvl++) {
		unsigned shift = 12 + 9 * (3 - lvl);
		volatile uint64_t *e = (volatile uint64_t *)lg_pool_va(table) + ((va >> shift) & 511);
		if (!(*e & 1)) {
			uint64_t t = lg_alloc(1);
			if (!t) {
				return -1;
			}
			*e = t | PTE_VALID3;
		}
		table = *e & 0x0000fffffffff000ull;
	}
	volatile uint64_t *e = (volatile uint64_t *)lg_pool_va(table) + ((va >> 12) & 511);
	*e = pa | PTE_VALID3 | PTE_AF | PTE_NG | PTE_SH_OS | PTE_AP_UNPRIV | attrs;
	return 0;
}

static int
lg_map(uint64_t va, uint64_t pa, uint64_t attrs)
{
	return lg_map_in(lg_root, va, pa, attrs);
}

/* clear the leaf entry of va, if any (tables stay until the VM goes) */
static void
lg_unmap_in(uint64_t root, uint64_t va)
{
	uint64_t table = root;
	for (unsigned lvl = 4 - lg_levels; lvl < 3; lvl++) {
		unsigned shift = 12 + 9 * (3 - lvl);
		volatile uint64_t *e = (volatile uint64_t *)lg_pool_va(table) + ((va >> shift) & 511);
		if (!(*e & 1)) {
			return;
		}
		table = *e & 0x0000fffffffff000ull;
	}
	((volatile uint64_t *)lg_pool_va(table))[(va >> 12) & 511] = 0;
}

/* a table and the tables under it back to the pool (not the leaves' pages) */
static void
lg_free_tables(uint64_t table, unsigned lvl)
{
	if (lvl < 3) {
		volatile uint64_t *t = lg_pool_va(table);
		for (int i = 0; i < 512; i++) {
			if (t[i] & 1) {
				lg_free_tables(t[i] & 0x0000fffffffff000ull, lvl + 1);
			}
		}
	}
	lg_free(table, 1);
}

static uint64_t
lg_map_new(uint32_t va, uint32_t pages)
{
	uint64_t pa = lg_alloc(pages);
	if (!pa) {
		return 0;
	}
	for (uint32_t i = 0; i < pages; i++) {
		if (lg_map(va + i * 0x1000ull, pa + i * 0x1000ull, 0)) {
			return 0;
		}
	}
	return pa;
}

static int
lg_load_fw(unsigned *nsec)
{
	const uint8_t *d = (const uint8_t *)lg_fw;
	if (*(const volatile uint32_t *)d != 0xc3f13a6eu) {
		return -1;
	}
	uint32_t size = *(const volatile uint32_t *)(d + 16), off = 20;
	if (size > LG_FW_MAX) {
		return -2;
	}
	*nsec = 0;
	while (off < size) {
		uint32_t eh = *(const volatile uint32_t *)(d + off), esz = (eh >> 8) & 0xff;
		if (!esz) {
			return -3;
		}
		if ((eh & 0xff) == 0) {
			const volatile uint32_t *h = (const volatile uint32_t *)(d + off + 4);
			uint32_t fl = h[0], vs = h[1], ve = h[2], ds = h[3], de = h[4];
			if (!(fl & (1u << 5)) && ve > vs) {     /* PROT sections skipped */
				uint32_t pages = (ve - vs) >> 12;
				uint64_t pa = lg_alloc(pages);  /* zeroed */
				if (!pa || de < ds || de > LG_FW_MAX) {  /* size is the header length */
					return -4;
				}
				volatile uint8_t *dst = lg_pool_va(pa);
				for (uint32_t i = 0; i < de - ds; i++) {
					dst[i] = d[ds + i];
				}
				uint64_t attrs = ((fl & 2u) ? 0 : PTE_AP_RDONLY) | ((fl & 4u) ? 0 : PTE_XN);
				for (uint32_t i = 0; i < pages; i++) {
					if (lg_map(vs + i * 0x1000ull, pa + i * 0x1000ull, attrs)) {
						return -5;
					}
				}
				if (vs == VA_SHARED) {
					lg_shared_pa = pa;
				}
				(*nsec)++;
			}
		}
		off += esz;
	}
	return lg_shared_pa ? 0 : -6;
}

/* a CPU pointer to an MCU VA inside the shared section */
static volatile uint32_t *
lg_sh(uint32_t va)
{
	return lg_pool_va(lg_shared_pa + (va - VA_SHARED));
}

static void
lg_toggle(volatile uint32_t *req, uint32_t ack, uint32_t mask)
{
	*req = ((ack ^ mask) & mask) | (*req & ~mask);
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint32_t
lg_acked(volatile uint32_t *req, volatile uint32_t *ack, uint32_t mask, uint32_t steps)
{
	for (uint32_t i = 0; i < steps; i++) {
		if (((*req ^ *ack) & mask) == 0) {
			return i;
		}
		delay(10);
	}
	return ~0u;
}

static uint32_t
lg_gpueb_power(int on)
{
	uint32_t magic = lg_rd(lg_gpueb, 0xd1c + 13 * 4);
	if (!magic) {
		return ~0u;
	}
	uint32_t msg[8] = { magic, 6, 0, 0, on ? 1u : 0u, 0, 0, 0 };
	for (int i = 0; i < 8; i++) {
		lg_wr(lg_gpueb, 0xd80 + 16 + i * 4, msg[i]);
	}
	lg_wr(lg_mbox, 0x04, 0x2);
	for (uint32_t t = 0; t < 1000; t++) {
		uint32_t r = lg_rd(lg_mbox, 0x78);
		if (r) {
			lg_wr(lg_mbox, 0x74, r);
			return t;
		}
		delay(100);
	}
	return ~1u;
}

static void
lg_as_setup(unsigned n, uint64_t root, uint64_t transcfg)
{
	gw(AS(n) + AS_TRANSTAB, (uint32_t)root);
	gw(AS(n) + AS_TRANSTAB + 4, (uint32_t)(root >> 32));
	gw(AS(n) + AS_MEMATTR, 0x4c4c4c4c);
	gw(AS(n) + AS_MEMATTR + 4, 0x4c4c4c4c);
	gw(AS(n) + AS_TRANSCFG, (uint32_t)transcfg);
	gw(AS(n) + AS_TRANSCFG + 4, (uint32_t)(transcfg >> 32));
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
	gw(AS(n) + AS_COMMAND, 1);              /* UPDATE */
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
}

/* after table changes: lock the whole VA space, then FLUSH_MEM (which unlocks) */
static void
lg_as_flush(unsigned n)
{
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
	gw(AS(n) + AS_LOCKADDR, 47);            /* base 0, 2^48 bytes */
	gw(AS(n) + AS_LOCKADDR + 4, 0);
	gw(AS(n) + AS_COMMAND, 2);              /* LOCK */
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
	gw(AS(n) + AS_COMMAND, 5);              /* FLUSH_MEM */
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
}

static void
lg_stop(void)
{
	gw(MCU_CONTROL, 0);
	(void)lg_poll(MCU_STATUS, 3, 0, 10000);
	gw(GPU_INT_CLEAR, 0xffffffff);
	gw(GPU_CMD, 0x101);
	(void)lg_poll(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000);
	(void)lg_gpueb_power(0);
}

static int
lg_run(void)
{
	lg_gpu = ml_io_map(LG_GPU_PA, LG_GPU_SIZE);
	lg_gpueb = ml_io_map(LG_GPUEB_PA, 0x1000);
	lg_mbox = ml_io_map(LG_MBOXREG_PA, 0x1000);
	lg_scp = ml_io_map(LG_SCPSYS_PA, 0x1000);
	lg_fw = ml_io_map(LG_FW_PA, LG_FW_MAX);
	lg_pool = ml_io_map(LG_POOL_PA, LG_POOL_SIZE);
	if (!lg_gpu || !lg_gpueb || !lg_mbox || !lg_scp || !lg_fw || !lg_pool) {
		printf("LAPIS GPU: map failed\n");
		return 0;
	}
	uint32_t pwr = lg_rd(lg_scp, 0xeb4);
	if ((pwr & 0xc0000000u) != 0xc0000000u) {
		printf("LAPIS GPU: mfg0 domain off (%#x), skipped\n", pwr);
		return 0;
	}
	uint32_t ipi = lg_gpueb_power(1);
	uint32_t id = g(GPU_ID), mmuf = g(GPU_MMU_FEATURES), va_bits = mmuf & 0xff;
	printf("LAPIS GPU: GPUEB power-on %u x100us, GPU_ID %#x, MMU_FEATURES %#x\n", ipi, id, mmuf);
	if (!id) {
		return 0;
	}
	gw(GPU_INT_CLEAR, 0xffffffff);
	gw(GPU_CMD, 0x101);
	uint32_t rs = lg_poll(GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 10000);
	gw(GPU_INT_CLEAR, 0xffffffff);
	gw(GPU_COHERENCY, 31);
	(void)lg_poll(L2_PWRTRANS, 1, 0, 2000);
	gw(L2_PWRON, 1);
	uint32_t l2 = lg_poll(L2_READY, 1, 1, 2000);
	lg_levels = va_bits > 39 ? 4 : 3;
	lg_root = lg_alloc(1);
	unsigned nsec = 0;
	int lr = lg_load_fw(&nsec);
	printf("LAPIS GPU: reset %u, L2 %u (x10us); firmware %d, %u sections, pool %#llx used\n",
	    rs, l2, lr, nsec, (unsigned long long)(lg_pool_next - LG_POOL_PA));
	if (lr) {
		lg_stop();
		return 0;
	}
	uint64_t transcfg = 6ull | ((uint64_t)(55 - va_bits) << 6) | (1ull << 24);
	lg_transcfg = transcfg;
	lg_as_setup(0, lg_root, transcfg);
	gw(JOB_INT_CLEAR, 0xffffffff);
	gw(JOB_INT_MASK, 0xffffffff);
	gw(MCU_CONTROL, 2);                     /* AUTO */
	uint32_t boot = lg_poll(JOB_INT_RAWSTAT, 1u << 31, 1u << 31, 100000);
	volatile uint32_t *glb = lg_sh(VA_SHARED);
	printf("LAPIS GPU: MCU boot %u x10us, status %u, interface %#x, %u groups\n",
	    boot, g(MCU_STATUS), glb[0], glb[4]);
	if (!glb[0]) {
		lg_stop();
		return 0;
	}

	/* GLB setup + PING */
	volatile uint32_t *gin = lg_sh(glb[2]), *gout = lg_sh(glb[3]);
	lg_gin = gin;
	lg_gout = gout;
	gin[0x18 / 4] = 0x5;
	gin[0x1c / 4] = 0;
	gin[0x10 / 4] = (uint32_t)((5ull * 500 * 1024 * 1024) >> 10);
	gin[0x14 / 4] = 0x7fffffffu;
	gin[0x04 / 4] = (1u << 2) | (1u << 8) | (1u << 1) | (1u << 3) | (1u << 10) | (1u << 26);
	gin[0] = (gin[0] & ~((1u << 10) | (1u << 7))) | (1u << 10) | (1u << 7);
	lg_toggle(&gin[0], gout[0], (1u << 2) | (1u << 3) | (1u << 1));
	gw(DOORBELL(0), 1);
	uint32_t c1 = lg_acked(&gin[0], &gout[0], (1u << 2) | (1u << 3) | (1u << 1), 10000);
	lg_toggle(&gin[0], gout[0], 1u << 8);
	gw(DOORBELL(0), 1);
	uint32_t c2 = lg_acked(&gin[0], &gout[0], 1u << 8, 10000);

	/* group 0 / stream 0 */
	volatile uint32_t *csgc = lg_sh(VA_SHARED + 0x1000);
	volatile uint32_t *csgin = lg_sh(csgc[1]), *csgout = lg_sh(csgc[2]);
	volatile uint32_t *csc = (volatile uint32_t *)((uintptr_t)csgc + 0x40);
	volatile uint32_t *csin = lg_sh(csc[1]), *csout = lg_sh(csc[2]);
	lg_csgin = csgin;
	lg_csgout = csgout;
	lg_csin = csin;
	lg_csout = csout;
	lg_slots[0] = (struct lg_slot){ csgin, csgout, csin, csout, 1, 0 };
	lg_cs_features = csc[0];
	lg_ngroups = glb[4] < 8 ? glb[4] : 8;
	lg_group_stride = glb[5];
	uint32_t susp = csgc[0x0c / 4], psusp = csgc[0x10 / 4];
	uint64_t ring = lg_map_new(VA_RING, 1), uin = lg_map_new(VA_UIO_IN, 1);
	uint64_t uout = lg_map_new(VA_UIO_OUT, 1), target = lg_map_new(VA_TARGET, 1);
	uint64_t sbuf = lg_map_new(VA_SUSPEND, (susp + 0xfff) >> 12);
	uint64_t pbuf = psusp ? lg_map_new(VA_PSUSPEND, (psusp + 0xfff) >> 12) : 1;
	if (!ring || !uin || !uout || !target || !sbuf || !pbuf) {
		printf("LAPIS GPU: pool exhausted\n");
		lg_stop();
		return 0;
	}
	lg_as_setup(1, lg_root, transcfg);
	gw(AS(0) + AS_COMMAND, 1);              /* AS0 sees the new pages too */
	(void)lg_poll(AS(0) + AS_STATUS, 1, 0, 10000);

	*(volatile uint64_t *)&csgin[0x20 / 4] = 0x5;
	*(volatile uint64_t *)&csgin[0x28 / 4] = 0x5;
	csgin[0x30 / 4] = g(GPU_TILER_PRESENT);
	csgin[0x34 / 4] = 2u | (2u << 8) | (g(GPU_TILER_PRESENT) ? (1u << 16) : 0) | (1u << 28);
	*(volatile uint64_t *)&csgin[0x40 / 4] = VA_SUSPEND;
	*(volatile uint64_t *)&csgin[0x48 / 4] = psusp ? VA_PSUSPEND : 0;
	csgin[0x50 / 4] = 1;                    /* AS1 */
	csgin[0x04 / 4] = ~0u;
	csgin[0] = (csgin[0] & ~7u) | 1u;       /* START */
	lg_toggle(&csgin[0], csgout[0], 1u << 4);
	lg_toggle(&gin[0x08 / 4], gout[0x08 / 4], 1u);
	gw(DOORBELL(0), 1);
	uint32_t c3 = lg_acked(&csgin[0], &csgout[0], 7u | (1u << 4), 10000);

	*(volatile uint64_t *)&csin[0x10 / 4] = VA_RING;
	csin[0x18 / 4] = 0x1000;
	*(volatile uint64_t *)&csin[0x30 / 4] = VA_UIO_IN;
	*(volatile uint64_t *)&csin[0x38 / 4] = VA_UIO_OUT;
	csin[0x04 / 4] = 1u | (1u << 8);        /* priority 1, doorbell 1 */
	csin[0x0c / 4] = ~0u;
	csin[0] = (csin[0] & ~(0x1500u | 7u)) | 0x1500u | 1u;
	lg_toggle(&csgin[0x08 / 4], csgout[0x08 / 4], 1u);
	lg_toggle(&gin[0x08 / 4], gout[0x08 / 4], 1u);
	gw(DOORBELL(0), 1);
	uint32_t c4 = lg_acked(&csin[0], &csout[0], 7u, 10000);

	lg_ring = lg_pool_va(ring);
	lg_uin = lg_pool_va(uin);
	lg_uout = lg_pool_va(uout);
	lg_tgt = lg_pool_va(target);
	*lg_tgt = 0x11111111;
	lg_uin[1] = 0;
	printf("LAPIS GPU: GLB config %u ping %u, CSG start %u, CS start %u (x10us)\n", c1, c2, c3, c4);
	return 1;
}

/*
 * Step 2: the GPU stays up. Our queue (group 0 / stream 0, AS1) takes any
 * number of submissions: instructions go into the 4 KiB ring at the insert
 * offset (modulo the ring), user I/O insert advances, user doorbell 1 rings.
 * No interrupt line is wired in this port (touch and keys poll too), so a
 * service thread does what panthor's IRQ handlers do: clear JOB_INT, ack the
 * GLB / CSG / CS events the firmware raised, log GPU, MMU and stream faults.
 */
LCK_GRP_DECLARE(lg_lck_grp, "lapisgpu");
LCK_MTX_DECLARE(lg_mtx, &lg_lck_grp);
static uint32_t lg_nfault, lg_nevent;

static uint64_t
lg_now_us(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
lg_service_locked(void)
{
	uint32_t gi = g(GPU_INT_RAWSTAT) & ~((1u << 8) | (1u << 9) | (1u << 10)); /* reset/L2/clean done */
	if (gi) {
		gw(GPU_INT_CLEAR, gi);
		if ((gi & 1u) && lg_nfault++ < 8) {     /* GPU_FAULT */
			printf("LAPIS GPU: GPU fault status %#x, int %#x\n", g(0x3c), gi);
		}
	}
	/* bits 0..15 are page faults; 16..31 REQ_COMPLETE, set again by every AS
	 * command (v46 took them for faults and redid the AS disable every tick) */
	uint32_t mi = g(MMU_INT_RAWSTAT) & 0xffffu;
	if (mi) {
		gw(MMU_INT_CLEAR, mi);
		for (unsigned n = 0; n < 8; n++) {
			if (!(mi & (1u << n))) {
				continue;
			}
			if (lg_nfault++ < 8) {
				printf("LAPIS GPU: AS%u fault %#x at %#x%08x\n", n, g(AS(n) + AS_FAULTSTATUS),
				    g(AS(n) + 0x24), g(AS(n) + 0x20));
			}
			if (n >= 2) {
				/*
				 * As panthor: disable the faulting AS (unmapped: the stalled
				 * access fails and so does its stream) and fail its groups.
				 * v45: without this the group stayed stalled, would not
				 * terminate, and freeing it under the firmware killed the MCU.
				 */
				gw(AS(n) + AS_TRANSTAB, 0);
				gw(AS(n) + AS_TRANSTAB + 4, 0);
				gw(AS(n) + AS_MEMATTR, 0);
				gw(AS(n) + AS_MEMATTR + 4, 0);
				gw(AS(n) + AS_TRANSCFG, 1);
				gw(AS(n) + AS_TRANSCFG + 4, 0);
				gw(AS(n) + AS_COMMAND, 1);
				(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
				lg_as_faulty |= 1u << n;
				for (unsigned k = 1; k < 8; k++) {
					if (lg_slots[k].active && lg_slot_vm(k) == (int)n - 1) {
						lg_slots[k].fatal = 1;
					}
				}
			}
		}
	}
	uint32_t ji = g(JOB_INT_RAWSTAT);
	if (!ji) {
		return;
	}
	gw(JOB_INT_CLEAR, ji);
	lg_nevent++;
	if (ji & (1u << 31)) {                  /* GLB events: req follows ack */
		lg_gin[0] = (lg_gin[0] & ~GLB_EVT_MASK) | (lg_gout[0] & GLB_EVT_MASK);
	}
	for (unsigned n = 0; n < 8; n++) {
		struct lg_slot *sl = &lg_slots[n];
		if (!(ji & (1u << n)) || !sl->active) {
			continue;
		}
		uint32_t csev = (sl->csin[0] ^ sl->csout[0]) & CS_EVT_MASK;
		if (csev) {
			if (lg_nfault++ < 8) {
				printf("LAPIS GPU: CSG%u CS0 event %#x, fault %#x fatal %#x, cmd ptr %#x\n", n, csev,
				    sl->csout[0x80 / 4], sl->csout[0x84 / 4], sl->csout[0x40 / 4]);
			}
			if (csev & (1u << 30)) {
				sl->fatal = 1;
			}
			sl->csin[0] = (sl->csin[0] & ~CS_EVT_MASK) | (sl->csout[0] & CS_EVT_MASK);
		}
		sl->csgin[0x0c / 4] = sl->csgout[0x0c / 4];     /* cs_irq_ack */
		sl->csgin[0] = (sl->csgin[0] & ~CSG_EVT_MASK) | (sl->csgout[0] & CSG_EVT_MASK);
		if (csev) {                     /* the firmware rechecks the stream */
			lg_toggle(&sl->csgin[0x08 / 4], sl->csgout[0x08 / 4], 1u);
			lg_toggle(&lg_gin[0x08 / 4], lg_gout[0x08 / 4], 1u << n);
			gw(DOORBELL(0), 1);
		}
	}
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Run instructions on our queue; 0 or -1 if the ring is full, -2 if down. */
static int
lg_submit_locked(const uint64_t *ins, uint32_t n)
{
	if (!lg_ring) {
		return -2;
	}
	/*
	 * Whole 64-byte blocks, NOP (0) padded: the stream fetches 64 bytes at a
	 * time and would replay a stale copy of the rest of a block it already
	 * read (v41: of 300 32-byte submissions every second one was skipped).
	 */
	uint64_t insert = lg_uin[0];
	uint32_t padded = (n + 7u) & ~7u;
	if (padded > LG_RING / 8 || insert + padded * 8 - lg_uout[0] > LG_RING) {
		return -1;
	}
	for (uint32_t i = 0; i < padded; i++) {
		lg_ring[((insert / 8) + i) % (LG_RING / 8)] = i < n ? ins[i] : 0;
	}
	__asm__ volatile("dsb osh" ::: "memory");
	lg_uin[0] = insert + padded * 8;
	__asm__ volatile("dsb osh" ::: "memory");
	gw(DOORBELL(1), 1);
	return 0;
}

int sweet_lapis_gpu_submit(const uint64_t *ins, uint32_t n);
int
sweet_lapis_gpu_submit(const uint64_t *ins, uint32_t n)
{
	lck_mtx_lock(&lg_mtx);
	int r = lg_submit_locked(ins, n);
	lck_mtx_unlock(&lg_mtx);
	return r;
}

/* one store of value to the target page; microseconds to see it, or ~0u */
static uint32_t
lg_test_store(uint32_t value)
{
	uint64_t ins[4] = {
		(1ull << 56) | VA_TARGET,                       /* MOVE48 r0 */
		(2ull << 56) | (2ull << 48) | value,            /* MOVE32 r2 */
		(21ull << 56) | (2ull << 48) | (1ull << 16),    /* STORE_MULTIPLE [r0] = r2 */
		(3ull << 56) | (0xffffull << 16),               /* WAIT all */
	};
	uint64_t t0 = lg_now_us();
	lck_mtx_lock(&lg_mtx);
	int r = lg_submit_locked(ins, 4);
	lck_mtx_unlock(&lg_mtx);
	if (r) {
		return ~0u;
	}
	while (*lg_tgt != value) {
		uint64_t t = lg_now_us() - t0;
		if (t > 100000) {
			return ~0u;
		}
		if (t > 200) {
			lck_mtx_lock(&lg_mtx);
			lg_service_locked();
			lck_mtx_unlock(&lg_mtx);
		}
		delay(5);
	}
	return (uint32_t)(lg_now_us() - t0);
}

static uint32_t
lg_ping(void)
{
	lck_mtx_lock(&lg_mtx);
	lg_toggle(&lg_gin[0], lg_gout[0], 1u << 8);
	gw(DOORBELL(0), 1);
	uint32_t r = lg_acked(&lg_gin[0], &lg_gout[0], 1u << 8, 10000);
	lck_mtx_unlock(&lg_mtx);
	return r;
}

static void
lg_sleep_ms(uint32_t ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&lg_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * Step 3: what LapisGPU.cpp (panthor's UAPI for Mesa) is built on. VMs
 * 1..LG_NVM: own page tables (pool), on AS 1 + id, the user's addresses
 * below LG_KVA and the kernel's ring and sync page of each group above it.
 * Groups: CSG slots 1..7, one queue (CS0) each, user I/O and suspend buffers
 * in the MCU shared region (AS0), doorbell slot + 1. A submission is panthor's
 * ring entry: CALL the user's stream, wait for its work, store the
 * submission's number into the group's sync page.
 */
#define LG_NVM          4
#define LG_KVA          0xff0000000000ull
#define LG_SLOT_KVA(n)  (LG_KVA + (uint64_t)(n) * 0x100000ull)
#define LG_SLOT_MCU(n)  (0x04200000u + (uint32_t)(n) * 0x100000u)
#define LG_RING_MAX     0x10000u

static uint64_t lg_vm_root[LG_NVM + 1];
static int lg_trace;                    /* lapisgputrace=1: steps of group create, each waited for the log writer */

static void
lg_step(int n, const char *what, uint32_t a, uint32_t b)
{
	if (lg_trace) {
		printf("LAPIS GPU: group %d: %s %#x %#x\n", n, what, a, b);
		lg_sleep_ms(1200);
	}
}
struct lg_group {
	int vm;
	uint32_t ring_size, susp_pages, psusp_pages, seq;
	uint64_t ring_pa, uin_pa, uout_pa, sync_pa, susp_pa, psusp_pa;
	volatile uint64_t *ring, *uin, *uout;
	volatile uint32_t *sync;
};
static struct lg_group lg_groups[8];

static int
lg_slot_vm(unsigned n)
{
	return lg_groups[n].vm;
}

int lapis_gpu_ready(void);
uint32_t lapis_gpu_reg(uint32_t off);
void lapis_gpu_csif(uint32_t out[5]);
int lapis_gpu_vm_create(void);
void lapis_gpu_vm_destroy(int vm);
int lapis_gpu_vm_map(int vm, uint64_t va, uint64_t pa, int readonly, int noexec);
void lapis_gpu_vm_unmap(int vm, uint64_t va, uint64_t size);
int lapis_gpu_vm_map_kernel(int vm, uint64_t va, uint64_t pa, int noexec);
void lapis_gpu_vm_unmap_kernel(int vm, uint64_t va, uint64_t size);
void lapis_gpu_vm_flush(int vm);
int lapis_gpu_group_create(int vm, uint32_t ring_size);
void lapis_gpu_group_destroy(int slot);
uint32_t lapis_gpu_group_submit(int slot, uint64_t stream, uint32_t size);
int lapis_gpu_group_wait(int slot, uint32_t seq, uint64_t timeout_us);
int lapis_gpu_group_fatal(int slot);
uint32_t lapis_gpu_group_seq(int slot);

int
lapis_gpu_ready(void)
{
	return lg_ring != NULL;
}

uint32_t
lapis_gpu_reg(uint32_t off)
{
	return lg_ring && off < 0x400 ? g(off) : 0;
}

/* csg slots, cs slots, cs registers, scoreboards, registers the kernel keeps */
void
lapis_gpu_csif(uint32_t out[5])
{
	out[0] = lg_ngroups;
	out[1] = 1;
	out[2] = (lg_cs_features & 0xff) + 1;
	out[3] = (lg_cs_features >> 8) & 0xff;
	out[4] = 4;
}

/* the kernel's registers in a ring entry: the last 4 (panthor's choice) */
static uint32_t
lg_kreg(void)
{
	return (lg_cs_features & 0xff) + 1 - 4;
}

static int
lg_vm_ok(int vm)
{
	return vm >= 1 && vm <= LG_NVM && lg_vm_root[vm] && (g(0x18) & (1u << (1 + vm)));
}

int
lapis_gpu_vm_create(void)
{
	int id = 0;
	lck_mtx_lock(&lg_mtx);
	for (int i = 1; lg_ring && i <= LG_NVM; i++) {
		if (!lg_vm_root[i] && (g(0x18) & (1u << (1 + i)))) {
			uint64_t root = lg_alloc(1);
			if (root) {
				lg_vm_root[i] = root;
				lg_as_setup(1 + i, root, lg_transcfg);
				id = i;
			}
			break;
		}
	}
	lck_mtx_unlock(&lg_mtx);
	return id;
}

void
lapis_gpu_vm_destroy(int vm)
{
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm)) {
		for (int n = 1; n < 8; n++) {
			if (lg_slots[n].active && lg_groups[n].vm == vm) {
				printf("LAPIS GPU: VM %d destroyed under group %d\n", vm, n);
			}
		}
		gw(AS(1 + vm) + AS_TRANSTAB, 0);
		gw(AS(1 + vm) + AS_TRANSTAB + 4, 0);
		gw(AS(1 + vm) + AS_TRANSCFG, 1);         /* ADRMODE_UNMAPPED: everything faults */
		gw(AS(1 + vm) + AS_TRANSCFG + 4, 0);
		gw(AS(1 + vm) + AS_COMMAND, 1);
		lg_as_flush(1 + vm);
		lg_free_tables(lg_vm_root[vm], 4 - lg_levels);
		lg_vm_root[vm] = 0;
		lg_as_faulty &= ~(1u << (1 + vm));
	}
	lck_mtx_unlock(&lg_mtx);
}

/* one 4 KiB page; the caller flushes (lapis_gpu_vm_flush) after a batch */
int
lapis_gpu_vm_map(int vm, uint64_t va, uint64_t pa, int readonly, int noexec)
{
	int r = -1;
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm) && va < LG_KVA && !(va & 0xfff) && !(pa & 0xfff)) {
		r = lg_map_in(lg_vm_root[vm], va, pa, (readonly ? PTE_AP_RDONLY : 0) | (noexec ? PTE_XN : 0));
	}
	lck_mtx_unlock(&lg_mtx);
	return r;
}

void
lapis_gpu_vm_unmap(int vm, uint64_t va, uint64_t size)
{
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm) && va < LG_KVA) {
		for (uint64_t o = 0; o < size; o += 0x1000) {
			lg_unmap_in(lg_vm_root[vm], va + o);
		}
		lg_as_flush(1 + vm);
	}
	lck_mtx_unlock(&lg_mtx);
}

/* the kernel's part of a VM (at and above LG_KVA, below the group rings' area
 * only by convention: LapisGPU.cpp puts tiler heaps at LG_KVA + 1 GiB * (1 + id)) */
int
lapis_gpu_vm_map_kernel(int vm, uint64_t va, uint64_t pa, int noexec)
{
	int r = -1;
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm) && va >= LG_KVA + 0x40000000ull && !(va & 0xfff) && !(pa & 0xfff)) {
		r = lg_map_in(lg_vm_root[vm], va, pa, noexec ? PTE_XN : 0);
	}
	lck_mtx_unlock(&lg_mtx);
	return r;
}

void
lapis_gpu_vm_unmap_kernel(int vm, uint64_t va, uint64_t size)
{
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm) && va >= LG_KVA + 0x40000000ull) {
		for (uint64_t o = 0; o < size; o += 0x1000) {
			lg_unmap_in(lg_vm_root[vm], va + o);
		}
		lg_as_flush(1 + vm);
	}
	lck_mtx_unlock(&lg_mtx);
}

void
lapis_gpu_vm_flush(int vm)
{
	lck_mtx_lock(&lg_mtx);
	if (lg_vm_ok(vm)) {
		lg_as_flush(1 + vm);
	}
	lck_mtx_unlock(&lg_mtx);
}

static void
lg_group_free_locked(int n)
{
	struct lg_group *gr = &lg_groups[n];
	if (gr->vm && lg_vm_root[gr->vm]) {
		for (uint32_t o = 0; o < gr->ring_size; o += 0x1000) {
			lg_unmap_in(lg_vm_root[gr->vm], LG_SLOT_KVA(n) + o);
		}
		lg_unmap_in(lg_vm_root[gr->vm], LG_SLOT_KVA(n) + 0x80000);
		lg_as_flush(1 + gr->vm);
	}
	lg_unmap_in(lg_root, LG_SLOT_MCU(n));
	lg_unmap_in(lg_root, LG_SLOT_MCU(n) + 0x1000);
	for (uint32_t i = 0; i < gr->susp_pages; i++) {
		lg_unmap_in(lg_root, LG_SLOT_MCU(n) + 0x10000 + i * 0x1000);
	}
	for (uint32_t i = 0; i < gr->psusp_pages; i++) {
		lg_unmap_in(lg_root, LG_SLOT_MCU(n) + 0x80000 + i * 0x1000);
	}
	lg_as_flush(0);
	/* multi-page blocks go back page by page: the free list hands out single pages */
	if (gr->ring_pa) {
		lg_free(gr->ring_pa, gr->ring_size >> 12);
	}
	if (gr->uin_pa) {
		lg_free(gr->uin_pa, 1);
	}
	if (gr->uout_pa) {
		lg_free(gr->uout_pa, 1);
	}
	if (gr->sync_pa) {
		lg_free(gr->sync_pa, 1);
	}
	if (gr->susp_pa) {
		lg_free(gr->susp_pa, gr->susp_pages);
	}
	if (gr->psusp_pa) {
		lg_free(gr->psusp_pa, gr->psusp_pages);
	}
	bzero(gr, sizeof(*gr));
	bzero(&lg_slots[n], sizeof(lg_slots[n]));
}

/* a group with one queue on the VM; its CSG slot (1..7), or < 0 */
int
lapis_gpu_group_create(int vm, uint32_t ring_size)
{
	int n, r = -1;
	lck_mtx_lock(&lg_mtx);
	if (!lg_vm_ok(vm)) {
		goto out;
	}
	if (lg_as_faulty & (1u << (1 + vm))) {
		lg_as_setup(1 + vm, lg_vm_root[vm], lg_transcfg);
		lg_as_faulty &= ~(1u << (1 + vm));
	}
	for (n = 1; n < (int)lg_ngroups; n++) {
		if (!lg_slots[n].active && !lg_groups[n].vm) {
			break;
		}
	}
	if (n >= (int)lg_ngroups) {
		r = -2;
		goto out;
	}
	if (ring_size < 0x1000 || ring_size > LG_RING_MAX || (ring_size & (ring_size - 1))) {
		ring_size = 0x1000;
	}
	struct lg_group *gr = &lg_groups[n];
	volatile uint32_t *csgc = lg_sh(VA_SHARED + 0x1000 + n * lg_group_stride);
	volatile uint32_t *csc = (volatile uint32_t *)((uintptr_t)csgc + 0x40);
	uint32_t susp = csgc[3], psusp = csgc[4];
	gr->vm = vm;
	gr->ring_size = ring_size;
	gr->susp_pages = (susp + 0xfff) >> 12;
	gr->psusp_pages = (psusp + 0xfff) >> 12;
	if (gr->susp_pages > 0x70 || gr->psusp_pages > 0x80) {
		printf("LAPIS GPU: group %d suspend buffers too big (%#x/%#x)\n", n, susp, psusp);
		gr->susp_pages = gr->psusp_pages = 0;
		lg_group_free_locked(n);
		r = -3;
		goto out;
	}
	gr->ring_pa = lg_alloc(ring_size >> 12);
	gr->uin_pa = lg_alloc(1);
	gr->uout_pa = lg_alloc(1);
	gr->sync_pa = lg_alloc(1);
	gr->susp_pa = gr->susp_pages ? lg_alloc(gr->susp_pages) : 0;
	gr->psusp_pa = gr->psusp_pages ? lg_alloc(gr->psusp_pages) : 0;
	if (!gr->ring_pa || !gr->uin_pa || !gr->uout_pa || !gr->sync_pa ||
	    (gr->susp_pages && !gr->susp_pa) || (gr->psusp_pages && !gr->psusp_pa)) {
		lg_group_free_locked(n);
		r = -4;
		goto out;
	}
	int bad = 0;
	for (uint32_t o = 0; o < ring_size; o += 0x1000) {
		bad |= lg_map_in(lg_vm_root[vm], LG_SLOT_KVA(n) + o, gr->ring_pa + o, 0);
	}
	bad |= lg_map_in(lg_vm_root[vm], LG_SLOT_KVA(n) + 0x80000, gr->sync_pa, 0);
	bad |= lg_map(LG_SLOT_MCU(n), gr->uin_pa, 0);
	bad |= lg_map(LG_SLOT_MCU(n) + 0x1000, gr->uout_pa, 0);
	for (uint32_t i = 0; i < gr->susp_pages; i++) {
		bad |= lg_map(LG_SLOT_MCU(n) + 0x10000 + i * 0x1000, gr->susp_pa + i * 0x1000ull, 0);
	}
	for (uint32_t i = 0; i < gr->psusp_pages; i++) {
		bad |= lg_map(LG_SLOT_MCU(n) + 0x80000 + i * 0x1000, gr->psusp_pa + i * 0x1000ull, 0);
	}
	if (bad) {
		lg_group_free_locked(n);
		r = -5;
		goto out;
	}
	lg_step(n, "pages mapped, susp/psusp pages", gr->susp_pages, gr->psusp_pages);
	lg_as_flush(1 + vm);
	lg_as_flush(0);
	lg_step(n, "flushed AS0 and AS", 1 + vm, g(AS(1 + vm) + AS_STATUS));
	gr->ring = lg_pool_va(gr->ring_pa);
	gr->uin = lg_pool_va(gr->uin_pa);
	gr->uout = lg_pool_va(gr->uout_pa);
	gr->sync = lg_pool_va(gr->sync_pa);

	struct lg_slot *sl = &lg_slots[n];
	sl->csgin = lg_sh(csgc[1]);
	sl->csgout = lg_sh(csgc[2]);
	sl->csin = lg_sh(csc[1]);
	sl->csout = lg_sh(csc[2]);
	sl->fatal = 0;
	volatile uint32_t *ci = sl->csgin, *co = sl->csgout;
	uint32_t tiler = g(GPU_TILER_PRESENT);
	*(volatile uint64_t *)&ci[0x20 / 4] = g(0x100);         /* compute: every shader core */
	*(volatile uint64_t *)&ci[0x28 / 4] = g(0x100);         /* fragment */
	ci[0x30 / 4] = tiler;
	ci[0x34 / 4] = 2u | (2u << 8) | (tiler ? (1u << 16) : 0) | (1u << 28);
	*(volatile uint64_t *)&ci[0x40 / 4] = LG_SLOT_MCU(n) + 0x10000;
	*(volatile uint64_t *)&ci[0x48 / 4] = gr->psusp_pages ? LG_SLOT_MCU(n) + 0x80000 : 0;
	ci[0x50 / 4] = 1 + vm;                  /* AS */
	ci[0x04 / 4] = ~0u;
	ci[0] = (ci[0] & ~7u) | 1u;             /* START */
	lg_toggle(&ci[0], co[0], 1u << 4);      /* ENDPOINT_CONFIG */
	lg_toggle(&lg_gin[0x08 / 4], lg_gout[0x08 / 4], 1u << n);
	gw(DOORBELL(0), 1);
	uint32_t a1 = lg_acked(&ci[0], &co[0], 7u | (1u << 4), 10000);
	lg_step(n, "CSG start ack, csg ack word", a1, co[0]);

	volatile uint32_t *si = sl->csin, *so = sl->csout;
	*(volatile uint64_t *)&si[0x10 / 4] = LG_SLOT_KVA(n);
	si[0x18 / 4] = ring_size;
	*(volatile uint64_t *)&si[0x30 / 4] = LG_SLOT_MCU(n);
	*(volatile uint64_t *)&si[0x38 / 4] = LG_SLOT_MCU(n) + 0x1000;
	si[0x04 / 4] = 1u | ((uint32_t)(n + 1) << 8);   /* priority 1, doorbell n + 1 */
	si[0x0c / 4] = ~0u;
	si[0] = (si[0] & ~(0x1500u | 7u)) | 0x1500u | 1u;
	lg_toggle(&ci[0x08 / 4], co[0x08 / 4], 1u);
	lg_toggle(&lg_gin[0x08 / 4], lg_gout[0x08 / 4], 1u << n);
	gw(DOORBELL(0), 1);
	uint32_t a2 = lg_acked(&si[0], &so[0], 7u, 10000);
	lg_step(n, "CS start ack, cs ack word", a2, so[0]);
	if (a1 == ~0u || a2 == ~0u) {
		printf("LAPIS GPU: group %d on VM %d did not start (csg %u, cs %u)\n", n, vm, a1, a2);
		lg_group_free_locked(n);
		r = -6;
		goto out;
	}
	sl->active = 1;
	r = n;
out:
	lck_mtx_unlock(&lg_mtx);
	return r;
}

void
lapis_gpu_group_destroy(int n)
{
	lck_mtx_lock(&lg_mtx);
	if (n >= 1 && n < 8 && lg_slots[n].active) {
		struct lg_slot *sl = &lg_slots[n];
		sl->csgin[0] = sl->csgin[0] & ~7u;      /* TERMINATE */
		lg_toggle(&lg_gin[0x08 / 4], lg_gout[0x08 / 4], 1u << n);
		gw(DOORBELL(0), 1);
		if (lg_acked(&sl->csgin[0], &sl->csgout[0], 7u, 10000) == ~0u) {
			/* the firmware may still use its pages: keep them, never reuse the slot */
			printf("LAPIS GPU: group %d did not terminate (csg ack %#x): slot retired\n", n, sl->csgout[0]);
			sl->active = 0;
			lg_groups[n].vm = -1;
		} else {
			lg_group_free_locked(n);
		}
	}
	lck_mtx_unlock(&lg_mtx);
}

/* the user's stream at stream (its VM's address), size bytes; the submission's number, 0 if refused */
uint32_t
lapis_gpu_group_submit(int n, uint64_t stream, uint32_t size)
{
	uint32_t seq = 0;
	lck_mtx_lock(&lg_mtx);
	if (n >= 1 && n < 8 && lg_slots[n].active && !lg_slots[n].fatal && !(size & 7) && stream < LG_KVA) {
		struct lg_group *gr = &lg_groups[n];
		uint64_t a = lg_kreg(), v = a + 2;
		uint64_t ins[8] = {
			(1ull << 56) | (a << 48) | (stream & 0xffffffffffffull),        /* MOVE48 a = stream */
			(2ull << 56) | (v << 48) | size,                                /* MOVE32 v = size */
			size ? (32ull << 56) | (a << 40) | (v << 32) : 0,               /* CALL a, v */
			(3ull << 56) | (0xffffull << 16),                               /* WAIT all */
			(1ull << 56) | (a << 48) | (LG_SLOT_KVA(n) + 0x80000),          /* MOVE48 a = sync */
			(2ull << 56) | (v << 48) | (gr->seq + 1),                       /* MOVE32 v = seq */
			(21ull << 56) | (v << 48) | (a << 40) | (1ull << 16),           /* STORE_MULTIPLE [a] = v */
			(3ull << 56) | (0xffffull << 16),                               /* WAIT all */
		};
		uint64_t insert = gr->uin[0];
		if (insert + 64 - gr->uout[0] <= gr->ring_size) {
			for (int i = 0; i < 8; i++) {
				gr->ring[((insert / 8) + i) % (gr->ring_size / 8)] = ins[i];
			}
			__asm__ volatile("dsb osh" ::: "memory");
			gr->uin[0] = insert + 64;
			__asm__ volatile("dsb osh" ::: "memory");
			gw(DOORBELL(n + 1), 1);
			seq = ++gr->seq;
		}
	}
	lck_mtx_unlock(&lg_mtx);
	return seq;
}

/* 0 when submission seq is done, -1 timeout, -2 the group faulted, -3 no group */
int
lapis_gpu_group_wait(int n, uint32_t seq, uint64_t timeout_us)
{
	if (n < 1 || n >= 8 || !lg_slots[n].active) {
		return -3;
	}
	uint64_t t0 = lg_now_us();
	for (;;) {
		if ((int32_t)(*lg_groups[n].sync - seq) >= 0) {
			return 0;
		}
		if (lg_slots[n].fatal) {
			return -2;
		}
		uint64_t t = lg_now_us() - t0;
		if (t > timeout_us) {
			return -1;
		}
		if (t > 200) {
			lck_mtx_lock(&lg_mtx);
			lg_service_locked();
			lck_mtx_unlock(&lg_mtx);
		}
		if (t > 2000) {
			lg_sleep_ms(1);
		} else {
			delay(5);
		}
		if (!lg_slots[n].active) {
			return -3;
		}
	}
}

int
lapis_gpu_group_fatal(int n)
{
	return n >= 1 && n < 8 && lg_slots[n].fatal;
}

uint32_t
lapis_gpu_group_seq(int n)
{
	return n >= 1 && n < 8 ? lg_groups[n].seq : 0;
}

static void
lg_selftest(void)
{
	/* 300 back-to-back: 19200 bytes of instructions, the ring wraps 4 times */
	uint32_t ok = 0, worst = 0, sum = 0, first_bad = ~0u;
	for (uint32_t i = 0; i < 300; i++) {
		uint32_t us = lg_test_store(0xa5000000u | i);
		if (us == ~0u) {
			if (first_bad == ~0u) {
				first_bad = i;
			}
			continue;
		}
		ok++;
		sum += us;
		worst = us > worst ? us : worst;
	}
	printf("LAPIS GPU: 300 streams: %u ran, avg %u us, worst %u us, first miss %d, insert %#llx extract %#llx\n",
	    ok, ok ? sum / ok : 0, worst, (int)first_bad, (unsigned long long)lg_uin[0],
	    (unsigned long long)lg_uout[0]);
}

static void
lg_thread(void *arg __unused, wait_result_t wr __unused)
{
	int mode = 1;
	(void)PE_parse_boot_argn("lapisgpu", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("lapisgputrace", &lg_trace, sizeof(lg_trace));
	if (!lg_run()) {
		lg_ring = NULL;
	} else {
		lg_selftest();
		extern void LapisGPUStart(void);        /* iokit/Kernel/arm/LapisGPU.cpp */
		LapisGPUStart();
		/* later checks: an idle GPU must still answer */
		static const uint32_t at_s[] = { 1, 10, 30, 60, 120 };
		uint64_t start = lg_now_us();
		for (unsigned k = 0; k < sizeof(at_s) / sizeof(at_s[0]); k++) {
			while (lg_now_us() - start < at_s[k] * 1000000ull) {
				lck_mtx_lock(&lg_mtx);
				lg_service_locked();
				lck_mtx_unlock(&lg_mtx);
				lg_sleep_ms(20);
			}
			uint32_t us = lg_test_store(0x5a000000u | k);
			uint32_t png = lg_ping();
			printf("LAPIS GPU: at %us: stream %s %u us, ping %u x10us, MCU %u, events %u faults %u\n",
			    at_s[k], us == ~0u ? "MISSED" : "ran", us == ~0u ? 0 : us, png,
			    g(MCU_STATUS), lg_nevent, lg_nfault);
			if (mode == 2) {
				break;
			}
		}
		if (mode == 2) {
			lck_mtx_lock(&lg_mtx);
			lg_ring = NULL;
			lg_stop();
			lck_mtx_unlock(&lg_mtx);
			printf("LAPIS GPU: stopped and powered off (lapisgpu=2)\n");
		} else {
			printf("LAPIS GPU: up, servicing events every 20 ms\n");
			for (;;) {
				lck_mtx_lock(&lg_mtx);
				lg_service_locked();
				lck_mtx_unlock(&lg_mtx);
				lg_sleep_ms(20);
			}
		}
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_lapis_gpu_init(void)
{
	int enabled = 1;
	static bool started;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("lapisgpu", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS GPU: disabled (lapisgpu=0)\n");
		return;
	}
	if (kernel_thread_start(lg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("LAPIS GPU: thread start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
