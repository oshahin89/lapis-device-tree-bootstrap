/*
 * Lapis NT38771 touch driver.
 *
 * Talks to the NT38771 through the SPI3 controller (PIO, 9.45 MHz), as
 * Android does; the earlier GPIO bit-bang made the running firmware answer
 * 0xFE after ~100 ms. GPIO90 (shared panel/touch reset) is never touched.
 * Enable with lapisnt=1.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "lapis_nt38771_fw_data.h"
#include "nt38771_pipeline.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);

void sweet_nt38771_init(void);

/* SPI3 controller (mediatek,mt6989-spi), driven like the stock spi-mt65xx
 * PIO path; proven on the phone by tools/lapis_shim/tspi.c. */
#define NVT_GPIO_PA             0x10005000u
#define NVT_GPIO_MODE           0x380u   /* pins 64..71, 4 bits each */
#define NVT_TOPCK_PA            0x10000000u
#define NVT_PERAO_PA            0x11036000u
#define NVT_SPI_PA              0x11013000u
#define TOPCK_CFG_UPDATE        0x004u
#define TOPCK_CFG_4             0x050u
#define TOPCK_CFG_4_SET         0x054u
#define TOPCK_CFG_4_CLR         0x058u
#define PERAO_CG0_STA           0x010u
#define PERAO_CG0_CLR           0x028u
#define SPI_CFG1                0x04u
#define SPI_TX_DATA             0x10u
#define SPI_RX_DATA             0x14u
#define SPI_CMD                 0x18u
#define SPI_STATUS1             0x20u
#define SPI_PAD_SEL             0x24u
#define SPI_CFG2                0x28u
#define SPI_CFG3                0x40u
#define SPI_CFG5                0x68u
#define CMD_ACT                 (1u << 0)
#define CMD_RST                 (1u << 2)
#define CMD_DEASSERT            (1u << 5)
#define CMD_SAMPLE_SEL          (1u << 6)
#define CMD_CS_POL              (1u << 7)
#define CMD_CPHA                (1u << 8)
#define CMD_CPOL                (1u << 9)
#define CMD_RX_DMA              (1u << 10)
#define CMD_TX_DMA              (1u << 11)
#define CMD_TXMSBF              (1u << 12)
#define CMD_RXMSBF              (1u << 13)
#define CMD_RX_ENDIAN           (1u << 14)
#define CMD_TX_ENDIAN           (1u << 15)
#define CMD_FINISH_IE           (1u << 16)
#define CMD_PAUSE_IE            (1u << 17)
#define CMD_CS_PIN_SEL          (1u << 18)
#define CMD_NONIDLE             (1u << 19)
#define CMD_LOOP                (1u << 21)
#define CMD_TICKDLY_MSK         (7u << 22)
#define SPI_TICK_DLY            2u       /* FDT mediatek,tickdly */
#define SPI_SCK_TICKS           11u      /* 208 MHz / 22 = 9.45 MHz */
#define SPI_FIFO                32u

#define NVT_CHIP_ID             0x3f004u
#define NVT_RESET               0x3f0feu
#define NVT_RESX_DECOUPLE       0x3f8d4u
#define NVT_BOOT_READY           0x3f102u
#define NVT_EVENT_BUF            0x2b400u
#define NVT_FW_INFO              (NVT_EVENT_BUF + 0x78u)
#define NVT_XFER_MAX             (15u * 1024u)
#define NVT_REPORT_BYTES         LAPIS_NVT_REPORT_BYTES
/* Reset-complete states (Novatek RESET_STATE_INIT .. RESET_STATE_MAX). */
#define NVT_STATE_INIT           0xa0u
#define NVT_STATE_MAX            0xafu
#define NVT_REPORT_PERIOD_MS     10u
#define NVT_BAD_FRAME_LIMIT      50u

/* The iPhone X display (1125x2436) that SWEET's AppleCLCD presents, scaled
 * to the whole 1280x2772 panel (lapis patch 0086, loader passes the panel). */
#define NVT_VIEW_WIDTH           1125u
#define NVT_VIEW_HEIGHT          2436u

struct nvt_partition {
	uint32_t bin, sram, size, crc;
};

