/* Hardware-neutral mono capture pipeline. The caller owns and serializes the
 * DMA mapping, clock, lock and access policy. No device I/O is performed here. */
#ifndef LAPIS_MIC_PIPELINE_H
#define LAPIS_MIC_PIPELINE_H
#include "lapis_mic_cursor.h"
#include "lapis_mic_pcm.h"
#include "lapis_mic_queue.h"

enum lapis_mic_encoding { LAPIS_MIC_S16LE_STEREO = 1, LAPIS_MIC_S24LE32_STEREO = 2 };
struct lapis_mic_pipeline {
    struct lapis_mic_cursor cursor;
    struct lapis_mic_queue queue;
    enum lapis_mic_encoding encoding;
    unsigned channel, active;
    uint64_t accepted, dropped;
};

/* Guard, max_step and max_gap must be chosen from a verified DMA configuration. */
static inline enum lapis_mic_cursor_result
lapis_mic_pipeline_init(struct lapis_mic_pipeline *p, uint64_t base, uint32_t size,
    enum lapis_mic_encoding encoding, unsigned channel, uint32_t guard, uint32_t max_step,
    uint64_t initial, uint64_t now_ns, uint64_t max_gap_ns)
{
    if (!p) return LAPIS_MIC_CURSOR_INVALID;
    p->active = 0;
    lapis_mic_queue_reset(&p->queue);
    p->accepted = p->dropped = 0;
    if (channel > 1 || (encoding != LAPIS_MIC_S16LE_STEREO && encoding != LAPIS_MIC_S24LE32_STEREO))
        return LAPIS_MIC_CURSOR_INVALID;
    uint32_t frame = encoding == LAPIS_MIC_S16LE_STEREO ? 4 : 8;
    enum lapis_mic_cursor_result r = lapis_mic_cursor_init(&p->cursor, base, size, frame,
        guard, max_step, initial, now_ns, max_gap_ns);
    if (r != LAPIS_MIC_CURSOR_OK) return r;
    p->encoding = encoding; p->channel = channel; p->active = 1;
    return LAPIS_MIC_CURSOR_OK;
}

static inline void lapis_mic_pipeline_copy_span(struct lapis_mic_pipeline *p,
    const uint8_t *ring, uint32_t offset, uint32_t bytes)
{
    int16_t samples[256];
    uint32_t frame = p->cursor.frame;
    while (bytes) {
        uint32_t count = bytes / frame;
        if (count > 256) count = 256;
        size_t made = p->encoding == LAPIS_MIC_S16LE_STEREO ?
            lapis_mic_s16le_stereo_to_mono16(ring + offset, (size_t)count * frame, p->channel, samples, count) :
            lapis_mic_s24le32_stereo_to_mono16(ring + offset, (size_t)count * frame, p->channel, samples, count);
        uint32_t accepted = lapis_mic_queue_push(&p->queue, samples, (uint32_t)made);
        p->accepted += accepted;
        p->dropped += made - accepted;
        volatile int16_t *wipe = samples;
        for (unsigned i = 0; i < 256; i++) wipe[i] = 0;
        offset += count * frame;
        bytes -= count * frame;
    }
}

/* DMA bytes must be CPU-visible before this call. The ring must stay mapped and
 * spans must not be overwritten while copying. A cursor fault wipes the queue. */
static inline enum lapis_mic_cursor_result
lapis_mic_pipeline_feed(struct lapis_mic_pipeline *p, const uint8_t *ring, size_t ring_bytes,
    uint64_t current, uint64_t now_ns)
{
    if (!p || !p->active) return LAPIS_MIC_CURSOR_INVALID;
    if (!ring || ring_bytes != p->cursor.size) {
        p->active = 0; lapis_mic_queue_reset(&p->queue); return LAPIS_MIC_CURSOR_INVALID;
    }
    struct lapis_mic_spans s;
    enum lapis_mic_cursor_result r = lapis_mic_cursor_observe(&p->cursor, current, now_ns, &s);
    if (r != LAPIS_MIC_CURSOR_OK) { p->active = 0; lapis_mic_queue_reset(&p->queue); return r; }
    if (s.offset1 > p->cursor.size || s.bytes1 > p->cursor.size - s.offset1 ||
        s.offset2 > p->cursor.size || s.bytes2 > p->cursor.size - s.offset2 ||
        s.bytes1 % p->cursor.frame || s.bytes2 % p->cursor.frame) {
        p->active = 0; lapis_mic_queue_reset(&p->queue); return LAPIS_MIC_CURSOR_INVALID;
    }
    lapis_mic_pipeline_copy_span(p, ring, s.offset1, s.bytes1);
    lapis_mic_pipeline_copy_span(p, ring, s.offset2, s.bytes2);
    return LAPIS_MIC_CURSOR_OK;
}

static inline uint32_t lapis_mic_pipeline_read(struct lapis_mic_pipeline *p, int16_t *out, uint32_t frames)
{
    if (!p || !p->active || !out) return 0;
    return lapis_mic_queue_pop(&p->queue, out, frames);
}
static inline void lapis_mic_pipeline_stop(struct lapis_mic_pipeline *p)
{
    if (!p) return;
    p->active = 0;
    lapis_mic_queue_reset(&p->queue);
}
#endif
