/* nd_callaudio.h -- software gain for the call-only speaker path. */

#ifndef ND_CALLAUDIO_H_INCLUDED
#define ND_CALLAUDIO_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#define ND_CALL_VOLUME_MIN     0
#define ND_CALL_VOLUME_MAX     10
#define ND_CALL_VOLUME_DEFAULT 8
#define ND_CALL_VOLUME_SETTING "phone.call_volume"

int32_t nd_call_volume_from_setting(const char *value);
int32_t nd_call_volume_gain_q15(int32_t level);
int16_t nd_call_volume_apply_gain(int16_t sample, int32_t level);
void nd_call_volume_gain_buffer(int16_t *samples, size_t n_samples, int32_t level);

#endif /* ND_CALLAUDIO_H_INCLUDED */
