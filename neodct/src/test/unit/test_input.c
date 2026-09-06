/* test_input.c -- the composed input facade: evdev decoding, held state, key
 * auto-repeat, the app channel and device discovery.
 *
 * None of this had a test before. The parts that matter most are the ones the
 * Python could not express at all:
 *
 *   - RELEASES. The Python's read_keypress() reports presses only, which is
 *     why Koki went behind the driver's back. Here they are first-class.
 *   - HELD STATE derived from the press/release stream, for every app.
 *   - AUTO-REPEAT, and specifically that it fires for the arrows and NOT for
 *     the digits -- a repeat on a digit would cycle T9 multi-tap letters
 *     behind the user's back.
 *
 * A pipe stands in for the input device. That is not a shortcut: a pipe and
 * an evdev character device deliver the same bytes, and the app channel this
 * module also implements IS a pipe.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"

#include "platform_test.h"

#define EV_KEY_T 0x01
#define EV_SYN_T 0x00

/* struct input_event as this build's kernel would deliver it. */
typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} ev_native;

static void write_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    ev_native ev;

    memset(&ev, 0, sizeof ev);
    ev.tv_sec = 1;
    ev.tv_usec = 2;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    CHECK_INT(write(fd, &ev, sizeof ev), (int)sizeof ev);
}

static void write_key(int fd, uint16_t code, int32_t value)
{
    write_event(fd, EV_KEY_T, code, value);
    write_event(fd, EV_SYN_T, 0u, 0);
}

/* The 32-bit ARM layout, which is 16 bytes because its timeval is two 32-bit
 * words. The phone speaks this one; the test host speaks the other. */
static void write_key_16byte(int fd, uint16_t code, int32_t value)
{
    uint8_t buf[16];

    memset(buf, 0, sizeof buf);
    buf[0] = 7; /* tv_sec, little end first -- endianness is the host's */
    memcpy(buf + 8, &code, 0u);
    buf[8] = (uint8_t)(EV_KEY_T & 0xFF);
    buf[9] = 0u;
    buf[10] = (uint8_t)(code & 0xFFu);
    buf[11] = (uint8_t)((code >> 8) & 0xFFu);
    buf[12] = (uint8_t)((uint32_t)value & 0xFFu);
    buf[13] = (uint8_t)(((uint32_t)value >> 8) & 0xFFu);
    buf[14] = (uint8_t)(((uint32_t)value >> 16) & 0xFFu);
    buf[15] = (uint8_t)(((uint32_t)value >> 24) & 0xFFu);
    CHECK_INT(write(fd, buf, sizeof buf), (int)sizeof buf);
}

/* A pipe wearing an nd_input. Returns the write end. */
static int open_over_pipe(nd_input **in)
{
    int fds[2];

    CHECK_INT(pipe(fds), 0);
    CHECK_INT(nd_input_open_fd(in, fds[0]), ND_OK);
    return fds[1];
}

/* ------------------------------------------------------------------ *
 * Decoding
 * ------------------------------------------------------------------ */

static void test_a_press_and_a_release_both_arrive(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;

    write_key(w, ND_KEY_DOWN, 1);
    write_key(w, ND_KEY_DOWN, 0);

    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK_INT(ev.code, ND_KEY_DOWN);
    CHECK(ev.pressed);

    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK_INT(ev.code, ND_KEY_DOWN);
    CHECK(!ev.pressed);

    (void)close(w);
    nd_input_close(in);
}

static void test_read_key_returns_presses_only(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    /* ui.read_keypress() is what every existing screen is written against,
     * and it has never seen a release. */
    write_key(w, ND_KEY_ENTER, 1);
    write_key(w, ND_KEY_ENTER, 0);
    write_key(w, ND_KEY_UP, 1);

    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_ENTER);
    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_UP);

    (void)close(w);
    nd_input_close(in);
}

static void test_kernel_autorepeat_is_ignored(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    /* Value 2 is the kernel's own autorepeat. The Python dropped it and so
     * does this: the core makes its own, so the i2c keypad -- which has no
     * kernel to repeat for it -- behaves the same as a USB keyboard. */
    write_key(w, ND_KEY_UP, 1);
    write_key(w, ND_KEY_UP, 2);
    write_key(w, ND_KEY_UP, 2);
    write_key(w, ND_KEY_DOWN, 1);

    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_UP);
    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_DOWN);

    (void)close(w);
    nd_input_close(in);
}

