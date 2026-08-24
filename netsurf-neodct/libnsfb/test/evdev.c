/*
 * Test for the linux evdev keycode -> nsfb keycode translation used
 * by the linux framebuffer surface input handling.
 *
 * Standalone logic test: needs no surface, exits non-zero on failure.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "libnsfb.h"
#include "libnsfb_event.h"

#include "../src/surface/linux_evdev.h"

static int failures = 0;

static void check(uint16_t evdev, enum nsfb_key_code_e want)
{
	enum nsfb_key_code_e got = nsfb_linux_evdev_to_nsfb(evdev);

	if (got != want) {
		printf("FAIL evdev %u: got %d want %d\n", evdev, got, want);
		failures++;
	}
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	/* navigation cluster (NeoDCT keypad uses these codes too) */
	check(103, NSFB_KEY_UP);
	check(108, NSFB_KEY_DOWN);
	check(105, NSFB_KEY_LEFT);
	check(106, NSFB_KEY_RIGHT);
	check(28, NSFB_KEY_RETURN);   /* enter / navikey */
	check(96, NSFB_KEY_KP_ENTER);
	check(14, NSFB_KEY_BACKSPACE); /* back/clear */
	check(1, NSFB_KEY_ESCAPE);

	/* letters: qwerty rows are not contiguous in evdev */
	check(16, NSFB_KEY_q);
	check(25, NSFB_KEY_p);
	check(30, NSFB_KEY_a);
	check(38, NSFB_KEY_l);
	check(44, NSFB_KEY_z);
	check(50, NSFB_KEY_m);

	/* digits: evdev row starts at 1 and ends with 0 */
	check(2, NSFB_KEY_1);
	check(10, NSFB_KEY_9);
	check(11, NSFB_KEY_0);

	/* url punctuation and editing */
	check(52, NSFB_KEY_PERIOD);
	check(53, NSFB_KEY_SLASH);
	check(12, NSFB_KEY_MINUS);
	check(13, NSFB_KEY_EQUALS);
	check(51, NSFB_KEY_COMMA);
	check(39, NSFB_KEY_SEMICOLON);
	check(57, NSFB_KEY_SPACE);
	check(15, NSFB_KEY_TAB);
	check(111, NSFB_KEY_DELETE);
	check(104, NSFB_KEY_PAGEUP);
	check(109, NSFB_KEY_PAGEDOWN);

	/* modifiers, needed for shifted characters */
	check(42, NSFB_KEY_LSHIFT);
	check(54, NSFB_KEY_RSHIFT);
	check(29, NSFB_KEY_LCTRL);
	check(97, NSFB_KEY_RCTRL);

	/* unmapped codes must translate to UNKNOWN, not garbage */
	check(0, NSFB_KEY_UNKNOWN);
	check(465, NSFB_KEY_UNKNOWN);  /* BTN_* range (mouse/tablet) */
	check(65535, NSFB_KEY_UNKNOWN);

	printf("evdev translate: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
