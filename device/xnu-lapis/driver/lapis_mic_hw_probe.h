/* Bounded MT6369/VUL9 capture experiment, included by the AFE owner.
 * Register values come from Lapis's Android capture trace. */
#include "lapis_pmif.h"
#include "lapis_mic_cursor.h"

static bool
au_mic_probe(void)
{
	const uint32_t base = 0x11059000u, size = 0x4000u;
	const uint32_t on = 1u << 28;
	const uint32_t offsets[] = {
		0x04, 0x14, 0x1204, 0x1190, 0x11f0, 0x1200,
		0x0bd4, 0x0bec, 0x0bf0, 0x0bf4, 0x0bf8, 0x0bfc, 0x0bd0,
		0x2500, 0x2520, 0x4f10, 0x4f14, 0x4f20, 0x4f24,
	};
	const uint32_t values[] = {
		0, 3, 0xb9, 0x80010000, 0x80010000, 0x06303100,
		0x7fff0412, 0, 0x3fb8, 0x3fb80000, 0x3fb80000, 0xc048, 0x60401,
		1u << 16, 1u << 17, 0, base, 0, base + size - 1,
	};
	struct codec_step { uint16_t reg; uint8_t mask, value; };
	const struct codec_step codec[] = {
		/* MT6369 GPIO_MODE7 routes the codec transmit pins. */
		{ 0x00d0, 0x3f, 0x09 }, { 0x25a0, 0x30, 0x20 }, { 0x2514, 0x01, 0x01 },
		{ 0x25a2, 0x01, 0x01 }, { 0x25a2, 0x04, 0x04 }, { 0x25a2, 0x20, 0x20 },
		{ 0x230c, 0x06, 0 }, { 0x2519, 0x71, 0x21 },
		{ 0x238f, 0xc0, 0x40 }, { 0x2388, 0x01, 0x01 },
		{ 0x23de, 0x07, 0x01 }, { 0x23db, 0x3f, 0x22 },
		{ 0x2508, 0xc1, 0x41 }, { 0x250a, 0xc1, 0xc1 },
		{ 0x2509, 0xd7, 0xd1 }, { 0x250b, 0x77, 0x51 },
		{ 0x23bc, 0xff, 0x10 }, { 0x23bd, 0xff, 0x02 },
		{ 0x23d3, 0xff, 0x12 }, { 0x23d3, 0xff, 0x13 }, { 0x238b, 0x01, 0x01 },
	};
	uint32_t old[sizeof(offsets) / sizeof(offsets[0])];
	uint8_t saved[sizeof(codec) / sizeof(codec[0])];
	unsigned applied = 0, mmio_applied = 0;
	bool restored = true, dma_started = false, passed = false;
	uint8_t id, ul, left_adc, right_adc, bias;
	uint8_t vote_before = 0xff, oscillator_before = 0;
	bool oscillator_touched = false;
	uint32_t control = au_rd(au_afe, 0x4f30);
	/* Verify exclusive ownership of the engines whose Android buffers overlap.
	 * Leave active capture or playback alone. */
	if ((control & (on | (1u << 7))) ||
	    ((au_rd(au_afe, 0x4460) | au_rd(au_afe, 0x4640) |
	    au_rd(au_afe, 0x4e10) | au_rd(au_afe, 0x4db0)) & on) ||
	    (au_rd(au_afe, 0x0bd0) & 1)) {
		printf("LAPIS MIC: skipped, audio engine busy\n");
		return true;
	}
	const uint32_t other_routes[] = { 0x2504, 0x2508, 0x2510, 0x2518, 0x2524, 0x2528, 0x2530, 0x2538 };
	for (unsigned i = 0; i < sizeof(other_routes) / sizeof(other_routes[0]); i++) {
		if (au_rd(au_afe, other_routes[i])) {
			printf("LAPIS MIC: skipped, VUL9 has another route\n");
			return true;
		}
	}
	if (lapis_pmif_shared_read(5, 0x0b, &id) != LAPIS_PMIF_OK || id != 0x69) {
		printf("LAPIS MIC: skipped, codec ID unavailable\n");
		return true;
	}
	if (lapis_pmif_shared_read(5, 0x238b, &ul) != LAPIS_PMIF_OK || (ul & 1) ||
	    lapis_pmif_shared_read(5, 0x2509, &left_adc) != LAPIS_PMIF_OK || (left_adc & 0x10) ||
	    lapis_pmif_shared_read(5, 0x250b, &right_adc) != LAPIS_PMIF_OK || (right_adc & 0x10) ||
	    lapis_pmif_shared_read(5, 0x2519, &bias) != LAPIS_PMIF_OK || (bias & 1)) {
		printf("LAPIS MIC: skipped, codec not idle\n");
		return true;
	}
	volatile uint32_t *ring = (volatile uint32_t *)ml_io_map(base, size);
	if (!ring) {
		printf("LAPIS MIC: capture SRAM map failed\n");
		return true;
	}
	const uint32_t gpio_offsets[] = { 0x440, 0x450 };
	const uint32_t gpio_mask[] = { 0x0f0f0000, 0x0000ff00 };
	const uint32_t gpio_mode[] = { 0x01010000, 0x00001100 };
	uint32_t gpio_saved[2];
	for (unsigned i = 0; i < 2; i++) gpio_saved[i] = au_rd(au_gpio, gpio_offsets[i]);
	for (unsigned i = 0; i < size / 4; i++) ring[i] = 0;
	__asm__ volatile("dsb oshst" ::: "memory");
	for (unsigned i = 0; i < 2; i++)
		au_wr(au_gpio, gpio_offsets[i], (gpio_saved[i] & ~gpio_mask[i]) | gpio_mode[i]);
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		old[i] = au_rd(au_afe, offsets[i]);
		mmio_applied++;
		au_wr(au_afe, offsets[i], values[i]);
		uint32_t mask = offsets[i] == 0x4f14 || offsets[i] == 0x4f24 ? ~0xfu : ~0u;
		if ((au_rd(au_afe, offsets[i]) & mask) != (values[i] & mask)) {
			printf("LAPIS MIC: AFE readback failed at %#x\n", offsets[i]);
			goto stop;
		}
	}
	if (lapis_pmif_audio_vote(1, &vote_before) != LAPIS_PMIF_OK) {
		printf("LAPIS MIC: VS1 vote failed\n");
		goto stop;
	}
	if (lapis_pmif_shared_read(9, 0x079e, &oscillator_before) != LAPIS_PMIF_OK) goto stop;
	oscillator_touched = true;
	if (lapis_pmif_shared_update(9, 0x079e, 0x04, 0x04, &oscillator_before) != LAPIS_PMIF_OK) {
		printf("LAPIS MIC: oscillator update failed\n");
		goto stop;
	}
	for (unsigned i = 0; i < sizeof(codec) / sizeof(codec[0]); i++) {
		const struct codec_step *step = &codec[i];
		if (lapis_pmif_shared_read(5, step->reg, &saved[i]) != LAPIS_PMIF_OK) goto stop;
		applied++;
		uint8_t before = saved[i];
		enum lapis_pmif_result result = lapis_pmif_shared_update(5, step->reg,
		    step->mask, step->value, &before);
		saved[i] = before;
		if (result != LAPIS_PMIF_OK) {
			printf("LAPIS MIC: codec update %#x failed (%u)\n", step->reg, (unsigned)result);
			goto stop;
		}
		au_sleep_ms(2);
	}
	dma_started = true;
	au_wr(au_afe, 0x4f30, on | 0x100a49);
	if (!(au_rd(au_afe, 0x4f30) & on)) {
		printf("LAPIS MIC: VUL9 enable did not latch\n");
		goto stop;
	}
	printf("LAPIS MIC: capturing aggregate signal for two seconds\n");
	struct lapis_mic_cursor position;
	bool cursor_ready = false;
	uint32_t cursor = 0;
	unsigned observed = 0, nonzero[2] = {0, 0}, peak[2] = {0, 0};
	for (unsigned tick = 0; tick < 200; tick++) {
		au_sleep_ms(10);
		uint64_t now;
		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
		cursor = au_rd(au_afe, 0x4f1c);
		if (au_rd(au_afe, 0x4f18) || cursor < base || cursor >= base + size) {
			if (cursor_ready) break;
			continue;
		}
		if (!cursor_ready) {
			cursor_ready = lapis_mic_cursor_init(&position, base, size, 8, 512, 0x3000,
			    cursor, now, 30000000) == LAPIS_MIC_CURSOR_OK;
			continue;
		}
		struct lapis_mic_spans spans;
		enum lapis_mic_cursor_result result = lapis_mic_cursor_observe(&position, cursor, now, &spans);
		if (result != LAPIS_MIC_CURSOR_OK) {
			printf("LAPIS MIC: cursor fault %u\n", (unsigned)result);
			break;
		}
		const uint32_t starts[] = { spans.offset1, spans.offset2 };
		const uint32_t counts[] = { spans.bytes1, spans.bytes2 };
		for (unsigned span = 0; span < 2; span++) {
			for (uint32_t i = starts[span] / 4; i < (starts[span] + counts[span]) / 4; i++) {
				uint32_t u = ring[i] & 0xffffffu;
				unsigned mag = u & 0x800000u ? 0x1000000u - u : u;
				nonzero[i & 1] += mag != 0;
				if (mag > peak[i & 1]) peak[i & 1] = mag;
				observed++;
			}
		}
	}
	printf("LAPIS MIC: cursor=%#x words=%u nonzero=%u/%u peak24=%u/%u\n",
	    cursor, observed, nonzero[0], nonzero[1], peak[0], peak[1]);
	passed = nonzero[0] || nonzero[1];
