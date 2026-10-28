#include "nt38771_sweet_hid_glue.h"

extern void sweet_hid_touch(int finger, int down, int x, int y);

void
lapis_nvt_sweet_hid_emit(void *context, uint8_t slot, int down,
	uint16_t x, uint16_t y)
{
	(void)context;
	sweet_hid_touch((int)slot, down, (int)x, (int)y);
}
