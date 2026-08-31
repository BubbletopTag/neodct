/* apps/KeypadMapperI2C/main.c -- press each of the sixteen keys once, and the
 * phone learns which wire crossing each one is.
 *
 * A one-to-one port of System/engineering/apps/KeypadMapperI2C/main.py. App
 * id 9003, engineering menu, manifest name "KeyMapI2C". It drives the PCF8575
 * expander directly through nd_matrix_scan_once(), asks for the sixteen keys
 * in KEY_TARGETS order, and writes /NeoDCT/User/keymap.json -- the file
 * lib/nd_keymap.c reads back on every boot to decide whether this phone has a
 * matrix keypad at all.
 *
 * Its GPIO sibling, apps/KeypadMapper, is two dialogs and a gate; the header
 * comment there says why. This one is the enrolment path that actually works
 * on the device, so everything is here.
 *
 * ============ THE PROMPT CLIPS, AND THAT IS THE FRAME ============
 *
 * _draw_capture_prompt builds SIX body lines and draws FOUR. y runs
 * 56, 75, 94, 113, and the fifth would be at 132, which is past the
 * `y > content_bottom - line_h` cut-off of 126. So:
 *
 *     y=56    Press: NaviKey
 *     y=75    (blank)
 *     y=94    Capture one keypad
 *     y=113   button now.
 *
 * and "Menu key cancels.", the bus line and the pin line are computed on
 * every redraw and never reach the panel. The break exits only the INNER
 * loop, but the next body entry's first line trips the same test immediately,
 * so the outer loop finishes doing nothing. Someone porting from the intent
 * rather than the code will "fix" this; it is on the reference frame and it
 * stays.
 *
 * ============ THE ONE THING THIS PORT CANNOT DO ============
 *
 * main.py's run() contains:
 *
 *     suspended_input = getattr(self.ui, "matrix_input", None)
 *     self.ui.matrix_input = None                  # ... and restores it later
 *
 * with the comment "Suspend the UI's own input backend during capture so it
 * cannot scan the PCF8575 at the same time as the scanner." In Python the app
 * was exec_module()d into the core's own process, so reaching into the UI
 * object was enough. In C the app is a separate process (nd_app.h): the core
 * keeps its matrix backend open and nd_proc_launch_app()'s pump_keys() keeps
 * polling it at 20 Hz for the whole life of the child. An app cannot write to
 * the core's state and there is no message that asks it to stop.
 *
 * Two consequences, both real, and neither of them something this file can
 * fix from inside apps/:
 *
 *   1. On a phone that ALREADY has a working keymap, the core's scan and this
 *      app's scan interleave on the same bus. Each transaction is atomic, so
 *      nothing is corrupted, but "drive row 2 low" from one process can land
 *      between "drive row 0 low" and "read" in the other, and the read then
 *      describes the wrong row. Expect missed and phantom presses. First
 *      enrolment -- the case this app exists for -- is unaffected, because
 *      without a keymap the core has no matrix backend to run.
 *   2. The MENU-key cancel WORKS here, where in the Python it did not.
 *      ui.read_keypress() fell through to evdev, which on keypad-only
 *      hardware does not exist, so the Python's operator had to finish all
 *      sixteen captures or pull the battery. Here the key arrives down the
 *      inherited channel the core is still feeding. That is a behaviour
 *      change and it comes from the process boundary, not from a decision
 *      taken in this file.
 *
 * The fix for (1) is in the core, not here: nd_proc_launch_app() would have
 * to suspend ui->input's matrix backend for the child's lifetime. That is a
 * lib/ change and it is reported rather than made.
 *
 * ============ THE ONE DELIBERATE DIFFERENCE ============
 *
 * _save_keymap does os.makedirs() and then a plain open()/json.dump()/write.
 * kmi2c_save() writes to "<path>.tmp", fsync()s, and renames. The bytes that
 * end up at the path are identical; what changes is that a power cut during
 * the write can no longer leave half a keymap behind. nd_keypad.h states the
 * requirement for the same file in as many words -- "The atomicity is not
 * decoration: this file lands on the only writable partition and a torn write
 * leaves a phone whose only input device does not work" -- and libneodct's
 * own nd_keymap_save() already honours it. Diverging from that here to
 * reproduce a non-atomic write would be reproducing an accident.
 *
 * nd_keymap_save() itself is not used, and could not be: it writes the
 * first-boot wizard's payload, which has no "generated_at_unix" and no
 * per-key "label", and it always emits the i2c fields. This app's payload is
 * main.py's, field for field.
 */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "keypadmapper_i2c.h"

/* main.py prints "[KEYMAP-I2C] ...". nd_log.h has no constant for it -- the
 * tag is not in the named palette -- so the derived-colour path gives it a
 * stable colour of its own, which is exactly what that path is for. */
#define KMI2C_TAG "KEYMAP-I2C"

/* _wait_for_matrix_press's two 0.01 s waits. */
#define KMI2C_POLL_S 0.01

/* ------------------------------------------------------------------ *
 * The strings, verbatim
 * ------------------------------------------------------------------ */

/* OUTPUT_PATH. Spelled through nd_paths.h so that a future move of the user
 * partition moves this too; the two strings are the same today. */
const char *const nd_kmi2c_output_path = ND_PATH_KEYMAP;

const char *const nd_kmi2c_i2c_required_msg =
    "This app requires I2C. No /dev/i2c-* devices found. This application can not run in QEMU.";

const char *const nd_kmi2c_intro_msg =
    "Captures PCF8575 keypad presses to /NeoDCT/User/keymap.json.";

