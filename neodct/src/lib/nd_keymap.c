/* nd_keymap.c -- /NeoDCT/User/keymap.json, read and written.
 *
 * Ported from core/main.py:_load_matrix_keymap (the reader) and
 * hw/i2c_keypad_setup._build_payload (the writer).
 *
 * ============ WHY THE READER IS SO FORGIVING ============
 *
 * This file is the only thing standing between a working phone and a phone
 * with no input device at all, and it is written by a first-boot wizard that
 * a user can interrupt. So the reader refuses the WHOLE file for a structural
 * problem -- pins that are not a list, no recognisable keys -- and silently
 * SKIPS one bad entry, because a keymap missing the '7' key still boots a
 * phone you can fix, while a keymap rejected over the '7' key does not.
 *
 * Every refusal path keeps the Python's exact log line, because those lines
 * are what someone reads over the serial console at 03:00 with a phone that
 * will not respond to a keypress.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_log.h"
#include "nd_paths.h"

/* int(x) over the three spellings the file can legally carry: a JSON number,
 * a JSON float (Python's int() truncates toward zero), or a decimal string.
 * A hex string is handled separately -- only i2c_addr accepts one. */
static bool json_to_int(const nd_json_val *v, int64_t *out)
{
    const char *s;
    double d;
    char *end;
    long long parsed;

    if (v == NULL)
        return false;
    if (nd_json_int(v, out))
        return true;
    if (nd_json_real(v, &d)) {
        *out = (int64_t)d;
        return true;
    }
    if (nd_json_str(v, &s)) {
        errno = 0;
        parsed = strtoll(s, &end, 10);
        if (end == s || *end != '\0' || errno != 0)
            return false;
        *out = (int64_t)parsed;
        return true;
    }
    return false;
}

/* i2c_addr is written as an int (32 for 0x20), but the loader also accepts
 * "0x20" and "32" because two other tools have written this field by hand. */
static bool json_to_addr(const nd_json_val *v, int64_t *out)
{
    const char *s;
    char *end;
    long long parsed;

    if (v == NULL)
        return false;
    if (nd_json_str(v, &s)) {
        int base = 10;

        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            base = 16;
        errno = 0;
        parsed = strtoll(s, &end, base);
        if (end == s || *end != '\0' || errno != 0)
            return false;
        *out = (int64_t)parsed;
        return true;
    }
    return json_to_int(v, out);
}

static nd_err read_pin_list(const nd_json_val *arr, uint8_t *out, size_t max, size_t *n_out)
{
    size_t n = nd_json_len(arr);
    size_t i;

    if (n > max)
        return ND_ERR_TOOLONG;

    for (i = 0u; i < n; i++) {
        int64_t pin = 0;

        if (!json_to_int(nd_json_at(arr, i), &pin))
            return ND_ERR_PARSE;
        if (pin < 0 || pin >= (int64_t)ND_MATRIX_MAX_PINS)
            return ND_ERR_INVAL;
        out[i] = (uint8_t)pin;
    }
    *n_out = n;
    return ND_OK;
}

