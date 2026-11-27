#include <assert.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_mic_cursor.h"
int main(void)
{
    struct lapis_mic_cursor c; struct lapis_mic_spans s;
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 4, 8, 60, 0x1030, 0, 100) == LAPIS_MIC_CURSOR_OK);
    assert(lapis_mic_cursor_observe(&c, 0x1034, 1, &s) == LAPIS_MIC_CURSOR_OK && s.bytes1 == 0 && s.bytes2 == 0);
    assert(lapis_mic_cursor_observe(&c, 0x1038, 2, &s) == LAPIS_MIC_CURSOR_OK && s.bytes1 == 0);
    assert(lapis_mic_cursor_observe(&c, 0x103c, 3, &s) == LAPIS_MIC_CURSOR_OK && s.offset1 == 48 && s.bytes1 == 4);
    assert(lapis_mic_cursor_observe(&c, 0x1004, 4, &s) == LAPIS_MIC_CURSOR_OK && s.offset1 == 52 && s.bytes1 == 8);
    assert(lapis_mic_cursor_observe(&c, 0x1014, 5, &s) == LAPIS_MIC_CURSOR_OK && s.offset1 == 60 && s.bytes1 == 4 && s.bytes2 == 12);
    assert(lapis_mic_cursor_observe(&c, 0x1014, 105, &s) == LAPIS_MIC_CURSOR_LATE);
    assert(lapis_mic_cursor_observe(&c, 0x1018, 106, &s) == LAPIS_MIC_CURSOR_INVALID);
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 4, 8, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    assert(lapis_mic_cursor_observe(&c, 0x1008, 1, &s) == LAPIS_MIC_CURSOR_OK && s.bytes1 == 0);
    assert(lapis_mic_cursor_observe(&c, 0x1004, 2, &s) == LAPIS_MIC_CURSOR_OVERRUN);
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 0, 8, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_INVALID);
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 4, 3, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_INVALID);
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 4, 8, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    assert(lapis_mic_cursor_observe(&c, 0x1001, 1, &s) == LAPIS_MIC_CURSOR_INVALID && c.faulted);
    assert(lapis_mic_cursor_init(&c, 0x1000, 64, 4, 8, 16, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    assert(lapis_mic_cursor_observe(&c, 0x1014, 1, &s) == LAPIS_MIC_CURSOR_DISCONTINUITY && c.faulted);
    puts("DMA cursor guard, wrap, overrun, timeout and invalid-state checks passed");
    return 0;
}