const char *const nd_kmi2c_cancel_msg = "Calibration canceled. Keymap not saved.";

const char *const nd_kmi2c_title = "Keypad Mapper I2C";

const char *const nd_kmi2c_format = "neodct.keymap.v3.matrix.i2c";
const char *const nd_kmi2c_driver = "pcf8575-i2c";

const char *const nd_kmi2c_softkey_text = "Capture";

/* KEY_TARGETS, in capture order. NaviKey first because it is the one key an
 * operator can always find, and the digits in reading order after it. */
const nd_kmi2c_target nd_kmi2c_targets[ND_KMI2C_N_TARGETS] = {
    {"navikey", "NaviKey"}, {"clear", "C"}, {"up", "Up"},   {"down", "Down"},
    {"num_1", "1"},         {"num_2", "2"}, {"num_3", "3"}, {"num_4", "4"},
    {"num_5", "5"},         {"num_6", "6"}, {"num_7", "7"}, {"num_8", "8"},
    {"num_9", "9"},         {"num_0", "0"}, {"star", "*"},  {"hash", "#"},
};

const int32_t nd_kmi2c_default_rows[ND_KMI2C_N_DEFAULT_PINS] = {0, 1, 2, 3};
const int32_t nd_kmi2c_default_cols[ND_KMI2C_N_DEFAULT_PINS] = {4, 5, 6, 7};

/* ------------------------------------------------------------------ *
 * Small shared helpers
 * ------------------------------------------------------------------ */

/* time.sleep(). Skipped while the virtual clock is running: under capture,
 * time is a frame counter (nd_vclock.h) and a real sleep moves no pixel. */
static void dwell(double seconds)
{
    struct timespec req;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &req) != 0)
        break; /* EINTR only; anything else would spin */
}

/* Python's `//` floors toward negative infinity and C's `/` truncates toward
 * zero. They disagree for odd negatives, which is exactly the case that
 * happens here: the title is 238 px wide against a 240 px panel today, and a
 * one-pixel-wider face would put (240 - 241) // 2 at -1 in Python and 0 in C. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? v / 2 : -(((-v) + 1) / 2);
}

/* str.split()'s idea of whitespace, restricted to ASCII and written out so no
 * locale can widen or narrow it. */
static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* text.strip() into a caller buffer. Returns false only on truncation, which
 * for an environment variable this long means it was never a pin list. */
static bool strip_into(char *out, size_t out_sz, const char *raw)
{
    size_t begin = 0u;
    size_t end;
    size_t len;

    if (raw == NULL)
        raw = ""; /* Python's `(raw or "")` */
    end = strlen(raw);
    while (begin < end && is_space(raw[begin]))
        begin++;
    while (end > begin && is_space(raw[end - 1u]))
        end--;
    len = end - begin;
    if (len >= out_sz)
        return false;
    memcpy(out, raw + begin, len);
    out[len] = '\0';
    return true;
}

/* format(value, "02X"), which for a negative int is a '-' in front of the
 * magnitude rather than C's two's-complement spelling. Only 0x20-shaped
 * values can reach the panel -- the scanner has already accepted the address
 * by then -- but nd_kmi2c_body() is exported and a test may say otherwise. */
static void hex02(char *out, size_t out_sz, int32_t value)
{
    if (value < 0) {
        /* -(int64) first: negating INT32_MIN as an int32 is undefined. */
        (void)nd_snprintf(out, out_sz, "-%02llX", (unsigned long long)(-(int64_t)value));
        return;
    }
    (void)nd_snprintf(out, out_sz, "%02llX", (unsigned long long)value);
}

/* ------------------------------------------------------------------ *
 * int(), the way Python spells its failures
 * ------------------------------------------------------------------ */

static void int_error(char *err, size_t err_sz, int base, const char *text)
{
    /* CPython's ValueError, with repr() around the offending text. repr()
     * switches to double quotes for a string containing an apostrophe; a pin
     * list never does, and the single-quote form is what an operator has
     * always seen on the console. */
    (void)nd_snprintf(err, err_sz, "invalid literal for int() with base %d: '%s'", base, text);
}

/* int(text, base) over an already-stripped string.
 *
 * Two differences from CPython, both unreachable from an environment variable
 * that was meant as a pin list, and both recorded rather than papered over:
 * underscore separators ("1_0" is 10 in Python 3.6+) are refused here, and a
 * value too large for int32 saturates instead of becoming a bignum. The
 * saturated value is still out of the scanner's 0..15 range, so the operator
 * gets the same "out of range 0-15" dialog either way -- just with a
 * different number in it. */
static bool py_int(const char *text, int base, int32_t *out, char *err, size_t err_sz)
{
    char *end = NULL;
    long long v;

    if (text == NULL || text[0] == '\0') {
        int_error(err, err_sz, base, text != NULL ? text : "");
        return false;
    }

    errno = 0;
    v = strtoll(text, &end, base);
    if (end == text || *end != '\0') {
        int_error(err, err_sz, base, text);
        return false;
    }
    if (v > (long long)INT32_MAX)
        v = (long long)INT32_MAX;
    else if (v < (long long)INT32_MIN)
        v = (long long)INT32_MIN;

    *out = (int32_t)v;
    return true;
}

/* ------------------------------------------------------------------ *
 * _parse_pins / _parse_addr / the bus
 * ------------------------------------------------------------------ */

static void copy_fallback(const int32_t *fallback, size_t n_fallback, int32_t *out, size_t max,
                          size_t *n_out)
{
    size_t n = (n_fallback < max) ? n_fallback : max;

    memcpy(out, fallback, n * sizeof out[0]);
    *n_out = n;
}

