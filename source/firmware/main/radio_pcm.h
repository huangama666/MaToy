#pragma once

#include <stddef.h>
#include <stdint.h>

// Downmix in place. Invalid PCM framing is rejected before any samples are read.
static inline size_t radio_pcm_mono(int16_t *pcm, size_t bytes, unsigned channels)
{
    if ((channels != 1 && channels != 2) || bytes % (sizeof(*pcm) * channels)) return 0;
    const size_t count = bytes / (sizeof(*pcm) * channels);
    if (channels == 2) {
        for (size_t i = 0; i < count; ++i) {
            pcm[i] = ((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2;
        }
    }
    return count * sizeof(*pcm);
}
