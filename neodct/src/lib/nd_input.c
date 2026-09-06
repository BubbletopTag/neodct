/* nd_input.c -- the one key API, over three backends, with press AND release.
 *
 * This is the module the settled answer to OPEN-QUESTIONS.md question 2 lands
 * in. The core owns the keypad; an app gets a pipe carrying ordinary evdev
 * records and reads it with exactly the code below. Nothing above this line
 * knows or cares which backend produced a key.
 *
 * ============ WHERE RELEASES COME FROM ============
 *
 * evdev and the app pipe carry them already. The i2c matrix does not -- it
 * reports press edges and nothing else, which is precisely why Koki used to
 * reach into the scanner's private state. So for the matrix backend this
 * module keeps a shadow of the positions it has reported as pressed and
 * compares it against nd_matrix_held() after every scan; a position that has
 * left the scanner's debounced held set becomes a release event. Releases are
 * emitted BEFORE the press that displaced them, which is what makes the
 * one-key-at-a-time hardware look sane to an app written for a keyboard.
 *
 * ============ AUTO-REPEAT ============
 *
 * Synthesised here rather than passed through from the kernel, because the
 * matrix has no kernel to pass anything through. One implementation means an
 * app behaves identically on the phone and under QEMU. Defaults, and the
 * reasoning behind the two numbers, are in nd_keypad.h.
 *
 * ============ A DELIBERATE DEVIATION ============
 *
 * The Python's read_keypress() waits the FULL timeout on the matrix and then
 * the FULL timeout again on evdev, so a poll with both backends present can
 * block for 2x timeout. This module polls both inside one timeout, because
 * nd_input.h's contract says "or ND_KEY_NONE if none arrived within
 * timeout_s" and because the double wait cannot happen on the phone: the
 * target has no /dev/input/event0 when the matrix is in use. Recorded in
 * OPEN-QUESTIONS.md.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"

#include "nd_input_priv.h"

#define ND_EV_KEY     0x01
#define ND_EV_SYN     0x00
#define ND_SYN_REPORT 0

/* Synthesised releases plus the press that displaced them. Sixteen is far
 * more than the hardware can produce in one scan and costs 256 bytes. */
#define QUEUE_MAX 16

/* How often a core with no evdev descriptor looks for one again. See
 * try_reopen_evdev(). One second is chosen against a person's patience
 * rather than against the race it exists for, which is over in a few
 * hundred milliseconds: it has to be short enough that a phone which came
 * up keyless is typing before the owner has finished wondering, and long
 * enough that a phone which genuinely has no keyboard -- every Luckfox,
 * where the keypad is an i2c matrix -- is not opening a directory sixty
 * times a second forever. */
#define ND_REOPEN_INTERVAL_US 1000000u

typedef struct {
    int32_t code;
    bool used;
    uint64_t next_repeat_us; /* 0 when this code does not repeat */
} held_slot;

struct nd_input {
    nd_input_backend backend;

    int fd; /* evdev or pipe; -1 when there is none */
    bool owns_fd;
    char path[ND_PATH_MAX];

    /* Only an input opened by nd_input_open() may go looking for a device on
     * its own. A wrapped pipe or fd belongs to the caller -- an app's channel
     * is the obvious one -- and must never turn into an evdev reader behind
     * its back. */
    bool may_reopen;
    uint64_t reopen_after_us;

    bool have_matrix;
    nd_matrix_input matrix;
    /* Positions this module has reported as pressed, so it can tell when one
     * has gone. Indexed [row][col]; holds the keycode, or -1. */
    int32_t matrix_down[ND_MATRIX_MAX_PINS][ND_MATRIX_MAX_PINS];

    held_slot held[ND_INPUT_HELD_MAX];

    uint64_t repeat_delay_us;
    uint64_t repeat_interval_us;
    int32_t repeat_codes[ND_INPUT_HELD_MAX];
    size_t n_repeat_codes;
    bool last_was_repeat;

