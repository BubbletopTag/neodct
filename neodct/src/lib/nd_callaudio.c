/* nd_callaudio.c -- the call speaker's 3 dB software-volume ladder.
 *
 * This is deliberately separate from the ALSA mixer. Calls, music, tones and
 * the ringtone all use the same physical output, so moving a hardware control
 * for a call would leave everything else at the call level. Only the PCM
 * samples owned by nd-callplay pass through here; when that process exits,
 * normal speaker volume is restored by construction. */

#include "nd_callaudio.h"

#include <errno.h>
#include <stdlib.h>

/* 0..10 in 3 dB steps, with level 10 at unity. Rounded Q15 values of
 * 10^(-dB/20). The same perceptually even ladder Music uses. */
static const int32_t GAINS_Q15[ND_CALL_VOLUME_MAX + 1] = {
    1036, 1464, 2068, 2922, 4127, 5830, 8235, 11633, 16423, 23198, 32768,
};

static int32_t clamp_level(int32_t level)
{
    if (level < ND_CALL_VOLUME_MIN)
        return ND_CALL_VOLUME_MIN;
    if (level > ND_CALL_VOLUME_MAX)
        return ND_CALL_VOLUME_MAX;
    return level;
}

int32_t nd_call_volume_from_setting(const char *value)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0')
        return ND_CALL_VOLUME_DEFAULT;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || end == NULL || *end != '\0' || parsed < ND_CALL_VOLUME_MIN ||
        parsed > ND_CALL_VOLUME_MAX)
        return ND_CALL_VOLUME_DEFAULT;
    return (int32_t)parsed;
}

int32_t nd_call_volume_gain_q15(int32_t level)
{
    return GAINS_Q15[clamp_level(level)];
}

int16_t nd_call_volume_apply_gain(int16_t sample, int32_t level)
{
    int32_t scaled;

    if (level >= ND_CALL_VOLUME_MAX)
        return sample;
    scaled = ((int32_t)sample * nd_call_volume_gain_q15(level)) / 32768;
    if (scaled > INT16_MAX)
        scaled = INT16_MAX;
    if (scaled < INT16_MIN)
        scaled = INT16_MIN;
    return (int16_t)scaled;
}

void nd_call_volume_gain_buffer(int16_t *samples, size_t n_samples, int32_t level)
{
    size_t i;

    if (samples == NULL || level >= ND_CALL_VOLUME_MAX)
        return;
    for (i = 0u; i < n_samples; i++)
        samples[i] = nd_call_volume_apply_gain(samples[i], level);
}
