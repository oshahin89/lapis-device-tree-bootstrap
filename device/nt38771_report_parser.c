#include "nt38771_report_parser.h"

enum lapis_nvt_parse_result
lapis_nvt_parse_report(const uint8_t *frame, size_t frame_len,
	struct lapis_nvt_report *report)
{
	unsigned int checksum = 0;
	unsigned int i;

	if (frame == NULL || report == NULL)
		return LAPIS_NVT_PARSE_BAD_ARGUMENT;

	if (frame_len != LAPIS_NVT_REPORT_BYTES)
		return LAPIS_NVT_PARSE_BAD_LENGTH;

	for (i = 0; i < LAPIS_NVT_CHECKSUM_BYTES; i++)
		checksum = (checksum + frame[i]) & 0xffU;
	if (frame[LAPIS_NVT_CHECKSUM_OFFSET] != ((0U - checksum) & 0xffU))
		return LAPIS_NVT_PARSE_BAD_CHECKSUM;

	/* Do not expose partial results from a failed frame. */
	report->present_slots = 0;
	report->contact_count = 0;

	for (i = 0; i < LAPIS_NVT_CONTACTS; i++) {
		const unsigned int offset = i * LAPIS_NVT_CONTACT_BYTES;
		const uint8_t state_id = frame[offset];
		const uint8_t state = state_id & 0x07U;
		const uint8_t id = state_id >> 3;
		/* Big-endian: Android's own frames (kprobe after CTP_SPI_READ,
		 * device/captures/nt38771_android_touch_frames_2026-11-22.txt)
		 * only fit 0..20479 x 0..44351 read this way. */
		const uint16_t x = (uint16_t)(((uint16_t)frame[offset + 1] << 8) |
			frame[offset + 2]);
		const uint16_t y = (uint16_t)(((uint16_t)frame[offset + 3] << 8) |
			frame[offset + 4]);
		uint8_t touch_major;
		uint8_t slot;
		struct lapis_nvt_contact *contact;

		/* Stock code treats states 1..4 as active contact records. */
		if (state < 1U || state > 4U || id < 1U || id > LAPIS_NVT_CONTACTS)
			continue;
		if (x > LAPIS_NVT_MAX_X || y > LAPIS_NVT_MAX_Y)
			continue;

		slot = id - 1U;
		touch_major = frame[offset + 5];
		if (touch_major == 0)
			touch_major = 1;

		contact = &report->contacts[report->contact_count++];
		contact->slot = slot;
		contact->state = state;
		contact->x = x;
		contact->y = y;
		contact->touch_major = touch_major;
		report->present_slots |= (uint16_t)(1U << slot);
	}

	return LAPIS_NVT_PARSE_OK;
}