    nd_key_event queue[QUEUE_MAX];
    size_t q_head;
    size_t q_len;

    /* Why there is no active backend, in words for the screen -- empty when
     * there is one. Set by try_open_matrix() and finalised in nd_input_open();
     * read by nd_input_no_backend_reason() so the core can show a keyless
     * phone what went wrong instead of a home screen it cannot drive. */
    char no_backend[ND_INPUT_REASON_MAX];
};

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static void sleep_us(uint64_t usec)
{
    struct timespec req;

    if (usec == 0u)
        return;
    req.tv_sec = (time_t)(usec / 1000000u);
    req.tv_nsec = (long)((usec % 1000000u) * 1000u);
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

/* ------------------------------------------------------------------ *
 * The queue
 * ------------------------------------------------------------------ */

static void queue_push(nd_input *in, int32_t code, bool pressed, uint64_t at)
{
    size_t slot;

    if (in->q_len >= QUEUE_MAX)
        return; /* the hardware cannot fill this; dropping beats blocking */
    slot = (in->q_head + in->q_len) % QUEUE_MAX;
    in->queue[slot].code = code;
    in->queue[slot].pressed = pressed;
    in->queue[slot].time_us = at;
    in->q_len++;
}

static bool queue_pop(nd_input *in, nd_key_event *out)
{
    if (in->q_len == 0u)
        return false;
    *out = in->queue[in->q_head];
    in->q_head = (in->q_head + 1u) % QUEUE_MAX;
    in->q_len--;
    return true;
}

/* ------------------------------------------------------------------ *
 * Held state and repeat
 * ------------------------------------------------------------------ */

static bool code_repeats(const nd_input *in, int32_t code)
{
    size_t i;

    if (in->repeat_delay_us == 0u || in->repeat_interval_us == 0u)
        return false;
    for (i = 0u; i < in->n_repeat_codes; i++) {
        if (in->repeat_codes[i] == code)
            return true;
    }
    return false;
}

static void held_press(nd_input *in, int32_t code, uint64_t at)
{
    size_t free_slot = ND_INPUT_HELD_MAX;
    size_t i;

    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (in->held[i].used && in->held[i].code == code)
            return; /* already down; a second press edge changes nothing */
        if (!in->held[i].used && free_slot == ND_INPUT_HELD_MAX)
            free_slot = i;
    }
    if (free_slot == ND_INPUT_HELD_MAX)
        return;

    in->held[free_slot].used = true;
    in->held[free_slot].code = code;
    in->held[free_slot].next_repeat_us = code_repeats(in, code) ? at + in->repeat_delay_us : 0u;
}

static void held_release(nd_input *in, int32_t code)
{
    size_t i;

    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (in->held[i].used && in->held[i].code == code) {
            in->held[i].used = false;
            in->held[i].next_repeat_us = 0u;
            return;
        }
    }
}

/* The soonest repeat that is due, or 0 when nothing is armed. */
static uint64_t next_repeat_due(const nd_input *in)
{
    uint64_t soonest = 0u;
    size_t i;

    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (!in->held[i].used || in->held[i].next_repeat_us == 0u)
            continue;
        if (soonest == 0u || in->held[i].next_repeat_us < soonest)
            soonest = in->held[i].next_repeat_us;
    }
    return soonest;
}

/* Emit the repeat that is due, if one is. */
static bool take_repeat(nd_input *in, uint64_t now, nd_key_event *out)
{
    size_t best = ND_INPUT_HELD_MAX;
    size_t i;

    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (!in->held[i].used || in->held[i].next_repeat_us == 0u)
            continue;
        if (in->held[i].next_repeat_us > now)
            continue;
        if (best == ND_INPUT_HELD_MAX || in->held[i].next_repeat_us < in->held[best].next_repeat_us)
            best = i;
    }
    if (best == ND_INPUT_HELD_MAX)
        return false;

    /* Advance from the scheduled time, not from now, so a slow frame does not
     * make the list scroll slower than the key is held. */
    in->held[best].next_repeat_us += in->repeat_interval_us;
    if (in->held[best].next_repeat_us <= now)
        in->held[best].next_repeat_us = now + in->repeat_interval_us;

    out->code = in->held[best].code;
    out->pressed = true;
    out->time_us = now;
    in->last_was_repeat = true;
    return true;
}

