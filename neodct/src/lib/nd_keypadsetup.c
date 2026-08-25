/* nd_keypadsetup.c -- first-boot on-screen keypad setup.
 *
 * System/hw/i2c_keypad_setup.py, ported. nd_keypadsetup.h has the argument
 * for the shape of the module; this file has the two things a reader of the
 * Python will want checked, and the one place the bytes on disk differ.
 *
 * ============ THE PAYLOAD, AND THE ONE DIFFERENCE ============
 *
 * _save_keymap writes json.dump(payload, indent=2, sort_keys=True) plus a
 * newline, atomically. So does nd_keymap_save(), which nd_keypad.h introduces
 * as the writer of "the first-boot wizard's payload" and which this file uses
 * rather than rendering a second copy of the same JSON.
 *
 * Two fields of the Python's payload have nowhere to live in an nd_keymap and
 * are therefore NOT written:
 *
 *   "generated_at_unix": int(time.time())
 *   keys.<name>.label:  "NaviKey (center)", "C (clear/back)", ...
 *
 * Neither is read by anything. nd_keymap_load() ignores both -- it takes
 * row/col out of each key entry and the format/driver/i2c fields off the top
 * -- and so does the Python's own _load_matrix_keymap. The label is the
 * prompt text, which lives in nd_kpsetup_targets[] where the wizard can
 * actually use it, and the timestamp was only ever a note to a human reading
 * the file. apps/KeypadMapperI2C writes both because its payload is asserted
 * byte-for-byte against CPython; this one is asserted through the loader,
 * which is the property that keeps a phone's keys working.
 *
 * Everything else is identical: the format string, the driver string, the
 * decimal i2c_addr, the row/col INDICES alongside the row_pin/col_pin numbers,
 * the empty "by_code" and the "row,col" -> name "by_matrix".
 *
 * ============ THE THREE THINGS THAT LOOK WRONG AND ARE NOT ============
 *
 *   1. rectangle((0, 0, UI_W, UI_H)) names x=240 and y=175 on a 240x175
 *      canvas, i.e. one column and one row past the end. Pillow clips it and
 *      so does nd_draw_rect_fill(); the coordinates are the Python's and they
 *      stay.
 *
 *   2. The progress bar is only filled when index > 0, so the first prompt
 *      shows an empty outline and the LAST prompt shows 15/16 of a bar. The
 *      bar reaches its end only on a screen nobody sees.
 *
 *   3. _probe_chip's `except OSError` does `chip.close()` on a name that is
 *      unbound when the very first PCF8575() call is what raised. The
 *      resulting NameError is swallowed by the bare `except Exception` one
 *      line down, so the loop continues and nothing is lost. There is nothing
 *      to port -- it is quirk with no effect -- but it is the kind of thing a
 *      reader diffs the two files over.
 *
 * ============ WHAT CANNOT BE VERIFIED WITHOUT THE HARDWARE ============
 *
 * The test drives every line below through a socketpair standing in for the
 * chip, which is the hook nd_keypad.h describes and which covers the drive
 * words, the bit arithmetic, the pairing, the split and the file. What it
 * cannot cover is that a real PCF8575 on a real bus behaves the way the model
 * says: that 500 us is enough settle time for the phone's membrane, that an
 * unconnected address answers with ENXIO rather than a garbage word, and that
 * the eight-address probe finds the chip the owner actually soldered. Those
 * need /dev/i2c-3 and a keypad on the other end of it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_keypadsetup.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

/* ------------------------------------------------------------------ *
 * The strings, verbatim
 * ------------------------------------------------------------------ */

const char *const nd_kpsetup_title = "Keypad setup";
const char *const nd_kpsetup_press = "Press:";
const char *const nd_kpsetup_press_each = "Press each key as asked.";
const char *const nd_kpsetup_aborted = "Setup aborted";
const char *const nd_kpsetup_no_press = "No key was pressed.";
const char *const nd_kpsetup_without = "Starting without a keymap.";
const char *const nd_kpsetup_failed = "Setup failed";
const char *const nd_kpsetup_saved = "Keymap saved!";
const char *const nd_kpsetup_restarting = "Restarting UI...";
const char *const nd_kpsetup_no_bus_title = "No keypad bus";
const char *const nd_kpsetup_no_chip_title = "No keypad found";
const char *const nd_kpsetup_format = "neodct.keymap.v3.matrix.i2c";
const char *const nd_kpsetup_driver = "pcf8575-i2c";

/* KEY_TARGETS. The comment above it in the Python is load-bearing and comes
 * across with it: "Must mirror System/core/main.py MATRIX_NAME_TO_CODE names
 * and the console builder's enrolment order." A name here that
 * nd_keycode_for_name() does not resolve is a key the phone will not have
 * after the file is written, which is the failure this wizard exists to
 * prevent, so the test asserts all sixteen. */
const nd_kpsetup_target nd_kpsetup_targets[ND_KPSETUP_N_TARGETS] = {
    {"navikey", "NaviKey (center)"},
    {"clear", "C (clear/back)"},
    {"up", "Up"},
    {"down", "Down"},
    {"num_1", "1"},
    {"num_2", "2"},
    {"num_3", "3"},
    {"num_4", "4"},
    {"num_5", "5"},
    {"num_6", "6"},
    {"num_7", "7"},
    {"num_8", "8"},
    {"num_9", "9"},
    {"num_0", "0"},
    {"star", "*"},
    {"hash", "#"},
};

