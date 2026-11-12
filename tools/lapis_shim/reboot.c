/*
 * Warm reset into LK fastboot, the way Linux's `reboot bootloader` does on
 * lapis (device/lapis_ufs_bringup.md, runtime FDT watchdog@1c00a000):
 *
 *  1. syscon-reboot-mode: TOPRGU +0x24 (NONRST_REG2, survives reset), bits
 *     [3:0] = mode; "mode-bootloader = <3>". Read-modify-write; bits 0x820
 *     are cleared too (see below).
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
#define RGU_KERNEL_FLAGS	0x820U	/* see lapis_reboot_bootloader() */

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
	/*
	 * Measured in the preloader's RGU dumps (expdb): after Linux's
	 * `reboot bootloader` this register (NONRST_REG2) reads 0x2C002003;
	 * after our first try it read 0x2C002820 and LK did not enter fastboot
	 * (black screen). Bits 0x820 only appear after our kernels' resets, never
	 * after Linux's; their meaning is inferred, not documented. Clear them and
	 * leave the value Linux leaves.
	 */
	uint32_t m = rd32(TOPRGU + RGU_REBOOT_MODE);
	wr32(TOPRGU + RGU_REBOOT_MODE,
	    (m & ~(REBOOT_MODE_MASK | RGU_KERNEL_FLAGS)) | REBOOT_MODE_BOOTLOADER);

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

/*
 * Linux on arm64 restarts through PSCI SYSTEM_RESET when the firmware
 * provides it, after the reboot-mode notifier wrote NONRST_REG2. Our direct
 * WDT_SWRST resets left the preloader reading NONRST_REG2 = 0x2C002820
 * although we had written ...2003, so let the firmware do the reset.
 * Returns only if the PSCI call fails (x0 = negative PSCI error).
 */
int64_t lapis_reboot_bootloader_psci(void)
{
	uint32_t m = rd32(TOPRGU + RGU_REBOOT_MODE);
	wr32(TOPRGU + RGU_REBOOT_MODE,
	    (m & ~(REBOOT_MODE_MASK | RGU_KERNEL_FLAGS)) | REBOOT_MODE_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");

	register uint64_t x0 __asm__("x0") = 0x84000009ULL;	/* PSCI SYSTEM_RESET */
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
	    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}
