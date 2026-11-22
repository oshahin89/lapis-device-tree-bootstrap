#pragma once
#include <stdint.h>
int dual_pick_ios(void);
void dual_wdt_off(void);
void __attribute__((noreturn)) dual_chain_android(uint64_t dtb);
