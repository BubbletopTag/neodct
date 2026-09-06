/* nd_app.h -- what an app IS.
 *
 * An app is a directory under /NeoDCT/System/apps containing manifest.json,
 * icon.png and app.so. The core never loads app.so. It fork()s and
 * immediately execve()s /NeoDCT/System/bin/nd-apprun, which dlopen()s the
 * .so, resolves one entry point, calls it once, and exits with its return
 * value. The kernel enforces the boundary in hardware, so a null dereference
 * in an app kills the app and nothing else.
 *
 * ============ THE FIVE EXPORTED SYMBOLS ============
 *
 *     int  app_run(nd_ui *ui);          MANDATORY. The C equivalent of the
 *                                       Python's run(ui). Return 0 for a
 *                                       normal exit; non-zero is treated as a
 *                                       crash and gets the crash screen.
 *
 *     void app_shutdown(void);          MANDATORY. See the teardown contract
 *                                       below. An app with nothing to release
 *                                       still exports an empty one, so that
 *                                       "missing" always means "the author
 *                                       forgot" and never "nothing to do".
 *
 *     int  app_open_message(nd_ui *ui, int64_t message_id);   Messages only.
 *     int  app_open_inbox(nd_ui *ui);                         Messages only.
 *     int  app_open_event(nd_ui *ui, int64_t event_id);        Calendar only.
 *
 * The fifth exists for the same reason the third does: a banner on the home
 * screen names ONE thing, and pressing it has to arrive at that thing rather
 * than at the app's front door. A reminder's banner carries the event's
 * rowid exactly as a text's carries the inbox rowid.
 *
 * nd-apprun picks the entry point from argv[2], defaulting to "run":
 *
 *     nd-apprun <app-dir> [entry] [arg]
 *     nd-apprun /NeoDCT/System/apps/Koki
 *     nd-apprun /NeoDCT/System/apps/Messages open_message 41
 *     nd-apprun /NeoDCT/System/apps/Calendar open_event 3
 *
 * ============ THE SIGTERM TEARDOWN CONTRACT ============
 *
 * THIS IS THE PART THAT BREAKS THE PHONE IF IT IS GOT WRONG, so it is spelled
 * out completely.
 *
 * In Python an incoming call raises IncomingCall, and the exception unwinding
 * up through the app is WHAT RUNS EACH APP'S finally: BLOCK. That is how the
 * sound card gets released before the ringtone plays. C has no unwinding. If
 * an app is simply killed, ALSA is still held and the phone rings silently.
 *
 * So:
 *
 *   1. The core's modem thread sees RING and sends the child SIGTERM.
 *   2. nd-apprun's handler is installed WITHOUT SA_RESTART, so a blocked
 *      read() or getline() returns EINTR rather than resuming -- the Browser
 *      wrapper sits in exactly such a read for a whole browsing session.
 *   3. The handler sets a flag. nd-apprun calls app_shutdown() from ordinary
 *      code, not from the handler, then _exit(0).
 *   4. app_shutdown() releases the sound card, kills any child process it
 *      spawned, and returns. It must not draw, must not allocate, and must
 *      not block for more than a moment.
 *   5. The core waitpid()s with a timeout, escalating to SIGKILL if the child
 *      does not go, and only then starts the ringer.
 *
 * nd_app_should_exit() is how a long-running loop notices in between.
 *
 * ============ INHERITED DESCRIPTORS ============
 *
 * The child inherits four descriptors, whose numbers arrive in the
 * environment because the numbers themselves are not fixed:
 *
 *     NEODCT_KEYPAD_FD   read end of the key channel; press AND release
 *                        records in evdev format (see nd_input.h)
 *     NEODCT_CRASH_FD    write end of the crash report pipe; nd-apprun's
 *                        signal handler writes the signal, si_code, faulting
 *                        address and a backtrace here before re-raising
 *     NEODCT_FB_FD       the already-open framebuffer, so the child does not
 *                        need /dev/fb0 permission
 *     NEODCT_SERVICE_FD  an AF_UNIX SOCK_SEQPACKET socketpair to the core,
 *                        on which the app asks the core to do the four
 *                        things it cannot do itself -- send an SMS, snapshot
 *                        the modem, snapshot the battery, quick-start the
 *                        gauge. See nd_svc.h; ABSENT is not an error, it
 *                        just means there is nobody to ask.
 *
 * Everything else is closed on exec.
 *
 * ============ AND ONE FACT THAT IS NOT A DESCRIPTOR ============
 *
 *     NEODCT_KEYPAD_MATRIX  "1" when the core's keypad is the i2c matrix
 *
 * An app CANNOT WORK THIS OUT FOR ITSELF. nd_ui_init_app() derives
 * ui->has_matrix_keypad from ui->input, and an app's input is the pipe above
 * -- which has no matrix by construction, on every device. So the flag read
 * that way is false everywhere, and everything gated on it (multi-tap,
 * predictive, the # mode cycle, the T9 mode indicator) is dead in every app
 * on the phone it was written for. That was BR-3 in OPEN-QUESTIONS.md.
 *
 * The core knows the answer -- it opened the backend -- so it simply says so.
 * Anything that wants the fact reads the flag; nothing re-derives it by
 * reading keymap.json a second time, because a keymap on disk is not the
 * same claim as a matrix the core actually opened.
 *
 * ============ WHAT AN APP MAY CALL ============
 *
 * All of libneodct: the widgets, the rasterizer, settings, storage, the
 * databases, JSON. NOT the modem, battery or notify handles -- those live in
 * the core and are NULL in an app's context. What an app needs from the first
 * two it asks for over NEODCT_SERVICE_FD, through nd_svc.h, which is a
 * DELIBERATELY NARROW surface: four operations, no dial, no answer, no
 * hang-up and no raw AT.
 *
 * An app MUST NOT write to the core's state. Settings used to assign
 * ui->wallpaper and rewrite ui->apps; it now writes the setting and the core
 * re-reads it on exit. See nd_ui.h.
 */