static struct {
	vm_offset_t gpio, spi, topck, perao;
	bool started;
	unsigned spi_timeouts;
	struct lapis_nvt_touch_state state;
	uint16_t hid_slots;
	uint16_t hid_x[LAPIS_NVT_CONTACTS], hid_y[LAPIS_NVT_CONTACTS];
	uint8_t report[NVT_REPORT_BYTES];
	uint8_t tx[NVT_XFER_MAX + 1u];
	uint8_t rx[NVT_XFER_MAX + 2u];
} nvt;

static int nvt_sleep_chan;

static uint32_t
nvt_mmio_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
nvt_mmio_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

#define spi_rd(off)      nvt_mmio_rd(nvt.spi, (off))
#define spi_wr(off, v)   nvt_mmio_wr(nvt.spi, (off), (v))

static void
nvt_wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&nvt_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* mtk_spi_prepare_message: mode 0, MSB first, active-low software CS. */
static void
nvt_spi_prepare(void)
{
	uint32_t c = spi_rd(SPI_CMD);
	c |= CMD_NONIDLE | CMD_CS_PIN_SEL | CMD_TXMSBF | CMD_RXMSBF;
	c &= ~(CMD_LOOP | CMD_CPHA | CMD_CPOL | CMD_RX_ENDIAN | CMD_TX_ENDIAN |
	    CMD_CS_POL | CMD_SAMPLE_SEL | CMD_TX_DMA | CMD_RX_DMA |
	    CMD_DEASSERT | CMD_FINISH_IE | CMD_PAUSE_IE | CMD_TICKDLY_MSK);
	c |= SPI_TICK_DLY << 22;
	spi_wr(SPI_CMD, c);
	spi_wr(SPI_PAD_SEL, 0);
	spi_wr(SPI_CFG2, ((SPI_SCK_TICKS - 1u) << 16) | (SPI_SCK_TICKS - 1u));
	spi_wr(SPI_CFG3, 0);    /* full duplex, single bit */
}

/* sw_cs: CS_POL set = asserted; release = clear + controller reset. */
static void
nvt_spi_cs(bool assert)
{
	uint32_t c = spi_rd(SPI_CMD);
	if (assert) {
		spi_wr(SPI_CMD, c | CMD_CS_POL);
		return;
	}
	spi_wr(SPI_CMD, c & ~CMD_CS_POL);
	spi_wr(SPI_CMD, spi_rd(SPI_CMD) | CMD_RST);
	spi_wr(SPI_CMD, spi_rd(SPI_CMD) & ~(CMD_RST | CMD_CS_PIN_SEL));
}

/* One FIFO packet (<= 32 bytes), full duplex, polled. */
static void
nvt_spi_packet(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	uint32_t cfg1 = spi_rd(SPI_CFG1) & ~0xffffff00u;
	spi_wr(SPI_CFG1, cfg1 | ((n - 1u) << 16));
	spi_wr(SPI_CFG5, 0);
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && i + k < n; k++) {
			w |= (uint32_t)tx[i + k] << (8 * k);
		}
		spi_wr(SPI_TX_DATA, w);
	}
	spi_wr(SPI_CMD, spi_rd(SPI_CMD) | CMD_ACT);
	uint64_t start = mach_absolute_time(), limit;
	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &limit);
	while (spi_rd(SPI_STATUS1) == 0) {
		if (mach_absolute_time() - start > limit) {
			nvt.spi_timeouts++;
			break;
		}
	}
	for (unsigned i = 0; i < n; i += 4) {
		uint32_t w = spi_rd(SPI_RX_DATA);
		for (unsigned k = 0; k < 4 && i + k < n; k++) {
			if (rx != NULL) {
				rx[i + k] = (uint8_t)(w >> (8 * k));
			}
		}
	}
}

/* One CS assertion for the whole message, in 32-byte packets. */
static void
nvt_message(const uint8_t *tx, uint8_t *rx, unsigned len)
{
	nvt_spi_prepare();
	nvt_spi_cs(true);
	for (unsigned pos = 0; pos < len; pos += SPI_FIFO) {
		unsigned n = len - pos > SPI_FIFO ? SPI_FIFO : len - pos;
		nvt_spi_packet(tx + pos, rx != NULL ? rx + pos : NULL, n);
	}
	nvt_spi_cs(false);
}

static void
nvt_set_page(uint32_t address)
{
	uint8_t page[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) };
	nvt_message(page, NULL, sizeof(page));
}

