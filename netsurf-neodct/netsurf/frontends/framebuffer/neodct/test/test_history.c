/*
 * Tests for neodct_history: most-recent-first, one entry per url,
 * capped, and a file that survives a round trip and a bad line.
 */

#define _POSIX_C_SOURCE 200809L /* mkstemp under -std=c99 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "test_util.h"
#include "../neodct_history.h"

int main(void)
{
	static struct neodct_history h, back;
	char path[] = "/tmp/neodct_history_XXXXXX";
	char url[64];
	FILE *f;
	int fd, i;

	neodct_history_init(&h);
	CHECK_INT(h.count, 0);
	CHECK(neodct_history_url(&h, 0) == NULL);

	/* newest first; the label is the title, or the url without one */
	CHECK(neodct_history_add(&h, "https://a.org/", "Page A"));
	CHECK(neodct_history_add(&h, "https://b.org/", NULL));
	CHECK_INT(h.count, 2);
	CHECK_STR(neodct_history_url(&h, 0), "https://b.org/");
	CHECK_STR(h.labels[0], "https://b.org/");
	CHECK_STR(h.labels[1], "Page A");

	/* a revisit moves to the top instead of adding a copy, and keeps
	 * its title when the new visit has none yet */
	CHECK(neodct_history_add(&h, "https://a.org/", NULL));
	CHECK_INT(h.count, 2);
	CHECK_STR(neodct_history_url(&h, 0), "https://a.org/");
	CHECK_STR(h.labels[0], "Page A");
	CHECK_STR(neodct_history_url(&h, 1), "https://b.org/");

	/* ...and takes a new one when it has it */
	CHECK(neodct_history_add(&h, "https://b.org/", "Page B"));
	CHECK_STR(h.labels[0], "Page B");

	/* titles are one line: control characters become spaces and the
	 * ends are trimmed; all-whitespace means no title */
	CHECK(neodct_history_add(&h, "https://c.org/", "  Two\tpart\ntitle "));
	CHECK_STR(h.labels[0], "Two part title");
	CHECK(neodct_history_add(&h, "https://d.org/", " \t "));
	CHECK_STR(h.labels[0], "https://d.org/");

	/* nothing that would break the file or the list */
	CHECK(!neodct_history_add(&h, NULL, NULL));
	CHECK(!neodct_history_add(&h, "", "x"));
	CHECK(!neodct_history_add(&h, "https://e.org/\tx", NULL));
	CHECK(!neodct_history_add(&h, "https://e.org/\nx", NULL));
	CHECK_INT(h.count, 4);

	/* capped: the oldest falls off */
	neodct_history_init(&h);
	for (i = 0; i < NEODCT_HISTORY_MAX + 5; i++) {
		snprintf(url, sizeof(url), "https://%d.org/", i);
		neodct_history_add(&h, url, NULL);
	}
	CHECK_INT(h.count, NEODCT_HISTORY_MAX);
	snprintf(url, sizeof(url), "https://%d.org/", NEODCT_HISTORY_MAX + 4);
	CHECK_STR(neodct_history_url(&h, 0), url);
	CHECK_STR(neodct_history_url(&h, NEODCT_HISTORY_MAX - 1),
		  "https://5.org/");

	/* a revisit of the last entry in a full list loses nothing */
	neodct_history_add(&h, "https://5.org/", NULL);
	CHECK_INT(h.count, NEODCT_HISTORY_MAX);
	CHECK_STR(neodct_history_url(&h, 0), "https://5.org/");
	CHECK_STR(neodct_history_url(&h, NEODCT_HISTORY_MAX - 1),
		  "https://6.org/");

	/* round trip through the file */
	fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);

	neodct_history_init(&h);
	neodct_history_add(&h, "https://a.org/", "Page A");
	neodct_history_add(&h, "https://b.org/", NULL);
	CHECK(neodct_history_save(&h, path));
	neodct_history_load(&back, path);
	CHECK_INT(back.count, 2);
	CHECK_STR(neodct_history_url(&back, 0), "https://b.org/");
	CHECK_STR(back.labels[0], "https://b.org/");
	CHECK_STR(neodct_history_url(&back, 1), "https://a.org/");
	CHECK_STR(back.labels[1], "Page A");

	/* a hand-edited file: blank lines, a line with no title, CRLF and
	 * an over-long line are all survivable */
	f = fopen(path, "w");
	CHECK(f != NULL);
	fprintf(f, "\nhttps://x.org/\r\n");
	for (i = 0; i < 2000; i++)
		fputc('z', f);
	fprintf(f, "\nhttps://y.org/\tWhy\n");
	fclose(f);
	neodct_history_load(&back, path);
	CHECK_INT(back.count, 2);
	CHECK_STR(neodct_history_url(&back, 0), "https://x.org/");
	CHECK_STR(back.labels[0], "https://x.org/");
	CHECK_STR(neodct_history_url(&back, 1), "https://y.org/");
	CHECK_STR(back.labels[1], "Why");

	/* a missing file is an empty history */
	unlink(path);
	neodct_history_load(&back, path);
	CHECK_INT(back.count, 0);

	TEST_EXIT();
}
