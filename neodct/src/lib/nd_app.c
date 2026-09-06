/* nd_app.c -- the app side of the process boundary.
 *
 * Three small things an app process needs and the core does not:
 *
 *   1. the SIGTERM flag. In Python an incoming call raised IncomingCall and
 *      the unwinding ran every app's `finally:` on the way out -- which is
 *      what released ALSA before the ringtone played. C has no unwinding, so
 *      the core sends SIGTERM, the handler here sets a flag, and nd-apprun
 *      calls app_shutdown() from ordinary code once the app returns. An app
 *      loop that runs longer than a frame polls nd_app_should_exit().
 *
 *   2. the app's own directory, so an asset ships beside the manifest and is
 *      opened without the app knowing where it was installed.
 *
 *   3. the inherited framebuffer. The whole point of NEODCT_FB_FD is that an
 *      app process needs no /dev/fb0 permission (SECURITY.md); wrapping the
 *      descriptor rather than reopening the device is what makes that true.
 *
 * Everything here is a no-op in the core process: nd_app_dir() answers "" and
 * nd_app_should_exit() stays false, because the core installs no handler.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_fb.h"
#include "nd_json.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

/* Written from a signal handler, read from everywhere. sig_atomic_t is the
 * only type C11 promises is safe to touch from both. */
static volatile sig_atomic_t g_should_exit;

/* The app's directory, argv[1]. Static rather than allocated: there is exactly
 * one per process and it must survive an allocator that has run out. */
static char g_app_dir[ND_PATH_MAX];

static void on_term(int signo)
{
    ND_UNUSED(signo);
    g_should_exit = 1;
}

bool nd_app_should_exit(void)
{
    return g_should_exit != 0;
}

nd_err nd_app_install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    (void)sigemptyset(&sa.sa_mask);
    /* NO SA_RESTART, deliberately. The Browser wrapper sits in a blocking
     * read() for a whole browsing session; with SA_RESTART that read would
     * resume and the app would never learn the phone was ringing. EINTR is
     * the notification. */
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        nd_log_err(ND_LOG_OS, "sigaction(SIGTERM): %s", strerror(errno));
        return ND_ERR_IO;
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        nd_log_err(ND_LOG_OS, "sigaction(SIGINT): %s", strerror(errno));
        return ND_ERR_IO;
    }
    /* A child that writes to a framebuffer or a closed key channel must see
     * the error, not die on it. */
    (void)signal(SIGPIPE, SIG_IGN);
    return ND_OK;
}

nd_err nd_app_set_dir(const char *dir)
{
    if (dir == NULL) {
        g_app_dir[0] = '\0';
        return ND_OK;
    }
    if (nd_strlcpy(g_app_dir, dir, sizeof g_app_dir) >= sizeof g_app_dir) {
        g_app_dir[0] = '\0';
        return ND_ERR_TOOLONG;
    }
    return ND_OK;
}

const char *nd_app_dir(void)
{
    return g_app_dir;
}