static bool
nvt_read(uint32_t address, uint8_t *data, unsigned len)
{
	if (len == 0 || len > NVT_XFER_MAX - 2u || data == NULL) {
		return false;
	}
	nvt_set_page(address);
	memset(nvt.tx, 0, len + 2u);
	nvt.tx[0] = (uint8_t)(address & 0x7fu);
	nvt_message(nvt.tx, nvt.rx, len + 2u);
	memcpy(data, nvt.rx + 2, len);
	return true;
}

static bool
nvt_write(uint32_t address, const uint8_t *data, unsigned len)
{
	if (len == 0 || len > NVT_XFER_MAX || data == NULL) {
		return false;
	}
	nvt_set_page(address);
	nvt.tx[0] = (uint8_t)((address & 0x7fu) | 0x80u);
	memcpy(nvt.tx + 1, data, len);
	nvt_message(nvt.tx, NULL, len + 1u);
	return true;
}

static bool
nvt_write_u8(uint32_t address, uint8_t value)
{
	return nvt_write(address, &value, 1);
}

static uint32_t
nvt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool
nvt_partition_table(struct nvt_partition *parts, unsigned *count)
{
	const uint8_t *fw = lapis_nt38771_fw;
	const size_t fw_size = sizeof(lapis_nt38771_fw);
	uint32_t end = nvt_le32(fw);
	unsigned info_count = 0;
	uint32_t pos;

	if (fw_size != 196608 || (unsigned)fw[0x2f000] + fw[0x2f001] != 0xffu ||
	    end < 0x30 || end > fw_size || (fw[0x20] & 0x02u) || (fw[0x28] & 0x10u)) {
		return false;
	}
	for (pos = 0x30; pos + 16u <= end; pos += 16u) {
		if (nvt_le32(fw + pos + 4u) != 0 && nvt_le32(fw + pos + 8u) == 0) {
			end = pos + 16u;
			break;
		}
	}
	if (pos > end || end > fw_size) {
		return false;
	}
	info_count = (end - 0x30u) / 16u;
	if (info_count == 0 || info_count > 16u) {
		return false;
	}
	parts[0] = (struct nvt_partition){ nvt_le32(fw), nvt_le32(fw + 4),
	    nvt_le32(fw + 8), nvt_le32(fw + 0x18) };
	parts[1] = (struct nvt_partition){ nvt_le32(fw + 12), nvt_le32(fw + 16),
	    nvt_le32(fw + 20), nvt_le32(fw + 0x1c) };
	for (unsigned i = 0; i < info_count; ++i) {
		const uint8_t *p = fw + 0x30u + i * 16u;
		parts[2u + i] = (struct nvt_partition){ nvt_le32(p + 8), nvt_le32(p),
		    nvt_le32(p + 4), nvt_le32(p + 12) };
	}
	*count = 2u + info_count;
	for (unsigned i = 0; i < *count; ++i) {
		const struct nvt_partition *p = &parts[i];
		uint64_t bytes = p->size ? (uint64_t)p->size + 1u : 0;
		if ((uint64_t)p->bin + bytes > fw_size || p->sram > 0xffffffu ||
		    (uint64_t)p->sram + bytes > 0x1000000u) {
			return false;
		}
	}
	return true;
}

static bool
nvt_write_partition(const struct nvt_partition *p)
{
	const uint8_t *fw = lapis_nt38771_fw;
	uint32_t remaining = p->size + 1u;
	uint32_t bin = p->bin, sram = p->sram;
	if (p->size == 0) {
		return true;
	}
	while (remaining != 0) {
		unsigned chunk = remaining > NVT_XFER_MAX ? NVT_XFER_MAX : remaining;
		/* nvt_write adds the SPI write bit (0x80) to the SRAM offset. */
		if (!nvt_write(sram, fw + bin, chunk)) {
			return false;
		}
		remaining -= chunk;
		bin += chunk;
		sram += chunk;
	}
	return true;
}