bool nd_kmi2c_parse_pins(const char *raw, const int32_t *fallback, size_t n_fallback, int32_t *out,
                         size_t max, size_t *n_out, char *err, size_t err_sz)
{
    /* Long enough for sixteen signed 32-bit pins with separators, which is
     * more than any real override and far more than the scanner accepts. */
    char text[ND_KMI2C_REPR_MAX];
    char chunk[ND_KMI2C_REPR_MAX];
    const char *p;
    size_t n = 0u;

    if (out == NULL || n_out == NULL || max == 0u)
        return false;

    if (!strip_into(text, sizeof text, raw)) {
        (void)nd_strlcpy(err, "keypad pin override is too long", err_sz);
        return false;
    }
    if (text[0] == '\0') {
        copy_fallback(fallback, n_fallback, out, max, n_out);
        return true;
    }

    for (p = text;;) {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        int32_t pin = 0;
        char piece[ND_KMI2C_REPR_MAX];

        if (len >= sizeof piece) {
            (void)nd_strlcpy(err, "keypad pin override is too long", err_sz);
            return false;
        }
        memcpy(piece, p, len);
        piece[len] = '\0';
        (void)strip_into(chunk, sizeof chunk, piece);

        /* `if not chunk: continue` -- ",," and " , " are not errors. */
        if (chunk[0] != '\0') {
            if (!py_int(chunk, 10, &pin, err, err_sz))
                return false;
            if (n >= max) {
                /* The Python has no ceiling here and lets a 17-pin list reach
                 * _validate_pins, which always refuses it: seventeen pins
                 * cannot all be distinct in 0..15. The outcome is the same
                 * "Failed to write keymap:" dialog either way; only the
                 * sentence inside it differs. */
                (void)nd_snprintf(err, err_sz, "expander pin list is longer than %u",
                                  (unsigned)max);
                return false;
            }
            out[n++] = pin;
        }

        if (comma == NULL)
            break;
        p = comma + 1;
    }

    /* `return out if out else list(fallback)`. */
    if (n == 0u) {
        copy_fallback(fallback, n_fallback, out, max, n_out);
        return true;
    }
    *n_out = n;
    return true;
}

bool nd_kmi2c_parse_addr(const char *raw, int32_t fallback, int32_t *out, char *err, size_t err_sz)
{
    char text[ND_KMI2C_REPR_MAX];
    bool hex;

    if (out == NULL)
        return false;
    if (!strip_into(text, sizeof text, raw)) {
        (void)nd_strlcpy(err, "i2c address override is too long", err_sz);
        return false;
    }
    if (text[0] == '\0') {
        *out = fallback;
        return true;
    }

    /* text.lower().startswith("0x"). A leading '-' or '+' therefore does NOT
     * qualify, so int("-0x20") is attempted in base 10 and raises -- which is
     * what happens here too. */
    hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    return py_int(text, hex ? 16 : 10, out, err, err_sz);
}

bool nd_kmi2c_parse_bus(const char *raw, int32_t fallback, int32_t *out, char *err, size_t err_sz)
{
    char text[ND_KMI2C_REPR_MAX];

    if (out == NULL)
        return false;
    if (!strip_into(text, sizeof text, raw)) {
        (void)nd_strlcpy(err, "i2c bus override is too long", err_sz);
        return false;
    }
    /* int(<stripped> or DEFAULT_BUS): the empty string is falsy, so the
     * default is passed to int() as an int and comes straight back. */
    if (text[0] == '\0') {
        *out = fallback;
        return true;
    }
    return py_int(text, 10, out, err, err_sz);
}

static void config_defaults(nd_kmi2c_config *out)
{
    memset(out, 0, sizeof *out);
    copy_fallback(nd_kmi2c_default_rows, ND_KMI2C_N_DEFAULT_PINS, out->row_pins, ND_KMI2C_MAX_PINS,
                  &out->n_rows);
    copy_fallback(nd_kmi2c_default_cols, ND_KMI2C_N_DEFAULT_PINS, out->col_pins, ND_KMI2C_MAX_PINS,
                  &out->n_cols);
    out->bus = ND_I2C_BUS_DEFAULT;
    out->addr = ND_I2C_ADDR_DEFAULT;
}

void nd_kmi2c_config_from(nd_kmi2c_config *out, const char *rows, const char *cols, const char *bus,
                          const char *addr, bool log)
{
    nd_kmi2c_config cfg;
    char err[ND_KMI2C_ERR_MAX];
    bool ok;

    if (out == NULL)
        return;

    memset(&cfg, 0, sizeof cfg);
    err[0] = '\0';

    /* One try block around all four, so the FIRST failure throws away
     * whatever the earlier three had already produced. && short-circuits in
     * the same order the Python evaluates. */
    ok = nd_kmi2c_parse_pins(rows, nd_kmi2c_default_rows, ND_KMI2C_N_DEFAULT_PINS, cfg.row_pins,
                             ND_KMI2C_MAX_PINS, &cfg.n_rows, err, sizeof err) &&
         nd_kmi2c_parse_pins(cols, nd_kmi2c_default_cols, ND_KMI2C_N_DEFAULT_PINS, cfg.col_pins,
                             ND_KMI2C_MAX_PINS, &cfg.n_cols, err, sizeof err) &&
         nd_kmi2c_parse_bus(bus, ND_I2C_BUS_DEFAULT, &cfg.bus, err, sizeof err) &&
         nd_kmi2c_parse_addr(addr, ND_I2C_ADDR_DEFAULT, &cfg.addr, err, sizeof err);

    if (!ok) {
        if (log)
            nd_log(KMI2C_TAG, "Invalid override: %s; using defaults.", err);
        config_defaults(&cfg);
    }
    *out = cfg;
}

