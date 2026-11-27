/* Explicit candidate converters for interleaved little-endian stereo capture.
 * Use only after the hardware slot format and selected microphone channel have
 * been verified. These functions do not access hardware or retain samples. */
#ifndef LAPIS_MIC_PCM_H
#define LAPIS_MIC_PCM_H
#include <stddef.h>
#include <stdint.h>

/* Decode low-justified signed 24-bit samples in four-byte little-endian slots.
 * Android's observed HD mode is consistent with this candidate, but it has not
 * been verified on Lapis hardware. */
static inline size_t lapis_mic_s24le32_stereo_to_mono16(const uint8_t *src, size_t bytes,
    unsigned channel, int16_t *dst, size_t capacity)
{
    if (!src || !dst || channel > 1) return 0;
    size_t frames = bytes / 8;
    if (frames > capacity) frames = capacity;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *p = src + i * 8 + channel * 4;
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        int32_t signed24 = u & 0x800000u ? (int32_t)u - 0x1000000 : (int32_t)u;
        dst[i] = (int16_t)(signed24 / 256);
    }
    return frames;
}

/* Signed 16-bit little-endian stereo to one mono channel. */
static inline size_t lapis_mic_s16le_stereo_to_mono16(const uint8_t *src, size_t bytes,
    unsigned channel, int16_t *dst, size_t capacity)
{
    if (!src || !dst || channel > 1) return 0;
    size_t frames = bytes / 4;
    if (frames > capacity) frames = capacity;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *p = src + i * 4 + channel * 2;
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        dst[i] = (int16_t)(u & 0x8000u ? (int32_t)u - 0x10000 : (int32_t)u);
    }
    return frames;
}
#endif
