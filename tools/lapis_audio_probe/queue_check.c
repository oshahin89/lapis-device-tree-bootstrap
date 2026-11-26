#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../device/xnu-lapis/driver/lapis_audio_queue.h"

static struct lapis_audio_queue q;
static uint32_t input[LAPIS_AUDIO_QUEUE_FRAMES + 2], output[LAPIS_AUDIO_QUEUE_FRAMES + 2];

int main(void)
{
	for (uint32_t i = 0; i < LAPIS_AUDIO_QUEUE_FRAMES + 2; i++) input[i] = i + 1;
	assert(lapis_audio_queue_push(&q, input, LAPIS_AUDIO_QUEUE_FRAMES + 2) == LAPIS_AUDIO_QUEUE_FRAMES);
	assert(lapis_audio_queue_pop(&q, output, 100) == 100);
	for (unsigned i = 0; i < 100; i++) assert(output[i] == input[i]);
	assert(lapis_audio_queue_push(&q, input, 100) == 100);
	assert(lapis_audio_queue_pop(&q, output, LAPIS_AUDIO_QUEUE_FRAMES + 2) == LAPIS_AUDIO_QUEUE_FRAMES);
	for (uint32_t i = 0; i < LAPIS_AUDIO_QUEUE_FRAMES - 100; i++) assert(output[i] == input[i + 100]);
	for (unsigned i = 0; i < 100; i++) assert(output[LAPIS_AUDIO_QUEUE_FRAMES - 100 + i] == input[i]);
	assert(output[LAPIS_AUDIO_QUEUE_FRAMES] == 0 && output[LAPIS_AUDIO_QUEUE_FRAMES + 1] == 0);
	assert(q.count == 0);
	puts("PCM queue: order, wrap, overflow and underrun passed");
	return 0;
}