nd_err nd_keymap_load(const char *path, nd_keymap *out)
{
    nd_err rc = ND_OK;
    char err[128];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    const nd_json_val *rows;
    const nd_json_val *cols;
    const nd_json_val *keys;
    const nd_json_val *v;
    int64_t addr = ND_I2C_ADDR_DEFAULT;
    int64_t bus = ND_I2C_BUS_DEFAULT;
    size_t n_keys;
    size_t i;
    size_t r;
    size_t c;
    bool any_key = false;

    if (path == NULL || out == NULL)
        return ND_ERR_INVAL;

    memset(out, 0, sizeof *out);
    for (r = 0u; r < ND_KEYMAP_MAX_ROWS; r++) {
        for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++)
            out->matrix_to_code[r][c] = -1;
    }
    (void)nd_strlcpy(out->path, path, sizeof out->path);
    (void)nd_strlcpy(out->format, "unknown", sizeof out->format);
    /* The Python's default when the field is absent. Nothing the wizard
     * writes omits it; a hand-edited file might. */
    (void)nd_strlcpy(out->driver, "gpiozero-matrix", sizeof out->driver);
    out->i2c_addr = ND_I2C_ADDR_DEFAULT;
    out->i2c_bus = ND_I2C_BUS_DEFAULT;

    /* Absent is the normal case in QEMU and is deliberately silent. */
    if (!nd_path_exists(path))
        return ND_ERR_NOTFOUND;

    /* nd_json_parse_file() resolves ND_ROOT itself, so the VIRTUAL path goes
     * in -- resolving here as well would prefix the root twice. */
    err[0] = '\0';
    rc = nd_json_parse_file(path, &doc, err, sizeof err);
    if (rc != ND_OK) {
        nd_log(ND_LOG_INPUT, "Keymap read failed (%s): %s", path,
               err[0] != '\0' ? err : nd_strerror(rc));
        rc = ND_ERR_PARSE;
        goto done;
    }

    root = nd_json_root(doc);
    rows = nd_json_get(root, "row_pins");
    cols = nd_json_get(root, "col_pins");
    keys = nd_json_get(root, "keys");

    if (nd_json_type_of(rows) != ND_JSON_ARRAY || nd_json_type_of(cols) != ND_JSON_ARRAY ||
        nd_json_type_of(keys) != ND_JSON_OBJECT) {
        nd_log(ND_LOG_INPUT, "Keymap ignored (missing matrix fields): %s", path);
        rc = ND_ERR_PARSE;
        goto done;
    }

    rc = read_pin_list(rows, out->row_pins, ND_KEYMAP_MAX_ROWS, &out->n_rows);
    if (rc == ND_OK)
        rc = read_pin_list(cols, out->col_pins, ND_KEYMAP_MAX_COLS, &out->n_cols);
    if (rc != ND_OK) {
        nd_log(ND_LOG_INPUT, "Keymap ignored (invalid pin list): %s", nd_strerror(rc));
        rc = ND_ERR_PARSE;
        goto done;
    }

    n_keys = nd_json_len(keys);
    for (i = 0u; i < n_keys; i++) {
        const char *name = nd_json_key_at(keys, i);
        const nd_json_val *entry = nd_json_member_at(keys, i);
        int64_t row = 0;
        int64_t col = 0;
        int32_t code;

        /* Per-entry parsing is forgiving on purpose: a non-dict entry, an
         * unknown name, or a row/col that is not a number is skipped in
         * silence rather than losing the whole keymap. */
        if (nd_json_type_of(entry) != ND_JSON_OBJECT)
            continue;
        code = nd_keycode_for_name(name);
        if (code < 0)
            continue;
        if (!json_to_int(nd_json_get(entry, "row"), &row))
            continue;
        if (!json_to_int(nd_json_get(entry, "col"), &col))
            continue;
        /* The Python would happily record (99, 3) in a dict no scan can ever
         * match. The C table is a flat 16x16, so an out-of-range position is
         * dropped here instead of being unreachable later. */
        if (row < 0 || row >= (int64_t)ND_KEYMAP_MAX_ROWS)
            continue;
        if (col < 0 || col >= (int64_t)ND_KEYMAP_MAX_COLS)
            continue;

        out->matrix_to_code[row][col] = code;
        any_key = true;
    }

    if (!any_key) {
        nd_log(ND_LOG_INPUT, "Keymap ignored (no recognized keys): %s", path);
        rc = ND_ERR_PARSE;
        goto done;
    }

    v = nd_json_get(root, "i2c_addr");
    if (v != NULL && !json_to_addr(v, &addr)) {
        nd_log(ND_LOG_INPUT, "Keymap ignored (invalid i2c fields): bad i2c_addr");
        rc = ND_ERR_PARSE;
        goto done;
    }
    v = nd_json_get(root, "i2c_bus");
    if (v != NULL && !json_to_int(v, &bus)) {
        nd_log(ND_LOG_INPUT, "Keymap ignored (invalid i2c fields): bad i2c_bus");
        rc = ND_ERR_PARSE;
        goto done;
    }
    out->i2c_addr = (int)addr;
    out->i2c_bus = (int)bus;

    (void)nd_strlcpy(out->format, nd_json_get_str(root, "format", "unknown"), sizeof out->format);
    (void)nd_strlcpy(out->driver, nd_json_get_str(root, "driver", "gpiozero-matrix"),
                     sizeof out->driver);
    rc = ND_OK;

done:
    nd_json_free(doc);
    return rc;
}

/* ------------------------------------------------------------------ *
 * The writer
 * ------------------------------------------------------------------ */

/* The wizard's _build_payload, sorted the way json.dump(sort_keys=True)
 * sorts: by byte value of the key. Written out in that order by hand rather
 * than by sorting at runtime, because there are nine keys and they never
 * change -- and a sort here would have to agree with Python's forever. */