/* ------------------------------------------------------------------ *
 * Time
 * ------------------------------------------------------------------ */

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap(double seconds)
{
    struct timespec ts;

    if (seconds <= 0.0)
        return;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    /* A signal must not shorten a settle time or a dwell the owner is
     * reading, so the remainder is slept out. */
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* ------------------------------------------------------------------ *
 * PairScanner
 * ------------------------------------------------------------------ */

/* The set. Pairs are unordered with a < b, so a 16x16 bitmap of the upper
 * triangle is the whole thing in 32 bytes and membership is a bit test --
 * which is what Python's set() gives scan_pairs() for free and what the
 * "already used" check needs on every keypress. */
typedef struct {
    uint16_t row[ND_KPSETUP_MAX_PINS];
} pair_set;

static void pair_set_clear(pair_set *s)
{
    memset(s, 0, sizeof *s);
}

static bool pair_set_add(pair_set *s, uint8_t a, uint8_t b)
{
    uint16_t mask = (uint16_t)(1u << b);

    if ((s->row[a] & mask) != 0u)
        return false; /* already there; the same switch seen from both ends */
    s->row[a] = (uint16_t)(s->row[a] | mask);
    return true;
}

nd_err nd_kpsetup_scan_pairs(nd_pcf8575 *chip, nd_kpsetup_pair *out, size_t max, size_t *n_out)
{
    pair_set seen;
    size_t n = 0u;
    uint32_t drive;

    if (chip == NULL || out == NULL || n_out == NULL)
        return ND_ERR_INVAL;

    pair_set_clear(&seen);

    for (drive = 0u; drive < (uint32_t)ND_KPSETUP_MAX_PINS; drive++) {
        uint16_t word = 0u;
        uint32_t bit;
        nd_err rc;

        /* 0xFFFF & ~(1 << drive): every pin released to its pull-up except
         * this one, which is driven hard low. */
        rc = nd_pcf8575_write16(chip, (uint16_t)(0xFFFFu & ~(1u << drive)));
        if (rc != ND_OK)
            return ND_ERR_IO;

        nap((double)ND_KPSETUP_SETTLE_US / 1e6);

        rc = nd_pcf8575_read16(chip, &word);
        if (rc != ND_OK)
            return ND_ERR_IO;

        for (bit = 0u; bit < (uint32_t)ND_KPSETUP_MAX_PINS; bit++) {
            uint8_t lo;
            uint8_t hi;

            if (bit == drive)
                continue;
            if ((((uint32_t)word >> bit) & 1u) != 0u)
                continue; /* still pulled high: nothing shorting it here */

            lo = (uint8_t)(drive < bit ? drive : bit);
            hi = (uint8_t)(drive < bit ? bit : drive);
            if (!pair_set_add(&seen, lo, hi))
                continue;
            if (n >= max)
                return ND_ERR_TOOLONG;
            out[n].a = lo;
            out[n].b = hi;
            n++;
        }
    }

    /* Not reached after a bus failure above, because the Python's exception
     * does not reach its own trailing write16 either. */
    if (nd_pcf8575_write16(chip, 0xFFFFu) != ND_OK)
        return ND_ERR_IO;

    *n_out = n;
    return ND_OK;
}

nd_err nd_kpsetup_wait_new_pair(nd_pcf8575 *chip, double timeout_s, nd_kpsetup_pair *out,
                                bool *found)
{
    double deadline;

    if (chip == NULL || out == NULL || found == NULL)
        return ND_ERR_INVAL;

    *found = false;
    deadline = monotonic_now() + timeout_s;

    while (monotonic_now() < deadline) {
        nd_kpsetup_pair pairs[ND_KPSETUP_MAX_PAIRS];
        size_t n = 0u;
        nd_err rc = nd_kpsetup_scan_pairs(chip, pairs, ND_ARRAY_LEN(pairs), &n);

        /* ND_ERR_TOOLONG cannot happen: the buffer is C(16,2) entries and
         * scan_pairs() cannot report more distinct pairs than that. It is
         * still returned rather than swallowed. */
        if (rc != ND_OK)
            return rc;
        if (n == 1u) {
            *out = pairs[0];
            *found = true;
            return ND_OK;
        }
        nap(ND_KPSETUP_POLL_S);
    }
    return ND_OK;
}

nd_err nd_kpsetup_wait_release(nd_pcf8575 *chip, double max_s)
{
    double deadline;
    int empties = 0;

    if (chip == NULL)
        return ND_ERR_INVAL;

    deadline = monotonic_now() + max_s;
    while (monotonic_now() < deadline) {
        nd_kpsetup_pair pairs[ND_KPSETUP_MAX_PAIRS];
        size_t n = 0u;
        nd_err rc = nd_kpsetup_scan_pairs(chip, pairs, ND_ARRAY_LEN(pairs), &n);

        if (rc != ND_OK)
            return rc;
        if (n == 0u) {
            empties++;
            if (empties >= ND_KPSETUP_RELEASE_SCANS)
                return ND_OK;
        } else {
            empties = 0;
        }
        nap(ND_KPSETUP_POLL_S);
    }
    /* Running out of time is not an error. A key that never reads released is
     * a stuck switch, and refusing to continue would leave the owner with no
     * keymap at all -- the Python simply falls out of the loop here. */
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * _bipartition
 * ------------------------------------------------------------------ */

bool nd_kpsetup_bipartition(const nd_kpsetup_pair *pairs, size_t n, uint8_t *side_a, size_t *n_a,
                            uint8_t *side_b, size_t *n_b, uint8_t *conflict_a, uint8_t *conflict_b)
{
    uint16_t adj[ND_KPSETUP_MAX_PINS];
    int8_t colour[ND_KPSETUP_MAX_PINS];
    uint8_t stack[ND_KPSETUP_MAX_PINS];
    size_t depth;
    size_t i;
    uint32_t start;
    size_t count_a = 0u;
    size_t count_b = 0u;

    if (pairs == NULL || side_a == NULL || n_a == NULL || side_b == NULL || n_b == NULL)
        return false;

    memset(adj, 0, sizeof adj);
    memset(colour, -1, sizeof colour);

    for (i = 0u; i < n; i++) {
        uint8_t a = pairs[i].a;
        uint8_t b = pairs[i].b;

        if (a >= ND_KPSETUP_MAX_PINS || b >= ND_KPSETUP_MAX_PINS || a == b)
            return false;
        adj[a] = (uint16_t)(adj[a] | (uint16_t)(1u << b));
        adj[b] = (uint16_t)(adj[b] | (uint16_t)(1u << a));
    }

    /* `for start in sorted(adj)`: the smallest UNCOLOURED pin of every
     * connected component takes colour 0, which is what makes the row/column
     * assignment the same on every run even though the choice itself is
     * arbitrary. A pin with no edges is not in adj at all and appears in
     * neither list -- an expander pin nothing is wired to. */
    for (start = 0u; start < (uint32_t)ND_KPSETUP_MAX_PINS; start++) {
        if (adj[start] == 0u || colour[start] >= 0)
            continue;

        colour[start] = 0;
        stack[0] = (uint8_t)start;
        depth = 1u;

        /* queue.pop() takes from the END, so the Python's traversal is a
         * depth-first one despite the name. The colouring of a bipartite
         * component does not depend on the order; only which clashing edge
         * gets reported first does. */
        while (depth > 0u) {
            uint8_t node = stack[--depth];
            uint32_t nb;

            for (nb = 0u; nb < (uint32_t)ND_KPSETUP_MAX_PINS; nb++) {
                if ((((uint32_t)adj[node] >> nb) & 1u) == 0u)
                    continue;
                if (colour[nb] < 0) {
                    colour[nb] = (int8_t)(1 - colour[node]);
                    if (depth < ND_ARRAY_LEN(stack))
                        stack[depth++] = (uint8_t)nb;
                } else if (colour[nb] == colour[node]) {
                    if (conflict_a != NULL)
                        *conflict_a = node;
                    if (conflict_b != NULL)
                        *conflict_b = (uint8_t)nb;
                    return false;
                }
            }
        }
    }

    for (i = 0u; i < (size_t)ND_KPSETUP_MAX_PINS; i++) {
        if (colour[i] == 0)
            side_a[count_a++] = (uint8_t)i;
        else if (colour[i] == 1)
            side_b[count_b++] = (uint8_t)i;
    }
    *n_a = count_a;
    *n_b = count_b;
    return true;
}

/* ------------------------------------------------------------------ *
 * _build_payload
 * ------------------------------------------------------------------ */

static size_t index_of(const uint8_t *list, size_t n, uint8_t pin, bool *ok)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (list[i] == pin) {
            *ok = true;
            return i;
        }
    }
    *ok = false;
    return 0u;
}

