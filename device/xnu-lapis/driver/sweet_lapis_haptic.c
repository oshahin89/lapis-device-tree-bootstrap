/*
 * Lapis vibration motor: Awinic AW86927 on I2C2 (0x11c22000) at 0x5a.
 *
 * Proven from the boot shim (tools/lapis_shim/tvib.c v5, 2026-11-22):
 *  - on this bus a 16-bit store to DATA_PORT pushes two FIFO bytes (value,
 *    0x00), so every register write landed as 0; DATA_PORT takes 8-bit stores.
 *  - Android's idle register set (haptic_hv sysfs dump, below) cloned into the
 *    chip, then CONT play as the driver does it: PLAYCFG1 0x06 = 0x57 (boost
 *    bypass), PLAYCFG3 0x08 mode 2, CONTCFG6 0x1d = 0xff (track, drv1 0x7f),
 *    CONTCFG7 0x1e = 0x36, CONTCFG9 0x20 = 0xff, PLAYCFG4 0x09 = 1 (GO);
 *    stop = PLAYCFG4 0x09 = 2. GLBRD5 0x3f reads 6 while it plays.
 *
 * lapis_haptic_pulse(ms) starts a buzz and a thread call stops it. A boot buzz
 * shows the driver is alive; lapis keys call it for a short click per press.
 * On by default; lapishaptic=0 disables.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define HP_I2C2_PA          0x11c22000UL
#define HP_ADDR             0x5au

#define HP_DATA_PORT        0x00
#define HP_INTR_MASK        0x08
#define HP_INTR_STAT        0x0c
#define HP_CONTROL          0x10
#define HP_TRANSFER_LEN     0x14
#define HP_TRANSAC_LEN      0x18
#define HP_START            0x24
#define HP_EXT_CONF         0x28
#define HP_FIFO_ADDR_CLR    0x38
#define HP_TRANSFER_LEN_AUX 0x44
#define HP_SLAVE_ADDR       0x94
#define HP_FIFO_STAT        0xf4
#define HP_CTL_RS           (1u << 1)
#define HP_CTL_DIR_CHANGE   (1u << 4)
#define HP_INT_COMP         (1u << 0)
#define HP_INT_ACKERR       (1u << 1)
#define HP_INT_HS_NACKERR   (1u << 2)
#define HP_INT_RS           (1u << 4)
#define HP_INT_ALL          (HP_INT_RS | HP_INT_HS_NACKERR | HP_INT_ACKERR | HP_INT_COMP)
#define HP_FS_START_CON     0x1800

void lapis_haptic_pulse(unsigned ms);
void sweet_lapis_haptic_init(void);

/* AW86927 registers as Android's haptic_hv driver leaves them (idle),
 * device/captures/aw86927_android_regs_2026-11-22.txt */
