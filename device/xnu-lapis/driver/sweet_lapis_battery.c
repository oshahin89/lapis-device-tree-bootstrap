/*
 * Lapis battery gauge for SweetBattery (iokit/Kernel/arm/SweetBattery.cpp,
 * lapis patch 0088): TI bq28z610 at 0x55 on I2C0 (0x11c20000,
 * mediatek,mt6989-i2c), measured from the shim (device/lapis_battery_bringup.md).
 *
 * Controller: stock i2c-mt65xx mt_i2c_regs_v2 offsets; LK leaves it set up
 * (CONTROL 0x38, TIMING 0x20). One WRRD transaction per gauge word: write
 * the command byte, repeated start, read 2 bytes. The FIFO then holds one
 * extra leading byte (FIFO_STAT low nibble = n + 1, the byte is 0), skipped
 * here; the data is little endian. Read-only towards the gauge.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define LB_I2C_PA           0x11c20000u
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

/* bq28z610 standard commands */
#define BQ_TEMPERATURE      0x06u   /* 0.1 K */
#define BQ_VOLTAGE          0x08u   /* mV */
#define BQ_BATTERY_STATUS   0x0au
#define BQ_CURRENT          0x0cu   /* mA, signed, + = charging */
#define BQ_REMAINING        0x10u   /* mAh */
#define BQ_FULL_CHARGE      0x12u   /* mAh */
#define BQ_CYCLE_COUNT      0x2au
#define BQ_SOC              0x2cu   /* % */
#define BQ_STATUS_FC        (1u << 5)
#define BQ_STATUS_DSG       (1u << 6)

struct lapis_battery_sample {
	unsigned percent, mv, remaining_mah, full_mah, cycles, status;
	int ma, temp_c100;
};

int lapis_battery_init(void);
int lapis_battery_read(struct lapis_battery_sample *s);

static vm_offset_t lb_i2c;
static unsigned lb_errors;

static uint32_t
lb_rd(uint32_t off)
{
	uint32_t v = *(volatile uint16_t *)(lb_i2c + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
lb_wr(uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint16_t *)(lb_i2c + off) = (uint16_t)value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* One gauge word; false on NACK, timeout or an unexpected FIFO count. */
static bool
lb_word_once(uint8_t cmd, uint16_t *out)
{
	uint32_t st = 0, count, lo, hi;
	lb_wr(LB_CONTROL, (lb_rd(LB_CONTROL) & ~(LB_CTL_DIR_CHANGE | LB_CTL_RS)) |
	    LB_CTL_DIR_CHANGE | LB_CTL_RS);
	lb_wr(LB_EXT_CONF, LB_FS_START_CON);
	lb_wr(LB_SLAVE_ADDR, LB_GAUGE << 1);
	lb_wr(LB_INTR_STAT, LB_INT_ALL);
	lb_wr(LB_FIFO_ADDR_CLR, 1);
	lb_wr(LB_INTR_MASK, LB_INT_ALL);
	lb_wr(LB_TRANSFER_LEN, 1);
	lb_wr(LB_TRANSFER_LEN_AUX, 2);
	lb_wr(LB_TRANSAC_LEN, 2);
	lb_wr(LB_DATA_PORT, cmd);
	lb_wr(LB_START, 1);
	for (unsigned i = 0; i < 20000; i++) {   /* ~20 ms */
		st = lb_rd(LB_INTR_STAT);
		if (st & (LB_INT_COMP | LB_INT_ACKERR | LB_INT_HS_NACKERR)) {
			break;
		}
		delay(1);
	}
	lb_wr(LB_INTR_STAT, LB_INT_ALL);
	if (!(st & LB_INT_COMP) || (st & (LB_INT_ACKERR | LB_INT_HS_NACKERR))) {
		return false;
	}
	count = lb_rd(LB_FIFO_STAT) & 0xfu;
	if (count == 3u) {
		(void)lb_rd(LB_DATA_PORT);      /* the extra leading byte */
	} else if (count != 2u) {
		return false;
	}
	lo = lb_rd(LB_DATA_PORT) & 0xffu;
	hi = lb_rd(LB_DATA_PORT) & 0xffu;
	*out = (uint16_t)(lo | hi << 8);
	return true;
}

static bool
lb_word(uint8_t cmd, uint16_t *out)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (lb_word_once(cmd, out)) {
			return true;
		}
		lb_errors++;
		delay(1000);
	}
	return false;
}

int
lapis_battery_init(void)
{
	uint16_t soc = 0;
	if (lb_i2c == 0) {
		lb_i2c = ml_io_map(LB_I2C_PA, 0x1000);
	}
	if (lb_i2c == 0 || !lb_word(BQ_SOC, &soc) || soc > 100) {
		printf("LAPIS BATTERY: gauge not answering (soc=%u errors=%u)\n", soc, lb_errors);
		return -1;
	}
	printf("LAPIS BATTERY: bq28z610 on I2C0, %u%%\n", soc);
	return 0;
}

int
lapis_battery_read(struct lapis_battery_sample *s)
{
	uint16_t soc, mv, status, cur, temp, rem, full, cycles;
	if (lb_i2c == 0 ||
	    !lb_word(BQ_SOC, &soc) || !lb_word(BQ_VOLTAGE, &mv) ||
	    !lb_word(BQ_BATTERY_STATUS, &status) || !lb_word(BQ_CURRENT, &cur) ||
	    !lb_word(BQ_TEMPERATURE, &temp) || !lb_word(BQ_REMAINING, &rem) ||
	    !lb_word(BQ_FULL_CHARGE, &full) || !lb_word(BQ_CYCLE_COUNT, &cycles)) {
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
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