static bool
nvt_crc_bank(uint32_t des, uint32_t length, uint32_t golden,
	uint32_t sram, uint32_t size, uint32_t crc)
{
	uint8_t b[4];
	nvt_set_page(des);
	b[0] = (uint8_t)sram; b[1] = (uint8_t)(sram >> 8); b[2] = (uint8_t)(sram >> 16);
	if (!nvt_write(des, b, 3)) return false;
	b[0] = (uint8_t)size; b[1] = (uint8_t)(size >> 8); b[2] = (uint8_t)(size >> 16);
	if (!nvt_write(length, b, 3)) return false;
	b[0] = (uint8_t)crc; b[1] = (uint8_t)(crc >> 8);
	b[2] = (uint8_t)(crc >> 16); b[3] = (uint8_t)(crc >> 24);
	return nvt_write(golden, b, sizeof(b));
}

static bool
nvt_boot_firmware(void)
{
	struct nvt_partition parts[18];
	unsigned count = 0;
	uint8_t id[6] = { 0 }, ver[2], decouple[2], check[2];
	uint8_t status = 0, running = 0;
	bool already_running;

	/*
	 * Android's probe (nvt_ts_check_chip_ver_trim_loop, kprobe capture
	 * 2026-11-22) runs before every cold load: bootloader reset 0x69, 5 ms,
	 * write six zero bytes at 0x3F004 (chip_ver_trim), then read the ID.
	 * Our earlier driver only read the ID; the rest of its sequence already
	 * matched Android byte for byte, yet the firmware never booted.
	 */
	if (!nvt_write_u8(NVT_RESET, 0x69)) {
		printf("LAPIS NVT: chip-check reset write failed\n");
		return false;
	}
	delay(5000);
	memset(id, 0, sizeof(id));
	if (!nvt_write(NVT_CHIP_ID, id, sizeof(id))) {
		printf("LAPIS NVT: chip-check zero write failed\n");
		return false;
	}
	if (!nvt_read(NVT_CHIP_ID, id, sizeof(id)) ||
	    (id[3] != 0x72 && id[3] != 0x71) || id[4] != 0x87 || id[5] != 0x03) {
		printf("LAPIS NVT: chip ID check failed (%02x %02x %02x)\n",
		    id[3], id[4], id[5]);
		return false;
	}
	ver[0] = ver[1] = 0;
	if (!nvt_read(NVT_FW_INFO, ver, sizeof(ver))) {
		printf("LAPIS NVT: firmware-version read failed\n");
		return false;
	}
	already_running = ((unsigned)ver[0] + ver[1] == 0xffu &&
	    nvt_read(NVT_EVENT_BUF + 0x60u, &running, 1) && running >= NVT_STATE_INIT && running <= NVT_STATE_MAX);
	if (already_running) {
		printf("LAPIS NVT: firmware already active (fwinfo=%02x%02x state=%02x)\n",
		    ver[0], ver[1], running);
		return true;
	}
	printf("LAPIS NVT: chip=%02x%02x%02x fwinfo-before=%02x%02x\n",
	    id[3], id[4], id[5], ver[0], ver[1]);
	/* Android loads ~1 s after the chip check (probe -> Boot_Update_Firmware). */
	nvt_wait_ms(1000);
	if (!nvt_partition_table(parts, &count)) {
		printf("LAPIS NVT: firmware table rejected\n");
		return false;
	}
	printf("LAPIS NVT: firmware table accepted (%u partitions)\n", count);
	/*
	 * Bootloader reset. Android's shipped nt38771.ko (HyperOS 4) only writes
	 * 0x69 to swrst_sif and waits 5 ms: it has no RESX-decouple helper, and
	 * its full ILM load boots to 0xA0 that way (kprobe capture 2026-11-22).
	 * Novatek's GPL revision adds an AA55 RESX decouple at 0x3F8D4; with it
	 * our firmware never left the bootloader. Keep it as an experiment only.
	 */
	if (!nvt_write_u8(NVT_RESET, 0x69)) {
		printf("LAPIS NVT: bootloader reset write failed\n");
		return false;
	}
	{
		int resx = 0;
		(void)PE_parse_boot_argn("lapisnt_resx", &resx, sizeof(resx));
		if (resx) {
			delay(500);
			decouple[0] = 0xaa; decouple[1] = 0x55;
			if (!nvt_write(NVT_RESX_DECOUPLE, decouple, sizeof(decouple)) ||
			    !nvt_read(NVT_RESX_DECOUPLE, check, sizeof(check)) ||
			    check[0] != decouple[0] || check[1] != decouple[1]) {
				printf("LAPIS NVT: RESX decouple failed (read %02x%02x)\n",
				    check[0], check[1]);
				return false;
			}
			printf("LAPIS NVT: bootloader reset + RESX decouple (lapisnt_resx=1)\n");
		} else {
			printf("LAPIS NVT: bootloader reset (plain, as Android)\n");
		}
	}
	delay(5000);
	if (!nvt_crc_bank(0x3f7b4u, 0x3f7b8u, 0x3f7bcu,
	    parts[0].sram, parts[0].size, parts[0].crc)) {
		printf("LAPIS NVT: ILM CRC bank setup failed\n");
		return false;
	}
	if (!nvt_crc_bank(0x3fb84u, 0x3fb88u, 0x3fb8cu,
	    parts[1].sram, parts[1].size, parts[1].crc)) {
		printf("LAPIS NVT: DLM CRC bank setup failed\n");
		return false;
	}
	printf("LAPIS NVT: ILM/DLM CRC banks programmed\n");
	for (unsigned i = 0; i < count; ++i) {
		if (!nvt_write_partition(&parts[i])) {
			printf("LAPIS NVT: firmware partition %u failed (sram=%06x size=%u)\n",
			    i, parts[i].sram, parts[i].size + 1u);
			return false;
		}
	}
	printf("LAPIS NVT: firmware partitions transferred (%u)\n", count);
	if (!nvt_write_u8(NVT_EVENT_BUF + 0x60u, 0)) {
		printf("LAPIS NVT: reset-state clear failed\n");
		return false;
	}
	if (!nvt_write_u8(NVT_EVENT_BUF + 0x50u, 0xae)) {
		printf("LAPIS NVT: FW CRC enable command failed\n");
		return false;
	}
	if (!nvt_write_u8(NVT_BOOT_READY, 1)) {
		printf("LAPIS NVT: BOOT_RDY write failed\n");
		return false;
	}
	printf("LAPIS NVT: BOOT_RDY sent; waiting for reset state 0xa0\n");
	delay(5000);
	for (unsigned i = 0; i < 100; ++i) {
		if (nvt_read(NVT_EVENT_BUF + 0x60u, &status, 1) &&
		    status >= NVT_STATE_INIT && status <= NVT_STATE_MAX) {
			if (!nvt_read(NVT_FW_INFO, ver, sizeof(ver)) ||
			    (unsigned)ver[0] + ver[1] != 0xffu) {
				printf("LAPIS NVT: state ready but fwinfo invalid (%02x%02x)\n",
				    ver[0], ver[1]);
				return false;
			}
			printf("LAPIS NVT: firmware loaded (version=%02x%02x state=%02x)\n",
			    ver[0], ver[1], status);
			return true;
		}
		delay(10000);
	}
	if (!nvt_read(NVT_FW_INFO, ver, sizeof(ver))) ver[0] = ver[1] = 0xff;
	if (!nvt_read(NVT_CHIP_ID, id, sizeof(id))) id[3] = id[4] = id[5] = 0xff;
	printf("LAPIS NVT: firmware boot timeout state=%02x fwinfo=%02x%02x chip=%02x%02x%02x\n",
	    status, ver[0], ver[1], id[3], id[4], id[5]);
	{
		uint8_t ilm_head[4] = { 0 }, dlm_head[4] = { 0 };
		uint8_t ilm_crc[4] = { 0 }, dlm_crc[4] = { 0 };
		uint8_t boot_ready = 0, host_cmd = 0;
		uint8_t crc_flags[2] = { 0 }, reset_bytes[6] = { 0 };
		(void)nvt_read(parts[0].sram, ilm_head, sizeof(ilm_head));
		(void)nvt_read(parts[1].sram, dlm_head, sizeof(dlm_head));
		(void)nvt_read(0x3fbbcu, ilm_crc, sizeof(ilm_crc));
		(void)nvt_read(0x3fbc0u, dlm_crc, sizeof(dlm_crc));
		(void)nvt_read(0x3f127u, crc_flags, sizeof(crc_flags));
		(void)nvt_read(NVT_EVENT_BUF + 0x60u, reset_bytes, sizeof(reset_bytes));
		(void)nvt_read(NVT_BOOT_READY, &boot_ready, 1);
		(void)nvt_read(NVT_EVENT_BUF + 0x50u, &host_cmd, 1);
		printf("LAPIS NVT: verify ILM=%08x/%08x CRC=%08x/%08x DLM=%08x/%08x CRC=%08x/%08x\n",
		    nvt_le32(ilm_head), nvt_le32(lapis_nt38771_fw + parts[0].bin),
		    nvt_le32(ilm_crc), parts[0].crc,
		    nvt_le32(dlm_head), nvt_le32(lapis_nt38771_fw + parts[1].bin),
		    nvt_le32(dlm_crc), parts[1].crc);
		printf("LAPIS NVT: control readback BOOT_RDY=%02x HOST_CMD=%02x\n",
		    boot_ready, host_cmd);
		printf("LAPIS NVT: boot flags CRC=%02x%02x RESET=%02x%02x%02x%02x%02x%02x FWINFO=%02x%02x\n",
		    crc_flags[0], crc_flags[1], reset_bytes[0], reset_bytes[1],
		    reset_bytes[2], reset_bytes[3], reset_bytes[4], reset_bytes[5],
		    ver[0], ver[1]);
	}
	return false;
}

