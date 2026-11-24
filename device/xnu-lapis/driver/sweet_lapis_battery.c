/*
 * Lapis battery for SweetBattery (iokit/Kernel/arm/SweetBattery.cpp, lapis
 * patch 0088). Measured from the shim (device/lapis_battery_bringup.md):
 *
 *   gauge    TI bq28z610, I2C0 (0x11c20000) address 0x55, read-only
 *   charger  MT6375 bank 1, I2C5 (0x11280000) address 0x34 (vendor id
 *            0x100 = 0x72). LK leaves it charging at 500 mA input / 500 mA
 *            charge (AICR 0x122 = 0x12, CC 0x126 = 0x0a), watchdog off.
 *
 * Both controllers are mediatek,mt6989-i2c (stock i2c-mt65xx
 * mt_i2c_regs_v2 offsets), left configured by LK. Reads are one WRRD
 * transaction; the FIFO may hold one extra leading byte (count n + 1),
 * which is skipped. Writes are one WR transaction of [reg, value].
 *
 * Charger policy: "external" = MT6375 ST_PWR_RDY (0x1e0 bit 0). While a
 * charger is present and the battery is 5..45 C, input limit (IAICR 0x122
 * [6:0], 100 mA + (sel - 2) * 25) and charge current (CC 0x126 [5:0],
 * 300 mA + (sel - 6) * 50) are raised to 1500 mA, only those bits changed,
 * every write read back; any mismatch stops all charger writes.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define LB_I2C0_PA          0x11c20000u     /* gauge */
#define LB_I2C5_PA          0x11280000u     /* MT6375 */
#define LB_DATA_PORT        0x00u
#define LB_INTR_MASK        0x08u
#define LB_INTR_STAT        0x0cu
#define LB_CONTROL          0x10u
#define LB_TRANSFER_LEN     0x14u
#define LB_TRANSAC_LEN      0x18u
#define LB_START            0x24u
#define LB_EXT_CONF         0x28u
#define LB_FIFO_ADDR_CLR    0x38u
#define LB_TRANSFER_LEN_AUX 0x44u
#define LB_SLAVE_ADDR       0x94u
#define LB_FIFO_STAT        0xf4u
#define LB_CTL_RS           (1u << 1)
#define LB_CTL_DIR_CHANGE   (1u << 4)
#define LB_INT_COMP         (1u << 0)
#define LB_INT_ACKERR       (1u << 1)
#define LB_INT_HS_NACKERR   (1u << 2)
#define LB_INT_RS           (1u << 4)
#define LB_INT_ALL          (LB_INT_RS | LB_INT_HS_NACKERR | LB_INT_ACKERR | LB_INT_COMP)
#define LB_FS_START_CON     0x1800u

#define LB_GAUGE            0x55u
#define BQ_TEMPERATURE      0x06u   /* 0.1 K */
#define BQ_VOLTAGE          0x08u   /* mV */
#define BQ_BATTERY_STATUS   0x0au
#define BQ_CURRENT          0x0cu   /* mA, signed, + = charging */
#define BQ_REMAINING        0x10u   /* mAh */
#define BQ_FULL_CHARGE      0x12u   /* mAh */
#define BQ_CYCLE_COUNT      0x2au
#define BQ_SOC              0x2cu   /* % */

#define LB_CHG              0x34u   /* MT6375 bank 1 */
#define CHG_VID             0x00u   /* 0x100 */
#define CHG_IAICR           0x22u   /* 0x122 [6:0] */
#define CHG_CC              0x26u   /* 0x126 [5:0] */
#define CHG_PWR_RDY         0xe0u   /* 0x1e0 bit 0 */
#define CHG_TARGET_MA       1500u
#define CHG_AICR_SEL(ma)    (((ma) - 100u) / 25u + 2u)
#define CHG_CC_SEL(ma)      (((ma) - 300u) / 50u + 6u)

struct lapis_battery_sample {
	unsigned percent, mv, remaining_mah, full_mah, cycles, status;
	int ma, temp_c100;
	int vbus;       /* MT6375 power-good: 1 / 0, -1 unknown */
};

int lapis_battery_init(void);
int lapis_battery_read(struct lapis_battery_sample *s);

