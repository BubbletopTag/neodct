/* remote_app.h -- the parts of the Remote Shell app a unit test can reach.
 *
 * The app itself (engineering/apps/RemoteShell/main.py, 165 lines) is seven
 * menu lines and six dialogs over the module in rshell.h. Almost all of it is
 * widget calls; what is left is the menu the list is built from -- which is
 * also the status display, and therefore the only screen that says whether the
 * phone is reachable -- and the exact words of the six dialogs.
 *
 * test/unit/test_remoteshell.c dlopen()s the BUILT app.so and dlsym()s these,
 * the way test_power.c does.
 *
 * ============ WHAT THE TEST DELIBERATELY DOES NOT CALL ============
 *
 * nd_rs_start(). It forks a real sshd and a real reconnect loop in their own
 * sessions, and then nd_rs_stop() signals a process GROUP. On a developer's
 * machine that is somebody's ssh server, and a test that can leave a daemon
 * behind on the machine it runs on is not a test. Everything underneath it --
 * check_ready's refusals, the generated config and script, the quoting, the
 * pid ownership check -- is reachable and is tested; the composition is not,
 * and the hole is named here rather than discovered.
 */

#ifndef ND_REMOTE_APP_H_INCLUDED
#define ND_REMOTE_APP_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TITLE = "Remote", not "Remote Shell", and the comment in the Python says
 * why in measured pixels: "Short enough to sit beside the breadcrumb counter
 * without the framework having to trim it. 'Remote Shell' is 189px against 136
 * available; this is 111." Do not "improve" it. */
extern const char *const nd_rsapp_title;

/* STATUS, TOGGLE, RELAY, LOGIN, PORT, KEYS, FINGERPRINT = range(7) */
#define ND_RSAPP_STATUS      0
#define ND_RSAPP_TOGGLE      1
#define ND_RSAPP_RELAY       2
#define ND_RSAPP_LOGIN       3
#define ND_RSAPP_PORT        4
#define ND_RSAPP_KEYS        5
#define ND_RSAPP_FINGERPRINT 6
#define ND_RSAPP_MENU_ITEMS  7

/* "Relay: " plus a 255-character IPv6 relay address is the longest line. */
#define ND_RSAPP_LINE_MAX 288

/* The longest dialog is "Copied: ..." with all four names in it. */
#define ND_RSAPP_MSG_MAX 320

/* _menu_lines(): the list, rebuilt each time round, because it is also the
 * status display. Reads live state -- nd_rs_status_get() and
 * nd_rs_settings_get() -- exactly as the Python does. */
void nd_rsapp_menu_lines(char lines[][ND_RSAPP_LINE_MAX], size_t n);

/* The three-state word in line 0. "Half up is worth naming. It means the relay
 * refused the tunnel or dropped it, and 'On' would be a lie while nothing can
 * reach you." */
const char *nd_rsapp_running_word(bool sshd, bool tunnel);

/* The six fixed dialog texts, so a test can pin what a user reads without
 * driving the widgets. */
extern const char *const nd_rsapp_no_card;
extern const char *const nd_rsapp_ask_turn_on;
extern const char *const nd_rsapp_turn_on_button;
extern const char *const nd_rsapp_now_on;
extern const char *const nd_rsapp_now_off;

/* "Copied: %s.\n\nDelete them from the card now -- ..." Note the literal
 * double hyphen: it is not an em dash and this font would draw one wrong. */
void nd_rsapp_copied_message(char *out, size_t n, const char *taken);

/* "This phone:\n%s", with "unknown" for an empty fingerprint. */
void nd_rsapp_fingerprint_message(char *out, size_t n, const char *fingerprint);

/* _tell(ui, message): MessageDialog titled "Remote" with an "OK" button,
 * answer ignored. _confirm(ui, question, button): the same dialog with the
 * caller's button, true only on ENTER. */
void nd_rsapp_tell(nd_ui *ui, const char *message);
bool nd_rsapp_confirm(nd_ui *ui, const char *question, const char *button);

#ifdef __cplusplus
}
#endif

#endif /* ND_REMOTE_APP_H_INCLUDED */
