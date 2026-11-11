/*
 * Write text into Android's ramoops console zone so the next Android boot
 * shows it as /sys/fs/pstore/console-ramoops-0 (device/lapis_ufs_bringup.md).
 *
 * Layout measured on lapis (ramoops.mem_address 0x48090000, mem_size 0xe0000,
 * record_size 0x1000, console_size 0x40000, ecc 0): 31 dump records come
 * first, so the console zone is 0x480af000, a struct persistent_ram_buffer
 * { u32 sig = "DBGC"; u32 start; u32 size; u8 data[]; }. Linux keeps the old
 * contents when sig matches, size <= capacity and start <= size; it copies
 * data[start..size) then data[0..start), so start == size == length gives the
 * text in order. ECC is off.
 *
 * MMU off: Device memory, so only aligned 32-bit stores.
 */
#include <stdint.h>
#include "pstore.h"

#define PSTORE_CONSOLE		0x480af000UL
#define PSTORE_CONSOLE_SIZE	0x40000UL
#define PRZ_SIG			0x43474244U	/* "DBGC" */
#define PRZ_HDR			12UL
#define PRZ_CAP			(PSTORE_CONSOLE_SIZE - PRZ_HDR)

static uint32_t len;
static uint32_t word;		/* pending bytes of the current 32-bit word */

static inline volatile uint32_t *hdr(void)
{
	return (volatile uint32_t *)PSTORE_CONSOLE;
}

static void flush_word(void)
{
	/* data starts at +12, so byte offset (12 + len) & ~3 is word aligned */
	uintptr_t a = PSTORE_CONSOLE + PRZ_HDR + (len & ~3U);
	*(volatile uint32_t *)a = word;
}

void ps_init(void)
{
	len = 0;
	word = 0;
	hdr()[0] = PRZ_SIG;
	hdr()[1] = 0;
	hdr()[2] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

void ps_putc(char c)
{
	if (len >= PRZ_CAP - 4)
		return;
	word |= (uint32_t)(uint8_t)c << (8 * (len & 3));
	flush_word();
	len++;
	if ((len & 3) == 0)
		word = 0;
	hdr()[1] = len;		/* start */
	hdr()[2] = len;		/* size */
}

void ps_puts(const char *s)
{
	while (*s)
		ps_putc(*s++);
	__asm__ volatile("dsb sy" ::: "memory");
}

void ps_hex(uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = digits - 1; i >= 0; i--)
		ps_putc(hex[(v >> (4 * i)) & 0xf]);
}
