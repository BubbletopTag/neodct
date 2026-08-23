/* nd_clock.h -- what time it is, and how we found out.
 *
 * This runs FIRST at boot, before anything can reach the network, because a
 * clock stuck in 1970 fails every TLS "not valid before" check and the update
 * system would then be unable to verify anything.
 *
 * The floor is the build epoch from version.prop: the image cannot honestly
 * have been built after the current time, so if the RTC says earlier than the
 * build, the RTC is wrong and we set the clock forward. Then NTP refines it in
 * the background.
 *
 * These are free functions with no object, exactly as the Python has them.
 */

#ifndef ND_CLOCK_H_INCLUDED
#define ND_CLOCK_H_INCLUDED

#include <time.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_NTP_EPOCH_OFFSET    2208988800u /* 1900-01-01 -> 1970-01-01 */
#define ND_NTP_QUERY_TIMEOUT_S 5
#define ND_NTP_PORT            123
#define ND_CLOCK_SANE_MIN      1577836800 /* 2020-01-01 */
#define ND_CLOCK_SANE_MAX      4102444800 /* 2100-01-01 */
#define ND_CLOCK_ROUTE_TRIES   60         /* x 5 s = five minutes */
#define ND_CLOCK_ROUTE_SLEEP_S 5

#define ND_NTP_SERVER_COUNT 3
extern const char *const ND_NTP_SERVERS[ND_NTP_SERVER_COUNT];
/* { "0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org" } */

/* system.os.buildepoch, read DIRECTLY from version.prop rather than through
 * the settings layering -- ClockService runs before settings are usable. */
bool nd_clock_build_epoch(time_t *out);

/* The last time we were confident about, from /NeoDCT/User/.clock. */
bool nd_clock_last_known(time_t *out);
bool nd_clock_remember(time_t when);

/* settimeofday plus a log line naming the reason, which is the whole point of
 * the CLOCK tag being its own colour: on a serial console you can see at a
 * glance what moved the clock. */
bool nd_clock_set(time_t when, const char *reason);

/* Push the clock forward to max(build epoch, last known) if it is behind.
 * false means it was already sane and was left alone. */
bool nd_clock_apply_floor(time_t *settled);

nd_err nd_clock_query(const char *server, int timeout_s, time_t *out);
bool nd_clock_sync(const char *const *servers, size_t n, int timeout_s, time_t *out);

/* Apply the floor synchronously, then (when background is true) start a thread
 * that waits for a route and syncs. Boot calls this with background true and
 * ignores failures. */
void nd_clock_start(bool background, const char *const *servers, size_t n);

bool nd_clock_has_route(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_CLOCK_H_INCLUDED */
