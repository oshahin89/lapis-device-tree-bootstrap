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
 * Then the GPU is stopped and powered off again (nothing uses it yet).
 * Register layout and sequence after Linux panthor (GPL, read for reference,
 * not copied) and Android's kbase (kprobed on this phone). lapisgpu=0 skips.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

void sweet_lapis_gpu_init(void);

#define LG_GPU_PA       0x13000000UL
#define LG_GPU_SIZE     0xa0000UL       /* up to doorbell page 1 */
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
#define JOB_INT_RAWSTAT 0x1000
#define JOB_INT_CLEAR   0x1004
#define JOB_INT_MASK    0x1008
#define AS(n)           (0x2400u + (n) * 0x40u)
#define AS_TRANSTAB     0x00
#define AS_MEMATTR      0x08
#define AS_COMMAND      0x18
#define AS_FAULTSTATUS  0x1c
#define AS_STATUS       0x28
#define AS_TRANSCFG     0x30
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

static uint64_t
lg_alloc(uint32_t pages)
{
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

static int
lg_map(uint64_t va, uint64_t pa, uint64_t attrs)
{
	uint64_t table = lg_root;
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
				if (!pa || de > size) {
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
lg_as_setup(unsigned n, uint64_t transcfg)
{
	gw(AS(n) + AS_TRANSTAB, (uint32_t)lg_root);
	gw(AS(n) + AS_TRANSTAB + 4, (uint32_t)(lg_root >> 32));
	gw(AS(n) + AS_MEMATTR, 0x4c4c4c4c);
	gw(AS(n) + AS_MEMATTR + 4, 0x4c4c4c4c);
	gw(AS(n) + AS_TRANSCFG, (uint32_t)transcfg);
	gw(AS(n) + AS_TRANSCFG + 4, (uint32_t)(transcfg >> 32));
	(void)lg_poll(AS(n) + AS_STATUS, 1, 0, 10000);
	gw(AS(n) + AS_COMMAND, 1);              /* UPDATE */
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

static void
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
		return;
	}
	uint32_t pwr = lg_rd(lg_scp, 0xeb4);
	if ((pwr & 0xc0000000u) != 0xc0000000u) {
		printf("LAPIS GPU: mfg0 domain off (%#x), skipped\n", pwr);
		return;
	}
	uint32_t ipi = lg_gpueb_power(1);
	uint32_t id = g(GPU_ID), mmuf = g(GPU_MMU_FEATURES), va_bits = mmuf & 0xff;
	printf("LAPIS GPU: GPUEB power-on %u x100us, GPU_ID %#x, MMU_FEATURES %#x\n", ipi, id, mmuf);
	if (!id) {
		return;
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
		return;
	}
	uint64_t transcfg = 6ull | ((uint64_t)(55 - va_bits) << 6) | (1ull << 24);
	lg_as_setup(0, transcfg);
	gw(JOB_INT_CLEAR, 0xffffffff);
	gw(JOB_INT_MASK, 0xffffffff);
	gw(MCU_CONTROL, 2);                     /* AUTO */
	uint32_t boot = lg_poll(JOB_INT_RAWSTAT, 1u << 31, 1u << 31, 100000);
	volatile uint32_t *glb = lg_sh(VA_SHARED);
	printf("LAPIS GPU: MCU boot %u x10us, status %u, interface %#x, %u groups\n",
	    boot, g(MCU_STATUS), glb[0], glb[4]);
	if (!glb[0]) {
		lg_stop();
		return;
	}

	/* GLB setup + PING */
	volatile uint32_t *gin = lg_sh(glb[2]), *gout = lg_sh(glb[3]);
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
	uint32_t susp = csgc[0x0c / 4], psusp = csgc[0x10 / 4];
	uint64_t ring = lg_map_new(VA_RING, 1), uin = lg_map_new(VA_UIO_IN, 1);
	uint64_t uout = lg_map_new(VA_UIO_OUT, 1), target = lg_map_new(VA_TARGET, 1);
	uint64_t sbuf = lg_map_new(VA_SUSPEND, (susp + 0xfff) >> 12);
	uint64_t pbuf = psusp ? lg_map_new(VA_PSUSPEND, (psusp + 0xfff) >> 12) : 1;
	if (!ring || !uin || !uout || !target || !sbuf || !pbuf) {
		printf("LAPIS GPU: pool exhausted\n");
		lg_stop();
		return;
	}
	lg_as_setup(1, transcfg);
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

	volatile uint64_t *ins = lg_pool_va(ring);
	ins[0] = (1ull << 56) | VA_TARGET;                      /* MOVE48 r0 */
	ins[1] = (2ull << 56) | (2ull << 48) | 0xc0ffee42ull;   /* MOVE32 r2 */
	ins[2] = (21ull << 56) | (2ull << 48) | (1ull << 16);   /* STORE_MULTIPLE [r0] = r2 */
	ins[3] = (3ull << 56) | (0xffffull << 16);              /* WAIT all */
	volatile uint32_t *tgt = lg_pool_va(target);
	*tgt = 0x11111111;
	volatile uint64_t *uio = lg_pool_va(uin);
	uio[1] = 0;
	__asm__ volatile("dsb osh" ::: "memory");
	uio[0] = 32;
	__asm__ volatile("dsb osh" ::: "memory");
	gw(DOORBELL(1), 1);
	uint32_t t;
	for (t = 0; t < 10000; t++) {
		if (*tgt == 0xc0ffee42u) {
			break;
		}
		delay(10);
	}
	printf("LAPIS GPU: GLB config %u ping %u, CSG start %u, CS start %u (x10us)\n", c1, c2, c3, c4);
	printf("LAPIS GPU: command stream %s: target %#x after %u x10us, AS1 fault %#x\n",
	    *tgt == 0xc0ffee42u ? "RAN" : "did not run", *tgt, t, g(AS(1) + AS_FAULTSTATUS));
	lg_stop();
	printf("LAPIS GPU: stopped and powered off\n");
}

static void
lg_thread(void *arg __unused, wait_result_t wr __unused)
{
	lg_run();
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
	(void)lg_sleep_chan;
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
