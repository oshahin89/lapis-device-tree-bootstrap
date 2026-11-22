#pragma once
#include <stdint.h>
enum { DUAL_ANDROID, DUAL_IOS, DUAL_PROBE };
int dual_pick(void);
void dual_wdt_off(void);
void __attribute__((noreturn)) dual_chain_android(uint64_t dtb);
void __attribute__((noreturn)) dual_reboot(void);
void __attribute__((noreturn)) dual_power_reboot(void);
/* build.sh dual PROBE: the probe's shim_main, renamed */
void probe_main(uint64_t dtb, uint64_t entry_el);
