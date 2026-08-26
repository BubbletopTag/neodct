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
    if (device == NULL && nd_media_discover_keypad(discovered, sizeof discovered) == ND_OK)
        device = discovered;
    if (device != NULL) {
        keypad_fd = nd_media_open_keypad(device);
        if (keypad_fd < 0)
            device = NULL;
    }

    if (dry_run) {
        nd_media_argv a;
        size_t n;

        (void)printf("suspend: ");
        if (suspend_pid > 0)
            (void)printf("%ld\n", (long)suspend_pid);
        else
            (void)printf("none\n");
        (void)printf("keypad: %s\n", device != NULL ? device : "none");

        if (nd_media_build_argv(&a, url, nd_media_kind_for(url), fbdev,
                                keypad_fd >= 0 ? ND_MEDIA_IPC_SOCKET : NULL, mpv,
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
                               ND_MEDIA_IPC_SOCKET, ND_MEDIA_INPUT_CONF);

        if (keypad_fd >= 0)
            (void)close(keypad_fd);
        return rc;
    }
}
