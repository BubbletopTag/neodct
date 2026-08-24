/*
 * NeoDCT browser chrome: status bar state.
 *
 * Pure logic with explicit time so it is testable; the shell calls
 * neodct_status_tick() from its schedule loop with a millisecond
 * clock of its choosing.
 */

#ifndef NEODCT_STATUS_H
#define NEODCT_STATUS_H

#include <stdbool.h>

#define NEODCT_STATUS_TEXT_MAX 80
#define NEODCT_STATUS_HIDE_MS 2000

struct neodct_status {
	char text[NEODCT_STATUS_TEXT_MAX + 1];
	bool visible;
	long hide_at_ms; /**< monotonic ms to hide at, -1 when none */
};

void neodct_status_init(struct neodct_status *s);

void neodct_status_waiting(struct neodct_status *s, const char *url);
void neodct_status_connected(struct neodct_status *s);
/** pct is 0-100, or -1 for no percentage */
void neodct_status_transferring(struct neodct_status *s, int pct);
void neodct_status_done(struct neodct_status *s, long now_ms);

/** apply any scheduled hide; call periodically */
void neodct_status_tick(struct neodct_status *s, long now_ms);

#endif
