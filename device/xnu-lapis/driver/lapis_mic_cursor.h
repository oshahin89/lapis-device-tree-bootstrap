/* Read-side DMA cursor accounting, independent of hardware. Callers serialize
 * access, verify the configured rate and copy offered bytes immediately. */
#ifndef LAPIS_MIC_CURSOR_H
#define LAPIS_MIC_CURSOR_H
#include <stdint.h>

struct lapis_mic_cursor {
    uint64_t base, last_ns, max_gap_ns;
    uint32_t size, frame, guard, writer, reader, pending;
    int faulted;
};
struct lapis_mic_spans { uint32_t offset1, bytes1, offset2, bytes2; };

enum lapis_mic_cursor_result {
    LAPIS_MIC_CURSOR_OK = 0,
    LAPIS_MIC_CURSOR_INVALID,
    LAPIS_MIC_CURSOR_LATE,
    LAPIS_MIC_CURSOR_OVERRUN
};

/* max_gap_ns must be strictly shorter than a full ring traversal at the
 * configured DMA rate. The caller must configure and verify that rate. */
static inline enum lapis_mic_cursor_result
lapis_mic_cursor_init(struct lapis_mic_cursor *c, uint64_t base, uint32_t size,
    uint32_t frame, uint32_t guard, uint64_t initial, uint64_t now_ns, uint64_t max_gap_ns)
{
    if (!c || !frame || !size || size % frame || guard >= size || guard % frame ||
        !max_gap_ns || base + size < base || initial < base || initial >= base + size ||
        (initial - base) % frame) return LAPIS_MIC_CURSOR_INVALID;
    *c = (struct lapis_mic_cursor){ .base = base, .last_ns = now_ns, .max_gap_ns = max_gap_ns,
        .size = size, .frame = frame, .guard = guard, .writer = (uint32_t)(initial - base),
        .reader = (uint32_t)(initial - base), .pending = 0, .faulted = 0 };
    return LAPIS_MIC_CURSOR_OK;
}

/* Offsets are relative to the ring. The most recent guard bytes are withheld.
 * A late/invalid cursor permanently faults this session rather than exposing
 * potentially overwritten or stale samples. */
static inline enum lapis_mic_cursor_result
lapis_mic_cursor_observe(struct lapis_mic_cursor *c, uint64_t current, uint64_t now_ns,
    struct lapis_mic_spans *spans)
{
    if (!c || !spans || c->faulted || !c->size || !c->frame || current < c->base ||
        current >= c->base + c->size || (current - c->base) % c->frame) {
        if (c) c->faulted = 1;
        return LAPIS_MIC_CURSOR_INVALID;
    }
    *spans = (struct lapis_mic_spans){0};
    if (now_ns < c->last_ns || now_ns - c->last_ns >= c->max_gap_ns) {
        c->faulted = 1;
        return LAPIS_MIC_CURSOR_LATE;
    }
    uint32_t writer = (uint32_t)(current - c->base);
    uint32_t advance = (uint32_t)(((uint64_t)writer + c->size - c->writer) % c->size);
    if (advance && advance >= c->size - c->pending) {
        c->faulted = 1;
        return LAPIS_MIC_CURSOR_OVERRUN;
    }
    c->pending += advance;
    c->writer = writer;
    c->last_ns = now_ns;
    uint32_t ready = c->pending > c->guard ? c->pending - c->guard : 0;
    spans->offset1 = c->reader;
    spans->bytes1 = ready < c->size - c->reader ? ready : c->size - c->reader;
    spans->offset2 = 0;
    spans->bytes2 = ready - spans->bytes1;
    c->reader = (uint32_t)(((uint64_t)c->reader + ready) % c->size);
    c->pending -= ready;
    return LAPIS_MIC_CURSOR_OK;
}
#endif
