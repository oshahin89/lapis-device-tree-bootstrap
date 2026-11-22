/*
 * lapis speaker test tone (shim alternate main, build.sh ttone; dual: Volume
 * down). 3 s of 1 kHz at about -12 dBFS from both speakers, AP-only path:
 * AFE SRAM ring -> memif DL0 -> interconnect -> I2S OUT4 -> TFA9865 x2.
 *
 * Where each value comes from (device/lapis_audio_bringup.md):
 *  - taud v1: audio domain, bus protect, peri gates and topckgen muxes keep
 *    Android's state after a reboot; APLL2 is off but its PCW is set
 *    (196.608 MHz). APLL2 power/enable as the MediaTek PLL driver: CON3 0x358
 *    PWR_ON bit 0 then ISO bit 1 off, CON0 0x34c EN bit 0.
 *  - Android playback trace: AFE clock gates 0x0..0x10 = 0, 0x14 = 9, 0x38 =
 *    0x375, 0x1204 = 0xb8, I2S OUT4 0x13c0..0x13e4, I2S IN4 0x1580/0x1590,
 *    0x1690, topckgen 0x320 = 0x23ffd.
 *  - snd-soc-mt6878-afe.ko: memif DL0 base 0x4444 / end 0x4454 (MSB 0x4440 /
 *    0x4450), CON 0x4460: enable bit 28, fs [12:8] (0xa = 48 kHz as Android
 *    used), hd bit 3 (0 = 16 bit), mono bit 4; interconnect I2SOUT4_CH1 <-
 *    DL0_CH1 = 0x2f04 bit 0, I2SOUT4_CH2 <- DL0_CH2 = 0x2f24 bit 1.
 *  - Pins (AFE pinctrl): I2S IN4 GPIO 42, 43, 45 and OUT4 data GPIO 44, mode 2.
 *  - Amps: reset GPIO 188/189 high, I2C6 (ACK detection on, 8-bit FIFO
 *    stores), container device list (tfa9865_init.h) + "music" profile, then
 *    Android's start: 0x00 = 0xf250, 0xf258, 0xf250, 0xe0 = 1, 0xe1 = 1,
 *    0x00 = 0xf258 once the I2S clock runs.
 */
#include <stdint.h>
#include "console.h"
#include "tfa9865_init.h"
#include "ttone_data.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPIO		0x10005000UL
#define TOPCK		0x10000000UL
#define APMIX		0x1000c000UL
#define AFE		0x11050000UL
#define RING		0x11060000UL	/* AFE SRAM, unused by Android's streams */
#define RING_BYTES	(48u * 4u * 42u)	/* 42 periods of 1 kHz stereo 16 bit */

