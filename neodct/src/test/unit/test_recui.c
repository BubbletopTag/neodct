/* test_recui.c -- the pure half of the on-screen recovery UI.
 *
 * SKILL.md: "Pure functions -- a key map, a layout, a formatter -- are worth
 * far more than UI tests here." That is even truer for this program than for
 * an app: nd-recui blocks on a keypad that does not exist on a build host,
 * and its whole reason to exist is a screen. So what is checked here is
 * everything that decides what appears on that screen, plus the two things
 * that would silently break it -- the keymap reader and the matrix scan.
 *
 * THE HIGHEST-VALUE CASE IN THIS FILE is t_only_the_sixteen_real_keys. Every
 * other test in the suite runs on a QWERTY development keyboard that has Left
 * and Right; the phone does not, and nd_keycode_for_name() will happily hand
 * back 105 or 106 for a keymap that names them. A future edit routing a menu
 * direction onto one of those would pass every other test in the tree and be
 * unusable on the device.
 *
 * ============ IT ALSO WRITES PICTURES ============
 *
 * Set NEODCT_RECUI_FRAMES to a directory and the last two cases render the
 * vertical list and the progress screen into it as PNGs, through the same
 * nd_recdraw code the phone runs -- a memory framebuffer instead of a mapped
 * /dev/fb0, which is the only difference. Unit tests do not tell you whether
 * a layout looks right, and this is a UI project.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_image.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"

#include "platform_test.h"

/* nd_recui.h is deliberately outside the include path the rest of the tree
 * builds against -- nd-recui may not include an nd_*.h, and the Makefile
 * enforces that by leaving -Iinclude off its rule. This test is on the other
 * side of the line and needs both, so it reaches across by path. */
#include "../../recovery/nd_recui.h"

/* ------------------------------------------------------------------ *
 * The sixteen keys
 * ------------------------------------------------------------------ */

static void t_only_the_sixteen_real_keys(void)
{
    /* The enrolment order of nd_kpsetup_targets[], which is by definition
     * every key the hardware has, checked against nd_keycodes.h. */
    static const struct {
        const char *name;
        int32_t code;
    } want[] = {
        {"navikey", ND_KEY_NAVIKEY}, {"clear", ND_KEY_CLEAR}, {"up", ND_KEY_UP},
        {"down", ND_KEY_DOWN},       {"num_1", ND_KEY_1},     {"num_2", ND_KEY_2},
        {"num_3", ND_KEY_3},         {"num_4", ND_KEY_4},     {"num_5", ND_KEY_5},
        {"num_6", ND_KEY_6},         {"num_7", ND_KEY_7},     {"num_8", ND_KEY_8},
        {"num_9", ND_KEY_9},         {"num_0", ND_KEY_0},     {"star", ND_KEY_STAR},
        {"hash", ND_KEY_HASH},
    };
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(want); i++)
        CHECK_INT(nd_reckey_for_name(want[i].name, strlen(want[i].name)), want[i].code);

    /* nd_keycode_for_name() accepts all three of these. This table must not:
     * there is no left and no right on the phone, and "menu" is the QWERTY
     * dev keyboard's M. A keymap naming one has to yield nothing rather than
     * a direction the hardware cannot produce. */
    CHECK_INT(nd_reckey_for_name("left", 4u), -1);
    CHECK_INT(nd_reckey_for_name("right", 5u), -1);
    CHECK_INT(nd_reckey_for_name("menu", 4u), -1);
    CHECK_INT(nd_reckey_for_name("", 0u), -1);
    /* len is honoured, not strlen: the parser hands in a slice of the JSON. */
    CHECK_INT(nd_reckey_for_name("upside", 2u), ND_KEY_UP);
    CHECK_INT(nd_reckey_for_name("up", 1u), -1);
}

/* ------------------------------------------------------------------ *
 * keymap.json
 * ------------------------------------------------------------------ */

/* A genuine payload, written by the code that writes the phone's own. Building
 * the fixture by hand would leave the reader agreeing with a guess about the
 * format rather than with the format. */
static void build_keymap_file(const char *virtual_path, nd_keymap *km)
{
    static const uint8_t rows[4] = {0u, 1u, 2u, 3u};
    static const uint8_t cols[4] = {4u, 5u, 6u, 7u};
    static const char *const names[16] = {"navikey", "clear", "up",    "down",  "num_1", "num_2",
                                          "num_3",   "num_4", "num_5", "num_6", "num_7", "num_8",
                                          "num_9",   "num_0", "star",  "hash"};
    size_t i;

    memset(km, 0, sizeof *km);
    for (i = 0u; i < ND_KEYMAP_MAX_ROWS; i++) {
        size_t c;

        for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++)
            km->matrix_to_code[i][c] = -1;
    }
    memcpy(km->row_pins, rows, sizeof rows);
    memcpy(km->col_pins, cols, sizeof cols);
    km->n_rows = 4u;
    km->n_cols = 4u;
    km->i2c_bus = 3;
    km->i2c_addr = 0x20;
    (void)nd_strlcpy(km->driver, "pcf8575-i2c", sizeof km->driver);
    (void)nd_strlcpy(km->format, "neodct-keymap-1", sizeof km->format);
    for (i = 0u; i < 16u; i++)
        km->matrix_to_code[i / 4u][i % 4u] = nd_keycode_for_name(names[i]);

    CHECK_INT(nd_keymap_save(km, virtual_path), ND_OK);
}

static char *slurp(const char *virtual_path)
{
    char resolved[ND_PATH_MAX];
    static char buf[16384];
    ssize_t got;
    int fd;

    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, virtual_path), ND_OK);
    fd = open(resolved, O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0)
        return NULL;
    got = read(fd, buf, sizeof buf - 1u);
    (void)close(fd);
    CHECK(got > 0);
    if (got <= 0)
        return NULL;
    buf[got] = '\0';
    return buf;
}