nd_err nd_kpsetup_build_keymap(nd_keymap *out, const nd_kpsetup_pair *pairs, const bool *have,
                               int bus, int addr, char *err, size_t err_sz)
{
    nd_kpsetup_pair distinct[ND_KPSETUP_N_TARGETS];
    pair_set seen;
    size_t n_distinct = 0u;
    uint8_t row_pins[ND_KPSETUP_MAX_PINS];
    uint8_t col_pins[ND_KPSETUP_MAX_PINS];
    size_t n_rows = 0u;
    size_t n_cols = 0u;
    uint8_t clash_a = 0u;
    uint8_t clash_b = 0u;
    size_t i;
    size_t r;
    size_t c;

    if (out == NULL || pairs == NULL || have == NULL)
        return ND_ERR_INVAL;
    if (err != NULL && err_sz > 0u)
        err[0] = '\0';

    /* `all_pairs = set(pair_by_name.values())`. The wizard's own "already
     * used" check means there are never duplicates by the time it gets here,
     * but _build_payload does not rely on that and neither does this. */
    pair_set_clear(&seen);
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        if (!have[i])
            continue;
        if (pairs[i].a >= ND_KPSETUP_MAX_PINS || pairs[i].b >= ND_KPSETUP_MAX_PINS ||
            pairs[i].a >= pairs[i].b)
            return ND_ERR_INVAL;
        if (pair_set_add(&seen, pairs[i].a, pairs[i].b))
            distinct[n_distinct++] = pairs[i];
    }

    if (!nd_kpsetup_bipartition(distinct, n_distinct, row_pins, &n_rows, col_pins, &n_cols,
                                &clash_a, &clash_b)) {
        (void)nd_snprintf(err, err_sz, "P%u/P%u conflict", (unsigned)clash_a, (unsigned)clash_b);
        return ND_ERR_PARSE;
    }

    memset(out, 0, sizeof *out);
    for (r = 0u; r < ND_KEYMAP_MAX_ROWS; r++) {
        for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++)
            out->matrix_to_code[r][c] = -1;
    }
    (void)nd_strlcpy(out->path, ND_PATH_KEYMAP, sizeof out->path);
    (void)nd_strlcpy(out->format, nd_kpsetup_format, sizeof out->format);
    (void)nd_strlcpy(out->driver, nd_kpsetup_driver, sizeof out->driver);
    out->i2c_bus = bus;
    out->i2c_addr = addr;
    memcpy(out->row_pins, row_pins, n_rows);
    out->n_rows = n_rows;
    memcpy(out->col_pins, col_pins, n_cols);
    out->n_cols = n_cols;

    /* KEY_TARGETS order, so the first key that cannot be placed is the one
     * named in the message -- which is the one the owner is told about. */
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        uint8_t a;
        uint8_t b;
        bool a_is_row = false;
        bool b_is_col = false;
        size_t row;
        size_t col;
        int32_t code;

        if (!have[i])
            continue;
        a = pairs[i].a;
        b = pairs[i].b;

        row = index_of(row_pins, n_rows, a, &a_is_row);
        col = index_of(col_pins, n_cols, b, &b_is_col);
        if (!a_is_row || !b_is_col) {
            bool swap_row = false;
            bool swap_col = false;
            size_t row2 = index_of(row_pins, n_rows, b, &swap_row);
            size_t col2 = index_of(col_pins, n_cols, a, &swap_col);

            if (!swap_row || !swap_col) {
                /* Unreachable from a successful bipartition -- both ends of
                 * every edge are coloured and the two colours are the two
                 * lists -- but the Python has the branch and so does this,
                 * because a caller may hand in pairs it never scanned. */
                (void)nd_snprintf(err, err_sz, "key '%s' does not fit the matrix split",
                                  nd_kpsetup_targets[i].name);
                return ND_ERR_PARSE;
            }
            row = row2;
            col = col2;
        }

        code = nd_keycode_for_name(nd_kpsetup_targets[i].name);
        if (code < 0)
            return ND_ERR_INVAL; /* the test forbids this ever being true */
        out->matrix_to_code[row][col] = code;
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The gates
 * ------------------------------------------------------------------ */

