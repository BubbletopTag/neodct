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

/* ---- the memory card, and the apps the owner installed ------------ */

/* ============ WHY APPS LIVE ON THE CARD AND NOT ON /NeoDCT/User ============
 *
 * They were briefly at /NeoDCT/User/apps, which does not fit: on the Luckfox
 * the user partition is EIGHT MEGABYTES, shared with the databases, the
 * settings, the logs, the browser profile and the pending update record. An
 * app directory there is a feature that fills the partition the phone needs
 * to save anything at all.
 *
 * So the card, which is also the honest place for it -- an installed app is
 * removable media by nature, and putting it on removable media makes that
 * true rather than merely said.
 *
 * ============ AND WHY THE CARD IS EXT4 ============
 *
 * FAT stores no ownership. Every permission on a FAT mount comes from uid=,
 * gid=, fmask= and dmask=, applied uniformly to the WHOLE filesystem -- which
 * is why the old card needed a second partition to give downloads a different
 * regime from music, and why an app.so on the media side would have been
 * 0640 ndusr:ndusr and therefore unreadable by the ndusr_ut process that has
 * to dlopen it.
 *
 * ext4 records owner, group and mode per inode. One partition then carries
 * directories with completely different rules, the second partition and the
 * whole partition table go away, and app code becomes something an app can
 * read and execute but not write. See neodct-sdcard's layout table.
 *
 * ============ WHAT IS UNTRUSTED, AND WHY IT IS THE PATH THAT SAYS SO ======
 *
 * Everything under the apps directory is UNTRUSTED, without exception and
 * regardless of what its manifest claims. The card is removable and now
 * readable on any Linux PC, so an app.so there is bytes a stranger chose --
 * which is exactly why the rule is about LOCATION and not about the app's own
 * assertions. nd_proc_app_is_untrusted() enforces it by prefix, the only
 * place in the tree where a prefix decides a privilege; the comment there
 * says why that is safe here and would not be elsewhere. */
#define ND_PATH_CARD_DIR      "/NeoDCT/User/sdcard"
#define ND_PATH_USER_APPS_DIR "/NeoDCT/User/sdcard/apps"

/* ============ THE NOTE THE INSTALLER LEAVES ============
 *
 * A counter, bumped by nd_nap_install(), that says "the set of installed apps
 * changed". It lives on the USER PARTITION and deliberately not on the card:
 * the whole point is to answer "do I need to read the card?" without reading
 * the card.
 *
 * The core used to answer that question by re-walking every app directory
 * after EVERY app exit, because an app exiting might have been Settings
 * installing something and nothing told it otherwise. That walk includes the
 * SD card, it happens before the menu draws its first frame, and a card that
 * is slow to answer cannot be interrupted while it answers -- so the phone sat
 * on a frozen home screen for as long as the card took. This file is the
 * "otherwise". */
#define ND_PATH_APPGEN "/NeoDCT/User/.appgen"

/* Bump the counter -- "the set of installed apps changed". Two callers:
 * nd_nap_install(), and nd_storage_setup_folders() (which turns a mounted card
 * into a READY one, i.e. one whose apps/ is looked at at all).
 *
 * FALSE WHEN IT COULD NOT BE WRITTEN, and the caller has to care. The failure
 * is not fail-safe and it would be comfortable to pretend otherwise: an
 * unchanged counter is read by the core as "nothing changed", so a successful
 * install whose note did not land is an app that never appears in the menu,
 * with nothing on screen and nothing in the log saying why. Fail-safe would be
 * the other direction, and there is no way to spell it in a counter.
 *
 * A counter and not a timestamp: nd_clock.h's boot floor exists because this
 * phone can come up believing it is 1970, and a token that goes backwards
 * would pin a stale list until the next install. */
bool nd_appgen_bump(void);

/* The counter's current value, or 0 when it has never been written. */
unsigned long nd_appgen_value(void);

/* The one directory on the card that ndusr_ut may WRITE: the browser's and
 * the media player's own state, and where a download lands. Deliberately not
 * inside apps/ -- it is shared, and app storage is not.
 *
 * An installed app's storage is its own <app>/data, created by the core at
 * first launch (nd_proc_launch_app). The app never creates it, because a
 * process that can create its own data directory can create siblings next to
 * its app.so instead. */
#define ND_PATH_CARD_UNTRUSTED "/NeoDCT/User/sdcard/untrusted"

/* The subdirectory of an installed app that the app may write. Its parent is
 * ndusr's, so the app cannot replace its own app.so -- which is the whole of
 * "an app cannot rewrite itself", and the reason persistence needs the owner
 * to install something rather than an app to decide to stay. */
#define ND_PATH_APP_DATA_NAME "data"
#define ND_PATH_VERSION_PROP  "/NeoDCT/System/version.prop"

