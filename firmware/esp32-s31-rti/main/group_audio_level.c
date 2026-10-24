#include "group_audio_level.h"

#define LEVEL_Q 4096U

_Static_assert(GROUP_AUDIO_LEVEL_GAIN >= 1 && GROUP_AUDIO_LEVEL_GAIN <= 16,
               "group gain must fit the signed Q12 intermediate (1..16)");
_Static_assert(GROUP_AUDIO_LEVEL_PEAK_LIMIT >= 1 && GROUP_AUDIO_LEVEL_PEAK_LIMIT <= 32767,
               "group peak limit must be 1..32767");

void group_audio_level_apply(int16_t *pcm, size_t samples)
{
    if (pcm == NULL || samples == 0) return;

    uint32_t peak = 0;
    for (size_t index = 0; index < samples; ++index) {
        const int32_t sample = pcm[index];
        const uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
        if (magnitude > peak) peak = magnitude;
    }
    if (peak == 0) return;

    uint32_t gain_q12 = GROUP_AUDIO_LEVEL_GAIN * LEVEL_Q;
    const uint32_t peak_gain_q12 = GROUP_AUDIO_LEVEL_PEAK_LIMIT * LEVEL_Q / peak;
    if (peak_gain_q12 < gain_q12) gain_q12 = peak_gain_q12;

    for (size_t index = 0; index < samples; ++index) {
        // The static gain bound keeps this product within int32, including
        // INT16_MIN. One common gain preserves the waveform; truncation toward
        // zero and a downward-rounded gain keep every sample below the limit.
        const int32_t sample = pcm[index];
        pcm[index] = (int16_t)((sample * (int32_t)gain_q12) / (int32_t)LEVEL_Q);
    }
}
