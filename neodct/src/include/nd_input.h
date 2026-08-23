/* nd_input.h -- keys, from three places, with press AND release.
 *
 * The core owns the keypad. There are three backends and one API:
 *
 *   ND_INPUT_MATRIX  the i2c PCF8575 key matrix on real hardware
 *   ND_INPUT_EVDEV   /dev/input/eventN, which is what QEMU and a USB keyboard
 *                    give you
 *   ND_INPUT_PIPE    the channel an app child inherits from the core
 *
 * ============ THE CROSS-PROCESS CHANNEL (settled decision) ============
 *
 * OPEN-QUESTIONS.md question 2 is answered: apps are separate processes and
 * must not touch the i2c bus, so THE CORE SENDS PRESS AND RELEASE RECORDS TO
 * THE APP. It synthesises ordinary evdev `struct input_event` records onto a
 * pipe the child inherits, and the child reads them with the same code path it
 * would use for a real device. Consequences, all intended:
 *
 *   - Koki's matrix-scanner branch disappears from app code entirely.
 *   - HELD-KEY STATE AND KEY REPEAT BECOME AVAILABLE TO EVERY APP, not just
 *     Koki. This is the point of the exercise, not a side effect: it is what
 *     fixes NetSurf navigation and lets VerticalList and PagedList hold to
 *     repeat. Treat it as a requirement.
 *   - Apps need no input-device permission at all, which is what makes the
 *     sandboxing in SECURITY.md possible later.
 *
 * Caveat to carry forward: the i2c matrix reports ONE KEY AT A TIME. No
 * chords on hardware, even though a QEMU keyboard will happily send them.
 * Do not design a feature that needs two keys down at once.
 *
 * The records on the pipe are native `struct input_event`, so their size
 * differs between the 32-bit target and the 64-bit host. Both ends of the pipe
 * are the same process family on the same machine, so that is safe -- but the
 * evdev READER still handles both the 24- and 16-byte layouts, because the
 * kernel's own layout depends on how the device was created.
 */

#ifndef ND_INPUT_H_INCLUDED
#define ND_INPUT_H_INCLUDED

#include "nd_keycodes.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_INPUT_NONE = 0, ND_INPUT_EVDEV, ND_INPUT_MATRIX, ND_INPUT_PIPE } nd_input_backend;

/* One key transition. `pressed` false is a release -- the thing the Python
 * never reported and Koki had to go behind its back to find out. */
typedef struct {
    int32_t code;
    bool pressed;
    uint64_t time_us; /* CLOCK_MONOTONIC microseconds, for repeat timing */
} nd_key_event;

/* At most this many keys can be down at once. The hardware allows one; QEMU
 * allows more; 16 is slack with no cost. */
#define ND_INPUT_HELD_MAX 16

typedef struct nd_input nd_input;

/* ------------------------------------------------------------------ *
 * Opening
 * ------------------------------------------------------------------ */

/* The core's path: try the matrix keypad (only when /NeoDCT/User/keymap.json
 * exists and names a driver that is present), then evdev. Both may fail, in
 * which case reads always return ND_KEY_NONE and the core logs
 * "[INPUT] WARNING: no active input backend." -- boot continues. */
nd_err nd_input_open(nd_input **out);

/* An app's path: wrap the inherited channel. Takes ownership of fd. */
nd_err nd_input_open_pipe(nd_input **out, int fd);

/* A test's path: wrap any already-open evdev-format fd. */
nd_err nd_input_open_fd(nd_input **out, int fd);

void nd_input_close(nd_input *in);

nd_input_backend nd_input_which(const nd_input *in);

/* framework._t9_active(): true only on the real i2c keypad. The T9 mode
 * indicator is never drawn on a QEMU keyboard, and that is correct. */
bool nd_input_has_matrix(const nd_input *in);

/* The readable descriptor, or -1. Widgets read it directly to flush pending
 * input before showing a screen -- AppSelector and PagedList poll with a
 * 0.01 s timeout, MessageDialog with 0.0. Keep those two numbers apart. */
int nd_input_fd(const nd_input *in);

/* ------------------------------------------------------------------ *
 * Reading
 * ------------------------------------------------------------------ */

/* One PRESS, or ND_KEY_NONE if none arrived within timeout_s. Releases are
 * consumed to maintain held state but are not returned -- this is the
 * ui.read_keypress() shape every existing screen is written against.
 * timeout_s < 0 blocks forever. */
int32_t nd_input_read_key(nd_input *in, double timeout_s);

/* Block until a press arrives. Never returns ND_KEY_NONE. */
int32_t nd_input_wait_key(nd_input *in);

/* The full transition, presses and releases both. This is the API a game or a
 * scrolling list uses to implement hold-to-repeat. Returns false on timeout. */
bool nd_input_read_event(nd_input *in, double timeout_s, nd_key_event *out);

/* Held state, derived from the press/release stream. Cheap -- it is a
 * bitmap-ish scan of at most ND_INPUT_HELD_MAX entries, no syscall. */
