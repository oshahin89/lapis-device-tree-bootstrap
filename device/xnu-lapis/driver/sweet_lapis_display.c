/*
 * Lapis OLED brightness: DCS 0x51 <hi> <lo> (14 bit) on DSI0 (0x1401a000)
 * while LK's video mode keeps running.
 *
 * Proven from the boot shim (tools/lapis_shim/tbl.c, 2026-11-22): LK leaves
 * MODE_CTRL 0x14 = 2 (video) and VM_CMD_CON 0x200 = 0x21; a long packet goes
 * out in the vertical front porch: VM_CMD_DATA0 0x208 = bytes, VM_CMD_CON =
 * 0x39 << 8 | len << 16 | LONG_PKT | VM_CMD_EN | TS_VFP_EN, clear INTSTA
 * bit 5, START bit 16 0 -> 1, wait INTSTA bit 5 (~3 ms), START bit 16 -> 0,
 * VM_CMD_CON back to 0x21. LK's own level is 0x639.
 *
 * SweetBacklight (iokit/Kernel/arm/SweetBattery.cpp, lapis patch 0089) maps
 * backboardd's brightness to a level and calls lapis_brightness_set().
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define DSP_DSI0_PA         0x1401a000UL
#define DSP_START           0x00
#define DSP_INTSTA          0x0c
#define DSP_MODE_CTRL       0x14
#define DSP_VM_CMD_CON      0x200
#define DSP_VM_CMD_DATA0    0x208
#define DSP_VM_CMD_START    (1u << 16)
#define DSP_VM_CMD_DONE     (1u << 5)
#define DSP_LONG_PKT        (1u << 1)
#define DSP_VM_CMD_EN       (1u << 0)
#define DSP_TS_VFP_EN       (1u << 5)
#define DSP_LEVEL_MAX       0x3fffu

int lapis_brightness_set(unsigned level);

static vm_offset_t dsp_dsi;
static lck_grp_t *dsp_grp;
static lck_mtx_t *dsp_lock;
static bool dsp_ready, dsp_tried;
static unsigned dsp_logged;

static uint32_t
dsp_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(dsp_dsi + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
dsp_wr(uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(dsp_dsi + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
dsp_init(void)
{
	int enabled = 1;
	if (dsp_tried) {
		return dsp_ready;
	}
	dsp_tried = true;
	(void)PE_parse_boot_argn("lapisbacklight", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS BACKLIGHT: disabled (lapisbacklight=0)\n");
		return false;
	}
	dsp_dsi = ml_io_map(DSP_DSI0_PA, 0x1000);
	dsp_grp = lck_grp_alloc_init("lapis_backlight", LCK_GRP_ATTR_NULL);
	dsp_lock = lck_mtx_alloc_init(dsp_grp, LCK_ATTR_NULL);
	if (!dsp_dsi || !dsp_lock) {
		printf("LAPIS BACKLIGHT: setup failed\n");
		return false;
	}
	uint32_t mode = dsp_rd(DSP_MODE_CTRL);
	if ((mode & 3u) == 0) {
		printf("LAPIS BACKLIGHT: DSI0 not in video mode (%#x)\n", mode);
		return false;
	}
	dsp_ready = true;
	printf("LAPIS BACKLIGHT: DSI0 video mode %#x, vm_cmd_con %#x\n", mode, dsp_rd(DSP_VM_CMD_CON));
	return true;
}

int
lapis_brightness_set(unsigned level)
{
	if (!dsp_init()) {
		return -1;
	}
	if (level > DSP_LEVEL_MAX) {
		level = DSP_LEVEL_MAX;
	}
	lck_mtx_lock(dsp_lock);
	dsp_wr(DSP_VM_CMD_DATA0, 0x51u | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16);
	dsp_wr(DSP_VM_CMD_CON, 0x39u << 8 | 3u << 16 | DSP_LONG_PKT | DSP_VM_CMD_EN | DSP_TS_VFP_EN);
	dsp_wr(DSP_INTSTA, dsp_rd(DSP_INTSTA) & ~DSP_VM_CMD_DONE);
	dsp_wr(DSP_START, dsp_rd(DSP_START) & ~DSP_VM_CMD_START);
	dsp_wr(DSP_START, dsp_rd(DSP_START) | DSP_VM_CMD_START);
	bool done = false;
	for (unsigned i = 0; i < 5000; i++) {   /* ~50 ms: several frames */
		if (dsp_rd(DSP_INTSTA) & DSP_VM_CMD_DONE) {
			done = true;
			break;
		}
		delay(10);
	}
	dsp_wr(DSP_START, dsp_rd(DSP_START) & ~DSP_VM_CMD_START);
	dsp_wr(DSP_INTSTA, dsp_rd(DSP_INTSTA) & ~DSP_VM_CMD_DONE);
	dsp_wr(DSP_VM_CMD_CON, 0x21);
	lck_mtx_unlock(dsp_lock);
	if (dsp_logged < 40) {
		dsp_logged++;
		printf("LAPIS BACKLIGHT: level %#x %s\n", level, done ? "sent" : "timeout");
	}
	return done ? 0 : -1;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
