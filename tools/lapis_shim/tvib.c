/*
 * lapis vibration probe (shim alternate main, build.sh tvib).
 *
 * Runtime FDT: AW86927 haptic driver (awinic,awinic_haptic) on I2C2
 * (0x11c22000, mediatek,mt6989-i2c, 400 kHz) at 0x5a; reset-gpio = GPIO 116
 * (active high: Android's hw_reset drives it 0, 1 ms, 1, 8 ms), irq GPIO 10;
 * aw86927_vib_cont_drv1_lvl 0x7f, drv2_lvl 0x36, f0_pre 1700 (170.0 Hz).
 * Registers from Awinic's GPL haptic_hv driver (aw8692x.c / haptic_hv_reg.h):
 * CHIPID 0x57/0x58 = 0x9270, PLAYCFG1 0x06 b7 BST (0 bypass), PLAYCFG3 0x08
 * [1:0] mode (2 CONT), PLAYCFG4 0x09 b0 GO / b1 STOP, CONTCFG6 0x1d b7 track
 * [6:0] drv1 level, CONTCFG7 0x1e drv2 level, CONTCFG9 0x20 drv2 time,
 * GLBRD5 0x3f [3:0] state (0 standby, 6 CONT), VBATCTRL 0x4c b6 HW comp,
 * ANACFG20 0x79 (GO only if <= 0x3f).
 *
 * 1. shows GPIO 116 and I2C2; 2. reads the chip ID, and if it does not
 * answer, raises GPIO 116 (output high) like hw_reset and reads again;
 * 3. v2: Xiaomi's aw86927.c (MiCode garnet-t-oss) misc_para_init, then three
 *    CONT plays as aw86927_haptic_cont_play (track on, on, off), GLBRD5
 *    sampled every 10 ms; SYSST 0x01, SYSINT 0x02, SYSST2 0x04, SYSER 0x05 shown.
 * v3: Android's idle registers cloned instead (2nd play with boost).
 *    v2 on the phone: GO ignored (state 0, no DONE).
 *    v1 on the phone: ID 0x9270 at once, 0 I2C errors, state 0 after GO, no buzz. Writes: GPIO 116 and the AW86927 only.
 */
#include <stdint.h>
#include "console.h"
#include "aw86927_android.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPIO		0x10005000UL
#define G116_MODE	0x3e0		/* pins 112..119: 116 = bits 16..19 */
#define G116_DIR	0x030		/* pins 96..127: 116 = bit 20 */
#define G116_DO		0x130
#define G116_DI		0x230
#define G116_BIT	(1u << 20)

#define I2C		0x11c22000UL
#define DATA_PORT	0x00
#define INTR_MASK	0x08
#define INTR_STAT	0x0c
#define CONTROL		0x10
#define TRANSFER_LEN	0x14
#define TRANSAC_LEN	0x18
#define TIMING		0x20
#define START		0x24
#define EXT_CONF	0x28
#define FIFO_ADDR_CLR	0x38
#define TRANSFER_LEN_AUX 0x44
#define SLAVE_ADDR	0x94
#define FIFO_STAT	0xf4
#define CTL_RS		(1u << 1)
#define CTL_DIR_CHANGE	(1u << 4)
#define INT_COMP	(1u << 0)
#define INT_ACKERR	(1u << 1)
#define INT_HS_NACKERR	(1u << 2)
#define INT_RS		(1u << 4)
#define INT_ALL		(INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP)
#define FS_START_CON	0x1800
#define AW		0x5a

