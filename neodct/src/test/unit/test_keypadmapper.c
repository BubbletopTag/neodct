/* test_keypadmapper.c -- the two keypad enrolment apps, ids 9002 and 9003.
 *
 * ============ WHY ONE FILE FOR TWO APPS ============
 *
 * They are the same wizard. main.py's KeypadMapperI2C is main.py's
 * KeypadMapper with the gpiozero scanner swapped for the PCF8575 one, and
 * both write the same file to the same path. The i2c one is the one that
 * runs; the GPIO one is a gate and two dialogs, for the reason its own header
 * comment sets out. Testing them apart would mean two copies of the fixture
 * and no place to say that out loud.
 *
 * ============ WHAT THIS TEST CLAIMS ============
 *
 *  1. KEY_TARGETS is main.py's sixteen pairs in main.py's order, and every
 *     name in it is one lib/nd_keycodes.c resolves. That second half is the
 *     whole point of the app: a name this wizard invents is a key the phone
 *     will not have when nd_keymap_load() reads the file back.
 *
 *  2. _parse_pins, _parse_addr and the bus parse are CPython's int(),
 *     including the messages it raises -- those reach the operator inside
 *     "Failed to write keymap:".
 *
 *  3. _i2c_config wraps all four reads in ONE try, so a bad NEODCT_I2C_KEYPAD_BUS
 *     throws away a perfectly good NEODCT_KEYPAD_ROWS. That is a quirk and it
 *     is asserted as one.
 *
 *  4. _validate_pins refuses a pin outside 0..15 and a pin used twice, rows
 *     and columns counted together, with the Python's exact wording.
 *
 *  5. The prompt's six body lines are the Python's strings, "P" prefixes and
 *     list reprs included -- and only FOUR of them are ever drawn, because
 *     line five would land at y=132 against a cut-off of 126. The clipping is
 *     the frame; a port that "fixes" it is wrong.
 *
 *  6. _wrap_text is the app's own seventh wrapper: it collapses every run of
 *     whitespace, never emits a blank line, never breaks an over-long word,
 *     and answers [""] for empty input.
 *
 *  7. THE PAYLOAD IS BYTE-EXACT. The reference below came out of CPython:
 *     json.dumps(payload, indent=2, sort_keys=True) + "\n" over a full
 *     sixteen-key capture. Every key at every level in ASCII order, two-space
 *     indent, ": " after each key, arrays expanded one element per line,
 *     "by_code" empty on one line, and i2c_addr a DECIMAL 32.
 *
 *  8. What it writes, lib/nd_keymap.c reads: the file goes through
 *     nd_keymap_load() and comes back with the right driver, bus, address,
 *     pins and all sixteen positions mapped to the right keycodes. And no
 *     ".tmp" is left behind.
 *
 *  9. Both gates. No /dev/i2c-* is the I2C_REQUIRED_MSG dialog and nothing
 *     else; no /dev/gpiochip* is GPIO_REQUIRED_MSG; gpiochips present is
 *     GPIOZERO_REQUIRED_MSG, which is the ONLY thing KeypadMapper can ever do
 *     on hardware. Each is judged by rendering the dialog it should have
 *     shown into the same fixture and comparing digests.
 *
 * ============ WHAT IT CANNOT CLAIM ============
 *
 * Nothing here touches an i2c bus. The capture loop, the scanner and the
 * "Matrix R0 C0 is already mapped" retry need a PCF8575 answering on
 * /dev/i2c-3; test_keypad.c already fakes THE CHIP for the scanner underneath
 * this app, and the app's own loop over it is only ever exercised on a real
 * phone. Said out loud rather than hidden behind a green tick.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (for the
 * font); the scratch root is this test's own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_widgets.h"

#include "smallapp_test.h"

#include "../../apps/KeypadMapperI2C/keypadmapper_i2c.h"

/* ------------------------------------------------------------------ *
 * The two app.so handles
 * ------------------------------------------------------------------ */

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    bool (*parse_pins)(const char *, const int32_t *, size_t, int32_t *, size_t, size_t *, char *,
                       size_t);
    bool (*parse_addr)(const char *, int32_t, int32_t *, char *, size_t);
    bool (*parse_bus)(const char *, int32_t, int32_t *, char *, size_t);
    void (*config_from)(nd_kmi2c_config *, const char *, const char *, const char *, const char *,
                        bool);
    bool (*validate_pins)(const nd_kmi2c_config *, char *, size_t);
    bool (*i2c_available)(void);
    nd_err (*pin_repr)(char *, size_t, const int32_t *, size_t);
    void (*wrap)(nd_lines *, const char *, const nd_font *, int32_t);
    size_t (*body)(const nd_kmi2c_config *, const char *, char (*)[ND_KMI2C_BODY_MAX], size_t);
    nd_err (*progress)(char *, size_t, int32_t, int32_t);
    nd_err (*payload)(char *, size_t, const nd_kmi2c_config *, int64_t, const nd_kmi2c_entry *,
                      size_t);
    nd_err (*save)(const char *, const char *);
    const nd_kmi2c_target *targets;
    const int32_t *default_rows;
    const int32_t *default_cols;
    const char *const *output_path;
    const char *const *i2c_required_msg;
    const char *const *intro_msg;
    const char *const *cancel_msg;
    const char *const *title;
    const char *const *format;
    const char *const *driver;
    const char *const *softkey_text;
} i2c;


