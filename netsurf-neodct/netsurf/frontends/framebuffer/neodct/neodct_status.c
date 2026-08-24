/*
 * NeoDCT browser chrome: status bar state.
 */

#include <stdio.h>

#include "neodct_status.h"
#include "neodct_url.h"

static void show(struct neodct_status *s)
{
	s->visible = true;
	s->hide_at_ms = -1;
}

void neodct_status_init(struct neodct_status *s)
{
	s->text[0] = '\0';
	s->visible = false;
	s->hide_at_ms = -1;
}

void neodct_status_waiting(struct neodct_status *s, const char *url)
{
	char host[NEODCT_STATUS_TEXT_MAX + 1];

	if (neodct_host_from_url(url, host, sizeof(host)) == NULL)
		host[0] = '\0';
	snprintf(s->text, sizeof(s->text), "Waiting for %.64s...", host);
	show(s);
}

void neodct_status_connected(struct neodct_status *s)
{
	snprintf(s->text, sizeof(s->text), "Connected...");
	show(s);
}

void neodct_status_transferring(struct neodct_status *s, int pct)
{
	if (pct < 0)
		snprintf(s->text, sizeof(s->text), "Transferring...");
	else
		snprintf(s->text, sizeof(s->text), "Transferring... %d%%", pct);
	show(s);
}

void neodct_status_done(struct neodct_status *s, long now_ms)
{
	snprintf(s->text, sizeof(s->text), "Done.");
	s->visible = true;
	s->hide_at_ms = now_ms + NEODCT_STATUS_HIDE_MS;
}

void neodct_status_tick(struct neodct_status *s, long now_ms)
{
	if (s->hide_at_ms >= 0 && now_ms >= s->hide_at_ms) {
		s->visible = false;
		s->hide_at_ms = -1;
	}
}
