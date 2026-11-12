/*
 * NT38771 chip-ID probe over bit-banged SPI3 (alternative main for the shim;
 * build.sh OUT tchip). device/nt38771_host_download.md has the protocol.
 *
 * 1. Shows the current mux/dir/do of GPIO 68-71 (SPI3 CLK/CSB/MOSI/MISO).
 * 2. Switches those four pins to GPIO (mode 0): CLK/CSB/MOSI outputs
 *    (CSB high, CLK low), MISO input. No other pin is touched; GPIO 90
 *    (panel + touch reset) is left alone.
 * 3. Sends two SPI mode-0 messages to the touch controller:
 *      set page 0x3F004:  FF 07 E0
 *      read 6 bytes:      04 00 00 00 00 00 00 + 1 dummy (payload from rx[2])
^ *    The page select is the only write to the chip: no reset, no zero
 *    write, no firmware.
 * 4. Shows every received byte and whether rx[5..7] (driver buf[4..6]) are
 *    72|71, 87, 03.
 * 5. After 30 s returns to fastboot (reboot mode 3 + PSCI SYSTEM_RESET).
 *
 * MMIO writes: GPIO base 0x10005000 (+0x380 mode, +0x20 dir, +0x120 do) for
 * pins 68-71 only, and TOPRGU (watchdog off in start.S, reboot path).
 */
#include <stdint.h>
#include "console.h"
#include "reboot.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GPIO		0x10005000UL	/* runtime FDT pinctrl reg "gpio" */
#define GPIO_MODE	(GPIO + 0x380)	/* pins 64-71, 4 bits each: 68..71 = bits 16..31 */
#define GPIO_DIR	(GPIO + 0x020)	/* pins 64-95, bit = pin - 64 */
#define GPIO_DO		(GPIO + 0x120)
#define GPIO_DI		(GPIO + 0x220)
#define B_CLK		(1U << 4)	/* GPIO 68 */
#define B_CS		(1U << 5)	/* GPIO 69 */
#define B_MOSI		(1U << 6)	/* GPIO 70 */
#define B_MISO		(1U << 7)	/* GPIO 71 */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t half_period;	/* counter ticks for half an SCK period */

static void tick_wait(uint64_t n)
{
	uint64_t t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < n)
		;
}

static void wait_seconds(unsigned s)
{
	tick_wait(rsr("cntfrq_el0") * s);
}

static void do_set(uint32_t set, uint32_t clr)
{
	wr32(GPIO_DO, (rd32(GPIO_DO) & ~clr) | set);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* SPI mode 0, MSB first: MOSI valid before the rising edge, MISO sampled on it. */
static uint8_t xfer_byte(uint8_t out)
{
	uint8_t in = 0;
	for (int b = 7; b >= 0; b--) {
		do_set((out >> b) & 1 ? B_MOSI : 0, (out >> b) & 1 ? 0 : B_MOSI);
		tick_wait(half_period);
		do_set(B_CLK, 0);
		in = (uint8_t)((in << 1) | ((rd32(GPIO_DI) & B_MISO) ? 1 : 0));
		tick_wait(half_period);
		do_set(0, B_CLK);
	}
	return in;
}

static void message(const uint8_t *tx, uint8_t *rx, int n)
{
	do_set(0, B_CS);
	tick_wait(half_period * 4);
	for (int i = 0; i < n; i++) {
		uint8_t r = xfer_byte(tx[i]);
		if (rx)
			rx[i] = r;
	}
	tick_wait(half_period * 2);
	do_set(B_CS, B_MOSI);
	tick_wait(half_period * 8);
}

static uint32_t word(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "NT38771 chip-ID probe (bit-bang SPI3)");
	half_period = rsr("cntfrq_el0") / 500000;	/* 2 us -> 250 kHz SCK */
	if (half_period == 0)
		half_period = 1;

	uint32_t mode = rd32(GPIO_MODE), dir = rd32(GPIO_DIR), dout = rd32(GPIO_DO);
	con_hex2("BEFORE", "mode=", mode, "dir=", dir);
	con_hex2("BEFORE", "do=", dout, "di=", rd32(GPIO_DI));

	/* outputs idle: CS high, CLK low, MOSI low; then direction; then mux */
	wr32(GPIO_DO, (dout & ~(B_CLK | B_MOSI)) | B_CS);
	wr32(GPIO_DIR, (dir & ~B_MISO) | B_CLK | B_CS | B_MOSI);
	wr32(GPIO_MODE, mode & 0x0000ffffU);	/* pins 68-71 -> function 0 (GPIO) */
	__asm__ volatile("dsb sy" ::: "memory");
	tick_wait(rsr("cntfrq_el0") / 1000);
	con_hex2("AFTER", "mode=", rd32(GPIO_MODE), "dir=", rd32(GPIO_DIR));
	con_skip();

	for (int attempt = 0; attempt < 2; attempt++) {
		static const uint8_t page[3] = { 0xFF, 0x07, 0xE0 };	/* 0x3F004 >> 15, >> 7 */
		uint8_t tx[8] = { 0x04, 0, 0, 0, 0, 0, 0, 0 };		/* 0x3F004 & 0x7F, read */
		uint8_t rx[8] = { 0 };
		message(page, 0, 3);
		message(tx, rx, 8);
		con_hex2("RX0-7", "", word(rx), "", word(rx + 4));
		/* driver buf[1..6] = rx[2..7]; ID bytes buf[4..6] = rx[5..7] */
		int nt = rx[6] == 0x87 && rx[7] == 0x03 && (rx[5] == 0x72 || rx[5] == 0x71);
		con_str("ID", nt ? "NT38771 found (bytes 4-6 match)" : "no NT38771 ID");
	}

	con_skip();
	con_str("REBOOT", "to fastboot in 30 s (PSCI)");
	wait_seconds(30);
	int64_t rc = lapis_reboot_bootloader_psci();
	con_hex("PSCI rc", (uint64_t)rc);
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