void nd_kmi2c_config_from_env(nd_kmi2c_config *out, bool log)
{
    nd_kmi2c_config_from(out, getenv(ND_KMI2C_ENV_ROWS), getenv(ND_KMI2C_ENV_COLS),
                         getenv(ND_KMI2C_ENV_BUS), getenv(ND_KMI2C_ENV_ADDR), log);
}

bool nd_kmi2c_validate_pins(const nd_kmi2c_config *cfg, char *err, size_t err_sz)
{
    bool seen[ND_KMI2C_MAX_PINS];
    size_t i;

    if (cfg == NULL)
        return false;
    memset(seen, 0, sizeof seen);

    /* `for pin in self.row_pins + self.col_pins` -- rows first, then columns,
     * against ONE shared set. A pin used as both a row and a column is the
     * "listed twice" case and it is refused. */
    for (i = 0u; i < cfg->n_rows + cfg->n_cols; i++) {
        int32_t pin = (i < cfg->n_rows) ? cfg->row_pins[i] : cfg->col_pins[i - cfg->n_rows];

        if (pin < 0 || pin >= (int32_t)ND_KMI2C_MAX_PINS) {
            (void)nd_snprintf(err, err_sz, "expander pin %d out of range 0-15", pin);
            return false;
        }
        if (seen[pin]) {
            (void)nd_snprintf(err, err_sz, "expander pin %d listed twice", pin);
            return false;
        }
        seen[pin] = true;
    }
    return true;
}

bool nd_kmi2c_i2c_available(void)
{
    char resolved[ND_PATH_MAX];
    glob_t g;
    bool any;

    /* glob.glob("/dev/i2c-*") -- through nd_path_resolve so that ND_ROOT
     * applies, which is how a host test presents a /dev with and without
     * buses. lib/nd_input.c checks the same tree the same way. */
    if (nd_path_resolve(resolved, sizeof resolved, "/dev/i2c-*") != ND_OK)
        return false;

    memset(&g, 0, sizeof g);
    if (glob(resolved, 0, NULL, &g) != 0) {
        globfree(&g);
        return false;
    }
    any = g.gl_pathc > 0u;
    globfree(&g);
    return any;
}

/* ------------------------------------------------------------------ *
 * The prompt
 * ------------------------------------------------------------------ */

nd_err nd_kmi2c_pin_repr(char *out, size_t out_sz, const int32_t *pins, size_t n)
{
    size_t i;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    out[0] = '\0';
    if (nd_strlcat(out, "[", out_sz) >= out_sz)
        return ND_ERR_TOOLONG;

    for (i = 0u; i < n; i++) {
        char num[16];

        if (i > 0u && nd_strlcat(out, ", ", out_sz) >= out_sz)
            return ND_ERR_TOOLONG;
        if (nd_snprintf(num, sizeof num, "%d", pins[i]) != ND_OK)
            return ND_ERR_TOOLONG;
        if (nd_strlcat(out, num, out_sz) >= out_sz)
            return ND_ERR_TOOLONG;
    }
    if (nd_strlcat(out, "]", out_sz) >= out_sz)
        return ND_ERR_TOOLONG;
    return ND_OK;
}

void nd_kmi2c_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w)
{
    char current[ND_TEXT_LINE_MAX];
    char cand[ND_TEXT_LINE_MAX];
    const char *p;
    bool any_word = false;

    if (out == NULL)
        return;
    nd_lines_clear(out);
    current[0] = '\0';
    if (text == NULL)
        text = ""; /* `(text or "")` */

    for (p = text; *p != '\0';) {
        char word[ND_TEXT_LINE_MAX];
        size_t len = 0u;
        int32_t w = 0;
        int32_t h = 0;

        while (*p != '\0' && is_space(*p))
            p++;
        while (*p != '\0' && !is_space(*p)) {
            /* A word longer than one full line of 14 px text is not something
             * any of the six body strings can contain -- the widest is the pin
             * line, whose longest token is a list repr of under 70 bytes. The
             * tail of a longer one is dropped rather than overrunning. */
            if (len + 1u < sizeof word)
                word[len++] = *p;
            p++;
        }
        if (len == 0u)
            break;
        word[len] = '\0';
        any_word = true;

        /* candidate = f"{current} {word}".strip() if current else word. The
         * .strip() is a no-op: current is already stripped and a word out of
         * str.split() can never be empty. */
        if (current[0] == '\0') {
            (void)nd_strlcpy(cand, word, sizeof cand);
        } else if (nd_snprintf(cand, sizeof cand, "%s %s", current, word) != ND_OK) {
            /* Longer than any line this font can draw, so treat it as not
             * fitting and let the wrap break here. */
            cand[0] = '\0';
        }

        if (cand[0] != '\0') {
            nd_text_size(f, cand, &w, &h);
        } else {
            w = max_w + 1;
        }

        if (w <= max_w) {
            (void)nd_strlcpy(current, cand, sizeof current);
        } else {
            if (current[0] != '\0')
                (void)nd_lines_push(out, current);
            /* An over-wide word is NOT broken: it goes on a line of its own
             * and overflows the margin. */
            (void)nd_strlcpy(current, word, sizeof current);
        }
    }

    if (!any_word) {
        /* `if not words: return [""]`, which is also what the caller's
         * `[""] if raw == ""` special case produces -- so that branch in
         * _draw_capture_prompt is redundant and is not reproduced. */
        (void)nd_lines_push(out, "");
        return;
    }
    if (current[0] != '\0')
        (void)nd_lines_push(out, current);
}