/* ------------------------------------------------------------------ *
 * Backends
 * ------------------------------------------------------------------ */

/* One matrix scan, turned into release edges plus at most one press. */
static void matrix_poll_into_queue(nd_input *in)
{
    int32_t code;
    uint64_t at;
    size_t row;
    size_t col;

    code = nd_matrix_input_poll(&in->matrix);
    at = now_us();

    /* Releases first: on this hardware a new press has already displaced
     * whatever was down, and an app that sees the press before the release
     * ends up believing two keys are held on a keypad that cannot do it. */
    for (row = 0u; row < ND_MATRIX_MAX_PINS; row++) {
        for (col = 0u; col < ND_MATRIX_MAX_PINS; col++) {
            if (in->matrix_down[row][col] < 0)
                continue;
            if (nd_matrix_is_held(&in->matrix.scanner, (uint8_t)row, (uint8_t)col))
                continue;
            queue_push(in, in->matrix_down[row][col], false, at);
            in->matrix_down[row][col] = -1;
        }
    }

    if (code == ND_KEY_NONE)
        return;

    /* Record where that code came from so its release can be found later.
     * The scan that produced it has already updated the scanner's held set. */
    for (row = 0u; row < ND_MATRIX_MAX_PINS; row++) {
        for (col = 0u; col < ND_MATRIX_MAX_PINS; col++) {
            if (in->matrix.cfg.matrix_to_code[row][col] != code)
                continue;
            if (!nd_matrix_is_held(&in->matrix.scanner, (uint8_t)row, (uint8_t)col))
                continue;
            in->matrix_down[row][col] = code;
        }
    }
    queue_push(in, code, true, at);
}

/* Drain everything the descriptor has right now into the queue. */
static bool fd_poll_into_queue(nd_input *in, double wait_s)
{
    bool got_any = false;

    for (;;) {
        uint16_t type = 0u;
        uint16_t code = 0u;
        int32_t value = 0;

        if (!nd_evdev_read_record(in->fd, got_any ? 0.0 : wait_s, &type, &code, &value))
            break;
        got_any = true;

        if (type != ND_EV_KEY)
            continue; /* EV_SYN and everything else is not a key */
        /* Value 2 is the kernel's autorepeat. Dropped, exactly as the Python
         * dropped it, because this module makes its own. */
        if (value != 0 && value != 1)
            continue;
        queue_push(in, (int32_t)code, value == 1, now_us());
        if (in->q_len >= QUEUE_MAX)
            break;
    }
    return got_any;
}

/* ------------------------------------------------------------------ *
 * Opening
 * ------------------------------------------------------------------ */

static void input_defaults(nd_input *in)
{
    size_t row;
    size_t col;

    in->fd = -1;
    in->backend = ND_INPUT_NONE;
    in->may_reopen = false;
    in->reopen_after_us = 0u;
    in->repeat_delay_us = (uint64_t)(ND_REPEAT_DELAY_S * 1e6);
    in->repeat_interval_us = (uint64_t)(ND_REPEAT_INTERVAL_S * 1e6);
    /* Arrows only. See the reasoning in nd_keypad.h -- a repeat on a digit
     * would cycle T9 multi-tap letters behind the user's back. */
    in->repeat_codes[0] = ND_KEY_UP;
    in->repeat_codes[1] = ND_KEY_DOWN;
    in->repeat_codes[2] = ND_KEY_LEFT;
    in->repeat_codes[3] = ND_KEY_RIGHT;
    in->n_repeat_codes = 4u;

    for (row = 0u; row < ND_MATRIX_MAX_PINS; row++) {
        for (col = 0u; col < ND_MATRIX_MAX_PINS; col++)
            in->matrix_down[row][col] = -1;
    }
}

