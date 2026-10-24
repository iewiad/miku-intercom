#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "group_audio_level.h"

static int32_t magnitude(int16_t sample)
{
    const int32_t wide = sample;
    return wide < 0 ? -wide : wide;
}

int main(void)
{
    int16_t weak[] = {-100, -17, -1, 0, 1, 17, 100};
    group_audio_level_apply(weak, sizeof(weak) / sizeof(*weak));
    const int16_t expected[] = {-800, -136, -8, 0, 8, 136, 800};
    assert(memcmp(weak, expected, sizeof(weak)) == 0);

    int16_t silence[320] = {0};
    group_audio_level_apply(silence, 320);
    for (size_t index = 0; index < 320; ++index) assert(silence[index] == 0);
    group_audio_level_apply(NULL, 0);
    int16_t untouched = 123;
    group_audio_level_apply(&untouched, 0);
    assert(untouched == 123);

    int16_t extremes[] = {INT16_MIN, INT16_MAX, -16384, 16384, 0};
    group_audio_level_apply(extremes, 5);
    assert(extremes[0] < -29480 && extremes[1] > 29480);
    for (size_t index = 0; index < 5; ++index)
        assert(magnitude(extremes[index]) <= (int32_t)GROUP_AUDIO_LEVEL_PEAK_LIMIT);
    assert(magnitude(extremes[0]) - 2 * magnitude(extremes[2]) <= 1);
    assert(extremes[2] == -extremes[3] && extremes[4] == 0);

    // A loud crest must lower the gain for the complete frame, not flatten
    // individual peaks. Half-amplitude samples remain half-amplitude.
    int16_t crest[] = {-10000, -5000, 0, 5000, 10000};
    group_audio_level_apply(crest, 5);
    assert(crest[4] >= 29480 && crest[4] <= (int32_t)GROUP_AUDIO_LEVEL_PEAK_LIMIT);
    assert(crest[0] == -crest[4] && crest[1] == -crest[3] && crest[2] == 0);
    assert(crest[4] - 2 * crest[3] >= 0 && crest[4] - 2 * crest[3] <= 1);

    int16_t threshold[] = {3686, -3686};
    group_audio_level_apply(threshold, 2);
    assert(threshold[0] == 29488 && threshold[1] == -29488);
    int16_t above[] = {3687, -3687};
    group_audio_level_apply(above, 2);
    assert(above[0] >= 29480 && above[0] <= (int32_t)GROUP_AUDIO_LEVEL_PEAK_LIMIT);
    assert(above[0] == -above[1]);

    // An earlier loud frame or another PTT window cannot change a later quiet
    // frame's gain. No normalization state may leak between calls.
    int16_t later[] = {-100, -17, -1, 0, 1, 17, 100};
    group_audio_level_apply(later, 7);
    assert(memcmp(later, expected, sizeof(later)) == 0);

    // The caller processes only the next encoder frame; the pending tail must
    // remain byte-identical for its own subsequent call.
    int16_t accumulator[] = {100, -100, 200, -200};
    group_audio_level_apply(accumulator, 2);
    assert(accumulator[0] == 800 && accumulator[1] == -800);
    assert(accumulator[2] == 200 && accumulator[3] == -200);
    group_audio_level_apply(accumulator + 2, 2);
    assert(accumulator[2] == 1600 && accumulator[3] == -1600);

    puts("group audio level: passed (8x weak input, zero, INT16_MIN, peak limit, uniform ratio, no history, frame boundary)");
    return 0;
}
