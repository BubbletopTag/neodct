/* nd-callplay -- the small, call-only replacement for `aplay`.
 *
 * fd 3 is a non-blocking control pipe. Each byte is the newest 0..10 volume;
 * draining it before each 20 ms PCM block makes repeated keys collapse to the
 * last value instead of queueing slow mixer subprocesses. */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "nd_callaudio.h"

#define CONTROL_FD          3
#define FRAMES_PER_BLOCK    320
#define PLAYBACK_LATENCY_US 80000u

static void drain_level(int32_t *level)
{
    uint8_t values[32];
    ssize_t n;

    for (;;) {
        n = read(CONTROL_FD, values, sizeof values);
        if (n > 0) {
            uint8_t newest = values[(size_t)n - 1u];

            if (newest <= ND_CALL_VOLUME_MAX)
                *level = (int32_t)newest;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return;
    }
}

static int write_frames(snd_pcm_t *pcm, int16_t *samples, snd_pcm_sframes_t frames)
{
    snd_pcm_sframes_t done = 0;

    while (done < frames) {
        snd_pcm_sframes_t n =
            snd_pcm_writei(pcm, samples + done, (snd_pcm_uframes_t)(frames - done));

        if (n == -EINTR)
            continue;
        if (n < 0) {
            int recovered = snd_pcm_recover(pcm, (int)n, 1);

            if (recovered < 0)
                return recovered;
            continue;
        }
        done += n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int16_t samples[FRAMES_PER_BLOCK];
    uint8_t *bytes = (uint8_t *)samples;
    snd_pcm_t *pcm = NULL;
    char *end = NULL;
    long rate;
    long initial;
    int input = -1;
    int32_t level;
    size_t carry = 0u;
    uint8_t tail = 0u;
    int rc = 1;

    if (argc != 4)
        return 2;
    rate = strtol(argv[2], &end, 10);
    if (end == argv[2] || end == NULL || *end != '\0' || rate < 8000 || rate > 48000)
        return 2;
    end = NULL;
    initial = strtol(argv[3], &end, 10);
    if (end == argv[3] || end == NULL || *end != '\0' || initial < ND_CALL_VOLUME_MIN ||
        initial > ND_CALL_VOLUME_MAX)
        return 2;
    level = (int32_t)initial;

    input = open(argv[1], O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (input < 0)
        goto done;
    (void)fcntl(CONTROL_FD, F_SETFL, fcntl(CONTROL_FD, F_GETFL) | O_NONBLOCK);

    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        goto done;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1u,
                           (unsigned)rate, 1, PLAYBACK_LATENCY_US) < 0)
        goto done;

    for (;;) {
        ssize_t got;
        size_t total;
        size_t even;

        if (carry != 0u)
            bytes[0] = tail;
        got = read(input, bytes + carry, sizeof samples - carry);

        if (got == 0) {
            rc = 0;
            break;
        }
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        total = carry + (size_t)got;
        even = total & ~(sizeof(int16_t) - 1u);
        carry = total - even;
        if (carry != 0u)
            tail = bytes[even];
        if (even == 0u)
            continue;
        drain_level(&level);
        nd_call_volume_gain_buffer(samples, even / sizeof(int16_t), level);
        if (write_frames(pcm, samples, (snd_pcm_sframes_t)(even / sizeof(int16_t))) < 0)
            break;
    }

done:
    if (pcm != NULL) {
        (void)snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    if (input >= 0)
        (void)close(input);
    return rc;
}
