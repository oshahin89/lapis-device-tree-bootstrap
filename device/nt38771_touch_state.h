#ifndef LAPIS_NT38771_TOUCH_STATE_H
#define LAPIS_NT38771_TOUCH_STATE_H

#include <stdint.h>

#include "nt38771_report_parser.h"

#define LAPIS_NVT_DISPLAY_WIDTH   1280U
#define LAPIS_NVT_DISPLAY_HEIGHT  2772U
#define LAPIS_NVT_COORD_SHIFT        4U

struct lapis_nvt_touch_state {
	uint16_t active_slots;
	uint16_t last_x[LAPIS_NVT_CONTACTS];
	uint16_t last_y[LAPIS_NVT_CONTACTS];
};

#define LAPIS_NVT_TOUCH_STATE_INITIALIZER { 0 }

typedef void (*lapis_nvt_touch_event_fn)(void *context, uint8_t slot,
	int down, uint16_t x, uint16_t y);

void lapis_nvt_touch_state_init(struct lapis_nvt_touch_state *state);

/*
 * Apply one successfully parsed report. Coordinates are converted from the
 * Linux driver's 16x-scaled range to native panel pixels. Slots absent from
 * this report are released using their last known coordinates.
 */
int lapis_nvt_apply_report(struct lapis_nvt_touch_state *state,
	const struct lapis_nvt_report *report,
	lapis_nvt_touch_event_fn emit, void *context);

#endif /* LAPIS_NT38771_TOUCH_STATE_H */
