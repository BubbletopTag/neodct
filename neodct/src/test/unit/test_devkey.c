/* test_devkey.c -- the developer key channel in nd_input.
 *
 * The channel exists so a phone can be driven from outside the UI: nd-key
 * writes "<keycode> <0|1>" to a datagram socket and the key comes out of
 * nd_input_read_key() as though it had been pressed. That makes it the one
 * piece of ndlink that can be proved without any hardware at all, which is
 * why the cases below are the acceptance for it.
 *
 * ============ WHY THE GATE IS THE FIRST CASE ============
 *
 * What is behind this socket is the ability to press keys on somebody's
 * phone. The gate is ENGINEERING MODE, the switch in Settings, and nothing
 * else -- it used to be /etc/neodct-devenv, a marker only a rebuild could
 * place, which meant a phone sitting in engineering mode still refused to be
 * driven and told its owner to go and rebuild the image. That property is
 * worth more than the feature, so it is asserted first and from both
 * directions: engineering mode off means no socket at all, not merely a
 * socket that refuses.
 *
 * These tests are all against a scratch ND_ROOT (pt_new_case()), so the
 * settings file and the socket are files under /tmp and nothing touches the
 * real /NeoDCT or /run.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/* Write the engineering-mode switch into the scratch root's settings file.
 *
 * Spelled out rather than relying on the default, in BOTH directions. The
 * default happens to be ON (ND_SET_UI_ENG_MODE_DFLT), so a case that wanted
 * the channel closed and simply wrote no settings file would get an open one
 * and pass for the wrong reason. */
static void given_engineering_mode(bool on)
{
    pt_mkdir("/NeoDCT/User");
    pt_write_text(ND_PATH_SETTINGS_PROP,
                  on ? "system.ui.engineering_mode=ON\n" : "system.ui.engineering_mode=OFF\n");
}

/* What every case that wants a working channel opens with. */
static void give_marker(void)
{
    given_engineering_mode(true);
}

/* The resolved socket path inside the scratch root. */
static void sock_path(char *out, size_t out_sz)
{
    CHECK(nd_path_resolve(out, out_sz, ND_PATH_DEVKEY_SOCK) == ND_OK);
}

/* Send one raw datagram; returns false when the socket is not there. Raw
 * rather than via nd-key so the malformed cases can say things nd-key would
 * never say. */
static bool send_raw(const void *msg, size_t len)
{
    struct sockaddr_un addr;
    char path[ND_PATH_MAX];
    int fd;
    ssize_t n;

    sock_path(path, sizeof path);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)nd_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    n = sendto(fd, msg, len, 0, (const struct sockaddr *)&addr, sizeof addr);
    (void)close(fd);
    return n == (ssize_t)len;
}

static bool send_text(const char *msg)
{
    return send_raw(msg, strlen(msg));
}

/* ------------------------------------------------------------------ *
 * The gate
 * ------------------------------------------------------------------ */

static void test_with_engineering_mode_off_there_is_no_socket(void)
{
    nd_input *in = NULL;
    char path[ND_PATH_MAX];
    struct stat st;

    given_engineering_mode(false); /* explicitly OFF -- that is the whole case */
    CHECK(nd_input_open(&in) == ND_OK);
    CHECK(in != NULL);

    CHECK(!nd_input_devkey_active(in));

    /* Not merely inactive: nothing was created. A socket that exists but is
     * ignored would still be a thing an attacker could find and probe. */
    sock_path(path, sizeof path);
    CHECK(stat(path, &st) != 0);

    /* And a sender gets nowhere. */
    CHECK(!send_text("50 1"));

    nd_input_close(in);
}

static void test_with_engineering_mode_on_the_socket_exists_and_is_private(void)
{
    nd_input *in = NULL;
    char path[ND_PATH_MAX];
    struct stat st;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);
    CHECK(nd_input_devkey_active(in));

    sock_path(path, sizeof path);
    CHECK(stat(path, &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    /* 0600: a way to press keys on somebody's phone is not group-readable. */
    CHECK_INT(st.st_mode & 0777, 0600);

    nd_input_close(in);

    /* Closing takes the socket file with it, or the next boot's bind() fails
     * with EADDRINUSE on an inode nothing is listening to. */
    CHECK(stat(path, &st) != 0);
}

/* ------------------------------------------------------------------ *
 * The channel actually delivers
 * ------------------------------------------------------------------ */

static void test_a_datagram_comes_out_of_read_key(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);
    CHECK(nd_input_devkey_active(in));

    CHECK(send_text("50 1")); /* MENU pressed */
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);

    nd_input_close(in);
}

static void test_press_and_release_leaves_nothing_held(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    CHECK(send_text("50 1"));
    CHECK(send_text("50 0"));

    /* read_key returns presses only; the release is consumed for held state. */
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);
    /* Drain the release so held state settles. */
    (void)nd_input_read_key(in, 0.2);

    CHECK(!nd_input_is_held(in, ND_KEY_MENU));
    nd_input_close(in);
}