size_t nd_kmi2c_body(const nd_kmi2c_config *cfg, const char *label, char out[][ND_KMI2C_BODY_MAX],
                     size_t max)
{
    char rows[ND_KMI2C_REPR_MAX];
    char cols[ND_KMI2C_REPR_MAX];
    char addr[24];
    size_t n = 0u;

    if (cfg == NULL || out == NULL)
        return 0u;

#define PUSH(...)                                                  \
    do {                                                           \
        if (n >= max)                                              \
            return n;                                              \
        (void)nd_snprintf(out[n], ND_KMI2C_BODY_MAX, __VA_ARGS__); \
        n++;                                                       \
    } while (0)

    PUSH("Press: %s", label != NULL ? label : "");
    PUSH("%s", "");
    PUSH("%s", "Capture one keypad button now.");
    PUSH("%s", "Menu key cancels.");

    hex02(addr, sizeof addr, cfg->addr);
    PUSH("Bus: /dev/i2c-%d Addr: 0x%s", cfg->bus, addr);

    /* f"Rows: P{self.row_pins} Cols: P{self.col_pins}" -- the "P" prefixes
     * the whole list repr, not each pin: "Rows: P[0, 1, 2, 3]". */
    (void)nd_kmi2c_pin_repr(rows, sizeof rows, cfg->row_pins, cfg->n_rows);
    (void)nd_kmi2c_pin_repr(cols, sizeof cols, cfg->col_pins, cfg->n_cols);
    PUSH("Rows: P%s Cols: P%s", rows, cols);

#undef PUSH

    return n;
}

nd_err nd_kmi2c_progress(char *out, size_t out_sz, int32_t index, int32_t total)
{
    return nd_snprintf(out, out_sz, "%d-%d/%d", ND_KMI2C_ROOT_ID, index, total);
}

/* ------------------------------------------------------------------ *
 * The payload
 * ------------------------------------------------------------------ */

/* json.dump(sort_keys=True) orders by the key's bytes. Insertion sort over an
 * index array: sixteen entries at most, nothing allocated, and it agrees with
 * Python because both compare byte values -- "10,2" really does sort before
 * "2,0", and a row count above nine is the only case where anyone notices. */
static void sort_by_key(size_t *idx, size_t n, const char *const *keys)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        size_t v = idx[i];
        size_t j = i;

        while (j > 0u && strcmp(keys[idx[j - 1u]], keys[v]) > 0) {
            idx[j] = idx[j - 1u];
            j--;
        }
        idx[j] = v;
    }
}

nd_err nd_kmi2c_payload(char *out, size_t out_sz, const nd_kmi2c_config *cfg,
                        int64_t generated_at_unix, const nd_kmi2c_entry *keys, size_t n_keys)
{
    nd_err rc = ND_OK;
    nd_json_writer *w = NULL;
    char pos[ND_KMI2C_N_TARGETS][16];
    const char *pos_key[ND_KMI2C_N_TARGETS];
    const char *name_key[ND_KMI2C_N_TARGETS];
    size_t by_pos[ND_KMI2C_N_TARGETS];
    size_t by_name[ND_KMI2C_N_TARGETS];
    const char *text;
    size_t len = 0u;
    size_t i;

    if (out == NULL || out_sz == 0u || cfg == NULL || (keys == NULL && n_keys > 0u))
        return ND_ERR_INVAL;
    if (n_keys > ND_KMI2C_N_TARGETS)
        return ND_ERR_TOOLONG;

    for (i = 0u; i < n_keys; i++) {
        if (nd_snprintf(pos[i], sizeof pos[i], "%d,%d", keys[i].row, keys[i].col) != ND_OK)
            return ND_ERR_TOOLONG;
        pos_key[i] = pos[i];
        name_key[i] = keys[i].name;
        by_pos[i] = i;
        by_name[i] = i;
    }
    sort_by_key(by_pos, n_keys, pos_key);
    sort_by_key(by_name, n_keys, name_key);

    w = nd_json_writer_new(2);
    if (w == NULL)
        return ND_ERR_NOMEM;

#define PUT(expr)        \
    do {                 \
        rc = (expr);     \
        if (rc != ND_OK) \
            goto done;   \
    } while (0)

    PUT(nd_json_begin_object(w));

    /* "by_code" is written empty and nothing has ever read it; main.py emits
     * it and so does the first-boot wizard, so a third-party tool may still
     * expect the field to exist. */
    PUT(nd_json_key(w, "by_code"));
    PUT(nd_json_begin_object(w));
    PUT(nd_json_end_object(w));

    PUT(nd_json_key(w, "by_matrix"));
    PUT(nd_json_begin_object(w));
    for (i = 0u; i < n_keys; i++) {
        PUT(nd_json_key(w, pos[by_pos[i]]));
        PUT(nd_json_put_str(w, keys[by_pos[i]].name));
    }
    PUT(nd_json_end_object(w));

    PUT(nd_json_key(w, "col_pins"));
    PUT(nd_json_begin_array(w));
    for (i = 0u; i < cfg->n_cols; i++)
        PUT(nd_json_put_int(w, cfg->col_pins[i]));
    PUT(nd_json_end_array(w));

    PUT(nd_json_key(w, "driver"));
    PUT(nd_json_put_str(w, nd_kmi2c_driver));
    PUT(nd_json_key(w, "format"));
    PUT(nd_json_put_str(w, nd_kmi2c_format));
    PUT(nd_json_key(w, "generated_at_unix"));
    PUT(nd_json_put_int(w, generated_at_unix));

    /* i2c_addr is a DECIMAL integer -- 32, not "0x20". That is what
     * lib/nd_keymap.c reads back, and what core/main.py read back before it. */
    PUT(nd_json_key(w, "i2c_addr"));
    PUT(nd_json_put_int(w, cfg->addr));
    PUT(nd_json_key(w, "i2c_bus"));
    PUT(nd_json_put_int(w, cfg->bus));

    PUT(nd_json_key(w, "keys"));
    PUT(nd_json_begin_object(w));
    for (i = 0u; i < n_keys; i++) {
        const nd_kmi2c_entry *e = &keys[by_name[i]];

        PUT(nd_json_key(w, e->name));
        PUT(nd_json_begin_object(w));
        PUT(nd_json_key(w, "col"));
        PUT(nd_json_put_int(w, e->col));
        PUT(nd_json_key(w, "col_pin"));
        PUT(nd_json_put_int(w, e->col_pin));
        PUT(nd_json_key(w, "label"));
        PUT(nd_json_put_str(w, e->label));
        PUT(nd_json_key(w, "row"));
        PUT(nd_json_put_int(w, e->row));
        PUT(nd_json_key(w, "row_pin"));
        PUT(nd_json_put_int(w, e->row_pin));
        PUT(nd_json_end_object(w));
    }
    PUT(nd_json_end_object(w));

    /* The CONSTANT, not the path actually written: main.py stores OUTPUT_PATH
     * and ND_ROOT is a test fixture the file must never learn about. */
    PUT(nd_json_key(w, "output"));
    PUT(nd_json_put_str(w, nd_kmi2c_output_path));

    PUT(nd_json_key(w, "row_pins"));
    PUT(nd_json_begin_array(w));
    for (i = 0u; i < cfg->n_rows; i++)
        PUT(nd_json_put_int(w, cfg->row_pins[i]));
    PUT(nd_json_end_array(w));

    PUT(nd_json_end_object(w));

#undef PUT

    text = nd_json_writer_text(w, &len);
    if (text == NULL) {
        rc = ND_ERR_INVAL;
        goto done;
    }
    /* json.dump(...) and then f.write("\n"). */
    if (len + 2u > out_sz) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }
    memcpy(out, text, len);
    out[len] = '\n';
    out[len + 1u] = '\0';

