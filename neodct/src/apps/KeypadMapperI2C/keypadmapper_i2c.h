/* keypadmapper_i2c.h -- what apps/KeypadMapperI2C/main.c shows its unit test.
 *
 * "kmi2c" is KeypadMapperI2C; the abbreviation exists only so the symbol
 * names fit on a line. Everything here is a one-to-one port of a function or
 * a constant in System/engineering/apps/KeypadMapperI2C/main.py, and the
 * comment beside each says which.
 *
 * The split is the one main.py's Modem sibling explains for itself: the parts
 * that decide WHAT is written -- the environment parsing, the pin validation,
 * the six prompt lines, the JSON payload -- are drawing-free so they can be
 * asserted directly, and main.c keeps only the parts that put pixels on a
 * panel or bytes on a bus.
 */

#ifndef ND_KEYPADMAPPER_I2C_H_INCLUDED
#define ND_KEYPADMAPPER_I2C_H_INCLUDED

#include "nd_font.h"
#include "nd_text.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py: ROOT_ID = 10 -- the "10" in the "10-1/16" breadcrumb. It is NOT
 * the app's manifest id (9003); it is the engineering menu's own root. */
#define ND_KMI2C_ROOT_ID 10

/* main.py: KEY_MENU = 50. Spelled as the number the Python holds; nd_keycodes
 * calls the same code ND_KEY_MENU. */
#define ND_KMI2C_KEY_MENU 50

/* The sixteen buttons, in capture order. */
#define ND_KMI2C_N_TARGETS 16

/* The expander has sixteen pins, so neither list can be longer. */
#define ND_KMI2C_MAX_PINS 16

/* "navikey" is the longest name at 7, "NaviKey" the longest label at 7. */
#define ND_KMI2C_NAME_MAX  16
#define ND_KMI2C_LABEL_MAX 16

/* A Python list repr of sixteen int32 values: 16 * 11 digits + 15 * ", " plus
 * the two brackets is 208. Only 0..15 can reach a drawn line -- the scanner
 * validates before anything is drawn -- but the parser hands this function
 * whatever the environment said, so the bound is the parser's, not the
 * scanner's. */
#define ND_KMI2C_REPR_MAX 256

/* "Rows: P" + repr + " Cols: P" + repr, the widest of the six body lines. */
#define ND_KMI2C_BODY_MAX 544

/* Six lines, one of which is deliberately blank. */
#define ND_KMI2C_N_BODY 6

/* Python's ValueError and OSError messages are short; 160 covers the longest
 * ("invalid literal for int() with base 10: '<32 bytes>'") with room over. */
#define ND_KMI2C_ERR_MAX 160

/* json.dumps(payload, indent=2, sort_keys=True) of a full sixteen-key map is
 * 2,461 bytes; a 16x16 map with two-digit pins is under 4,000. 8 KB is the
 * next power of two and this buffer is written once per run. */
#define ND_KMI2C_PAYLOAD_MAX 8192

/* ------------------------------------------------------------------ *
 * The strings, verbatim
 * ------------------------------------------------------------------ */

extern const char *const nd_kmi2c_output_path;      /* OUTPUT_PATH          */
extern const char *const nd_kmi2c_i2c_required_msg; /* I2C_REQUIRED_MSG     */
extern const char *const nd_kmi2c_intro_msg;        /* run()'s first dialog */
extern const char *const nd_kmi2c_cancel_msg;       /* MENU pressed         */
extern const char *const nd_kmi2c_title;            /* both dialog + prompt */
extern const char *const nd_kmi2c_format;           /* payload "format"     */
extern const char *const nd_kmi2c_driver;           /* payload "driver"     */
extern const char *const nd_kmi2c_softkey_text;     /* "Capture"            */

/* KEYPAD_ROWS_ENV / KEYPAD_COLS_ENV / I2C_BUS_ENV / I2C_ADDR_ENV. */
#define ND_KMI2C_ENV_ROWS "NEODCT_KEYPAD_ROWS"
#define ND_KMI2C_ENV_COLS "NEODCT_KEYPAD_COLS"
#define ND_KMI2C_ENV_BUS  "NEODCT_I2C_KEYPAD_BUS"
#define ND_KMI2C_ENV_ADDR "NEODCT_I2C_KEYPAD_ADDR"

/* KEY_TARGETS. The name is what keymap.json calls the key and what
 * nd_keycode_for_name() resolves; the label is what the operator is told to
 * press. */
typedef struct {
    const char *name;
    const char *label;
} nd_kmi2c_target;

extern const nd_kmi2c_target nd_kmi2c_targets[ND_KMI2C_N_TARGETS];

/* System/hw/pcf8575_keypad.py: DEFAULT_ROW_PINS / DEFAULT_COL_PINS, i.e.
 * expander pins P00-P03 and P04-P07. DEFAULT_BUS and DEFAULT_ADDR are
 * ND_I2C_BUS_DEFAULT and ND_I2C_ADDR_DEFAULT from nd_keypad.h. */
#define ND_KMI2C_N_DEFAULT_PINS 4
extern const int32_t nd_kmi2c_default_rows[ND_KMI2C_N_DEFAULT_PINS];
extern const int32_t nd_kmi2c_default_cols[ND_KMI2C_N_DEFAULT_PINS];

