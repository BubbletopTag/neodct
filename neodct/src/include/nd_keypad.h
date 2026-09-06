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

/* WHERE A FAILURE HAPPENED. An i2c keypad that does not work fails in one of
 * four places and the repair is different for each: open() says the udev
 * group has not landed (or the bus does not exist), the I2C_SLAVE ioctl says
 * the kernel driver is unhappy, and a failed write or read says the wires or
 * the expander itself. For two releases every one of them reached the owner
 * as the same sentence -- "a permission or wiring problem" -- which names two
 * of the four and leaves the reader to guess. The stage and the errno below
 * are what turn that sentence into the true one. */
typedef enum {
    ND_PCF_STAGE_NONE = 0, /* nothing has failed on this chip yet          */
    ND_PCF_STAGE_OPEN,     /* open("/dev/i2c-N")                          */
    ND_PCF_STAGE_SLAVE,    /* ioctl(I2C_SLAVE)                            */
    ND_PCF_STAGE_WRITE,    /* a two-byte write -- the chip did not ACK    */
    ND_PCF_STAGE_READ      /* a two-byte read                             */
} nd_pcf8575_stage;

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
    /* The last failure, kept so a caller that only sees an nd_err can still
     * say what the kernel said. Deliberately NOT cleared by a later success:
     * nd_matrix_input_open() memsets its whole struct on the way in, so what
     * survives a FAILED open is that open's own errno and nothing older. */
    int last_errno;
    nd_pcf8575_stage last_stage;
    /* One log line per failure BURST, not per transfer. A dead bus is
     * rescanned two hundred times a second; logging each failed transfer
     * fills the serial console faster than it can be read and hides the
     * first line, which is the only one that says when it started. Cleared
     * by the next transfer that works, so a bus that dies twice says so
     * twice. */
    bool io_error_logged;
    char dev_path[32];
} nd_pcf8575;

nd_err nd_pcf8575_open(nd_pcf8575 *c, int bus, int addr);

/* Wrap an already-open descriptor. No open(), no ioctl -- see the header
 * comment. The caller keeps ownership of fd; nd_pcf8575_close() will not
 * close it, but it still writes the 0xFFFF release word first. */
nd_err nd_pcf8575_attach(nd_pcf8575 *c, int fd);

/* nd_pcf8575_attach() for a descriptor that is a REAL bus rather than a test
 * socketpair: the caller has already opened /dev/i2c-<bus> and already issued
 * ioctl(I2C_SLAVE, addr) on it, and passes both so that every log line and
 * every on-screen reason still names the device instead of "<attached>".
 *
 * This is how the keypad crosses the privilege drop. I2C_SLAVE is per-
 * DESCRIPTOR state and open() checks permission only at open() time, so a
 * descriptor root opened before nd-core became ndusr keeps working
 * afterwards -- which takes the udev race out of the keypad's boot entirely.
 * See nd_kpsetup_open_keypad_as_root(). */
nd_err nd_pcf8575_adopt(nd_pcf8575 *c, int fd, int bus, int addr);

/* Hand the descriptor to the caller and forget it: returns the fd (or -1) and
 * leaves the chip closed WITHOUT closing or releasing anything. The root-
 * phase opener uses it to keep the validated descriptor after the nd_pcf8575
 * it validated with has served its purpose. */
int nd_pcf8575_detach(nd_pcf8575 *c);

/* Low byte first, as the chip latches it. */
nd_err nd_pcf8575_write16(nd_pcf8575 *c, uint16_t value);
nd_err nd_pcf8575_read16(nd_pcf8575 *c, uint16_t *out);

/* "open", "I2C_SLAVE", "write", "read" or "" -- for a message a person reads.
 * A string literal; never NULL. */
const char *nd_pcf8575_stage_name(nd_pcf8575_stage stage);

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

/* Same, over an already-open descriptor. Tests, and the root-phase keypad
 * bring-up -- see nd_matrix_scanner_adopt() for the difference. */
nd_err nd_matrix_scanner_init_fd(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                                 const uint8_t *col_pins, size_t n_cols, int fd);

/* nd_matrix_scanner_init_fd() over a descriptor that is a real /dev/i2c-<bus>
 * already pointed at `addr`. Identical in every way except that the chip
 * remembers what it is, so its log lines and its failure reasons name the bus
 * instead of "<attached>". */
nd_err nd_matrix_scanner_adopt(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                               const uint8_t *col_pins, size_t n_cols, int fd, int bus, int addr);

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

/* Consecutive failed scans after which the bus counts as DEAD rather than
 * merely glitching. At the read loop's 5 ms cadence twenty scans is about a
 * tenth of a second of solid failure -- far longer than the odd arbitration
 * loss the rk3x controller reports when the fuel gauge on the same bus is
 * being read, and far shorter than a person can notice.
 *
 * The number matters because of what happens on either side of it. Below it
 * the failure is absorbed, because a keypad that drops one scan out of a
 * thousand is a working keypad. At it the matrix is TORN DOWN, which is the
 * only thing that re-arms the reopen path and the only thing that lets the
 * core tell the owner the keypad stopped answering. Before this existed a bus
 * that died after open left the phone with a backend on paper, no keys, no
 * log line and no screen -- silent, which is the worst of the three. */
#define ND_MATRIX_DEAD_SCANS 20

typedef struct {
    nd_matrix_scanner scanner;
    nd_keymap cfg;
    bool have_last_unmapped;
    nd_matrix_pos last_unmapped;
    /* Consecutive nd_matrix_scan_once() failures; reset by any scan that
     * works. See ND_MATRIX_DEAD_SCANS. */
    uint32_t scan_errors;
} nd_matrix_input;