static inline uint32_t mrd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void mwr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t rd(uint32_t off) { return *(volatile uint16_t *)(I2C + off); }
static inline void wr(uint32_t off, uint32_t v)
{
	*(volatile uint16_t *)(I2C + off) = (uint16_t)v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static uint32_t n_err;

static void setup(int wrrd)
{
	uint32_t c = rd(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS);
	wr(CONTROL, wrrd ? (c | CTL_DIR_CHANGE | CTL_RS) : c);
	wr(EXT_CONF, FS_START_CON);
	wr(SLAVE_ADDR, AW << 1);
	wr(INTR_STAT, INT_ALL);
	wr(FIFO_ADDR_CLR, 1);
	wr(INTR_MASK, INT_ALL);
}

static int go_wait(void)
{
	uint32_t st = 0;
	wr(START, 1);
	for (int i = 0; i < 20000; i++) {
		st = rd(INTR_STAT);
		if (st & (INT_COMP | INT_ACKERR | INT_HS_NACKERR))
			break;
		udelay(1);
	}
	wr(INTR_STAT, INT_ALL);
	if (!(st & INT_COMP) || (st & (INT_ACKERR | INT_HS_NACKERR))) {
		n_err++;
		return 0;
	}
	return 1;
}

static int aw_rd(uint8_t reg)
{
	setup(1);
	wr(TRANSFER_LEN, 1);
	wr(TRANSFER_LEN_AUX, 1);
	wr(TRANSAC_LEN, 2);
	wr(DATA_PORT, reg);
	if (!go_wait())
		return -1;
	uint32_t count = rd(FIFO_STAT) & 0xf;
	if (count == 2)
		(void)rd(DATA_PORT);
	else if (count != 1) {
		n_err++;
		return -1;
	}
	return (int)(rd(DATA_PORT) & 0xff);
}

static int aw_wr(uint8_t reg, uint8_t v)
{
	setup(0);
	wr(TRANSFER_LEN, 2);
	wr(TRANSAC_LEN, 1);
	wr(DATA_PORT, reg);
	wr(DATA_PORT, v);
	return go_wait();
}

static void aw_bits(uint8_t reg, uint8_t mask, uint8_t val)
{
	int v = aw_rd(reg);
	if (v >= 0)
		aw_wr(reg, (uint8_t)((v & ~mask) | (val & mask)));
}

static uint32_t chip_id(void)
{
	int h = aw_rd(0x57), l = aw_rd(0x58);
	return (h < 0 || l < 0) ? 0xffffffffu : ((uint32_t)h << 8 | (uint32_t)l);
}

static int st_max, st_seen;
static uint32_t st_mask;

/* GLBRD5 every 10 ms for ms: the highest state, a bitmask of states seen */
static void watch(uint32_t ms)
{
	st_max = -1;
	st_mask = 0;
	st_seen = 0;
	for (uint32_t t = 0; t < ms; t += 10) {
		int v = aw_rd(0x3f);
		if (v >= 0) {
			st_seen++;
			st_mask |= 1u << (v & 0xf);
			if ((v & 0xf) > st_max)
				st_max = v & 0xf;
		}
		udelay(10000);
	}
}

static void stop_standby(void)
{
	aw_wr(0x09, 0x02);				/* PLAYCFG4 STOP */
	for (int i = 0; i < 40; i++) {
		int v = aw_rd(0x3f);
		if (v >= 0 && (v & 0xf) == 0)
			return;
		udelay(2000);
	}
	aw_bits(0x45, 0x20, 0x20);			/* SYSCTRL3 force standby */
	aw_bits(0x45, 0x20, 0x00);
}

static void show_status(const char *label)
{
	uint32_t a = (uint32_t)aw_rd(0x01) << 24 | (uint32_t)aw_rd(0x04) << 16 |
		     (uint32_t)aw_rd(0x05) << 8 | (uint32_t)(aw_rd(0x02) & 0xff);
	con_hex2(label, "sysst.st2.ser.int=", a, "glbrd5=", (uint32_t)aw_rd(0x3f));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tvib v3: Android's AW86927 config + CONT");
	uint32_t id = chip_id();
	if (id != 0x9270) {
		/* reset pin -> GPIO output high (Android hw_reset: 0, 1 ms, 1, 8 ms) */
		mwr(GPIO + G116_MODE, mrd(GPIO + G116_MODE) & ~(0xfu << 16));
		mwr(GPIO + G116_DO, mrd(GPIO + G116_DO) & ~G116_BIT);
		mwr(GPIO + G116_DIR, mrd(GPIO + G116_DIR) | G116_BIT);
		udelay(2000);
		mwr(GPIO + G116_DO, mrd(GPIO + G116_DO) | G116_BIT);
		udelay(10000);
		id = chip_id();
	}
	con_hex2("CHIP", "id=", id, "errors=", n_err);
	if (id != 0x9270) {
		con_str("FAIL", "no AW86927 answering at 0x5a");
		goto out;
	}
	show_status("BEFORE");
	con_hex2("PLAYCFG", "1.2.3.4=", (uint32_t)aw_rd(0x06) << 24 | (uint32_t)aw_rd(0x07) << 16 |
		 (uint32_t)aw_rd(0x08) << 8 | (uint32_t)aw_rd(0x09), "efcfg6=", (uint32_t)aw_rd(0x61));
	con_hex2("SYSCTRL", "3.4.5=", (uint32_t)aw_rd(0x45) << 16 | (uint32_t)aw_rd(0x46) << 8 |
		 (uint32_t)aw_rd(0x47), "pwmcfg1=", (uint32_t)aw_rd(0x48));
	con_hex2("CONTCFG", "1.6.7.9=", (uint32_t)aw_rd(0x18) << 24 | (uint32_t)aw_rd(0x1d) << 16 |
		 (uint32_t)aw_rd(0x1e) << 8 | (uint32_t)aw_rd(0x20), "f0 lra=", (uint32_t)aw_rd(0x25) << 8 | (uint32_t)aw_rd(0x26));

	/* v3: clone Android's whole idle configuration, then read it back */
	stop_standby();
	uint32_t bad = 0, first_bad = 0;
	for (unsigned i = 0; i < sizeof(aw_android) / sizeof(aw_android[0]); i++)
		aw_wr(aw_android[i][0], aw_android[i][1]);
	aw_wr(0x5b, 0x7d);				/* TMCFG unlock */
	for (unsigned i = 0; i < sizeof(aw_android_trim) / sizeof(aw_android_trim[0]); i++)
		aw_wr(aw_android_trim[i][0], aw_android_trim[i][1]);
	aw_wr(0x5b, 0x00);				/* TMCFG lock */
	for (unsigned i = 0; i < sizeof(aw_android) / sizeof(aw_android[0]); i++) {
		int v = aw_rd(aw_android[i][0]);
		if (v != aw_android[i][1]) {
			if (!bad)
				first_bad = (uint32_t)aw_android[i][0] << 16 | (uint32_t)(v & 0xffff);
			bad++;
		}
	}
	con_hex2("CLONE", "mismatches=", bad, "first reg.val=", first_bad);
	con_hex2("NOW", "pwmcfg2=", (uint32_t)aw_rd(0x49), "playcfg1=", (uint32_t)aw_rd(0x06));
	show_status("INIT");
	con_skip();

	for (int n = 1; n <= 3; n++) {
		udelay(700000);
		/* aw86927_haptic_cont_play */
		stop_standby();
		aw_bits(0x08, 0x03, 0x02);		/* PLAYCFG3 CONT */
		aw_bits(0x06, 0x80, n == 2 ? 0x80 : 0x00);	/* bypass (2nd: boost) */
		aw_bits(0x1d, 0x80, 0x80);		/* track on */
		aw_bits(0x1d, 0x7f, 0x7f);		/* drv1 level */
		aw_wr(0x1e, 0x36);			/* drv2 level */
		aw_wr(0x20, 0xff);			/* drv2 time */
		(void)aw_rd(0x02);			/* clear SYSINT */
		aw_wr(0x09, 0x01);			/* GO */
		watch(300);
		show_status("PLAYED");
		con_hex2("STATES", "max=", (uint32_t)st_max, "seen mask=", st_mask);
		stop_standby();
	}
	con_hex2("DONE", "errors=", n_err, 0, 0);
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