/* core/main.py:542 -- the matrix half of the backend selection. */
static void try_open_matrix(nd_input *in)
{
    nd_keymap cfg;
    char i2c_dev[32];

    if (nd_keymap_load(ND_PATH_KEYMAP, &cfg) != ND_OK) {
        (void)nd_strlcpy(in->no_backend,
                         "No keypad map yet. First-boot keypad setup has not written one.",
                         sizeof in->no_backend);
        return;
    }

    if (strcmp(cfg.driver, "pcf8575-i2c") != 0) {
        /* The gpiozero backend is deliberately not ported: it needs a keymap
         * naming a different driver AND /dev/gpiochip*, neither of which the
         * target has, and porting it means reimplementing gpiozero over
         * libgpiod for a path that cannot run. Recorded in
         * OPEN-QUESTIONS.md; the refusal line is kept. */
        nd_log(ND_LOG_INPUT, "Keymap present, but the %s driver is not supported.", cfg.driver);
        (void)nd_snprintf(in->no_backend, sizeof in->no_backend,
                          "Keymap names an unsupported keypad driver: %s.", cfg.driver);
        return;
    }

    if (snprintf(i2c_dev, sizeof i2c_dev, "/dev/i2c-%d", cfg.i2c_bus) < 0)
        return;
    if (!nd_path_exists(i2c_dev)) {
        nd_log(ND_LOG_INPUT, "Keymap wants %s, but %s does not exist.", cfg.driver, i2c_dev);
        (void)nd_snprintf(in->no_backend, sizeof in->no_backend,
                          "The keypad's i2c bus %s does not exist.", i2c_dev);
        return;
    }

    if (nd_matrix_input_open(&in->matrix, &cfg) != ND_OK) {
        nd_log(ND_LOG_INPUT, "I2C matrix init failed; falling back to evdev.");
        (void)nd_snprintf(in->no_backend, sizeof in->no_backend,
                          "The keypad on %s did not open (a permission or wiring problem).",
                          i2c_dev);
        return;
    }
    in->no_backend[0] = '\0'; /* a backend exists */
    in->have_matrix = true;
    in->backend = ND_INPUT_MATRIX;
    nd_log(ND_LOG_INPUT, "I2C matrix input active from %s (bus=%d addr=0x%02X rows=%u cols=%u).",
           cfg.path, cfg.i2c_bus, (unsigned)cfg.i2c_addr, (unsigned)cfg.n_rows,
           (unsigned)cfg.n_cols);
}

/* ============ WHEN THE KEYBOARD IS LATE, AND NOT ABSENT ============
 *
 * A device node exists before udev has touched it. devtmpfs creates
 * /dev/input/event0 the moment the kernel registers the device, owned
 * root:root and mode 0600; the group and the 0660 that make it readable by
 * ndusr are applied afterwards, when udevd processes the uevent and
 * 50-udev-default.rules' `SUBSYSTEM=="input", GROUP="input"` runs. Between
 * those two instants the node is present, correctly named, and unopenable by
 * anything that is not root.
 *
 * The core opens input AFTER dropping to ndusr (nd_main.c step 4b), so it can
 * land in that window, and S10udevd cannot close it on its own: it triggers
 * and settles the input subsystem, but a device that has not registered YET
 * is not in the set the trigger replays, so settle returns having waited for
 * nothing. Measured at roughly one boot in five under QEMU, where the keypad
 * is a virtio-input device on the PCI bus. It was invisible for as long as
 * the UI ran as root, because root ignores the mode.
 *
 * A failure to open therefore does not mean there is no keyboard. It means
 * there is no keyboard YET, and the difference is the whole bug: giving up
 * once at startup is what turned a race measured in milliseconds into a
 * phone with no keys until it was rebooted.
 *
 * So the descriptor is looked for again, at ND_REOPEN_INTERVAL_US, from the
 * read path -- which is the one place that is already awake and already
 * knows the difference between "waiting for a key" and "spinning". It costs
 * nothing on a phone whose keyboard opened, because it never runs there.
 *
 * It stays enabled after the first success is impossible to need, because
 * the phone that needs it most is the one with no evdev device at all: a USB
 * keyboard plugged into a real handset arrives through exactly this path,
 * which is what nd_input_open()'s "both backends can coexist" comment
 * promises and could not previously deliver. */