nd_err nd_matrix_input_open(nd_matrix_input *in, const nd_keymap *cfg);

/* Tests: the same thing over a descriptor the caller supplies. */
nd_err nd_matrix_input_open_fd(nd_matrix_input *in, const nd_keymap *cfg, int fd);

/* ND_KEY_NONE when nothing was pressed inside timeout_s. Scans AT LEAST ONCE
 * even at timeout 0 -- the Browser's drain path depends on read_key(0) still
 * consuming a queued press. */
int32_t nd_matrix_input_read_key(nd_matrix_input *in, double timeout_s);

/* Exactly one scan, no waiting and no sleeping. nd_input drives its own
 * timing across two backends and cannot afford read_key's internal loop.
 *
 * ND_KEY_NONE means "no key" AND "the scan failed" -- the two are genuinely
 * indistinguishable in one int32_t, which is why the failure is counted
 * inside instead. Ask nd_matrix_input_bus_dead() after polling. */
int32_t nd_matrix_input_poll(nd_matrix_input *in);

/* True once ND_MATRIX_DEAD_SCANS consecutive scans have failed: this input is
 * no longer a keypad and the caller must close it, stop counting it as a
 * backend and start trying to open it again. False for a healthy bus and for
 * one that has merely glitched. */
bool nd_matrix_input_bus_dead(const nd_matrix_input *in);

/* What the kernel said about the last failure on this input's bus, and where
 * it happened. Both survive a FAILED nd_matrix_input_open*(), which memsets
 * the struct on the way in -- so the caller of a failed open can name the
 * real reason instead of guessing between permission and wiring. Zero and
 * ND_PCF_STAGE_NONE when nothing has failed. */
int nd_matrix_input_last_errno(const nd_matrix_input *in);
nd_pcf8575_stage nd_matrix_input_last_stage(const nd_matrix_input *in);

/* The device this input is scanning ("/dev/i2c-3"), for a message. Never
 * NULL; "" when there is none. */
const char *nd_matrix_input_dev(const nd_matrix_input *in);

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
 * Bringing the keypad up, and keeping it up
 * ------------------------------------------------------------------ */

/* ============ WHY A PROCESS-WIDE DESCRIPTOR ============
 *
 * nd-core opens its input from inside nd_ui_init(), which runs long after the
 * privilege drop -- so the keypad's /dev/i2c-N had to be group-reachable to
 * ndusr AT THAT INSTANT, and on a cold Luckfox it often is not yet. Three
 * releases of retries did not fix it because the retry is on the wrong side
 * of the drop.
 *
 * The fix is to open the bus while nd-core is still root and never let go:
 * permission is checked at open(2) and nowhere else, and the I2C_SLAVE
 * address is per-descriptor state, so a descriptor root opened and root
 * pointed at the expander keeps working after setuid. nd_main.c does that
 * before step 4b and hands the descriptor here; nd_input_open() prefers it
 * over opening the node itself, and the udev race stops being able to reach
 * the keypad at all.
 *
 * It is process-wide rather than a parameter because the call that consumes
 * it is nd_ui_init()'s nd_input_open(), three modules and one work package
 * away from the boot sequence that produces it -- and because it IS a
 * process-wide fact: this process opened the keypad bus once, as root, before
 * it stopped being root. Passing -1 forgets it.
 *
 * nd_input_close() does NOT close the descriptor. It outlives every nd_input
 * built over it, which is what lets the matrix be torn down and rebuilt when
 * the bus dies without ever needing the node's group again. */
void nd_input_provide_keypad_fd(int fd, int bus, int addr);

/* Ask, right now, whether a backend can be opened that could not be before --
 * the i2c matrix first, then an evdev device. True when there is one after
 * the attempt. Unlike the retry the read path makes, this ignores the once-a-
 * second rate limit and does not spend the bounded reopen budget, because its
 * caller is the boot-time grace window rather than a loop that runs forever.
 *
 * This exists because the retry added in 0.5.7b could never run on the only
 * phone that needed it: it lived in nd_input_read_event(), and a core with no
 * backend at all never reaches its read loop -- it goes to the input-failure
 * screen first. The recovery has to be callable from where the decision is
 * made, not only from where the keys are read. */
bool nd_input_retry_backend(nd_input *in);

/* ------------------------------------------------------------------ *
 * Classifying a failure to open the keypad -- the pure decision
 * ------------------------------------------------------------------ */

typedef enum {
    ND_INPUT_FAIL_NONE = 0,  /* it opened                                   */
    ND_INPUT_FAIL_TRANSIENT, /* try again: the udev race, a NAK, a late bus */
    ND_INPUT_FAIL_PERMANENT  /* nothing will change: the keymap is wrong    */
} nd_input_fail;

/* Is this errno one that a later attempt could get past?
 *
 * The distinction is the whole of the "keypad error screen on some boots"
 * bug. A phone whose expander NAKed once while its rail was still rising, or
 * whose i2c node was still root:root when the UI looked, is a phone with a
 * perfectly good keypad -- and for three releases both were reported as a
 * permanent absence. Everything a cold boot can produce is transient here;
 * only errors that describe the SOFTWARE's request being wrong are not. */
bool nd_input_errno_is_transient(int err);

/* The same question about a whole open attempt: an nd_err from
 * nd_matrix_input_open*() plus the errno underneath it. ND_ERR_INVAL and
 * ND_ERR_TOOLONG come from validate_pins() and from a keymap whose numbers do
 * not fit -- a file that will fail identically for ever, so they are
 * permanent no matter what errno says. */
nd_input_fail nd_input_classify_open_failure(nd_err rc, int err);

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
