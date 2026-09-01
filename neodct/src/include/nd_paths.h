/* nd_paths.h -- every absolute runtime path in one place, plus the one hook
 * that lets the host test harness redirect them.
 *
 * AGENTS.md is explicit that /NeoDCT/... paths are load-bearing: the phone's
 * rootfs is a read-only squashfs mounted at /, the user partition is at
 * /NeoDCT/User, and half the shell scripts in the image hard-code these
 * strings. So the constants below are the real thing, unprefixed.
 *
 * The host tests obviously cannot write to /NeoDCT. Rather than teaching
 * seventy call sites about a test mode, every path that is OPENED goes through
 * nd_path_resolve(), which prepends ND_ROOT (the NEODCT_ROOT environment
 * variable, empty by default). Introduced now on purpose -- retrofitting it
 * later means auditing every fopen in the project.
 */

#ifndef ND_PATHS_H_INCLUDED
#define ND_PATHS_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the read-only system image ---------------------------------- */
#define ND_PATH_SYSTEM       "/NeoDCT/System"
#define ND_PATH_BIN_DIR      "/NeoDCT/System/bin"
#define ND_PATH_LIB_DIR      "/NeoDCT/System/lib"
#define ND_PATH_ND_CORE      "/NeoDCT/System/bin/nd-core"
#define ND_PATH_ND_APPRUN    "/NeoDCT/System/bin/nd-apprun"
#define ND_PATH_APPS_DIR     "/NeoDCT/System/apps"
#define ND_PATH_ENG_APPS_DIR "/NeoDCT/System/engineering/apps"

/* Where an app the OWNER installed lives, as opposed to one the image shipped.
 *
 * It is on the WRITABLE partition, which is the whole point and the whole
 * problem. Anything that can write a file here gets code that runs when the
 * owner opens it, and /NeoDCT/User survives system updates -- so unlike the
 * rootfs, a bad app here is not removed by reinstalling the OS.
 *
 * Everything under this directory is therefore UNTRUSTED, without exception
 * and regardless of what its manifest claims. nd_proc_app_is_untrusted()
 * enforces it by prefix, and that is the only place in the tree where a prefix
 * decides a privilege -- see the comment there for why it is safe here and
 * would not be elsewhere. */
#define ND_PATH_USER_APPS_DIR "/NeoDCT/User/apps"
#define ND_PATH_VERSION_PROP  "/NeoDCT/System/version.prop"
#define ND_PATH_DISPLAYD      "/NeoDCT/System/hw/neodct_displayd"
/* The SD-card helper. Lives here rather than in settings_app.h because the
 * CORE runs it now: formatting a card is a verb on the service socket
 * (nd_svc.h), and the app that used to spawn it can no longer spawn
 * anything. settings_app.h still names it, pointing at this. */
#define ND_PATH_SDCARD_HELPER "/NeoDCT/System/hw/neodct-sdcard"

#define ND_PATH_FONT             "/NeoDCT/System/ui/resources/fonts/font.ttf"
#define ND_PATH_HOME_LAYOUT      "/NeoDCT/System/ui/resources/ui_home.json"
#define ND_PATH_ENVELOPE         "/NeoDCT/System/ui/resources/img/envelope.png"
#define ND_PATH_CRASH_IMAGE      "/NeoDCT/System/ui/resources/CRASH.jpg"
#define ND_PATH_WARNING_ICON     "/NeoDCT/System/ui/resources/img/errorscreen/warning.png"
#define ND_PATH_PLACEHOLDER_ICON "/NeoDCT/System/ui/resources/img/appselector/placeholder_icon.png"

#define ND_PATH_TONES_DIR "/NeoDCT/System/tones"
#define ND_PATH_DTMF_DIR  "/NeoDCT/System/tones/dtmf"
#define ND_PATH_SMS_TONE  "/NeoDCT/System/tones/sms.wav"
#define ND_PATH_T9_DICT   "/NeoDCT/System/core/t9.dict"

/* ---- the writable user partition --------------------------------- */
#define ND_PATH_USER "/NeoDCT/User"

/* The mode of that directory, and it is load-bearing rather than tidy.
 *
 * 0751 is o+x WITHOUT o+r: ndusr_ut can resolve a path THROUGH the partition
 * to reach /NeoDCT/User/browser, and cannot list the partition to discover
 * the ssh keys, the databases and the update records by name. Traversal and
 * listing are different bits and the whole confinement in SECURITY-PLAN.md
 * section 1 rests on the difference.
 *
 * Take o+x away and the browser has nowhere to write. Add o+r -- which is
 * what 0755 does, and 0755 is the reflex -- and the boundary is gone with no
 * other symptom. So anything that fixes this directory's mode fixes it to
 * THIS, and says so by using this name.
 *
 * overlay/etc/init.d/S00userdata carries the same number in its own layout
 * table, because it is shell and cannot include a header;
 * tests/test_userdata_layout.py pins the two together. */