nd_err nd_keymap_save(const nd_keymap *km, const char *path)
{
    nd_err rc = ND_OK;
    nd_json_writer *w = NULL;
    size_t i;
    size_t r;
    size_t c;
    /* Sorted by name so the "keys" object matches sort_keys=True. Only the
     * names the wizard enrols appear; nd_keycode_for_name's two aliases
     * ("enter", "back") are never written, so a round trip is stable. */
    static const char *const names[] = {
        "clear", "down",  "hash",  "left",  "menu",  "navikey", "num_0", "num_1", "num_2", "num_3",
        "num_4", "num_5", "num_6", "num_7", "num_8", "num_9",   "right", "star",  "up"};

    if (km == NULL || path == NULL)
        return ND_ERR_INVAL;

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

    /* "by_code" and "by_matrix" are legacy fields the loader ignores; the
     * wizard still writes them and a third-party tool may still read them. */
    PUT(nd_json_key(w, "by_code"));
    PUT(nd_json_begin_object(w));
    PUT(nd_json_end_object(w));

    PUT(nd_json_key(w, "by_matrix"));
    PUT(nd_json_begin_object(w));
    for (r = 0u; r < km->n_rows; r++) {
        for (c = 0u; c < km->n_cols; c++) {
            char pos[16];
            int32_t code = km->matrix_to_code[r][c];
            size_t k;

            if (code < 0)
                continue;
            if (snprintf(pos, sizeof pos, "%u,%u", (unsigned)r, (unsigned)c) < 0) {
                rc = ND_ERR_TOOLONG;
                goto done;
            }
            for (k = 0u; k < ND_ARRAY_LEN(names); k++) {
                if (nd_keycode_for_name(names[k]) == code) {
                    PUT(nd_json_key(w, pos));
                    PUT(nd_json_put_str(w, names[k]));
                    break;
                }
            }
        }
    }
    PUT(nd_json_end_object(w));

    PUT(nd_json_key(w, "col_pins"));
    PUT(nd_json_begin_array(w));
    for (i = 0u; i < km->n_cols; i++)
        PUT(nd_json_put_int(w, km->col_pins[i]));
    PUT(nd_json_end_array(w));

    PUT(nd_json_key(w, "driver"));
    PUT(nd_json_put_str(w, km->driver));
    PUT(nd_json_key(w, "format"));
    PUT(nd_json_put_str(w, km->format));
    PUT(nd_json_key(w, "i2c_addr"));
    PUT(nd_json_put_int(w, km->i2c_addr));
    PUT(nd_json_key(w, "i2c_bus"));
    PUT(nd_json_put_int(w, km->i2c_bus));

    PUT(nd_json_key(w, "keys"));
    PUT(nd_json_begin_object(w));
    for (i = 0u; i < ND_ARRAY_LEN(names); i++) {
        int32_t want = nd_keycode_for_name(names[i]);
        bool placed = false;

        for (r = 0u; r < km->n_rows && !placed; r++) {
            for (c = 0u; c < km->n_cols && !placed; c++) {
                if (km->matrix_to_code[r][c] != want)
                    continue;
                PUT(nd_json_key(w, names[i]));
                PUT(nd_json_begin_object(w));
                PUT(nd_json_key(w, "col"));
                PUT(nd_json_put_int(w, (int64_t)c));
                PUT(nd_json_key(w, "col_pin"));
                PUT(nd_json_put_int(w, km->col_pins[c]));
                PUT(nd_json_key(w, "row"));
                PUT(nd_json_put_int(w, (int64_t)r));
                PUT(nd_json_key(w, "row_pin"));
                PUT(nd_json_put_int(w, km->row_pins[r]));
                PUT(nd_json_end_object(w));
                placed = true;
            }
        }
    }
    PUT(nd_json_end_object(w));

    PUT(nd_json_key(w, "output"));
    PUT(nd_json_put_str(w, path));

    PUT(nd_json_key(w, "row_pins"));
    PUT(nd_json_begin_array(w));
    for (i = 0u; i < km->n_rows; i++)
        PUT(nd_json_put_int(w, km->row_pins[i]));
    PUT(nd_json_end_array(w));

    PUT(nd_json_end_object(w));

#undef PUT

    /* Temp file, fsync, rename -- nd_json_writer_save does all three. A torn
     * write here leaves a phone whose only input device does not work. */
    rc = nd_json_writer_save(w, path);

    /* The first-boot wizard writes this as ROOT (core/nd_main.c, 4a-bis: the
     * i2c probe needs it) into a /NeoDCT/User that belongs to ndusr, under a
     * 0027 umask. Left as written, that is a root:root 0640 keymap that the
     * UI cannot read once it has dropped privilege -- a phone that mapped
     * every key and then came up dead to all of them, until the next boot's
     * S00userdata happened to chown it. So the file goes to the directory's
     * owner here, where it is created. A no-op for every other writer (the
     * KeypadMapperI2C app, the tests) and on an image with no ndusr.
     *
     * Failing to hand it over is logged and NOT an error: the keymap is on
     * disk and correct, and the wizard restarting the UI over it is still
     * the best next step -- the log line is what a debugger needs. */
    if (rc == ND_OK && !nd_path_give_to_dir_owner(path))
        nd_log_err(ND_LOG_INPUT,
                   "Keymap %s written, but could not be given to its directory's owner: %s",
                   path, strerror(errno));

done:
    nd_json_writer_free(w);
    return rc;
}