bool nd_app_manifest_use_wallpaper(const char *app_dir)
{
    char path[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    bool use;

    /* The core passes "" here and gets true, which is right: the core has no
     * manifest and nothing to opt out of. */
    if (app_dir == NULL || app_dir[0] == '\0')
        return true;
    if (nd_snprintf(path, sizeof path, "%s/manifest.json", app_dir) != ND_OK)
        return true;
    if (nd_json_parse_file(path, &doc, NULL, 0u) != ND_OK)
        return true;

    root = nd_json_root(doc);
    use = (root != NULL && nd_json_type_of(root) == ND_JSON_OBJECT)
              ? nd_json_get_bool(root, ND_APP_KEY_USE_WALLPAPER, true)
              : true;
    nd_json_free(doc);
    return use;
}

/* ------------------------------------------------------------------ *
 * manifest.json's "useKeypadDevice" / "keypadDeviceMap"
 * ------------------------------------------------------------------ */

nd_app_keydev nd_app_keydev_from_name(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return ND_APP_KEYDEV_RAW;
    if (strcmp(text, "raw") == 0)
        return ND_APP_KEYDEV_RAW;
    if (strcmp(text, "browser") == 0)
        return ND_APP_KEYDEV_BROWSER;
    if (strcmp(text, "shell") == 0)
        return ND_APP_KEYDEV_SHELL;
    return ND_APP_KEYDEV_NONE; /* "not a map name"; the caller decides */
}

nd_app_keydev nd_app_manifest_key_device(const char *app_dir)
{
    char path[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    nd_app_keydev kind = ND_APP_KEYDEV_NONE;

    /* The core passes "" and gets NONE, which is right: the core has no
     * manifest, and it is the process that MAKES the device rather than one
     * that could ask for one. */
    if (app_dir == NULL || app_dir[0] == '\0')
        return ND_APP_KEYDEV_NONE;
    if (nd_snprintf(path, sizeof path, "%s/manifest.json", app_dir) != ND_OK)
        return ND_APP_KEYDEV_NONE;
    if (nd_json_parse_file(path, &doc, NULL, 0u) != ND_OK)
        return ND_APP_KEYDEV_NONE;

    root = nd_json_root(doc);
    if (root != NULL && nd_json_type_of(root) == ND_JSON_OBJECT &&
        nd_json_get_bool(root, ND_APP_KEY_USE_KEYPAD_DEVICE, false)) {
        const char *map = nd_json_get_str(root, ND_APP_KEY_KEYPAD_DEVICE_MAP, NULL);

        kind = nd_app_keydev_from_name(map);
        if (kind == ND_APP_KEYDEV_NONE) {
            /* A typo in a preference must not take the keypad away: the app
             * asked for a device and gets one, untranslated, and the log says
             * which word was not understood so the author can fix it. */
            nd_log_err(ND_LOG_INPUT,
                       "%s: manifest " ND_APP_KEY_KEYPAD_DEVICE_MAP
                       "=\"%s\" is not raw, browser or shell; using raw",
                       app_dir, map);
            kind = ND_APP_KEYDEV_RAW;
        }
    }
    nd_json_free(doc);
    return kind;
}

const char *nd_app_key_evdev(void)
{
    const char *env = getenv(ND_ENV_KEY_EVDEV);

    return (env != NULL && env[0] != '\0') ? env : NULL;
}

nd_err nd_app_asset_path(char *out, size_t out_sz, const char *name)
{
    if (out == NULL || out_sz == 0u || name == NULL)
        return ND_ERR_INVAL;
    if (g_app_dir[0] == '\0') {
        out[0] = '\0';
        return ND_ERR_NOTFOUND;
    }
    return nd_path_join(out, out_sz, g_app_dir, name);
}

/* ------------------------------------------------------------------ *
 * The keypad fact the core hands down
 * ------------------------------------------------------------------ */

bool nd_app_keypad_is_matrix(void)
{
    const char *env = getenv(ND_ENV_KEYPAD_MATRIX);

    return env != NULL && env[0] == '1' && env[1] == '\0';
}

/* ------------------------------------------------------------------ *
 * The inherited framebuffer
 * ------------------------------------------------------------------ */

nd_err nd_app_fb_from_env(nd_fb **out)
{
    const char *s;
    char *end = NULL;
    long fd;

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    s = getenv(ND_ENV_FB_FD);
    if (s == NULL || s[0] == '\0')
        return nd_fb_open(out, ND_PATH_FB);

    errno = 0;
    fd = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || fd < 0 || fd > 1000000L) {
        nd_log_err(ND_LOG_FB, "%s=%s is not a descriptor", ND_ENV_FB_FD, s);
        return nd_fb_open(out, ND_PATH_FB);
    }
    return nd_fb_adopt_fd(out, (int)fd);
}
