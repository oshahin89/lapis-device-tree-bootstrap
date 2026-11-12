#pragma once
#include <stdint.h>

/* Warm reset into LK fastboot (TOPRGU reboot mode 3 + WDT_SWRST); never returns. */
void lapis_reboot_bootloader(void) __attribute__((noreturn));

/* Same reboot mode, then PSCI SYSTEM_RESET; returns the PSCI error on failure. */
int64_t lapis_reboot_bootloader_psci(void);
