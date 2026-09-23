/*
 * NeoDCT browser chrome: visited-page history.
 *
 * Most recent first, one entry per url (a revisit moves the entry to
 * the top rather than adding a second copy), capped at
 * NEODCT_HISTORY_MAX so the list stays a list and not an archive --
 * three rows are visible at a time on this screen.
 *
 * Pure logic plus a plain text file, no netsurf dependencies, unit
 * tested in test/. The file is one entry per line, "url\ttitle", so
 * it can be read (and emptied) from a shell on the phone.
 */

#ifndef NEODCT_HISTORY_H
#define NEODCT_HISTORY_H

#include <stdbool.h>

#define NEODCT_HISTORY_MAX 20
#define NEODCT_HISTORY_URL_MAX 511
#define NEODCT_HISTORY_TITLE_MAX 79

struct neodct_history_entry {
	char url[NEODCT_HISTORY_URL_MAX + 1];
	char title[NEODCT_HISTORY_TITLE_MAX + 1];
};

struct neodct_history {
	struct neodct_history_entry entries[NEODCT_HISTORY_MAX];
	int count;
	/** per-entry display text (title, or the url when there is none),
	 * laid out as a neodct_menu item array */
	const char *labels[NEODCT_HISTORY_MAX];
};

void neodct_history_init(struct neodct_history *h);

/**
 * Record a visit. An existing entry for the url moves to the top and
 * takes the new title if one is given; otherwise the oldest entry
 * falls off the end when the list is full.
 *
 * \return false when nothing was recorded (empty or oversized url)
 */
bool neodct_history_add(struct neodct_history *h, const char *url,
			const char *title);

/** url of entry i (0 is most recent), or NULL when out of range */
const char *neodct_history_url(const struct neodct_history *h, int i);

/** load from path, replacing the contents; a missing file is empty */
void neodct_history_load(struct neodct_history *h, const char *path);

/** write to path via a temporary file and rename; false on failure */
bool neodct_history_save(const struct neodct_history *h, const char *path);

#endif