static void try_reopen_evdev(nd_input *in, uint64_t now)
{
    char path[ND_PATH_MAX];
    int fd;

    if (!in->may_reopen || in->fd >= 0)
        return;
    if (in->reopen_after_us != 0u && now < in->reopen_after_us)
        return;
    in->reopen_after_us = now + ND_REOPEN_INTERVAL_US;

    (void)nd_evdev_discover_quiet(path, sizeof path);
    fd = nd_evdev_open(path);
    if (fd < 0)
        return;

    in->fd = fd;
    in->owns_fd = true;
    (void)nd_strlcpy(in->path, path, sizeof in->path);
    if (!in->have_matrix)
        in->backend = ND_INPUT_EVDEV;
    /* Deliberately at the same level as the failure that preceded it: a phone
     * that started keyless and then got keys has to say so, or the warning in
     * the log is the last word on a problem that fixed itself. */
    nd_log(ND_LOG_INPUT, "Input recovered: listening on %s", path);
}

nd_err nd_input_open(nd_input **out)
{
    /* owned by the caller; free with nd_input_close() */
    nd_input *in;
    char path[ND_PATH_MAX];

    if (out == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    in = calloc(1u, sizeof *in);
    if (in == NULL)
        return ND_ERR_NOMEM;
    input_defaults(in);

    try_open_matrix(in);

    /* Both backends can coexist: the evdev device is opened regardless, so a
     * developer with a USB keyboard plugged into a real phone can still type. */
    (void)nd_evdev_discover(path, sizeof path);
    in->fd = nd_evdev_open(path);
    if (in->fd < 0) {
        nd_log(ND_LOG_INPUT, "Failed opening %s: %s", path, strerror(errno));
        if (strcmp(path, ND_PATH_KEYPAD) != 0) {
            nd_log(ND_LOG_INPUT, "Falling back to %s", ND_PATH_KEYPAD);
            (void)nd_strlcpy(path, ND_PATH_KEYPAD, sizeof path);
            in->fd = nd_evdev_open(path);
            if (in->fd < 0)
                nd_log(ND_LOG_INPUT, "Evdev fallback failed: %s", strerror(errno));
        }
    }

    if (in->fd >= 0) {
        in->owns_fd = true;
        (void)nd_strlcpy(in->path, path, sizeof in->path);
        if (!in->have_matrix)
            in->backend = ND_INPUT_EVDEV;
        in->no_backend[0] = '\0'; /* the keyboard opened; there is a backend */
        nd_log(ND_LOG_INPUT, "Listening on %s", path);
    } else if (!in->have_matrix) {
        nd_log(ND_LOG_INPUT, "WARNING: no active input backend.");
        /* try_open_matrix() left the reason; a phone that reached here with an
         * empty one had no keymap and no keyboard at all. */
        if (in->no_backend[0] == '\0')
            (void)nd_strlcpy(in->no_backend, "No keypad and no keyboard were found.",
                             sizeof in->no_backend);
    }

    /* Set last, and for both outcomes: a phone that opened its keyboard can
     * still lose it, and one that has a matrix can still be handed a USB
     * keyboard. See try_reopen_evdev(). */
    in->may_reopen = true;

    *out = in;
    return ND_OK;
}

static nd_err open_wrapped(nd_input **out, int fd, nd_input_backend backend)
{
    /* owned by the caller; free with nd_input_close() */
    nd_input *in;

    if (out == NULL || fd < 0)
        return ND_ERR_INVAL;
    *out = NULL;

    in = calloc(1u, sizeof *in);
    if (in == NULL)
        return ND_ERR_NOMEM;
    input_defaults(in);

    in->fd = fd;
    in->owns_fd = true;
    in->backend = backend;
    *out = in;
    return ND_OK;
}

nd_err nd_input_open_pipe(nd_input **out, int fd)
{
    return open_wrapped(out, fd, ND_INPUT_PIPE);
}

nd_err nd_input_open_fd(nd_input **out, int fd)
{
    return open_wrapped(out, fd, ND_INPUT_EVDEV);
}

void nd_input_close(nd_input *in)
{
    if (in == NULL)
        return;
    if (in->have_matrix)
        nd_matrix_input_close(&in->matrix);
    if (in->owns_fd && in->fd >= 0)
        (void)close(in->fd);
    free(in);
}

nd_input_backend nd_input_which(const nd_input *in)
{
    return (in != NULL) ? in->backend : ND_INPUT_NONE;
}

bool nd_input_has_matrix(const nd_input *in)
{
    return in != NULL && in->have_matrix;
}

int nd_input_fd(const nd_input *in)
{
    return (in != NULL) ? in->fd : -1;
}

/* An opened nd_input can exist with nothing behind it -- no matrix and no
 * evdev descriptor -- and then every read returns nothing, which on a phone
 * looks like dead keys. This is how the core tells that apart from a quiet
 * keypad. A wrapped pipe (an app's channel) always counts as a backend. */
bool nd_input_has_backend(const nd_input *in)
{
    if (in == NULL)
        return false;
    return in->have_matrix || in->fd >= 0;
}

/* The one-line reason there is no backend, or "" when there is one. Never
 * NULL. For the screen a keyless phone shows instead of an undriveable home. */
const char *nd_input_no_backend_reason(const nd_input *in)
{
    if (in == NULL)
        return "No input was opened at all.";
    return in->no_backend;
}

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

bool nd_input_read_event(nd_input *in, double timeout_s, nd_key_event *out)
{
    const double poll_s = (double)ND_READ_POLL_US / 1e6;
    uint64_t deadline = 0u;
    bool forever;
    bool first = true;

    if (in == NULL || out == NULL)
        return false;

    forever = timeout_s < 0.0;
    if (!forever)
        deadline = now_us() + (uint64_t)(timeout_s * 1e6);

    for (;;) {
        uint64_t now;
        uint64_t due;
        uint64_t wake;

        if (queue_pop(in, out)) {
            in->last_was_repeat = false;
            if (out->pressed)
                held_press(in, out->code, out->time_us);
            else
                held_release(in, out->code);
            return true;
        }

        now = now_us();
        if (take_repeat(in, now, out))
            return true;

        /* Every backend is polled AT LEAST ONCE even at timeout 0: the
         * Browser's drain path calls this with 0 precisely to consume input
         * that is already waiting. */
        if (!first && !forever && now >= deadline)
            return false;
        first = false;

        /* Wake at whichever comes first, the caller's deadline or the next
         * repeat. Sleeping past a repeat would make hold-to-scroll stutter in
         * exactly the widget that asked for it. */
        wake = forever ? 0u : deadline;
        due = next_repeat_due(in);
        if (due != 0u && (wake == 0u || due < wake))
            wake = due;

        if (in->have_matrix) {
            matrix_poll_into_queue(in);
            if (in->q_len > 0u)
                continue;
        }

        /* Not there YET, rather than not there at all: the window between
         * devtmpfs making the node and udev making it readable. Rate-limited
         * inside, so this costs one comparison on every phone that already
         * has its keyboard. */
        if (in->fd < 0)
            try_reopen_evdev(in, now_us());

        if (in->fd >= 0) {
            double slice;

            now = now_us();
            if (wake == 0u)
                slice = in->have_matrix ? poll_s : -1.0;
            else if (wake <= now)
                slice = 0.0;
            else
                slice = (double)(wake - now) / 1e6;
            /* With a matrix present the descriptor must not own the whole
             * wait: the matrix has to be rescanned at its own cadence. */
            if (in->have_matrix && (slice < 0.0 || slice > poll_s))
                slice = poll_s;
            if (fd_poll_into_queue(in, slice))
                continue;
        } else if (in->have_matrix) {
            uint64_t nap = (uint64_t)ND_READ_POLL_US;

            now = now_us();
            if (!forever) {
                if (now >= deadline)
                    return false;
                if (deadline - now < nap)
                    nap = deadline - now;
            }
            if (wake != 0u && wake > now && wake - now < nap)
                nap = wake - now;
            sleep_us(nap);
        } else {
            /* No backend at all. Sleeping out the timeout rather than
             * spinning is what keeps wait_for_key() off 100% of a core --
             * the Python has the same branch for the same reason. */
            now = now_us();
            if (forever) {
                sleep_us(100000u);
                continue;
            }
            if (now < deadline)
                sleep_us(deadline - now);
            return false;
        }
    }
}

int32_t nd_input_read_key(nd_input *in, double timeout_s)
{
    uint64_t deadline = 0u;
    bool forever;

    if (in == NULL)
        return ND_KEY_NONE;

    forever = timeout_s < 0.0;
    if (!forever)
        deadline = now_us() + (uint64_t)(timeout_s * 1e6);

    for (;;) {
        nd_key_event ev;
        double remaining = -1.0;

        if (!forever) {
            uint64_t now = now_us();

            remaining = (now >= deadline) ? 0.0 : (double)(deadline - now) / 1e6;
        }
        if (!nd_input_read_event(in, remaining, &ev))
            return ND_KEY_NONE;
        if (ev.pressed)
            return ev.code;
        /* A release is consumed to keep held state honest but never returned:
         * this is the ui.read_keypress() shape every screen is written to. */
        if (!forever && now_us() >= deadline)
            return ND_KEY_NONE;
    }
}

int32_t nd_input_wait_key(nd_input *in)
{
    for (;;) {
        int32_t code = nd_input_read_key(in, 0.1);

        if (code != ND_KEY_NONE)
            return code;
    }
}

bool nd_input_is_held(const nd_input *in, int32_t code)
{
    size_t i;

    if (in == NULL)
        return false;
    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (in->held[i].used && in->held[i].code == code)
            return true;
    }
    return false;
}