static void t_the_keymap_reader_agrees_with_the_writer(void)
{
    nd_keymap km;
    nd_reckeymap out;
    const char *text;
    size_t r;
    size_t c;

    build_keymap_file("/keymap.json", &km);
    text = slurp("/keymap.json");
    if (text == NULL)
        return;

    CHECK_INT(nd_reckeymap_parse(text, &out), 0);
    CHECK_INT(out.n_rows, 4);
    CHECK_INT(out.n_cols, 4);
    CHECK_INT(out.row_pins[0], 0);
    CHECK_INT(out.row_pins[3], 3);
    CHECK_INT(out.col_pins[0], 4);
    CHECK_INT(out.col_pins[3], 7);
    CHECK_INT(out.i2c_bus, 3);
    CHECK_INT(out.i2c_addr, 0x20);
    CHECK(out.any_key);

    /* Every position the writer recorded, read back identically. */
    for (r = 0u; r < ND_RECMATRIX_MAX_PINS; r++) {
        for (c = 0u; c < ND_RECMATRIX_MAX_PINS; c++)
            CHECK_INT(out.code[r][c], km.matrix_to_code[r][c]);
    }
}

static void t_the_reader_uses_the_file_the_loader_uses(void)
{
    nd_keymap km;
    nd_reckeymap out;
    char resolved[ND_PATH_MAX];

    build_keymap_file("/keymap.json", &km);
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/keymap.json"), ND_OK);

    CHECK_INT(nd_reckeymap_load(resolved, &out), 0);
    CHECK_INT(out.code[0][0], ND_KEY_NAVIKEY);
    CHECK_INT(out.code[3][3], ND_KEY_HASH);

    /* Absent is not a crash and not a default matrix: guessing which switch
     * is Up would move the selection at random on a phone being rescued. */
    CHECK_INT(nd_reckeymap_load("/does/not/exist.json", &out), -1);
}

static void t_a_mangled_keymap_loses_only_what_is_broken(void)
{
    nd_reckeymap out;

    /* An unknown name, a name the phone does not have, a position off the
     * end of the expander and a non-positional key -- all skipped, and the
     * keys around them survive. nd_keymap.c is forgiving for exactly this
     * reason: a keymap missing the '7' key still boots a phone you can fix. */
    static const char mangled[] = "{\n"
                                  "  \"by_matrix\": {\n"
                                  "    \"0,0\": \"navikey\",\n"
                                  "    \"0,1\": \"wobble\",\n"
                                  "    \"0,2\": \"left\",\n"
                                  "    \"99,0\": \"down\",\n"
                                  "    \"notapair\": \"up\",\n"
                                  "    \"1,1\": \"down\"\n"
                                  "  },\n"
                                  "  \"col_pins\": [4, 5],\n"
                                  "  \"i2c_addr\": \"0x21\",\n"
                                  "  \"i2c_bus\": 7,\n"
                                  "  \"row_pins\": [0, 1]\n"
                                  "}\n";

    CHECK_INT(nd_reckeymap_parse(mangled, &out), 0);
    CHECK_INT(out.code[0][0], ND_KEY_NAVIKEY);
    CHECK_INT(out.code[1][1], ND_KEY_DOWN);
    CHECK_INT(out.code[0][1], -1); /* unknown name  */
    CHECK_INT(out.code[0][2], -1); /* not on this phone */
    /* A hex string for i2c_addr, which two other tools have written by hand. */
    CHECK_INT(out.i2c_addr, 0x21);
    CHECK_INT(out.i2c_bus, 7);
}

static void t_a_structurally_broken_keymap_is_refused_whole(void)
{
    nd_reckeymap out;

    /* No by_matrix at all. */
    CHECK_INT(nd_reckeymap_parse("{\"row_pins\": [0], \"col_pins\": [1]}", &out), -1);
    /* by_matrix present but nothing in it that this phone has. */
    CHECK_INT(nd_reckeymap_parse("{\"row_pins\":[0],\"col_pins\":[1],"
                                 "\"by_matrix\":{\"0,0\":\"left\"}}",
                                 &out),
              -1);
    /* A pin outside the expander's sixteen: half a pin list would produce a
     * scan that presses nothing, so the whole file goes. */
    CHECK_INT(nd_reckeymap_parse("{\"row_pins\":[0,99],\"col_pins\":[1],"
                                 "\"by_matrix\":{\"0,0\":\"up\"}}",
                                 &out),
              -1);
    CHECK_INT(nd_reckeymap_parse("", &out), -1);

    /* A scalar where a list belongs. Scanning forward to the next bracket
     * would find col_pins's array and report ITS pins as the rows -- a keymap
     * that is merely wrong rather than obviously broken, which is the worse
     * of the two failures on a phone somebody is trying to rescue. */
    CHECK_INT(nd_reckeymap_parse("{\"row_pins\": 5, \"col_pins\":[1,2],"
                                 "\"by_matrix\":{\"0,0\":\"up\"}}",
                                 &out),
              -1);
    /* And the same for by_matrix. */
    CHECK_INT(nd_reckeymap_parse("{\"row_pins\":[0],\"col_pins\":[1],"
                                 "\"by_matrix\": 7, \"keys\":{\"0,0\":\"up\"}}",
                                 &out),
              -1);
}

static void t_row_pin_singular_cannot_be_mistaken_for_row_pins(void)
{
    nd_reckeymap out;

    /* nd_keymap_save() writes "row_pin" inside each entry of "keys", and it
     * sorts before "row_pins" in the file. A search that matched a prefix
     * would read the scalar 11 as the pin list and lose the matrix. */
    static const char text[] = "{\n"
                               "  \"by_matrix\": {\"0,0\": \"up\"},\n"
                               "  \"col_pins\": [5],\n"
                               "  \"keys\": {\"up\": {\"col\": 0, \"col_pin\": 5,\n"
                               "                     \"row\": 0, \"row_pin\": 11}},\n"
                               "  \"row_pins\": [2]\n"
                               "}\n";

    CHECK_INT(nd_reckeymap_parse(text, &out), 0);
    CHECK_INT(out.n_rows, 1);
    CHECK_INT(out.row_pins[0], 2);
    CHECK_INT(out.n_cols, 1);
    CHECK_INT(out.col_pins[0], 5);
}

/* ------------------------------------------------------------------ *
 * The matrix scan, over a socketpair standing in for the chip
 * ------------------------------------------------------------------ */

static void seed_reads(int fd, const uint16_t *words, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        uint8_t out[2];

        out[0] = (uint8_t)(words[i] & 0xFFu);
        out[1] = (uint8_t)((words[i] >> 8) & 0xFFu);
        CHECK_INT(write(fd, out, sizeof out), 2);
    }
}

static void drain(int fd)
{
    uint8_t junk[512];
    ssize_t n;

    do {
        n = recv(fd, junk, sizeof junk, MSG_DONTWAIT);
    } while (n > 0);
}

