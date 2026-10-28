#include "nt38771_touch_state.h"

void
lapis_nvt_touch_state_init(struct lapis_nvt_touch_state *state)
{
	unsigned int i;

	if (state == 0)
		return;

	state->active_slots = 0;
	for (i = 0; i < LAPIS_NVT_CONTACTS; i++) {
		state->last_x[i] = 0;
		state->last_y[i] = 0;
	}
}

int
lapis_nvt_apply_report(struct lapis_nvt_touch_state *state,
	const struct lapis_nvt_report *report,
	lapis_nvt_touch_event_fn emit, void *context)
{
	uint16_t old_slots;
	unsigned int i;

	if (state == 0 || report == 0 || emit == 0 ||
	    report->contact_count > LAPIS_NVT_CONTACTS)
		return -1;

	old_slots = state->active_slots;
	for (i = 0; i < report->contact_count; i++) {
		const struct lapis_nvt_contact *contact = &report->contacts[i];
		const uint16_t x = contact->x >> LAPIS_NVT_COORD_SHIFT;
		const uint16_t y = contact->y >> LAPIS_NVT_COORD_SHIFT;

		if (contact->slot >= LAPIS_NVT_CONTACTS ||
		    x >= LAPIS_NVT_DISPLAY_WIDTH || y >= LAPIS_NVT_DISPLAY_HEIGHT)
			continue;

		emit(context, contact->slot, 1, x, y);
		state->last_x[contact->slot] = x;
		state->last_y[contact->slot] = y;
	}

	for (i = 0; i < LAPIS_NVT_CONTACTS; i++) {
		const uint16_t bit = (uint16_t)(1U << i);

		if ((old_slots & bit) != 0 && (report->present_slots & bit) == 0)
			emit(context, (uint8_t)i, 0,
				state->last_x[i], state->last_y[i]);
	}

	state->active_slots = report->present_slots &
		(uint16_t)((1U << LAPIS_NVT_CONTACTS) - 1U);
	return 0;
}