size_t nd_input_held(const nd_input *in, int32_t *out, size_t max)
{
    size_t n = 0u;
    size_t i;

    if (in == NULL)
        return 0u;
    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (!in->held[i].used)
            continue;
        if (out != NULL && n < max)
            out[n] = in->held[i].code;
        n++;
    }
    return n;
}

void nd_input_drain(nd_input *in)
{
    size_t guard;

    if (in == NULL)
        return;

    /* Held state is still updated while draining -- the events are discarded,
     * but a release that happened before the screen changed must not leave a
     * key stuck down forever.
     *
     * The guard is 256 rather than the Browser's 64 because one call here
     * consumes one event, where the Browser's 64 iterations each consumed a
     * whole scan; the matrix's pending queue is at most 256 positions and
     * cannot outlast this. */
    for (guard = 0u; guard < 256u; guard++) {
        nd_key_event ev;

        if (!nd_input_read_event(in, 0.0, &ev))
            break;
    }
}

/* ------------------------------------------------------------------ *
 * Repeat tuning
 * ------------------------------------------------------------------ */

void nd_input_set_repeat(nd_input *in, double delay_s, double interval_s)
{
    if (in == NULL)
        return;
    if (delay_s <= 0.0 || interval_s <= 0.0) {
        in->repeat_delay_us = 0u;
        in->repeat_interval_us = 0u;
        return;
    }
    in->repeat_delay_us = (uint64_t)(delay_s * 1e6);
    in->repeat_interval_us = (uint64_t)(interval_s * 1e6);
}

