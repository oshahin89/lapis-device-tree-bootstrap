#pragma once

/* Warm reset into LK fastboot (TOPRGU reboot mode 3 + WDT_SWRST); never returns. */
void lapis_reboot_bootloader(void) __attribute__((noreturn));
