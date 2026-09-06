/* settings_app.h -- the parts of the Settings app a unit test can reach.
 *
 * System/apps/Settings/main.py is 310 lines: a four-item VerticalList over
 * the wallpaper picker, the memory-card screen, the engineering-mode toggle
 * and an About page that is the only screen in the app drawn by hand rather
 * than by a widget.
 *
 * test/unit/test_settings_app.c dlopen()s the BUILT app.so and dlsym()s
 * these, the way test_tones.c and test_phonebook.c do.
 *
 * ============ IT IS NAMED settings_app, NOT settings ============
 *
 * lib/nd_settings.c already owns `nd_settings_*` -- the phone's preference
 * store, which this app is a CLIENT of. Two different things called
 * nd_settings_ in one process is how a dlsym() picks the wrong one, so every
 * symbol here is `nd_setapp_`.
 *
 * ============ DECISION 3: THIS APP WRITES ONLY THE SETTING ============
 *
 * The Python reaches into the core's live memory three times -- it assigns
 * `ui.wallpaper` (main.py:91-93), flips `ui.engineering_mode` and rewrites
 * `ui.apps` (main.py:133-156). Across a process boundary that is not merely
 * discouraged, it is impossible: the app is a separate process and its `ui`
 * is its own copy. OPEN-QUESTIONS.md answer 3 settles it -- Settings writes
 * the setting and nothing else, and nd_ui_refresh_after_app() re-reads
 * system.ui.wallpaper, system.ui.engineering_mode and the app scan after
 * every app exit.
 *
 * MEASURED, THAT CHANGES NO PIXEL WHILE SETTINGS IS RUNNING. An app's
 * SoftKeyBar is always the opaque one -- framework.py:464 sets
 * `is_transparent = not hasattr(ui, 'softkey')` and the core's bar exists by
 * the time any app runs -- so framework.py:472's `if self.is_transparent and
 * wallpaper` is false for every bar this app paints, and no other screen in
 * the app reads ui.wallpaper, ui.apps or ui.engineering_mode. Neither golden
 * frame needed re-cutting.
 *
 * ============ THE CAPS THIS PORT ADDS ============
 *
 * The Python holds the wallpaper list in a Python list and never bounds it;
 * CODING-STANDARDS.md section 1.5 will not have an array sized by the
 * contents of an SD card. The three numbers mirror the Tones port's, which
 * scans the same shape of tree from the same three directories:
 *
 *   ND_SETAPP_MAX       256 wallpapers, heap allocated and freed before the
 *                       screen returns.
 *   ND_SETAPP_NAME_MAX  96 bytes of display name.
 *   ND_SETAPP_WALK_MAX  64 directories pending in the walk.
 *
 * All three are recorded in OPEN-QUESTIONS.md under ST-1.
 */

#ifndef ND_SETTINGS_APP_H_INCLUDED
#define ND_SETTINGS_APP_H_INCLUDED

#include <sys/types.h>

#include "nd_font.h"
#include "nd_nap.h"
#include "nd_paths.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ROOT_ID = 4 -- manifest.json, and the "4-1" breadcrumb on every screen. */
#define ND_SETAPP_ROOT_ID 4

/* SYSTEM_WALLPAPER_DIR / WALLPAPER_DIR. Absolute and load-bearing
 * (CODING-STANDARDS.md section 9.5); both go through nd_path_resolve(). */
#define ND_SETAPP_SYSTEM_WALLPAPER_DIR "/NeoDCT/System/wallpapers"
#define ND_SETAPP_WALLPAPER_DIR        "/NeoDCT/User/wallpapers"

/* SDCARD_HELPER. The app no longer runs it -- nd_svc_format_card() asks the
 * core to, so that no app has to hold the privilege to repartition a disk --
 * and the string moved to nd_paths.h with it. The name stays because it is
 * still the Python constant this file is a transcription of, and because
 * test_settings_app.c pins its value. */
#define ND_SETAPP_SDCARD_HELPER ND_PATH_SDCARD_HELPER


/* The memory-card screen's two strings for a card in the pre-0.5.0b FAT
 * format. Declared here rather than left as literals because the dialog has
 * room for FIVE LINES and a test measures these against the real font -- see
 * nd_msgdialog_measure(). A sixth line is clipped with a U+2026 the font
 * cannot draw, so it disappears silently, which is how a modem error shipped
 * with its second sentence missing. */
extern const char *const nd_setapp_sdcard_legacy;
extern const char *const nd_setapp_sdcard_legacy_help;

