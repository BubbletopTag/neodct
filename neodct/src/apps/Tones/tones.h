/* tones.h -- the parts of the Tones app a unit test can reach.
 *
 * System/apps/Tones/main.py is 229 lines: a two-item PagedList, a
 * two-item VerticalList that saves nothing, and the ringtone browser, which
 * is the only screen in the eleven stock apps that drives a VerticalList's
 * scroll state by hand instead of calling show().
 *
 * test/unit/test_tones.c dlopen()s the BUILT app.so and dlsym()s these, the
 * way test_cubebench.c and test_phonebook.c do.
 *
 * ============ THE THREE CAPS THIS PORT ADDS ============
 *
 * The Python holds the tone list in a Python list and never bounds it.
 * CODING-STANDARDS.md section 1.5 will not have an array sized by the
 * contents of an SD card, so:
 *
 *   ND_TONES_MAX       256 tones. The list is HEAP allocated -- 256 * 352 =
 *                      90,112 bytes -- and freed before the screen returns.
 *                      A card with more than 256 .mp3 files shows the first
 *                      256 in walk order and logs that it stopped.
 *   ND_TONES_NAME_MAX  96 bytes of display name. About twenty characters fit
 *                      across a 240 px screen at 18 px, so a name long
 *                      enough to be truncated here was already running off
 *                      the right-hand edge.
 *   ND_TONES_WALK_MAX  64 directories pending in the walk. The stock tones
 *                      directory is flat; a card is not necessarily.
 *
 * All three are recorded in OPEN-QUESTIONS.md under TN-1.
 */

#ifndef ND_TONES_H_INCLUDED
#define ND_TONES_H_INCLUDED

#include <sys/types.h>

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ROOT_ID = 9 -- manifest.json, and the breadcrumb on every screen here. */
#define ND_TONES_ROOT_ID 9

/* SYSTEM_TONES_DIR / USER_TONES_DIR. Absolute and load-bearing
 * (CODING-STANDARDS.md section 9.5); both go through nd_path_resolve(). */
#define ND_TONES_SYSTEM_DIR "/NeoDCT/System/tones"
#define ND_TONES_USER_DIR   "/NeoDCT/User/tones"

/* SUPPORTED_EXTS. Note the Python writes `(".mp3")`, which is a STRING and
 * not a one-element tuple -- str.endswith accepts both, so the two behave
 * identically here and would not if a second extension were ever added
 * without the missing comma being noticed. Recorded in OPEN-QUESTIONS.md
 * TN-2; the C is a plain single-suffix test, which is what runs today. */
#define ND_TONES_EXT ".mp3"

#define ND_TONES_MAX      256
#define ND_TONES_NAME_MAX 96
#define ND_TONES_PATH_MAX 256
#define ND_TONES_WALK_MAX 64

/* ADD_MORE_LABEL, and the two help texts behind it. Which one is shown
 * depends on whether a card is in the phone right now. */
extern const char *const nd_tones_add_more_label;
extern const char *const nd_tones_add_more_help;
extern const char *const nd_tones_add_more_help_with_card;

/* run()'s PagedList and _show_ringing_options()'s VerticalList. */
#define ND_TONES_MENU_ITEMS 2
extern const char *const nd_tones_menu[ND_TONES_MENU_ITEMS];
#define ND_TONES_RINGING_ITEMS 2
extern const char *const nd_tones_ringing_options[ND_TONES_RINGING_ITEMS];

/* MPV_CMD, without the path.
 *
 * ============ THE `nice -n -10` THE PYTHON HAD IS GONE ============
 *
 * The Python's argv was `nice -n -10 mpv ...`, and it was ported verbatim,
 * and on 0.4.x it worked because apps ran as root. Since 27cf79bf an app is
 * ndusr, and a negative nice needs CAP_SYS_NICE or RLIMIT_NICE headroom that
 * nothing on this phone grants (the kernel default is 0 and neither
 * run_neodct.sh nor inittab raises it).
 *
 * That would be survivable if nice(1) degraded. GNU coreutils warns and runs
 * the program anyway; the phone ships BUSYBOX nice, which calls
 * bb_perror_msg_and_die and never reaches its exec. So the process that was
 * actually spawned -- nice, not mpv -- exited 1 on every preview, and because
 * the spawn puts stderr on /dev/null and treats a successful fork as
 * playback, highlighting a tone simply made no sound and said nothing.
 *
 * The honest options were normal priority or giving ndusr RLIMIT_NICE
 * headroom, and normal priority is plainly better than silence: the stutter
 * the nice was added to prevent is a cosmetic fault, and this was a total
 * one. mpv is now the exec target itself, which also means execve failure is
 * reported about the program we actually wanted. */