bool nd_kpsetup_bus_from_env(const char *raw, int *out)
{
    char text[64];
    char *end = NULL;
    long long v;
    size_t len;
    size_t lead = 0u;

    if (out == NULL)
        return false;
    if (raw == NULL) {
        /* os.environ.get(SETUP_BUS_ENV, DEFAULT_SETUP_BUS) hands int() the
         * integer 3, and int(3) is 3. The default never goes through the
         * string parse and so cannot be rejected by it. */
        *out = ND_KPSETUP_DEFAULT_BUS;
        return true;
    }

    /* int() tolerates surrounding whitespace and nothing else. It also
     * tolerates underscores between digits ("1_0" is ten); that is not
     * reproduced, because nobody sets a bus number that way and strtoll would
     * have to be replaced to do it. */
    len = nd_strlcpy(text, raw, sizeof text);
    if (len >= sizeof text)
        return false;
    while (text[lead] == ' ' || text[lead] == '\t' || text[lead] == '\n' || text[lead] == '\r')
        lead++;
    while (len > lead && (text[len - 1u] == ' ' || text[len - 1u] == '\t' ||
                          text[len - 1u] == '\n' || text[len - 1u] == '\r'))
        text[--len] = '\0';
    if (text[lead] == '\0')
        return false; /* int("") is a ValueError, NOT a fall back to 3 */

    errno = 0;
    v = strtoll(text + lead, &end, 10);
    if (end == text + lead || *end != '\0' || errno != 0)
        return false;
    if (v < (long long)INT32_MIN || v > (long long)INT32_MAX)
        return false;
    *out = (int)v;
    return true;
}

/* f"/dev/i2c-{bus}". */
static nd_err bus_dev_path(char *out, size_t out_sz, int bus)
{
    return nd_snprintf(out, out_sz, "/dev/i2c-%d", bus);
}

nd_kpsetup_gate nd_kpsetup_gate_check(int bus)
{
    char dev[ND_KPSETUP_LINE_MAX];
    bool is_hw;

    if (nd_path_exists(ND_PATH_KEYMAP))
        return ND_KPSETUP_GATE_HAVE_KEYMAP;

    if (bus_dev_path(dev, sizeof dev, bus) != ND_OK)
        return ND_KPSETUP_GATE_QUIET;

    /* _is_real_hardware(): the Rockchip FIQ console only exists on the
     * device, never in QEMU. Same hint the launcher uses, and the reason a
     * dev box stays silent while a phone announces every skip. */
    is_hw = nd_path_exists(ND_PATH_SERIAL_FIQ);

    if (nd_path_exists(dev))
        return ND_KPSETUP_GATE_PROBE;
    if (!is_hw)
        return ND_KPSETUP_GATE_QUIET;
    return ND_KPSETUP_GATE_WAIT_FOR_BUS;
}

