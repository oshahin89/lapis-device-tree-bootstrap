/* Bounded MT6369/VUL9 capture experiment, included by the AFE owner.
 * Register values come from Lapis's Android capture trace. */
#include "lapis_pmif.h"
#include "lapis_mic_cursor.h"

/* MT6369's Android DAPM handlers retry ADC power when RC trim is at an
 * endpoint. The enclosing transaction already owns and restores bit 4. */
static bool
au_mic_check_trim(uint16_t control, uint16_t status)
{
	uint8_t trim, previous;
	if (lapis_pmif_shared_read(5, status, &trim) != LAPIS_PMIF_OK) return false;
	unsigned initial = trim & 0x1f;
	if (initial == 0 || initial == 0x1f) {
		if (lapis_pmif_shared_update(5, control, 0x10, 0, &previous) != LAPIS_PMIF_OK ||
		    lapis_pmif_shared_update(5, control, 0x10, 0x10, &previous) != LAPIS_PMIF_OK)
			return false;
		au_sleep_ms(2);
		if (lapis_pmif_shared_read(5, status, &trim) != LAPIS_PMIF_OK) return false;
	}
	printf("LAPIS MIC: ADC trim reg=%#x initial=%u final=%u\n", status, initial, trim & 0x1f);
	return true;
}

static const uint32_t au_mic_marker = 0xac51e379u;

struct au_mic_codec_step {
	uint16_t reg;
	uint8_t mask, value;
};

static bool
au_mic_apply(const struct au_mic_codec_step *step, uint8_t *saved, unsigned *applied)
{
	if (lapis_pmif_shared_read(5, step->reg, saved) != LAPIS_PMIF_OK) {
		printf("LAPIS MIC: codec snapshot %#x failed\n", step->reg);
		return false;
	}
	++*applied;
	enum lapis_pmif_result result = lapis_pmif_shared_update(5, step->reg,
	    step->mask, step->value, saved);
	if (result != LAPIS_PMIF_OK)
		printf("LAPIS MIC: codec update %#x failed (%u)\n", step->reg, (unsigned)result);
	return result == LAPIS_PMIF_OK;
}

