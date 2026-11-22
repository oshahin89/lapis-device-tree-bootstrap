/*
 * lapis battery gauge probe (shim alternate main, build.sh tgauge).
 *
 * Android: bq28z610 fuel gauge at 0x55 on I2C0 (0x11c20000,
 * mediatek,mt6989-i2c, 400 kHz). Stock i2c-mt65xx.ko: mt6989 uses
 * mt_i2c_regs_v2 (upstream offsets) with aux_len_reg and auto_restart.
 * A gauge read = one WRRD transaction: write the command byte, repeated
 * start, read 2 bytes (little endian), as upstream mtk_i2c_do_transfer in
 * FIFO mode. Shows the controller registers LK left, then the gauge's
 * standard commands, refreshed every second. Only reads from the gauge.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define I2C		0x11c20000UL
#define DATA_PORT	0x00
#define INTR_MASK	0x08
#define INTR_STAT	0x0c
#define CONTROL		0x10
#define TRANSFER_LEN	0x14
#define TRANSAC_LEN	0x18
#define DELAY_LEN	0x1c
#define TIMING		0x20
#define START		0x24
#define EXT_CONF	0x28
#define LTIMING		0x2c
#define HS		0x30
#define IO_CONFIG	0x34
#define FIFO_ADDR_CLR	0x38
#define TRANSFER_LEN_AUX 0x44
#define CLOCK_DIV	0x48
#define SOFTRESET	0x50
#define SLAVE_ADDR	0x94
#define DEBUGSTAT	0xe4
#define FIFO_STAT	0xf4

#define CTL_RS		(1u << 1)
#define CTL_DIR_CHANGE	(1u << 4)
#define INT_COMP	(1u << 0)
#define INT_ACKERR	(1u << 1)
#define INT_HS_NACKERR	(1u << 2)
#define INT_RS		(1u << 4)
#define FS_START_CON	0x1800
#define GAUGE		0x55

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

static uint32_t last_stat, n_err;

/* Write cmd, repeated start, read 2 bytes. -1 on NACK/timeout. */
static int gauge_word(uint8_t cmd)
{
	uint32_t c = rd(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS);
	wr(CONTROL, c | CTL_DIR_CHANGE | CTL_RS);
	wr(EXT_CONF, FS_START_CON);
	wr(SLAVE_ADDR, GAUGE << 1);
	wr(INTR_STAT, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	wr(FIFO_ADDR_CLR, 1);
	wr(INTR_MASK, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	wr(TRANSFER_LEN, 1);
	wr(TRANSFER_LEN_AUX, 2);
	wr(TRANSAC_LEN, 2);
	wr(DATA_PORT, cmd);
	wr(START, 1);
	uint32_t st = 0;
	for (int i = 0; i < 20000; i++) {	/* ~20 ms */
		st = rd(INTR_STAT);
		if (st & (INT_COMP | INT_ACKERR | INT_HS_NACKERR))
			break;
		udelay(1);
	}
	last_stat = st;
	wr(INTR_STAT, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	if (!(st & INT_COMP) || (st & (INT_ACKERR | INT_HS_NACKERR))) {
		n_err++;
		return -1;
	}
	uint32_t lo = rd(DATA_PORT) & 0xff, hi = rd(DATA_PORT) & 0xff;
	return (int)(lo | hi << 8);
}

/* v2: one WRRD of cmd with an rx length of n, then the raw FIFO: FIFO_STAT
 * and 4 port reads, packed as b0<<24|b1<<16|b2<<8|b3. */
static uint32_t raw_fifo_stat;
static uint32_t gauge_raw(uint8_t cmd, uint32_t n)
{
	uint32_t c = rd(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS);
	wr(CONTROL, c | CTL_DIR_CHANGE | CTL_RS);
	wr(EXT_CONF, FS_START_CON);
	wr(SLAVE_ADDR, GAUGE << 1);
	wr(INTR_STAT, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	wr(FIFO_ADDR_CLR, 1);
	wr(INTR_MASK, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	wr(TRANSFER_LEN, 1);
	wr(TRANSFER_LEN_AUX, n);
	wr(TRANSAC_LEN, 2);
	wr(DATA_PORT, cmd);
	wr(START, 1);
	for (int i = 0; i < 20000; i++) {
		if (rd(INTR_STAT) & (INT_COMP | INT_ACKERR | INT_HS_NACKERR))
			break;
		udelay(1);
	}
	wr(INTR_STAT, INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP);
	raw_fifo_stat = rd(FIFO_STAT);
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v = v << 8 | (rd(DATA_PORT) & 0xff);
	return v;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tgauge v2: raw FIFO bytes (read-only)");
	con_hex2("I2C0", "control=", rd(CONTROL), "timing=", rd(TIMING));
	con_hex2("I2C0", "ltiming=", rd(LTIMING), "clkdiv=", rd(CLOCK_DIV));
	con_hex2("I2C0", "ext_conf=", rd(EXT_CONF), "io_cfg=", rd(IO_CONFIG));
	con_hex2("I2C0", "hs=", rd(HS), "debugstat=", rd(DEBUGSTAT));
	con_hex2("I2C0", "intr_stat=", rd(INTR_STAT), "fifo_stat=", rd(FIFO_STAT));
	con_skip();
	/* v2: raw FIFO for Voltage (0x08) and SOC (0x2C), rx length 2 and 3 */
	uint32_t r;
	r = gauge_raw(0x08, 2);
	con_hex2("RAW08 n2", "bytes=", r, "fifo=", raw_fifo_stat);
	r = gauge_raw(0x08, 3);
	con_hex2("RAW08 n3", "bytes=", r, "fifo=", raw_fifo_stat);
	r = gauge_raw(0x2c, 2);
	con_hex2("RAW2C n2", "bytes=", r, "fifo=", raw_fifo_stat);
	r = gauge_raw(0x2c, 3);
	con_hex2("RAW2C n3", "bytes=", r, "fifo=", raw_fifo_stat);
	con_skip();
	int live = con_row();
	for (;;) {
		int soc = gauge_word(0x2c), mv = gauge_word(0x08), ma = gauge_word(0x0c);
		int temp = gauge_word(0x06), status = gauge_word(0x0a), cycles = gauge_word(0x2a);
		int rem = gauge_word(0x10), full = gauge_word(0x12);
		con_set_row(live);
		con_hex2("SOC %", "", (uint32_t)soc, "errors=", n_err);
		con_hex2("VOLT mV", "", (uint32_t)mv, "stat=", last_stat);
		con_hex2("CURR mA", "", (uint32_t)ma, 0, 0);
		con_hex2("TEMP dK", "", (uint32_t)temp, 0, 0);
		con_hex2("STATUS", "", (uint32_t)status, "cycles=", (uint32_t)cycles);
		con_hex2("CAP mAh", "rem=", (uint32_t)rem, "full=", (uint32_t)full);
		udelay(1000000);
	}
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