/* Poll-loop counters, printed every ~3 s while they change (v12 diagnostics). */
static struct {
	unsigned polls, rd_fail, bad_sum, ok, emits;
} nvt_diag;

static void
nvt_emit(void *context, uint8_t slot, int down, uint16_t x, uint16_t y)
{
	uint16_t bit = (uint16_t)(1u << slot);
	static unsigned logged;
	(void)context;
	nvt_diag.emits++;
	if (logged < 40) {
		logged++;
		printf("LAPIS NVT: emit slot=%u down=%d native=%u,%u\n",
		    slot, down, x, y);
	}
	if (down) {
		/* Panel pixel -> the logical pixel shown there (same maps as
		 * AppleCLCD's scaled present: floor(p * logical / panel)). */
		if (x >= LAPIS_NVT_DISPLAY_WIDTH) {
			x = LAPIS_NVT_DISPLAY_WIDTH - 1u;
		}
		if (y >= LAPIS_NVT_DISPLAY_HEIGHT) {
			y = LAPIS_NVT_DISPLAY_HEIGHT - 1u;
		}
		nvt.hid_x[slot] = (uint16_t)((uint32_t)x * NVT_VIEW_WIDTH / LAPIS_NVT_DISPLAY_WIDTH);
		nvt.hid_y[slot] = (uint16_t)((uint32_t)y * NVT_VIEW_HEIGHT / LAPIS_NVT_DISPLAY_HEIGHT);
		nvt.hid_slots |= bit;
		sweet_hid_touch(slot, 1, nvt.hid_x[slot], nvt.hid_y[slot]);
	} else if (nvt.hid_slots & bit) {
		sweet_hid_touch(slot, 0, nvt.hid_x[slot], nvt.hid_y[slot]);
		nvt.hid_slots &= (uint16_t)~bit;
	}
}

