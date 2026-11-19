/*
 * Lapis NT38771 touch driver.
 *
 * First implementation uses the already measured GPIO bit-bang path on
 * SPI3. It deliberately leaves GPIO90 alone (shared panel/touch reset) and
 * uses the controller's documented SPI reset register. Enable with lapisnt=1.
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

#define NVT_GPIO_PA             0x10005000u
#define NVT_GPIO_MODE           0x380u
#define NVT_GPIO_DIR            0x020u
#define NVT_GPIO_DO             0x120u
#define NVT_GPIO_DI             0x220u
#define NVT_CLK                 (1u << 4) /* GPIO68 */
#define NVT_CS                  (1u << 5) /* GPIO69 */
#define NVT_MOSI                (1u << 6) /* GPIO70 */
#define NVT_MISO                (1u << 7) /* GPIO71 */

#define NVT_CHIP_ID             0x3f004u
#define NVT_RESET               0x3f0feu
#define NVT_RESX_DECOUPLE       0x3f8d4u
#define NVT_BOOT_READY           0x3f102u
#define NVT_EVENT_BUF            0x31400u
#define NVT_FW_VERSION           0x2f000u
#define NVT_XFER_MAX             (15u * 1024u)
#define NVT_REPORT_BYTES         LAPIS_NVT_REPORT_BYTES
#define NVT_REPORT_PERIOD_MS     10u
#define NVT_BAD_FRAME_LIMIT      50u

/* iPhone X boot_args.Video window presented by the lapis loader. */
#define NVT_VIEW_WIDTH           1125u
#define NVT_VIEW_HEIGHT          2436u
#define NVT_VIEW_TOP             168u

struct nvt_partition {
	uint32_t bin, sram, size, crc;
};

static struct {
	vm_offset_t gpio;
	bool started;
	struct lapis_nvt_touch_state state;
	uint16_t hid_slots;
	uint16_t hid_x[LAPIS_NVT_CONTACTS], hid_y[LAPIS_NVT_CONTACTS];
	uint8_t report[NVT_REPORT_BYTES];
	uint8_t tx[NVT_XFER_MAX + 1u];
	uint8_t rx[NVT_XFER_MAX + 2u];
} nvt;

static int nvt_sleep_chan;

