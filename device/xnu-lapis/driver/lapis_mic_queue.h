/* Bounded mono S16 capture queue. Callers hold one lock around each operation.
 * It contains sensitive PCM: reset wipes every sample. */
#ifndef LAPIS_MIC_QUEUE_H
#define LAPIS_MIC_QUEUE_H
#include <stdint.h>
#define LAPIS_MIC_QUEUE_FRAMES 8192u
struct lapis_mic_queue {
    int16_t samples[LAPIS_MIC_QUEUE_FRAMES];
    uint32_t read_pos, write_pos, count;
};
static inline void lapis_mic_queue_reset(struct lapis_mic_queue *q)
{
    volatile int16_t *samples = q->samples;
    for (uint32_t i = 0; i < LAPIS_MIC_QUEUE_FRAMES; ++i) samples[i] = 0;
    q->read_pos = q->write_pos = q->count = 0;
}
/* Drop incoming samples once full; never overwrite unread audio. */
static inline uint32_t lapis_mic_queue_push(struct lapis_mic_queue *q, const int16_t *in, uint32_t n)
{
    uint32_t accepted = 0;
    while (accepted < n && q->count < LAPIS_MIC_QUEUE_FRAMES) {
        q->samples[q->write_pos] = in[accepted++];
        q->write_pos = (q->write_pos + 1) % LAPIS_MIC_QUEUE_FRAMES;
        q->count++;
    }
    return accepted;
}
/* The caller determines whether to return a short read or add silence. */
static inline uint32_t lapis_mic_queue_pop(struct lapis_mic_queue *q, int16_t *out, uint32_t n)
{
    uint32_t read = 0;
    while (read < n && q->count) {
        out[read++] = q->samples[q->read_pos];
        q->samples[q->read_pos] = 0;
        q->read_pos = (q->read_pos + 1) % LAPIS_MIC_QUEUE_FRAMES;
        q->count--;
    }
    return read;
}
#endif