static void simple_map(nd_reckeymap *map)
{
    size_t r;
    size_t c;

    memset(map, 0, sizeof *map);
    for (r = 0u; r < ND_RECMATRIX_MAX_PINS; r++) {
        for (c = 0u; c < ND_RECMATRIX_MAX_PINS; c++)
            map->code[r][c] = -1;
    }
    map->row_pins[0] = 0u;
    map->row_pins[1] = 1u;
    map->n_rows = 2u;
    map->col_pins[0] = 4u;
    map->col_pins[1] = 5u;
    map->n_cols = 2u;
    map->code[0][0] = ND_KEY_UP;
    map->code[0][1] = ND_KEY_DOWN;
    map->code[1][0] = ND_KEY_NAVIKEY;
    map->code[1][1] = ND_KEY_CLEAR;
    map->i2c_bus = 3;
    map->i2c_addr = 0x20;
    map->any_key = true;
}

static void t_the_scan_drives_rows_low_and_reads_columns(void)
{
    nd_reckeymap map;
    nd_recmatrix mx;
    uint8_t seen[8];
    int sv[2];
    /* Nothing pressed: every pin reads high on its pull-up. */
    uint16_t idle[2] = {0xFFFFu, 0xFFFFu};

    simple_map(&map);
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seed_reads(sv[1], idle, 2u);

    CHECK_INT(nd_recmatrix_attach(&mx, &map, sv[0]), 0);
    CHECK_INT(nd_recmatrix_scan(&mx), ND_RECKEY_NONE);

    /* attach writes the 0xFFFF release word, then the scan writes one
     * row-drive word per row and a release at the end. Low byte first. */
    CHECK_INT(read(sv[1], seen, 2u), 2);
    CHECK_INT(seen[0], 0xFF);
    CHECK_INT(seen[1], 0xFF);
    CHECK_INT(read(sv[1], seen, 2u), 2);
    CHECK_INT(seen[0], 0xFE); /* ~(1 << 0) */
    CHECK_INT(seen[1], 0xFF);
    CHECK_INT(read(sv[1], seen, 2u), 2);
    CHECK_INT(seen[0], 0xFD); /* ~(1 << 1) */
    CHECK_INT(seen[1], 0xFF);
    CHECK_INT(read(sv[1], seen, 2u), 2);
    CHECK_INT(seen[0], 0xFF); /* released on the way out */

    nd_recmatrix_close(&mx);
    (void)close(sv[1]);
}

static void t_a_closed_switch_becomes_its_key(void)
{
    nd_reckeymap map;
    nd_recmatrix mx;
    int sv[2];
    /* Row 0 driven low, column pin 5 reads low -> (0,1), which the map says
     * is Down. Row 1 sees nothing. */
    uint16_t pressed[2] = {(uint16_t) ~(1u << 5), 0xFFFFu};

    simple_map(&map);
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_INT(nd_recmatrix_attach(&mx, &map, sv[0]), 0);
    drain(sv[1]);
    seed_reads(sv[1], pressed, 2u);

    CHECK_INT(nd_recmatrix_scan(&mx), ND_KEY_DOWN);

    nd_recmatrix_close(&mx);
    (void)close(sv[1]);
}

static void t_a_held_key_is_reported_once_and_released_after_three_scans(void)
{
    nd_reckeymap map;
    nd_recmatrix mx;
    int sv[2];
    uint16_t held[2] = {(uint16_t) ~(1u << 4), 0xFFFFu}; /* (0,0) = Up */
    uint16_t idle[2] = {0xFFFFu, 0xFFFFu};
    int i;

    simple_map(&map);
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_INT(nd_recmatrix_attach(&mx, &map, sv[0]), 0);
    drain(sv[1]);

    seed_reads(sv[1], held, 2u);
    CHECK_INT(nd_recmatrix_scan(&mx), ND_KEY_UP);
    drain(sv[1]);

    /* Still down: a press edge, not a level. Repeat is not synthesised here
     * and must not be -- a five-item menu does not need it. */
    seed_reads(sv[1], held, 2u);
    CHECK_INT(nd_recmatrix_scan(&mx), ND_RECKEY_NONE);
    drain(sv[1]);

    /* A membrane chatters on the way up, so the release takes three clean
     * scans; only after that does the same key press again. */
    for (i = 0; i < ND_RECMATRIX_RELEASE_SCANS; i++) {
        seed_reads(sv[1], idle, 2u);
        CHECK_INT(nd_recmatrix_scan(&mx), ND_RECKEY_NONE);
        drain(sv[1]);
    }
    seed_reads(sv[1], held, 2u);
    CHECK_INT(nd_recmatrix_scan(&mx), ND_KEY_UP);

    nd_recmatrix_close(&mx);
    (void)close(sv[1]);
}

static void t_an_unmapped_position_is_not_a_key(void)
{
    nd_reckeymap map;
    nd_recmatrix mx;
    int sv[2];
    /* Row 0, column pin 6 -- a third column this map does not scan, plus
     * row 1 column pin 4, which IS mapped. The unmapped one must not shadow
     * the real one. */
    uint16_t words[2] = {(uint16_t) ~(1u << 6), (uint16_t) ~(1u << 4)};

    simple_map(&map);
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_INT(nd_recmatrix_attach(&mx, &map, sv[0]), 0);
    drain(sv[1]);
    seed_reads(sv[1], words, 2u);

    CHECK_INT(nd_recmatrix_scan(&mx), ND_KEY_NAVIKEY);

    nd_recmatrix_close(&mx);
    (void)close(sv[1]);
}

/* ------------------------------------------------------------------ *
 * evdev decoding
 * ------------------------------------------------------------------ */

