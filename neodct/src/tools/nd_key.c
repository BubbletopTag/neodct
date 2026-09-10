/* nd-key -- press keys on a NeoDCT phone from a shell.
 *
 *     nd-key MENU 5 DOWN NAVIKEY      tap each in turn
 *     nd-key --hold DOWN              press and leave held
 *     nd-key --release DOWN           the matching release
 *     nd-key --list                   every name this accepts
 *     nd-key --delay 250 1 2 3        slower, for a UI that redraws
 *     nd-key --socket PATH            somewhere other than the default
 *
 * It writes "<keycode> <0|1>" datagrams to the developer key channel that
 * nd_input listens on. That channel exists only while the phone is in
 * engineering mode, which is the whole of the access control -- one switch in
 * Settings, nothing baked into the image: see devkey_open() in nd_input.c.
 *
 * ============ WHY A TAP IS TWO DATAGRAMS ============
 *
 * Because held state is real. nd_input tracks press and release to drive
 * hold-to-repeat, and a tool that only pressed would leave DOWN held and the
 * phone scrolling by itself. --hold and --release exist for the case where
 * that is what you actually want, and they are the only way to get it.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "nd_key.h"
#include "nd_keycodes.h"
#include "nd_keypadsetup.h"
#include "nd_paths.h"
#include "nd_types.h"

static void sleep_ms(unsigned int ms)
{
    struct timespec req;

    if (ms == 0u)
        return;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000u);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

static void usage(FILE *to)
{
    (void)fprintf(to,
                  "nd-key -- press keys on a NeoDCT phone\n"
                  "\n"
                  "  nd-key [options] KEY...\n"
                  "\n"
                  "  --hold          send the press only, leaving the key held\n"
                  "  --release       send the release only\n"
                  "  --delay MS      gap between keys (default %u)\n"
                  "  --socket PATH   the key channel (default %s)\n"
                  "  --list          every key name, and which are on the phone\n"
                  "\n"
                  "Keys: a name (MENU, DOWN, NAVIKEY, STAR...), a single digit\n"
                  "0-9 meaning that digit key, a multi-digit raw evdev code, or\n"
                  "raw:N to force a raw code.\n",
                  ND_KEY_DEFAULT_DELAY_MS, ND_PATH_DEVKEY_SOCK);
}

/* --list. The matrix column is the honest part: LEFT, RIGHT and MENU are
 * codes this tool will happily send and keys the phone does not have, and
 * somebody driving a test needs to know which is which before they write a
 * flow that cannot run on hardware. */
static void list_keys(void)
{
    static const char *const names[] = {
        "navikey", "enter",  "clear", "back",  "up",    "down",  "left",  "right",
        "menu",    "star",   "hash",  "num_0", "num_1", "num_2", "num_3", "num_4",
        "num_5",   "num_6",  "num_7", "num_8", "num_9", "space", "minus", "dot",
        "comma",
    };
    size_t i;

    (void)printf("%-10s %5s  %s\n", "NAME", "CODE", "WHERE");
    for (i = 0u; i < sizeof names / sizeof names[0]; i++) {
        int32_t code = nd_key_token_to_code(names[i]);

        if (code < 0)
            continue;
        (void)printf("%-10s %5d  %s\n", names[i], code,
                     nd_key_on_matrix(code) ? "on the phone's keypad" : "dev keyboard only");
    }
    (void)printf("\nA single digit 0-9 is that digit key; two or more digits is a\n"
                 "raw evdev code; raw:N forces a raw code.\n"
                 "The phone has %d keys and there is no LEFT or RIGHT.\n",
                 (int)ND_KPSETUP_N_TARGETS);
}

/* Connect to the channel, or explain why not. Returns -1 after printing. */
static int open_channel(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(path) >= sizeof addr.sun_path) {
        (void)fprintf(stderr, "nd-key: socket path too long: %s\n", path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        (void)fprintf(stderr, "nd-key: socket: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)nd_strlcpy(addr.sun_path, path, sizeof addr.sun_path);

    /* connect() on a datagram socket only fixes the peer, but it is worth
     * doing: it turns "the phone is not listening" into one error here rather
     * than a silently discarded send per key. */
    if (connect(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        if (errno == ENOENT) {
            /* The wording the brief asks for, because this is the failure
             * everybody hits first and the cause is never guessable from
             * "connection refused". */
            (void)fprintf(stderr,
                          "nd-key: no key channel at %s -- engineering mode is off, so\n"
                          "        nd_input never opened one. Turn it on in Settings and\n"
                          "        restart the phone.\n",
                          path);
        } else {
            (void)fprintf(stderr, "nd-key: connect %s: %s\n", path, strerror(errno));
        }
        (void)close(fd);
        return -1;
    }
    return fd;
}

static bool send_edge(int fd, int32_t code, bool pressed)
{
    char msg[32];
    int n;
    ssize_t w;

    n = snprintf(msg, sizeof msg, "%d %d", (int)code, pressed ? 1 : 0);
    if (n < 0 || (size_t)n >= sizeof msg)
        return false;
    w = send(fd, msg, (size_t)n, 0);
    if (w != (ssize_t)n) {
        (void)fprintf(stderr, "nd-key: send: %s\n", strerror(errno));
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    nd_key_opts opts;
    char err[160];
    char resolved[ND_PATH_MAX];
    const char *path;
    int fd;
    int i;

    /* nd_input.h:157 is explicit that anything touching these channels must
     * do this. A datagram socket will not raise SIGPIPE the way a pipe does,
     * but this tool is the obvious thing to wire into a pipeline, and dying
     * from a closed stdout mid-sequence would leave keys held. */
    (void)signal(SIGPIPE, SIG_IGN);

    if (!nd_key_parse_args(argc, argv, &opts, err, sizeof err)) {
        (void)fprintf(stderr, "nd-key: %s\n", err);
        usage(stderr);
        return 2;
    }
    if (opts.help) {
        usage(stdout);
        return 0;
    }
    if (opts.list) {
        list_keys();
        return 0;
    }

    if (opts.socket_path != NULL) {
        path = opts.socket_path;
    } else {
        if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_DEVKEY_SOCK) != ND_OK) {
            (void)fprintf(stderr, "nd-key: cannot resolve %s\n", ND_PATH_DEVKEY_SOCK);
            return 1;
        }
        path = resolved;
    }

    fd = open_channel(path);
    if (fd < 0)
        return 1;

    for (i = opts.first_key; i < argc; i++) {
        int32_t code = nd_key_token_to_code(argv[i]);

        /* Already validated by nd_key_parse_args, so this cannot fail; the
         * check is here because a silent negative would be sent as a keycode. */
        if (code < 0) {
            (void)close(fd);
            return 2;
        }

        if (opts.action == ND_KEY_ACT_TAP || opts.action == ND_KEY_ACT_HOLD) {
            if (!send_edge(fd, code, true)) {
                (void)close(fd);
                return 1;
            }
        }
        if (opts.action == ND_KEY_ACT_TAP) {
            /* A perceptible gap between the edges. Without it the UI can see
             * press and release inside one poll and a widget that distinguishes
             * a tap from a hold has nothing to measure. */
            sleep_ms(20u);
        }
        if (opts.action == ND_KEY_ACT_TAP || opts.action == ND_KEY_ACT_RELEASE) {
            if (!send_edge(fd, code, false)) {
                (void)close(fd);
                return 1;
            }
        }

        if (i + 1 < argc)
            sleep_ms(opts.delay_ms);
    }

    (void)close(fd);
    return 0;
}