static void test_the_16_byte_layout_decodes_too(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    /* struct input_event is 24 bytes on this host and 16 on the phone. The
     * reader has to handle both, because the layout belongs to the kernel
     * that made the device, not to the program reading it. */
    write_key_16byte(w, ND_KEY_LEFT, 1);
    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_LEFT);

    (void)close(w);
    nd_input_close(in);
}

static void test_a_non_key_event_is_not_a_key(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    write_event(w, 0x03u, 0u, 55); /* EV_ABS */
    write_event(w, EV_SYN_T, 0u, 0);
    CHECK_INT(nd_input_read_key(in, 0.05), ND_KEY_NONE);

    (void)close(w);
    nd_input_close(in);
}

static void test_a_timeout_returns_nothing(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    CHECK_INT(nd_input_read_key(in, 0.02), ND_KEY_NONE);

    (void)close(w);
    nd_input_close(in);
}

/* ------------------------------------------------------------------ *
 * Held state
 * ------------------------------------------------------------------ */

static void test_held_state_follows_the_stream(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    int32_t held[4];

    /* This is the whole answer to OPEN-QUESTIONS.md question 2: an app in its
     * own process derives held state from the events it already receives, and
     * never touches the i2c bus. */
    CHECK(!nd_input_is_held(in, ND_KEY_RIGHT));

    write_key(w, ND_KEY_RIGHT, 1);
    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_RIGHT);
    CHECK(nd_input_is_held(in, ND_KEY_RIGHT));
    CHECK_INT(nd_input_held(in, held, 4u), 1);
    CHECK_INT(held[0], ND_KEY_RIGHT);

    write_key(w, ND_KEY_RIGHT, 0);
    /* read_key consumes the release without returning it. */
    CHECK_INT(nd_input_read_key(in, 0.05), ND_KEY_NONE);
    CHECK(!nd_input_is_held(in, ND_KEY_RIGHT));
    CHECK_INT(nd_input_held(in, held, 4u), 0);

    (void)close(w);
    nd_input_close(in);
}

static void test_drain_discards_events_but_keeps_held_honest(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);

    /* The Browser drains before handing the screen over. A release that
     * happened during the drain must still land, or the key is stuck down
     * for the rest of the session. */
    write_key(w, ND_KEY_UP, 1);
    write_key(w, ND_KEY_UP, 0);
    write_key(w, ND_KEY_DOWN, 1);

    nd_input_drain(in);
    CHECK(!nd_input_is_held(in, ND_KEY_UP));
    CHECK(nd_input_is_held(in, ND_KEY_DOWN));
    CHECK_INT(nd_input_read_key(in, 0.02), ND_KEY_NONE);

    (void)close(w);
    nd_input_close(in);
}

/* ------------------------------------------------------------------ *
 * Auto-repeat
 * ------------------------------------------------------------------ */

static void test_a_held_arrow_repeats(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;

    /* The delay and interval are shortened so the test does not sit for two
     * seconds; the DEFAULTS are checked separately below. */
    nd_input_set_repeat(in, 0.02, 0.01);

    write_key(w, ND_KEY_DOWN, 1);
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(ev.pressed);
    CHECK(!nd_input_last_was_repeat(in));

    /* No further bytes on the pipe: everything from here is synthesised. */
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK_INT(ev.code, ND_KEY_DOWN);
    CHECK(ev.pressed);
    CHECK(nd_input_last_was_repeat(in));

    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK_INT(ev.code, ND_KEY_DOWN);
    CHECK(nd_input_last_was_repeat(in));

    /* read_key sees them too -- that is what gives VerticalList and PagedList
     * hold-to-scroll for nothing. */
    CHECK_INT(nd_input_read_key(in, 0.5), ND_KEY_DOWN);

    (void)close(w);
    nd_input_close(in);
}

