/* test_keypad.c -- the fake-i2c harness, the matrix scanner and the keymap.
 *
 * This code had ZERO tests before this file. PORT-PLAN.md rung 6 asks for a
 * fake-i2c harness written BEFORE the C, faking THE CHIP rather than the
 * driver, and that is what this is: a socketpair standing in for
 * /dev/i2c-3, seeded with the sixteen-bit words a PCF8575 would return.
 * Everything above the descriptor -- the row-drive arithmetic, the column bit
 * test, the edge detection, the release debounce, the pending queue and the
 * keymap lookup -- is the code that actually ships.
 *
 * ============ HOW THE FAKE CHIP WORKS ============
 *
 * A socketpair is two descriptors joined back to back, and each direction has
 * its own buffer. So the driver's write()s pile up on the test's end where
 * they can be inspected afterwards, and the test's write()s are what the
 * driver's read()s return. That is exactly the shape of an i2c transaction
 * against a chip with no register byte.
 *
 * The driver's end is non-blocking, deliberately: a scan that reads more
 * words than the test seeded fails immediately with ND_ERR_IO instead of
 * hanging the test suite.
 *
 * ============ THE SCAN, IN BYTES ============
 *
 * With rows P00-P03 and columns P04-P07 the driver writes, per pass:
 *   0xFFFE  drive row 0 low   -> read
 *   0xFFFD  drive row 1 low   -> read
 *   0xFFFB  drive row 2 low   -> read
 *   0xFFF7  drive row 3 low   -> read
 *   0xFFFF  release everything
 * and a column bit reading LOW in one of those reads is a pressed key.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_keycodes.h"
#include "nd_keypad.h"

#include "platform_test.h"

static const uint8_t ROWS[4] = {0, 1, 2, 3};
static const uint8_t COLS[4] = {4, 5, 6, 7};

/* The word a chip returns when the keys in `cols` are shorting the driven
 * row. Everything else reads high, because of the internal pull-ups. */
static uint16_t scan_word(const uint8_t *cols, size_t n)
{
    uint16_t v = 0xFFFFu;
    size_t i;

    for (i = 0u; i < n; i++)
        v = (uint16_t)(v & ~(uint16_t)(1u << COLS[cols[i]]));
    return v;
}

#define NO_KEYS scan_word(NULL, 0u)

typedef struct {
    int driver_fd;
    int chip_fd;
} fake_chip;

static void fake_chip_open(fake_chip *fc)
{
    int fds[2];
    int flags;

    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fc->driver_fd = fds[0];
    fc->chip_fd = fds[1];

    flags = fcntl(fc->driver_fd, F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK_INT(fcntl(fc->driver_fd, F_SETFL, flags | O_NONBLOCK), 0);
    flags = fcntl(fc->chip_fd, F_GETFL, 0);
    CHECK_INT(fcntl(fc->chip_fd, F_SETFL, flags | O_NONBLOCK), 0);
}

static void fake_chip_close(fake_chip *fc)
{
    if (fc->driver_fd >= 0)
        (void)close(fc->driver_fd);
    if (fc->chip_fd >= 0)
        (void)close(fc->chip_fd);
    fc->driver_fd = -1;
    fc->chip_fd = -1;
}

/* Queue one 16-bit answer, low byte first, as the chip puts it on the wire. */
static void chip_reply(fake_chip *fc, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)(value & 0xFFu);
    b[1] = (uint8_t)((value >> 8) & 0xFFu);
    CHECK_INT(write(fc->chip_fd, b, 2u), 2);
}

/* Queue the four answers one full pass consumes. */
static void chip_reply_pass(fake_chip *fc, const uint16_t rows[4])
{
    size_t i;

    for (i = 0u; i < 4u; i++)
        chip_reply(fc, rows[i]);
}

/* Everything the driver has written since the last drain. */
static size_t chip_drain(fake_chip *fc, uint16_t *out, size_t max)
{
    uint8_t buf[512];
    ssize_t got = read(fc->chip_fd, buf, sizeof buf);
    size_t n = 0u;
    size_t i;

    if (got <= 0)
        return 0u;
    for (i = 0u; i + 1u < (size_t)got && n < max; i += 2u)
        out[n++] = (uint16_t)((uint16_t)buf[i] | (uint16_t)((uint16_t)buf[i + 1] << 8));
    return n;
}