/* And the pair for a card that was made on somebody else's computer --
 * ND_CARD_FOREIGN. Same five-line budget, same reason it is a named constant
 * rather than a literal: it is a dialog, and a dialog that overflows is
 * clipped with a glyph this font cannot draw.
 *
 * Kept separate from the legacy pair even though both end at the same offer,
 * because the two are not the same fault and the remedies differ. A legacy
 * card WORKS for music and updates and only fails at apps; a foreign card
 * cannot be read at all here, and the owner has a second remedy the legacy
 * one does not -- copy the files across on the computer that owns them. */
extern const char *const nd_setapp_sdcard_foreign;
extern const char *const nd_setapp_sdcard_foreign_help;
/* SUPPORTED_WALLPAPERS. A real two-element tuple here, unlike the Tones
 * app's `(".mp3")`, which is a string (OPEN-QUESTIONS.md TN-2). */
/* .gif joined the two the Python shipped when animated wallpapers landed. It
 * goes LAST so the two that were always here keep their indices -- the order
 * is what test_settings_app.c pins, and a wallpaper list is sorted by name
 * anyway, so nothing on screen depends on it. */
#define ND_SETAPP_EXT_COUNT 3
extern const char *const nd_setapp_exts[ND_SETAPP_EXT_COUNT]; /* ".jpg", ".jpeg", ".gif" */

/* The magic value system.ui.wallpaper takes for "no wallpaper". It is stored
 * as the literal four characters, not as an empty value: nd_ui.h's reader
 * compares `wallpaper_setting.upper() != "NONE"`. */
#define ND_SETAPP_WALLPAPER_NONE "NONE"

#define ND_SETAPP_MAX      256
#define ND_SETAPP_NAME_MAX 96
#define ND_SETAPP_PATH_MAX 256
#define ND_SETAPP_WALK_MAX 64
#define ND_SETAPP_DIRS_MAX 3

/* GET_MORE_LABEL and the two help texts behind it. Which one is shown
 * depends on whether a card is in the phone right now, and getting that
 * backwards tells a user with no card to copy files onto it. */
extern const char *const nd_setapp_get_more_label;
extern const char *const nd_setapp_get_more_help;
extern const char *const nd_setapp_get_more_help_with_card;

/* SDCARD_HELP, the TextScroller behind every memory-card message. */
extern const char *const nd_setapp_sdcard_help;
/* The destructive confirmation, shared by both routes to a format. */
extern const char *const nd_setapp_format_warning;

/* run()'s VerticalList and _show_engineering_mode()'s.
 *
 * WAS 4. "Messages Style" is new and is placed BEFORE Engineering Mode so
 * that the two engineering-ish rows stay together at the end; the Python's
 * four are otherwise in their original order.
 *
 * WAS 6. "Install apps" sits right after "Memory card" because it is the
 * card's other job -- an installed app lives on it -- and an owner who has
 * just been told the card is ready is looking for the next thing to do with
 * it. That moves the third row of the first screen, so golden/app-settings
 * was re-cut. */
#define ND_SETAPP_MENU_ITEMS 7
extern const char *const nd_setapp_menu[ND_SETAPP_MENU_ITEMS];

/* ------------------------------------------------------------------ *
 * Install apps
 * ------------------------------------------------------------------ */

/* The help behind the "Install apps" row: what a .nap is, where to put one,
 * and what installing does. An nd_scroller, so it pages. */
extern const char *const nd_setapp_install_help;

/* Shown when the card is in but the scan found nothing. */
extern const char *const nd_setapp_install_none;

/* The confirmation before anything is written. Both are printf formats
 * taking the app's name, and both fit nd_msgdialog's five lines with a
 * ND_APP_NAME_MAX name -- test_settings_app.c measures them. */
extern const char *const nd_setapp_install_confirm;
extern const char *const nd_setapp_install_replace;

/* What an install screen says about a card that cannot hold apps: the
 * legacy FAT card. Points at the Memory card row, which offers the reformat. */
extern const char *const nd_setapp_install_legacy;

/* And for the card that belongs to another computer: not "set it up first",
 * which is an instruction that cannot be followed. */
extern const char *const nd_setapp_install_foreign;

/* ------------------------------------------------------------------ *
 * The BT Audio screen
 * ------------------------------------------------------------------ */

/* "Disconnect" is the longest row by a wide margin. */
#define ND_SETAPP_BT_LINE_MAX  24
#define ND_SETAPP_BT_MAX_ITEMS 3

/* bt_lines(): the rows, rebuilt from live state each time the screen is drawn,
 * because the screen IS the status display -- there is nowhere else on it to
 * say whether Bluetooth is on or whether something is connected.
 *
 * Off            -> Enable
 * On, nothing    -> Disable, Scan
 * On, connected  -> Disable, Scan, Disconnect
 *
 * Disconnect appears only when there is something to disconnect. A row that
 * does nothing teaches the owner that rows sometimes do nothing. */
