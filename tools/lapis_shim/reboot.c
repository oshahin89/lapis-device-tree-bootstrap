/*
 * Warm reset into LK fastboot, the way Linux's `reboot bootloader` does on
 * lapis (device/lapis_ufs_bringup.md, runtime FDT watchdog@1c00a000):
 *
 *  1. syscon-reboot-mode: TOPRGU +0x24 (a non-reset register), bits [3:0] =
 *     mode; "mode-bootloader = <3>". Read-modify-write, other bits kept.
 *  2. mtk_wdt_restart (drivers/watchdog/mtk_wdt.c): WDT_MODE &= ~IRQ_EN,
 *     written with the 0x22 key; then WDT_SWRST (+0x14) = 0x1209, repeated.
 *
 * A warm reset keeps DRAM, so the pstore console zone survives into the
 * restore boot (a key reset does not).
 */
#include <stdint.h>
#include "reboot.h"

#define TOPRGU			0x1c00a000UL
#define WDT_MODE		0x00
#define WDT_MODE_IRQ_EN		(1U << 3)
#define WDT_MODE_KEY		0x22000000U
#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209U
#define RGU_REBOOT_MODE		0x24
#define REBOOT_MODE_MASK	0x0fU
#define REBOOT_MODE_BOOTLOADER	0x03U

static inline uint32_t rd32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static void delay_ticks(uint64_t n)
{
	uint64_t t0, t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
	do
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	while (t - t0 < n);
}

void lapis_reboot_bootloader(void)
{
	uint32_t m = rd32(TOPRGU + RGU_REBOOT_MODE);
	wr32(TOPRGU + RGU_REBOOT_MODE, (m & ~REBOOT_MODE_MASK) | REBOOT_MODE_BOOTLOADER);

	uint32_t mode = rd32(TOPRGU + WDT_MODE) & 0x00ffffffU;
	wr32(TOPRGU + WDT_MODE, (mode & ~WDT_MODE_IRQ_EN) | WDT_MODE_KEY);
	__asm__ volatile("dsb sy" ::: "memory");

	uint64_t frq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	for (;;) {
		wr32(TOPRGU + WDT_SWRST, WDT_SWRST_KEY);
		__asm__ volatile("dsb sy" ::: "memory");
		delay_ticks(frq / 200);		/* 5 ms, as mtk_wdt_restart */
	}
}
