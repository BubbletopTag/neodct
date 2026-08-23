/* nd_keypad.h -- the hardware keypad layer beneath nd_input.
 *
 * nd_input.h is the frozen contract every app and widget is written against:
 * open a source of keys, read presses and releases, ask what is held. It
 * deliberately says nothing about how the keys are produced. This header is
 * the layer underneath -- the PCF8575 expander, the matrix scanner that runs
 * on top of it, the keymap writer, and the tuning knobs for the auto-repeat
 * that nd_input synthesises.
 *
 * It exists as a separate file rather than as edits to nd_input.h because
 * nd_input.h is frozen and other modules are compiling against it right now.
 * Nothing here changes a declaration there; it only adds.
 *
 * ============ WHO NEEDS WHAT ============
 *
 *   nd_input.c          all of it
 *   the first-boot wizard (WP-27)   nd_pcf8575_*, nd_matrix_*, nd_keymap_save
 *   Koki (WP-47)        nd_input_is_held() from nd_input.h -- NOT this header.
 *                       The whole point of the settled cross-process decision
 *                       is that no app ever touches the i2c bus again.
 *   the unit tests      nd_pcf8575_attach(), which is the fake-chip hook
 *
 * ============ THE FAKE-CHIP HOOK ============
 *
 * nd_pcf8575_attach() takes an already-open descriptor and skips both the
 * open() and the I2C_SLAVE ioctl. A test hands it one end of a socketpair,
 * seeds the other end with the 16-bit words a real chip would return, and
 * then checks the row-drive words the scanner wrote. That fakes THE CHIP, not
 * the driver -- the whole scan path including the bit arithmetic is the code
 * that actually ships. It mirrors t9_uinput.py's `fd=` injection, which is
 * how the Python's own uinput tests work.
 */

#ifndef ND_KEYPAD_H_INCLUDED
#define ND_KEYPAD_H_INCLUDED

#include "nd_input.h"
#include "nd_t9.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The PCF8575 i2c expander
 * ------------------------------------------------------------------ */

#define ND_I2C_SLAVE        0x0703 /* linux/i2c-dev.h                       */
#define ND_I2C_BUS_DEFAULT  3
#define ND_I2C_ADDR_DEFAULT 0x20

/* The chip is quasi-bidirectional and has no direction register and no
 * command byte: writing 1 releases a pin to its weak internal pull-up (it
 * then reads high), writing 0 drives it hard low. A plain two-byte write()
 * and a plain two-byte read() after one I2C_SLAVE ioctl ARE the correct raw
 * transactions -- there is nothing missing here. */
typedef struct {
    int fd; /* -1 when closed */
    int bus;
    int addr;
    bool owns_fd; /* false for an attached (test) descriptor */
    char dev_path[32];
} nd_pcf8575;

nd_err nd_pcf8575_open(nd_pcf8575 *c, int bus, int addr);

/* Wrap an already-open descriptor. No open(), no ioctl -- see the header
 * comment. The caller keeps ownership of fd; nd_pcf8575_close() will not
 * close it, but it still writes the 0xFFFF release word first. */
nd_err nd_pcf8575_attach(nd_pcf8575 *c, int fd);

/* Low byte first, as the chip latches it. */
nd_err nd_pcf8575_write16(nd_pcf8575 *c, uint16_t value);
nd_err nd_pcf8575_read16(nd_pcf8575 *c, uint16_t *out);

/* Releases every pin (0xFFFF) before closing, so nothing is left driven low
 * across a restart. A failure of that write is ignored, exactly as the
 * Python ignores the OSError. */
void nd_pcf8575_close(nd_pcf8575 *c);

/* ------------------------------------------------------------------ *
 * The matrix scanner
 * ------------------------------------------------------------------ */

#define ND_MATRIX_MAX_PINS 16 /* the expander has sixteen                   */

/* Consecutive scans a key must be missing from before it counts as released.
 * At the ~5 ms poll cadence of the read loop that is about 15 ms of debounce,
 * which is what mushy membrane contacts need. */
#define ND_RELEASE_SCANS 3

#define ND_SCAN_SETTLE_US 500  /* after driving a row low, before reading   */
#define ND_READ_POLL_US   5000 /* between scans inside read_key             */

typedef struct {
    uint8_t row;
    uint8_t col;
} nd_matrix_pos;

/* _held and _pending are flat arrays, not a hash map and not a list: at most
 * 16x16 positions exist, so the whole state is under a kilobyte and the scan
 * path allocates nothing. */
typedef struct {
    nd_pcf8575 chip;
    uint8_t row_pins[ND_MATRIX_MAX_PINS];
    size_t n_rows;
    uint8_t col_pins[ND_MATRIX_MAX_PINS];
    size_t n_cols;
    /* -1 = not held, otherwise the number of consecutive scans it has been
     * missing from. Indexed [row][col], which is why iterating it row-major
     * yields positions in the ascending order the Python sorts them into. */
    int8_t held[ND_MATRIX_MAX_PINS][ND_MATRIX_MAX_PINS];
    nd_matrix_pos pending[ND_MATRIX_MAX_PINS * ND_MATRIX_MAX_PINS];
    size_t pending_head;
    size_t pending_len;
} nd_matrix_scanner;

/* Every pin must be 0..15 and must not repeat, across rows and columns
 * together. Drives 0xFFFF once on the way in. */
nd_err nd_matrix_scanner_init(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                              const uint8_t *col_pins, size_t n_cols, int bus, int addr);

/* Same, over an already-open descriptor. Tests only. */
nd_err nd_matrix_scanner_init_fd(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                                 const uint8_t *col_pins, size_t n_cols, int fd);

