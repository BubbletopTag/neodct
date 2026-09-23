/*
 * NeoDCT browser chrome: visited-page history.
 */

#include <stdio.h>
#include <string.h>

#include "neodct_history.h"

static void refresh_labels(struct neodct_history *h)
{
	int i;

	for (i = 0; i < h->count; i++) {
		const struct neodct_history_entry *e = &h->entries[i];

		h->labels[i] = (e->title[0] != '\0') ? e->title : e->url;
	}
}

/* A url with a tab or a line break in it would split its own line in
 * the file, and no url the core hands us legitimately carries one. */
static bool url_ok(const char *url)
{
	size_t len;
	const char *p;

	if (url == NULL || url[0] == '\0')
		return false;
	len = strlen(url);
	if (len > NEODCT_HISTORY_URL_MAX)
		return false;
	for (p = url; *p != '\0'; p++) {
		if ((unsigned char)*p < 0x20)
			return false;
	}
	return true;
}

/* Titles come from the page, so anything goes: control characters
 * become spaces (a title is one line on screen and in the file), and
 * leading and trailing space is dropped so a title of whitespace
 * falls back to showing the url. */
static void copy_title(char *dst, const char *src)
{
	size_t o = 0;
	const char *p;

	dst[0] = '\0';
	if (src == NULL)
		return;
	for (p = src; *p == ' ' || ((unsigned char)*p < 0x20 && *p != '\0'); p++)
		;
	for (; *p != '\0' && o < NEODCT_HISTORY_TITLE_MAX; p++)
		dst[o++] = ((unsigned char)*p < 0x20) ? ' ' : *p;
	while (o > 0 && dst[o - 1] == ' ')
		o--;
	dst[o] = '\0';
}

void neodct_history_init(struct neodct_history *h)
{
	memset(h, 0, sizeof(*h));
}

bool neodct_history_add(struct neodct_history *h, const char *url,
			const char *title)
{
	struct neodct_history_entry keep;
	int found = -1;
	int i;

	if (!url_ok(url))
		return false;

	for (i = 0; i < h->count; i++) {
		if (strcmp(h->entries[i].url, url) == 0) {
			found = i;
			break;
		}
	}

	if (found >= 0) {
		keep = h->entries[found];
	} else {
		memset(&keep, 0, sizeof(keep));
		memcpy(keep.url, url, strlen(url) + 1);
		/* the oldest entry falls off the end when full */
		found = (h->count < NEODCT_HISTORY_MAX) ? h->count++ :
			NEODCT_HISTORY_MAX - 1;
	}

	/* A revisit keeps the title it had unless the page gave a new
	 * one: a reload that is still loading has none yet. */
	if (title != NULL && title[0] != '\0')
		copy_title(keep.title, title);

	memmove(&h->entries[1], &h->entries[0],
		(size_t)found * sizeof(h->entries[0]));
	h->entries[0] = keep;
	refresh_labels(h);
	return true;
}

const char *neodct_history_url(const struct neodct_history *h, int i)
{
	if (i < 0 || i >= h->count)
		return NULL;
	return h->entries[i].url;
}

void neodct_history_load(struct neodct_history *h, const char *path)
{
	/* a line is a url, a tab, a title and a newline */
	char line[NEODCT_HISTORY_URL_MAX + NEODCT_HISTORY_TITLE_MAX + 4];
	struct neodct_history loaded;
	FILE *f;

	neodct_history_init(h);
	if (path == NULL)
		return;
	f = fopen(path, "r");
	if (f == NULL)
		return;

	neodct_history_init(&loaded);
	while (loaded.count < NEODCT_HISTORY_MAX &&
	       fgets(line, sizeof(line), f) != NULL) {
		struct neodct_history_entry *e;
		char *tab;
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
			/* longer than any line we write: skip the rest */
			int c;

			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		tab = strchr(line, '\t');
		if (tab != NULL)
			*tab++ = '\0';
		if (!url_ok(line))
			continue;

		e = &loaded.entries[loaded.count++];
		memcpy(e->url, line, strlen(line) + 1);
		copy_title(e->title, tab);
	}
	fclose(f);

	*h = loaded;
	refresh_labels(h);
}

bool neodct_history_save(const struct neodct_history *h, const char *path)
{
	char tmp[1024];
	FILE *f;
	int i;
	bool ok = true;

	if (path == NULL)
		return false;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return false;

	f = fopen(tmp, "w");
	if (f == NULL)
		return false;
	for (i = 0; i < h->count; i++) {
		if (fprintf(f, "%s\t%s\n", h->entries[i].url,
			    h->entries[i].title) < 0)
			ok = false;
	}
	if (fclose(f) != 0)
		ok = false;

	/* rename, so a crash mid-write leaves the previous list rather
	 * than half of this one */
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}
