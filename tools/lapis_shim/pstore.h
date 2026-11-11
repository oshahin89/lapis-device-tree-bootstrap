#pragma once
#include <stdint.h>

/* Android ramoops console zone writer (see pstore.c). */
void ps_init(void);
void ps_putc(char c);
void ps_puts(const char *s);
void ps_hex(uint64_t v, int digits);