static void test_a_release_stops_the_repeat(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;
    int guard;
    bool saw_release = false;

    nd_input_set_repeat(in, 0.02, 0.01);

    write_key(w, ND_KEY_UP, 1);
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(nd_input_read_event(in, 0.5, &ev)); /* at least one repeat */

    write_key(w, ND_KEY_UP, 0);
    for (guard = 0; guard < 200 && !saw_release; guard++) {
        if (!nd_input_read_event(in, 0.5, &ev))
            break;
        if (!ev.pressed)
            saw_release = true;
    }
    CHECK(saw_release);
    CHECK(!nd_input_is_held(in, ND_KEY_UP));
    /* Nothing more is synthesised once the key is up. */
    CHECK(!nd_input_read_event(in, 0.05, &ev));

    (void)close(w);
    nd_input_close(in);
}

static void test_a_digit_does_not_repeat(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;

    /* THE reason the default set is the four arrows and nothing else: a
     * repeat on key 2 would walk the T9 multi-tap cycle a -> b -> c while the
     * user was still deciding whether to lift their finger. */
    nd_input_set_repeat(in, 0.02, 0.01);

    write_key(w, ND_KEY_2, 1);
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(ev.pressed);
    CHECK(nd_input_is_held(in, ND_KEY_2));
    CHECK(!nd_input_read_event(in, 0.15, &ev));

    (void)close(w);
    nd_input_close(in);
}

static void test_the_repeat_set_can_be_widened_and_disabled(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;
    int32_t codes[1];

    nd_input_set_repeat(in, 0.02, 0.01);
    codes[0] = ND_KEY_2;
    CHECK_INT(nd_input_set_repeat_codes(in, codes, 1u), ND_OK);

    write_key(w, ND_KEY_2, 1);
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(nd_input_last_was_repeat(in));

    /* Narrowing the set must disarm a key that is already down, not leave it
     * repeating until somebody lets go. */
    CHECK_INT(nd_input_set_repeat_codes(in, codes, 0u), ND_OK);
    CHECK(!nd_input_read_event(in, 0.1, &ev));

    /* NULL restores the four arrows. */
    CHECK_INT(nd_input_set_repeat_codes(in, NULL, 0u), ND_OK);
    CHECK_INT(nd_input_set_repeat_codes(in, codes, ND_INPUT_HELD_MAX + 1u), ND_ERR_TOOLONG);

    nd_input_set_repeat(in, 0.0, 0.0);
    write_key(w, ND_KEY_DOWN, 1);
    CHECK(nd_input_read_event(in, 0.5, &ev));
    CHECK(!nd_input_read_event(in, 0.1, &ev));

    (void)close(w);
    nd_input_close(in);
}

static void test_the_defaults_are_the_documented_ones(void)
{
    nd_input *in = NULL;
    int w = open_over_pipe(&in);
    nd_key_event ev;
    struct timespec t0;
    struct timespec t1;
    double elapsed;

    /* 400 ms then 120 ms. The delay is long enough that a deliberate press
     * (150-250 ms of contact) never repeats, and short of the 1.0 s T9
     * multi-tap window; the interval sits just above the UI's 100 ms poll
     * tick, so a repeat is always observable and never floods. */
    CHECK(ND_REPEAT_DELAY_S > 0.35 && ND_REPEAT_DELAY_S < 1.0);
    CHECK(ND_REPEAT_INTERVAL_S > 0.1 && ND_REPEAT_INTERVAL_S < 0.25);

    write_key(w, ND_KEY_LEFT, 1);
    CHECK(nd_input_read_event(in, 1.0, &ev));
    CHECK(!nd_input_last_was_repeat(in));

    /* Nothing repeats before the delay is up. */
    CHECK(!nd_input_read_event(in, ND_REPEAT_DELAY_S / 2.0, &ev));

    CHECK_INT(clock_gettime(CLOCK_MONOTONIC, &t0), 0);
    CHECK(nd_input_read_event(in, 2.0, &ev));
    CHECK_INT(clock_gettime(CLOCK_MONOTONIC, &t1), 0);
    CHECK(nd_input_last_was_repeat(in));
    elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    /* Half the delay has already gone by, so the rest of it is what remains. */
    CHECK(elapsed < ND_REPEAT_DELAY_S);

    (void)close(w);
    nd_input_close(in);
}

/* ------------------------------------------------------------------ *
 * The cross-process channel
 * ------------------------------------------------------------------ */