nd_err nd_kpsetup_probe(nd_pcf8575 *chip, int bus, int *addr_out)
{
    int addr;

    if (chip == NULL || addr_out == NULL)
        return ND_ERR_INVAL;

    for (addr = ND_KPSETUP_PROBE_FIRST; addr <= ND_KPSETUP_PROBE_LAST; addr++) {
        uint16_t v = 0u;

        /* nd_pcf8575_open() has already closed the descriptor when it fails,
         * so there is nothing here matching the Python's chip.close(). */
        if (nd_pcf8575_open(chip, bus, addr) != ND_OK)
            continue;
        if (nd_pcf8575_write16(chip, 0xFFFFu) == ND_OK && nd_pcf8575_read16(chip, &v) == ND_OK) {
            *addr_out = addr;
            return ND_OK;
        }
        nd_pcf8575_close(chip);
    }
    return ND_ERR_NOTFOUND;
}

/* ------------------------------------------------------------------ *
 * The lines that carry a substitution
 * ------------------------------------------------------------------ */

size_t nd_kpsetup_found_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus, int addr)
{
    if (out == NULL || max < 3u)
        return 0u;
    (void)nd_snprintf(out[0], ND_KPSETUP_LINE_MAX, "Keypad found on bus %d", bus);
    (void)nd_snprintf(out[1], ND_KPSETUP_LINE_MAX, "(PCF8575 at 0x%02X)", (unsigned)addr);
    (void)nd_strlcpy(out[2], nd_kpsetup_press_each, ND_KPSETUP_LINE_MAX);
    return 3u;
}

size_t nd_kpsetup_no_chip_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus)
{
    if (out == NULL || max < 3u)
        return 0u;
    (void)nd_snprintf(out[0], ND_KPSETUP_LINE_MAX, "Nothing answered on /dev/i2c-%d", bus);
    (void)nd_snprintf(out[1], ND_KPSETUP_LINE_MAX, "(addresses 0x%02X-0x%02X).",
                      (unsigned)ND_KPSETUP_PROBE_FIRST, (unsigned)ND_KPSETUP_PROBE_LAST);
    (void)nd_strlcpy(out[2], nd_kpsetup_without, ND_KPSETUP_LINE_MAX);
    return 3u;
}

size_t nd_kpsetup_no_bus_lines(char out[][ND_KPSETUP_LINE_MAX], size_t max, int bus)
{
    if (out == NULL || max < 2u)
        return 0u;
    (void)nd_snprintf(out[0], ND_KPSETUP_LINE_MAX, "/dev/i2c-%d does not exist.", bus);
    (void)nd_strlcpy(out[1], nd_kpsetup_without, ND_KPSETUP_LINE_MAX);
    return 2u;
}

nd_err nd_kpsetup_used_note(char *out, size_t out_sz, const char *label)
{
    return nd_snprintf(out, out_sz, "Already used by '%s'", label != NULL ? label : "");
}

nd_err nd_kpsetup_counter(char *out, size_t out_sz, size_t index, size_t total)
{
    return nd_snprintf(out, out_sz, "%zu/%zu", index + 1u, total);
}

/* ------------------------------------------------------------------ *
 * SetupScreen
 * ------------------------------------------------------------------ */

typedef struct {
    nd_fb *fb; /* borrowed; may be NULL                            */
    nd_image *canvas;
    nd_draw draw;
    nd_font *font_big;
    nd_font *font;
    nd_font *font_small;
} setup_screen;

/* Python's `//`, which floors, against C's `/`, which truncates. They differ
 * only for a negative odd numerator -- a string wider than the 240 px band --
 * and then by one pixel. Ported rather than ignored because a "Could not
 * save: ..." line is exactly the sort of string that overflows. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static bool screen_open(setup_screen *s, nd_fb *fb)
{
    char font_path[ND_PATH_MAX];

    memset(s, 0, sizeof *s);
    s->fb = fb;

    /* 240 * 175 * 3 = 126,000 bytes, the same frame the UI draws into.
     * Owned here; released by screen_close(). */
    s->canvas = nd_image_new_filled(ND_KPSETUP_UI_W, ND_KPSETUP_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (s->canvas == NULL) {
        nd_log_err(ND_LOG_SETUP, "no memory for the setup canvas");
        return false;
    }
    if (nd_draw_bind(&s->draw, s->canvas) != ND_OK) {
        nd_image_free(s->canvas);
        s->canvas = NULL;
        return false;
    }

    /* nd_font_load() resolves nothing itself, so the path is resolved here
     * the way every other font caller does it. A NULL face is drawn as
     * nothing; see the header. */
    if (nd_path_resolve(font_path, sizeof font_path, ND_PATH_FONT) == ND_OK) {
        s->font_big = nd_font_load(font_path, ND_KPSETUP_FONT_BIG_PX);
        s->font = nd_font_load(font_path, ND_KPSETUP_FONT_PX);
        s->font_small = nd_font_load(font_path, ND_KPSETUP_FONT_SMALL_PX);
    }
    if (s->font == NULL)
        nd_log_err(ND_LOG_SETUP, "font load failed; the setup screens will be blank");
    return true;
}