#ifndef ND_APP_H_INCLUDED
#define ND_APP_H_INCLUDED

#include "nd_fb.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_APP_ENTRY_RUN          "run"
#define ND_APP_ENTRY_OPEN_MESSAGE "open_message"
#define ND_APP_ENTRY_OPEN_INBOX   "open_inbox"
#define ND_APP_ENTRY_OPEN_EVENT   "open_event"

/* manifest.json's key, spelled camelCase because that is how it is written in
 * the manifests and JSON keys are not C identifiers. */
#define ND_APP_KEY_USE_WALLPAPER "useWallpaper"

/* manifest.json's two key-device keys. See nd_app_manifest_key_device(). */
#define ND_APP_KEY_USE_KEYPAD_DEVICE "useKeypadDevice"
#define ND_APP_KEY_KEYPAD_DEVICE_MAP "keypadDeviceMap"

#define ND_APP_SYM_RUN          "app_run"
#define ND_APP_SYM_SHUTDOWN     "app_shutdown"
#define ND_APP_SYM_OPEN_MESSAGE "app_open_message"
#define ND_APP_SYM_OPEN_INBOX   "app_open_inbox"
#define ND_APP_SYM_OPEN_EVENT   "app_open_event"

#define ND_ENV_KEYPAD_FD  "NEODCT_KEYPAD_FD"
#define ND_ENV_CRASH_FD   "NEODCT_CRASH_FD"
#define ND_ENV_FB_FD      "NEODCT_FB_FD"
#define ND_ENV_SERVICE_FD "NEODCT_SERVICE_FD"

/* The /dev/input/eventN node the core made for THIS launch, when the app's
 * manifest asked for one. Not a descriptor: a path, because the program that
 * needs it is usually not this process -- it is the binary this app is about
 * to exec, which opens it by name. Absent means there is no such device, and
 * that is normal on a phone with a real keyboard. See nd_app_key_evdev(). */
#define ND_ENV_KEY_EVDEV "NEODCT_KEY_EVDEV"

/* "1" when the core's keypad is the i2c matrix, absent otherwise. NOT a
 * descriptor -- it is the one FACT about the keypad an app cannot work out
 * for itself, because its own input is a pipe. See the block above. */
#define ND_ENV_KEYPAD_MATRIX "NEODCT_KEYPAD_MATRIX"

/* Developer override for the same flag, honoured in the core AND in every
 * app: "0" forces T9 off, any other non-empty value forces it on. Unset in
 * production and in every golden frame, so the reference captures are
 * unaffected. This is how T9 is exercised on a QEMU keyboard, where there is
 * no i2c matrix to detect. */
