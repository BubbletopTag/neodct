/* koki_input.c -- engine.py's Input: HELD keys, which read_keypress cannot give.
 *
 * Koki is the one app on the phone that needs to know a key is DOWN, not that
 * it went down: walking is "left is held", jumping is "z is held at the
 * moment the feet touch". The Python went behind the framework's back and
 * read the evdev stream itself for exactly that reason. This does the same
 * thing with the descriptor nd-apprun inherited (nd_app.h, NEODCT_KEYPAD_FD),
 * which under the new architecture carries press AND release records written
 * by the core.
 *
 * ============ THE MATRIX BRANCH IS GONE, ON PURPOSE ============
 *
 * engine.py has a second input path for the i2c keypad, with a rollover
 * scanner, a single-key backend and a documented latching bug where a key is
 * only released when a different one is pressed. None of it is ported, and
 * that is not an omission: nd_input.h settles it in as many words --
 * "Koki's matrix-scanner branch disappears from app code entirely", because
 * the core owns the keypad now and synthesises evdev records onto the pipe.
 * An app process must not touch the i2c bus. The latching bug therefore
 * cannot occur here.
 *
 * ============ WHY THIS DOES NOT USE nd_input ============
 *
 * nd_input tracks held state and would serve, but it consumes the stream to
 * do it, and the two readers would steal each other's records. Koki never
 * calls nd_ui_read_keypress(), so it owns the descriptor for the duration --
 * which is exactly the arrangement nd_ui.h sanctions when it says widgets
 * poll keypad_fd directly. What it does need that nd_input does not expose is
 * the per-FRAME press edge set, which is Scratch's "key pressed?" hat.
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "nd_types.h"
#include "nd_ui.h"

#include "koki.h"

/* struct input_event without <linux/input.h>: the acceptance gate compiles
 * every source under musl-gcc, which does not see /usr/include/linux, and
 * lib/nd_evdev.c sets the precedent. The layout is fixed ABI. */
#define KOKI_EV_KEY 0x01

/* 16 bytes of timeval on a 64-bit host, 8 on 32-bit ARM, then type, code and
 * value. Both are decoded, because a real keypad's layout is decided by the
 * kernel that created the device, not by the program reading it. */
#define KOKI_EV_SIZE_64 24u
#define KOKI_EV_SIZE_32 16u

static size_t ev_native_size(void)
{
    return (sizeof(long) == 8u) ? KOKI_EV_SIZE_64 : KOKI_EV_SIZE_32;
}

/* engine.py's KEYMAP, verbatim, including the phone-keypad aliases that match
 * MATRIX_NAME_TO_CODE in core/main.py. Several codes map to one logical key
 * on purpose: num5, star and z are all "jump". */
static const struct {
    uint16_t code;
    koki_key_id key;
} KEYMAP[] = {
    {105u, KOKI_KEY_LEFT},
    {106u, KOKI_KEY_RIGHT},
    {103u, KOKI_KEY_UP},
    {108u, KOKI_KEY_DOWN},
    {44u, KOKI_KEY_Z},
    {45u, KOKI_KEY_X},
    {28u, KOKI_KEY_ENTER},
    {14u, KOKI_KEY_BACK},
    /* num4 num6 num2 num8 */
    {5u, KOKI_KEY_LEFT},
    {7u, KOKI_KEY_RIGHT},
    {3u, KOKI_KEY_UP},
    {9u, KOKI_KEY_DOWN},
    /* num5 = jump, num0 = action */
    {6u, KOKI_KEY_Z},
    {11u, KOKI_KEY_X},
    /* star = jump, hash = action */
    {42u, KOKI_KEY_Z},
    {43u, KOKI_KEY_X},
};

static bool map_key(uint16_t code, koki_key_id *out)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(KEYMAP); i++) {
        if (KEYMAP[i].code == code) {
            *out = KEYMAP[i].key;
            return true;
        }
    }
    return false;
}