bool nd_input_is_held(const nd_input *in, int32_t code);
size_t nd_input_held(const nd_input *in, int32_t *out, size_t max);

/* Discard everything pending without blocking. The Browser does this before
 * handing the screen over; the widgets do it before their first draw. */
void nd_input_drain(nd_input *in);

/* ------------------------------------------------------------------ *
 * The core side of the app channel
 * ------------------------------------------------------------------ */

typedef struct {
    int read_fd;  /* handed to the child, becomes its keypad_fd    */
    int write_fd; /* kept by the core                              */
} nd_input_channel;

/* pipe2(O_CLOEXEC) plus a non-blocking write end, so a child that stops
 * reading cannot wedge the core. */
nd_err nd_input_channel_open(nd_input_channel *ch);

/* Write one press or release as a native struct input_event pair (the key
 * event followed by EV_SYN/SYN_REPORT, exactly as the kernel does).
 * ND_ERR_IO when the child has gone; the caller reaps it. */
nd_err nd_input_channel_send(nd_input_channel *ch, int32_t code, bool pressed);

/* Close both ends. Safe on an unopened channel. */
void nd_input_channel_close(nd_input_channel *ch);

/* The core closes its read end after the fork; the child closes the write end
 * before exec. Both are just close(2), but naming them stops the fd leak that
 * makes a child's read block forever. */
void nd_input_channel_close_read(nd_input_channel *ch);
void nd_input_channel_close_write(nd_input_channel *ch);

/* ------------------------------------------------------------------ *
 * evdev, exposed for the core and the tests
 * ------------------------------------------------------------------ */

/* _discover_keypad_path(), in the Python's exact priority order:
 *   1. $NEODCT_KEYPAD_DEVICE if it is set AND the path exists (realpath'd)
 *   2. sorted "/dev/input/by-path" entries ending in "-kbd"
 *   3. sorted "/dev/input/by-id" entries ending in "-kbd"
 *   4. /dev/input/event0 if it exists
 * Sort with strcmp, not strcoll -- Python's sorted() is code-point order and a
 * locale must never reorder the device list. */
nd_err nd_evdev_discover(char *out_path, size_t out_sz);

/* EVIOCGNAME, for the log line. Empty string when the ioctl fails. */
nd_err nd_evdev_device_name(const char *path, char *out, size_t out_sz);

/* O_RDONLY|O_NONBLOCK. -1 on failure with errno set. */
int nd_evdev_open(const char *path);

/* One press, or ND_KEY_NONE. Only EV_KEY with value == 1 produces a code;
 * value 2 (autorepeat) is ignored, matching the Python. Handles both the
 * 24-byte and 16-byte input_event layouts. */
int32_t nd_evdev_read_key(int fd, double timeout_s);

/* ------------------------------------------------------------------ *
 * The keymap file and the i2c matrix
 * ------------------------------------------------------------------ */

#define ND_KEYMAP_MAX_ROWS 16
#define ND_KEYMAP_MAX_COLS 16

typedef struct {
    char path[128];
    char format[48];
    char driver[32]; /* "pcf8575-i2c" or the gpiozero form */
    uint8_t row_pins[ND_KEYMAP_MAX_ROWS];
    size_t n_rows;
    uint8_t col_pins[ND_KEYMAP_MAX_COLS];
    size_t n_cols;
    /* -1 where a position is unmapped. 1 KB flat, which removes every hash
     * lookup from the scan path. */
    int32_t matrix_to_code[ND_KEYMAP_MAX_ROWS][ND_KEYMAP_MAX_COLS];
    int i2c_bus;
    int i2c_addr;
} nd_keymap;

/* ND_ERR_NOTFOUND means "no matrix keypad on this device", which is the normal
 * case in QEMU and is not an error. Every other failure logs the same
 * rejection message the Python printed. */
nd_err nd_keymap_load(const char *path, nd_keymap *out);

/* ------------------------------------------------------------------ *
 * The uinput bridge (LinuxShell and the Browser type through it)
 * ------------------------------------------------------------------ */

typedef struct {
    int fd;
    bool owns_device;
} nd_uinput_kbd;

nd_err nd_uinput_open(nd_uinput_kbd *k, const char *path, const char *name);
nd_err nd_uinput_attach(nd_uinput_kbd *k, int fd); /* tests: no ioctls issued */
nd_err nd_uinput_send_key(nd_uinput_kbd *k, uint16_t code, bool shift);
bool nd_uinput_type_char(nd_uinput_kbd *k, char c); /* false = untypeable */
nd_err nd_uinput_backspace(nd_uinput_kbd *k);
void nd_uinput_close(nd_uinput_kbd *k);
bool nd_uinput_char_to_keypress(char c, uint16_t *code, bool *shift);

#ifdef __cplusplus
}
#endif

#endif /* ND_INPUT_H_INCLUDED */
