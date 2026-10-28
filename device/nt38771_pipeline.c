#include "nt38771_pipeline.h"

#include "nt38771_report_parser.h"
#include "nt38771_sweet_hid_glue.h"

int
lapis_nvt_process_frame(struct lapis_nvt_touch_state *state,
	const uint8_t *frame, size_t frame_len)
{
	struct lapis_nvt_report report;
	enum lapis_nvt_parse_result result;

	result = lapis_nvt_parse_report(frame, frame_len, &report);
	if (result != LAPIS_NVT_PARSE_OK)
		return (int)result;

	return lapis_nvt_apply_report(state, &report,
		lapis_nvt_sweet_hid_emit, 0);
}