#define ND_MODE_USER_DIR      0751u
#define ND_PATH_SETTINGS_PROP "/NeoDCT/User/settings.prop"
#define ND_PATH_KEYMAP        "/NeoDCT/User/keymap.json"
#define ND_PATH_WALLPAPER     "/NeoDCT/User/wallpaper.jpg"
#define ND_PATH_DB_DIR        "/NeoDCT/User/db"
#define ND_PATH_DB_PHONEBOOK  "/NeoDCT/User/db/phonebook.db"
#define ND_PATH_DB_SMS_INBOX  "/NeoDCT/User/db/sms_inbox.db"
#define ND_PATH_DB_SMS_OUTBOX "/NeoDCT/User/db/sms_outbox.db"
#define ND_PATH_DB_CALL_LOG   "/NeoDCT/User/db/call_log.db"
#define ND_PATH_DB_CALENDAR   "/NeoDCT/User/db/calendar.db"
#define ND_PATH_LOG_DIR       "/NeoDCT/User/logs"
#define ND_PATH_CRASH_LOG     "/NeoDCT/User/logs/crash.log"
#define ND_PATH_CRASH_LOG_1   "/NeoDCT/User/logs/crash.log.1"
#define ND_PATH_ACK_SECURITY  "/NeoDCT/User/.ack_security_warning"
#define ND_PATH_CLOCK_STATE   "/NeoDCT/User/.clock"
#define ND_PATH_SDCARD_MOUNT  "/NeoDCT/User/sdcard"
#define ND_PATH_REMOTE_DIR    "/NeoDCT/User/.remote"

/* ---- volatile state ---------------------------------------------- */
#define ND_PATH_SDCARD_STATE "/run/neodct/sdcard.prop"
#define ND_PATH_MODEM_LOCK   "/tmp/neodct-modem.lock"
#define ND_PATH_BANNER       "/etc/neodct-banner"
#define ND_PATH_COLORS_SH    "/etc/neodct-colors.sh"

/* ---- devices ------------------------------------------------------ */
#define ND_PATH_FB         "/dev/fb0"
#define ND_PATH_KEYPAD     "/dev/input/event0"
#define ND_PATH_SERIAL_FIQ "/dev/ttyFIQ0"
#define ND_PATH_SERIAL_AMA "/dev/ttyAMA0"

/* ---- environment overrides the Python already honours ------------- */
#define ND_ENV_ROOT          "NEODCT_ROOT"
#define ND_ENV_KEYPAD_DEVICE "NEODCT_KEYPAD_DEVICE"
#define ND_ENV_SERIAL_DEVICE "NEODCT_SERIAL_DEVICE"
#define ND_ENV_COLOR         "NEODCT_COLOR"
#define ND_ENV_NO_COLOR      "NO_COLOR"

#define ND_PATH_MAX 512

/* Writes ND_ROOT + path into out. With no NEODCT_ROOT set this is a plain
 * copy, so the production build pays one strlen and nothing else.
 * Returns ND_ERR_TOOLONG rather than truncating. */
nd_err nd_path_resolve(char *out, size_t out_sz, const char *path);

/* Same, with a trailing "/child" appended. Both components are resolved once,
 * so this is the right way to build "<appdir>/manifest.json". */
nd_err nd_path_join(char *out, size_t out_sz, const char *dir, const char *child);

/* The active ND_ROOT prefix, "" when unset. Owned by libneodct; never NULL.
 * Read once at first use and cached, so changing the environment mid-process
 * has no effect -- call nd_path_set_root() instead. */
const char *nd_path_root(void);

/* Test hook. Pass NULL or "" to clear. Copies the string. */
nd_err nd_path_set_root(const char *root);

/* mkdir -p, honouring ND_ROOT. mode 0755. Already-exists is ND_OK. */
nd_err nd_mkdir_p(const char *path, unsigned int mode);

/* Cheap existence tests, ND_ROOT-resolved. Errors read as "no". */
bool nd_path_exists(const char *path);
bool nd_path_is_dir(const char *path);
bool nd_path_is_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ND_PATHS_H_INCLUDED */