done:
    nd_json_writer_free(w);
    return rc;
}

nd_err nd_kmi2c_save(const char *path, const char *text)
{
    char dir[ND_PATH_MAX];
    char tmp_virtual[ND_PATH_MAX];
    char tmp[ND_PATH_MAX];
    char final[ND_PATH_MAX];
    const char *slash;
    nd_err rc;
    size_t len;
    size_t off = 0u;
    int fd = -1;

    if (path == NULL || text == NULL)
        return ND_ERR_INVAL;
    len = strlen(text);

    /* os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True). */
    slash = strrchr(path, '/');
    if (slash != NULL && slash != path) {
        size_t dlen = (size_t)(slash - path);

        if (dlen >= sizeof dir)
            return ND_ERR_TOOLONG;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        rc = nd_mkdir_p(dir, 0755u);
        if (rc != ND_OK)
            return rc;
    }

    /* Temp file, fsync, rename. See the header comment: the Python writes in
     * place and this does not. */
    rc = nd_snprintf(tmp_virtual, sizeof tmp_virtual, "%s.tmp", path);
    if (rc != ND_OK)
        return rc;
    rc = nd_path_resolve(tmp, sizeof tmp, tmp_virtual);
    if (rc != ND_OK)
        return rc;
    rc = nd_path_resolve(final, sizeof final, path);
    if (rc != ND_OK)
        return rc;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        nd_log_err(KMI2C_TAG, "cannot open %s: %s", tmp, strerror(errno));
        return ND_ERR_IO;
    }

    rc = ND_OK;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = ND_ERR_IO;
            goto done;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        rc = ND_ERR_IO;
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        rc = ND_ERR_IO;
        goto done;
    }
    fd = -1;
    if (rename(tmp, final) != 0)
        rc = ND_ERR_IO;

done:
    if (fd >= 0)
        (void)close(fd);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

static void draw_capture_prompt(nd_ui *ui, nd_softkey *bar, const nd_kmi2c_config *cfg,
                                const char *label, int32_t index, int32_t total)
{
    char body[ND_KMI2C_N_BODY][ND_KMI2C_BODY_MAX];
    char storage[8][ND_TEXT_LINE_MAX];
    char progress[32];
    nd_lines lines;
    nd_draw *d = ui->draw;
    int32_t screen_w = nd_ui_width(ui);
    int32_t bottom = nd_ui_content_bottom(ui);
    int32_t tw = 0;
    int32_t th = 0;
    int32_t line_h = 0;
    int32_t y = 56;
    size_t n_body;
    size_t i;
    size_t j;

    /* rectangle((0, 0, W, H)) -- the FULL height, softkey strip included, and
     * the bar is repainted over it a few lines below. */
    nd_ui_paint_chrome_full(ui);

    nd_ui_text_size(ui, nd_kmi2c_title, ui->font_n, &tw, &th);
    (void)nd_draw_text(d, floordiv2(screen_w - tw), 8, nd_kmi2c_title, ui->font_n, ND_WHITE);
    (void)nd_draw_line(d, 12, 32, screen_w - 12, 32, ND_WHITE, 1);

    (void)nd_kmi2c_progress(progress, sizeof progress, index, total);
    nd_ui_text_size(ui, progress, ui->font_s, &tw, &th);
    (void)nd_draw_text(d, screen_w - tw - 8, 38, progress, ui->font_s, ND_GRAY);

    /* line_h = get_text_size("Ag", font_s)[1] + 4. Ink height, so the '+ 4' is
     * the whole of the leading and the 'g' descender is what sets the rest. */
    nd_ui_text_size(ui, "Ag", ui->font_s, &tw, &line_h);
    line_h += 4;

    n_body = nd_kmi2c_body(cfg, label, body, ND_KMI2C_N_BODY);
    nd_lines_init(&lines, storage, ND_ARRAY_LEN(storage));

    for (i = 0u; i < n_body; i++) {
        nd_kmi2c_wrap(&lines, body[i], ui->font_s, screen_w - 16);
        for (j = 0u; j < lines.n; j++) {
            /* The break leaves the INNER loop only. The outer loop keeps
             * going and every remaining entry trips this test on its first
             * line, so nothing more is drawn -- see the header comment. */
            if (y > bottom - line_h)
                break;
            (void)nd_draw_text(d, 8, y, nd_lines_at(&lines, j), ui->font_s, ND_WHITE);
            y += line_h;
        }
    }

    nd_softkey_update(bar, nd_kmi2c_softkey_text, false);
    (void)nd_ui_present(ui);
}