#define ND_ENV_T9 "NEODCT_T9"

/* The file an app's compiled code lives in, beside its manifest.json. */
#define ND_APP_SO_NAME "app.so"

/* ------------------------------------------------------------------ *
 * What an app EXPORTS. Declared here so a typo is a compile error in the
 * app rather than a dlsym failure at runtime.
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui);
void app_shutdown(void);

/* Messages only; nd-apprun tolerates their absence in every other app. */
int app_open_message(nd_ui *ui, int64_t message_id);
int app_open_inbox(nd_ui *ui);

/* Calendar only, and tolerated absent in the same way. */
int app_open_event(nd_ui *ui, int64_t event_id);

/* ------------------------------------------------------------------ *
 * What an app CALLS, from libneodct
 * ------------------------------------------------------------------ */

/* True when the core's keypad is the i2c matrix: the HARDWARE fact, straight
 * from NEODCT_KEYPAD_MATRIX, WITHOUT the NEODCT_T9 policy override folded in.
 *
 * ui->has_matrix_keypad is the other question and they are not the same one:
 *
 *   ui->has_matrix_keypad  "should keys mean T9?"  -- multi-tap, predictive,
 *                          the # cycle, the mode indicator. A developer may
 *                          turn this on over a QWERTY keyboard to exercise it.
 *   this function          "is the console's keyboard missing?" -- which is
 *                          what LinuxShell and the Browser ask before they
 *                          start a uinput bridge.
 *
 * Folding the override into the second one would make NEODCT_T9=1 on a dev
 * box start a bridge alongside a real keyboard, and netsurf would then see
 * every press twice. */
bool nd_app_keypad_is_matrix(void);

/* True once SIGTERM has arrived. Poll it in any loop that runs longer than a
 * frame; return from app_run() promptly when it goes true. Reading it is a
 * relaxed atomic load, safe from anywhere. */
bool nd_app_should_exit(void);

/* Installed by nd-apprun before the app is loaded; an app never calls this. */
nd_err nd_app_install_signal_handlers(void);

/* The app's own directory, absolute, as passed in argv[1]. Owned by
 * libneodct; never NULL inside an app process, always "" inside the core. */
const char *nd_app_dir(void);

/* ============ manifest.json's "useWallpaper" ============
 *
 * The framework draws the wallpaper behind its own chrome -- see
 * nd_ui_paint_chrome() -- and some apps must not have it. Three kinds:
 *
 *   an app that fills the screen with its own art       Koki, Games
 *   an app whose screens are a diagnostic, not decor    every engineering
 *                                                        app, Update
 *   an app that hands the panel to something else       Browser
 *
 * So the manifest carries a boolean and the app decides:
 *
 *     { "name": "Games", "id": "6", "useWallpaper": false }
 *
 * ABSENT MEANS TRUE. That is the direction that matters: a manifest written
 * before this key existed -- every one shipped so far, and every third-party
 * one -- gets the wallpaper, and an app opts OUT rather than having to know
 * to opt in. A value that is present but not a JSON boolean is also true; the
 * key is a preference, and refusing to draw a background over a typo is worse
 * than ignoring it. (nd_manifest.h's rules are stricter, and deliberately so:
 * that file decides whether to overwrite the root filesystem.)
 *
 * The flag reaches library code as ui->app_use_wallpaper, set once by
 * nd_ui_init_app() and read by nothing else. */
bool nd_app_manifest_use_wallpaper(const char *app_dir);

