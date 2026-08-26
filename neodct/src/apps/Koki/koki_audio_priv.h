/* koki_audio_priv.h -- the mixer's sink, exposed to one unit test.
 *
 * ============ WHY THIS HEADER EXISTS ============
 *
 * koki.h is what the game is written against and the sink has no business
 * being in it: the game asks for a sound, it does not start players. But the
 * sink is also the half of the audio path most able to go wrong on the phone
 * -- a socket sized by the kernel's floor, a fork that must happen before
 * the feeder thread exists, an EPIPE that must unwind it, and a teardown
 * whose ORDER decides whether a descriptor gets recycled underneath a
 * blocked send().
 *
 * None of that can be reached through koki_sound_open() on a development
 * machine, because that path stops at "/dev/snd missing" before it gets
 * anywhere near a socket. So the three entry points are declared here and
 * test_koki_audio.c drives them against a fake player it puts on PATH. This
 * is lib/'s own nd_*_priv.h pattern (nd_notify_priv.h, nd_image_priv.h),
 * applied to an app.
 *
 * Nothing outside apps/Koki and test/unit may include this.
 */

#ifndef KOKI_AUDIO_PRIV_H_INCLUDED
#define KOKI_AUDIO_PRIV_H_INCLUDED

#include <sys/types.h>

#include "koki.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn one `aplay` reading raw PCM on stdin and start the feeder thread
 * that drives `m`. `why` receives a short reason on failure.
 *
 * THE FORK IS IN HERE AND IT HAPPENS BEFORE THE THREAD (CODING-STANDARDS
 * 1.1). Every failure path leaves nothing running and nothing to join.
 *
 * owned by the caller; release with koki_sink_stop() */
struct koki_sink *koki_sink_start(koki_mixer *m, const char **why);

/* Kill the player, unwind the feeder, close the socket -- in that order,
 * which is the only order that works. Safe on NULL. */
void koki_sink_stop(struct koki_sink *s);

/* What the kernel actually granted, in payload bytes, and what aplay was
 * actually asked for. Both are inputs to koki_mix_latency_ms(), and both are
 * environment- and kernel-dependent, which is why they are read back rather
 * than assumed. */
int32_t koki_sink_sock_bytes(const struct koki_sink *s);
int32_t koki_sink_alsa_ms(const struct koki_sink *s);

#ifdef __cplusplus
}
#endif

#endif /* KOKI_AUDIO_PRIV_H_INCLUDED */
