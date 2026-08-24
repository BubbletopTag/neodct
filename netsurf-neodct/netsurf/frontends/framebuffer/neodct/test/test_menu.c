/*
 * Tests for neodct_menu: vertical list selection with a scrolling
 * visibility window, matching VerticalListWidget.navigate() in the
 * old WebKit browser (3 visible rows).
 */

#include "test_util.h"
#include "../neodct_menu.h"

static const char *const items[] = {
	"Exit", "Go to URL", "Back", "Forward", "Home", "Reload"
};

int main(void)
{
	struct neodct_menu m;

	/* init: first item selected, window at top */
	neodct_menu_init(&m, items, 6, 3);
	CHECK_INT(m.selected, 0);
	CHECK_INT(m.window_start, 0);
	CHECK_STR(neodct_menu_selected(&m), "Exit");

	/* moving down within the window does not scroll */
	neodct_menu_down(&m);
	neodct_menu_down(&m);
	CHECK_INT(m.selected, 2);
	CHECK_INT(m.window_start, 0);
	CHECK_STR(neodct_menu_selected(&m), "Back");

	/* moving past the window scrolls it down one row */
	neodct_menu_down(&m);
	CHECK_INT(m.selected, 3);
	CHECK_INT(m.window_start, 1);

	/* run to the end; window shows the last 3 items */
	neodct_menu_down(&m);
	neodct_menu_down(&m);
	CHECK_INT(m.selected, 5);
	CHECK_INT(m.window_start, 3);
	CHECK_STR(neodct_menu_selected(&m), "Reload");

	/* down at the end is a no-op */
	neodct_menu_down(&m);
	CHECK_INT(m.selected, 5);
	CHECK_INT(m.window_start, 3);

	/* moving up within the window does not scroll */
	neodct_menu_up(&m);
	neodct_menu_up(&m);
	CHECK_INT(m.selected, 3);
	CHECK_INT(m.window_start, 3);

	/* moving above the window scrolls it up */
	neodct_menu_up(&m);
	CHECK_INT(m.selected, 2);
	CHECK_INT(m.window_start, 2);

	/* run back to the top; up at the top is a no-op */
	neodct_menu_up(&m);
	neodct_menu_up(&m);
	neodct_menu_up(&m);
	CHECK_INT(m.selected, 0);
	CHECK_INT(m.window_start, 0);

	/* reset (menu reopened): back to top like the old toggle_menu() */
	neodct_menu_down(&m);
	neodct_menu_down(&m);
	neodct_menu_down(&m);
	neodct_menu_reset(&m);
	CHECK_INT(m.selected, 0);
	CHECK_INT(m.window_start, 0);

	/* a list shorter than the window never scrolls */
	neodct_menu_init(&m, items, 2, 3);
	neodct_menu_down(&m);
	neodct_menu_down(&m);
	CHECK_INT(m.selected, 1);
	CHECK_INT(m.window_start, 0);

	TEST_EXIT();
}