static bool i2c_api_open(void *h)
{
    *(void **)&i2c.run = sa_sym(h, "app_run");
    *(void **)&i2c.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&i2c.parse_pins = sa_sym(h, "nd_kmi2c_parse_pins");
    *(void **)&i2c.parse_addr = sa_sym(h, "nd_kmi2c_parse_addr");
    *(void **)&i2c.parse_bus = sa_sym(h, "nd_kmi2c_parse_bus");
    *(void **)&i2c.config_from = sa_sym(h, "nd_kmi2c_config_from");
    *(void **)&i2c.validate_pins = sa_sym(h, "nd_kmi2c_validate_pins");
    *(void **)&i2c.i2c_available = sa_sym(h, "nd_kmi2c_i2c_available");
    *(void **)&i2c.pin_repr = sa_sym(h, "nd_kmi2c_pin_repr");
    *(void **)&i2c.wrap = sa_sym(h, "nd_kmi2c_wrap");
    *(void **)&i2c.body = sa_sym(h, "nd_kmi2c_body");
    *(void **)&i2c.progress = sa_sym(h, "nd_kmi2c_progress");
    *(void **)&i2c.payload = sa_sym(h, "nd_kmi2c_payload");
    *(void **)&i2c.save = sa_sym(h, "nd_kmi2c_save");
    i2c.targets = sa_sym(h, "nd_kmi2c_targets");
    i2c.default_rows = sa_sym(h, "nd_kmi2c_default_rows");
    i2c.default_cols = sa_sym(h, "nd_kmi2c_default_cols");
    i2c.output_path = sa_sym(h, "nd_kmi2c_output_path");
    i2c.i2c_required_msg = sa_sym(h, "nd_kmi2c_i2c_required_msg");
    i2c.intro_msg = sa_sym(h, "nd_kmi2c_intro_msg");
    i2c.cancel_msg = sa_sym(h, "nd_kmi2c_cancel_msg");
    i2c.title = sa_sym(h, "nd_kmi2c_title");
    i2c.format = sa_sym(h, "nd_kmi2c_format");
    i2c.driver = sa_sym(h, "nd_kmi2c_driver");
    i2c.softkey_text = sa_sym(h, "nd_kmi2c_softkey_text");

    return i2c.run != NULL && i2c.shutdown != NULL && i2c.parse_pins != NULL &&
           i2c.parse_addr != NULL && i2c.parse_bus != NULL && i2c.config_from != NULL &&
           i2c.validate_pins != NULL && i2c.i2c_available != NULL && i2c.pin_repr != NULL &&
           i2c.wrap != NULL && i2c.body != NULL && i2c.progress != NULL && i2c.payload != NULL &&
           i2c.save != NULL && i2c.targets != NULL && i2c.default_rows != NULL &&
           i2c.default_cols != NULL && i2c.output_path != NULL && i2c.i2c_required_msg != NULL &&
           i2c.intro_msg != NULL && i2c.cancel_msg != NULL && i2c.title != NULL &&
           i2c.format != NULL && i2c.driver != NULL && i2c.softkey_text != NULL;
}


/* ------------------------------------------------------------------ *
 * A scratch ND_ROOT, so /dev can be invented
 * ------------------------------------------------------------------ */

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static void root_to_scratch(void)
{
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
}

static void root_restore(void)
{
    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
}

/* Creates an empty file at a VIRTUAL path, parents included. The apps only
 * glob for these, so an ordinary file stands in for a character device. */
static bool touch_virtual(const char *path)
{
    char resolved[ND_PATH_MAX];
    const char *slash = strrchr(path, '/');
    FILE *f;

    if (slash != NULL && slash != path) {
        char dir[ND_PATH_MAX];

        (void)nd_strlcpy(dir, path, (size_t)(slash - path) + 1u);
        if (nd_mkdir_p(dir, 0755u) != ND_OK)
            return false;
    }
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return false;
    f = fopen(resolved, "wb");
    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static void unlink_virtual(const char *path)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, path) == ND_OK)
        (void)remove(resolved);
}

/* ------------------------------------------------------------------ *
 * 1. KEY_TARGETS
 * ------------------------------------------------------------------ */

static void test_key_targets(void)
{
    static const char *const NAMES[ND_KMI2C_N_TARGETS] = {
        "navikey", "clear", "up",    "down",  "num_1", "num_2", "num_3", "num_4",
        "num_5",   "num_6", "num_7", "num_8", "num_9", "num_0", "star",  "hash"};
    static const char *const LABELS[ND_KMI2C_N_TARGETS] = {
        "NaviKey", "C", "Up", "Down", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "*", "#"};
    size_t i;

    for (i = 0u; i < ND_KMI2C_N_TARGETS; i++) {
        CHECK_STR(i2c.targets[i].name, NAMES[i], "KEY_TARGETS name");
        CHECK_STR(i2c.targets[i].label, LABELS[i], "KEY_TARGETS label");
        /* MATRIX_NAME_TO_CODE has to know every name this wizard writes, or
         * the key it enrolled does nothing on the next boot. */
        CHECK(nd_keycode_for_name(i2c.targets[i].name) >= 0,
              "nd_keycode_for_name knows the enrolled name");
    }

    CHECK_INT(nd_keycode_for_name("navikey"), ND_KEY_ENTER, "navikey is ENTER");
    CHECK_INT(nd_keycode_for_name("clear"), ND_KEY_CLEAR, "clear is BACKSPACE");
    CHECK_INT(nd_keycode_for_name("star"), ND_KEY_STAR, "star is LEFTSHIFT");
    CHECK_INT(nd_keycode_for_name("hash"), ND_KEY_HASH, "hash is BACKSLASH");
    CHECK_INT(nd_keycode_for_name("num_0"), ND_KEY_0, "num_0 is 11, after 9");
}

