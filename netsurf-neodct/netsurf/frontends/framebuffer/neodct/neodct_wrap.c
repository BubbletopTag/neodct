/*
 * NeoDCT browser chrome: greedy word wrap for TextInputLong.
 */

#include <string.h>

#include "neodct_wrap.h"

/* emit one line; returns 0 when the output is full */
static int emit(struct neodct_wrap_line *lines, int max_lines, int *n,
		const char *start, int len)
{
	if (*n >= max_lines)
		return 0;
	lines[*n].start = start;
	lines[*n].len = len;
	(*n)++;
	return 1;
}

/* hard-break a single word wider than max_w into as many lines as fit */
static int break_word(const char *word, int len, int max_w,
		      neodct_measure_fn measure, void *ctx,
		      struct neodct_wrap_line *lines, int max_lines, int *n)
{
	int start = 0, cur = 1;

	while (cur <= len) {
		if (cur < len &&
		    measure(ctx, word + start, (size_t)(cur - start + 1)) >
		    max_w) {
			if (!emit(lines, max_lines, n, word + start,
				  cur - start))
				return 0;
			start = cur;
		}
		cur++;
	}
	if (start < len)
		return emit(lines, max_lines, n, word + start, len - start);
	return 1;
}

/* wrap a single paragraph (no newlines) */
static int wrap_para(const char *para, int plen, int max_w,
		     neodct_measure_fn measure, void *ctx,
		     struct neodct_wrap_line *lines, int max_lines, int *n)
{
	int line_start = 0;
	int pos = 0;
	int n_start = *n;

	while (pos < plen) {
		/* find the end of the next word */
		int word_start = pos;
		int word_end;

		while (pos < plen && para[pos] != ' ')
			pos++;
		word_end = pos;
		if (pos < plen)
			pos++; /* skip the space */

		if (word_end == word_start)
			continue; /* consecutive spaces */

		/* a word wider than the line: flush and hard-break it */
		if (measure(ctx, para + word_start,
			    (size_t)(word_end - word_start)) > max_w) {
			if (word_start > line_start &&
			    para[word_start - 1] == ' ') {
				if (!emit(lines, max_lines, n,
					  para + line_start,
					  word_start - 1 - line_start))
					return 0;
			}
			if (!break_word(para + word_start,
					word_end - word_start, max_w,
					measure, ctx, lines, max_lines, n))
				return 0;
			line_start = pos;
			continue;
		}

		/* does the line up to this word still fit? */
		if (measure(ctx, para + line_start,
			    (size_t)(word_end - line_start)) > max_w) {
			/* no: close the line before this word */
			int prev_end = word_start;

			while (prev_end > line_start &&
			       para[prev_end - 1] == ' ')
				prev_end--;
			if (!emit(lines, max_lines, n, para + line_start,
				  prev_end - line_start))
				return 0;
			line_start = word_start;
		}
	}

	/* remaining text, or an empty line for an empty paragraph;
	 * a paragraph ending exactly on a hard break adds nothing */
	if (line_start < plen || *n == n_start)
		return emit(lines, max_lines, n, para + line_start,
			    plen - line_start);
	return 1;
}

int neodct_wrap_text(const char *text, int max_w,
		     neodct_measure_fn measure, void *ctx,
		     struct neodct_wrap_line *lines, int max_lines)
{
	int n = 0;
	const char *para = text;

	if (text == NULL || max_lines <= 0)
		return 0;

	while (1) {
		const char *nl = strchr(para, '\n');
		int plen = (nl != NULL) ? (int)(nl - para)
					: (int)strlen(para);

		if (!wrap_para(para, plen, max_w, measure, ctx,
			       lines, max_lines, &n))
			break;
		if (nl == NULL)
			break;
		para = nl + 1;
	}

	if (n == 0) {
		lines[0].start = text;
		lines[0].len = 0;
		n = 1;
	}
	return n;
}