/* ------------------------------------------------------------------ *
 * The chip driver
 * ------------------------------------------------------------------ */

static void test_write16_is_low_byte_first(void)
{
    fake_chip fc;
    nd_pcf8575 c;
    uint8_t buf[2];

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&c, fc.driver_fd), ND_OK);
    CHECK_INT(nd_pcf8575_write16(&c, 0x1234u), ND_OK);
    CHECK_INT(read(fc.chip_fd, buf, 2u), 2);
    CHECK_INT(buf[0], 0x34);
    CHECK_INT(buf[1], 0x12);
    fake_chip_close(&fc);
}

static void test_read16_reassembles_low_byte_first(void)
{
    fake_chip fc;
    nd_pcf8575 c;
    uint16_t v = 0u;

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&c, fc.driver_fd), ND_OK);
    chip_reply(&fc, 0xBEEFu);
    CHECK_INT(nd_pcf8575_read16(&c, &v), ND_OK);
    CHECK_INT(v, 0xBEEF);
    fake_chip_close(&fc);
}

static void test_a_short_read_is_an_error_not_a_guess(void)
{
    fake_chip fc;
    nd_pcf8575 c;
    uint16_t v = 0u;
    uint8_t one = 0x11u;

    /* A one-byte answer is a chip that is not there or a bus that glitched.
     * Inventing the missing byte would produce phantom keypresses. */
    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&c, fc.driver_fd), ND_OK);
    CHECK_INT(write(fc.chip_fd, &one, 1u), 1);
    CHECK_INT(nd_pcf8575_read16(&c, &v), ND_ERR_IO);
    fake_chip_close(&fc);
}

static void test_close_releases_every_pin(void)
{
    fake_chip fc;
    nd_pcf8575 c;
    uint16_t words[4];

    /* Nothing may be left driven low across a restart, or the next boot's
     * first scan sees a key that is not pressed. */
    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&c, fc.driver_fd), ND_OK);
    nd_pcf8575_close(&c);
    CHECK_INT(chip_drain(&fc, words, 4u), 1);
    CHECK_INT(words[0], 0xFFFF);
    /* An attached descriptor is not closed by the driver. */
    CHECK_INT(c.fd, -1);
    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * Pin validation
 * ------------------------------------------------------------------ */

static void test_pins_out_of_range_and_repeated_are_refused(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    uint8_t bad_range[4] = {0, 1, 2, 16};
    uint8_t repeated[4] = {0, 1, 2, 4};

    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, bad_range, 4u, COLS, 4u, fc.driver_fd), ND_ERR_INVAL);
    /* 4 is already a column pin, so it cannot also be a row pin. */
    CHECK_INT(nd_matrix_scanner_init_fd(&s, repeated, 4u, COLS, 4u, fc.driver_fd), ND_ERR_INVAL);
    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * The scan
 * ------------------------------------------------------------------ */

static void test_a_pass_drives_every_row_and_releases_after(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    nd_matrix_pos pos;
    bool found = true;
    uint16_t rows[4];
    uint16_t words[16];
    size_t n;

    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, ROWS, 4u, COLS, 4u, fc.driver_fd), ND_OK);
    /* init's own 0xFFFF */
    CHECK_INT(chip_drain(&fc, words, 16u), 1);

    rows[0] = NO_KEYS;
    rows[1] = NO_KEYS;
    rows[2] = NO_KEYS;
    rows[3] = NO_KEYS;
    chip_reply_pass(&fc, rows);

    CHECK_INT(nd_matrix_scan_once(&s, &pos, &found), ND_OK);
    CHECK(!found);

    n = chip_drain(&fc, words, 16u);
    CHECK_INT(n, 5);
    CHECK_INT(words[0], 0xFFFE);
    CHECK_INT(words[1], 0xFFFD);
    CHECK_INT(words[2], 0xFFFB);
    CHECK_INT(words[3], 0xFFF7);
    CHECK_INT(words[4], 0xFFFF);

    nd_matrix_scanner_close(&s);
    fake_chip_close(&fc);
}