static void test_the_app_channel_carries_presses_and_releases(void)
{
    nd_input_channel ch;
    nd_input *child = NULL;
    nd_key_event ev;

    /* This is the settled answer to question 2 in one test: the core writes
     * evdev records onto a pipe, and the child reads them with exactly the
     * code it would use for a real device. */
    CHECK_INT(nd_input_channel_open(&ch), ND_OK);
    CHECK(ch.read_fd >= 0);
    CHECK(ch.write_fd >= 0);

    CHECK_INT(nd_input_channel_send(&ch, ND_KEY_ENTER, true), ND_OK);
    CHECK_INT(nd_input_channel_send(&ch, ND_KEY_ENTER, false), ND_OK);

    CHECK_INT(nd_input_open_pipe(&child, ch.read_fd), ND_OK);
    CHECK_INT(nd_input_which(child), ND_INPUT_PIPE);
    CHECK(!nd_input_has_matrix(child));

    CHECK(nd_input_read_event(child, 0.5, &ev));
    CHECK_INT(ev.code, ND_KEY_ENTER);
    CHECK(ev.pressed);
    CHECK(nd_input_is_held(child, ND_KEY_ENTER));

    CHECK(nd_input_read_event(child, 0.5, &ev));
    CHECK(!ev.pressed);
    CHECK(!nd_input_is_held(child, ND_KEY_ENTER));

    /* nd_input_open_pipe took ownership of read_fd. */
    ch.read_fd = -1;
    nd_input_close(child);
    nd_input_channel_close(&ch);
}

static void test_a_dead_child_is_an_io_error_not_a_hang(void)
{
    nd_input_channel ch;
    nd_err rc = ND_OK;
    int i;

    /* The write end is non-blocking on purpose: a child that stops reading,
     * because it is wedged or mid-crash, must not be able to wedge the core
     * along with it. */
    CHECK_INT(nd_input_channel_open(&ch), ND_OK);
    nd_input_channel_close_read(&ch);

    /* SIGPIPE would kill the test process before write() could report EPIPE. */
    (void)signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < 100000 && rc == ND_OK; i++)
        rc = nd_input_channel_send(&ch, ND_KEY_UP, true);
    CHECK_INT(rc, ND_ERR_IO);

    nd_input_channel_close(&ch);
    /* Closing an already-closed channel is safe. */
    nd_input_channel_close(&ch);
}

/* ------------------------------------------------------------------ *
 * Device discovery
 * ------------------------------------------------------------------ */

static void make_dev_file(const char *vpath)
{
    pt_write_text(vpath, "");
}

static void test_discovery_falls_back_to_event0(void)
{
    char path[ND_PATH_MAX];

    make_dev_file("/dev/input/event0");
    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_OK);
    CHECK_STR(path, "/dev/input/event0");
}

static void test_a_by_path_kbd_symlink_wins_over_event0(void)
{
    char path[ND_PATH_MAX];
    char target[ND_PATH_MAX];
    char link[ND_PATH_MAX];

    /* Priority order: by-path "-kbd" links, then by-id ones, then event0. On a
     * machine with a touchscreen AND a keyboard, event0 is usually the
     * touchscreen -- which is why the -kbd links come first. */
    make_dev_file("/dev/input/event0");
    make_dev_file("/dev/input/event1");
    pt_mkdir("/dev/input/by-path");
    CHECK_INT(nd_path_resolve(target, sizeof target, "/dev/input/event1"), ND_OK);
    CHECK_INT(nd_path_resolve(link, sizeof link, "/dev/input/by-path/pci-0000-usb-kbd"), ND_OK);
    CHECK_INT(symlink(target, link), 0);

    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_OK);
    CHECK_STR(path, "/dev/input/event1");
}

static void test_the_environment_override_wins_when_it_exists(void)
{
    char path[ND_PATH_MAX];

    make_dev_file("/dev/input/event0");
    make_dev_file("/dev/input/event9");
    CHECK_INT(setenv(ND_ENV_KEYPAD_DEVICE, "/dev/input/event9", 1), 0);
    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_OK);
    CHECK_STR(path, "/dev/input/event9");

    /* Set but missing: the Python logs and carries on down the list rather
     * than failing, because a stale export must not brick input. */
    CHECK_INT(setenv(ND_ENV_KEYPAD_DEVICE, "/dev/input/nope", 1), 0);
    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_OK);
    CHECK_STR(path, "/dev/input/event0");

    CHECK_INT(unsetenv(ND_ENV_KEYPAD_DEVICE), 0);
}