static void t_both_input_event_layouts_decode(void)
{
    uint8_t buf[24];
    uint16_t type = 1u; /* EV_KEY */
    uint16_t code = ND_KEY_DOWN;
    int32_t value = 1;

    /* 64-bit: two 8-byte time words, then type, code, value. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 16, &type, 2u);
    memcpy(buf + 18, &code, 2u);
    memcpy(buf + 20, &value, 4u);
    CHECK_INT(nd_recevdev_decode(buf, 24u), ND_KEY_DOWN);

    /* 32-bit ARM: two 4-byte time words. The same binary reads both, because
     * the layout is the kernel's choice and not the reader's. */
    memset(buf, 0, sizeof buf);
    memcpy(buf + 8, &type, 2u);
    memcpy(buf + 10, &code, 2u);
    memcpy(buf + 12, &value, 4u);
    CHECK_INT(nd_recevdev_decode(buf, 16u), ND_KEY_DOWN);

    /* Value 2 is the kernel's autorepeat and IS accepted here -- nd_input
     * synthesises its own and therefore drops it, but there is no synthesiser
     * in recovery and a held arrow has to keep scrolling under QEMU. */
    value = 2;
    memcpy(buf + 12, &value, 4u);
    CHECK_INT(nd_recevdev_decode(buf, 16u), ND_KEY_DOWN);

    /* A release is not a press. */
    value = 0;
    memcpy(buf + 12, &value, 4u);
    CHECK_INT(nd_recevdev_decode(buf, 16u), ND_RECKEY_NONE);

    /* EV_SYN, EV_MSC and friends are not keys. */
    value = 1;
    type = 0u;
    memcpy(buf + 8, &type, 2u);
    memcpy(buf + 12, &value, 4u);
    CHECK_INT(nd_recevdev_decode(buf, 16u), ND_RECKEY_NONE);

    /* A short or torn read is nothing, not a guess. */
    CHECK_INT(nd_recevdev_decode(buf, 9u), ND_RECKEY_NONE);
    CHECK_INT(nd_recevdev_decode(NULL, 16u), ND_RECKEY_NONE);
}

/* ------------------------------------------------------------------ *
 * List layout -- the numbers nd_vlist_draw() produces
 * ------------------------------------------------------------------ */

static void t_the_list_lays_out_exactly_as_nd_vlist_does(void)
{
    nd_reclist_metrics m;

    nd_reclist_metrics_of(ND_RECUI_W, ND_RECUI_CONTENT_BOTTOM, &m);

    /* nd_vlist.c's "FOUR NUMBERS THAT DECIDE THE PIXELS", verbatim. */
    CHECK_INT(m.header_y, 30);
    CHECK_INT(m.y_start, 40);
    CHECK_INT(m.line_height, 33);
    CHECK_INT(m.item_height, 29);
    CHECK_INT(m.max_lines, 3);
    CHECK_INT(m.bar_x, 235);
    CHECK_INT(m.selected_right, 225);
    CHECK_INT(m.track_top, 40);
    CHECK_INT(m.track_bottom, 140);

    /* Rows at 40, 73 and 106. */
    CHECK_INT(m.y_start + 0 * m.line_height, 40);
    CHECK_INT(m.y_start + 1 * m.line_height, 73);
    CHECK_INT(m.y_start + 2 * m.line_height, 106);
}

static void t_the_window_follows_the_selection_and_stops_at_the_end(void)
{
    /* Five items, three rows: the window slides only when the selection
     * leaves it, and clamps so the last frame is not two blanks and a row. */
    CHECK_INT(nd_reclist_window(0u, 0u, 5u, 3u), 0);
    CHECK_INT(nd_reclist_window(2u, 0u, 5u, 3u), 0);
    CHECK_INT(nd_reclist_window(3u, 0u, 5u, 3u), 1);
    CHECK_INT(nd_reclist_window(4u, 1u, 5u, 3u), 2);
    CHECK_INT(nd_reclist_window(4u, 4u, 5u, 3u), 2); /* clamped */
    CHECK_INT(nd_reclist_window(0u, 2u, 5u, 3u), 0);
    /* A caller that preselected a row past the first windowful still gets a
     * window containing it -- without this the frame comes back with no
     * selection bar at all, which reads as a menu that lost its place. */
    CHECK_INT(nd_reclist_window(4u, 0u, 5u, 3u), 2);
    /* Fewer items than rows: there is nowhere to scroll to. */
    CHECK_INT(nd_reclist_window(1u, 0u, 2u, 3u), 0);
}

static void t_the_notch_truncates_rather_than_rounds(void)
{
    nd_reclist_metrics m;

    nd_reclist_metrics_of(ND_RECUI_W, ND_RECUI_CONTENT_BOTTOM, &m);

    /* Five items over a 100 px track: the step is 25 and every position is
     * exact. */
    CHECK_INT(nd_reclist_notch_y(&m, 0u, 5u), 40);
    CHECK_INT(nd_reclist_notch_y(&m, 4u, 5u), 140);

    /* Three items: the step is 50. */
    CHECK_INT(nd_reclist_notch_y(&m, 1u, 3u), 90);

    /* Seven items: the step is 100/6 = 16.666..., and row 1 lands on 56.67.
     * Truncation gives 56; rounding would give 57 and move the notch a pixel
     * away from where every list in the OS puts it. */
    CHECK_INT(nd_reclist_notch_y(&m, 1u, 7u), 56);

    /* A one-item list divides by zero if the guard is missing. */
    CHECK_INT(nd_reclist_notch_y(&m, 0u, 1u), 40);
}

