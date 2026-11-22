/*
 * lapis XNU loader: act as iBoot for the SWEET-config xnu.
 *
 * Derived from ios-sweet shim/loader.c (MIT, Copyright (c) 2026 dipa-studio;
 * see LICENSE.ios-sweet). Changes for lapis:
 *   - the SWEETXNU payload is appended to the shim Image (LK on lapis takes
 *     the ramdisk from vendor_boot/init_boot, not from boot_b), so it is found
 *     at the first 4 KiB boundary after _end instead of in /chosen initrd;
 *   - the kernel's RAM range is checked against the bank measured free on
 *     lapis before anything is written;
 *   - boot_args.Video carries the measured LK scan-out buffer.
 *
 * mkpayload.py has already resolved the kernel's chained fixups and built
 * the ADT, so this copies segments to phys_base + (vmaddr - virt_base),
 * places the ADT and boot_args behind the kernel and jumps to the entry with
 * the MMU and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608

/* boot_args.Video presented to xnu: the whole panel (see loader_boot) */
#define VIDEO_W			FB_W
#define VIDEO_H			FB_H
#define VIDEO_OFF_Y		0
#define PAGE_16K		0x4000UL

/*
 * System RAM bank 0x140000000-0x33d7fffff with its first nested reservation
 * at 0x32df00000 (device/runtime_iomem.txt); tools/lapis_mkadt.py uses
 * 0x140000000 + 4 GiB. Refuse anything outside.
 */
#define SAFE_RAM_LO		0x140000000UL
#define SAFE_RAM_HI		0x32df00000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

extern char _head[], _end[];

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static int in_safe_ram(uint64_t lo, uint64_t hi)
{
	return lo >= SAFE_RAM_LO && hi <= SAFE_RAM_HI && lo < hi;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(void)
{
	uintptr_t img = (uintptr_t)_head;
	uintptr_t base = ((uintptr_t)_end + 0xfff) & ~0xfffUL;
	uint64_t image_size = *(volatile uint64_t *)(img + 16);
	con_hex2("IMAGE", "at=", img, "size=", (uint32_t)image_size);
	con_hex("PAYLOAD", base);

	const struct payload *p = (const struct payload *)base;
	if (image_size <= base - img || !magic_ok(p->magic))
		return "no SWEETXNU payload behind the shim";
	if (p->version != 1 || (p->adt_off & 7))
		return "bad payload header";

	uint64_t delta = p->phys_base - p->virt_base;	/* pa = va + delta */
	con_hex("PHYSBASE", p->phys_base);
	con_hex("MEMSIZE", p->mem_size);
	con_hex("ENTRY", p->entry_va);

	if (!in_safe_ram(p->phys_base, p->phys_base + p->mem_size))
		return "kernel RAM outside the measured free bank";
	if (img + image_size > p->phys_base)
		return "shim image overlaps kernel RAM";

	uint64_t hi = 0;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uint64_t end = s->vmaddr + delta + s->vmsize;
		if (!in_safe_ram(s->vmaddr + delta, end))
			return "segment outside kernel RAM";
		if (s->file_off + s->file_size > image_size - (base - img))
			return "segment beyond the payload";
		if (end > hi)
			hi = end;
	}

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + delta;
		copy64(dst, base + s->file_off, s->file_size);
		uint64_t done = (s->file_size + 7) & ~7UL;
		if (s->vmsize > done)
			zero64(dst + done, s->vmsize - done);
	}
	con_hex2("SEGS", "n=", p->nsegs, "endlo=", (uint32_t)hi);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + delta;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;
	if (!in_safe_ram(adt_pa, top) || top > p->phys_base + p->mem_size)
		return "ADT/boot_args outside kernel RAM";
	copy64(adt_pa, base + p->adt_off, p->adt_size);

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * Present the whole 1280x2772 panel. The iPhone X userland still sees a
	 * 1125x2436 display: SWEET's AppleCLCD (patch 0061) reports that size,
	 * and lapis patch 0086 scales every presented layer up to the panel
	 * (1.138x, the same aspect ratio to 0.02%). Before that, a left-aligned
	 * 1125x2436 window at row 168 was used (commit c891517); reporting the
	 * native size instead makes CoreAnimation ask for the hardware scaler
	 * (AppleM2ScalerCSCDriver) and never present.
	 */
	ba->Video.v_baseAddr = FB_BASE + (uint64_t)VIDEO_OFF_Y * FB_STRIDE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = VIDEO_W;
	ba->Video.v_height = VIDEO_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - delta;		/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex("BOOTARGS", ba_pa);
	con_hex("TOP", top);

	uintptr_t entry_pa = p->entry_va + delta;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
