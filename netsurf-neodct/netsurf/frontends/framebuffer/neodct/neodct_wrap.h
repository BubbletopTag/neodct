/*
 * NeoDCT browser chrome: greedy word wrap for TextInputLong.
 *
 * Pure logic with an injected text-measure callback so it is
 * unit-testable; the shell measures with the real font.
 */

#ifndef NEODCT_WRAP_H
#define NEODCT_WRAP_H

#include <stddef.h>

/** measure the pixel width of the first len bytes of s */
typedef int (*neodct_measure_fn)(void *ctx, const char *s, size_t len);

/** one wrapped line: a span of the input text */
struct neodct_wrap_line {
	const char *start;
	int len;
};

/**
 * Wrap text into lines no wider than max_w.
 *
 * Breaks at spaces (the break space is consumed), hard-breaks words
 * wider than a line, and honours embedded newlines. Always produces
 * at least one line so the caller can draw a cursor row.
 *
 * \return the number of lines written (at most max_lines)
 */
int neodct_wrap_text(const char *text, int max_w,
		     neodct_measure_fn measure, void *ctx,
		     struct neodct_wrap_line *lines, int max_lines);

#endif