static const uint8_t aw_android[][2] = {
	{ 0x06, 0xd7 },
	{ 0x07, 0x80 },
	{ 0x08, 0x00 },
	{ 0x0a, 0x08 },
	{ 0x0b, 0x00 },
	{ 0x0c, 0x00 },
	{ 0x0d, 0x00 },
	{ 0x0e, 0x00 },
	{ 0x0f, 0x00 },
	{ 0x10, 0x00 },
	{ 0x11, 0x00 },
	{ 0x12, 0x00 },
	{ 0x13, 0x00 },
	{ 0x14, 0x00 },
	{ 0x15, 0x00 },
	{ 0x16, 0x00 },
	{ 0x17, 0x00 },
	{ 0x18, 0x1e },
	{ 0x19, 0x8d },
	{ 0x1a, 0x6a },
	{ 0x1b, 0x06 },
	{ 0x1c, 0x58 },
	{ 0x1d, 0xff },
	{ 0x1e, 0x36 },
	{ 0x1f, 0x04 },
	{ 0x20, 0x06 },
	{ 0x21, 0x08 },
	{ 0x22, 0x0c },
	{ 0x23, 0x0c },
	{ 0x24, 0x62 },
	{ 0x25, 0x08 },
	{ 0x26, 0xa0 },
	{ 0x27, 0x08 },
	{ 0x28, 0x96 },
	{ 0x29, 0xe7 },
	{ 0x2a, 0x7f },
	{ 0x2b, 0x7f },
	{ 0x2c, 0x7f },
	{ 0x2d, 0x08 },
	{ 0x2e, 0x00 },
	{ 0x2f, 0x46 },
	{ 0x30, 0x00 },
	{ 0x31, 0x00 },
	{ 0x33, 0x81 },
	{ 0x34, 0x01 },
	{ 0x35, 0x01 },
	{ 0x36, 0x82 },
	{ 0x37, 0x02 },
	{ 0x38, 0x02 },
	{ 0x39, 0x44 },
	{ 0x3a, 0x40 },
	{ 0x3b, 0x05 },
	{ 0x3c, 0x08 },
	{ 0x3d, 0x00 },
	{ 0x3e, 0x1b },
	{ 0x43, 0x00 },
	{ 0x44, 0x00 },
	{ 0x45, 0x12 },
	{ 0x46, 0x09 },
	{ 0x47, 0x5a },
	{ 0x48, 0xa0 },
	{ 0x49, 0x28 },
	{ 0x4a, 0xbf },
	{ 0x4b, 0x32 },
	{ 0x4c, 0x00 },
	{ 0x4d, 0x24 },
	{ 0x4e, 0x04 },
};
/* written with TMCFG 0x5b = 0x7d (unlocked) */
static const uint8_t aw_android_trim[][2] = {
	{ 0x71, 0x93 },
	{ 0x72, 0x69 },
	{ 0x74, 0x89 },
	{ 0x75, 0x88 },
	{ 0x79, 0x09 },
};

static vm_offset_t hp_i2c;
static bool hp_ok;
static unsigned hp_errors;
static lck_grp_t *hp_grp;
static lck_mtx_t *hp_lock;
static thread_call_t hp_stop_call;

static uint32_t
hp_rd(uint32_t off)
{
	uint32_t v = *(volatile uint16_t *)(hp_i2c + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
hp_wr(uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint16_t *)(hp_i2c + off) = (uint16_t)value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
hp_wr8(uint8_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint8_t *)(hp_i2c + HP_DATA_PORT) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
hp_setup(bool wrrd)
{
	uint32_t c = hp_rd(HP_CONTROL) & ~(HP_CTL_DIR_CHANGE | HP_CTL_RS);
	hp_wr(HP_CONTROL, wrrd ? (c | HP_CTL_DIR_CHANGE | HP_CTL_RS) : c);
	hp_wr(HP_EXT_CONF, HP_FS_START_CON);
	hp_wr(HP_SLAVE_ADDR, HP_ADDR << 1);
	hp_wr(HP_INTR_STAT, HP_INT_ALL);
	hp_wr(HP_FIFO_ADDR_CLR, 1);
	hp_wr(HP_INTR_MASK, HP_INT_ALL);
}

static bool
hp_go(void)
{
	uint32_t st = 0;
	hp_wr(HP_START, 1);
	for (unsigned i = 0; i < 20000; i++) {   /* ~20 ms */
		st = hp_rd(HP_INTR_STAT);
		if (st & (HP_INT_COMP | HP_INT_ACKERR | HP_INT_HS_NACKERR)) {
			break;
		}
		delay(1);
	}
	hp_wr(HP_INTR_STAT, HP_INT_ALL);
	if (!(st & HP_INT_COMP) || (st & (HP_INT_ACKERR | HP_INT_HS_NACKERR))) {
		hp_errors++;
		return false;
	}
	return true;
}

static int
hp_read(uint8_t reg)
{
	hp_setup(true);
	hp_wr(HP_TRANSFER_LEN, 1);
	hp_wr(HP_TRANSFER_LEN_AUX, 1);
	hp_wr(HP_TRANSAC_LEN, 2);
	hp_wr8(reg);
	if (!hp_go()) {
		return -1;
	}
	uint32_t count = hp_rd(HP_FIFO_STAT) & 0xfu;
	if (count == 2) {
		(void)hp_rd(HP_DATA_PORT);
	} else if (count != 1) {
		hp_errors++;
		return -1;
	}
	return (int)(hp_rd(HP_DATA_PORT) & 0xffu);
}

static bool
hp_write(uint8_t reg, uint8_t value)
{
	hp_setup(false);
	hp_wr(HP_TRANSFER_LEN_AUX, 0);
	hp_wr(HP_TRANSFER_LEN, 2);
	hp_wr(HP_TRANSAC_LEN, 1);
	hp_wr8(reg);
	hp_wr8(value);
	return hp_go();
}

static void
hp_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	int v = hp_read(reg);
	if (v >= 0) {
		(void)hp_write(reg, (uint8_t)((v & ~mask) | (value & mask)));
	}
}

static void
hp_stop_locked(void)
{
	(void)hp_write(0x09, 0x02);                     /* PLAYCFG4 STOP */
}

static void
hp_stop_fn(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	lck_mtx_lock(hp_lock);
	hp_stop_locked();
	lck_mtx_unlock(hp_lock);
}

void
lapis_haptic_pulse(unsigned ms)
{
	uint64_t deadline;
	if (!hp_ok || ms == 0) {
		return;
	}
	lck_mtx_lock(hp_lock);
	hp_stop_locked();
	hp_bits(0x06, 0x80, 0x00);                      /* boost bypass */
	hp_bits(0x08, 0x03, 0x02);                      /* CONT */
	(void)hp_write(0x1d, 0xff);                     /* track on, drv1 0x7f */
	(void)hp_write(0x1e, 0x36);                     /* drv2 */
	(void)hp_write(0x20, 0xff);                     /* drv2 time: until stopped */
	(void)hp_write(0x09, 0x01);                     /* GO */
	lck_mtx_unlock(hp_lock);
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)thread_call_enter_delayed(hp_stop_call, deadline);
}