static void show_dialog(nd_ui *ui, const char *message, const char *title)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, ui, message);
    if (title != NULL)
        nd_msgdialog_set_title(&dlg, title);
    (void)nd_msgdialog_show(&dlg);
}

/* ------------------------------------------------------------------ *
 * The capture loop
 * ------------------------------------------------------------------ */

typedef enum { WAIT_PRESS = 0, WAIT_CANCEL, WAIT_ABORT, WAIT_ERROR } wait_result;

static wait_result wait_for_matrix_press(nd_ui *ui, nd_matrix_scanner *sc, nd_matrix_pos *out)
{
    for (;;) {
        int32_t key = nd_ui_read_keypress(ui, KMI2C_POLL_S);
        bool found = false;

        if (key == ND_KMI2C_KEY_MENU)
            return WAIT_CANCEL;

        if (nd_matrix_scan_once(sc, out, &found) != ND_OK)
            return WAIT_ERROR;
        if (found)
            return WAIT_PRESS;

        /* nd_app.h: any loop longer than a frame polls this. The Python had
         * no equivalent -- an incoming call raised IncomingCall out of
         * read_keypress -- and this loop can run for as long as the operator
         * takes to find the key. */
        if (nd_app_should_exit())
            return WAIT_ABORT;

        dwell(KMI2C_POLL_S);
    }
}

typedef enum { CAP_OK = 0, CAP_CANCELLED, CAP_ABORTED, CAP_ERROR } capture_result;

static capture_result capture_keymap(nd_ui *ui, nd_softkey *bar, const nd_kmi2c_config *cfg,
                                     nd_matrix_scanner *sc, nd_kmi2c_entry *out, size_t *n_out,
                                     char *err, size_t err_sz)
{
    bool used[ND_KMI2C_MAX_PINS][ND_KMI2C_MAX_PINS];
    size_t i;

    memset(used, 0, sizeof used);
    *n_out = 0u;

    for (i = 0u; i < ND_KMI2C_N_TARGETS; i++) {
        for (;;) {
            nd_matrix_pos pos;
            nd_kmi2c_entry *e;
            char msg[192];

            memset(&pos, 0, sizeof pos);
            draw_capture_prompt(ui, bar, cfg, nd_kmi2c_targets[i].label, (int32_t)i + 1,
                                (int32_t)ND_KMI2C_N_TARGETS);

            switch (wait_for_matrix_press(ui, sc, &pos)) {
            case WAIT_CANCEL:
                show_dialog(ui, nd_kmi2c_cancel_msg, NULL);
                return CAP_CANCELLED;
            case WAIT_ABORT:
                return CAP_ABORTED;
            case WAIT_ERROR:
                /* The Python's OSError carries the short-read text from the
                 * chip driver ("short read from /dev/i2c-3 (got 0 bytes)").
                 * nd_matrix_scan_once() logs that line itself and returns
                 * only ND_ERR_IO, so the dialog names the bus instead. */
                (void)nd_snprintf(err, err_sz, "i2c scan failed on /dev/i2c-%d", cfg->bus);
                return CAP_ERROR;
            case WAIT_PRESS:
            default:
                break;
            }

            if (used[pos.row][pos.col]) {
                (void)nd_snprintf(msg, sizeof msg,
                                  "Matrix R%u C%u is already mapped. Press a different key for %s.",
                                  (unsigned)pos.row, (unsigned)pos.col, nd_kmi2c_targets[i].label);
                show_dialog(ui, msg, NULL);
                continue; /* the SAME target is asked for again */
            }

            e = &out[*n_out];
            memset(e, 0, sizeof *e);
            (void)nd_strlcpy(e->name, nd_kmi2c_targets[i].name, sizeof e->name);
            (void)nd_strlcpy(e->label, nd_kmi2c_targets[i].label, sizeof e->label);
            e->row = (int32_t)pos.row;
            e->col = (int32_t)pos.col;
            e->row_pin = cfg->row_pins[pos.row];
            e->col_pin = cfg->col_pins[pos.col];
            (*n_out)++;
            used[pos.row][pos.col] = true;
            break;
        }
    }
    return CAP_OK;
}