static void test_with_no_devices_at_all_the_legacy_path_is_returned(void)
{
    char path[ND_PATH_MAX];

    /* Returning the legacy path rather than failing is deliberate: the
     * caller's open() then produces the one message that says what is wrong. */
    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_ERR_NOTFOUND);
    CHECK_STR(path, "/dev/input/event0");
}

static void test_the_lowest_numbered_event_device_is_the_fallback(void)
{
    char path[ND_PATH_MAX];

    /* sorted() is code-point order, so event10 sorts before event2. Matching
     * that exactly is why the C sorts with strcmp and not strcoll. */
    make_dev_file("/dev/input/event10");
    make_dev_file("/dev/input/event2");
    CHECK_INT(nd_evdev_discover(path, sizeof path), ND_OK);
    CHECK_STR(path, "/dev/input/event10");
}

static void test_opening_a_missing_device_fails_cleanly(void)
{
    CHECK_INT(nd_evdev_open("/dev/input/absent"), -1);
    CHECK_INT(nd_evdev_open(NULL), -1);
    CHECK_INT(nd_evdev_read_key(-1, 0.0), ND_KEY_NONE);
}

static void test_a_device_name_that_cannot_be_read_is_empty(void)
{
    char name[64];

    /* nd_input logs the literal word "unknown" in that case, which is what
     * the Python printed; the function itself reports emptiness. */
    make_dev_file("/dev/input/event0");
    (void)nd_evdev_device_name("/dev/input/event0", name, sizeof name);
    CHECK_STR(name, "");
}

static void test_the_sysfs_name_is_read_when_the_ioctl_cannot_be(void)
{
    char name[64];

    /* /sys/class/input/<eventN>/device/name is the Python's path and the one
     * a host test can fake, so it is kept as the fallback. */
    make_dev_file("/dev/input/event3");
    pt_write_text("/sys/class/input/event3/device/name", "NeoDCT Matrix Keypad\n");
    CHECK_INT(nd_evdev_device_name("/dev/input/event3", name, sizeof name), ND_OK);
    CHECK_STR(name, "NeoDCT Matrix Keypad");
}

/* ------------------------------------------------------------------ *
 * Bringing the keypad up, and the decisions behind it
 * ------------------------------------------------------------------ */

/* A keymap the loader will accept, on the bus and with the driver the caller
 * names. Written by hand rather than through nd_keymap_save(), because a
 * fixture built with the code under test cannot catch that code being wrong. */
static void write_keymap(const char *driver, int bus)
{
    char json[512];

    CHECK(nd_snprintf(json, sizeof json,
                      "{\n"
                      "  \"col_pins\": [4, 5, 6, 7],\n"
                      "  \"driver\": \"%s\",\n"
                      "  \"format\": \"neodct.keymap.v3.matrix.i2c\",\n"
                      "  \"i2c_addr\": 32,\n"
                      "  \"i2c_bus\": %d,\n"
                      "  \"keys\": {\n"
                      "    \"navikey\": {\"col\": 0, \"row\": 0},\n"
                      "    \"num_1\": {\"col\": 1, \"row\": 0}\n"
                      "  },\n"
                      "  \"row_pins\": [0, 1, 2, 3]\n"
                      "}\n",
                      driver, bus) == ND_OK);
    pt_write_text(ND_PATH_KEYMAP, json);
}