static void test_a_press_alone_leaves_the_key_held(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    /* This is why nd-key sends both edges by default: a sender that only
     * presses leaves real held state behind, and widgets read it. */
    CHECK(send_text("50 1"));
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);
    CHECK(nd_input_is_held(in, ND_KEY_MENU));

    nd_input_close(in);
}

static void test_ordering_is_preserved_across_several_keys(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    CHECK(send_text("2 1"));  /* 1 */
    CHECK(send_text("2 0"));
    CHECK(send_text("3 1"));  /* 2 */
    CHECK(send_text("3 0"));
    CHECK(send_text("50 1")); /* MENU */
    CHECK(send_text("50 0"));

    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_1);
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_2);
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);

    nd_input_close(in);
}

/* ------------------------------------------------------------------ *
 * What the outside world may send is checked, not guessed at
 * ------------------------------------------------------------------ */

static void test_malformed_datagrams_are_dropped(void)
{
    nd_input *in = NULL;
    static const char *const rubbish[] = {
        "",             /* empty                                  */
        "50",           /* no edge                                */
        "50 2",         /* an edge that is neither press nor release */
        "50 1 extra",   /* trailing junk                          */
        "abc 1",        /* not a number                           */
        "50x 1",        /* number with a tail                     */
        " 50 1",        /* leading space -- strtol would accept it, we do not
                         * want a format with optional shapes     */
        "-1 1",         /* the ND_KEY_NONE sentinel must not arrive from outside */
        "-2 1",         /* nor ND_KEY_INCOMING_CALL               */
        "0 1",          /* not an evdev code                      */
        "70000 1",      /* beyond 16 bits                         */
    };
    size_t i;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    for (i = 0u; i < sizeof rubbish / sizeof rubbish[0]; i++) {
        if (rubbish[i][0] != '\0')
            CHECK(send_text(rubbish[i]));
    }

    /* Nothing arrived, and just as importantly the queue still works
     * afterwards: a bad datagram must not wedge the channel. */
    CHECK_INT(nd_input_read_key(in, 0.2), ND_KEY_NONE);
    CHECK(send_text("50 1"));
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);

    nd_input_close(in);
}

static void test_an_oversized_datagram_is_dropped(void)
{
    nd_input *in = NULL;
    char big[512];

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    memset(big, '7', sizeof big);
    CHECK(send_raw(big, sizeof big));

    CHECK_INT(nd_input_read_key(in, 0.2), ND_KEY_NONE);

    /* Still usable. recv() truncates the oversized datagram to the buffer and
     * the parser rejects what is left; the socket is not left out of step. */
    CHECK(send_text("50 1"));
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);

    nd_input_close(in);
}

static void test_a_trailing_newline_is_accepted(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    /* Because writing one from a shell is the obvious thing to do:
     *     printf '50 1\n' | socat - UNIX-SENDTO:/run/neodct/devkey
     * Refusing it would make the channel needlessly awkward by hand. */
    CHECK(send_text("50 1\n"));
    CHECK_INT(nd_input_read_key(in, 1.0), ND_KEY_MENU);

    nd_input_close(in);
}

/* ------------------------------------------------------------------ *
 * It does not lie about the hardware
 * ------------------------------------------------------------------ */

static void test_the_channel_is_not_a_backend(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);
    CHECK(nd_input_devkey_active(in));

    /* The scratch root has no keymap.json and no /dev/input, so there is no
     * matrix and no evdev device. The channel must not paper over that: a
     * phone with a dead keypad has to keep saying so, and the T9 indicator
     * must stay off for keys that are being faked. */
    CHECK(!nd_input_has_matrix(in));
    CHECK(nd_input_which(in) != ND_INPUT_MATRIX);

    nd_input_close(in);
}

static void test_reads_still_time_out_normally(void)
{
    nd_input *in = NULL;

    give_marker();
    CHECK(nd_input_open(&in) == ND_OK);

    /* An open channel must not change what a timeout means. A caller should
     * not be able to tell from timing whether the channel exists. */
    CHECK_INT(nd_input_read_key(in, 0.0), ND_KEY_NONE);
    CHECK_INT(nd_input_read_key(in, 0.05), ND_KEY_NONE);

    nd_input_close(in);
}

int main(void)
{
    RUN(test_with_engineering_mode_off_there_is_no_socket);
    RUN(test_with_engineering_mode_on_the_socket_exists_and_is_private);
    RUN(test_a_datagram_comes_out_of_read_key);
    RUN(test_press_and_release_leaves_nothing_held);
    RUN(test_a_press_alone_leaves_the_key_held);
    RUN(test_ordering_is_preserved_across_several_keys);
    RUN(test_malformed_datagrams_are_dropped);
    RUN(test_an_oversized_datagram_is_dropped);
    RUN(test_a_trailing_newline_is_accepted);
    RUN(test_the_channel_is_not_a_backend);
    RUN(test_reads_still_time_out_normally);
    return pt_report("test_devkey");
}
