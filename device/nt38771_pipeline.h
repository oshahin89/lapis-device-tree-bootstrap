#ifndef LAPIS_NT38771_PIPELINE_H
#define LAPIS_NT38771_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "nt38771_touch_state.h"

/*
 * frame points to the 384-byte register payload returned by the SPI helper.
 * emit is supplied by the caller so the parser/state layer has no dependency
 * on a particular XNU HID implementation.
 */
int lapis_nvt_process_frame(struct lapis_nvt_touch_state *state,
	const uint8_t *frame, size_t frame_len,
	lapis_nvt_touch_event_fn emit, void *context);

#endif /* LAPIS_NT38771_PIPELINE_H */