stop:
	if (dma_started) {
		au_wr(au_afe, 0x4f30, control);
		dma_started = (au_rd(au_afe, 0x4f30) & on) != 0;
	}
	while (applied) {
		unsigned i = --applied;
		uint8_t ignored;
		if (lapis_pmif_shared_update(5, codec[i].reg, codec[i].mask, saved[i], &ignored) != LAPIS_PMIF_OK)
			restored = false;
	}
	uint8_t ignored;
	if (oscillator_touched &&
	    lapis_pmif_shared_update(9, 0x079e, 0x04, oscillator_before, &ignored) != LAPIS_PMIF_OK)
		restored = false;
	if (vote_before == 0 && lapis_pmif_audio_vote(0, &ignored) != LAPIS_PMIF_OK)
		restored = false;
	if (!dma_started) {
		while (mmio_applied) {
			unsigned i = --mmio_applied;
			au_wr(au_afe, offsets[i], old[i]);
			if (au_rd(au_afe, offsets[i]) != old[i]) restored = false;
		}
		for (unsigned i = 0; i < size / 4; i++) ring[i] = 0;
		__asm__ volatile("dsb oshst" ::: "memory");
	}
	for (unsigned i = 0; i < 2; i++) {
		uint32_t current = au_rd(au_gpio, gpio_offsets[i]);
		au_wr(au_gpio, gpio_offsets[i], (current & ~gpio_mask[i]) | (gpio_saved[i] & gpio_mask[i]));
	}
	printf("LAPIS MIC: sample_check=%u restored=%u dma_stopped=%u\n",
	    (unsigned)passed, (unsigned)restored, (unsigned)!dma_started);
	return restored && !dma_started;
}