#define ND_TONES_MPV_ARGC 4
extern const char *const nd_tones_mpv_cmd[ND_TONES_MPV_ARGC];

/* ------------------------------------------------------------------ *
 * The scan
 * ------------------------------------------------------------------ */

/* One row of `tones`. `path` is EMPTY for the "Add more..." pseudo-entry,
 * which is the C spelling of the Python's `"path": None`. */
typedef struct {
    char name[ND_TONES_NAME_MAX];
    char path[ND_TONES_PATH_MAX];
} nd_tone;

/* filename.lower().endswith(SUPPORTED_EXTS). ASCII-only lowering, because
 * Python's str.lower() does not consult the locale and strcasecmp does. */
bool nd_tones_is_supported(const char *filename);

/* os.path.splitext(os.path.basename(filename))[0].
 *
 * splitext's stem is not "up to the last dot": leading dots belong to the
 * name, so splitext(".hidden") is (".hidden", ""). Returns out. */
const char *nd_tones_display_name(const char *filename, char *out, size_t out_sz);

/* _tone_dirs(): stock tones from the image, then the card's, then user-added
 * ones. LOGICAL paths, not ND_ROOT-resolved -- see OPEN-QUESTIONS.md TN-3 for
 * why the C keeps them logical where uistub's os.walk hands the Python
 * staged ones. Returns how many were written. */
#define ND_TONES_DIRS_MAX 3
size_t nd_tones_dirs(char out[][ND_TONES_PATH_MAX], size_t max);

/* _scan_tones(): every .mp3 under every tone directory, sorted by
 * name.lower() with the walk order preserved between equals.
 *
 * `out` is caller-owned and holds at least `max` entries. Returns how many
 * were written. */
size_t nd_tones_scan(nd_tone *out, size_t max);

/* ------------------------------------------------------------------ *
 * TonePreviewPlayer
 * ------------------------------------------------------------------ */

/* PREVIEW_DELAY: "(time.time() - pending_time) >= 0.5". A preview is
 * scheduled on every cursor move and only fires if the cursor has been still
 * for half a second, so holding Down does not start sixteen players. */
#define ND_TONES_PREVIEW_DELAY 0.5

/* The Python gives mpv 0.2 s between terminate() and kill(). */
#define ND_TONES_PREVIEW_GRACE 0.2

/* How long the FIRST preview of an app session is watched for, to see whether
 * the player is still there a moment after the fork.
 *
 * The spawn puts the child's stderr on /dev/null and treats a successful fork
 * as playback, so for eight releases a preview that died before execve looked
 * exactly like one that was playing -- which is how `nice` exiting 1 on every
 * single preview went unnoticed. One measurement per session is enough: a
 * player that cannot start cannot start for any tone, and 50 ms is invisible
 * next to the half-second PREVIEW_DELAY that precedes it. */
#define ND_TONES_PREVIEW_PROBE 0.05

/* Start a preview, stopping whatever was playing first. A NULL or empty path
 * does nothing, which is `if not path: return`.
 *
 * THERE IS AT MOST ONE PREVIEW PROCESS IN THIS APP AT A TIME, and it is a
 * file-static rather than a field, because app_shutdown() takes no argument
 * and the SIGTERM teardown contract in nd_app.h requires it to be able to
 * kill any child this app spawned. */
void nd_tones_preview_play(const char *path);
void nd_tones_preview_stop(void);

/* -1 when nothing is playing. For the test. */
pid_t nd_tones_preview_pid(void);

#ifdef __cplusplus
}
#endif

#endif /* ND_TONES_H_INCLUDED */