static uint32_t
nvt_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(nvt.gpio + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
nvt_wr(uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(nvt.gpio + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
nvt_wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&nvt_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
nvt_spi_delay(void)
{
	delay(2); /* 250 kHz conservative bit-bang clock */
}

static void
nvt_set_lines(uint32_t set, uint32_t clear)
{
	uint32_t v = nvt_rd(NVT_GPIO_DO);
	nvt_wr(NVT_GPIO_DO, (v & ~clear) | set);
	nvt_spi_delay();
}

static uint8_t
nvt_spi_byte(uint8_t out)
{
	uint8_t in = 0;
	for (int bit = 7; bit >= 0; --bit) {
		nvt_set_lines((out & (1u << bit)) ? NVT_MOSI : 0,
		    (out & (1u << bit)) ? 0 : NVT_MOSI);
		nvt_set_lines(NVT_CLK, 0);
		in = (uint8_t)((in << 1) | ((nvt_rd(NVT_GPIO_DI) & NVT_MISO) != 0));
		nvt_set_lines(0, NVT_CLK);
	}
	return in;
}

static void
nvt_message(const uint8_t *tx, uint8_t *rx, unsigned len)
{
	nvt_set_lines(0, NVT_CS);
	nvt_spi_delay();
	for (unsigned i = 0; i < len; ++i) {
		uint8_t byte = nvt_spi_byte(tx[i]);
		if (rx != NULL) {
			rx[i] = byte;
		}
	}
	nvt_set_lines(NVT_CS, NVT_MOSI);
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

	if (fw_size != 196608 || fw[0x2f000] != 0x29 || fw[0x2f001] != 0xd6 ||
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

	if (!nvt_read(NVT_CHIP_ID, id, sizeof(id)) ||
	    (id[3] != 0x72 && id[3] != 0x71) || id[4] != 0x87 || id[5] != 0x03) {
		printf("LAPIS NVT: chip ID check failed (%02x %02x %02x)\n",
		    id[3], id[4], id[5]);
		return false;
	}
	ver[0] = ver[1] = 0;
	if (!nvt_read(NVT_FW_VERSION, ver, sizeof(ver))) {
		printf("LAPIS NVT: firmware-version read failed\n");
		return false;
	}
	already_running = (ver[0] == 0x29 && ver[1] == 0xd6 &&
	    nvt_read(NVT_EVENT_BUF + 0x60u, &running, 1) && running == 0xa0);
	if (already_running) {
		printf("LAPIS NVT: firmware already active (ver=%02x%02x state=%02x)\n",
		    ver[0], ver[1], running);
		return true;
	}
	printf("LAPIS NVT: chip=%02x%02x%02x firmware-before=%02x%02x\n",
	    id[3], id[4], id[5], ver[0], ver[1]);
	if (!nvt_partition_table(parts, &count)) {
		printf("LAPIS NVT: firmware table rejected\n");
		return false;
	}
	printf("LAPIS NVT: firmware table accepted (%u partitions)\n", count);
	if (!nvt_write_u8(NVT_RESET, 0x69)) {
		printf("LAPIS NVT: bootloader reset write failed\n");
		return false;
	}
	delay(500);
	decouple[0] = 0xaa; decouple[1] = 0x55;
	if (!nvt_write(NVT_RESX_DECOUPLE, decouple, sizeof(decouple))) {
		printf("LAPIS NVT: RESX decouple write failed\n");
		return false;
	}
	if (!nvt_read(NVT_RESX_DECOUPLE, check, sizeof(check))) {
		printf("LAPIS NVT: RESX decouple read failed\n");
		return false;
	}
	if (check[0] != decouple[0] || check[1] != decouple[1]) {
		printf("LAPIS NVT: RESX mismatch wrote=aa55 read=%02x%02x\n",
		    check[0], check[1]);
		return false;
	}
	printf("LAPIS NVT: bootloader reset and RESX decouple verified\n");
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
		if (nvt_read(NVT_EVENT_BUF + 0x60u, &status, 1) && status == 0xa0) {
			if (!nvt_read(NVT_FW_VERSION, ver, sizeof(ver)) ||
			    ver[0] != 0x29 || ver[1] != 0xd6) {
				printf("LAPIS NVT: state ready but version mismatch (%02x%02x)\n",
				    ver[0], ver[1]);
				return false;
			}
			printf("LAPIS NVT: firmware loaded (version=%02x%02x state=%02x)\n",
			    ver[0], ver[1], status);
			return true;
		}
		delay(10000);
	}
	if (!nvt_read(NVT_FW_VERSION, ver, sizeof(ver))) ver[0] = ver[1] = 0xff;
	if (!nvt_read(NVT_CHIP_ID, id, sizeof(id))) id[3] = id[4] = id[5] = 0xff;
	printf("LAPIS NVT: firmware boot timeout state=%02x version=%02x%02x chip=%02x%02x%02x\n",
	    status, ver[0], ver[1], id[3], id[4], id[5]);
	{
		uint8_t ilm_head[4] = { 0 }, dlm_head[4] = { 0 };
		uint8_t ilm_crc[4] = { 0 }, dlm_crc[4] = { 0 };
		(void)nvt_read(parts[0].sram, ilm_head, sizeof(ilm_head));
		(void)nvt_read(parts[1].sram, dlm_head, sizeof(dlm_head));
		(void)nvt_read(0x3fbbcu, ilm_crc, sizeof(ilm_crc));
		(void)nvt_read(0x3fbc0u, dlm_crc, sizeof(dlm_crc));
		printf("LAPIS NVT: verify ILM=%08x/%08x CRC=%08x/%08x DLM=%08x/%08x CRC=%08x/%08x\n",
		    nvt_le32(ilm_head), nvt_le32(lapis_nt38771_fw + parts[0].bin),
		    nvt_le32(ilm_crc), parts[0].crc,
		    nvt_le32(dlm_head), nvt_le32(lapis_nt38771_fw + parts[1].bin),
		    nvt_le32(dlm_crc), parts[1].crc);
	}
	return false;
}

static void
nvt_emit(void *context, uint8_t slot, int down, uint16_t x, uint16_t y)
{
	uint16_t bit = (uint16_t)(1u << slot);
	(void)context;
	if (down) {
		/* The framebuffer viewport is x=0..1124, physical y=168..2603. */
		if (x >= NVT_VIEW_WIDTH || y < NVT_VIEW_TOP ||
		    y >= NVT_VIEW_TOP + NVT_VIEW_HEIGHT) {
			if (nvt.hid_slots & bit) {
				sweet_hid_touch(slot, 0, nvt.hid_x[slot], nvt.hid_y[slot]);
				nvt.hid_slots &= (uint16_t)~bit;
			}
			return;
		}
		nvt.hid_x[slot] = x;
		nvt.hid_y[slot] = y - NVT_VIEW_TOP;
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
nvt_gpio_init(void)
{
	uint32_t mode, dir, dout;
	nvt.gpio = ml_io_map(NVT_GPIO_PA, 0x1000);
	if (nvt.gpio == 0) return false;
	mode = nvt_rd(NVT_GPIO_MODE);
	dir = nvt_rd(NVT_GPIO_DIR);
	dout = nvt_rd(NVT_GPIO_DO);
	/* Idle CS high/CLK low before changing mux; never touch GPIO90. */
	nvt_wr(NVT_GPIO_DO, (dout & ~(NVT_CLK | NVT_MOSI)) | NVT_CS);
	nvt_wr(NVT_GPIO_DIR, (dir & ~NVT_MISO) | NVT_CLK | NVT_CS | NVT_MOSI);
	nvt_wr(NVT_GPIO_MODE, mode & 0x0000ffffu);
	delay(1000);
	return true;
}

static void
nvt_thread(void *arg, wait_result_t wr)
{
	unsigned bad = 0;
	(void)arg; (void)wr;
	if (!nvt_gpio_init() || !nvt_boot_firmware()) goto out;
	lapis_nvt_touch_state_init(&nvt.state);
	printf("LAPIS NVT: touch polling started (%ux%u viewport)\n",
	    NVT_VIEW_WIDTH, NVT_VIEW_HEIGHT);
	for (;;) {
		if (!nvt_read(NVT_EVENT_BUF, nvt.report, NVT_REPORT_BYTES) ||
		    lapis_nvt_process_frame(&nvt.state, nvt.report,
		    LAPIS_NVT_REPORT_BYTES, nvt_emit, NULL) != LAPIS_NVT_PARSE_OK) {
			if (++bad >= NVT_BAD_FRAME_LIMIT) {
				nvt_release_all();
				bad = 0;
			}
		} else {
			bad = 0;
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
