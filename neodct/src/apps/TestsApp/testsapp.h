/* testsapp.h -- what apps/TestsApp/main.c shows its unit test.
 *
 * The three strings and the one geometry rule are pulled out of app_run() so
 * test_testsapp.c can pin them directly instead of only through a rendered
 * frame. Nothing else includes this.
 */

#ifndef ND_TESTSAPP_H_INCLUDED
#define ND_TESTSAPP_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py's three literals. Two of them reach pixels on
 * golden/eng-tests.png: the message decides where MessageDialog wraps, and
 * the greeting decides the centring on the frame the dialog paints over. */
extern const char *const nd_testsapp_softkey;  /* "Testing123"               */
extern const char *const nd_testsapp_message;  /* the error-screen notice    */
extern const char *const nd_testsapp_greeting; /* "Hello World"              */

/* `key in (46, 28, 50)` -- C, ENTER, MENU. The same three the Clock app uses,
 * and 14 is deliberately not among them: Back is MessageDialog's CANCEL key,
 * so it dismisses the dialog and the loop then draws it again. */
#define ND_TESTSAPP_EXIT_KEYS 3u
extern const int32_t nd_testsapp_exit_keys[ND_TESTSAPP_EXIT_KEYS];
bool nd_testsapp_is_exit_key(int32_t key);

/* ((screen_w - w) // 2, (content_bottom - h) // 2) for the greeting's INK
 * box, measured with ui.get_text_size(). Python's // FLOORS, which is not
 * C's / once a string is wider than the panel -- "Hello World" at 24 px is
 * not, and a longer greeting would be. Writes nothing when ui is NULL. */
void nd_testsapp_greeting_pos(nd_ui *ui, int32_t *x, int32_t *y);

#ifdef __cplusplus
}
#endif

#endif /* ND_TESTSAPP_H_INCLUDED */
