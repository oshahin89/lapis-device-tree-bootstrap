/*
 * lapis DSI0 register dump (shim alternate main, build.sh tdsi). Read-only.
 *
 * Brightness on lapis (OLED p16_36_02_0b_dsc_vdo, video mode, led-mode 4)
 * is a DCS long write 0x51 <level hi> <level lo> (14 bit, levels 1..14 ->
 * 15; panel-p16-36-02-0b-dsc-vdo.ko lcm_setbacklight_cmdq). It has to go out
 * on DSI0 (0x1401a000) while LK's video mode runs. Stock mediatek-drm.ko
 * mt6878_dsi_driver_data: CMDQ0 0xd00, CMDQ1 0xd04, VM_CMD_CON 0x200,
 * VM_CMD_DATA0/10/20/30 0x208/0x218/0x228/0x238, 0xc00, then 0x100 (VM
 * command start bit?), 0x20, 0x20, 0xe, 0xf, ... This dumps what LK left
 * there (upstream mtk_dsi.c names for 0x00..0x80), twice 1 s apart, so the
 * VM_CMD path can be written against real values.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define DSI	0x1401a000UL

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(DSI + off); }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static void pair(const char *label, uint32_t a, uint32_t b)
{
	char la[8] = "+000=", lb[8] = " +000=";
	static const char hx[] = "0123456789abcdef";
	la[1] = hx[(a >> 8) & 0xf]; la[2] = hx[(a >> 4) & 0xf]; la[3] = hx[a & 0xf];
	lb[2] = hx[(b >> 8) & 0xf]; lb[3] = hx[(b >> 4) & 0xf]; lb[4] = hx[b & 0xf];
	con_hex2(label, la, rd(a), lb, rd(b));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "tdsi v1: DSI0 0x1401a000 registers (read-only)");
	/* upstream: START, INTEN, INTSTA, CON_CTRL, MODE_CTRL, TXRX_CTRL, ... */
	pair("CTRL", 0x00, 0x08);
	pair("CTRL", 0x0c, 0x10);
	pair("CTRL", 0x14, 0x18);
	pair("CTRL", 0x1c, 0x20);
	pair("TIMING", 0x24, 0x28);
	pair("TIMING", 0x2c, 0x30);
	pair("TIMING", 0x50, 0x54);
	pair("TIMING", 0x58, 0x5c);
	pair("CMDQ", 0x60, 0x64);
	pair("STATE", 0x74, 0x84);
	pair("STATE", 0x88, 0x8c);
	pair("PHY", 0x104, 0x108);
	pair("VMCMD", 0x200, 0x204);
	pair("VMCMD", 0x208, 0x20c);
	pair("VMCMD", 0x210, 0x218);
	pair("SHADOW", 0xc00, 0xc04);
	pair("CMDQ0", 0xd00, 0xd04);
	pair("CMDQ0", 0xd08, 0xd0c);
	con_skip();
	udelay(1000000);
	con_str("AGAIN", "1 s later (moving counters/status):");
	pair("CTRL", 0x00, 0x08);
	pair("CTRL", 0x0c, 0x14);
	pair("STATE", 0x74, 0x84);
	pair("STATE", 0x88, 0x8c);
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