nd_err nd_input_set_repeat_codes(nd_input *in, const int32_t *codes, size_t n)
{
    size_t i;

    if (in == NULL)
        return ND_ERR_INVAL;
    if (codes == NULL) {
        in->repeat_codes[0] = ND_KEY_UP;
        in->repeat_codes[1] = ND_KEY_DOWN;
        in->repeat_codes[2] = ND_KEY_LEFT;
        in->repeat_codes[3] = ND_KEY_RIGHT;
        in->n_repeat_codes = 4u;
        return ND_OK;
    }
    if (n > ND_INPUT_HELD_MAX)
        return ND_ERR_TOOLONG;
    for (i = 0u; i < n; i++)
        in->repeat_codes[i] = codes[i];
    in->n_repeat_codes = n;

    /* A key already down must not keep an arming it no longer qualifies for. */
    for (i = 0u; i < ND_INPUT_HELD_MAX; i++) {
        if (in->held[i].used && !code_repeats(in, in->held[i].code))
            in->held[i].next_repeat_us = 0u;
    }
    return ND_OK;
}

bool nd_input_last_was_repeat(const nd_input *in)
{
    return in != NULL && in->last_was_repeat;
}

/* ------------------------------------------------------------------ *
 * The core side of the app channel
 * ------------------------------------------------------------------ */