static bool
au_mic_measure(volatile uint32_t *ring, uint32_t base, uint32_t size,
    const char *phase, unsigned ticks)
{
	printf("LAPIS MIC: phase=%s capturing aggregate signal\n", phase);
	struct lapis_mic_cursor position;
	bool cursor_ready = false;
	uint32_t cursor = 0;
	unsigned raw_nonzero[2] = {0, 0};
	unsigned observed = 0, nonzero[2] = {0, 0}, peak[2] = {0, 0};
	unsigned untouched_words = 0;
	unsigned changes[2] = {0, 0};
	uint32_t previous[2] = {0, 0};
	/* The selected MTKAIF debug field and pad status can be static or
	 * unavailable in this protocol. Count activity without logging PCM. */
	const struct {
		uint32_t offset, mask;
	} monitors[] = {
		{ 0x1208, 0xffff }, { 0x1188, ~0u }, { 0x0be8, 0xffffff },
		{ 0x4f2c, ~0u }, { 0x4f28, ~0u },
	};
	unsigned monitor_nonzero[sizeof(monitors) / sizeof(monitors[0])] = {0};
	unsigned monitor_changes[sizeof(monitors) / sizeof(monitors[0])] = {0};
	uint32_t monitor_previous[sizeof(monitors) / sizeof(monitors[0])] = {0};
	for (unsigned tick = 0; tick < ticks; tick++) {
		au_sleep_ms(10);
		for (unsigned i = 0; i < sizeof(monitors) / sizeof(monitors[0]); i++) {
			uint32_t value = au_rd(au_afe, monitors[i].offset) & monitors[i].mask;
			monitor_nonzero[i] += value != 0;
			monitor_changes[i] += tick != 0 && value != monitor_previous[i];
			monitor_previous[i] = value;
		}
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
				uint32_t raw = ring[i];
				if (raw == au_mic_marker) {
					untouched_words++;
					continue;
				}
				unsigned channel = i & 1;
				raw_nonzero[channel] += raw != 0;
				changes[channel] += raw != previous[channel];
				previous[channel] = raw;
				uint32_t u = raw & 0xffffffu;
				unsigned mag = u & 0x800000u ? 0x1000000u - u : u;
				nonzero[channel] += mag != 0;
				if (mag > peak[channel]) peak[channel] = mag;
				observed++;
			}
		}
	}
	printf("LAPIS MIC: phase=%s cursor=%#x words=%u untouched=%u raw_nonzero=%u/%u "
	    "changes=%u/%u nonzero24=%u/%u peak24=%u/%u\n",
	    phase, cursor, observed, untouched_words, raw_nonzero[0], raw_nonzero[1],
	    changes[0], changes[1], nonzero[0], nonzero[1], peak[0], peak[1]);
	for (unsigned i = 0; i < sizeof(monitors) / sizeof(monitors[0]); i++)
		printf("LAPIS MIC: phase=%s monitor=%#x nonzero=%u changes=%u\n",
		    phase, monitors[i].offset, monitor_nonzero[i], monitor_changes[i]);
	return changes[0] > 1 || changes[1] > 1;
}

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
		0, 3, 0xb8, 0x00010000, 0x00010000, 0x06303100,
		0x7fff0412, 0, 0x3fb8, 0x3fb80000, 0x3fb80000, 0xc048, 0x60400,
		1u << 16, 1u << 17, 0, base, 0, base + size - 1,
	};
	const struct au_mic_codec_step codec[] = {
		/* MT6369 GPIO_MODE7 routes the codec transmit pins. */
		{ 0x00d0, 0x3f, 0x09 }, { 0x25a0, 0x30, 0x20 }, { 0x2514, 0x01, 0x01 },
		{ 0x25a2, 0x01, 0x01 }, { 0x25a2, 0x04, 0x04 }, { 0x25a2, 0x20, 0x20 },
		{ 0x230c, 0x06, 0 }, { 0x2519, 0x71, 0x21 },
		{ 0x238f, 0xc0, 0x40 }, { 0x2388, 0x01, 0x01 },
		{ 0x23de, 0x07, 0x01 }, { 0x23db, 0x3f, 0x22 },
		{ 0x2508, 0xc1, 0x41 }, { 0x250a, 0xc1, 0xc1 },
		{ 0x2509, 0xd7, 0xd1 }, { 0x250b, 0x77, 0x51 },
		{ 0x23bc, 0xff, 0x10 }, { 0x23bd, 0xff, 0x02 },
		{ 0x23d3, 0xff, 0x12 }, { 0x23d3, 0xff, 0x13 },
		{ 0x238e, 0x02, 0 }, { 0x238b, 0x01, 0x01 },
	};
	/* Power the MT6369 test clock, use the vendor generator settings,
	 * and select UL SGEN only. Preserve the DAC input selector. */
	const struct au_mic_codec_step generator[] = {
		{ 0x238f, 0x02, 0 }, { 0x23c9, 0x0f, 0x01 }, { 0x23ca, 0x1f, 0 },
		{ 0x23c8, 0xc0, 0x80 }, { 0x238e, 0x02, 0x02 },
	};
	uint8_t generator_saved[sizeof(generator) / sizeof(generator[0])];
	unsigned generator_applied = 0;
	uint32_t old[sizeof(offsets) / sizeof(offsets[0])];
	uint8_t saved[sizeof(codec) / sizeof(codec[0])];
	unsigned applied = 0, mmio_applied = 0;
	bool restored = true, dma_started = false, mic_activity = false, tone_activity = false;
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
	for (unsigned i = 0; i < size / 4; i++) ring[i] = au_mic_marker;
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
	/* Follow the receiver transition seen in the Android trace after power.
	 * All four registers are already covered by the MMIO rollback. */
	au_wr(au_afe, 0x1204, 0xb8);
	au_wr(au_afe, 0x1204, 0xb9);
	au_wr(au_afe, 0x1190, 0x00010000);
	au_wr(au_afe, 0x11f0, 0x00010000);
	au_wr(au_afe, 0x1190, 0x80010000);
	au_wr(au_afe, 0x11f0, 0x80010000);
	if (au_rd(au_afe, 0x1204) != 0xb9 ||
	    au_rd(au_afe, 0x1190) != 0x80010000 ||
	    au_rd(au_afe, 0x11f0) != 0x80010000) {
		printf("LAPIS MIC: MTKAIF receiver enable readback failed\n");
		goto stop;
	}
	for (unsigned i = 0; i < sizeof(codec) / sizeof(codec[0]); i++) {
		const struct au_mic_codec_step *step = &codec[i];
		/* Enable the SoC UL source after codec clock setup, before mic bias. */
		if (step->reg == 0x230c) {
			au_wr(au_afe, 0x0bd0, 0x60401);
			if (au_rd(au_afe, 0x0bd0) != 0x60401) {
				printf("LAPIS MIC: UL0 enable readback failed\n");
				goto stop;
			}
		}
		if (!au_mic_apply(step, &saved[i], &applied)) goto stop;
		au_sleep_ms(2);
		if ((step->reg == 0x2509 && !au_mic_check_trim(0x2509, 0x2512)) ||
		    (step->reg == 0x250b && !au_mic_check_trim(0x250b, 0x2513))) {
			printf("LAPIS MIC: ADC trim check failed at %#x\n", step->reg);
			goto stop;
		}
	}
	dma_started = true;
	au_wr(au_afe, 0x4f30, on | 0x100a49);
	if (!(au_rd(au_afe, 0x4f30) & on)) {
		printf("LAPIS MIC: VUL9 enable did not latch\n");
		goto stop;
	}
	mic_activity = au_mic_measure(ring, base, size, "analog", 200);
	for (unsigned i = 0; i < sizeof(generator) / sizeof(generator[0]); i++) {
		if (!au_mic_apply(&generator[i], &generator_saved[i], &generator_applied))
			goto stop;
	}
	/* Let the live VUL9 ring wrap before sampling the new source. */
	au_sleep_ms(60);
	tone_activity = au_mic_measure(ring, base, size, "codec-tone", 100);
stop:
	if (dma_started) {
		au_wr(au_afe, 0x4f30, control);
		dma_started = (au_rd(au_afe, 0x4f30) & on) != 0;
	}
	while (generator_applied) {
		unsigned i = --generator_applied;
		uint8_t ignored;
		if (lapis_pmif_shared_update(5, generator[i].reg, generator[i].mask,
		    generator_saved[i], &ignored) != LAPIS_PMIF_OK)
			restored = false;
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
	printf("LAPIS MIC: analog_activity=%u codec_tone_activity=%u restored=%u dma_stopped=%u\n",
	    (unsigned)mic_activity, (unsigned)tone_activity, (unsigned)restored, (unsigned)!dma_started);
	return restored && !dma_started;
}
