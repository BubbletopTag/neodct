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

/* The second kind, and the reason `kind` was a string rather than a bool.
 * A calendar reminder comes up the same way a text does -- same banner, same
 * two lines, same Read-or-Clear on the home screen -- because that is the one
 * "something happened" idiom this phone has and inventing a second one would
 * make the first one mean less. */
#define ND_NOTIFY_KIND_EVENT "event"

/* The phone owns exactly one short chirp. A reminder shares it rather than
 * ringing silently or borrowing a 36-second ringtone; a second asset is the
 * only thing that would separate them, and the banner is what carries the
 * meaning. */
#define ND_NOTIFY_EVENT_TONE ND_SMS_TONE

#define ND_NOTIFY_LINE_MAX 32

typedef struct nd_notify nd_notify;

nd_err nd_notify_open(nd_notify **out);
void nd_notify_close(nd_notify *n);

/* A text arrived. row_id is the inbox rowid so the banner can open it;
 * tone false suppresses the sound (used when replaying at boot). */
void nd_notify_post_sms(nd_notify *n, int64_t row_id, bool tone);

/* A calendar reminder came due. row_id is the event's rowid so the banner
 * can open it, title is what the first line says and `when` is the
 * occurrence, whose clock reading is the second line.
 *
 * A title that is empty or NULL reads "Reminder": a banner is never blank.
 *
 * ============ THE BANNER SHOWS ONE KIND AT A TIME ============
 *
 * Posting a reminder while a text banner is up REPLACES it, and the count
 * starts again from one (and the same the other way round). One banner, the
 * newest news. Nothing is lost by it -- the text is still unread in the inbox
 * with the envelope flashing over it, and the appointment is still in the
 * calendar -- and the alternative is a home screen that has to say two things
 * in two lines, which is what the 3310 refused to do too. */
void nd_notify_post_event(nd_notify *n, int64_t row_id, const char *title, int64_t when, bool tone);

bool nd_notify_active(const nd_notify *n);
/* ND_NOTIFY_KIND_SMS, ND_NOTIFY_KIND_EVENT, or NULL. Compare with strcmp:
 * the pointer is one of the two literals above, but a caller that relies on
 * that is one refactor from being wrong. */
const char *nd_notify_kind(const nd_notify *n);
int32_t nd_notify_count(const nd_notify *n);
int64_t nd_notify_latest_data(const nd_notify *n); /* -1 for none */

/* Zero or two lines into caller-owned buffers. Returns how many were written.
 *
 *   sms,   one     "1 message"      / "received"
 *   sms,   many    "3 messages"     / "received"
 *   event, one     "Dentist"        / "10:30 am"
 *   event, many    "3 reminders"    / "due"
 *
 * The plural shape is the same in both, deliberately: when more than one
 * thing has happened the banner counts them and the app shows the list. */
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