/* The native struct input_event, whatever that is on this build. Both ends of
 * this pipe are the same program family on the same machine, so writing the
 * native layout is safe -- and the reader above handles the other one anyway,
 * because a real device's layout is not ours to choose. */
typedef struct {
    long tv_sec;
    long tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} channel_record;

nd_err nd_input_channel_open(nd_input_channel *ch)
{
    int fds[2];
    int flags;

    if (ch == NULL)
        return ND_ERR_INVAL;
    ch->read_fd = -1;
    ch->write_fd = -1;

    if (pipe2(fds, O_CLOEXEC) != 0)
        return ND_ERR_IO;

    /* The write end is non-blocking so a child that stops reading -- because
     * it is wedged, or mid-crash -- cannot take the core down with it. */
    flags = fcntl(fds[1], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    ch->read_fd = fds[0];
    ch->write_fd = fds[1];
    return ND_OK;
}

nd_err nd_input_channel_send(nd_input_channel *ch, int32_t code, bool pressed)
{
    channel_record rec[2];
    struct timespec ts;
    ssize_t n;

    if (ch == NULL || ch->write_fd < 0)
        return ND_ERR_INVAL;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }

    memset(rec, 0, sizeof rec);
    rec[0].tv_sec = (long)ts.tv_sec;
    rec[0].tv_usec = (long)(ts.tv_nsec / 1000);
    rec[0].type = ND_EV_KEY;
    rec[0].code = (uint16_t)code;
    rec[0].value = pressed ? 1 : 0;

    /* The kernel terminates every report with EV_SYN/SYN_REPORT, and a reader
     * written against a real device is entitled to expect it. */
    rec[1] = rec[0];
    rec[1].type = ND_EV_SYN;
    rec[1].code = ND_SYN_REPORT;
    rec[1].value = 0;

    n = write(ch->write_fd, rec, sizeof rec);
    if (n != (ssize_t)sizeof rec)
        return ND_ERR_IO; /* the child has gone; the caller reaps it */
    return ND_OK;
}

void nd_input_channel_close_read(nd_input_channel *ch)
{
    if (ch == NULL || ch->read_fd < 0)
        return;
    (void)close(ch->read_fd);
    ch->read_fd = -1;
}

void nd_input_channel_close_write(nd_input_channel *ch)
{
    if (ch == NULL || ch->write_fd < 0)
        return;
    (void)close(ch->write_fd);
    ch->write_fd = -1;
}

void nd_input_channel_close(nd_input_channel *ch)
{
    nd_input_channel_close_read(ch);
    nd_input_channel_close_write(ch);
}