static void screen_close(setup_screen *s)
{
    if (s == NULL)
        return;
    nd_font_free(s->font_big);
    nd_font_free(s->font);
    nd_font_free(s->font_small);
    nd_image_free(s->canvas);
    memset(s, 0, sizeof *s);
}

/* _center(). */
static void screen_center(setup_screen *s, const char *text, const nd_font *f, int32_t y,
                          nd_color c)
{
    int32_t w = 0;

    nd_text_size(f, text, &w, NULL);
    (void)nd_draw_text(&s->draw, floordiv2(ND_KPSETUP_UI_W - w), y, text, f, c);
}

/* The clear both screens open with. UI_W and UI_H really are named as the
 * bottom-right corner on a canvas that ends one pixel earlier; Pillow clips
 * it and so does nd_draw_rect_fill(). */
static void screen_clear(setup_screen *s)
{
    (void)nd_draw_rect_fill(&s->draw, ND_RECT(0, 0, ND_KPSETUP_UI_W, ND_KPSETUP_UI_H), ND_BLACK);
}

static void screen_present(setup_screen *s)
{
    /* ND_ERR_INVAL for a NULL framebuffer, which is the headless case and not
     * a failure worth a line on the console every frame. */
    (void)nd_fb_update(s->fb, s->canvas);
}

/* SetupScreen.message(). */
static void screen_message(setup_screen *s, const char *title, char lines[][ND_KPSETUP_LINE_MAX],
                           size_t n_lines, const char *footer)
{
    int32_t y = 58;
    size_t i;

    screen_clear(s);
    screen_center(s, title, s->font, 18, ND_WHITE);
    for (i = 0u; i < n_lines; i++) {
        screen_center(s, lines[i], s->font_small, y, ND_WHITE);
        y += 20;
    }
    if (footer != NULL)
        screen_center(s, footer, s->font_small, ND_KPSETUP_UI_H - 24, ND_GRAY);
    screen_present(s);
}

/* SetupScreen.prompt(). */
static void screen_prompt(setup_screen *s, const char *label, size_t index, size_t total,
                          const char *note)
{
    char counter[32];
    int32_t w = 0;
    int32_t bar_y = ND_KPSETUP_UI_H - 42;

    screen_clear(s);
    screen_center(s, nd_kpsetup_title, s->font_small, 6, ND_GRAY);

    (void)nd_kpsetup_counter(counter, sizeof counter, index, total);
    nd_text_size(s->font_small, counter, &w, NULL);
    (void)nd_draw_text(&s->draw, ND_KPSETUP_UI_W - 8 - w, 6, counter, s->font_small, ND_WHITE);

    screen_center(s, nd_kpsetup_press, s->font, 46, ND_WHITE);
    screen_center(s, label, s->font_big, 76, ND_WHITE);

    (void)nd_draw_rect_outline(&s->draw, ND_RECT(16, bar_y, ND_KPSETUP_UI_W - 16, bar_y + 8),
                               ND_WHITE, 1);
    /* `if index > 0` -- so the first prompt shows an empty track and the last
     * one shows fifteen sixteenths. The bar is never full on a screen anyone
     * sees, and that is the frame. */
    if (index > 0u) {
        int32_t fill_w =
            nd_trunc32((double)(ND_KPSETUP_UI_W - 34) * ((double)index / (double)total));

        (void)nd_draw_rect_fill(&s->draw, ND_RECT(17, bar_y + 1, 17 + fill_w, bar_y + 7), ND_WHITE);
    }

    if (note != NULL)
        screen_center(s, note, s->font_small, ND_KPSETUP_UI_H - 24, ND_WHITE);
    screen_present(s);
}

/* ------------------------------------------------------------------ *
 * The restart
 * ------------------------------------------------------------------ */

/* os.execv(sys.executable, [sys.executable] + sys.argv).
 *
 * /proc/self/exe is sys.executable and /proc/self/cmdline is sys.argv; there
 * is no other way to recover either from inside a library, and threading the
 * core's argv down here would mean changing a signature in nd_main.c that
 * this work package may not touch.
 *
 * execv() on a process with threads running -- and by this point the clock
 * and the reaper are -- replaces the whole image and every thread with it.
 * That is defined behaviour and it is exactly what the Python did to its own
 * threads. It is NOT the fork() rule in CODING-STANDARDS.md 1.1: there is no
 * fork here, so there is no half-copied allocator to avoid.
 *
 * Returns only on failure. */
static void restart_ui(void)
{
    char exe[ND_PATH_MAX];
    char cmdline[4096];
    char *argv[65];
    size_t n_argv = 0u;
    ssize_t link_len;
    ssize_t got = -1;
    int fd;

    link_len = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (link_len <= 0) {
        nd_log_err(ND_LOG_SETUP, "cannot read /proc/self/exe: %s", strerror(errno));
        return;
    }
    exe[link_len] = '\0';

    fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        got = read(fd, cmdline, sizeof cmdline - 1u);
        (void)close(fd);
    }
    if (got > 0) {
        ssize_t i;
        bool at_start = true;

        cmdline[got] = '\0';
        for (i = 0; i < got && n_argv < ND_ARRAY_LEN(argv) - 1u; i++) {
            if (at_start) {
                argv[n_argv++] = &cmdline[i];
                at_start = false;
            }
            if (cmdline[i] == '\0')
                at_start = true;
        }
    }
    /* An unreadable or empty cmdline still restarts, with no arguments. A
     * phone that has just written its keymap must come back up. */
    if (n_argv == 0u)
        argv[n_argv++] = exe;
    argv[n_argv] = NULL;

    /* The Python execs without announcing it; the "Restarting UI..." screen
     * is the announcement. Only the failure gets a line. */
    (void)fflush(NULL);
    (void)execv(exe, argv);
    nd_log_err(ND_LOG_SETUP, "could not restart %s: %s", exe, strerror(errno));
}

