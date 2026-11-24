/*
 * lapis MT6375 charger probe (shim alternate main, build.sh tchg). Read-only.
 *
 * MT6375 on I2C5 (0x11280000, mediatek,mt6989-i2c, 1 MHz). Stock mt6375.ko:
 * 16-bit register = bank << 8 | reg; bank selects an I2C address
 * (0: 0x4e, 1: 0x34 = the DT client, 2: 0x1a, 3: 0x4a, 4: 0x64, 5: 0x3f; vendor id 0x100 = 0x7X). Stock
 * mt6375-charger.ko fields (bank 1 -> 0x34): 0x120 CHG_EN b0 / BUCK_EN b1 /
 * HZ b3, 0x122 IAICR [6:0] (100 mA + (sel-2)*25), 0x125 CV [6:0]
 * (3900 mV + sel*10), 0x126 CC [5:0] (300 mA + (sel-6)*50), 0x12a WDT [1:0]
 * / WDT_RST b2 / WDT_EN b3, 0x134 IC_STAT [3:0], 0x151 PORT_STAT [3:0],
 * 0x1e0 ST_PWR_RDY b0. Same I2C transaction as tgauge (one extra leading
 * FIFO byte). Shows them live: plug and unplug the charger.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define I2C		0x11280000UL
#define DATA_PORT	0x00
#define INTR_MASK	0x08
#define INTR_STAT	0x0c
#define CONTROL		0x10
#define TRANSFER_LEN	0x14
#define TRANSAC_LEN	0x18
#define TIMING		0x20
#define START		0x24
#define EXT_CONF	0x28
#define LTIMING		0x2c
#define FIFO_ADDR_CLR	0x38
#define TRANSFER_LEN_AUX 0x44
#define CLOCK_DIV	0x48
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

static uint32_t n_err, last_fifo;

/* One register byte; -1 on NACK / timeout / odd FIFO count. */
static int reg_read(uint32_t addr7, uint8_t reg)
{
	uint32_t st = 0;
	wr(CONTROL, (rd(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS)) | CTL_DIR_CHANGE | CTL_RS);
	wr(EXT_CONF, FS_START_CON);
	wr(SLAVE_ADDR, addr7 << 1);
	wr(INTR_STAT, INT_ALL);
	wr(FIFO_ADDR_CLR, 1);
	wr(INTR_MASK, INT_ALL);
	wr(TRANSFER_LEN, 1);
	wr(TRANSFER_LEN_AUX, 1);
	wr(TRANSAC_LEN, 2);
	wr(DATA_PORT, reg);
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
		return -1;
	}
	last_fifo = rd(FIFO_STAT);
	uint32_t count = last_fifo & 0xf;
	if (count == 2)
		(void)rd(DATA_PORT);
	else if (count != 1) {
		n_err++;
		return -1;
	}
	return (int)(rd(DATA_PORT) & 0xff);
}

#define CHG(reg)	reg_read(0x34, (reg) & 0xff)

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tchg v2: MT6375 bank 1 = 0x34 (read-only)");
	con_hex2("I2C5", "control=", rd(CONTROL), "timing=", rd(TIMING));
	con_hex2("I2C5", "ltiming=", rd(LTIMING), "clkdiv=", rd(CLOCK_DIV));
	int vid = reg_read(0x34, 0x00);	/* 0x100: vendor id, 0x7X */
	con_hex2("MT6375", "vid 0x100=", (uint32_t)vid, "fifo=", last_fifo);
	con_skip();
	int live = con_row();
	for (;;) {
		int ctrl = CHG(0x120), aicr = CHG(0x122), cv = CHG(0x125), cc = CHG(0x126);
		int wdt = CHG(0x12a), ic = CHG(0x134), port = CHG(0x151), prdy = CHG(0x1e0);
		con_set_row(live);
		con_hex2("CTRL 120", "", (uint32_t)ctrl, "errors=", n_err);
		con_hex2("AICR 122", "", (uint32_t)aicr, "mA=", aicr >= 2 ? 100u + ((uint32_t)(aicr & 0x7f) - 2u) * 25u : 0);
		con_hex2("CV   125", "", (uint32_t)cv, "mV=", 3900u + (uint32_t)(cv & 0x7f) * 10u);
		con_hex2("CC   126", "", (uint32_t)cc, "mA=", (cc & 0x3f) >= 6 ? 300u + ((uint32_t)(cc & 0x3f) - 6u) * 50u : 0);
		con_hex2("WDT  12a", "", (uint32_t)wdt, "ic_stat=", (uint32_t)ic);
		con_hex2("PORT 151", "", (uint32_t)port, "pwr_rdy=", (uint32_t)prdy);
		udelay(500000);
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