static bool open_scanner(nd_matrix_scanner *sc, const nd_kmi2c_config *cfg, char *err,
                         size_t err_sz)
{
    uint8_t rows[ND_KMI2C_MAX_PINS];
    uint8_t cols[ND_KMI2C_MAX_PINS];
    char rows_repr[ND_KMI2C_REPR_MAX];
    char cols_repr[ND_KMI2C_REPR_MAX];
    char addr[24];
    size_t i;
    nd_err rc;

    /* I2CMatrixScanner.__init__ validates before it opens anything, and its
     * ValueError is what the operator ends up reading. */
    if (!nd_kmi2c_validate_pins(cfg, err, err_sz))
        return false;

    for (i = 0u; i < cfg->n_rows; i++)
        rows[i] = (uint8_t)cfg->row_pins[i];
    for (i = 0u; i < cfg->n_cols; i++)
        cols[i] = (uint8_t)cfg->col_pins[i];

    rc = nd_matrix_scanner_init(sc, rows, cfg->n_rows, cols, cfg->n_cols, (int)cfg->bus,
                                (int)cfg->addr);
    if (rc != ND_OK) {
        /* Python's OSError repr ("[Errno 2] No such file or directory:
         * '/dev/i2c-3'") comes from a layer nd_pcf8575_open does not surface;
         * it logs the errno itself and returns a category. */
        (void)nd_snprintf(err, err_sz, "/dev/i2c-%d: %s", cfg->bus, nd_strerror(rc));
        return false;
    }

    (void)nd_kmi2c_pin_repr(rows_repr, sizeof rows_repr, cfg->row_pins, cfg->n_rows);
    (void)nd_kmi2c_pin_repr(cols_repr, sizeof cols_repr, cfg->col_pins, cfg->n_cols);
    hex02(addr, sizeof addr, cfg->addr);
    nd_log(KMI2C_TAG, "Scanner ready. bus=%d addr=0x%s rows=%s cols=%s", cfg->bus, addr, rows_repr,
           cols_repr);
    return true;
}

static bool save_keymap(const nd_kmi2c_config *cfg, const nd_kmi2c_entry *keys, size_t n_keys,
                        char *err, size_t err_sz)
{
    /* 8 KB, written once per run, and an app process draws from one thread --
     * so it is static rather than eight kilobytes of stack frame
     * (CODING-STANDARDS.md 1.5). */
    static char payload[ND_KMI2C_PAYLOAD_MAX];
    nd_err rc;

    rc = nd_kmi2c_payload(payload, sizeof payload, cfg, (int64_t)nd_time_now(), keys, n_keys);
    if (rc != ND_OK) {
        (void)nd_snprintf(err, err_sz, "cannot build the keymap payload: %s", nd_strerror(rc));
        return false;
    }
    rc = nd_kmi2c_save(nd_kmi2c_output_path, payload);
    if (rc != ND_OK) {
        (void)nd_snprintf(err, err_sz, "%s: %s", nd_kmi2c_output_path, nd_strerror(rc));
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_kmi2c_config cfg;
    nd_matrix_scanner sc;
    nd_softkey bar;
    nd_kmi2c_entry entries[ND_KMI2C_N_TARGETS];
    char err[ND_KMI2C_ERR_MAX];
    char msg[ND_KMI2C_ERR_MAX + 32];
    char saved[ND_PATH_MAX + 32];
    size_t n_entries = 0u;
    bool scanner_open = false;
    bool failed = false;
    bool saved_ok = false;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* run(ui)'s only gate. Unlike the GPIO sibling there is no second one:
     * the driver is in-tree rather than a missing pip package. */
    if (!nd_kmi2c_i2c_available()) {
        show_dialog(ui, nd_kmi2c_i2c_required_msg, NULL);
        return 0;
    }

    memset(&sc, 0, sizeof sc);
    err[0] = '\0';
    nd_kmi2c_config_from_env(&cfg, true);
    nd_softkey_init(&bar, ui, false);

    show_dialog(ui, nd_kmi2c_intro_msg, nd_kmi2c_title);

    /* This is where main.py sets ui.matrix_input = None. See the header. */

    if (!open_scanner(&sc, &cfg, err, sizeof err)) {
        failed = true;
        goto finish;
    }
    scanner_open = true;

    switch (capture_keymap(ui, &bar, &cfg, &sc, entries, &n_entries, err, sizeof err)) {
    case CAP_CANCELLED: /* the dialog has already been shown */
    case CAP_ABORTED:   /* SIGTERM: draw nothing more */
        goto finish;
    case CAP_ERROR:
        failed = true;
        goto finish;
    case CAP_OK:
    default:
        break;
    }

    /* `if not captured: return` cannot fire here -- CAP_OK means all sixteen
     * were recorded -- but the Python's empty-dict test is the same test. */
    if (n_entries == 0u)
        goto finish;

    if (!save_keymap(&cfg, entries, n_entries, err, sizeof err)) {
        failed = true;
        goto finish;
    }
    saved_ok = true;

finish:
    /* Order matters and it is the Python's: the `except` block prints and
     * shows its dialog, and only then does `finally` close the scanner. */
    if (failed) {
        nd_log(KMI2C_TAG, "Capture/save error: %s", err);
        (void)nd_snprintf(msg, sizeof msg, "Failed to write keymap: %s", err);
        show_dialog(ui, msg, NULL);
    }
    if (scanner_open)
        nd_matrix_scanner_close(&sc);

    if (saved_ok) {
        (void)nd_snprintf(saved, sizeof saved, "Keymap saved to\n%s", nd_kmi2c_output_path);
        show_dialog(ui, saved, NULL);
    }
    return 0;
}

/* The scanner is closed on every path out of app_run() -- including the
 * SIGTERM one, which returns rather than longjmp'ing -- so there is nothing
 * left holding the bus by the time nd-apprun calls this. Exported because
 * nd_app.h requires it. */
void app_shutdown(void) {}