/* Run one pass with the given per-row column sets. */
static bool run_pass(fake_chip *fc, nd_matrix_scanner *s, const uint16_t rows[4],
                     nd_matrix_pos *pos)
{
    uint16_t sink[16];
    bool found = false;

    chip_reply_pass(fc, rows);
    CHECK_INT(nd_matrix_scan_once(s, pos, &found), ND_OK);
    (void)chip_drain(fc, sink, 16u);
    return found;
}

static void test_one_press_is_reported_exactly_once(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    nd_matrix_pos pos;
    uint8_t col1[1] = {1};
    uint16_t rows[4];
    uint16_t sink[16];
    int i;

    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, ROWS, 4u, COLS, 4u, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    rows[0] = scan_word(col1, 1u); /* R0 C1 held down */
    rows[1] = NO_KEYS;
    rows[2] = NO_KEYS;
    rows[3] = NO_KEYS;

    CHECK(run_pass(&fc, &s, rows, &pos));
    CHECK_INT(pos.row, 0);
    CHECK_INT(pos.col, 1);

    /* Still down for the next five passes; edge detection means silence. */
    for (i = 0; i < 5; i++)
        CHECK(!run_pass(&fc, &s, rows, &pos));
    CHECK(nd_matrix_is_held(&s, 0u, 1u));

    nd_matrix_scanner_close(&s);
    fake_chip_close(&fc);
}

static void test_release_needs_three_empty_scans(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    nd_matrix_pos pos;
    uint8_t col1[1] = {1};
    uint16_t down[4];
    uint16_t up[4];
    uint16_t sink[16];

    /* A membrane contact chatters on the way up. RELEASE_SCANS = 3 at the
     * 5 ms poll cadence is about 15 ms of debounce; a single dropped scan
     * must not re-fire the key. */
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, ROWS, 4u, COLS, 4u, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    down[0] = scan_word(col1, 1u);
    down[1] = NO_KEYS;
    down[2] = NO_KEYS;
    down[3] = NO_KEYS;
    up[0] = NO_KEYS;
    up[1] = NO_KEYS;
    up[2] = NO_KEYS;
    up[3] = NO_KEYS;

    CHECK(run_pass(&fc, &s, down, &pos));

    (void)run_pass(&fc, &s, up, &pos); /* missing 1 */
    CHECK(nd_matrix_is_held(&s, 0u, 1u));
    (void)run_pass(&fc, &s, up, &pos); /* missing 2 */
    CHECK(nd_matrix_is_held(&s, 0u, 1u));

    /* A blip back to pressed inside the window is NOT a new press. */
    CHECK(!run_pass(&fc, &s, down, &pos));

    (void)run_pass(&fc, &s, up, &pos);
    (void)run_pass(&fc, &s, up, &pos);
    (void)run_pass(&fc, &s, up, &pos); /* missing 3 -> gone */
    CHECK(!nd_matrix_is_held(&s, 0u, 1u));

    /* And now the same key is a fresh press again. */
    CHECK(run_pass(&fc, &s, down, &pos));
    CHECK_INT(pos.row, 0);
    CHECK_INT(pos.col, 1);

    nd_matrix_scanner_close(&s);
    fake_chip_close(&fc);
}

static void test_simultaneous_presses_queue_one_per_call(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    nd_matrix_pos pos;
    nd_matrix_pos held[8];
    uint8_t c0[1] = {0};
    uint8_t c2[1] = {2};
    uint16_t rows[4];
    uint16_t sink[16];

    /* The whole matrix is scanned every pass, so a second key pressed while
     * one is held IS seen -- it just waits its turn. Games miss direction
     * changes if either half of that is wrong. */
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, ROWS, 4u, COLS, 4u, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    rows[0] = scan_word(c0, 1u); /* R0 C0 */
    rows[1] = NO_KEYS;
    rows[2] = scan_word(c2, 1u); /* R2 C2 */
    rows[3] = NO_KEYS;

    CHECK(run_pass(&fc, &s, rows, &pos));
    CHECK_INT(pos.row, 0);
    CHECK_INT(pos.col, 0);
    CHECK_INT(nd_matrix_held(&s, held, 8u), 2);

    /* Nothing new this pass, so the queued one comes out. */
    CHECK(run_pass(&fc, &s, rows, &pos));
    CHECK_INT(pos.row, 2);
    CHECK_INT(pos.col, 2);

    CHECK(!run_pass(&fc, &s, rows, &pos));

    nd_matrix_scanner_close(&s);
    fake_chip_close(&fc);
}

