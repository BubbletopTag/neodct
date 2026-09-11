/* test_callaudio.c -- the call-only software gain and its saved setting. */

#include <stdint.h>
#include <stdio.h>

#include "nd_callaudio.h"

static int failures;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

int main(void)
{
    int16_t samples[] = {32767, -32768, 1000, -1000};

    CHECK(nd_call_volume_from_setting(NULL) == ND_CALL_VOLUME_DEFAULT);
    CHECK(nd_call_volume_from_setting("") == ND_CALL_VOLUME_DEFAULT);
    CHECK(nd_call_volume_from_setting("8") == 8);
    CHECK(nd_call_volume_from_setting("-1") == ND_CALL_VOLUME_DEFAULT);
    CHECK(nd_call_volume_from_setting("11") == ND_CALL_VOLUME_DEFAULT);
    CHECK(nd_call_volume_from_setting("8x") == ND_CALL_VOLUME_DEFAULT);

    CHECK(nd_call_volume_gain_q15(10) == 32768);
    CHECK(nd_call_volume_gain_q15(8) == 16423);
    CHECK(nd_call_volume_gain_q15(-1) == nd_call_volume_gain_q15(0));
    CHECK(nd_call_volume_gain_q15(99) == nd_call_volume_gain_q15(10));
    CHECK(nd_call_volume_apply_gain(12345, 10) == 12345);

    nd_call_volume_gain_buffer(samples, sizeof samples / sizeof samples[0], 8);
    CHECK(samples[0] > 16300 && samples[0] < 16500);
    CHECK(samples[1] < -16300 && samples[1] > -16500);
    CHECK(samples[2] > 490 && samples[2] < 510);
    CHECK(samples[3] < -490 && samples[3] > -510);

    if (failures != 0)
        return 1;
    puts("test_callaudio: all tests passed");
    return 0;
}
