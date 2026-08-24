/*
 * Tests for neodct_wrap: greedy word wrap with long-word breaking,
 * mirroring TextInputLong._wrap_text in the NeoDCT framework.
 *
 * The measure callback here charges 8px per byte so expected break
 * positions are easy to compute (max_w 80 = 10 chars per line).
 */

#include "test_util.h"
#include "../neodct_wrap.h"

static int measure8(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s;
	return (int)len * 8;
}

static int wrap(const char *text, struct neodct_wrap_line *lines, int max)
{
	return neodct_wrap_text(text, 80, measure8, NULL, lines, max);
}

static void check_line(struct neodct_wrap_line *l, const char *want,
		       const char *file, int line)
{
	char buf[64];

	if (l->len >= (int)sizeof(buf)) {
		printf("FAIL %s:%d: line too long (%d)\n", file, line, l->len);
		test_failures++;
		test_checks++;
		return;
	}
	memcpy(buf, l->start, l->len);
	buf[l->len] = '\0';
	test_checks++;
	if (strcmp(buf, want) != 0) {
		printf("FAIL %s:%d: got \"%s\" want \"%s\"\n",
		       file, line, buf, want);
		test_failures++;
	}
}
#define CHECK_LINE(l, want) check_line(l, want, __FILE__, __LINE__)

int main(void)
{
	struct neodct_wrap_line lines[8];
	int n;

	/* short text: single line */
	n = wrap("hello", lines, 8);
	CHECK_INT(n, 1);
	CHECK_LINE(&lines[0], "hello");

	/* empty text still yields one empty line (cursor row) */
	n = wrap("", lines, 8);
	CHECK_INT(n, 1);
	CHECK_LINE(&lines[0], "");

	/* words wrap at spaces; the break space is consumed */
	n = wrap("hello brave world", lines, 8);
	CHECK_INT(n, 3);
	CHECK_LINE(&lines[0], "hello");
	CHECK_LINE(&lines[1], "brave");
	CHECK_LINE(&lines[2], "world");

	/* exact fit is kept on one line */
	n = wrap("0123456789", lines, 8);
	CHECK_INT(n, 1);
	CHECK_LINE(&lines[0], "0123456789");

	/* a word longer than the line is hard-broken */
	n = wrap("abcdefghijklmnop", lines, 8);
	CHECK_INT(n, 2);
	CHECK_LINE(&lines[0], "abcdefghij");
	CHECK_LINE(&lines[1], "klmnop");

	/* newlines force breaks */
	n = wrap("ab\ncd", lines, 8);
	CHECK_INT(n, 2);
	CHECK_LINE(&lines[0], "ab");
	CHECK_LINE(&lines[1], "cd");

	/* mixed: words pack greedily */
	n = wrap("to be or not to be", lines, 8);
	CHECK_INT(n, 2);
	CHECK_LINE(&lines[0], "to be or");
	CHECK_LINE(&lines[1], "not to be");

	/* output capped at max_lines without overflow */
	n = wrap("a b c d e f g h i j k l m n o p q r s t", lines, 2);
	CHECK_INT(n, 2);

	TEST_EXIT();
}
