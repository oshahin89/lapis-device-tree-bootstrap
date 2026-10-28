#ifndef LAPIS_NT38771_SWEET_HID_GLUE_H
#define LAPIS_NT38771_SWEET_HID_GLUE_H

#include "nt38771_touch_state.h"

/* Adapter for the reference XNU port's sweet_hid_touch() entry point. */
void lapis_nvt_sweet_hid_emit(void *context, uint8_t slot, int down,
	uint16_t x, uint16_t y);

#endif /* LAPIS_NT38771_SWEET_HID_GLUE_H */
