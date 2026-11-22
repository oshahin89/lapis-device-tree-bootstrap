/*
 * lapis speaker amp probe (shim alternate main, build.sh tamp; dual: Volume
 * down). Read-only.
 *
 * Android (2026-11-22): the fitted amps are NXP TFA98xx at 6-0034 and 6-0037,
 * driver tfa98xx, firmware /vendor/firmware/tfa98xx.cnt. I2C6 = i2c@11e01000
 * (mediatek,mt6989-i2c, 400 kHz). Reset GPIO 188 / 189, IRQ GPIO 31 / 41,
 * reset-polarity 0. TFA98xx registers are 16 bit, big endian, 8-bit address;
 * 0x03 = REVISIONNUMBER (low byte = type: 0x73/0x74/0x78/0x94 ...).
 *
 * DATA_PORT is loaded with 8-bit stores: on I2C2 a 16-bit store pushed two
 * bytes (tvib v5). A read takes the FIFO count as n or n + 1 (one stale
 * leading byte, as on the other buses).
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPIO		0x10005000UL
#define I2C		0x11e01000UL
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

static inline uint32_t mrd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint32_t rd(uint32_t off) { return *(volatile uint16_t *)(I2C + off); }
static inline void wr(uint32_t off, uint32_t v)
{
	*(volatile uint16_t *)(I2C + off) = (uint16_t)v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline void wr8(uint32_t off, uint8_t v)
{
	*(volatile uint8_t *)(I2C + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static uint32_t n_err, last_st;

/* 16-bit big-endian register; -1 on NACK / timeout / odd FIFO count */
static int tfa_rd(uint32_t addr7, uint8_t reg)
{
	uint32_t st = 0;
	wr(CONTROL, (rd(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS)) | CTL_DIR_CHANGE | CTL_RS);
	wr(EXT_CONF, FS_START_CON);
	wr(SLAVE_ADDR, addr7 << 1);
	wr(INTR_STAT, INT_ALL);
	wr(FIFO_ADDR_CLR, 1);
	wr(INTR_MASK, INT_ALL);
	wr(TRANSFER_LEN, 1);
	wr(TRANSFER_LEN_AUX, 2);
	wr(TRANSAC_LEN, 2);
	wr8(DATA_PORT, reg);
	wr(START, 1);
	for (int i = 0; i < 20000; i++) {
		st = rd(INTR_STAT);
		if (st & (INT_COMP | INT_ACKERR | INT_HS_NACKERR))
			break;
		udelay(1);
	}
	wr(INTR_STAT, INT_ALL);
	last_st = st;
	if (!(st & INT_COMP) || (st & (INT_ACKERR | INT_HS_NACKERR))) {
		n_err++;
		return -1;
	}
	uint32_t count = rd(FIFO_STAT) & 0xf;
	if (count == 3)
		(void)rd(DATA_PORT);
	else if (count != 2) {
		n_err++;
		return -1;
	}
	uint32_t hi = rd(DATA_PORT) & 0xff, lo = rd(DATA_PORT) & 0xff;
	return (int)(hi << 8 | lo);
}

static void gpio_pin(const char *label, unsigned pin)
{
	uint32_t bit = 1u << (pin % 32), bank = (pin / 32) * 0x10;
	uint32_t mode = (mrd(GPIO + 0x300 + (pin / 8) * 0x10) >> ((pin % 8) * 4)) & 0xf;
	uint32_t v = (mode << 12) | ((mrd(GPIO + bank) & bit) ? 0x100 : 0) |
		     ((mrd(GPIO + 0x100 + bank) & bit) ? 0x10 : 0) | ((mrd(GPIO + 0x200 + bank) & bit) ? 1 : 0);
	con_hex2(label, "pin=", pin, "mode.dir.do.di=", v);
}

static void amp(const char *label, uint32_t a)
{
	int r03 = tfa_rd(a, 0x03);
	con_hex2(label, "rev 0x03=", (uint32_t)r03, "i2c stat=", last_st);
	if (r03 < 0)
		return;
	con_hex2(label, "0x00=", (uint32_t)tfa_rd(a, 0x00), "0x01=", (uint32_t)tfa_rd(a, 0x01));
	con_hex2(label, "0x10=", (uint32_t)tfa_rd(a, 0x10), "0x13=", (uint32_t)tfa_rd(a, 0x13));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tamp v1: TFA98xx amps on I2C6 (read-only)");
	con_hex2("I2C6", "control=", rd(CONTROL), "timing=", rd(TIMING));
	con_hex2("I2C6", "clkdiv=", rd(CLOCK_DIV), "ext=", rd(EXT_CONF));
	gpio_pin("RESET L", 188);
	gpio_pin("RESET R", 189);
	gpio_pin("IRQ L", 31);
	gpio_pin("IRQ R", 41);
	con_skip();
	amp("AMP 34", 0x34);
	amp("AMP 37", 0x37);
	/* the other bus users as a bus check */
	con_hex2("GATE 11", "0x00=", (uint32_t)tfa_rd(0x11, 0x00), "errors=", n_err);
	con_hex2("DONE", "errors=", n_err, 0, 0);
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