static void test_constants(void)
{
    CHECK_STR(*i2c.output_path, "/NeoDCT/User/keymap.json", "OUTPUT_PATH");
    CHECK_STR(*i2c.output_path, ND_PATH_KEYMAP, "and it is the path the core reads");
    CHECK_STR(*i2c.i2c_required_msg,
              "This app requires I2C. No /dev/i2c-* devices found. "
              "This application can not run in QEMU.",
              "I2C_REQUIRED_MSG");
    CHECK_STR(*i2c.intro_msg,
              "This tool captures PCF8575 I2C keypad presses and writes JSON to "
              "/NeoDCT/User/keymap.json.",
              "run()'s first dialog");
    CHECK_STR(*i2c.cancel_msg, "Calibration canceled. Keymap not saved.", "the MENU dialog");
    CHECK_STR(*i2c.title, "Keypad Mapper I2C", "title");
    CHECK_STR(*i2c.format, "neodct.keymap.v3.matrix.i2c", "payload format");
    CHECK_STR(*i2c.driver, "pcf8575-i2c", "payload driver");
    CHECK_STR(*i2c.softkey_text, "Capture", "SoftKeyBar text");
    CHECK_INT(ND_KMI2C_ROOT_ID, 10, "ROOT_ID");
    CHECK_INT(ND_KMI2C_KEY_MENU, ND_KEY_MENU, "KEY_MENU = 50");

    /* System/hw/pcf8575_keypad.py's four defaults. */
    CHECK_INT(i2c.default_rows[0], 0, "DEFAULT_ROW_PINS[0]");
    CHECK_INT(i2c.default_rows[3], 3, "DEFAULT_ROW_PINS[3]");
    CHECK_INT(i2c.default_cols[0], 4, "DEFAULT_COL_PINS[0]");
    CHECK_INT(i2c.default_cols[3], 7, "DEFAULT_COL_PINS[3]");
    CHECK_INT(ND_I2C_BUS_DEFAULT, 3, "DEFAULT_BUS");
    CHECK_INT(ND_I2C_ADDR_DEFAULT, 0x20, "DEFAULT_ADDR");

}

/* ------------------------------------------------------------------ *
 * 2. _parse_pins / _parse_addr / the bus
 * ------------------------------------------------------------------ */