/* "qemu or hw", hardcoded into the image beside the version so that nothing
 * has to work it out from a device node. Written by the same block of
 * post-build-system-metadata.sh that writes version.prop; read by
 * nd_platform.h in C and by /bin/nd-platform in the boot scripts.
 *
 * A sibling of version.prop rather than a key inside it, because this one has
 * to be legible to busybox sh: /bin/nd-platform hands on only values made of
 * [A-Za-z0-9._-] and throws the rest away, and version.prop's build time is a
 * date with spaces and a colon in it.
 *
 * NOT visible to the initramfs. This is in the root squashfs and the
 * initramfs runs before that is mounted; mkinitramfs.py does not pack it, and
 * nothing in neodct/initramfs/ asks the question -- see the block above the
 * heredoc in post-build-system-metadata.sh for why that is settled rather
 * than pending. */
#define ND_PATH_PLATFORM      "/NeoDCT/platform"
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

/* The Engineering tile's art. It sits with the selector's own resources and
 * NOT under a System/apps/Engineering/ directory, because there is no such
 * app: the tile is synthesised by the core and opens a second selector rather
 * than launching anything. A directory with a manifest and no app.so would be
 * a menu entry that nd-apprun fails to dlopen the moment something reached it
 * by another route. */
#define ND_PATH_ENG_TILE_ICON "/NeoDCT/System/ui/resources/img/appselector/engineering.png"

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

/* Hands a file that ROOT has just created to the owner of the directory it
 * sits in. nd_path.c has the reasoning; the short form is that nd-core is
 * root for a moment at boot, /NeoDCT/User belongs to ndusr, and a file root
 * leaves there under the 0027 umask is one ndusr can never read.
 *
 * A no-op that returns true when the caller is not root, or when the
 * directory is root's own (an image with no ndusr). False only when the
 * chown itself failed, or the file is not there to be given. ND_ROOT-
 * resolved like everything else here.
 *
 * ONLY FOR A PATH NOTHING UNTRUSTED CAN WRITE. It chowns by NAME, and
 * chown(2) follows a symlink, so in a directory somebody else can write this
 * is a root chown at a path they choose. Its two callers are /NeoDCT/User's
 * own files (0751 ndusr:ndusr -- ndusr_ut is not in that group and cannot
 * create a name there) and a directory Fetch has just created with mkdir(2)
 * inside apps/, which is 0755 ndusr:ndusr for the same reason. Anywhere else
 * -- untrusted/ above all -- use the two below. */
bool nd_path_give_to_dir_owner(const char *path);

/* Leave the mode alone. A `mode` argument below is applied with fchmod(2);
 * this says there is nothing to state, only an owner to hand it to. */
#define ND_PATH_MODE_KEEP ((unsigned int)~0u)

/* ============ THE SAME HANDOVER, WHERE AN ATTACKER IS STANDING ==========
 *
 * The card's untrusted/ is 0770 ndusr:ndusr_ut -- THE ONE directory the
 * untrusted set can write -- and the engineering Fetch app is root. Every
 * root operation there that names a FILE can have that name mean something
 * else by the time the syscall runs: chown(2), chmod(2) and open(2) without
 * O_NOFOLLOW all dereference, so a planted symlink turns a repair of a
 * download into a root chmod of /NeoDCT/User/settings.prop. It is the hole
 * neodct-sdcard's apply_layout() had in 0.5.15a, and a reviewer found it a
 * second time in the C beside it.
 *
 * A name cannot be made safe from a race; an OBJECT can. Both of these act
 * on a descriptor -- fchown(2) and fchmod(2) -- so whatever the name says a
 * moment later, the inode changed is the one that was checked:
 *
 *   _fd       for a caller that already holds the file open, which is the
 *             strongest form: it created the object and never let go of it.
 *             `path` is used ONLY to find the directory whose owner is
 *             copied down, never to reach the file.
 *   _nofollow for a caller that has only a name. The open refuses a symlink
 *             (ELOOP) rather than following it.
 *
 * ND_ERR_NOTFOUND when there is nothing there, ND_ERR_PERM when what is
 * there is not a plain single-linked file (a symlink, a directory, a fifo,
 * or a hard link somebody else also holds -- this image sets no
 * fs.protected_hardlinks, so a link into a directory an attacker owns is a
 * way to keep the file after root has changed its owner), ND_ERR_IO when the
 * fchown or the fchmod itself failed. ND_OK also covers the not-root and
 * root's-own-directory cases nd_path_give_to_dir_owner() returns true for. */
nd_err nd_path_give_fd_to_dir_owner(int fd, const char *path, unsigned int mode);
nd_err nd_path_give_to_dir_owner_nofollow(const char *path, unsigned int mode);

#ifdef __cplusplus
}
#endif

#endif /* ND_PATHS_H_INCLUDED */