/* ============ manifest.json's "useKeypadDevice" ============
 *
 * THIS IS HOW A PROGRAM THAT IS NOT AN app.so GETS THE KEYPAD, and it is the
 * only supported way. Read nd_proc.h's "THE KEY DEVICE" block for the whole
 * argument; the short version:
 *
 *   - an app.so reads keys from the pipe in NEODCT_KEYPAD_FD, and always has;
 *   - a real ELF binary an app STARTS -- netsurf-fb, mpv, an emulator, a
 *     viewer -- cannot read that pipe. It scans /dev/input, and on the phone
 *     /dev/input is empty because the keypad is an i2c matrix;
 *   - so an app that wraps such a binary asks for a device, and the core --
 *     which is the only process allowed to inject keys -- makes one and puts
 *     its /dev/input/eventN path in ND_ENV_KEY_EVDEV for the app to pass on.
 *
 *     { "name": "PlayStation", "id": "113", "useKeypadDevice": true }
 *
 * ABSENT MEANS FALSE, the opposite direction from useWallpaper, and for the
 * opposite reason: a wallpaper is decoration and costs an app nothing, while
 * a key device is a real evdev node in a global namespace that every other
 * program on the phone can also see. An app that does not need one must not
 * get one, or two programs end up reading the same keys.
 *
 * "keypadDeviceMap" chooses what the device carries. It is optional and only
 * consulted when useKeypadDevice is true:
 *
 *   "raw"      (the default) the sixteen keys the phone HAS, as themselves,
 *              press and release: NaviKey (KEY_ENTER), C (KEY_BACKSPACE), Up,
 *              Down, 0-9 on the number row, * (KEY_LEFTSHIFT) and #
 *              (KEY_BACKSLASH). NeoDCT keycodes are Linux keycodes, so
 *              nothing is translated and nothing is lost.
 *   "browser"  nd_t9_bridge's browser map: 2/4/5/6/8 stand in for a d-pad
 *              (the phone has no Left or Right key at all), and # cycles the
 *              rest of the pad through abc / ABC / 123 so a URL can be typed.
 *              For a program that expects a QWERTY keyboard and a cursor.
 *   "shell"    nd_t9_bridge's shell map: multi-tap letters everywhere and *
 *              as Tab. For a program that expects a terminal.
 *
 * The MAP is the app's choice and the INJECTION is the core's job, and that
 * split is the point: an app says what its binary expects to read, and never
 * gets a descriptor it could type into the real UI with.
 *
 * An unknown map string is "raw" with a log line -- refusing to start an app
 * over a typo in a preference would be worse than giving it the keys
 * untranslated. */
typedef enum {
    ND_APP_KEYDEV_NONE = 0, /* the manifest did not ask for one */
    ND_APP_KEYDEV_RAW,
    ND_APP_KEYDEV_BROWSER,
    ND_APP_KEYDEV_SHELL
} nd_app_keydev;

/* What `app_dir`/manifest.json asks for. ND_APP_KEYDEV_NONE for a missing,
 * unparseable or silent manifest, and for the core's own "". */
nd_app_keydev nd_app_manifest_key_device(const char *app_dir);

/* The map names as they are spelled in a manifest, and the parse, exposed so
 * a test can pin the table rather than a copy of it. `text` may be NULL. */
nd_app_keydev nd_app_keydev_from_name(const char *text);

/* The evdev node the core made for this app, or NULL when it made none --
 * because the manifest did not ask, or because this phone has a real keyboard
 * and needs no synthetic one. Pass it to the binary you are starting; do not
 * scan /dev/input for it yourself, because on a phone with two bridges up
 * there is more than one answer and only this one is yours. */
const char *nd_app_key_evdev(void);

/* "<app dir>/<name>", the correct way to open an asset that ships with the
 * app. Honours ND_ROOT like everything else. */
nd_err nd_app_asset_path(char *out, size_t out_sz, const char *name);

/* ---- ADDITIVE, and only nd-apprun calls these -------------------- *
 *
 * Three declarations the frozen header set implies but never spells. Nothing
 * above changed; see the P-section of OPEN-QUESTIONS.md.
 */

/* Record argv[1] so nd_app_dir() and nd_app_asset_path() can answer. Called
 * once, before the .so is loaded. */
nd_err nd_app_set_dir(const char *dir);

/* Wrap the framebuffer descriptor the core passed in NEODCT_FB_FD, so the app
 * process needs no /dev/fb0 permission -- that is the entire reason the
 * descriptor is inherited (SECURITY.md). Falls back to opening the device
 * when the variable is absent, which is what a hand-run nd-apprun does. */
nd_err nd_app_fb_from_env(nd_fb **out);

/* The mechanism underneath it: an nd_fb over a descriptor somebody else
 * opened. Declared here rather than in nd_fb.h because it exists solely for
 * the process boundary this header describes, and nd_fb.h is another work
 * package's contract. The mapping is NOT re-zeroed; see nd_fb_adopt.c. */
nd_err nd_fb_adopt_fd(nd_fb **out, int fd);

#ifdef __cplusplus
}
#endif

#endif /* ND_APP_H_INCLUDED */