static vm_offset_t lb_i2c0, lb_i2c5;
static unsigned lb_errors;
static bool lb_chg_ok, lb_chg_writes_off;

static uint32_t
lb_rd(vm_offset_t bus, uint32_t off)
{
	uint32_t v = *(volatile uint16_t *)(bus + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
lb_wr(vm_offset_t bus, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint16_t *)(bus + off) = (uint16_t)value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
lb_xfer_wait(vm_offset_t bus)
{
	uint32_t st = 0;
	lb_wr(bus, LB_START, 1);
	for (unsigned i = 0; i < 20000; i++) {   /* ~20 ms */
		st = lb_rd(bus, LB_INTR_STAT);
		if (st & (LB_INT_COMP | LB_INT_ACKERR | LB_INT_HS_NACKERR)) {
			break;
		}
		delay(1);
	}
	lb_wr(bus, LB_INTR_STAT, LB_INT_ALL);
	return (st & LB_INT_COMP) && !(st & (LB_INT_ACKERR | LB_INT_HS_NACKERR));
}

static void
lb_setup(vm_offset_t bus, uint8_t addr7, bool wrrd)
{
	uint32_t c = lb_rd(bus, LB_CONTROL) & ~(LB_CTL_DIR_CHANGE | LB_CTL_RS);
	lb_wr(bus, LB_CONTROL, wrrd ? (c | LB_CTL_DIR_CHANGE | LB_CTL_RS) : c);
	lb_wr(bus, LB_EXT_CONF, LB_FS_START_CON);
	lb_wr(bus, LB_SLAVE_ADDR, (uint32_t)addr7 << 1);
	lb_wr(bus, LB_INTR_STAT, LB_INT_ALL);
	lb_wr(bus, LB_FIFO_ADDR_CLR, 1);
	lb_wr(bus, LB_INTR_MASK, LB_INT_ALL);
}

/* Write reg, repeated start, read n (1..2) bytes, little endian. */
static bool
lb_read_once(vm_offset_t bus, uint8_t addr7, uint8_t reg, unsigned n, uint16_t *out)
{
	uint32_t count, v = 0;
	lb_setup(bus, addr7, true);
	lb_wr(bus, LB_TRANSFER_LEN, 1);
	lb_wr(bus, LB_TRANSFER_LEN_AUX, n);
	lb_wr(bus, LB_TRANSAC_LEN, 2);
	lb_wr(bus, LB_DATA_PORT, reg);
	if (!lb_xfer_wait(bus)) {
		return false;
	}
	count = lb_rd(bus, LB_FIFO_STAT) & 0xfu;
	if (count == n + 1u) {
		(void)lb_rd(bus, LB_DATA_PORT);         /* the extra leading byte */
	} else if (count != n) {
		return false;
	}
	for (unsigned i = 0; i < n; i++) {
		v |= (lb_rd(bus, LB_DATA_PORT) & 0xffu) << (8 * i);
	}
	*out = (uint16_t)v;
	return true;
}

static bool
lb_read(vm_offset_t bus, uint8_t addr7, uint8_t reg, unsigned n, uint16_t *out)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (lb_read_once(bus, addr7, reg, n, out)) {
			return true;
		}
		lb_errors++;
		delay(1000);
	}
	return false;
}

static bool
lb_write_byte(vm_offset_t bus, uint8_t addr7, uint8_t reg, uint8_t value)
{
	lb_setup(bus, addr7, false);
	lb_wr(bus, LB_TRANSFER_LEN, 2);
	lb_wr(bus, LB_TRANSAC_LEN, 1);
	lb_wr(bus, LB_DATA_PORT, reg);
	lb_wr(bus, LB_DATA_PORT, value);
	return lb_xfer_wait(bus);
}

#define GAUGE(cmd, out)     lb_read(lb_i2c0, LB_GAUGE, (cmd), 2, (out))
#define CHG_RD(reg, out)    lb_read(lb_i2c5, LB_CHG, (reg), 1, (out))

