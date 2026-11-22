#ifndef LAPIS_NT38771_REPORT_PARSER_H
#define LAPIS_NT38771_REPORT_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define LAPIS_NVT_REPORT_BYTES       108U  /* Android: CTP_SPI_READ len 109 */
#define LAPIS_NVT_CHECKSUM_BYTES      64U
#define LAPIS_NVT_CHECKSUM_OFFSET     64U
#define LAPIS_NVT_CONTACTS            10U
#define LAPIS_NVT_CONTACT_BYTES        6U
#define LAPIS_NVT_MAX_X            20479U
#define LAPIS_NVT_MAX_Y            44351U

enum lapis_nvt_parse_result {
	LAPIS_NVT_PARSE_OK = 0,
	LAPIS_NVT_PARSE_BAD_ARGUMENT = -1,
	LAPIS_NVT_PARSE_BAD_LENGTH = -2,
	LAPIS_NVT_PARSE_BAD_CHECKSUM = -3
};

struct lapis_nvt_contact {
	uint8_t slot;
	uint8_t state;
	uint8_t touch_major;
	uint16_t x;
	uint16_t y;
};

struct lapis_nvt_report {
	uint16_t present_slots;
	uint8_t contact_count;
	struct lapis_nvt_contact contacts[LAPIS_NVT_CONTACTS];
};

/*
 * Parse the 108 report bytes copied by CTP_SPI_READ into buf[1..108].
 * A checksum failure rejects the whole frame. Invalid individual contacts
 * are skipped, matching the stock client's per-record validation behavior.
 * FOD key events are intentionally left to a separate platform layer.
 */
enum lapis_nvt_parse_result
lapis_nvt_parse_report(const uint8_t *frame, size_t frame_len,
	struct lapis_nvt_report *report);

#endif /* LAPIS_NT38771_REPORT_PARSER_H */