#define I2C		0x11e01000UL
#define DATA_PORT	0x00
#define INTR_MASK	0x08
#define INTR_STAT	0x0c
#define CONTROL		0x10
#define TRANSFER_LEN	0x14
#define TRANSAC_LEN	0x18
#define START		0x24
#define EXT_CONF	0x28
#define FIFO_ADDR_CLR	0x38
#define TRANSFER_LEN_AUX 0x44
#define SLAVE_ADDR	0x94
#define FIFO_STAT	0xf4
#define CTL_RS		(1u << 1)
#define CTL_DIR_CHANGE	(1u << 4)
#define CTL_ACKERR_DET	(1u << 5)
#define INT_COMP	(1u << 0)
#define INT_ACKERR	(1u << 1)
#define INT_HS_NACKERR	(1u << 2)
#define INT_RS		(1u << 4)
#define INT_ALL		(INT_RS | INT_HS_NACKERR | INT_ACKERR | INT_COMP)
#define FS_START_CON	0x1800

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t i2r(uint32_t off) { return *(volatile uint16_t *)(I2C + off); }
static inline void i2w(uint32_t off, uint32_t v)
{
	*(volatile uint16_t *)(I2C + off) = (uint16_t)v;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline void i2w8(uint8_t v)
{
	*(volatile uint8_t *)(I2C + DATA_PORT) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static uint32_t n_err;

static void i2c_setup(uint32_t a, int wrrd)
{
	uint32_t c = (i2r(CONTROL) & ~(CTL_DIR_CHANGE | CTL_RS)) | CTL_ACKERR_DET;
	i2w(CONTROL, wrrd ? (c | CTL_DIR_CHANGE | CTL_RS) : c);
	i2w(EXT_CONF, FS_START_CON);
	i2w(SLAVE_ADDR, a << 1);
	i2w(INTR_STAT, INT_ALL);
	i2w(FIFO_ADDR_CLR, 1);
	i2w(INTR_MASK, INT_ALL);
}

static int i2c_go(void)
{
	uint32_t st = 0;
	i2w(START, 1);
	for (int i = 0; i < 20000; i++) {
		st = i2r(INTR_STAT);
		if (st & (INT_COMP | INT_ACKERR | INT_HS_NACKERR))
			break;
		udelay(1);
	}
	i2w(INTR_STAT, INT_ALL);
	if (!(st & INT_COMP) || (st & (INT_ACKERR | INT_HS_NACKERR))) {
		n_err++;
		return 0;
	}
	return 1;
}

static int tfa_rd(uint32_t a, uint8_t reg)
{
	i2c_setup(a, 1);
	i2w(TRANSFER_LEN, 1);
	i2w(TRANSFER_LEN_AUX, 2);
	i2w(TRANSAC_LEN, 2);
	i2w8(reg);
	if (!i2c_go())
		return -1;
	uint32_t count = i2r(FIFO_STAT) & 0xf;
	if (count == 3)
		(void)i2r(DATA_PORT);
	else if (count != 2) {
		n_err++;
		return -1;
	}
	uint32_t hi = i2r(DATA_PORT) & 0xff, lo = i2r(DATA_PORT) & 0xff;
	return (int)(hi << 8 | lo);
}

static int tfa_wr(uint32_t a, uint8_t reg, uint16_t v)
{
	i2c_setup(a, 0);
	i2w(TRANSFER_LEN_AUX, 0);
	i2w(TRANSFER_LEN, 3);
	i2w(TRANSAC_LEN, 1);
	i2w8(reg);
	i2w8((uint8_t)(v >> 8));
	i2w8((uint8_t)v);
	return i2c_go();
}

/* bitfield = reg << 8 | pos << 4 | (len - 1) */
static void tfa_bf(uint32_t a, uint16_t bf, uint16_t val)
{
	uint8_t reg = (uint8_t)(bf >> 8);
	uint32_t pos = (bf >> 4) & 0xf, len = (bf & 0xf) + 1;
	uint32_t mask = ((1u << len) - 1) << pos;
	int v = tfa_rd(a, reg);
	if (v < 0)
		return;
	tfa_wr(a, reg, (uint16_t)(((uint32_t)v & ~mask) | (((uint32_t)val << pos) & mask)));
}

static void set_mode(unsigned pin, uint32_t m)
{
	uintptr_t r = GPIO + 0x300 + (pin / 8) * 0x10;
	uint32_t sh = (pin % 8) * 4;
	wr(r, (rd(r) & ~(0xfu << sh)) | (m << sh));
}

static void gpio_high(unsigned pin)
{
	uint32_t bit = 1u << (pin % 32), bank = (pin / 32) * 0x10;
	set_mode(pin, 0);
	wr(GPIO + 0x100 + bank, rd(GPIO + 0x100 + bank) | bit);
	wr(GPIO + bank, rd(GPIO + bank) | bit);
}

static void amps(const char *label, uint16_t r0)
{
	for (uint32_t a = 0x34; a <= 0x37; a += 3)
		tfa_wr(a, 0x00, r0);
	con_hex2(label, "34: 00=", (uint32_t)tfa_rd(0x34, 0x00), "10=", (uint32_t)tfa_rd(0x34, 0x10));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "ttone v1: 3 s of 1 kHz on both speakers");

	/* 1. APLL2 */
	wr(APMIX + 0x358, rd(APMIX + 0x358) | 1u);
	udelay(1);
	wr(APMIX + 0x358, rd(APMIX + 0x358) & ~2u);
	udelay(1);
	wr(APMIX + 0x34c, rd(APMIX + 0x34c) | 1u);
	udelay(30);
	con_hex2("APLL2", "con0=", rd(APMIX + 0x34c), "con3=", rd(APMIX + 0x358));
	wr(TOPCK + 0x320, 0x23ffd);

	/* 2. AFE clocks and top control, as Android's playback start */
	for (uint32_t o = 0; o <= 0x10; o += 4)
		wr(AFE + o, 0);
	wr(AFE + 0x14, 9);
	wr(AFE + 0x38, 0x375);
	wr(AFE + 0x1204, 0xb8);

	/* 3. I2S OUT4 (+ IN4, which shares the clock pins) */
	wr(AFE + 0x13c4, 0x80098465);
	wr(AFE + 0x13c8, 0x0410c462);
	wr(AFE + 0x13cc, 0x15f80000);
	wr(AFE + 0x13d0, 0x00a00100);
	wr(AFE + 0x13e4, 0x1800);
	wr(AFE + 0x13c0, 0x109ff802);
	wr(AFE + 0x1590, 0x0a000445);
	wr(AFE + 0x1580, 0x109ff800);
	wr(AFE + 0x1690, 0x88b2a008);
	set_mode(42, 2);
	set_mode(43, 2);
	set_mode(44, 2);
	set_mode(45, 2);

	/* 4. the tone into AFE SRAM, memif DL0 over it, DL0 -> I2S OUT4 */
	volatile uint32_t *ring = (volatile uint32_t *)RING;
	for (uint32_t i = 0; i < RING_BYTES / 4; i++) {
		uint16_t s = (uint16_t)tone48[i % 48];
		ring[i] = (uint32_t)s << 16 | s;
	}
	wr(AFE + 0x4440, 0);
	wr(AFE + 0x4450, 0);
	wr(AFE + 0x4444, (uint32_t)RING);
	wr(AFE + 0x4454, (uint32_t)RING + RING_BYTES - 1);
	wr(AFE + 0x4460, 0xau << 8);			/* 48 kHz, stereo, 16 bit */
	wr(AFE + 0x2f04, rd(AFE + 0x2f04) | 1u);
	wr(AFE + 0x2f24, rd(AFE + 0x2f24) | 2u);

	/* 5. amps: out of reset, container config */
	gpio_high(188);
	gpio_high(189);
	udelay(5000);
	for (uint32_t a = 0x34; a <= 0x37; a += 3) {
		for (unsigned i = 0; i < sizeof(tfa_init) / sizeof(tfa_init[0]); i++)
			tfa_bf(a, tfa_init[i][0], tfa_init[i][1]);
		for (unsigned i = 0; i < sizeof(tfa_music) / sizeof(tfa_music[0]); i++)
			tfa_bf(a, tfa_music[i][0], tfa_music[i][1]);
	}
	con_hex2("AMPS", "configured, i2c errors=", n_err, "rev=", (uint32_t)tfa_rd(0x34, 0x03));
	amps("AMP", 0xf250);

	/* 6. start: I2S on, memif on, amps on */
	wr(AFE + 0x13c0, 0x109ff803);
	wr(AFE + 0x1580, 0x109ff801);
	wr(AFE + 0x4460, rd(AFE + 0x4460) | (1u << 28));
	udelay(2000);
	amps("AMP", 0xf258);
	amps("AMP", 0xf250);
	for (uint32_t a = 0x34; a <= 0x37; a += 3) {
		tfa_wr(a, 0xe0, 1);
		tfa_wr(a, 0xe1, 1);
	}
	amps("PLAY", 0xf258);
	uint32_t cur0 = rd(AFE + 0x444c);
	udelay(20000);
	con_hex2("DL0", "cur=", cur0, "20 ms later=", rd(AFE + 0x444c));
	con_hex2("AFE", "4460=", rd(AFE + 0x4460), "13c0=", rd(AFE + 0x13c0));
	udelay(3000000);
	con_hex2("AMP 37", "00=", (uint32_t)tfa_rd(0x37, 0x00), "10=", (uint32_t)tfa_rd(0x37, 0x10));

	/* 7. stop */
	amps("STOP", 0xf250);
	wr(AFE + 0x4460, rd(AFE + 0x4460) & ~(1u << 28));
	wr(AFE + 0x13c0, 0x109ff802);
	wr(AFE + 0x1580, 0x109ff800);
	con_hex2("DONE", "i2c errors=", n_err, 0, 0);
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