/* ------------------------------------------------------------------ *
 * run_wizard
 * ------------------------------------------------------------------ */

bool nd_kpsetup_run_wizard(nd_fb *fb, nd_pcf8575 *chip, int addr, int bus, bool restart)
{
    setup_screen screen;
    nd_kpsetup_pair pair_by_target[ND_KPSETUP_N_TARGETS];
    bool have[ND_KPSETUP_N_TARGETS];
    /* used_pairs, as a flat table rather than a dict: 256 borrowed label
     * pointers, no allocation, and the "already used" test is one load. */
    const char *used_label[ND_KPSETUP_MAX_PINS][ND_KPSETUP_MAX_PINS];
    char lines[3][ND_KPSETUP_LINE_MAX];
    char note_buf[ND_KPSETUP_LINE_MAX];
    char err[ND_KPSETUP_ERR_MAX];
    const char *note = NULL;
    nd_keymap km;
    size_t total = ND_KPSETUP_N_TARGETS;
    size_t index;
    size_t enrolled = 0u;
    bool ok = false;

    if (chip == NULL)
        return false;

    memset(pair_by_target, 0, sizeof pair_by_target);
    memset(have, 0, sizeof have);
    memset(used_label, 0, sizeof used_label);

    if (!screen_open(&screen, fb))
        return false;

    (void)nd_kpsetup_found_lines(lines, ND_ARRAY_LEN(lines), bus, addr);
    screen_message(&screen, nd_kpsetup_title, lines, 3u, NULL);
    nap(ND_KPSETUP_DWELL_INTRO_S);

    for (index = 0u; index < total; index++) {
        double timeout = (index == 0u) ? ND_KPSETUP_FIRST_KEY_TIMEOUT : ND_KPSETUP_KEY_TIMEOUT;

        for (;;) {
            nd_kpsetup_pair pair;
            bool found = false;

            screen_prompt(&screen, nd_kpsetup_targets[index].label, index, total, note);

            if (nd_kpsetup_wait_new_pair(chip, timeout, &pair, &found) != ND_OK)
                goto bus_failed;

            if (!found) {
                (void)nd_strlcpy(lines[0], nd_kpsetup_no_press, ND_KPSETUP_LINE_MAX);
                (void)nd_strlcpy(lines[1], nd_kpsetup_without, ND_KPSETUP_LINE_MAX);
                screen_message(&screen, nd_kpsetup_aborted, lines, 2u, NULL);
                nap(ND_KPSETUP_DWELL_ABORT_S);
                goto done;
            }

            if (used_label[pair.a][pair.b] != NULL) {
                (void)nd_kpsetup_used_note(note_buf, sizeof note_buf, used_label[pair.a][pair.b]);
                note = note_buf;
                if (nd_kpsetup_wait_release(chip, ND_KPSETUP_RELEASE_MAX_S) != ND_OK)
                    goto bus_failed;
                continue;
            }

            pair_by_target[index] = pair;
            have[index] = true;
            enrolled++;
            used_label[pair.a][pair.b] = nd_kpsetup_targets[index].label;
            /* Cleared on success, so a note never survives into the next
             * key's prompt. */
            note = NULL;
            if (nd_kpsetup_wait_release(chip, ND_KPSETUP_RELEASE_MAX_S) != ND_OK)
                goto bus_failed;
            break;
        }
    }

    if (nd_kpsetup_build_keymap(&km, pair_by_target, have, bus, addr, err, sizeof err) != ND_OK) {
        (void)nd_strlcpy(lines[0], err, ND_KPSETUP_LINE_MAX);
        (void)nd_strlcpy(lines[1], nd_kpsetup_without, ND_KPSETUP_LINE_MAX);
        screen_message(&screen, nd_kpsetup_failed, lines, 2u, NULL);
        nap(ND_KPSETUP_DWELL_FAILED_S);
        goto done;
    }

    {
        nd_err rc = nd_keymap_save(&km, ND_PATH_KEYMAP);

        if (rc != ND_OK) {
            /* f"Could not save: {exc}". A C error name where Python had an
             * exception's str(), which is the closest this gets to the text
             * the owner would have read. */
            (void)nd_snprintf(lines[0], ND_KPSETUP_LINE_MAX, "Could not save: %s", nd_strerror(rc));
            screen_message(&screen, nd_kpsetup_failed, lines, 1u, NULL);
            nap(ND_KPSETUP_DWELL_FAILED_S);
            goto done;
        }
    }

    (void)nd_snprintf(lines[0], ND_KPSETUP_LINE_MAX, "%zu keys mapped.", enrolled);
    (void)nd_strlcpy(lines[1], nd_kpsetup_restarting, ND_KPSETUP_LINE_MAX);
    screen_message(&screen, nd_kpsetup_saved, lines, 2u, NULL);
    nap(ND_KPSETUP_DWELL_SAVED_S);

    ok = true;
    if (restart) {
        /* Everything the process still owns goes back before the image is
         * replaced -- the chip first, exactly as the Python does, so no pin
         * is left driven low across the restart. */
        nd_pcf8575_close(chip);
        screen_close(&screen);
        restart_ui();
        /* Only here if execv() failed. The keymap is on disk either way, so
         * the caller carries on and the UI comes up without it until the next
         * reboot. */
        return true;
    }
    goto done;

bus_failed:
    /* main.py wraps this whole call in a try/except and prints exactly this
     * line before carrying on. C has no exception to catch, so the module
     * that would have raised prints it instead. */
    nd_log_err(ND_LOG_SETUP, "First-time keypad setup failed; continuing boot.");

done:
    screen_close(&screen);
    return ok;
}