static void test_transient_errnos_are_the_ones_a_cold_boot_makes(void)
{
    /* THE POLICY THIS WHOLE GROUP OF BUGS TURNS ON. Every one of these is
     * something a Luckfox produces in the first two seconds of userspace and
     * stops producing shortly afterwards, and every one of them used to be a
     * permanent "this phone has no keypad" shown to the owner of a working
     * phone. */
    CHECK(nd_input_errno_is_transient(EACCES));    /* udev has not applied the group */
    CHECK(nd_input_errno_is_transient(EPERM));     /* the same, other spelling       */
    CHECK(nd_input_errno_is_transient(ENOENT));    /* i2c-dev registering late       */
    CHECK(nd_input_errno_is_transient(ENODEV));
    CHECK(nd_input_errno_is_transient(ENXIO));     /* nobody answered -- yet         */
    CHECK(nd_input_errno_is_transient(EREMOTEIO)); /* a NAK on a rising rail         */
    CHECK(nd_input_errno_is_transient(EAGAIN));    /* rk3x arbitration               */
    CHECK(nd_input_errno_is_transient(EBUSY));
    CHECK(nd_input_errno_is_transient(EIO));
    CHECK(nd_input_errno_is_transient(ETIMEDOUT));
    CHECK(nd_input_errno_is_transient(EINTR));

    /* And these are not: they say the REQUEST was wrong, and a request that
     * is wrong now is wrong in thirty seconds. Retrying them is thirty log
     * lines and thirty seconds spent proving something already known. */
    CHECK(!nd_input_errno_is_transient(EBADF));
    CHECK(!nd_input_errno_is_transient(ENOTTY));
    CHECK(!nd_input_errno_is_transient(EINVAL));
    CHECK(!nd_input_errno_is_transient(0));
}

static void test_a_keymap_that_cannot_be_right_is_never_retried(void)
{
    /* validate_pins() rejecting a file is not a race and cannot become one. */
    CHECK_INT(nd_input_classify_open_failure(ND_ERR_INVAL, EACCES), ND_INPUT_FAIL_PERMANENT);
    CHECK_INT(nd_input_classify_open_failure(ND_ERR_TOOLONG, 0), ND_INPUT_FAIL_PERMANENT);

    /* Everything else is judged on the errno underneath it. */
    CHECK_INT(nd_input_classify_open_failure(ND_ERR_IO, EACCES), ND_INPUT_FAIL_TRANSIENT);
    CHECK_INT(nd_input_classify_open_failure(ND_ERR_HARDWARE, EREMOTEIO), ND_INPUT_FAIL_TRANSIENT);
    CHECK_INT(nd_input_classify_open_failure(ND_ERR_IO, ENOTTY), ND_INPUT_FAIL_PERMANENT);

    CHECK_INT(nd_input_classify_open_failure(ND_OK, 0), ND_INPUT_FAIL_NONE);
}

static void test_a_backend_can_be_recovered_without_reading_a_key(void)
{
    nd_input *in = NULL;

    /* ============ THE HEADLINE BUG, IN ONE CASE ============
     *
     * 0.5.7b added a retry for the boot race and put its only call site
     * inside nd_input_read_event(). A core with NO backend never reaches its
     * read loop -- core_run() decides what to draw first, and on a Luckfox
     * there is no spare evdev keyboard to make that decision come out right
     * -- so the recovery written for this exact race could not execute even
     * once on the only hardware that needed it. Three releases of fixes did
     * not fix the keypad error screen for precisely this reason.
     *
     * The device appearing between the open and the retry below is the race,
     * played out in the order the phone plays it. Nothing here reads a key. */
    CHECK_INT(nd_input_open(&in), ND_OK);
    CHECK(in != NULL);
    CHECK(!nd_input_has_backend(in));
    CHECK(nd_input_no_backend_reason(in)[0] != '\0');

    make_dev_file("/dev/input/event0");

    CHECK(nd_input_retry_backend(in));
    CHECK(nd_input_has_backend(in));
    /* And the phone stops telling the owner it has no keys. */
    CHECK_STR(nd_input_no_backend_reason(in), "");

    nd_input_close(in);
}

static void test_retrying_a_phone_that_really_has_nothing_is_still_false(void)
{
    nd_input *in = NULL;

    CHECK_INT(nd_input_open(&in), ND_OK);
    CHECK(!nd_input_retry_backend(in));
    CHECK(!nd_input_has_backend(in));
    /* Never NULL, and never empty while there is no backend: this string is
     * what the core puts on the panel. */
    CHECK(nd_input_no_backend_reason(in)[0] != '\0');
    nd_input_close(in);
}

