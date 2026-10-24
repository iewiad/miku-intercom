#pragma once

#include <stddef.h>
#include <stdint.h>

/* Compile-time tuning only. Gain must be 1..16; peak limit must be 1..32767.
 * Defaults: +18.06 dB for weak input, with a whole-frame peak limit of 0.9 FS.
 * The Q12 arithmetic and its signed intermediate are bounded by these ranges.
 */
#ifndef GROUP_AUDIO_LEVEL_GAIN
#define GROUP_AUDIO_LEVEL_GAIN 8U
#endif
#ifndef GROUP_AUDIO_LEVEL_PEAK_LIMIT
#define GROUP_AUDIO_LEVEL_PEAK_LIMIT 29490U
#endif

/* In-place, stateless uniform scaling of exactly one encoder frame. Never pass
 * a larger accumulator: its remaining samples must not be amplified twice.
 * Zero input remains zero; background noise receives the same bounded gain.
 * No VAD, noise gate, history, or audio is retained.
 * Caller owns group-mode/grant checks. NULL or zero samples is a no-op.
 */
void group_audio_level_apply(int16_t *pcm, size_t samples);
