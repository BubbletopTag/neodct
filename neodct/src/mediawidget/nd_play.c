/* nd_play.c -- neodct-play, the one program that starts mpv.
 *
 *     neodct-play URL
 *     neodct-play --dry-run URL       print the command instead of running it
 *     neodct-play --parent PID        suspend PID rather than our parent
 *     neodct-play --no-suspend URL    leave the application running
 *
 * NetSurf execs this when a <video> placeholder is clicked, and the path it
 * execs is a compile-time constant in the browser
 * (netsurf-neodct/netsurf/frontends/framebuffer/neodct/neodct_media.h):
 *
 *     /NeoDCT/System/core/MediaWidget/neodct-play
 *
 * so this binary installs exactly there. It was missing from the C branch
 * entirely, which is why the browser answered "Cannot play that" for every
 * video on the web.
 *
 * Doing it as a separate program rather than inside the browser keeps every
 * decision about how mpv runs in one place -- see nd_media.h -- and gives
 * the browser a way to be stopped for the duration without knowing anything
 * about signals: the pid suspended is our own parent.
 *
 * Exit status is mpv's, so the caller can tell playback apart from a build
 * with no mpv in it (127).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_media.h"
#include "nd_paths.h"
#include "nd_types.h"

static void usage(FILE *out)
{
    (void)fprintf(out,
                  "usage: neodct-play [options] URL\n"
                  "  --parent PID    pid to stop while playing (default: our parent)\n"
                  "  --no-suspend    leave the calling application running\n"
                  "  --dry-run       print what would happen and exit\n"
                  "  --device DEV    evdev device the keypad arrives on\n"
                  "  --fbdev DEV     framebuffer to decode into (default %s)\n",
                  ND_MEDIA_FBDEV);
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    const char *device = NULL;
    const char *fbdev = ND_MEDIA_FBDEV;
    const char *mpv;
    char discovered[ND_PATH_MAX];
    char sock[ND_PATH_MAX];
    bool no_suspend = false;
    bool dry_run = false;
    bool end_of_options = false;
    long parent = -1;
    pid_t suspend_pid;
    int keypad_fd = -1;
    int i;

    for (i = 1; i < argc; i++) {
        /* "--" ends the options. THE BROWSER DEPENDS ON THIS.
         *
         * neodct_media_argv() in the NetSurf fork builds exactly
         *
         *     neodct-play -- <url>
         *
         * and its comment says why: "A src is text off a web page and
         * '--parent 1' in one would otherwise be read as options." That is
         * the right thing to do, and Python's argparse handled it for free.
         * This hand-written parser did not, so every <video> on the web
         * died on "unknown option --" until it did. */
        if (end_of_options) {
            if (url == NULL) {
                url = argv[i];
            } else {
                (void)fprintf(stderr, "neodct-play: one url at a time\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--") == 0) {
            end_of_options = true;
        } else if (strcmp(argv[i], "--no-suspend") == 0) {
            no_suspend = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--parent") == 0 && i + 1 < argc) {
            char *end = NULL;

            parent = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0') {
                (void)fprintf(stderr, "neodct-play: bad --parent %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--fbdev") == 0 && i + 1 < argc) {
            fbdev = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            (void)fprintf(stderr, "neodct-play: unknown option %s\n", argv[i]);
            usage(stderr);
            return 2;
        } else if (url == NULL) {
            url = argv[i];
        } else {
            (void)fprintf(stderr, "neodct-play: one url at a time\n");
            return 2;
        }
    }

    if (url == NULL) {
        usage(stderr);
        return 2;
    }

    suspend_pid = 0;
    if (!no_suspend) {
        suspend_pid = parent >= 0 ? (pid_t)parent : getppid();
        /* Under an init that reaped our parent we would be looking at pid 1,
         * and stopping init is not recoverable without the battery. */
        if (suspend_pid <= 1)
            suspend_pid = 0;
    }

    /* NEODCT_MPV exists so the test suite can put something predictable in
     * mpv's place; on the phone the default is the only mpv there is. */
    mpv = getenv("NEODCT_MPV");
    if (mpv == NULL || mpv[0] == '\0')
        mpv = ND_MPV_BIN;

    /* An empty --device means "find one", not "use the empty string": the
     * Python spells this `args.device or discover()` and an empty string is
     * falsy there. Getting this wrong would silently disable the keypad
     * bridge for anyone who passed --device "". */
    if (device != NULL && device[0] == '\0')
        device = NULL;

    /* ============ ASK BEFORE SEARCHING ============
     *
     * NEODCT_KEY_EVDEV is the node the CORE made for the app that started us
     * -- nd_app.h, and nd_proc.h's THE KEY DEVICE. Reading it here means any
     * app that asked for a key device can wrap this player and have the keys
     * work, without knowing that mpv exists or that nd_media has an override
     * of its own. The browser sets NEODCT_KEYPAD_DEVICE from the same value
     * and that still wins, because an explicit media override is a narrower
     * statement than "here is the app's keypad".
     *
     * It matters most where the search cannot work at all. On the phone the
     * keypad is an i2c matrix and /dev/input is EMPTY, so
     * nd_media_discover_keypad() has exactly one thing it can ever match: a
     * uinput device called "neodct-t9-keypad". That device used to be created
     * by the Browser app, which stopped being able to create it the day it
     * became ndusr_ut -- and from then on mpv had no keys on hardware at all.
     * A full-screen player that cannot be quit, with the browser SIGSTOPped
     * underneath it, is a phone the owner has to take the battery out of. */
    if (device == NULL) {
        const char *from_core = getenv(ND_ENV_KEY_EVDEV);

        if (from_core != NULL && from_core[0] != '\0')
            device = from_core;
    }
    if (device == NULL && nd_media_discover_keypad(discovered, sizeof discovered) == ND_OK)
        device = discovered;
    if (device != NULL) {
        keypad_fd = nd_media_open_keypad(device);
        if (keypad_fd < 0) {
            (void)fprintf(stderr,
                          "neodct-play: %s: cannot be opened for reading; "
                          "playback will not answer any key\n",
                          device);
            device = NULL;
        }
    }

    /* SAY WHICH, ALWAYS. This used to be printed only under --dry-run, so a
     * real run with no keypad logged nothing whatsoever and "the video cannot
     * be stopped" arrived with no evidence at all. The line goes to stderr,
     * which the Browser tags and forwards to the log. */
    if (!dry_run) {
        if (device != NULL)
            (void)fprintf(stderr, "neodct-play: keypad %s\n", device);
        else
            (void)fprintf(stderr, "neodct-play: NO KEYPAD FOUND -- nothing will answer a key, "
                                  "including C to quit. The app that started this player "
                                  "should set " ND_ENV_KEY_EVDEV " (see nd_proc.h).\n");
    }

    /* Our own socket, keypad or not: the events on it are how a failure
     * gets a name. See nd_media.h for why it is under /tmp. */
    if (nd_media_ipc_socket_path(sock, sizeof sock) != ND_OK)
        sock[0] = '\0';

    if (dry_run) {
        nd_media_argv a;
        size_t n;

        (void)printf("suspend: ");
        if (suspend_pid > 0)
            (void)printf("%ld\n", (long)suspend_pid);
        else
            (void)printf("none\n");
        (void)printf("keypad: %s\n", device != NULL ? device : "none");

        if (nd_media_build_argv(&a, url, nd_media_kind_for(url), fbdev, sock, mpv,
                                ND_MEDIA_INPUT_CONF) != ND_OK) {
            (void)fprintf(stderr, "neodct-play: command line too long\n");
            return 2;
        }
        for (n = 0u; n < a.n; n++)
            (void)printf("%s%s", n > 0u ? " " : "", a.argv[n]);
        (void)printf("\n");
        if (keypad_fd >= 0)
            (void)close(keypad_fd);
        return 0;
    }

    {
        int rc = nd_media_play(url, nd_media_kind_for(url), suspend_pid, keypad_fd, mpv, fbdev,
                               sock, ND_MEDIA_INPUT_CONF);

        /* One line on stderr, which the Browser app pumps to the serial
         * console with a tag: the same word the status bar is about to
         * show, next to the url it was about. */
        (void)fprintf(stderr, "neodct-play: %s (exit %d): %s\n", nd_media_exit_name(rc), rc,
                      url);
        if (keypad_fd >= 0)
            (void)close(keypad_fd);
        return rc;
    }
}