static void test_an_i2c_bus_that_has_not_appeared_says_so_and_not_that_it_is_absent(void)
{
    nd_input *in = NULL;

    /* Wording, and it matters: a node that has not registered yet is a "not
     * yet", and the message must not read as a verdict. The bus number is
     * named because the first thing anybody does with this message is go and
     * look at that node. */
    write_keymap("pcf8575-i2c", 9);
    CHECK_INT(nd_input_open(&in), ND_OK);
    CHECK(!nd_input_has_backend(in));
    CHECK(strstr(nd_input_no_backend_reason(in), "/dev/i2c-9") != NULL);
    nd_input_close(in);
}

static void test_a_failed_open_names_the_syscall_and_the_errno(void)
{
    nd_input *in = NULL;
    const char *why;

    /* THE MESSAGE THIS REPLACES: "The keypad on /dev/i2c-3 did not open (a
     * permission or wiring problem)", printed for all five distinct causes --
     * two of which are neither permission nor wiring, and the two it does
     * name having completely different repairs.
     *
     * A regular file standing in for the node opens perfectly and then fails
     * the I2C_SLAVE ioctl, which is exactly the shape of a bus that is not
     * the bus the keymap thinks it is. */
    write_keymap("pcf8575-i2c", 9);
    pt_write_text("/dev/i2c-9", "");
    CHECK_INT(nd_input_open(&in), ND_OK);
    CHECK(!nd_input_has_backend(in));
    why = nd_input_no_backend_reason(in);
    CHECK(strstr(why, "/dev/i2c-9") != NULL);
    CHECK(strstr(why, "I2C_SLAVE") != NULL);
    nd_input_close(in);
}

static void test_an_unsupported_driver_is_named_in_the_reason(void)
{
    nd_input *in = NULL;

    /* The one genuinely permanent verdict in the whole open path: no amount
     * of waiting turns a keymap that names another driver into one that names
     * this one. The owner is told which driver, because the only repair is to
     * re-run the wizard. */
    write_keymap("gpiozero-matrix", 3);
    CHECK_INT(nd_input_open(&in), ND_OK);
    CHECK(!nd_input_has_backend(in));
    CHECK(strstr(nd_input_no_backend_reason(in), "gpiozero-matrix") != NULL);
    /* Retrying it changes nothing and must not pretend otherwise. */
    CHECK(!nd_input_retry_backend(in));
    nd_input_close(in);
}

int main(void)
{
    RUN(test_a_press_and_a_release_both_arrive);
    RUN(test_read_key_returns_presses_only);
    RUN(test_kernel_autorepeat_is_ignored);
    RUN(test_the_16_byte_layout_decodes_too);
    RUN(test_a_non_key_event_is_not_a_key);
    RUN(test_a_timeout_returns_nothing);
    RUN(test_held_state_follows_the_stream);
    RUN(test_drain_discards_events_but_keeps_held_honest);
    RUN(test_a_held_arrow_repeats);
    RUN(test_a_release_stops_the_repeat);
    RUN(test_a_digit_does_not_repeat);
    RUN(test_the_repeat_set_can_be_widened_and_disabled);
    RUN(test_the_defaults_are_the_documented_ones);
    RUN(test_the_app_channel_carries_presses_and_releases);
    RUN(test_a_dead_child_is_an_io_error_not_a_hang);
    RUN(test_discovery_falls_back_to_event0);
    RUN(test_a_by_path_kbd_symlink_wins_over_event0);
    RUN(test_the_environment_override_wins_when_it_exists);
    RUN(test_with_no_devices_at_all_the_legacy_path_is_returned);
    RUN(test_the_lowest_numbered_event_device_is_the_fallback);
    RUN(test_opening_a_missing_device_fails_cleanly);
    RUN(test_a_device_name_that_cannot_be_read_is_empty);
    RUN(test_the_sysfs_name_is_read_when_the_ioctl_cannot_be);
    RUN(test_transient_errnos_are_the_ones_a_cold_boot_makes);
    RUN(test_a_keymap_that_cannot_be_right_is_never_retried);
    RUN(test_a_backend_can_be_recovered_without_reading_a_key);
    RUN(test_retrying_a_phone_that_really_has_nothing_is_still_false);
    RUN(test_an_i2c_bus_that_has_not_appeared_says_so_and_not_that_it_is_absent);
    RUN(test_a_failed_open_names_the_syscall_and_the_errno);
    RUN(test_an_unsupported_driver_is_named_in_the_reason);
    return pt_report("test_input");
}