/* One full pass over every row -- never stopping at the first hit, because
 * that is what gives key rollover and games miss direction changes without
 * it. Sets *found and *out for at most one NEW press; simultaneous presses
 * are queued and drip out of later calls, one per call.
 *
 * Returns ND_ERR_IO if the bus failed, in which case *found is false. */
nd_err nd_matrix_scan_once(nd_matrix_scanner *s, nd_matrix_pos *out, bool *found);

/* The debounced held set. This is the state Koki used to reach into the
 * Python scanner for; nd_input derives its release edges from it. */
size_t nd_matrix_held(const nd_matrix_scanner *s, nd_matrix_pos *out, size_t max);
bool nd_matrix_is_held(const nd_matrix_scanner *s, uint8_t row, uint8_t col);

void nd_matrix_scanner_close(nd_matrix_scanner *s);

/* ------------------------------------------------------------------ *
 * The keymapped matrix input backend
 * ------------------------------------------------------------------ */

typedef struct {
    nd_matrix_scanner scanner;
    nd_keymap cfg;
    bool have_last_unmapped;
    nd_matrix_pos last_unmapped;
} nd_matrix_input;

nd_err nd_matrix_input_open(nd_matrix_input *in, const nd_keymap *cfg);

/* Tests: the same thing over a descriptor the caller supplies. */
nd_err nd_matrix_input_open_fd(nd_matrix_input *in, const nd_keymap *cfg, int fd);

/* ND_KEY_NONE when nothing was pressed inside timeout_s. Scans AT LEAST ONCE
 * even at timeout 0 -- the Browser's drain path depends on read_key(0) still
 * consuming a queued press. */
int32_t nd_matrix_input_read_key(nd_matrix_input *in, double timeout_s);

/* Exactly one scan, no waiting and no sleeping. nd_input drives its own
 * timing across two backends and cannot afford read_key's internal loop. */
int32_t nd_matrix_input_poll(nd_matrix_input *in);

void nd_matrix_input_close(nd_matrix_input *in);

/* ------------------------------------------------------------------ *
 * Writing /NeoDCT/User/keymap.json
 * ------------------------------------------------------------------ */

/* json.dump(payload, indent=2, sort_keys=True) plus a trailing newline,
 * written to path + ".tmp", fsync'd, then renamed. The atomicity is not
 * decoration: this file lands on the only writable partition and a torn write
 * leaves a phone whose only input device does not work. */
nd_err nd_keymap_save(const nd_keymap *km, const char *path);

/* ------------------------------------------------------------------ *
 * Auto-repeat
 * ------------------------------------------------------------------ */
/*
 * The core synthesises repeats from its own held state rather than passing
 * the kernel's EV_KEY value 2 through, because the i2c matrix has no such
 * thing -- it reports edges and nothing else. Doing it in one place means an
 * app behaves the same on the phone and under QEMU.
 *
 * DEFAULTS AND WHY:
 *
 *   delay 400 ms, then one repeat every 120 ms.
 *
 *   400 ms because a deliberate single press is 150-250 ms of contact; below
 *   about 350 ms ordinary typing starts producing a second character. It is
 *   also comfortably shorter than the T9 multi-tap window (1.0 s), so the two
 *   never race for the same key.
 *
 *   120 ms because the UI polls input on a 100 ms tick (read_keypress's
 *   default timeout, which every blocking widget inherits). A repeat interval
 *   below that cannot be observed -- the events pile up and the list jumps by
 *   several rows at once. 120 ms sits just above the tick, giving a steady
 *   ~8 rows a second that a person can stop on the row they wanted.
 *
 *   ONLY THE FOUR ARROW KEYS REPEAT BY DEFAULT. That is the whole of what the
 *   owner asked for -- list scrolling and NetSurf navigation -- and it is the
 *   only set that is safe: a repeat on a digit would cycle T9 multi-tap
 *   letters behind the user's back, and a repeat on Enter would open whatever
 *   the list landed on. An app that wants more asks for it.
 */

#define ND_REPEAT_DELAY_S    0.400
#define ND_REPEAT_INTERVAL_S 0.120

/* Both in seconds. Either <= 0 disables repeat entirely. */
void nd_input_set_repeat(nd_input *in, double delay_s, double interval_s);

/* Which codes repeat. Pass NULL to restore the four arrows; pass a zero count
 * to stop everything repeating. At most ND_INPUT_HELD_MAX codes. */
nd_err nd_input_set_repeat_codes(nd_input *in, const int32_t *codes, size_t n);

/* True when this event was synthesised by the repeat timer rather than read
 * from a device. Presses only; a repeat never produces a release. */
bool nd_input_last_was_repeat(const nd_input *in);

/* ------------------------------------------------------------------ *
 * The T9 bridges, without a thread
 * ------------------------------------------------------------------ */
/*
 * nd_t9_bridge_start() needs a real matrix keypad and spawns a thread, so a
 * host test cannot reach nd_t9_bridge_handle_code() through it. These two
 * build the same object with no input source and no thread, which is how the
 * 27 cases in test_t9_uinput.py port across.
 */
nd_t9_bridge *nd_t9_bridge_new_for_test(nd_bridge_kind kind, nd_uinput_kbd *kbd);
void nd_t9_bridge_free_for_test(nd_t9_bridge *b);

/* "nav" while the browser bridge is in cursor mode, otherwise the engine's
 * own label. This is what the browser chrome shows. */
const char *nd_t9_bridge_mode_label(const nd_t9_bridge *b);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEYPAD_H_INCLUDED */