static void
nvt_release_all(void)
{
	for (unsigned i = 0; i < LAPIS_NVT_CONTACTS; ++i) {
		if (nvt.hid_slots & (1u << i)) {
			sweet_hid_touch((int)i, 0, nvt.hid_x[i], nvt.hid_y[i]);
		}
	}
	nvt.hid_slots = 0;
	lapis_nvt_touch_state_init(&nvt.state);
}

static bool
nvt_spi_init(void)
{
	uint32_t cfg4, cg0, mode;
	nvt.gpio = ml_io_map(NVT_GPIO_PA, 0x1000);
	nvt.topck = ml_io_map(NVT_TOPCK_PA, 0x1000);
	nvt.perao = ml_io_map(NVT_PERAO_PA, 0x1000);
	nvt.spi = ml_io_map(NVT_SPI_PA, 0x1000);
	if (!nvt.gpio || !nvt.topck || !nvt.perao || !nvt.spi) {
		return false;
	}
	/* LK already leaves these open; make sure: spi3_sel = univpll/12, pdn off,
	 * pericfg_ao spi3 gate open. */
	cfg4 = nvt_mmio_rd(nvt.topck, TOPCK_CFG_4);
	if (((cfg4 >> 8) & 0x87u) != 0x01u) {
		nvt_mmio_wr(nvt.topck, TOPCK_CFG_4_CLR, (7u << 8) | (1u << 15));
		nvt_mmio_wr(nvt.topck, TOPCK_CFG_4_SET, 1u << 8);
		nvt_mmio_wr(nvt.topck, TOPCK_CFG_UPDATE, 1u << 17);
		delay(100);
	}
	nvt_mmio_wr(nvt.perao, PERAO_CG0_CLR, 1u << 15);
	delay(100);
	cfg4 = nvt_mmio_rd(nvt.topck, TOPCK_CFG_4);
	cg0 = nvt_mmio_rd(nvt.perao, PERAO_CG0_STA);
	if (((cfg4 >> 8) & 0x87u) != 0x01u || (cg0 & (1u << 15))) {
		printf("LAPIS NVT: SPI3 clock closed (cfg4=%08x cg0=%08x)\n", cfg4, cg0);
		return false;
	}
	/* GPIO 68..71 -> function 2 (SPI3); GPIO90 is never touched. */
	mode = nvt_mmio_rd(nvt.gpio, NVT_GPIO_MODE);
	nvt_mmio_wr(nvt.gpio, NVT_GPIO_MODE, (mode & 0x0000ffffu) | 0x22220000u);
	spi_wr(SPI_CMD, spi_rd(SPI_CMD) | CMD_RST);
	spi_wr(SPI_CMD, spi_rd(SPI_CMD) & ~(CMD_RST | CMD_CS_PIN_SEL));
	printf("LAPIS NVT: SPI3 controller up (cfg4=%08x mode %08x->%08x)\n",
	    cfg4, mode, nvt_mmio_rd(nvt.gpio, NVT_GPIO_MODE));
	return true;
}

