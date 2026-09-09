/* nd_key.h -- the parts of nd-key that are worth testing without a socket.
 *
 * Token resolution and argument parsing live here rather than being static in
 * nd_key.c, because they are where the mistakes are: "is 5 the digit five or
 * evdev code 5" has one right answer and no way to check it by eye. main() is
 * the only thing left in nd_key.c, exactly as nd_bootbar.c is separated from
 * its drawing layer.
 */

#ifndef ND_KEY_H_INCLUDED
#define ND_KEY_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one invocation does to every key named on the command line. A tap is
 * the default and sends BOTH edges, because a press with no release leaves the
 * key held -- nd_input_is_held() is real state that widgets read, and a phone
 * left believing DOWN is held scrolls on its own. */
typedef enum { ND_KEY_ACT_TAP = 0, ND_KEY_ACT_HOLD, ND_KEY_ACT_RELEASE } nd_key_action;

/* The default gap between keys. Slow enough that a UI which redraws between
 * presses keeps up, fast enough that typing a word is not a wait. */
#define ND_KEY_DEFAULT_DELAY_MS 120u

typedef struct {
    nd_key_action action;
    unsigned int delay_ms;
    const char *socket_path; /* NULL means the built-in default */
    bool list;
    bool help;
    int first_key; /* argv index of the first key token, or argc if none */
} nd_key_opts;

/* Resolve one command-line token to an evdev keycode, or -1.
 *
 * ============ THE ONE AMBIGUITY, AND HOW IT IS SETTLED ============
 *
 * "5" could mean the digit key five (evdev 6) or raw evdev code 5 (which is
 * the digit key FOUR). Both readings are defensible and one of them has to
 * win, so:
 *
 *   a single digit 0-9      the DIGIT KEY. `nd-key 5` presses five.
 *   two or more digits      a RAW evdev code. `nd-key 50` presses code 50.
 *   raw:N                   a raw code, unambiguously, for single digits.
 *   a name                  case-insensitive, via nd_keycode_for_name(),
 *                           plus the aliases below.
 *   * and #                 star and hash, because that is what is printed
 *                           on the keys.
 *
 * The single-digit rule wins because nd-key is for driving a phone, and a
 * human writing `nd-key 5` means the key with 5 on it. raw:N exists so the
 * other reading is still reachable. */
int32_t nd_key_token_to_code(const char *token);

/* True when this code is one of the sixteen keys that physically exist on the
 * matrix. Derived from nd_kpsetup_targets[], not a second list -- so it stays
 * true if the keypad ever changes. LEFT, RIGHT and MENU are all false: they
 * are reachable from a dev keyboard and from this tool, and are not on the
 * phone. */
bool nd_key_on_matrix(int32_t code);

/* Parse argv. Returns true on success; on failure fills err and returns false.
 * Does not touch the socket -- a usage error must not depend on whether a
 * phone is listening. */
bool nd_key_parse_args(int argc, char **argv, nd_key_opts *out, char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEY_H_INCLUDED */
