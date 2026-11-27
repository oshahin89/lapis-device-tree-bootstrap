#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_mic_queue.h"
static struct lapis_mic_queue q;
static int16_t in[LAPIS_MIC_QUEUE_FRAMES + 3], out[LAPIS_MIC_QUEUE_FRAMES + 3];
int main(void)
{
    for (unsigned i = 0; i < LAPIS_MIC_QUEUE_FRAMES + 3; i++) in[i] = (int16_t)(i + 1);
    lapis_mic_queue_reset(&q);
    assert(lapis_mic_queue_push(&q, in, LAPIS_MIC_QUEUE_FRAMES + 3) == LAPIS_MIC_QUEUE_FRAMES);
    assert(lapis_mic_queue_pop(&q, out, 99) == 99);
    for (unsigned i = 0; i < 99; i++) assert(out[i] == in[i]);
    assert(lapis_mic_queue_push(&q, in, 99) == 99);
    assert(lapis_mic_queue_pop(&q, out, LAPIS_MIC_QUEUE_FRAMES + 3) == LAPIS_MIC_QUEUE_FRAMES);
    for (unsigned i = 0; i < LAPIS_MIC_QUEUE_FRAMES - 99; i++) assert(out[i] == in[i + 99]);
    for (unsigned i = 0; i < 99; i++) assert(out[LAPIS_MIC_QUEUE_FRAMES - 99 + i] == in[i]);
    assert(lapis_mic_queue_pop(&q, out, 1) == 0);
    assert(lapis_mic_queue_push(&q, in, 20) == 20);
    lapis_mic_queue_reset(&q);
    assert(q.count == 0 && q.read_pos == 0 && q.write_pos == 0);
    for (unsigned i = 0; i < LAPIS_MIC_QUEUE_FRAMES; i++) assert(q.samples[i] == 0);
    puts("Microphone queue ordering, wrap, overflow, short read and erase passed");
    return 0;
}