static void test_a_bus_failure_is_reported_not_swallowed(void)
{
    fake_chip fc;
    nd_matrix_scanner s;
    nd_matrix_pos pos;
    bool found = true;
    uint16_t sink[16];

    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_init_fd(&s, ROWS, 4u, COLS, 4u, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    /* No seeded answers at all: the first read fails. */
    CHECK_INT(nd_matrix_scan_once(&s, &pos, &found), ND_ERR_IO);
    CHECK(!found);

    nd_matrix_scanner_close(&s);
    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * The keymapped backend
 * ------------------------------------------------------------------ */

static void fill_keymap(nd_keymap *km)
{
    size_t r;
    size_t c;

    memset(km, 0, sizeof *km);
    for (r = 0u; r < ND_KEYMAP_MAX_ROWS; r++) {
        for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++)
            km->matrix_to_code[r][c] = -1;
    }
    (void)nd_strlcpy(km->path, "/User/keymap.json", sizeof km->path);
    (void)nd_strlcpy(km->format, "neodct.keymap.v3.matrix.i2c", sizeof km->format);
    (void)nd_strlcpy(km->driver, "pcf8575-i2c", sizeof km->driver);
    memcpy(km->row_pins, ROWS, sizeof ROWS);
    memcpy(km->col_pins, COLS, sizeof COLS);
    km->n_rows = 4u;
    km->n_cols = 4u;
    km->i2c_bus = 3;
    km->i2c_addr = 0x20;
    km->matrix_to_code[0][0] = ND_KEY_NAVIKEY; /* 28 */
    km->matrix_to_code[0][1] = ND_KEY_CLEAR;   /* 14 */
    km->matrix_to_code[1][0] = ND_KEY_UP;      /* 103 */
    km->matrix_to_code[1][1] = ND_KEY_DOWN;    /* 108 */
    km->matrix_to_code[2][0] = ND_KEY_1;       /* 2 */
    km->matrix_to_code[3][3] = ND_KEY_HASH;    /* 43 */
}

static void test_a_mapped_position_becomes_a_keycode(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    uint8_t c1[1] = {1};
    uint16_t rows[4];
    uint16_t sink[16];

    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    rows[0] = NO_KEYS;
    rows[1] = scan_word(c1, 1u); /* R1 C1 = down */
    rows[2] = NO_KEYS;
    rows[3] = NO_KEYS;
    chip_reply_pass(&fc, rows);
    CHECK_INT(nd_matrix_input_poll(&in), ND_KEY_DOWN);
    (void)chip_drain(&fc, sink, 16u);

    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

static void test_an_unmapped_position_produces_no_key(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    uint8_t c3[1] = {3};
    uint16_t rows[4];
    uint16_t sink[16];

    /* An unenrolled key is not an error and must not be invented into
     * something else; the log line is rate-limited elsewhere. */
    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    rows[0] = scan_word(c3, 1u); /* R0 C3 is not in the keymap */
    rows[1] = NO_KEYS;
    rows[2] = NO_KEYS;
    rows[3] = NO_KEYS;
    chip_reply_pass(&fc, rows);
    CHECK_INT(nd_matrix_input_poll(&in), ND_KEY_NONE);
    (void)chip_drain(&fc, sink, 16u);

    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

static void test_read_key_scans_at_least_once_at_timeout_zero(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    uint8_t c0[1] = {0};
    uint16_t rows[4];
    uint16_t sink[16];

    /* Browser/main.py:_drain_input calls read_key(0) precisely to consume a
     * press the scanner already has. A loop that checks the deadline first
     * would consume nothing and the key would arrive on the next screen. */
    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    (void)chip_drain(&fc, sink, 16u);

    rows[0] = scan_word(c0, 1u); /* R0 C0 = navikey */
    rows[1] = NO_KEYS;
    rows[2] = NO_KEYS;
    rows[3] = NO_KEYS;
    chip_reply_pass(&fc, rows);
    CHECK_INT(nd_matrix_input_read_key(&in, 0.0), ND_KEY_ENTER);
    (void)chip_drain(&fc, sink, 16u);

    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * The keymap file
 * ------------------------------------------------------------------ */

static const char GOOD_KEYMAP[] =
    "{\n"
    "  \"by_code\": {},\n"
    "  \"by_matrix\": {\"0,0\": \"navikey\"},\n"
    "  \"col_pins\": [4, 5, 6, 7],\n"
    "  \"driver\": \"pcf8575-i2c\",\n"
    "  \"format\": \"neodct.keymap.v3.matrix.i2c\",\n"
    "  \"generated_at_unix\": 1740000000,\n"
    "  \"i2c_addr\": 32,\n"
    "  \"i2c_bus\": 3,\n"
    "  \"keys\": {\n"
    "    \"navikey\": {\"col\": 0, \"col_pin\": 4, \"label\": \"NaviKey (center)\","
    " \"row\": 0, \"row_pin\": 0},\n"
    "    \"clear\": {\"col\": 1, \"col_pin\": 5, \"row\": 0, \"row_pin\": 0},\n"
    "    \"num_7\": {\"col\": 2, \"col_pin\": 6, \"row\": 3, \"row_pin\": 3}\n"
    "  },\n"
    "  \"output\": \"/NeoDCT/User/keymap.json\",\n"
    "  \"row_pins\": [0, 1, 2, 3]\n"
    "}\n";

static void test_a_good_keymap_loads(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", GOOD_KEYMAP);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_STR(km.driver, "pcf8575-i2c");
    CHECK_STR(km.format, "neodct.keymap.v3.matrix.i2c");
    CHECK_INT(km.n_rows, 4);
    CHECK_INT(km.n_cols, 4);
    CHECK_INT(km.row_pins[0], 0);
    CHECK_INT(km.col_pins[3], 7);
    CHECK_INT(km.i2c_addr, 0x20);
    CHECK_INT(km.i2c_bus, 3);
    CHECK_INT(km.matrix_to_code[0][0], 28);
    CHECK_INT(km.matrix_to_code[0][1], 14);
    CHECK_INT(km.matrix_to_code[3][2], 8); /* num_7 */
    CHECK_INT(km.matrix_to_code[2][2], -1);
}

static void test_a_missing_keymap_is_not_an_error(void)
{
    nd_keymap km;

    /* ND_ERR_NOTFOUND means "no matrix keypad on this device", which is the
     * normal case in QEMU and is deliberately silent. */
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_NOTFOUND);
}

static void test_broken_json_is_refused(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", "{\"row_pins\": [0,1,");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);
}

static void test_missing_matrix_fields_are_refused(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", "{\"row_pins\": 3, \"col_pins\": [4], \"keys\": {}}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);

    pt_write_text("/User/keymap.json", "{\"row_pins\": [0], \"col_pins\": [4], \"keys\": []}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);
}

static void test_an_unparseable_pin_is_refused(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", "{\"row_pins\": [\"zero\"], \"col_pins\": [4],"
                                       " \"keys\": {\"up\": {\"row\": 0, \"col\": 0}}}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);
}

static void test_unknown_and_malformed_entries_are_skipped_not_fatal(void)
{
    nd_keymap km;

    /* A keymap missing one key still boots a phone you can fix; a keymap
     * rejected over one key does not. */
    pt_write_text("/User/keymap.json", "{\"row_pins\": [0,1], \"col_pins\": [4,5], \"keys\": {"
                                       " \"not_a_key_name\": {\"row\": 0, \"col\": 0},"
                                       " \"up\": \"not an object\","
                                       " \"down\": {\"row\": \"x\", \"col\": 1},"
                                       " \"left\": {\"row\": 99, \"col\": 1},"
                                       " \"right\": {\"row\": 1, \"col\": 1}}}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_INT(km.matrix_to_code[1][1], 106); /* right survived */
    CHECK_INT(km.matrix_to_code[0][0], -1);
    CHECK_INT(km.matrix_to_code[0][1], -1);
}

static void test_no_recognised_keys_is_refused(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", "{\"row_pins\": [0], \"col_pins\": [4],"
                                       " \"keys\": {\"wat\": {\"row\": 0, \"col\": 0}}}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);
}

static void test_the_i2c_address_accepts_three_spellings(void)
{
    nd_keymap km;
    const char *tmpl = "{\"row_pins\": [0], \"col_pins\": [4],"
                       " \"keys\": {\"up\": {\"row\": 0, \"col\": 0}}, \"i2c_addr\": %s}";
    char buf[256];

    /* The wizard writes an int; two other tools have written the string
     * forms by hand, and the Python loader accepts all three. */
    CHECK(snprintf(buf, sizeof buf, tmpl, "32") > 0);
    pt_write_text("/User/keymap.json", buf);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_INT(km.i2c_addr, 0x20);

    CHECK(snprintf(buf, sizeof buf, tmpl, "\"0x21\"") > 0);
    pt_write_text("/User/keymap.json", buf);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_INT(km.i2c_addr, 0x21);

    CHECK(snprintf(buf, sizeof buf, tmpl, "\"34\"") > 0);
    pt_write_text("/User/keymap.json", buf);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_INT(km.i2c_addr, 34);

    CHECK(snprintf(buf, sizeof buf, tmpl, "\"nope\"") > 0);
    pt_write_text("/User/keymap.json", buf);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_ERR_PARSE);
}

static void test_absent_fields_take_the_python_defaults(void)
{
    nd_keymap km;

    pt_write_text("/User/keymap.json", "{\"row_pins\": [0], \"col_pins\": [4],"
                                       " \"keys\": {\"up\": {\"row\": 0, \"col\": 0}}}");
    CHECK_INT(nd_keymap_load("/User/keymap.json", &km), ND_OK);
    CHECK_STR(km.format, "unknown");
    CHECK_STR(km.driver, "gpiozero-matrix");
    CHECK_INT(km.i2c_addr, 0x20);
    CHECK_INT(km.i2c_bus, 3);
}

static void test_a_saved_keymap_reads_back_identically(void)
{
    nd_keymap out;
    nd_keymap back;
    size_t r;
    size_t c;
    bool same = true;

    /* The wizard writes this file and the core reads it; a round trip that
     * loses a key is a phone with a dead button and no clue why. */
    fill_keymap(&out);
    (void)nd_strlcpy(out.path, "/User/keymap.json", sizeof out.path);
    CHECK_INT(nd_keymap_save(&out, "/User/keymap.json"), ND_OK);
    CHECK_INT(nd_keymap_load("/User/keymap.json", &back), ND_OK);

    CHECK_STR(back.driver, out.driver);
    CHECK_STR(back.format, out.format);
    CHECK_INT(back.n_rows, out.n_rows);
    CHECK_INT(back.n_cols, out.n_cols);
    CHECK_INT(back.i2c_addr, out.i2c_addr);
    CHECK_INT(back.i2c_bus, out.i2c_bus);
    for (r = 0u; r < ND_KEYMAP_MAX_ROWS; r++) {
        for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++) {
            if (back.matrix_to_code[r][c] != out.matrix_to_code[r][c])
                same = false;
        }
    }
    CHECK(same);
}

/* ------------------------------------------------------------------ *
 * The keycode tables
 * ------------------------------------------------------------------ */

static void test_the_matrix_name_table_is_verbatim(void)
{
    /* MATRIX_NAME_TO_CODE, core/main.py:47. Two names alias two codes on
     * purpose; getting one of these wrong makes exactly one button on the
     * phone do the wrong thing, which is very hard to spot in a diff. */
    CHECK_INT(nd_keycode_for_name("navikey"), 28);
    CHECK_INT(nd_keycode_for_name("enter"), 28);
    CHECK_INT(nd_keycode_for_name("clear"), 14);
    CHECK_INT(nd_keycode_for_name("back"), 14);
    CHECK_INT(nd_keycode_for_name("up"), 103);
    CHECK_INT(nd_keycode_for_name("down"), 108);
    CHECK_INT(nd_keycode_for_name("left"), 105);
    CHECK_INT(nd_keycode_for_name("right"), 106);
    CHECK_INT(nd_keycode_for_name("menu"), 50);
    CHECK_INT(nd_keycode_for_name("num_1"), 2);
    CHECK_INT(nd_keycode_for_name("num_9"), 10);
    CHECK_INT(nd_keycode_for_name("num_0"), 11);
    CHECK_INT(nd_keycode_for_name("star"), 42);
    CHECK_INT(nd_keycode_for_name("hash"), 43);
    CHECK_INT(nd_keycode_for_name("nonesuch"), -1);
    CHECK_INT(nd_keycode_for_name(NULL), -1);
}

static void test_the_three_character_tables(void)
{
    /* 0 is code 11, AFTER 9 -- evdev's number row, not arithmetic. */
    CHECK_INT(nd_key_digit_char(2), '1');
    CHECK_INT(nd_key_digit_char(10), '9');
    CHECK_INT(nd_key_digit_char(11), '0');
    CHECK_INT(nd_key_digit_char(12), '\0');

    /* NeoDCT_UI.DEV_KEYMAP -- what a key DIALS. The 28 -> '#' entry is dead
     * code in the Python and is reproduced anyway. */
    CHECK_INT(nd_key_dial_char(11), '0');
    CHECK_INT(nd_key_dial_char(12), '-');
    CHECK_INT(nd_key_dial_char(42), '*');
    CHECK_INT(nd_key_dial_char(43), '#');
    CHECK_INT(nd_key_dial_char(28), '#');
    CHECK_INT(nd_key_dial_char(57), '\0');

    /* TextInput.DEV_KEYMAP -- what a key TYPES on a QWERTY host. No '*' and
     * no '#': those codes are shift and backslash there. */
    CHECK_INT(nd_key_dev_char(30), 'a');
    CHECK_INT(nd_key_dev_char(50), 'm');
    CHECK_INT(nd_key_dev_char(57), ' ');
    CHECK_INT(nd_key_dev_char(12), '-');
    CHECK_INT(nd_key_dev_char(11), '0');
    CHECK_INT(nd_key_dev_char(42), '\0');
    CHECK_INT(nd_key_dev_char(43), '\0');
}

int main(void)
{
    RUN(test_write16_is_low_byte_first);
    RUN(test_read16_reassembles_low_byte_first);
    RUN(test_a_short_read_is_an_error_not_a_guess);
    RUN(test_close_releases_every_pin);
    RUN(test_pins_out_of_range_and_repeated_are_refused);
    RUN(test_a_pass_drives_every_row_and_releases_after);
    RUN(test_one_press_is_reported_exactly_once);
    RUN(test_release_needs_three_empty_scans);
    RUN(test_simultaneous_presses_queue_one_per_call);
    RUN(test_a_bus_failure_is_reported_not_swallowed);
    RUN(test_a_mapped_position_becomes_a_keycode);
    RUN(test_an_unmapped_position_produces_no_key);
    RUN(test_read_key_scans_at_least_once_at_timeout_zero);
    RUN(test_a_good_keymap_loads);
    RUN(test_a_missing_keymap_is_not_an_error);
    RUN(test_broken_json_is_refused);
    RUN(test_missing_matrix_fields_are_refused);
    RUN(test_an_unparseable_pin_is_refused);
    RUN(test_unknown_and_malformed_entries_are_skipped_not_fatal);
    RUN(test_no_recognised_keys_is_refused);
    RUN(test_the_i2c_address_accepts_three_spellings);
    RUN(test_absent_fields_take_the_python_defaults);
    RUN(test_a_saved_keymap_reads_back_identically);
    RUN(test_the_matrix_name_table_is_verbatim);
    RUN(test_the_three_character_tables);
    return pt_report("test_keypad");
}
