/* Host-only synthetic end-to-end check. No microphone or device access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_mic_cursor.h"
#include "../../device/xnu-lapis/driver/lapis_mic_pcm.h"
#include "../../device/xnu-lapis/driver/lapis_mic_queue.h"

static uint8_t ring[64];
static struct lapis_mic_queue queue;
static void put16(unsigned offset, int16_t value)
{
    uint16_t u = (uint16_t)value;
    ring[offset] = (uint8_t)u;
    ring[offset + 1] = (uint8_t)(u >> 8);
}
static void consume(uint32_t offset, uint32_t bytes)
{
    int16_t mono[16];
    assert(offset + bytes <= sizeof(ring));
    size_t n = lapis_mic_s16le_stereo_to_mono16(ring + offset, bytes, 1, mono, 16);
    assert(n == bytes / 4);
    assert(lapis_mic_queue_push(&queue, mono, (uint32_t)n) == n);
}
static void put24(unsigned offset, int32_t value)
{
    uint32_t u = (uint32_t)value & 0xffffffu;
    ring[offset] = (uint8_t)u;
    ring[offset + 1] = (uint8_t)(u >> 8);
    ring[offset + 2] = (uint8_t)(u >> 16);
    ring[offset + 3] = 0;
}
static void consume24(uint32_t offset, uint32_t bytes)
{
    int16_t mono[8];
    assert(offset + bytes <= sizeof(ring));
    size_t n = lapis_mic_s24le32_stereo_to_mono16(ring + offset, bytes, 1, mono, 8);
    assert(n == bytes / 8);
    assert(lapis_mic_queue_push(&queue, mono, (uint32_t)n) == n);
}
int main(void)
{
    struct lapis_mic_cursor cursor; struct lapis_mic_spans spans;
    lapis_mic_queue_reset(&queue);
    assert(lapis_mic_cursor_init(&cursor, 0x1000, sizeof(ring), 4, 8, 60, 0x1000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    unsigned head = 0;
    for (unsigned tick = 1; tick <= 17; tick++) {
        for (unsigned i = 0; i < 2; i++) {
            int16_t sample = tick <= 16 ? (int16_t)((tick - 1) * 2 + i + 1) : 0;
            put16(head, sample); put16(head + 2, (int16_t)-sample);
            head = (head + 4) % sizeof(ring);
        }
        assert(lapis_mic_cursor_observe(&cursor, 0x1000 + head, tick, &spans) == LAPIS_MIC_CURSOR_OK);
        consume(spans.offset1, spans.bytes1); consume(spans.offset2, spans.bytes2);
    }
    int16_t result[32];
    assert(lapis_mic_queue_pop(&queue, result, 32) == 32);
    for (unsigned i = 0; i < 32; i++) assert(result[i] == -(int16_t)(i + 1));
    assert(queue.count == 0);
    assert(lapis_mic_cursor_observe(&cursor, 0x1000 + head, 117, &spans) == LAPIS_MIC_CURSOR_LATE);
    lapis_mic_queue_reset(&queue);
    assert(lapis_mic_cursor_init(&cursor, 0x2000, sizeof(ring), 8, 16, 56, 0x2000, 0, 100) == LAPIS_MIC_CURSOR_OK);
    head = 0;
    for (unsigned tick = 1; tick <= 17; tick++) {
        for (unsigned i = 0; i < 2; i++) {
            int32_t sample = tick <= 16 ? (int32_t)((tick - 1) * 2 + i + 1) * 256 : 0;
            put24(head, sample); put24(head + 4, -sample);
            head = (head + 8) % sizeof(ring);
        }
        assert(lapis_mic_cursor_observe(&cursor, 0x2000 + head, tick, &spans) == LAPIS_MIC_CURSOR_OK);
        consume24(spans.offset1, spans.bytes1); consume24(spans.offset2, spans.bytes2);
    }
    assert(lapis_mic_queue_pop(&queue, result, 32) == 32);
    for (unsigned i = 0; i < 32; i++) assert(result[i] == -(int16_t)(i + 1));
    lapis_mic_queue_reset(&queue);
    puts("Synthetic DMA-to-PCM-to-queue pipeline and late-poll safety passed");
    return 0;
}