size_t nd_setapp_bt_lines(char lines[][ND_SETAPP_BT_LINE_MAX], size_t max, bool enabled,
                          bool connected);
#define ND_SETAPP_ENG_ITEMS 2
extern const char *const nd_setapp_eng_options[ND_SETAPP_ENG_ITEMS];

/* ENGINEERING_MODE_KEY. Spelled here as the app spells it AND asserted equal
 * to nd_settings.h's ND_SET_UI_ENGINEERING by the test, so the two cannot
 * drift into writing a key the core does not read. */
#define ND_SETAPP_ENG_KEY "system.ui.engineering_mode"

/* Messages Style. The key and the two labels belong to the Messages app --
 * apps/Messages/messages.h owns them, and it is what READS the setting --
 * so they are spelled here only as the string Settings writes, and
 * test_settings_app.c asserts the two spellings match. Settings cannot
 * include messages.h: they are two separate .so files. */
#define ND_SETAPP_MSGSTYLE_KEY   "system.ui.messages_style"
#define ND_SETAPP_MSGSTYLE_ITEMS 2
extern const char *const nd_setapp_msgstyle_options[ND_SETAPP_MSGSTYLE_ITEMS];

/* ------------------------------------------------------------------ *
 * The scan
 * ------------------------------------------------------------------ */

/* One row of `wallpapers`.
 *
 * `path` carries THREE states, which is why it is not a bool:
 *   a real path      a file the walk found
 *   "NONE"           the row the Python inserts at index 0
 *   ""               the Python's `"path": None` -- the "Get more..." row,
 *                    which selects nothing and opens the help instead */
typedef struct {
    char name[ND_SETAPP_NAME_MAX];
    char path[ND_SETAPP_PATH_MAX];
} nd_wallpaper;

/* filename.lower().endswith(SUPPORTED_WALLPAPERS). ASCII-only lowering,
 * because Python's str.lower() does not consult the locale and strcasecmp
 * does -- they disagree about "I" in a Turkish locale. */
bool nd_setapp_is_supported(const char *filename);

/* os.path.splitext(os.path.basename(filename))[0].
 *
 * splitext's stem is not "up to the last dot": leading dots belong to the
 * name, so splitext(".hidden") is (".hidden", ""). Returns out. */
const char *nd_setapp_display_name(const char *filename, char *out, size_t out_sz);

/* _wallpaper_dirs(): stock wallpapers from the image, then the card's, then
 * user-added ones. LOGICAL paths, not ND_ROOT-resolved -- what goes into
 * system.ui.wallpaper has to be the path the core will read back. Returns how
 * many were written. */
size_t nd_setapp_wallpaper_dirs(char out[][ND_SETAPP_PATH_MAX], size_t max);

/* _scan_wallpapers(): every .jpg/.jpeg under every wallpaper directory,
 * sorted by name.lower() with the walk order preserved between equals.
 *
 * `out` is caller-owned and holds at least `max` entries. Returns how many
 * were written. */
size_t nd_setapp_scan(nd_wallpaper *out, size_t max);

/* ------------------------------------------------------------------ *
 * _wrap_text() -- the SEVENTH word-wrapper in this codebase
 * ------------------------------------------------------------------ *
 *
 * nd_text.h documents four that disagree, nd_widgets.h adds PagedList's as a
 * fifth, apps/Messages/main.c adds nd_msg_wrap_text() as a sixth
 * (OPEN-QUESTIONS.md MSG-7) and lib/nd_dialer.c a pair more (DL-5). This is
 * another one, and it is none of them:
 *
 *   * `text.split()` with no argument, so runs of whitespace collapse and
 *     NEWLINES ARE LOST -- nd_text_wrap() splits on them.
 *   * an empty or whitespace-only string returns ONE EMPTY LINE, not zero.
 *   * a word too wide for the column is left OVER-WIDE and un-ellipsised,
 *     which is where it parts company with Messages' otherwise
 *     line-for-line identical version (Settings/main.py:112-115 versus
 *     Messages/main.py:75-81).
 *
 * It lives here rather than in nd_text.c for the reason MSG-7 gives: moving
 * it next to the others invites exactly the merge nd_text.h warns against.
 *
 * Writes into `out`, which the caller sized. */
void nd_setapp_wrap_text(nd_lines *out, nd_ui *ui, const char *text, int32_t max_width,
                         const nd_font *font);

/* ------------------------------------------------------------------ *
 * _show_about()
 * ------------------------------------------------------------------ */

/* Everything _show_about() does except the key loop: paint the canvas, paint
 * the "Back" bar with present=False, then present. Split out so a test can
 * judge the frame without having to break a blocking wait. */
void nd_setapp_draw_about(nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_SETTINGS_APP_H_INCLUDED */