static void test_parse_pins(void)
{
    int32_t out[ND_KMI2C_MAX_PINS];
    size_t n = 0u;
    char err[ND_KMI2C_ERR_MAX];

    err[0] = '\0';

    /* `(raw or "").strip()` -- unset, empty and all-whitespace are the same
     * thing, and all three mean "use the fallback". */
    CHECK(i2c.parse_pins(NULL, i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "unset falls back");
    CHECK_INT(n, 4, "four fallback pins");
    CHECK_INT(out[0], 0, "fallback[0]");
    CHECK_INT(out[3], 3, "fallback[3]");

    CHECK(i2c.parse_pins("   ", i2c.default_cols, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "whitespace falls back");
    CHECK_INT(out[0], 4, "the COLUMN fallback this time");

    CHECK(
        i2c.parse_pins("1,2,3", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
        "a plain list parses");
    CHECK_INT(n, 3, "three pins");
    CHECK_INT(out[0], 1, "pins[0]");
    CHECK_INT(out[2], 3, "pins[2]");

    /* Each chunk is stripped, so a human-typed list with spaces works. */
    CHECK(i2c.parse_pins(" 7 , 8 ", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err,
                         sizeof err),
          "chunks are stripped");
    CHECK_INT(n, 2, "two pins");
    CHECK_INT(out[0], 7, "pins[0]");
    CHECK_INT(out[1], 8, "pins[1]");

    /* `if not chunk: continue` -- an empty chunk is skipped, not an error. */
    CHECK(i2c.parse_pins("1,,2", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "an empty chunk is skipped");
    CHECK_INT(n, 2, "two pins from three chunks");

    /* ...and a list of nothing but empty chunks falls back, because of
     * `return out if out else list(fallback)`. */
    CHECK(i2c.parse_pins(",,", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "all-empty falls back");
    CHECK_INT(n, 4, "the fallback, not an empty list");

    /* int('+3') is 3. */
    CHECK(i2c.parse_pins("+3", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "a leading plus parses");
    CHECK_INT(out[0], 3, "+3");

    /* An out-of-range pin survives PARSING. The Python only rejects it in
     * I2CMatrixScanner._validate_pins, which is a different dialog. */
    CHECK(i2c.parse_pins("-1", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "-1 parses");
    CHECK_INT(out[0], -1, "and stays -1 for the validator to refuse");

    CHECK(!i2c.parse_pins("abc", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "a non-integer raises");
    CHECK_STR(err, "invalid literal for int() with base 10: 'abc'", "CPython's own message");

    CHECK(!i2c.parse_pins("1,x", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "and the first bad chunk decides");
    CHECK_STR(err, "invalid literal for int() with base 10: 'x'", "naming that chunk");

    /* "0x4" is not a decimal integer to Python either. */
    CHECK(!i2c.parse_pins("0x4", i2c.default_rows, 4u, out, ND_KMI2C_MAX_PINS, &n, err, sizeof err),
          "hex is not accepted for pins");
}

static void test_parse_addr_and_bus(void)
{
    int32_t v = 0;
    char err[ND_KMI2C_ERR_MAX];

    err[0] = '\0';

    CHECK(i2c.parse_addr(NULL, ND_I2C_ADDR_DEFAULT, &v, err, sizeof err), "unset falls back");
    CHECK_INT(v, 0x20, "DEFAULT_ADDR");

    CHECK(i2c.parse_addr("0x21", ND_I2C_ADDR_DEFAULT, &v, err, sizeof err), "0x21 parses");
    CHECK_INT(v, 33, "as hexadecimal");
    /* text.lower().startswith("0x"), so an upper-case X counts. */
    CHECK(i2c.parse_addr("0X21", ND_I2C_ADDR_DEFAULT, &v, err, sizeof err), "0X21 parses");
    CHECK_INT(v, 33, "the lower() is on the TEST, not the digits");

    CHECK(i2c.parse_addr(" 32 ", ND_I2C_ADDR_DEFAULT, &v, err, sizeof err), "a decimal parses");
    CHECK_INT(v, 32, "as decimal");

    /* "-0x20" does not START with "0x", so Python tries base 10 and raises. */
    CHECK(!i2c.parse_addr("-0x20", ND_I2C_ADDR_DEFAULT, &v, err, sizeof err),
          "a negative hex address raises");
    CHECK_STR(err, "invalid literal for int() with base 10: '-0x20'", "in base 10, as Python does");

    CHECK(!i2c.parse_addr("0xzz", ND_I2C_ADDR_DEFAULT, &v, err, sizeof err), "bad hex raises");
    CHECK_STR(err, "invalid literal for int() with base 16: '0xzz'", "and says base 16");

    CHECK(i2c.parse_bus(NULL, ND_I2C_BUS_DEFAULT, &v, err, sizeof err), "unset falls back");
    CHECK_INT(v, 3, "DEFAULT_BUS");
    CHECK(i2c.parse_bus("", ND_I2C_BUS_DEFAULT, &v, err, sizeof err), "empty falls back");
    CHECK_INT(v, 3, "`<stripped> or DEFAULT_BUS` is the default");
    CHECK(i2c.parse_bus("5", ND_I2C_BUS_DEFAULT, &v, err, sizeof err), "a bus number parses");
    CHECK_INT(v, 5, "bus 5");
    CHECK(!i2c.parse_bus("i2c3", ND_I2C_BUS_DEFAULT, &v, err, sizeof err), "a name raises");
    CHECK_STR(err, "invalid literal for int() with base 10: 'i2c3'", "CPython's message");
}

/* ------------------------------------------------------------------ *
 * 3. _i2c_config -- one try around all four
 * ------------------------------------------------------------------ */

static void test_config(void)
{
    nd_kmi2c_config cfg;

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    CHECK_INT(cfg.n_rows, 4, "four default rows");
    CHECK_INT(cfg.n_cols, 4, "four default columns");
    CHECK_INT(cfg.row_pins[0], 0, "P00");
    CHECK_INT(cfg.col_pins[0], 4, "P04");
    CHECK_INT(cfg.bus, 3, "bus 3");
    CHECK_INT(cfg.addr, 0x20, "0x20");

    i2c.config_from(&cfg, "8,9,10,11", "12,13,14,15", "1", "0x21", false);
    CHECK_INT(cfg.row_pins[0], 8, "an override is taken");
    CHECK_INT(cfg.col_pins[3], 15, "on both lists");
    CHECK_INT(cfg.bus, 1, "and the bus");
    CHECK_INT(cfg.addr, 33, "and the address");

    /* THE QUIRK. All four reads are inside ONE try, so the first exception
     * discards the values the earlier reads already produced. A typo in the
     * address therefore silently puts the PINS back to the defaults too --
     * which on a phone whose rows are not P00-P03 means a wizard that
     * enrols nothing usable. */
    i2c.config_from(&cfg, "8,9,10,11", "12,13,14,15", "1", "nonsense", false);
    CHECK_INT(cfg.row_pins[0], 0, "one bad value resets the rows");
    CHECK_INT(cfg.col_pins[0], 4, "and the columns");
    CHECK_INT(cfg.bus, 3, "and the bus");
    CHECK_INT(cfg.addr, 0x20, "and the address");

    /* Same the other way round: a bad row list never reaches the bus parse. */
    i2c.config_from(&cfg, "nonsense", "12,13,14,15", "1", "0x21", false);
    CHECK_INT(cfg.col_pins[0], 4, "the good column list is discarded too");
    CHECK_INT(cfg.bus, 3, "and the good bus");
}

/* ------------------------------------------------------------------ *
 * 4. _validate_pins
 * ------------------------------------------------------------------ */

static void test_validate_pins(void)
{
    nd_kmi2c_config cfg;
    char err[ND_KMI2C_ERR_MAX];

    err[0] = '\0';
    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    CHECK(i2c.validate_pins(&cfg, err, sizeof err), "the defaults validate");

    i2c.config_from(&cfg, "0,1,2,16", "4,5,6,7", NULL, NULL, false);
    CHECK(!i2c.validate_pins(&cfg, err, sizeof err), "16 is off the end of the expander");
    CHECK_STR(err, "expander pin 16 out of range 0-15", "the Python's wording");

    i2c.config_from(&cfg, "-1,1,2,3", "4,5,6,7", NULL, NULL, false);
    CHECK(!i2c.validate_pins(&cfg, err, sizeof err), "-1 is refused here, not in the parser");
    CHECK_STR(err, "expander pin -1 out of range 0-15", "with the sign kept");

    i2c.config_from(&cfg, "0,1,2,2", "4,5,6,7", NULL, NULL, false);
    CHECK(!i2c.validate_pins(&cfg, err, sizeof err), "a repeat inside the rows");
    CHECK_STR(err, "expander pin 2 listed twice", "the Python's wording");

    /* Rows and columns share ONE set: a pin cannot be both. */
    i2c.config_from(&cfg, "0,1,2,3", "3,5,6,7", NULL, NULL, false);
    CHECK(!i2c.validate_pins(&cfg, err, sizeof err), "a pin used as a row AND a column");
    CHECK_STR(err, "expander pin 3 listed twice", "counted together");
}

/* ------------------------------------------------------------------ *
 * 5. The prompt
 * ------------------------------------------------------------------ */

static void test_pin_repr(void)
{
    static const int32_t FOUR[4] = {0, 1, 2, 3};
    static const int32_t ONE[1] = {7};
    char out[ND_KMI2C_REPR_MAX];

    /* Python's repr of a list of ints, which is interpolated straight into
     * two of the six body lines. */
    CHECK_INT(i2c.pin_repr(out, sizeof out, FOUR, 4u), ND_OK, "four pins");
    CHECK_STR(out, "[0, 1, 2, 3]", "square brackets, comma-space");
    CHECK_INT(i2c.pin_repr(out, sizeof out, ONE, 1u), ND_OK, "one pin");
    CHECK_STR(out, "[7]", "no separator");
    CHECK_INT(i2c.pin_repr(out, sizeof out, FOUR, 0u), ND_OK, "no pins");
    CHECK_STR(out, "[]", "an empty list is still brackets");
}

static void test_progress(void)
{
    char out[32];

    CHECK_INT(i2c.progress(out, sizeof out, 1, 16), ND_OK, "the first target");
    CHECK_STR(out, "10-1/16", "ROOT_ID, the 1-BASED index, the total");
    CHECK_INT(i2c.progress(out, sizeof out, 16, 16), ND_OK, "the last target");
    CHECK_STR(out, "10-16/16", "no padding");
}

static void test_body(void)
{
    nd_kmi2c_config cfg;
    char body[ND_KMI2C_N_BODY][ND_KMI2C_BODY_MAX];
    size_t n;

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    n = i2c.body(&cfg, "NaviKey", body, ND_KMI2C_N_BODY);
    CHECK_INT(n, 6, "six body lines are BUILT");
    CHECK_STR(body[0], "Press: NaviKey", "body[0]");
    CHECK_STR(body[1], "", "body[1] is deliberately blank");
    CHECK_STR(body[2], "Capture one keypad button now.", "body[2]");
    CHECK_STR(body[3], "Menu key cancels.", "body[3]");
    CHECK_STR(body[4], "Bus: /dev/i2c-3 Addr: 0x20", "body[4]");
    /* The "P" prefixes the whole list, not each pin. */
    CHECK_STR(body[5], "Rows: P[0, 1, 2, 3] Cols: P[4, 5, 6, 7]", "body[5]");

    i2c.config_from(&cfg, NULL, NULL, "11", "0x4f", false);
    n = i2c.body(&cfg, "#", body, ND_KMI2C_N_BODY);
    CHECK_INT(n, 6, "still six");
    CHECK_STR(body[0], "Press: #", "the label is interpolated raw");
    /* f"0x{addr:02X}" -- upper case, at least two digits. */
    CHECK_STR(body[4], "Bus: /dev/i2c-11 Addr: 0x4F", "upper-case hex");
}

/* ------------------------------------------------------------------ *
 * 6. _wrap_text, the seventh wrapper
 * ------------------------------------------------------------------ */

#define WRAP_STORAGE 8

static void test_wrap(sa_fixture *fx)
{
    char storage[WRAP_STORAGE][ND_TEXT_LINE_MAX];
    nd_lines lines;
    int32_t w = 0;
    int32_t h = 0;

    nd_lines_init(&lines, storage, WRAP_STORAGE);

    /* `if not words: return [""]` -- one EMPTY line, not zero lines. That is
     * what puts the blank second row on the prompt. */
    i2c.wrap(&lines, "", fx->ui.font_s, 224);
    CHECK_INT(lines.n, 1, "empty gives one line");
    CHECK_STR(nd_lines_at(&lines, 0), "", "and it is empty");

    i2c.wrap(&lines, NULL, fx->ui.font_s, 224);
    CHECK_INT(lines.n, 1, "NULL is Python's `(text or \"\")`");

    /* str.split() collapses every run of whitespace and drops the ends, so a
     * blank line can never come out of this wrapper. */
    i2c.wrap(&lines, "  Menu   key\tcancels.  ", fx->ui.font_s, 224);
    CHECK_INT(lines.n, 1, "runs of whitespace collapse");
    CHECK_STR(nd_lines_at(&lines, 0), "Menu key cancels.", "into single spaces");

    i2c.wrap(&lines, "Press: NaviKey", fx->ui.font_s, 224);
    CHECK_INT(lines.n, 1, "a short line does not wrap");
    CHECK_STR(nd_lines_at(&lines, 0), "Press: NaviKey", "unchanged");

    /* The one body line that really does wrap on this panel. */
    i2c.wrap(&lines, "Capture one keypad button now.", fx->ui.font_s, 224);
    CHECK_INT(lines.n, 2, "the long line wraps in two");
    CHECK_STR(nd_lines_at(&lines, 0), "Capture one keypad", "greedy, so as much as fits");
    CHECK_STR(nd_lines_at(&lines, 1), "button now.", "and the rest");

    /* An over-long WORD is not broken. It goes on a line of its own and
     * overflows the margin -- which is the difference between this wrapper
     * and nd_text_wrap_break(). */
    i2c.wrap(&lines, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", fx->ui.font_s, 40);
    CHECK_INT(lines.n, 1, "one line");
    nd_text_size(fx->ui.font_s, nd_lines_at(&lines, 0), &w, &h);
    CHECK(w > 40, "and it is wider than the box it was given");

    /* A word that does not fit still gets a line of its own AFTER whatever
     * was accumulated, and `if current:` stops an empty first line. */
    i2c.wrap(&lines, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bb", fx->ui.font_s, 40);
    CHECK_INT(lines.n, 2, "the over-wide word, then the rest");
    CHECK_STR(nd_lines_at(&lines, 1), "bb", "the short word follows");
}

/* The clipping quirk, as data: with y starting at 56, line_h from the ink
 * height of "Ag" at 14 px, and the cut-off at content_bottom - line_h, only
 * the first four wrapped lines are ever drawn. */
static void test_only_four_lines_are_drawn(sa_fixture *fx)
{
    static const char *const VISIBLE[4] = {"Press: NaviKey", "", "Capture one keypad",
                                           "button now."};
    char body[ND_KMI2C_N_BODY][ND_KMI2C_BODY_MAX];
    char storage[WRAP_STORAGE][ND_TEXT_LINE_MAX];
    char drawn[16][ND_TEXT_LINE_MAX];
    nd_lines lines;
    nd_kmi2c_config cfg;
    int32_t line_h = 0;
    int32_t unused_w = 0;
    int32_t y = 56;
    int32_t bottom = nd_ui_content_bottom(&fx->ui);
    size_t n_body;
    size_t n_drawn = 0u;
    size_t i;
    size_t j;

    nd_ui_text_size(&fx->ui, "Ag", fx->ui.font_s, &unused_w, &line_h);
    line_h += 4;
    CHECK_INT(line_h, 19, "line_h = ink height of \"Ag\" + 4");
    CHECK_INT(bottom, 145, "content_bottom");

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    n_body = i2c.body(&cfg, "NaviKey", body, ND_KMI2C_N_BODY);
    nd_lines_init(&lines, storage, WRAP_STORAGE);

    /* main.c's loop, reproduced here so the claim is about the rule and not
     * about a pixel that happens to be black. */
    for (i = 0u; i < n_body; i++) {
        i2c.wrap(&lines, body[i], fx->ui.font_s, nd_ui_width(&fx->ui) - 16);
        for (j = 0u; j < lines.n; j++) {
            if (y > bottom - line_h)
                break;
            if (n_drawn < ND_ARRAY_LEN(drawn))
                (void)nd_strlcpy(drawn[n_drawn], nd_lines_at(&lines, j), ND_TEXT_LINE_MAX);
            n_drawn++;
            y += line_h;
        }
    }

    CHECK_INT(n_drawn, 4, "FOUR of the six body entries' lines reach the panel");
    for (i = 0u; i < 4u; i++)
        CHECK_STR(drawn[i], VISIBLE[i], "the visible line");
    CHECK_INT(y, 132, "the fifth would have been at 132, past the 126 cut-off");
}

/* ------------------------------------------------------------------ *
 * 7. The payload, byte for byte
 * ------------------------------------------------------------------ */

/* json.dumps(payload, indent=2, sort_keys=True) + "\n", straight out of
 * CPython, over a sixteen-key capture that walks the 4x4 matrix in
 * KEY_TARGETS order. generated_at_unix is ND_VCLOCK_EPOCH's whole second so
 * the reference is fixed forever. */
static const char REF_PAYLOAD[] = "{\n"
                                  "  \"by_code\": {},\n"
                                  "  \"by_matrix\": {\n"
                                  "    \"0,0\": \"navikey\",\n"
                                  "    \"0,1\": \"clear\",\n"
                                  "    \"0,2\": \"up\",\n"
                                  "    \"0,3\": \"down\",\n"
                                  "    \"1,0\": \"num_1\",\n"
                                  "    \"1,1\": \"num_2\",\n"
                                  "    \"1,2\": \"num_3\",\n"
                                  "    \"1,3\": \"num_4\",\n"
                                  "    \"2,0\": \"num_5\",\n"
                                  "    \"2,1\": \"num_6\",\n"
                                  "    \"2,2\": \"num_7\",\n"
                                  "    \"2,3\": \"num_8\",\n"
                                  "    \"3,0\": \"num_9\",\n"
                                  "    \"3,1\": \"num_0\",\n"
                                  "    \"3,2\": \"star\",\n"
                                  "    \"3,3\": \"hash\"\n"
                                  "  },\n"
                                  "  \"col_pins\": [\n"
                                  "    4,\n"
                                  "    5,\n"
                                  "    6,\n"
                                  "    7\n"
                                  "  ],\n"
                                  "  \"driver\": \"pcf8575-i2c\",\n"
                                  "  \"format\": \"neodct.keymap.v3.matrix.i2c\",\n"
                                  "  \"generated_at_unix\": 1704112496,\n"
                                  "  \"i2c_addr\": 32,\n"
                                  "  \"i2c_bus\": 3,\n"
                                  "  \"keys\": {\n"
                                  "    \"clear\": {\n"
                                  "      \"col\": 1,\n"
                                  "      \"col_pin\": 5,\n"
                                  "      \"label\": \"C\",\n"
                                  "      \"row\": 0,\n"
                                  "      \"row_pin\": 0\n"
                                  "    },\n"
                                  "    \"down\": {\n"
                                  "      \"col\": 3,\n"
                                  "      \"col_pin\": 7,\n"
                                  "      \"label\": \"Down\",\n"
                                  "      \"row\": 0,\n"
                                  "      \"row_pin\": 0\n"
                                  "    },\n"
                                  "    \"hash\": {\n"
                                  "      \"col\": 3,\n"
                                  "      \"col_pin\": 7,\n"
                                  "      \"label\": \"#\",\n"
                                  "      \"row\": 3,\n"
                                  "      \"row_pin\": 3\n"
                                  "    },\n"
                                  "    \"navikey\": {\n"
                                  "      \"col\": 0,\n"
                                  "      \"col_pin\": 4,\n"
                                  "      \"label\": \"NaviKey\",\n"
                                  "      \"row\": 0,\n"
                                  "      \"row_pin\": 0\n"
                                  "    },\n"
                                  "    \"num_0\": {\n"
                                  "      \"col\": 1,\n"
                                  "      \"col_pin\": 5,\n"
                                  "      \"label\": \"0\",\n"
                                  "      \"row\": 3,\n"
                                  "      \"row_pin\": 3\n"
                                  "    },\n"
                                  "    \"num_1\": {\n"
                                  "      \"col\": 0,\n"
                                  "      \"col_pin\": 4,\n"
                                  "      \"label\": \"1\",\n"
                                  "      \"row\": 1,\n"
                                  "      \"row_pin\": 1\n"
                                  "    },\n"
                                  "    \"num_2\": {\n"
                                  "      \"col\": 1,\n"
                                  "      \"col_pin\": 5,\n"
                                  "      \"label\": \"2\",\n"
                                  "      \"row\": 1,\n"
                                  "      \"row_pin\": 1\n"
                                  "    },\n"
                                  "    \"num_3\": {\n"
                                  "      \"col\": 2,\n"
                                  "      \"col_pin\": 6,\n"
                                  "      \"label\": \"3\",\n"
                                  "      \"row\": 1,\n"
                                  "      \"row_pin\": 1\n"
                                  "    },\n"
                                  "    \"num_4\": {\n"
                                  "      \"col\": 3,\n"
                                  "      \"col_pin\": 7,\n"
                                  "      \"label\": \"4\",\n"
                                  "      \"row\": 1,\n"
                                  "      \"row_pin\": 1\n"
                                  "    },\n"
                                  "    \"num_5\": {\n"
                                  "      \"col\": 0,\n"
                                  "      \"col_pin\": 4,\n"
                                  "      \"label\": \"5\",\n"
                                  "      \"row\": 2,\n"
                                  "      \"row_pin\": 2\n"
                                  "    },\n"
                                  "    \"num_6\": {\n"
                                  "      \"col\": 1,\n"
                                  "      \"col_pin\": 5,\n"
                                  "      \"label\": \"6\",\n"
                                  "      \"row\": 2,\n"
                                  "      \"row_pin\": 2\n"
                                  "    },\n"
                                  "    \"num_7\": {\n"
                                  "      \"col\": 2,\n"
                                  "      \"col_pin\": 6,\n"
                                  "      \"label\": \"7\",\n"
                                  "      \"row\": 2,\n"
                                  "      \"row_pin\": 2\n"
                                  "    },\n"
                                  "    \"num_8\": {\n"
                                  "      \"col\": 3,\n"
                                  "      \"col_pin\": 7,\n"
                                  "      \"label\": \"8\",\n"
                                  "      \"row\": 2,\n"
                                  "      \"row_pin\": 2\n"
                                  "    },\n"
                                  "    \"num_9\": {\n"
                                  "      \"col\": 0,\n"
                                  "      \"col_pin\": 4,\n"
                                  "      \"label\": \"9\",\n"
                                  "      \"row\": 3,\n"
                                  "      \"row_pin\": 3\n"
                                  "    },\n"
                                  "    \"star\": {\n"
                                  "      \"col\": 2,\n"
                                  "      \"col_pin\": 6,\n"
                                  "      \"label\": \"*\",\n"
                                  "      \"row\": 3,\n"
                                  "      \"row_pin\": 3\n"
                                  "    },\n"
                                  "    \"up\": {\n"
                                  "      \"col\": 2,\n"
                                  "      \"col_pin\": 6,\n"
                                  "      \"label\": \"Up\",\n"
                                  "      \"row\": 0,\n"
                                  "      \"row_pin\": 0\n"
                                  "    }\n"
                                  "  },\n"
                                  "  \"output\": \"/NeoDCT/User/keymap.json\",\n"
                                  "  \"row_pins\": [\n"
                                  "    0,\n"
                                  "    1,\n"
                                  "    2,\n"
                                  "    3\n"
                                  "  ]\n"
                                  "}\n";

static void fill_entries(nd_kmi2c_entry *out, const nd_kmi2c_config *cfg)
{
    size_t i;

    for (i = 0u; i < ND_KMI2C_N_TARGETS; i++) {
        size_t row = i / 4u;
        size_t col = i % 4u;

        memset(&out[i], 0, sizeof out[i]);
        (void)nd_strlcpy(out[i].name, i2c.targets[i].name, ND_KMI2C_NAME_MAX);
        (void)nd_strlcpy(out[i].label, i2c.targets[i].label, ND_KMI2C_LABEL_MAX);
        out[i].row = (int32_t)row;
        out[i].col = (int32_t)col;
        out[i].row_pin = cfg->row_pins[row];
        out[i].col_pin = cfg->col_pins[col];
    }
}

static void test_payload_is_byte_exact(void)
{
    static char got[ND_KMI2C_PAYLOAD_MAX];
    nd_kmi2c_entry entries[ND_KMI2C_N_TARGETS];
    nd_kmi2c_config cfg;

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    fill_entries(entries, &cfg);

    CHECK_INT(i2c.payload(got, sizeof got, &cfg, 1704112496, entries, ND_KMI2C_N_TARGETS), ND_OK,
              "the payload builds");
    CHECK_INT(strlen(got), strlen(REF_PAYLOAD), "the same length as CPython's");
    CHECK_STR(got, REF_PAYLOAD, "byte for byte");
}

static void test_payload_edges(void)
{
    static char got[ND_KMI2C_PAYLOAD_MAX];
    char small[64];
    nd_kmi2c_entry entries[ND_KMI2C_N_TARGETS];
    nd_kmi2c_config cfg;

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    fill_entries(entries, &cfg);

    CHECK_INT(i2c.payload(small, sizeof small, &cfg, 0, entries, ND_KMI2C_N_TARGETS),
              ND_ERR_TOOLONG, "a buffer that cannot hold it is refused");

    /* An empty capture still produces a well-formed document, with both
     * "keys" and "by_matrix" rendered as "{}" on one line the way json.dump
     * renders an empty container. */
    CHECK_INT(i2c.payload(got, sizeof got, &cfg, 0, NULL, 0u), ND_OK, "no keys is not an error");
    CHECK(strstr(got, "\"by_code\": {},") != NULL, "by_code is empty and inline");
    CHECK(strstr(got, "\"by_matrix\": {},") != NULL, "so is an empty by_matrix");
    CHECK(strstr(got, "\"keys\": {},") != NULL, "and an empty keys");
    CHECK_INT(got[strlen(got) - 1u], '\n', "the trailing newline f.write(\"\\n\") adds");
}

/* ------------------------------------------------------------------ *
 * 8. What it writes, the core reads
 * ------------------------------------------------------------------ */

static void test_saved_file_loads_back(void)
{
    static char text[ND_KMI2C_PAYLOAD_MAX];
    nd_kmi2c_entry entries[ND_KMI2C_N_TARGETS];
    nd_kmi2c_config cfg;
    nd_keymap km;
    size_t i;

    root_to_scratch();

    i2c.config_from(&cfg, NULL, NULL, NULL, NULL, false);
    fill_entries(entries, &cfg);
    CHECK_INT(i2c.payload(text, sizeof text, &cfg, 1704112496, entries, ND_KMI2C_N_TARGETS), ND_OK,
              "payload");

    /* The parent directory does not exist yet, which is the first-boot case
     * os.makedirs(exist_ok=True) is there for. */
    CHECK(!nd_path_exists(ND_PATH_USER), "no /NeoDCT/User yet");
    CHECK_INT(i2c.save(*i2c.output_path, text), ND_OK, "the keymap saves");
    CHECK(nd_path_exists(ND_PATH_KEYMAP), "the file is where the core looks");
    CHECK(!nd_path_exists("/NeoDCT/User/keymap.json.tmp"), "and no temp file is left behind");

    /* THE CLAIM THAT MATTERS: lib/nd_keymap.c can read what this wrote. */
    memset(&km, 0, sizeof km);
    CHECK_INT(nd_keymap_load(ND_PATH_KEYMAP, &km), ND_OK, "nd_keymap_load accepts it");
    CHECK_STR(km.driver, "pcf8575-i2c", "the driver try_open_matrix() insists on");
    CHECK_STR(km.format, "neodct.keymap.v3.matrix.i2c", "the format");
    CHECK_INT(km.i2c_bus, 3, "bus");
    CHECK_INT(km.i2c_addr, 0x20, "address, read back from the DECIMAL 32");
    CHECK_INT(km.n_rows, 4, "four rows");
    CHECK_INT(km.n_cols, 4, "four columns");
    CHECK_INT(km.row_pins[0], 0, "P00");
    CHECK_INT(km.col_pins[3], 7, "P07");

    for (i = 0u; i < ND_KMI2C_N_TARGETS; i++) {
        int32_t want = nd_keycode_for_name(entries[i].name);

        CHECK_INT(km.matrix_to_code[entries[i].row][entries[i].col], want,
                  "every enrolled position maps to the right keycode");
    }

    root_restore();
}

static void test_save_reports_a_bad_path(void)
{
    char blocked[ND_PATH_MAX];

    /* A root that is a FILE, so mkdir -p underneath it cannot work. The
     * Python's OSError becomes the "Failed to write keymap:" dialog. */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_snprintf(blocked, sizeof blocked, "%s/not-a-directory", g_root);
    (void)nd_path_set_root(g_root);
    CHECK(touch_virtual("/not-a-directory"), "a file where a directory would go");
    (void)nd_path_set_root(blocked);

    CHECK(i2c.save(*i2c.output_path, "{}\n") != ND_OK, "the save fails rather than pretending");

    root_restore();
}

/* ------------------------------------------------------------------ *
 * 9. The two gates
 * ------------------------------------------------------------------ */

static bool digest_of_recent(sa_fixture *fx, char *out, size_t out_sz)
{
    const nd_image *frame = nd_capture_recent(fx->cap, 0u);

    if (frame == NULL)
        return false;
    return nd_capture_digest(frame, out, out_sz) == ND_OK;
}

/* Renders `message` as an untitled MessageDialog into the same fixture and
 * returns its digest, so "the app showed exactly this dialog" is a comparison
 * rather than an eyeball. */
static bool digest_of_dialog(sa_fixture *fx, const char *message, char *out, size_t out_sz)
{
    nd_msgdialog d;

    nd_msgdialog_init(&d, &fx->ui, message);
    nd_msgdialog_render(&d);
    return digest_of_recent(fx, out, out_sz);
}

static void test_i2c_gate(void)
{
    sa_fixture fx;
    char shown[65];
    char expect[65];

    root_to_scratch();
    unlink_virtual("/dev/i2c-3");
    CHECK(!i2c.i2c_available(), "no /dev/i2c-* in an empty tree");

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        root_restore();
        return;
    }
    CHECK(sa_hold(&fx, ND_KEY_ENTER), "held ENTER dismisses the dialog");
    CHECK_INT(i2c.run(&fx.ui), 0, "app_run returns cleanly");
    CHECK_INT(nd_capture_frames_drawn(fx.cap), 1, "ONE frame: the gate dialog and nothing else");
    CHECK(digest_of_recent(&fx, shown, sizeof shown), "a frame was committed");
    CHECK(digest_of_dialog(&fx, *i2c.i2c_required_msg, expect, sizeof expect), "reference dialog");
    CHECK_STR(shown, expect, "and it was I2C_REQUIRED_MSG");

    sa_fx_free(&fx);

    /* A bus node is enough to get past the gate; nothing beyond it is
     * exercised here, because the next thing run() does is open a chip. */
    CHECK(touch_virtual("/dev/i2c-3"), "a fake bus node");
    CHECK(i2c.i2c_available(), "and now the gate opens");
    unlink_virtual("/dev/i2c-3");

    root_restore();
}

static void test_null_safety(void)
{
    CHECK_INT(i2c.run(NULL), 1, "KeypadMapperI2C refuses a NULL context");
    i2c.shutdown(); /* both must be safe with nothing held */
    sa_checks++;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    void *h_i2c = sa_begin("KeypadMapperI2C", "ndkeymap");
    sa_fixture fx;
    int rc;

    if (h_i2c == NULL)
        return 1;
    if (!i2c_api_open(h_i2c)) {
        (void)dlclose(h_i2c);
        return 1;
    }
    if (!sa_tmpdir("ndkeymap-root", g_root, sizeof g_root)) {
        (void)dlclose(h_i2c);
        return 1;
    }

    RUN(test_key_targets);
    RUN(test_constants);
    RUN(test_parse_pins);
    RUN(test_parse_addr_and_bus);
    RUN(test_config);
    RUN(test_validate_pins);
    RUN(test_pin_repr);
    RUN(test_progress);
    RUN(test_body);

    /* The two wrapper cases need real glyph metrics. */
    if (sa_fx_init(&fx)) {
        test_wrap(&fx);
        test_only_four_lines_are_drawn(&fx);
    } else {
        CHECK(false, "fixture for the wrapper cases");
    }
    sa_fx_free(&fx);

    RUN(test_payload_is_byte_exact);
    RUN(test_payload_edges);
    RUN(test_saved_file_loads_back);
    RUN(test_save_reports_a_bad_path);
    RUN(test_i2c_gate);
    RUN(test_null_safety);

    rc = sa_end(h_i2c, "test_keypadmapper");
    sa_rmtree(g_root);
    return rc;
}