/* ------------------------------------------------------------------ *
 * maybe_run_first_time_setup
 * ------------------------------------------------------------------ */

bool nd_kpsetup_maybe_run(nd_fb *fb, bool restart)
{
    char dev[ND_KPSETUP_LINE_MAX];
    char lines[3][ND_KPSETUP_LINE_MAX];
    setup_screen screen;
    nd_pcf8575 chip;
    nd_kpsetup_gate gate;
    int bus = ND_KPSETUP_DEFAULT_BUS;
    int addr = 0;
    bool ran = false;

    if (!nd_kpsetup_bus_from_env(getenv(ND_KPSETUP_ENV_BUS), &bus)) {
        /* int() raised, which in the Python unwinds all the way out of
         * maybe_run_first_time_setup and into run()'s except clause. Same
         * outcome: no wizard, one line, boot continues. */
        nd_log_err(ND_LOG_SETUP, "First-time keypad setup failed; continuing boot.");
        return false;
    }

    gate = nd_kpsetup_gate_check(bus);
    if (gate == ND_KPSETUP_GATE_HAVE_KEYMAP) {
        nd_log(ND_LOG_SETUP, "Keymap already present (%s); skipping setup.", ND_PATH_KEYMAP);
        return false;
    }
    /* QEMU and dev boxes: no bus, no keypad, stay quiet. */
    if (gate == ND_KPSETUP_GATE_QUIET)
        return false;

    if (bus_dev_path(dev, sizeof dev, bus) != ND_OK)
        return false;

    if (!screen_open(&screen, fb))
        return false;

    if (gate == ND_KPSETUP_GATE_WAIT_FOR_BUS) {
        /* The i2c device node can appear a moment after the UI starts (udev
         * coldplug); give it a few seconds on hardware. */
        double deadline;

        (void)nd_snprintf(lines[0], ND_KPSETUP_LINE_MAX, "Waiting for %s...", dev);
        screen_message(&screen, nd_kpsetup_title, lines, 1u, NULL);

        deadline = monotonic_now() + ND_KPSETUP_BUS_WAIT_S;
        while (!nd_path_exists(dev) && monotonic_now() < deadline)
            nap(ND_KPSETUP_BUS_POLL_S);

        if (!nd_path_exists(dev)) {
            nd_log(ND_LOG_SETUP,
                   "%s never appeared; no keymap and no bus. Set %s if the keypad is on another "
                   "bus.",
                   dev, ND_KPSETUP_ENV_BUS);
            (void)nd_kpsetup_no_bus_lines(lines, ND_ARRAY_LEN(lines), bus);
            screen_message(&screen, nd_kpsetup_no_bus_title, lines, 2u, NULL);
            nap(ND_KPSETUP_DWELL_NO_BUS_S);
            goto done;
        }
    }

    if (nd_kpsetup_probe(&chip, bus, &addr) != ND_OK) {
        nd_log(ND_LOG_SETUP, "No PCF8575 answered on %s (tried 0x%02X-0x%02X).", dev,
               (unsigned)ND_KPSETUP_PROBE_FIRST, (unsigned)ND_KPSETUP_PROBE_LAST);
        (void)nd_kpsetup_no_chip_lines(lines, ND_ARRAY_LEN(lines), bus);
        screen_message(&screen, nd_kpsetup_no_chip_title, lines, 3u, NULL);
        nap(ND_KPSETUP_DWELL_NO_CHIP_S);
        goto done;
    }

    nd_log(ND_LOG_SETUP,
           "No keymap; PCF8575 found at 0x%02X on bus %d. Starting first-time keypad setup.",
           (unsigned)addr, bus);

    /* The wizard draws its own screens, so this one is put away first rather
     * than holding a second 126,000-byte canvas and three faces open beside
     * it. The Python builds a second SetupScreen and leaves the first to the
     * garbage collector; the pixels are the same either way. */
    screen_close(&screen);
    ran = nd_kpsetup_run_wizard(fb, &chip, addr, bus, restart);
    /* The Python's `finally`. Reached only when the wizard did not exec. */
    nd_pcf8575_close(&chip);
    return ran;

done:
    screen_close(&screen);
    return false;
}