void koki_input_open(koki_input *in, nd_ui *ui)
{
    if (in == NULL)
        return;
    memset(in, 0, sizeof *in);
    in->fd = (ui != NULL) ? ui->keypad_fd : -1;
}

/* One record out of the buffer at `off`, using `sz` as the record size. */
static void decode_record(const uint8_t *buf, size_t off, size_t sz, uint16_t *type, uint16_t *code,
                          int32_t *value)
{
    size_t tail = off + (sz - 8u);

    memcpy(type, buf + tail, 2u);
    memcpy(code, buf + tail + 2u, 2u);
    memcpy(value, buf + tail + 4u, 4u);
}

void koki_input_poll(koki_input *in)
{
    /* 4 KB is 170 records on the 64-bit layout: more than a frame can
     * plausibly hold, and the loop drains until poll says empty anyway. */
    uint8_t buf[4096];
    size_t native = ev_native_size();

    if (in == NULL)
        return;
    memset(in->pressed, 0, sizeof in->pressed);
    if (in->fd < 0)
        return;

    for (;;) {
        struct pollfd pfd;
        struct timespec ts;
        ssize_t got;
        size_t sz;
        size_t off;
        int rc;

        pfd.fd = in->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        /* ppoll rather than select: select's FD_SET is not warning-clean
         * under -Wconversion on musl, which lib/nd_evdev.c hit first. */
        rc = ppoll(&pfd, 1u, &ts, NULL);
        if (rc <= 0)
            break; /* any poll error reads as "nothing", as in the Python */

        got = read(in->fd, buf, sizeof buf);
        if (got <= 0)
            break;

        /* Records arrive whole. Prefer the native size; fall back to the
         * other layout only when the byte count rules the native one out,
         * which is how a 32-bit binary reads a 64-bit kernel's device. The
         * Python read 24 bytes at a time and mis-parsed two queued 16-byte
         * records as one 24-byte one; parsing the whole buffer in units
         * cannot do that. */
        sz = native;
        if (((size_t)got % sz) != 0u) {
            size_t other = (native == KOKI_EV_SIZE_64) ? KOKI_EV_SIZE_32 : KOKI_EV_SIZE_64;

            if (((size_t)got % other) == 0u)
                sz = other;
            else
                break; /* garbage: drop the read, as the Python drops it */
        }

        for (off = 0u; off + sz <= (size_t)got; off += sz) {
            uint16_t type = 0u;
            uint16_t code = 0u;
            int32_t value = 0;
            koki_key_id k;

            decode_record(buf, off, sz, &type, &code, &value);
            if (type != KOKI_EV_KEY)
                continue;
            if (!map_key(code, &k))
                continue;
            if (value == 1) {
                if (!in->held[k])
                    in->pressed[k] = true;
                in->held[k] = true;
            } else if (value == 0) {
                in->held[k] = false;
            }
            /* value == 2 is autorepeat: already held, ignore. */
        }

        if ((size_t)got < sizeof buf)
            break; /* a short read means the channel is empty */
    }
}

bool koki_key(const koki_engine *eng, koki_key_id k)
{
    if (eng == NULL || k >= KOKI_KEY_COUNT)
        return false;
    return eng->input.held[k];
}

bool koki_pressed(const koki_engine *eng, koki_key_id k)
{
    if (eng == NULL || k >= KOKI_KEY_COUNT)
        return false;
    return eng->input.pressed[k];
}

bool koki_any_key(const koki_engine *eng)
{
    size_t i;

    if (eng == NULL)
        return false;
    for (i = 0u; i < (size_t)KOKI_KEY_COUNT; i++) {
        if (eng->input.held[i])
            return true;
    }
    return false;
}

double koki_kdir(const koki_engine *eng)
{
    /* Scratch's (key right - key left) as -1/0/1. A double because every
     * caller multiplies it by a stage-unit step. */
    return (koki_key(eng, KOKI_KEY_RIGHT) ? 1.0 : 0.0) - (koki_key(eng, KOKI_KEY_LEFT) ? 1.0 : 0.0);
}
