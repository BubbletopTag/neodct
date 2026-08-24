/*
 * Tests for neodct_status: bottom status bar text/visibility,
 * matching set_status_waiting/connected/transferring/done of the
 * old WebKit browser ("Done." auto-hides after 2 seconds).
 */

#include "test_util.h"
#include "../neodct_status.h"

int main(void)
{
	struct neodct_status s;

	neodct_status_init(&s);
	CHECK_INT(s.visible, 0);

	/* navigation started: waiting on the host */
	neodct_status_waiting(&s, "https://www.foo.org/bar?q=1");
	CHECK_INT(s.visible, 1);
	CHECK_STR(s.text, "Waiting for www.foo.org...");

	neodct_status_connected(&s);
	CHECK_STR(s.text, "Connected...");

	/* transferring, with and without a percentage */
	neodct_status_transferring(&s, -1);
	CHECK_STR(s.text, "Transferring...");
	neodct_status_transferring(&s, 42);
	CHECK_STR(s.text, "Transferring... 42%");

	/* done: visible now, hides two seconds later */
	neodct_status_done(&s, 10000);
	CHECK_STR(s.text, "Done.");
	CHECK_INT(s.visible, 1);

	neodct_status_tick(&s, 11999);
	CHECK_INT(s.visible, 1);
	neodct_status_tick(&s, 12000);
	CHECK_INT(s.visible, 0);

	/* a new load cancels the pending hide */
	neodct_status_done(&s, 20000);
	neodct_status_waiting(&s, "bar.com");
	neodct_status_tick(&s, 30000);
	CHECK_INT(s.visible, 1);
	CHECK_STR(s.text, "Waiting for bar.com...");

	TEST_EXIT();
}
