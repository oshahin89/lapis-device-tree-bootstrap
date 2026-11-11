/*
 * Console API from ios-sweet shim/console.h (MIT, see LICENSE.ios-sweet);
 * framebuffer constants replaced with the lapis values measured on the
 * device with the stage 1 probe (device/lapis_boot_contract.md).
 */
#pragma once
#include <stdint.h>

#define FB_BASE		0xfd72f000UL	/* LK scan-out buffer 0 (mblock-15-framebuffer) */
#define FB_W		1280
#define FB_H		2772
#define FB_STRIDE	5120		/* ARGB8888, width aligned to 32 */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
