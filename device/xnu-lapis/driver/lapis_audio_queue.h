/* Bounded stereo PCM queue. The caller serializes all operations. */
#ifndef LAPIS_AUDIO_QUEUE_H
#define LAPIS_AUDIO_QUEUE_H
#include <stdint.h>

#define LAPIS_AUDIO_QUEUE_FRAMES 8192u
struct lapis_audio_queue {
	uint32_t frames[LAPIS_AUDIO_QUEUE_FRAMES];
	uint32_t read_pos, write_pos, count;
};

/* Accepts complete packed stereo frames; drops the newest excess frames. */
static inline uint32_t
lapis_audio_queue_push(struct lapis_audio_queue *q, const uint32_t *frames, uint32_t count)
{
	uint32_t accepted = 0;
	while (accepted < count && q->count < LAPIS_AUDIO_QUEUE_FRAMES) {
		q->frames[q->write_pos] = frames[accepted++];
		q->write_pos = (q->write_pos + 1) % LAPIS_AUDIO_QUEUE_FRAMES;
		q->count++;
	}
	return accepted;
}

/* Returns real frames read and fills an underrun with silence. */
static inline uint32_t
lapis_audio_queue_pop(struct lapis_audio_queue *q, uint32_t *frames, uint32_t count)
{
	uint32_t read = 0;
	while (read < count && q->count) {
		frames[read++] = q->frames[q->read_pos];
		q->read_pos = (q->read_pos + 1) % LAPIS_AUDIO_QUEUE_FRAMES;
		q->count--;
	}
	for (uint32_t i = read; i < count; i++) frames[i] = 0;
	return read;
}
#endif
