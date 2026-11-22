/*
 * lapis brightness probe (shim alternate main, build.sh tbl; dual: Volume down).
 *
 * tdsi on the phone (2026-11-22): DSI0 0x1401a000 runs video mode
 * (MODE_CTRL 0x14 = 2, START 0x00 = 1, INTSTA 0x0c = 0x800b0712 busy), LK's
 * last command in CMDQ0 is 0xd00 = 0x00033902 / 0xd04 = 0x00390651: a DCS long
 * write (0x39) of 3 bytes 51 06 39, i.e. brightness 0x639 of 14 bits, and
 * VM_CMD_CON 0x200 = 0x21 (VM_CMD_EN | TS_VFP_EN) is already set up.
 *
 * The stock mediatek-drm mtk_dsi_vm_cmdq path for a long packet, with the
 * mt6878 offsets (VM_CMD_CON 0x200, VM_CMD_DATA0 0x208): data bytes into
 * VM_CMD_DATA0, VM_CMD_CON = data id << 8 | length << 16 | LONG_PKT | VM_CMD_EN
 * | TS_VFP_EN, clear VM_CMD_DONE (INTSTA bit 5), set VM_CMD_START (START bit
 * 16), wait for VM_CMD_DONE, clear VM_CMD_START. The command goes out in the
 * vertical front porch while the video stream keeps running.
 *
 * Sequence (2 s each): 0x0200 (dim), 0x0639 (LK's), 0x0100 (dimmer),
 * 0x0c00 (brighter), back to 0x0639. Writes: DSI0 VM_CMD registers and the
 * START bit only; VM_CMD_CON is put back to 0x21 at the end.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define DSI		0x1401a000UL
#define DSI_START	0x00
#define DSI_INTSTA	0x0c
#define VM_CMD_CON	0x200
#define VM_CMD_DATA0	0x208
#define VM_CMD_START	(1u << 16)
#define VM_CMD_DONE	(1u << 5)
#define VM_CMD_EN	(1u << 0)
#define LONG_PKT	(1u << 1)
#define TS_VFP_EN	(1u << 5)

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(DSI + off); }
static inline void wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(DSI + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

/* DCS 0x51 <hi> <lo>; returns how many 10 us polls VM_CMD_DONE took (or ~0u). */
static uint32_t set_brightness(uint32_t level)
{
	wr(VM_CMD_DATA0, 0x51u | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16);
	wr(VM_CMD_CON, 0x39u << 8 | 3u << 16 | LONG_PKT | VM_CMD_EN | TS_VFP_EN);
	wr(DSI_INTSTA, rd(DSI_INTSTA) & ~VM_CMD_DONE);
	wr(DSI_START, rd(DSI_START) & ~VM_CMD_START);
	wr(DSI_START, rd(DSI_START) | VM_CMD_START);
	uint32_t n = ~0u;
	for (uint32_t i = 0; i < 10000; i++) {		/* ~100 ms: several frames */
		if (rd(DSI_INTSTA) & VM_CMD_DONE) {
			n = i;
			break;
		}
		udelay(10);
	}
	wr(DSI_START, rd(DSI_START) & ~VM_CMD_START);
	wr(DSI_INTSTA, rd(DSI_INTSTA) & ~VM_CMD_DONE);
	return n;
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tbl v1: brightness via DSI VM_CMD (DCS 0x51)");
	con_hex2("BEFORE", "start=", rd(DSI_START), "intsta=", rd(DSI_INTSTA));
	con_hex2("BEFORE", "vm_cmd_con=", rd(VM_CMD_CON), "mode=", rd(0x14));
	if ((rd(0x14) & 3) == 0) {
		con_str("FAIL", "DSI not in video mode: nothing sent");
		goto out;
	}
	con_str("WATCH", "dim, LK level, dimmer, brighter, LK level");
	con_skip();
	static const uint16_t levels[] = { 0x0200, 0x0639, 0x0100, 0x0c00, 0x0639 };
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		uint32_t n = set_brightness(levels[i]);
		con_hex2("SET", "level=", levels[i], "done after polls=", n);
		udelay(2000000);
	}
	wr(VM_CMD_CON, 0x21);
	con_hex2("AFTER", "start=", rd(DSI_START), "intsta=", rd(DSI_INTSTA));
out:
#ifdef LAPIS_DUAL_PROBE
	return;			/* shim.c: Power reboots to Android */
#endif
	con_str("END", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_report(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	con_skip();
	con_hex("EXC slot", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	for (;;)
		__asm__ volatile("wfe");
}