static void
nvt_thread(void *arg, wait_result_t wr)
{
	unsigned bad = 0;
	(void)arg; (void)wr;
	if (!nvt_spi_init() || !nvt_boot_firmware()) goto out;
	lapis_nvt_touch_state_init(&nvt.state);
	printf("LAPIS NVT: touch polling started (%ux%u viewport)\n",
	    NVT_VIEW_WIDTH, NVT_VIEW_HEIGHT);
	unsigned last_emits = ~0u, prints = 0;
	for (;;) {
		bool rd = nvt_read(NVT_EVENT_BUF, nvt.report, NVT_REPORT_BYTES);
		int res = rd ? lapis_nvt_process_frame(&nvt.state, nvt.report,
		    LAPIS_NVT_REPORT_BYTES, nvt_emit, NULL) : LAPIS_NVT_PARSE_BAD_ARGUMENT;
		nvt_diag.polls++;
		if (!rd) {
			nvt_diag.rd_fail++;
		} else if (res == LAPIS_NVT_PARSE_BAD_CHECKSUM) {
			nvt_diag.bad_sum++;
		} else if (res == LAPIS_NVT_PARSE_OK) {
			nvt_diag.ok++;
		}
		if (res != LAPIS_NVT_PARSE_OK) {
			if (++bad >= NVT_BAD_FRAME_LIMIT) {
				nvt_release_all();
				bad = 0;
			}
		} else {
			bad = 0;
		}
		/* every ~3 s: always the first 5 times, later only when touches came */
		if (nvt_diag.polls % 300u == 0 && prints < 200 &&
		    (prints < 5 || nvt_diag.emits != last_emits)) {
			prints++;
			last_emits = nvt_diag.emits;
			printf("LAPIS NVT: polls=%u rdfail=%u badsum=%u ok=%u emits=%u "
			    "frame=%02x %02x%02x %02x%02x %02x | %02x sum@64=%02x\n",
			    nvt_diag.polls, nvt_diag.rd_fail, nvt_diag.bad_sum,
			    nvt_diag.ok, nvt_diag.emits,
			    nvt.report[0], nvt.report[1], nvt.report[2],
			    nvt.report[3], nvt.report[4], nvt.report[5],
			    nvt.report[6], nvt.report[64]);
		}
		nvt_wait_ms(NVT_REPORT_PERIOD_MS);
	}
out:
	nvt_release_all();
	printf("LAPIS NVT: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_nt38771_init(void)
{
	int enabled = 0;
	thread_t thread;
	kern_return_t kr;
	if (nvt.started) return;
	nvt.started = true;
	(void)PE_parse_boot_argn("lapisnt", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS NVT: disabled (use lapisnt=1)\n");
		return;
	}
	kr = kernel_thread_start(nvt_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("LAPIS NVT: thread start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
