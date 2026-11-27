/* Host-only synthetic end-to-end check. No microphone or device access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_mic_pipeline.h"

static uint8_t ring[64];
static struct lapis_mic_pipeline pipeline;
static void put16(unsigned offset, int16_t value)
{
    uint16_t u = (uint16_t)value;
    ring[offset] = (uint8_t)u; ring[offset + 1] = (uint8_t)(u >> 8);
}
static void put24(unsigned offset, int32_t value)
{
    uint32_t u = (uint32_t)value & 0xffffffu;
    ring[offset] = (uint8_t)u; ring[offset + 1] = (uint8_t)(u >> 8);
    ring[offset + 2] = (uint8_t)(u >> 16); ring[offset + 3] = 0;
}
int main(void)
{
    assert(lapis_mic_pipeline_init(&pipeline, 0x1000, sizeof(ring), LAPIS_MIC_S16LE_STEREO,
        1, 8, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    unsigned head = 0;
    for (unsigned tick = 1; tick <= 17; tick++) {
        for (unsigned i = 0; i < 2; i++) {
            int16_t sample = tick <= 16 ? (int16_t)((tick - 1) * 2 + i + 1) : 0;
            put16(head, sample); put16(head + 2, (int16_t)-sample);
            head = (head + 4) % sizeof(ring);
        }
        assert(lapis_mic_pipeline_feed(&pipeline, ring, sizeof(ring), 0x1000 + head, tick) == LAPIS_MIC_CURSOR_OK);
    }
    int16_t result[32];
    assert(pipeline.accepted == 32 && pipeline.dropped == 0);
    assert(lapis_mic_pipeline_read(&pipeline, result, 32) == 32);
    for (unsigned i = 0; i < 32; i++) assert(result[i] == -(int16_t)(i + 1));
    assert(lapis_mic_pipeline_feed(&pipeline, ring, sizeof(ring), 0x1000 + head, 117) == LAPIS_MIC_CURSOR_LATE);
    assert(!pipeline.active && pipeline.queue.count == 0);
    assert(lapis_mic_pipeline_init(&pipeline, 0x2000, sizeof(ring), LAPIS_MIC_S24LE32_STEREO,
        1, 16, 56, 0x2000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    head = 0;
    for (unsigned tick = 1; tick <= 17; tick++) {
        for (unsigned i = 0; i < 2; i++) {
            int32_t sample = tick <= 16 ? (int32_t)((tick - 1) * 2 + i + 1) * 256 : 0;
            put24(head, sample); put24(head + 4, -sample);
            head = (head + 8) % sizeof(ring);
        }
        assert(lapis_mic_pipeline_feed(&pipeline, ring, sizeof(ring), 0x2000 + head, tick) == LAPIS_MIC_CURSOR_OK);
    }
    assert(lapis_mic_pipeline_read(&pipeline, result, 32) == 32);
    for (unsigned i = 0; i < 32; i++) assert(result[i] == -(int16_t)(i + 1));
    lapis_mic_pipeline_stop(&pipeline);
    assert(!pipeline.active && pipeline.queue.count == 0);
    puts("Synthetic DMA-to-PCM-to-queue pipeline and late-poll safety passed");
    return 0;
}
