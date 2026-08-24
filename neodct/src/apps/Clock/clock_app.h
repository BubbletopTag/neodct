/* clock_app.h -- the two constants System/apps/Clock/main.py is made of.
 *
 * Eighteen lines of Python, and every one of them is either a widget call or
 * one of these. They are declared rather than left as literals inside main.c
 * so that test/unit/test_clock_app.c can dlsym() the built app.so and assert
 * on the artefact that ships, the way test_cubebench.c and test_phonebook.c
 * do.
 *
 * The file is NOT called clock.h: lib/nd_clock.c and include/nd_clock.h are
 * the ClockService, which sets the machine's time and has nothing to do with
 * this app beyond the name on the menu.
 */

#ifndef ND_CLOCK_APP_H_INCLUDED
#define ND_CLOCK_APP_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 8 -- manifest.json. Nothing in main.py reads it; it is here for the
 * same reason the other apps' ids are, so the test can check the manifest
 * against the source. */
#define ND_CLOCK_APP_ID 8

/* The string golden/app-clock.png and golden/widget-messagedialog.png are
 * both a rendering of. Changing a byte of it changes two reference frames. */
extern const char *const nd_clock_app_message;

/* `if key in (46, 28, 50)` -- C, ENTER and MENU. NOT 14: Clear does not
 * leave this screen, because MessageDialog's own cancel key set already
 * consumed it inside show() and the loop below never sees it. */
#define ND_CLOCK_APP_EXIT_KEYS 3
extern const int32_t nd_clock_app_exit_keys[ND_CLOCK_APP_EXIT_KEYS];

bool nd_clock_app_is_exit_key(int32_t key);

#ifdef __cplusplus
}
#endif

#endif /* ND_CLOCK_APP_H_INCLUDED */