static void t_the_list_keys_are_the_ones_the_framework_has(void)
{
    size_t sel = 0u;

    CHECK_INT(nd_reclist_key(ND_KEY_DOWN, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 1);
    CHECK_INT(nd_reclist_key(ND_KEY_DOWN, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 2);
    /* The bottom of the list is a steady screen, not a wrap and not a dead
     * one: nd_vlist_handle_key() stops, and redraws anyway. */
    CHECK_INT(nd_reclist_key(ND_KEY_DOWN, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 2);
    CHECK_INT(nd_reclist_key(ND_KEY_ENTER, 3u, &sel), 2);

    sel = 0u;
    CHECK_INT(nd_reclist_key(ND_KEY_UP, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 0);

    /* Digits 1..9 pick outright; 0 (code 11) is NOT a shortcut, matching
     * nd_vlist_handle_key(), and a digit past the end is ignored without
     * moving anything. */
    CHECK_INT(nd_reclist_key(ND_KEY_2, 3u, &sel), 1);
    CHECK_INT(nd_reclist_key(ND_KEY_9, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(nd_reclist_key(ND_KEY_0, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 0);

    CHECK_INT(nd_reclist_key(ND_KEY_CLEAR, 3u, &sel), ND_RECLIST_BACK);
    CHECK_INT(nd_reclist_key(ND_KEY_STAR, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(nd_reclist_key(ND_KEY_HASH, 3u, &sel), ND_RECLIST_CONTINUE);

    /* Left and Right reach only a development keyboard. They must do nothing
     * here rather than quietly doing what Up and Down do, or a screen that
     * depends on them passes every test and is unusable on the phone. */
    CHECK_INT(nd_reclist_key(ND_KEY_LEFT, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(nd_reclist_key(ND_KEY_RIGHT, 3u, &sel), ND_RECLIST_CONTINUE);
    CHECK_INT(sel, 0);
}

/* ------------------------------------------------------------------ *
 * Progress
 * ------------------------------------------------------------------ */

static void t_the_progress_boxes_are_the_frameworks(void)
{
    nd_recprogress_metrics m;

    nd_recprogress_metrics_of(ND_RECUI_W, ND_RECUI_CONTENT_BOTTOM, &m);

    /* nd_progress.c: bar_top is int(content_bottom * 0.55) = 79, and the bar
     * is (20, 79, 220, 93). */
    CHECK_INT(m.bar_x0, 20);
    CHECK_INT(m.bar_y0, 79);
    CHECK_INT(m.bar_x1, 220);
    CHECK_INT(m.bar_y1, 93);
    /* The reading sits BELOW the bar, never on it: a percentage across its
     * own fill is the one thing that makes a bar look broken. */
    CHECK_INT(m.status_y, 102);
    CHECK(m.label_y + 14 <= m.bar_y0);
}

static void t_the_percentage_truncates_and_clamps(void)
{
    CHECK_INT(nd_recprogress_percent(0, 100), 0);
    CHECK_INT(nd_recprogress_percent(1, 3), 33);
    CHECK_INT(nd_recprogress_percent(2, 3), 66);
    CHECK_INT(nd_recprogress_percent(100, 100), 100);
    /* total == 0 means "done", not a divide by zero. */
    CHECK_INT(nd_recprogress_percent(0, 0), 100);
    /* A --total smaller than the stream (a manifest that disagrees with the
     * zip) pins the bar at full rather than overflowing it. */
    CHECK_INT(nd_recprogress_percent(200, 100), 100);
    CHECK_INT(nd_recprogress_percent(-5, 100), 0);

    /* 48 MB in 64 KB chunks, which is the real shape of an install. */
    CHECK_INT(nd_recprogress_percent(24u * 1024u * 1024u, 48u * 1024u * 1024u), 50);
}

static void t_the_percent_gate_stops_the_bar_outrunning_the_write(void)
{
    nd_recfb fb;
    nd_recprogress p;
    int64_t total = 48LL * 1024 * 1024;
    int64_t done;
    int drawn = 0;

    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    nd_recprogress_init(&p, &fb, "Checking image", NULL);

    /* Fed 64 KB at a time -- 768 calls over a 48 MB image. Only the hundred
     * that change the whole percentage may paint; that gate is the difference
     * between a bar and a bar that is slower than the write it reports on. */
    for (done = 0; done <= total; done += 65536) {
        if (nd_recprogress_draw(&p, done, total))
            drawn++;
    }
    CHECK_INT(drawn, 101); /* 0% through 100% inclusive */

    /* And a repeat of the same percentage paints nothing at all. */
    CHECK(!nd_recprogress_draw(&p, total, total));

    nd_recfb_close(&fb);
}

static void t_the_byte_reading_is_readable_at_a_glance(void)
{
    char buf[16];

    nd_recprogress_human(buf, sizeof buf, 0);
    CHECK_STR(buf, "0B");
    nd_recprogress_human(buf, sizeof buf, 512);
    CHECK_STR(buf, "512B");
    nd_recprogress_human(buf, sizeof buf, 1024);
    CHECK_STR(buf, "1.0K");
    nd_recprogress_human(buf, sizeof buf, 48LL * 1024 * 1024);
    CHECK_STR(buf, "48.0M");
}

/* ------------------------------------------------------------------ *
 * The font
 * ------------------------------------------------------------------ */

/* Every string fontref.py records, at the two sizes recovery ships, measured
 * against Pillow's own sum_of_advances. A C engine can match every glyph and
 * still drift if it accumulates advances differently; this is the check that
 * catches that, and it is the reason the generated table stores an advance
 * per glyph rather than a fixed cell width. */
static void t_the_bitmap_font_advances_match_pillow(void)
{
    const char *golden = getenv("NEODCT_GOLDEN");
    char path[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *sizes;
    char err[128];
    size_t which;
    static const struct {
        const char *key;
        nd_recfontsize font;
    } wanted[2] = {{"18", ND_RECFONT_LARGE}, {"14", ND_RECFONT_SMALL}};

    if (golden == NULL || golden[0] == '\0') {
        printf("test_recui: NEODCT_GOLDEN is not set; skipping the font check\n");
        return;
    }
    if (snprintf(path, sizeof path, "%s/font/fontref.json", golden) < 0)
        return;
    /* Plain path, NOT under ND_ROOT -- the reference set is not part of the
     * fixture. nd_json_parse_file resolves, and a relative-looking absolute
     * path would be prefixed, so the root is dropped for this read. */
    (void)nd_path_set_root("");
    err[0] = '\0';
    if (nd_json_parse_file(path, &doc, err, sizeof err) != ND_OK) {
        printf("test_recui: cannot read %s (%s); skipping the font check\n", path, err);
        return;
    }
    sizes = nd_json_get(nd_json_root(doc), "sizes");

    for (which = 0u; which < 2u; which++) {
        const nd_json_val *entry = nd_json_get(sizes, wanted[which].key);
        const nd_json_val *strings = nd_json_get(entry, "strings");
        const nd_json_val *glyphs = nd_json_get(entry, "glyphs");
        size_t i;
        size_t n;

        CHECK(strings != NULL);

        n = nd_json_len(strings);
        for (i = 0u; i < n; i++) {
            const nd_json_val *rec = nd_json_at(strings, i);
            const char *text = nd_json_get_str(rec, "text", NULL);
            double want = 0.0;
            int32_t got = 0;

            if (text == NULL || !nd_json_real(nd_json_get(rec, "sum_of_advances"), &want)) {
                int64_t as_int = 0;

                if (!nd_json_int(nd_json_get(rec, "sum_of_advances"), &as_int))
                    continue;
                want = (double)as_int;
            }
            nd_recdraw_text_size(wanted[which].font, text, &got, NULL);
            CHECK_INT(got, (int32_t)want);
        }

        /* And the ink height per glyph, which is what decides where a menu
         * row's text sits -- nd_vlist centres on THIS string's ink, not on
         * the font's line height. */
        n = nd_json_len(glyphs);
        for (i = 0u; i < n; i++) {
            const nd_json_val *rec = nd_json_at(glyphs, i);
            const char *ch = nd_json_get_str(rec, "char", NULL);
            int64_t ink_h = 0;
            char one[2];
            int32_t got = 0;
            int32_t want;

            if (ch == NULL || ch[0] == '\0' || (unsigned char)ch[0] > 0x7Eu)
                continue;
            if (!nd_json_int(nd_json_get(rec, "ink_h"), &ink_h))
                continue;
            one[0] = ch[0];
            one[1] = '\0';
            nd_recdraw_text_size(wanted[which].font, one, NULL, &got);

            /* nd_text_bbox() starts the box collapsed on the baseline, so a
             * glyph entirely above it measures taller than its own ink. Read
             * the reference the same way rather than comparing ink_h raw. */
            {
                int64_t ink_dy = 0;
                int32_t ascent = (wanted[which].font == ND_RECFONT_LARGE) ? 18 : 14;
                int32_t top;
                int32_t bottom;

                (void)nd_json_int(nd_json_get(rec, "ink_dy"), &ink_dy);
                top = ascent;
                bottom = ascent;
                if (ink_h > 0) {
                    if ((int32_t)ink_dy < top)
                        top = (int32_t)ink_dy;
                    if ((int32_t)ink_dy + (int32_t)ink_h > bottom)
                        bottom = (int32_t)ink_dy + (int32_t)ink_h;
                }
                want = bottom - top;
            }
            CHECK_INT(got, want);
        }
    }

    nd_json_free(doc);
}

static void t_text_that_does_not_fit_is_cut_at_the_box(void)
{
    char out[64];
    int32_t w;

    /* "wipe user data" is 161 px at 18 px and the widest menu item there is;
     * it clears the 225 px selection bar with room to spare. */
    nd_recdraw_text_size(ND_RECFONT_LARGE, "wipe user data", &w, NULL);
    CHECK_INT(w, 161);
    CHECK(w < 225 - 10);

    /* "NeoDCT recovery" fits at 18 px and does not at 24, which is why the
     * framework's own title size is unavailable here. */
    nd_recdraw_text_size(ND_RECFONT_LARGE, "NeoDCT recovery", &w, NULL);
    CHECK_INT(w, 188);
    CHECK(w < ND_RECUI_W);

    /* A real package name overflows 18 px and fits at 14. */
    nd_recdraw_text_size(ND_RECFONT_LARGE, "UPDATE-0.5.0b.ndsw", &w, NULL);
    CHECK_INT(w, 221);
    nd_recdraw_text_size(ND_RECFONT_SMALL, "UPDATE-0.5.0b.ndsw", &w, NULL);
    CHECK_INT(w, 181);

    /* Hard truncation, never past the box, and always NUL-terminated. */
    w = nd_recdraw_text_fit(out, sizeof out, "wipe user data", ND_RECFONT_LARGE, 60);
    CHECK(w <= 60);
    CHECK(strlen(out) > 0u);
    CHECK(strncmp(out, "wipe user data", strlen(out)) == 0);

    /* No room at all is an empty string, not a crash. */
    w = nd_recdraw_text_fit(out, sizeof out, "anything", ND_RECFONT_SMALL, 0);
    CHECK_INT(w, 0);
    CHECK_STR(out, "");

    /* A tiny output buffer truncates without overrunning. */
    (void)nd_recdraw_text_fit(out, 4u, "abcdefgh", ND_RECFONT_SMALL, 1000);
    CHECK_INT(strlen(out), 3);
}

/* ------------------------------------------------------------------ *
 * Pixels
 * ------------------------------------------------------------------ */

static void t_the_three_colours_survive_both_depths(void)
{
    nd_recfb fb32;
    nd_recfb fb16;

    /* The one-bit rule, stated as a test: whatever a driver says about
     * channel order, these three values are the same bytes either way round.
     * That is why recovery draws only them. */
    CHECK_INT(nd_recfb_open_mem(&fb32, 8, 4, 32), 0);
    CHECK_INT(nd_recfb_open_mem(&fb16, 8, 4, 16), 0);

    nd_recdraw_rect(&fb32, 1, 1, 2, 2, ND_RECCOL_WHITE);
    nd_recdraw_rect(&fb16, 1, 1, 2, 2, ND_RECCOL_WHITE);
    nd_recdraw_rect(&fb32, 5, 1, 5, 1, ND_RECCOL_GREY);
    nd_recdraw_rect(&fb16, 5, 1, 5, 1, ND_RECCOL_GREY);

    CHECK_INT(nd_recfb_get(&fb32, 1, 1), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb16, 1, 1), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb32, 5, 1), ND_RECCOL_GREY);
    CHECK_INT(nd_recfb_get(&fb16, 5, 1), ND_RECCOL_GREY);
    CHECK_INT(nd_recfb_get(&fb32, 0, 0), ND_RECCOL_BLACK);
    CHECK_INT(nd_recfb_get(&fb16, 0, 0), ND_RECCOL_BLACK);

    /* Rects are inclusive of both corners and clip rather than refuse -- the
     * Python names x=240 on a 240-wide canvas in more than one place. */
    nd_recdraw_rect(&fb32, -4, -4, 400, 400, ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb32, 7, 3), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb32, 0, 0), ND_RECCOL_WHITE);

    nd_recfb_close(&fb32);
    nd_recfb_close(&fb16);
}

static void t_the_list_clears_only_the_content_rows(void)
{
    nd_recfb fb;
    const char *items[2] = {"update system", "reboot"};
    size_t window = 0u;

    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);

    /* Something in the softkey strip, which the list must not touch: that is
     * the guarantee nd_vlist_draw()'s 0..145 clear exists to give, and
     * nd-recui uses it to keep a legend on screen across every redraw. */
    nd_recdraw_rect(&fb, 0, 150, 239, 160, ND_RECCOL_WHITE);
    nd_reclist_draw(&fb, "NeoDCT recovery", items, 2u, 0u, &window);

    CHECK_INT(nd_recfb_get(&fb, 10, 155), ND_RECCOL_WHITE);
    /* And the selection bar IS drawn, black text on white, stopping short of
     * the scrollbar. */
    CHECK_INT(nd_recfb_get(&fb, 0, 41), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb, 225, 41), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb, 226, 41), ND_RECCOL_BLACK);
    /* The divider is one white row at y=30. */
    CHECK_INT(nd_recfb_get(&fb, 120, 30), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb, 120, 31), ND_RECCOL_BLACK);
    /* The scrollbar track is grey and one pixel wide. */
    CHECK_INT(nd_recfb_get(&fb, 235, 130), ND_RECCOL_GREY);
    CHECK_INT(nd_recfb_get(&fb, 234, 130), ND_RECCOL_BLACK);

    nd_recfb_close(&fb);
}

static void t_the_destructive_question_opens_with_neither_answer_lit(void)
{
    nd_recfb fb;
    nd_reclist_metrics m;
    int32_t yes_row;
    int32_t no_row;

    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    nd_reclist_metrics_of(fb.w, ND_RECUI_CONTENT_BOTTOM, &m);
    yes_row = ND_RECUI_CONTENT_BOTTOM - (2 * m.item_height) - 4;
    no_row = yes_row + m.item_height;

    nd_recconfirm_draw(&fb, "WIPE SYSTEM? The phone will not boot.", -1);
    /* Neither row has a white bar: a stray Enter on this screen must not be
     * able to answer it. The tty menu defaults to "no"; this one defaults to
     * nothing, which is stronger and is the whole point. */
    CHECK_INT(nd_recfb_get(&fb, 2, yes_row + 2), ND_RECCOL_BLACK);
    CHECK_INT(nd_recfb_get(&fb, 2, no_row + 2), ND_RECCOL_BLACK);

    nd_recconfirm_draw(&fb, "WIPE SYSTEM? The phone will not boot.", 1);
    CHECK_INT(nd_recfb_get(&fb, 2, yes_row + 2), ND_RECCOL_BLACK);
    CHECK_INT(nd_recfb_get(&fb, 2, no_row + 2), ND_RECCOL_WHITE);

    nd_recconfirm_draw(&fb, "WIPE SYSTEM? The phone will not boot.", 0);
    CHECK_INT(nd_recfb_get(&fb, 2, yes_row + 2), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb, 2, no_row + 2), ND_RECCOL_BLACK);

    /* The wrapped question never reaches the options: overlapping them would
     * put text through the row somebody is about to select. */
    CHECK_INT(nd_recfb_get(&fb, 120, yes_row - 2), ND_RECCOL_BLACK);

    nd_recfb_close(&fb);
}

static void t_a_long_message_stops_before_the_prompt(void)
{
    nd_recfb fb;
    static const char *const lines[8] = {"one",  "two", "three", "four",
                                         "five", "six", "seven", "eight"};
    int32_t line_h = 0;
    int32_t prompt_y;

    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);

    /* Something in the softkey strip, which the caller owns. Eight lines at
     * the 18 px pitch would run past the content area, and clipping at the
     * panel edge happens too late -- it would already have painted over the
     * legend. */
    nd_recdraw_rect(&fb, 0, 150, 239, 160, ND_RECCOL_WHITE);
    nd_recmessage_draw(&fb, lines, 8u);

    CHECK_INT(nd_recfb_get(&fb, 10, 155), ND_RECCOL_WHITE);
    nd_recdraw_text_size(ND_RECFONT_LARGE, "Ag", NULL, &line_h);
    prompt_y = ND_RECUI_CONTENT_BOTTOM - line_h - 4;
    /* The row just above the prompt is blank: the last line that did not fit
     * was dropped rather than drawn through it. */
    CHECK_INT(nd_recfb_get(&fb, 12, prompt_y - 2), ND_RECCOL_BLACK);

    nd_recfb_close(&fb);
}

static void t_a_raw_splash_blits_at_either_depth(void)
{
    nd_recfb fb;
    char resolved[ND_PATH_MAX];
    /* Two pixels of the format mkinitramfs.py writes: XRGB8888, bytes B G R X,
     * exactly two colours. */
    static const uint8_t blob[8] = {0xFFu, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    pt_write("/splash.raw", blob, sizeof blob);
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/splash.raw"), ND_OK);

    CHECK_INT(nd_recfb_open_mem(&fb, 2, 1, 16), 0);
    CHECK_INT(nd_recdraw_blit_raw(&fb, resolved), 0);
    CHECK_INT(nd_recfb_get(&fb, 0, 0), ND_RECCOL_WHITE);
    CHECK_INT(nd_recfb_get(&fb, 1, 0), ND_RECCOL_BLACK);
    nd_recfb_close(&fb);

    CHECK_INT(nd_recfb_open_mem(&fb, 2, 1, 32), 0);
    CHECK_INT(nd_recdraw_blit_raw(&fb, resolved), 0);
    CHECK_INT(nd_recfb_get(&fb, 0, 0), ND_RECCOL_WHITE);
    nd_recfb_close(&fb);

    CHECK_INT(nd_recfb_open_mem(&fb, 2, 1, 32), 0);
    CHECK_INT(nd_recdraw_blit_raw(&fb, "/no/such/splash.raw"), -1);
    nd_recfb_close(&fb);
}

/* ------------------------------------------------------------------ *
 * Pictures
 * ------------------------------------------------------------------ */

/* The recovery framebuffer, as an nd_image the PNG writer understands. Only
 * the three colours exist, so this is a lookup rather than an unpack -- and
 * doing it that way means the picture shows what nd_recfb_get() sees, which
 * is what the assertions above are written against. */
static void dump_png(const nd_recfb *fb, const char *dir, const char *name)
{
    nd_image *img;
    char path[ND_PATH_MAX];
    int32_t x;
    int32_t y;

    img = nd_image_new(fb->w, fb->h, ND_PIXFMT_RGB888);
    if (img == NULL) {
        CHECK(img != NULL);
        return;
    }
    for (y = 0; y < fb->h; y++) {
        for (x = 0; x < fb->w; x++) {
            nd_reccolour c = nd_recfb_get(fb, x, y);
            uint8_t v = (c == ND_RECCOL_WHITE) ? 0xFFu : (c == ND_RECCOL_GREY ? 0x80u : 0x00u);

            nd_image_set_px(img, x, y, ND_RGB(v, v, v));
        }
    }
    if (snprintf(path, sizeof path, "%s/%s", dir, name) > 0) {
        /* The frames directory is a real path the caller named, not part of
         * the fixture, so the root is dropped for the write. */
        (void)nd_path_set_root("");
        CHECK_INT(nd_image_save_png(img, path), ND_OK);
        printf("test_recui: wrote %s\n", path);
    }
    nd_image_free(img);
}

static void t_render_the_screens(void)
{
    const char *dir = getenv("NEODCT_RECUI_FRAMES");
    static const char *const items[5] = {"update system", "wipe user data", "wipe system", "reboot",
                                         "shell"};
    nd_recfb fb;
    nd_recprogress p;
    size_t window = 0u;
    char legend[64];

    if (dir == NULL || dir[0] == '\0')
        return;

    /* The first screen a person sees: five items, the second selected so the
     * white bar and the scrollbar notch are both visible, and the heading in
     * the strip the list never clears. */
    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    (void)nd_recdraw_text_fit(legend, sizeof legend, "1-5 or arrows, Enter", ND_RECFONT_SMALL,
                              fb.w - 10);
    nd_recdraw_text(&fb, 5, ND_RECUI_CONTENT_BOTTOM + 6, legend, ND_RECFONT_SMALL, ND_RECCOL_WHITE);
    nd_reclist_draw(&fb, "NeoDCT recovery", items, 5u, 1u, &window);
    nd_recdraw_text(&fb, 5, 0, "NeoDCT recovery", ND_RECFONT_LARGE, ND_RECCOL_WHITE);
    dump_png(&fb, dir, "recovery-menu.png");
    nd_recfb_close(&fb);

    /* And the long operation: writing a 48 MB image, which is where the time
     * actually goes. */
    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    nd_recprogress_init(&p, &fb, "Writing image", "UPDATE-0.5.0b.ndsw");
    CHECK(nd_recprogress_draw(&p, 31LL * 1024 * 1024, 48LL * 1024 * 1024));
    dump_png(&fb, dir, "recovery-progress.png");
    nd_recfb_close(&fb);

    /* The question that stands between a person and an erased partition,
     * opening with neither answer lit. */
    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    (void)nd_recdraw_text_fit(legend, sizeof legend, "Up/Down, then Enter", ND_RECFONT_SMALL,
                              fb.w - 10);
    nd_recdraw_text(&fb, 5, ND_RECUI_CONTENT_BOTTOM + 6, legend, ND_RECFONT_SMALL, ND_RECCOL_WHITE);
    nd_recconfirm_draw(&fb, "WIPE USER DATA? Contacts, messages and settings will be erased.", -1);
    dump_png(&fb, dir, "recovery-confirm.png");
    nd_recfb_close(&fb);

    /* The message page, which is what wipe-system gets instead of a bar: it
     * is dd bs=1M count=1 and a bar that fills in 80 ms is a lie. */
    CHECK_INT(nd_recfb_open_mem(&fb, ND_RECUI_W, ND_RECUI_H, 32), 0);
    {
        static const char *const lines[3] = {"System wiped.", "Install an update",
                                             "from an SD card."};

        nd_recmessage_draw(&fb, lines, 3u);
    }
    dump_png(&fb, dir, "recovery-message.png");
    nd_recfb_close(&fb);
}


/* The bug a real recovery boot found, and the two halves that make it one.
 *
 * On a phone booted with neodct.recovery=1, "update system" with no card in
 * the slot drew "No .ndsw on the car" -- a sentence that had lost the word it
 * was about. nd_recdraw_text_fit() truncates at the character that does not
 * fit, with no ellipsis, which is right for a menu label and wrong for prose.
 *
 * Both assertions matter and they fail for different reasons. The first says
 * the string really does overflow at LARGE, so the step-down is not
 * cargo-cult; if a future font makes it fit, this fails and somebody deletes
 * the step-down deliberately rather than leaving dead code. The second says
 * that after stepping down the sentence is WHOLE -- not shorter, not
 * ellipsized, whole -- which is the thing a person standing in front of a
 * bricked phone actually needs. */
static void t_a_message_too_wide_for_the_big_font_is_not_cut(void)
{
    const char *const said[] = {
        "No .ndsw on the card.", /* the one that was seen truncated */
        "Copy one into update/", "No SD card found.",
        "FAILED. See the serial", "User data wiped.",
    };
    const int32_t room = 240 - 20; /* nd_recmessage_draw's own margin */
    char out[ND_RECUI_ITEM_MAX];
    int32_t w = 0;
    size_t i;

    nd_recdraw_text_size(ND_RECFONT_LARGE, said[0], &w, NULL);
    CHECK(w > room);

    /* Every line recovery can say has to survive the small font whole,
     * because there is no third size to fall back to. */
    for (i = 0u; i < sizeof said / sizeof said[0]; i++) {
        (void)nd_recdraw_text_fit(out, sizeof out, said[i], ND_RECFONT_SMALL, room);
        CHECK_STR(out, said[i]);
    }
}

int main(void)
{
    RUN(t_only_the_sixteen_real_keys);
    RUN(t_the_keymap_reader_agrees_with_the_writer);
    RUN(t_the_reader_uses_the_file_the_loader_uses);
    RUN(t_a_mangled_keymap_loses_only_what_is_broken);
    RUN(t_a_structurally_broken_keymap_is_refused_whole);
    RUN(t_row_pin_singular_cannot_be_mistaken_for_row_pins);
    RUN(t_the_scan_drives_rows_low_and_reads_columns);
    RUN(t_a_closed_switch_becomes_its_key);
    RUN(t_a_held_key_is_reported_once_and_released_after_three_scans);
    RUN(t_an_unmapped_position_is_not_a_key);
    RUN(t_both_input_event_layouts_decode);
    RUN(t_the_list_lays_out_exactly_as_nd_vlist_does);
    RUN(t_the_window_follows_the_selection_and_stops_at_the_end);
    RUN(t_the_notch_truncates_rather_than_rounds);
    RUN(t_the_list_keys_are_the_ones_the_framework_has);
    RUN(t_the_progress_boxes_are_the_frameworks);
    RUN(t_the_percentage_truncates_and_clamps);
    RUN(t_the_percent_gate_stops_the_bar_outrunning_the_write);
    RUN(t_the_byte_reading_is_readable_at_a_glance);
    RUN(t_the_bitmap_font_advances_match_pillow);
    RUN(t_text_that_does_not_fit_is_cut_at_the_box);
    RUN(t_the_three_colours_survive_both_depths);
    RUN(t_the_list_clears_only_the_content_rows);
    RUN(t_the_destructive_question_opens_with_neither_answer_lit);
    RUN(t_a_long_message_stops_before_the_prompt);
    RUN(t_a_message_too_wide_for_the_big_font_is_not_cut);
    RUN(t_a_raw_splash_blits_at_either_depth);
    RUN(t_render_the_screens);

    pt_cleanup();
    printf("test_recui: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