void
sweet_lapis_haptic_init(void)
{
	int enabled = 1;
	static bool started;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("lapishaptic", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS HAPTIC: disabled (lapishaptic=0)\n");
		return;
	}
	hp_i2c = ml_io_map(HP_I2C2_PA, 0x1000);
	hp_grp = lck_grp_alloc_init("lapis_haptic", LCK_GRP_ATTR_NULL);
	hp_lock = lck_mtx_alloc_init(hp_grp, LCK_ATTR_NULL);
	hp_stop_call = thread_call_allocate(hp_stop_fn, NULL);
	if (!hp_i2c || !hp_lock || !hp_stop_call) {
		printf("LAPIS HAPTIC: setup failed\n");
		return;
	}
	int hi = hp_read(0x57), lo = hp_read(0x58);
	if (hi != 0x92 || lo != 0x70) {
		printf("LAPIS HAPTIC: no AW86927 (id %d/%d, errors %u)\n", hi, lo, hp_errors);
		return;
	}
	unsigned bad = 0;
	for (unsigned i = 0; i < sizeof(aw_android) / sizeof(aw_android[0]); i++) {
		(void)hp_write(aw_android[i][0], aw_android[i][1]);
	}
	(void)hp_write(0x5b, 0x7d);                     /* TMCFG unlock */
	for (unsigned i = 0; i < sizeof(aw_android_trim) / sizeof(aw_android_trim[0]); i++) {
		(void)hp_write(aw_android_trim[i][0], aw_android_trim[i][1]);
	}
	(void)hp_write(0x5b, 0x00);                     /* TMCFG lock */
	for (unsigned i = 0; i < sizeof(aw_android) / sizeof(aw_android[0]); i++) {
		if (hp_read(aw_android[i][0]) != aw_android[i][1]) {
			bad++;
		}
	}
	hp_ok = true;
	printf("LAPIS HAPTIC: AW86927 ready, %u read-only/differing regs, i2c errors %u\n",
	    bad, hp_errors);
	lapis_haptic_pulse(80);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
