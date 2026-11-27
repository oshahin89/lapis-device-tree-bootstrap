#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_mic_pcm.h"
int main(void)
{
    const uint8_t raw24[] = {
        0xff,0xff,0x7f,0x00, 0x00,0x00,0x80,0xff,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x01,0x00,
        0xaa
    };
    int16_t out[4] = {123,123,123,123};
    assert(lapis_mic_s24le32_stereo_to_mono16(raw24, sizeof(raw24), 0, out, 4) == 2);
    assert(out[0] == 32767 && out[1] == 0 && out[2] == 123);
    assert(lapis_mic_s24le32_stereo_to_mono16(raw24, sizeof(raw24), 1, out, 1) == 1);
    assert(out[0] == -32768);
    assert(lapis_mic_s24le32_stereo_to_mono16(raw24, sizeof(raw24), 2, out, 4) == 0);
    const uint8_t raw16[] = {0xff,0x7f,0x00,0x80, 0x00,0x00,0xff,0xff};
    assert(lapis_mic_s16le_stereo_to_mono16(raw16, sizeof(raw16), 1, out, 4) == 2);
    assert(out[0] == -32768 && out[1] == -1);
    assert(lapis_mic_s16le_stereo_to_mono16(raw16, 3, 0, out, 4) == 0);
    puts("Candidate PCM conversion, channel, short-buffer and extremes passed");
    return 0;
}
