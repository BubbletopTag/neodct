/* nd_notify.h -- the banner at the top of the home screen, and the ringer.
 *
 * Two jobs that share an owner because they share the sound card: telling you
 * a text arrived, and playing the ringtone when a call does.
 *
 * ============ WHY THE RINGER'S OWNERSHIP MATTERS ============
 *
 * In Python an incoming call raises IncomingCall, the exception unwinds
 * through the running app, and THAT UNWINDING RUNS THE APP'S finally: BLOCK,
 * which is what releases ALSA before the ringtone starts. There is no
 * exception in C. If the app's teardown does not run, the sound card is still
 * busy and the phone rings silently.
 *
 * That is why app_shutdown() is mandatory in nd_app.h, why the core waits for
 * the child before starting the ringer, and why it escalates to SIGKILL rather
 * than waiting forever.
 */

#ifndef ND_NOTIFY_H_INCLUDED
#define ND_NOTIFY_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_TONES_DIR    "/NeoDCT/System/tones"
#define ND_SMS_TONE     "/NeoDCT/System/tones/sms.wav"
#define ND_RING_RATE    44100
#define ND_RING_BUF_MS  500
#define ND_RING_SETTING "system.audio.ringtone"

/* Tried in order when the configured ringtone cannot be played. ORDER IS
 * LOAD-BEARING. Note this walk is only reachable when settings.prop points at
 * a file that does not exist AND the same-stem extension retry also misses --
 * because system.audio.ringtone has a DEFAULTS entry, get_setting can never
 * return "". Do not simplify the chain away. */
#define ND_RING_FALLBACK_COUNT 4
extern const char *const ND_RING_FALLBACKS[ND_RING_FALLBACK_COUNT];

/* Extensions retried against the configured stem, then the last-resort sweep
 * of the tones directory. */
#define ND_RING_EXT_RETRY_COUNT 5
extern const char *const ND_RING_EXT_RETRY[ND_RING_EXT_RETRY_COUNT];
#define ND_RING_SWEEP_EXT_COUNT 3
extern const char *const ND_RING_SWEEP_EXT[ND_RING_SWEEP_EXT_COUNT];

#define ND_NOTIFY_KIND_SMS "sms"
#define ND_NOTIFY_LINE_MAX 32

typedef struct nd_notify nd_notify;

nd_err nd_notify_open(nd_notify **out);
void nd_notify_close(nd_notify *n);

/* A text arrived. row_id is the inbox rowid so the banner can open it;
 * tone false suppresses the sound (used when replaying at boot). */
void nd_notify_post_sms(nd_notify *n, int64_t row_id, bool tone);

bool nd_notify_active(const nd_notify *n);
const char *nd_notify_kind(const nd_notify *n); /* ND_NOTIFY_KIND_SMS or NULL */
int32_t nd_notify_count(const nd_notify *n);
int64_t nd_notify_latest_data(const nd_notify *n); /* -1 for none */

/* Zero or two lines into caller-owned buffers. Returns how many were written. */
size_t nd_notify_banner_lines(const nd_notify *n, char l1[ND_NOTIFY_LINE_MAX],
                              char l2[ND_NOTIFY_LINE_MAX]);

void nd_notify_dismiss(nd_notify *n);

/* One-shot sound, e.g. the SMS chirp or a DTMF tone. */
bool nd_notify_play_tone(nd_notify *n, const char *path);

/* The ringer. start_ring resolves the ringtone through the fallback chain
 * above; ringtone_path reports what it settled on, NULL when nothing played. */
bool nd_notify_start_ring(nd_notify *n);
void nd_notify_stop_ring(nd_notify *n);
bool nd_notify_ringing(const nd_notify *n);
const char *nd_notify_ringtone_path(nd_notify *n);

#ifdef __cplusplus
}
#endif

#endif /* ND_NOTIFY_H_INCLUDED */
