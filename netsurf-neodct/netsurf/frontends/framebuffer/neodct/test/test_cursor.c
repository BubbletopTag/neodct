/*
 * Tests for neodct_cursor: keypad-driven virtual pointer with
 * page-scroll at the view edges, matching the cursor block of
 * on_key() in the old WebKit browser (CURSOR_STEP 6, FAST 14).
 */

#include "test_util.h"
#include "../neodct_cursor.h"

int main(void)
{
	struct neodct_cursor c;
	struct neodct_scroll s;

	/* init centres the cursor in a 240x175 view */
	neodct_cursor_init(&c, 240, 175);
	CHECK_INT(c.x, 120);
	CHECK_INT(c.y, 87);

	/* interior move: cursor steps, no scroll */
	neodct_cursor_move(&c, NEODCT_DIR_RIGHT, 6, &s);
	CHECK_INT(c.x, 126);
	CHECK_INT(s.dx, 0);
	CHECK_INT(s.dy, 0);

	neodct_cursor_move(&c, NEODCT_DIR_UP, 6, &s);
	CHECK_INT(c.y, 81);
	CHECK_INT(s.dx, 0);
	CHECK_INT(s.dy, 0);

	/* fast step */
	neodct_cursor_move(&c, NEODCT_DIR_DOWN, 14, &s);
	CHECK_INT(c.y, 95);

	/* pushing past the right edge clamps to width-1 and scrolls */
	c.x = 236; c.y = 87;
	neodct_cursor_move(&c, NEODCT_DIR_RIGHT, 6, &s);
	CHECK_INT(c.x, 239);
	CHECK_INT(s.dx, 6);
	CHECK_INT(s.dy, 0);

	/* at the edge: stays put, keeps scrolling */
	neodct_cursor_move(&c, NEODCT_DIR_RIGHT, 6, &s);
	CHECK_INT(c.x, 239);
	CHECK_INT(s.dx, 6);

	/* left edge: partial move still emits a full-step scroll */
	c.x = 3;
	neodct_cursor_move(&c, NEODCT_DIR_LEFT, 6, &s);
	CHECK_INT(c.x, 0);
	CHECK_INT(s.dx, -6);

	/* top edge */
	c.y = 0;
	neodct_cursor_move(&c, NEODCT_DIR_UP, 6, &s);
	CHECK_INT(c.y, 0);
	CHECK_INT(s.dx, 0);
	CHECK_INT(s.dy, -6);

	/* bottom edge: target == height clamps and scrolls */
	c.y = 169;
	neodct_cursor_move(&c, NEODCT_DIR_DOWN, 6, &s);
	CHECK_INT(c.y, 174);
	CHECK_INT(s.dy, 6);

	TEST_EXIT();
}