/* ------------------------------------------------------------------ *
 * _i2c_config() and the three parsers under it
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t row_pins[ND_KMI2C_MAX_PINS];
    size_t n_rows;
    int32_t col_pins[ND_KMI2C_MAX_PINS];
    size_t n_cols;
    int32_t bus;
    int32_t addr;
} nd_kmi2c_config;

/* _parse_pins(raw, fallback). raw NULL is Python's missing variable, which
 * `(raw or "")` turns into the empty string and the empty string means "use
 * the fallback". A chunk that is not an integer is Python's ValueError: false
 * is returned and `err` gets int()'s own message, which is what the caller
 * prints and what the operator sees. An all-empty list ("," or " , ") falls
 * back too, exactly as `return out if out else list(fallback)` does.
 *
 * Pins are int32 and are NOT range-checked here, because the Python does not
 * check them here either: an out-of-range pin survives parsing and is refused
 * by the scanner's own validation, which is a different dialog. */
bool nd_kmi2c_parse_pins(const char *raw, const int32_t *fallback, size_t n_fallback, int32_t *out,
                         size_t max, size_t *n_out, char *err, size_t err_sz);

/* _parse_addr(raw, fallback): hexadecimal when the text starts "0x" or "0X",
 * decimal otherwise. Note that "-0x20" does NOT start "0x", so Python's
 * int("-0x20") raises -- and so does this. */
bool nd_kmi2c_parse_addr(const char *raw, int32_t fallback, int32_t *out, char *err, size_t err_sz);

/* int(os.environ.get(I2C_BUS_ENV, "").strip() or DEFAULT_BUS). */
bool nd_kmi2c_parse_bus(const char *raw, int32_t fallback, int32_t *out, char *err, size_t err_sz);

/* _i2c_config(): all four reads inside ONE try, so the first failure discards
 * the three that may already have succeeded and every value goes back to its
 * default. `log` false suppresses the "[KEYMAP-I2C] Invalid override" line,
 * which is only wanted when a test is driving this in a loop. */
void nd_kmi2c_config_from_env(nd_kmi2c_config *out, bool log);

/* Same, from four explicit strings rather than the environment. NULL means
 * "the variable is not set". */
void nd_kmi2c_config_from(nd_kmi2c_config *out, const char *rows, const char *cols, const char *bus,
                          const char *addr, bool log);

/* I2CMatrixScanner._validate_pins: every pin 0..15 and no pin listed twice,
 * rows and columns counted together. `err` gets the ValueError text the
 * Python raises, which reaches the operator inside "Failed to write keymap:". */
bool nd_kmi2c_validate_pins(const nd_kmi2c_config *cfg, char *err, size_t err_sz);

/* _i2c_available(): len(glob("/dev/i2c-*")) > 0. Goes through ND_ROOT like
 * every other path, so a host test can build a fake /dev. */
bool nd_kmi2c_i2c_available(void);

/* ------------------------------------------------------------------ *
 * The prompt
 * ------------------------------------------------------------------ */

/* Python's repr of a list of ints: "[0, 1, 2, 3]". Square brackets, ", "
 * between, "[]" when empty. Interpolated straight into two of the six body
 * lines, so the exact spelling is on screen. */
nd_err nd_kmi2c_pin_repr(char *out, size_t out_sz, const int32_t *pins, size_t n);

/* _wrap_text(ui, text, max_width, font) -- the app's OWN wrapper, and the
 * seventh in the system (nd_text.h lists the other six). It splits on
 * str.split(), so runs of whitespace and newlines collapse and no blank line
 * can ever come out of it; it never breaks an over-long word; and empty input
 * gives one empty line rather than none. */
void nd_kmi2c_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w);

/* The six body lines of _draw_capture_prompt, before wrapping and before the
 * clipping loop drops most of them. Returns how many were written. */
size_t nd_kmi2c_body(const nd_kmi2c_config *cfg, const char *label, char out[][ND_KMI2C_BODY_MAX],
                     size_t max);

/* f"{ROOT_ID}-{index}/{total}", index 1-based. */
nd_err nd_kmi2c_progress(char *out, size_t out_sz, int32_t index, int32_t total);

/* ------------------------------------------------------------------ *
 * The payload
 * ------------------------------------------------------------------ */

/* One entry of keymap_by_name. `label` is carried because the Python writes
 * it into the file; nothing reads it back. */
typedef struct {
    char name[ND_KMI2C_NAME_MAX];
    char label[ND_KMI2C_LABEL_MAX];
    int32_t row;
    int32_t col;
    int32_t row_pin;
    int32_t col_pin;
} nd_kmi2c_entry;

/* _save_keymap's payload, rendered as json.dump(payload, indent=2,
 * sort_keys=True) followed by the single "\n" the Python writes after it.
 * ND_ERR_TOOLONG when it will not fit in out_sz. */
nd_err nd_kmi2c_payload(char *out, size_t out_sz, const nd_kmi2c_config *cfg,
                        int64_t generated_at_unix, const nd_kmi2c_entry *keys, size_t n_keys);

/* os.makedirs(dirname, exist_ok=True) and then the write. See main.c for the
 * one deliberate difference from the Python: the write is atomic. */
nd_err nd_kmi2c_save(const char *path, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* ND_KEYPADMAPPER_I2C_H_INCLUDED */