/* Set the field (mask) of a charger register to sel; read back. */
static bool
lb_chg_field(uint8_t reg, uint8_t mask, uint8_t sel, const char *name)
{
	uint16_t before = 0, after = 0;
	if (lb_chg_writes_off || !CHG_RD(reg, &before)) {
		return false;
	}
	uint8_t want = (uint8_t)((before & ~mask) | (sel & mask));
	if ((uint8_t)before == want) {
		return true;
	}
	if (!lb_write_byte(lb_i2c5, LB_CHG, reg, want) || !CHG_RD(reg, &after) ||
	    (uint8_t)after != want) {
		lb_chg_writes_off = true;
		printf("LAPIS CHARGER: %s 0x1%02x %02x -> %02x read back %02x; writes stopped\n",
		    name, reg, before & 0xff, want, after & 0xff);
		return false;
	}
	printf("LAPIS CHARGER: %s 0x1%02x %02x -> %02x\n", name, reg, before & 0xff, want);
	return true;
}

/* Called every poll with the charger state and the battery temperature. */
static void
lb_charger_policy(int vbus, int temp_c100)
{
	bool cool = temp_c100 >= 500 && temp_c100 <= 4500;
	if (!lb_chg_ok || vbus != 1) {
		return;
	}
	/* Re-check on each plug-in and every poll while plugged (the chip may
	 * fall back to defaults on re-detection); a no-op when already set. */
	unsigned ma = cool ? CHG_TARGET_MA : 500u;
	(void)lb_chg_field(CHG_IAICR, 0x7f, (uint8_t)CHG_AICR_SEL(ma), "input limit");
	(void)lb_chg_field(CHG_CC, 0x3f, (uint8_t)CHG_CC_SEL(ma), "charge current");
}

int
lapis_battery_init(void)
{
	uint16_t soc = 0, vid = 0;
	int enable = 1;
	if (lb_i2c0 == 0) {
		lb_i2c0 = ml_io_map(LB_I2C0_PA, 0x1000);
	}
	if (lb_i2c5 == 0) {
		lb_i2c5 = ml_io_map(LB_I2C5_PA, 0x1000);
	}
	if (lb_i2c0 == 0 || !GAUGE(BQ_SOC, &soc) || soc > 100) {
		printf("LAPIS BATTERY: gauge not answering (soc=%u errors=%u)\n", soc, lb_errors);
		return -1;
	}
	(void)PE_parse_boot_argn("lapischarger", &enable, sizeof(enable));
	lb_chg_ok = enable && lb_i2c5 != 0 && CHG_RD(CHG_VID, &vid) && (vid & 0xf0) == 0x70;
	lb_chg_writes_off = enable == 2;     /* lapischarger=2: read-only */
	printf("LAPIS BATTERY: bq28z610 on I2C0, %u%%; MT6375 vid=%02x charger=%s\n",
	    soc, vid & 0xff, !lb_chg_ok ? "off" : lb_chg_writes_off ? "read-only" : "managed");
	return 0;
}

int
lapis_battery_read(struct lapis_battery_sample *s)
{
	uint16_t soc, mv, status, cur, temp, rem, full, cycles, prdy = 0;
	if (lb_i2c0 == 0 ||
	    !GAUGE(BQ_SOC, &soc) || !GAUGE(BQ_VOLTAGE, &mv) ||
	    !GAUGE(BQ_BATTERY_STATUS, &status) || !GAUGE(BQ_CURRENT, &cur) ||
	    !GAUGE(BQ_TEMPERATURE, &temp) || !GAUGE(BQ_REMAINING, &rem) ||
	    !GAUGE(BQ_FULL_CHARGE, &full) || !GAUGE(BQ_CYCLE_COUNT, &cycles)) {
		return -1;
	}
	/* Implausible data never becomes a reading (and never 0 %). */
	if (soc > 100 || mv < 2800 || mv > 4700 || temp < 2331 || temp > 3531) {
		return -1;
	}
	s->percent = soc;
	s->mv = mv;
	s->status = status;
	s->ma = (int)(int16_t)cur;
	s->temp_c100 = ((int)temp - 2731) * 10;     /* 0.1 K -> 0.01 C */
	s->remaining_mah = rem;
	s->full_mah = full;
	s->cycles = cycles;
	s->vbus = (lb_chg_ok && CHG_RD(CHG_PWR_RDY, &prdy)) ? (int)(prdy & 1u) : -1;
	lb_charger_policy(s->vbus, s->temp_c100);
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
