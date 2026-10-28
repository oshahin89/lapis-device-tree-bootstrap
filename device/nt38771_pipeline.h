#ifndef LAPIS_NT38771_PIPELINE_H
#define LAPIS_NT38771_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "nt38771_touch_state.h"

/* frame points to the 384-byte payload at CTP_SPI_READ buffer offset 1. */
int lapis_nvt_process_frame(struct lapis_nvt_touch_state *state,
	const uint8_t *frame, size_t frame_len);

#endif /* LAPIS_NT38771_PIPELINE_H */
