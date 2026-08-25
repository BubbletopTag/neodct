/* test_keypadsetup.c -- the first-boot keypad wizard, WP-27.
 *
 * The module under test is the only thing standing between a freshly flashed
 * phone and a phone whose keys do nothing, and it runs before any UI exists
 * to complain to. So this file drives the whole of it -- gates, scan, split,
 * payload, screens -- through the fake-chip hook nd_keypad.h describes, and
 * finishes by reading the file back with the loader the core actually uses.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. KEY_TARGETS is i2c_keypad_setup.py's sixteen pairs in its order, and
 *     every name in it is one nd_keycode_for_name() resolves. That second
 *     half is the whole point: a name this wizard invents is a key the phone
 *     will not have when nd_keymap_load() reads the file back.
 *
 *  2. scan_pairs() drives each of the sixteen pins low in turn and releases
 *     everything afterwards, reports a switch found from either end ONCE, and
 *     answers a dead bus with ND_ERR_IO rather than a guess -- a guess here
 *     is a phantom key enrolled against the wrong pins.
 *
 *  3. wait_new_pair() insists on EXACTLY ONE pair. Two keys held together are
 *     ignored until one is let go, which is what stops a fumbled press being
 *     enrolled as the wrong button.
 *
 *  4. The bipartition is the row/column split, it is stable across runs, and
 *     it refuses a graph that is not a matrix.
 *
 *  5. int(os.environ.get(SETUP_BUS_ENV, DEFAULT_SETUP_BUS)) really does mean
 *     that NEODCT_KEYPAD_SETUP_BUS="" disables first-boot setup instead of
 *     falling back to bus 3. Quirk, asserted as one.
 *
 *  6. THE ROUND TRIP. The wizard is run end to end over a fake chip, and what
 *     it writes is loaded by nd_keymap_load(): every pin, both i2c fields, and
 *     all sixteen matrix positions carrying the right keycode. A keymap this
 *     writes that the core cannot read is a phone with no keys, so this is the
 *     assertion the module exists to satisfy.
 *
 *  7. Ink reaches the panel. The wizard draws with nd_image/nd_draw/nd_font
 *     and nd_fb_update() because at this point in the boot that is all there
 *     is, so the test gives it a memory framebuffer and a real font and checks
 *     that the last screen is not blank.
 *
 * ============ WHAT NO HOST TEST CAN CLAIM ============
 *
 * That any of this works against a PCF8575. The socketpair below is a model
 * of the chip -- write two bytes, read two bytes, driven pins read low -- and
 * every line of the scan runs against it, but the model is ours. Settle time,
 * an unpopulated address's answer, and whether the eight-address probe finds
 * the chip on the owner's board all need /dev/i2c-3 and real hardware.
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_keypadsetup.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * The fake chip
 * ------------------------------------------------------------------ *
 *
 * Straight out of test_keypad.c, which introduced it: a socketpair is two
 * descriptors joined back to back with a buffer each way, which is the exact
 * shape of an i2c transaction against a chip that has no register byte. The
 * driver's end is non-blocking on purpose -- a scan that reads more words
 * than the test seeded fails with ND_ERR_IO instead of hanging the suite.
 */

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

/* One write() per PASS, not per word.
 *
 * A 2-byte write on an AF_UNIX stream socket is charged about 360 bytes of
 * socket buffer, so a sixteen-word pass sent word by word costs 5.8 KB of a
 * budget measured near 100 KB -- seventeen passes and the seeding itself
 * starts returning EAGAIN. The bytes on the wire are identical either way,
 * because the driver reads a stream and does not see message boundaries. */
static void chip_write_words(fake_chip *fc, const uint16_t *words, size_t n)
{
    uint8_t buf[64];
    size_t off = 0u;
    size_t i;

    CHECK(n * 2u <= sizeof buf);
    for (i = 0u; i < n; i++) {
        buf[i * 2u] = (uint8_t)(words[i] & 0xFFu);
        buf[i * 2u + 1u] = (uint8_t)(((uint32_t)words[i] >> 8) & 0xFFu);
    }
    while (off < n * 2u) {
        ssize_t got = write(fc->chip_fd, buf + off, n * 2u - off);

        if (got <= 0) {
            CHECK_INT(got, (ssize_t)(n * 2u - off));
            return;
        }
        off += (size_t)got;
    }
}

/* What a PCF8575 puts on the wire while pin `drive` is held low and `pressed`
 * is the set of switches currently closed. The driven pin reads low as well
 * as everything shorted to it -- scan_pairs() skips its own drive bit, but a
 * real chip does not, and modelling it the other way would hide a bug. */
static uint16_t word_for(uint8_t drive, const nd_kpsetup_pair *pressed, size_t n)
{
    uint16_t v = (uint16_t)(0xFFFFu & ~(1u << drive));
    size_t i;

    for (i = 0u; i < n; i++) {
        if (pressed[i].a == drive)
            v = (uint16_t)(v & ~(uint16_t)(1u << pressed[i].b));
        else if (pressed[i].b == drive)
            v = (uint16_t)(v & ~(uint16_t)(1u << pressed[i].a));
    }
    return v;
}

/* The sixteen answers one full discovery pass consumes. */
static void chip_seed_pass(fake_chip *fc, const nd_kpsetup_pair *pressed, size_t n)
{
    uint16_t words[16];
    uint32_t d;

    for (d = 0u; d < 16u; d++)
        words[d] = word_for((uint8_t)d, pressed, n);
    chip_write_words(fc, words, 16u);
}

/* ------------------------------------------------------------------ *
 * A chip that keeps up
 * ------------------------------------------------------------------ *
 *
 * The other direction has the same accounting problem and cannot be batched:
 * nd_pcf8575_write16() is one 2-byte write per drive word and the whole
 * enrolment is 1,088 of them, which is four times the socket's budget. A real
 * chip consumes every transaction as it arrives, so the fake one gets a
 * thread that does the same and throws the words away. What they contain is
 * asserted elsewhere, on a pass short enough to inspect.
 *
 * Nothing is shared with the wizard's thread but the descriptor and one
 * atomic flag; the replies are all seeded before the drain starts.
 */

typedef struct {
    fake_chip *fc;
    atomic_int stop;
    pthread_t tid;
    bool running;
} chip_drain_thread;

static void *chip_drain_main(void *arg)
{
    chip_drain_thread *d = (chip_drain_thread *)arg;
    uint8_t scratch[512];

    for (;;) {
        ssize_t got = read(d->fc->chip_fd, scratch, sizeof scratch);

        if (got > 0)
            continue;
        if (atomic_load(&d->stop) != 0)
            break;
        /* EAGAIN on a non-blocking socket with nothing in it yet. */
        (void)usleep(500);
    }
    return NULL;
}

static void chip_drain_start(chip_drain_thread *d, fake_chip *fc)
{
    d->fc = fc;
    atomic_init(&d->stop, 0);
    d->running = (pthread_create(&d->tid, NULL, chip_drain_main, d) == 0);
    CHECK(d->running);
}

static void chip_drain_stop(chip_drain_thread *d)
{
    if (!d->running)
        return;
    atomic_store(&d->stop, 1);
    (void)pthread_join(d->tid, NULL);
    d->running = false;
}

/* Everything the driver has written since the last drain. */
static size_t chip_drain(fake_chip *fc, uint16_t *out, size_t max)
{
    uint8_t buf[4096];
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
 * A real font, under the case root
 * ------------------------------------------------------------------ *
 *
 * The wizard opens ND_PATH_FONT through nd_path_resolve(), so putting a copy
 * of the overlay's font.ttf at that virtual path is all it takes to make the
 * screens draw for real. Without it the faces come back NULL and the wizard
 * still runs -- which is itself the documented behaviour -- but nothing can
 * be said about the pixels.
 */

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

static bool file_exists_plain(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static bool install_font(void)
{
    static char buf[65536];
    char cand[ND_PATH_MAX];
    const char *golden = getenv("NEODCT_GOLDEN");
    const char *found = NULL;
    FILE *f;
    size_t n;

    if (golden != NULL && golden[0] != '\0') {
        /* <repo>/neodct/tests/golden -> <repo>/neodct */
        char base[ND_PATH_MAX];
        char *cut;

        (void)snprintf(base, sizeof base, "%.480s", golden);
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        (void)snprintf(cand, sizeof cand, "%.400s/" FONT_REL, base);
        if (file_exists_plain(cand))
            found = cand;
    }
    if (found == NULL && file_exists_plain("../" FONT_REL))
        found = "../" FONT_REL;
    if (found == NULL)
        return false;

    f = fopen(found, "rb");
    if (f == NULL)
        return false;
    n = fread(buf, 1u, sizeof buf, f);
    (void)fclose(f);
    if (n == 0u)
        return false;
    pt_write(ND_PATH_FONT, buf, n);
    return true;
}

/* ------------------------------------------------------------------ *
 * KEY_TARGETS
 * ------------------------------------------------------------------ */

static void test_key_targets(void)
{
    /* i2c_keypad_setup.py KEY_TARGETS, in order. The order is the enrolment
     * order and is shared with the console builder; changing it changes which
     * key someone is asked for first on a phone that has never worked. */
    static const char *const names[ND_KPSETUP_N_TARGETS] = {
        "navikey", "clear", "up",    "down",  "num_1", "num_2", "num_3", "num_4",
        "num_5",   "num_6", "num_7", "num_8", "num_9", "num_0", "star",  "hash"};
    static const char *const labels[ND_KPSETUP_N_TARGETS] = {"NaviKey (center)",
                                                             "C (clear/back)",
                                                             "Up",
                                                             "Down",
                                                             "1",
                                                             "2",
                                                             "3",
                                                             "4",
                                                             "5",
                                                             "6",
                                                             "7",
                                                             "8",
                                                             "9",
                                                             "0",
                                                             "*",
                                                             "#"};
    size_t i;
    size_t j;

    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        CHECK_STR(nd_kpsetup_targets[i].name, names[i]);
        CHECK_STR(nd_kpsetup_targets[i].label, labels[i]);
        /* The comment above KEY_TARGETS says it must mirror
         * MATRIX_NAME_TO_CODE. A name that does not resolve is a button the
         * phone will not have after the file is written. */
        CHECK(nd_keycode_for_name(nd_kpsetup_targets[i].name) >= 0);
        for (j = 0u; j < i; j++)
            CHECK(strcmp(nd_kpsetup_targets[i].name, nd_kpsetup_targets[j].name) != 0);
    }
}

static void test_constants(void)
{
    CHECK_INT(ND_KPSETUP_DEFAULT_BUS, 3);
    CHECK_STR(ND_KPSETUP_ENV_BUS, "NEODCT_KEYPAD_SETUP_BUS");
    CHECK_INT(ND_KPSETUP_PROBE_FIRST, 0x20);
    CHECK_INT(ND_KPSETUP_PROBE_LAST, 0x27); /* range(0x20, 0x28) */
    CHECK(ND_KPSETUP_FIRST_KEY_TIMEOUT == 120.0);
    CHECK(ND_KPSETUP_KEY_TIMEOUT == 60.0);
    CHECK_INT(ND_KPSETUP_RELEASE_SCANS, 3);
    CHECK(ND_KPSETUP_RELEASE_MAX_S == 10.0);
    CHECK(ND_KPSETUP_POLL_S == 0.01);
    CHECK_INT(ND_KPSETUP_SETTLE_US, 500); /* time.sleep(0.0005) */
    CHECK(ND_KPSETUP_BUS_WAIT_S == 8.0);
    CHECK_INT(ND_KPSETUP_UI_W, 240);
    CHECK_INT(ND_KPSETUP_UI_H, 175);
    /* The Python asks for 26, which is deliberately not one of nd_font.h's
     * four UI sizes; see the module header for why it stays 26. */
    CHECK_INT(ND_KPSETUP_FONT_BIG_PX, 26);
    CHECK_INT(ND_KPSETUP_FONT_PX, 18);
    CHECK_INT(ND_KPSETUP_FONT_SMALL_PX, 14);
    CHECK_INT(ND_KPSETUP_N_TARGETS, 16);

    CHECK_STR(nd_kpsetup_format, "neodct.keymap.v3.matrix.i2c");
    CHECK_STR(nd_kpsetup_driver, "pcf8575-i2c");
    CHECK_STR(nd_kpsetup_title, "Keypad setup");
    CHECK_STR(nd_kpsetup_press, "Press:");
    CHECK_STR(nd_kpsetup_press_each, "Press each key as asked.");
    CHECK_STR(nd_kpsetup_aborted, "Setup aborted");
    CHECK_STR(nd_kpsetup_no_press, "No key was pressed.");
    CHECK_STR(nd_kpsetup_without, "Starting without a keymap.");
    CHECK_STR(nd_kpsetup_failed, "Setup failed");
    CHECK_STR(nd_kpsetup_saved, "Keymap saved!");
    CHECK_STR(nd_kpsetup_restarting, "Restarting UI...");
    CHECK_STR(nd_kpsetup_no_bus_title, "No keypad bus");
    CHECK_STR(nd_kpsetup_no_chip_title, "No keypad found");
}

static void test_the_lines_that_carry_a_number(void)
{
    char lines[3][ND_KPSETUP_LINE_MAX];
    char buf[ND_KPSETUP_LINE_MAX];

    CHECK_INT(nd_kpsetup_found_lines(lines, 3u, 3, 0x20), 3);
    CHECK_STR(lines[0], "Keypad found on bus 3");
    CHECK_STR(lines[1], "(PCF8575 at 0x20)");
    CHECK_STR(lines[2], "Press each key as asked.");

    /* f"0x{addr:02X}" -- upper case, two digits, zero padded. */
    CHECK_INT(nd_kpsetup_found_lines(lines, 3u, 11, 0x27), 3);
    CHECK_STR(lines[1], "(PCF8575 at 0x27)");

    CHECK_INT(nd_kpsetup_no_chip_lines(lines, 3u, 3), 3);
    CHECK_STR(lines[0], "Nothing answered on /dev/i2c-3");
    CHECK_STR(lines[1], "(addresses 0x20-0x27).");
    CHECK_STR(lines[2], "Starting without a keymap.");

    CHECK_INT(nd_kpsetup_no_bus_lines(lines, 3u, 3), 2);
    CHECK_STR(lines[0], "/dev/i2c-3 does not exist.");
    CHECK_STR(lines[1], "Starting without a keymap.");

    CHECK_INT(nd_kpsetup_used_note(buf, sizeof buf, "NaviKey (center)"), ND_OK);
    CHECK_STR(buf, "Already used by 'NaviKey (center)'");

    /* f"{index + 1}/{total}" -- one-based on screen, zero-based in the loop. */
    CHECK_INT(nd_kpsetup_counter(buf, sizeof buf, 0u, 16u), ND_OK);
    CHECK_STR(buf, "1/16");
    CHECK_INT(nd_kpsetup_counter(buf, sizeof buf, 15u, 16u), ND_OK);
    CHECK_STR(buf, "16/16");

    /* A short buffer must report truncation rather than write a wrong line. */
    CHECK_INT(nd_kpsetup_used_note(buf, 4u, "NaviKey (center)"), ND_ERR_TOOLONG);
    CHECK_INT(nd_kpsetup_found_lines(lines, 2u, 3, 0x20), 0);
}

/* ------------------------------------------------------------------ *
 * The bus override
 * ------------------------------------------------------------------ */

static void test_bus_from_env(void)
{
    int bus = -1;

    /* os.environ.get(SETUP_BUS_ENV, DEFAULT_SETUP_BUS) hands int() the
     * integer 3 when the variable is absent, so the default is never parsed
     * and can never be rejected. */
    CHECK(nd_kpsetup_bus_from_env(NULL, &bus));
    CHECK_INT(bus, 3);

    CHECK(nd_kpsetup_bus_from_env("0", &bus));
    CHECK_INT(bus, 0);
    CHECK(nd_kpsetup_bus_from_env("11", &bus));
    CHECK_INT(bus, 11);
    /* int() tolerates surrounding whitespace and a sign. */
    CHECK(nd_kpsetup_bus_from_env("  7\n", &bus));
    CHECK_INT(bus, 7);
    CHECK(nd_kpsetup_bus_from_env("+4", &bus));
    CHECK_INT(bus, 4);
    CHECK(nd_kpsetup_bus_from_env("-1", &bus));
    CHECK_INT(bus, -1);

    /* THE QUIRK. This call has no `or DEFAULT_BUS` the way KeypadMapperI2C's
     * bus parse does, so int("") raises and the whole wizard is skipped.
     * Setting NEODCT_KEYPAD_SETUP_BUS to nonsense turns first-boot setup off;
     * it does not fall back to bus 3. */
    CHECK(!nd_kpsetup_bus_from_env("", &bus));
    CHECK(!nd_kpsetup_bus_from_env("   ", &bus));
    CHECK(!nd_kpsetup_bus_from_env("three", &bus));
    CHECK(!nd_kpsetup_bus_from_env("3x", &bus));
    CHECK(!nd_kpsetup_bus_from_env("0x3", &bus)); /* int() is base 10 here */
    CHECK(!nd_kpsetup_bus_from_env("3.0", &bus));
}

/* ------------------------------------------------------------------ *
 * scan_pairs
 * ------------------------------------------------------------------ */

static void test_scan_pairs_drives_every_pin_and_releases(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair pairs[ND_KPSETUP_MAX_PAIRS];
    uint16_t words[64];
    size_t n = 99u;
    size_t got;
    size_t i;

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    chip_seed_pass(&fc, NULL, 0u);

    CHECK_INT(nd_kpsetup_scan_pairs(&chip, pairs, ND_ARRAY_LEN(pairs), &n), ND_OK);
    CHECK_INT(n, 0);

    /* Sixteen drive words, then the release. Unlike the shipping scanner,
     * which only drives the row pins it was told about, this one drives ALL
     * sixteen -- that is what lets it discover a keypad nobody has measured. */
    got = chip_drain(&fc, words, ND_ARRAY_LEN(words));
    CHECK_INT(got, 17);
    for (i = 0u; i < 16u; i++)
        CHECK_INT(words[i], (uint16_t)(0xFFFFu & ~(1u << i)));
    CHECK_INT(words[16], 0xFFFF);

    fake_chip_close(&fc);
}

static void test_scan_pairs_reports_each_switch_once(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair pressed[2];
    nd_kpsetup_pair pairs[ND_KPSETUP_MAX_PAIRS];
    size_t n = 0u;

    /* P2-P6 and P0-P7 shorted. Each is seen twice per pass, once from each
     * end, and each is one pair -- min first, as the Python's
     * (min(drive, bit), max(drive, bit)) does. */
    pressed[0].a = 2u;
    pressed[0].b = 6u;
    pressed[1].a = 0u;
    pressed[1].b = 7u;

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    chip_seed_pass(&fc, pressed, 2u);

    CHECK_INT(nd_kpsetup_scan_pairs(&chip, pairs, ND_ARRAY_LEN(pairs), &n), ND_OK);
    CHECK_INT(n, 2);
    /* Discovery order is drive order, so P0's partner comes out first. */
    CHECK_INT(pairs[0].a, 0);
    CHECK_INT(pairs[0].b, 7);
    CHECK_INT(pairs[1].a, 2);
    CHECK_INT(pairs[1].b, 6);

    fake_chip_close(&fc);
}

static void test_scan_pairs_refuses_to_guess_at_a_dead_bus(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair pairs[ND_KPSETUP_MAX_PAIRS];
    uint16_t words[64];
    size_t n = 0u;

    /* Nothing seeded: the first read finds an empty non-blocking socket,
     * which is what a chip that is not there looks like. Inventing a word
     * here enrols a phantom key against pins nothing is wired to. */
    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    CHECK_INT(nd_kpsetup_scan_pairs(&chip, pairs, ND_ARRAY_LEN(pairs), &n), ND_ERR_IO);

    /* And the trailing 0xFFFF release is NOT sent, because the Python's
     * exception does not reach its own trailing write16 either. */
    CHECK_INT(chip_drain(&fc, words, ND_ARRAY_LEN(words)), 1);
    CHECK_INT(words[0], 0xFFFE);

    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * wait_new_pair / wait_release
 * ------------------------------------------------------------------ */

static void test_wait_new_pair_wants_exactly_one(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair two[2];
    nd_kpsetup_pair one[1];
    nd_kpsetup_pair got;
    bool found = false;
    size_t i;

    two[0].a = 1u;
    two[0].b = 5u;
    two[1].a = 2u;
    two[1].b = 6u;
    one[0].a = 3u;
    one[0].b = 4u;

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);

    /* Two keys held down is two pairs and is not an answer. Neither is none.
     * Only the pass with a single pair ends the wait. */
    for (i = 0u; i < 3u; i++)
        chip_seed_pass(&fc, two, 2u);
    chip_seed_pass(&fc, NULL, 0u);
    chip_seed_pass(&fc, one, 1u);

    CHECK_INT(nd_kpsetup_wait_new_pair(&chip, 5.0, &got, &found), ND_OK);
    CHECK(found);
    CHECK_INT(got.a, 3);
    CHECK_INT(got.b, 4);

    fake_chip_close(&fc);
}

static void test_wait_new_pair_times_out_into_the_abort_path(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair got;
    bool found = true;
    size_t i;

    /* Plenty of empty passes: the wait must end on its deadline, not on a
     * starved socket, or this would be testing the harness. */
    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    for (i = 0u; i < 64u; i++)
        chip_seed_pass(&fc, NULL, 0u);

    CHECK_INT(nd_kpsetup_wait_new_pair(&chip, 0.05, &got, &found), ND_OK);
    /* Not an error -- it is "Setup aborted / No key was pressed." */
    CHECK(!found);

    fake_chip_close(&fc);
}

static void test_wait_release_needs_three_consecutive_empty_scans(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    nd_kpsetup_pair one[1];
    uint16_t words[256];
    size_t i;

    one[0].a = 0u;
    one[0].b = 4u;

    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);

    /* Held, released, released, TOUCHED AGAIN, then three clean passes. The
     * counter resets on the fourth, so the run of three has to start over --
     * which is the debounce a mushy membrane contact needs. */
    chip_seed_pass(&fc, one, 1u);
    chip_seed_pass(&fc, NULL, 0u);
    chip_seed_pass(&fc, NULL, 0u);
    chip_seed_pass(&fc, one, 1u);
    for (i = 0u; i < 3u; i++)
        chip_seed_pass(&fc, NULL, 0u);

    CHECK_INT(nd_kpsetup_wait_release(&chip, 5.0), ND_OK);
    /* Seven passes consumed, seventeen words each. */
    CHECK_INT(chip_drain(&fc, words, ND_ARRAY_LEN(words)), 7 * 17);

    fake_chip_close(&fc);
}

/* ------------------------------------------------------------------ *
 * _bipartition
 * ------------------------------------------------------------------ */

static void test_bipartition_splits_a_matrix(void)
{
    nd_kpsetup_pair pairs[16];
    uint8_t rows[ND_KPSETUP_MAX_PINS];
    uint8_t cols[ND_KPSETUP_MAX_PINS];
    size_t n_rows = 0u;
    size_t n_cols = 0u;
    size_t i;

    /* A 4x4 keypad wired to P00-P03 and P04-P07. */
    for (i = 0u; i < 16u; i++) {
        pairs[i].a = (uint8_t)(i / 4u);
        pairs[i].b = (uint8_t)(4u + (i % 4u));
    }

    CHECK(nd_kpsetup_bipartition(pairs, 16u, rows, &n_rows, cols, &n_cols, NULL, NULL));
    CHECK_INT(n_rows, 4);
    CHECK_INT(n_cols, 4);
    /* Both lists sorted ascending, and the class containing the numerically
     * smallest pin is the one that becomes "rows". That is arbitrary in
     * exactly the way the Python's is, and it is stable. */
    for (i = 0u; i < 4u; i++) {
        CHECK_INT(rows[i], i);
        CHECK_INT(cols[i], 4u + i);
    }
}

static void test_bipartition_ignores_unwired_pins(void)
{
    nd_kpsetup_pair pairs[2];
    uint8_t rows[ND_KPSETUP_MAX_PINS];
    uint8_t cols[ND_KPSETUP_MAX_PINS];
    size_t n_rows = 0u;
    size_t n_cols = 0u;

    /* Two components, P1-P9 and P3-P4. The smallest pin of each takes
     * colour 0, so P1 and P3 are rows; the twelve pins nothing is wired to
     * appear in neither list. */
    pairs[0].a = 1u;
    pairs[0].b = 9u;
    pairs[1].a = 3u;
    pairs[1].b = 4u;

    CHECK(nd_kpsetup_bipartition(pairs, 2u, rows, &n_rows, cols, &n_cols, NULL, NULL));
    CHECK_INT(n_rows, 2);
    CHECK_INT(n_cols, 2);
    CHECK_INT(rows[0], 1);
    CHECK_INT(rows[1], 3);
    CHECK_INT(cols[0], 4);
    CHECK_INT(cols[1], 9);
}

static void test_bipartition_refuses_a_graph_that_is_not_a_matrix(void)
{
    nd_kpsetup_pair pairs[3];
    uint8_t rows[ND_KPSETUP_MAX_PINS];
    uint8_t cols[ND_KPSETUP_MAX_PINS];
    size_t n_rows = 0u;
    size_t n_cols = 0u;
    uint8_t ca = 99u;
    uint8_t cb = 99u;

    /* P0-P1-P2-P0: an odd cycle, which no matrix can produce. On a real
     * phone this is a mis-press ghosting through a third switch, and enrolling
     * it would produce a keymap whose rows and columns are nonsense. */
    pairs[0].a = 0u;
    pairs[0].b = 1u;
    pairs[1].a = 1u;
    pairs[1].b = 2u;
    pairs[2].a = 0u;
    pairs[2].b = 2u;

    CHECK(!nd_kpsetup_bipartition(pairs, 3u, rows, &n_rows, cols, &n_cols, &ca, &cb));
    CHECK(ca < ND_KPSETUP_MAX_PINS);
    CHECK(cb < ND_KPSETUP_MAX_PINS);
}

/* ------------------------------------------------------------------ *
 * _build_payload
 * ------------------------------------------------------------------ */

/* A full sixteen-key enrolment on a 4x4 keypad wired to P00-P07: target i is
 * at matrix (i/4, i%4), which is pins (i/4, 4 + i%4). */
static void fill_enrolment(nd_kpsetup_pair *pairs, bool *have)
{
    size_t i;

    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        pairs[i].a = (uint8_t)(i / 4u);
        pairs[i].b = (uint8_t)(4u + (i % 4u));
        have[i] = true;
    }
}

static void test_build_keymap(void)
{
    nd_kpsetup_pair pairs[ND_KPSETUP_N_TARGETS];
    bool have[ND_KPSETUP_N_TARGETS];
    nd_keymap km;
    char err[ND_KPSETUP_ERR_MAX];
    size_t i;

    fill_enrolment(pairs, have);
    CHECK_INT(nd_kpsetup_build_keymap(&km, pairs, have, 3, 0x20, err, sizeof err), ND_OK);
    CHECK_STR(err, "");
    CHECK_STR(km.format, "neodct.keymap.v3.matrix.i2c");
    CHECK_STR(km.driver, "pcf8575-i2c");
    CHECK_INT(km.i2c_bus, 3);
    CHECK_INT(km.i2c_addr, 0x20);
    CHECK_INT(km.n_rows, 4);
    CHECK_INT(km.n_cols, 4);

    for (i = 0u; i < 4u; i++) {
        CHECK_INT(km.row_pins[i], i);
        CHECK_INT(km.col_pins[i], 4u + i);
    }
    /* row and col are INDICES into row_pins/col_pins, not pin numbers -- the
     * Python writes both, and it is the index the scanner matches on. */
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        CHECK_INT(km.matrix_to_code[i / 4u][i % 4u],
                  nd_keycode_for_name(nd_kpsetup_targets[i].name));
    }
}

static void test_build_keymap_tolerates_a_part_enrolment(void)
{
    nd_kpsetup_pair pairs[ND_KPSETUP_N_TARGETS];
    bool have[ND_KPSETUP_N_TARGETS];
    nd_keymap km;
    char err[ND_KPSETUP_ERR_MAX];

    /* The completed wizard never produces this, but _build_payload's
     * `if t[0] in pair_by_name` guard means it is defined behaviour, and a
     * caller that only enrolled the navigation cluster gets a usable file. */
    memset(have, 0, sizeof have);
    fill_enrolment(pairs, have);
    memset(have, 0, sizeof have);
    have[0] = true; /* navikey, pins (0,4) */
    have[3] = true; /* down,    pins (0,7) */

    CHECK_INT(nd_kpsetup_build_keymap(&km, pairs, have, 3, 0x20, err, sizeof err), ND_OK);
    CHECK_INT(km.n_rows, 1);
    CHECK_INT(km.n_cols, 2);
    CHECK_INT(km.row_pins[0], 0);
    CHECK_INT(km.col_pins[0], 4);
    CHECK_INT(km.col_pins[1], 7);
    CHECK_INT(km.matrix_to_code[0][0], nd_keycode_for_name("navikey"));
    CHECK_INT(km.matrix_to_code[0][1], nd_keycode_for_name("down"));
}

static void test_build_keymap_reports_a_conflict_the_python_way(void)
{
    nd_kpsetup_pair pairs[ND_KPSETUP_N_TARGETS];
    bool have[ND_KPSETUP_N_TARGETS];
    nd_keymap km;
    char err[ND_KPSETUP_ERR_MAX];

    memset(pairs, 0, sizeof pairs);
    memset(have, 0, sizeof have);
    pairs[0].a = 0u;
    pairs[0].b = 1u;
    pairs[1].a = 1u;
    pairs[1].b = 2u;
    pairs[2].a = 0u;
    pairs[2].b = 2u;
    have[0] = true;
    have[1] = true;
    have[2] = true;

    /* f"P{a}/P{b} conflict", which is what the owner reads under "Setup
     * failed". CPython picks WHICH clashing edge to name by set iteration
     * order and is not reproducible; the shape of the message is. */
    CHECK_INT(nd_kpsetup_build_keymap(&km, pairs, have, 3, 0x20, err, sizeof err), ND_ERR_PARSE);
    CHECK_STR(err, "P2/P1 conflict");
}

/* ------------------------------------------------------------------ *
 * THE ROUND TRIP
 * ------------------------------------------------------------------ */

static void test_what_it_writes_the_core_can_read(void)
{
    nd_kpsetup_pair pairs[ND_KPSETUP_N_TARGETS];
    bool have[ND_KPSETUP_N_TARGETS];
    nd_keymap km;
    nd_keymap back;
    char err[ND_KPSETUP_ERR_MAX];
    size_t i;

    /* nd_keymap_load() is the consumer, and it is the only one. A keymap this
     * wizard writes that the loader refuses is a phone with no keys and no
     * way to say so, which is the exact failure the wizard exists to end. */
    fill_enrolment(pairs, have);
    CHECK_INT(nd_kpsetup_build_keymap(&km, pairs, have, 3, 0x21, err, sizeof err), ND_OK);
    CHECK_INT(nd_keymap_save(&km, ND_PATH_KEYMAP), ND_OK);

    CHECK_INT(nd_keymap_load(ND_PATH_KEYMAP, &back), ND_OK);
    CHECK_STR(back.format, "neodct.keymap.v3.matrix.i2c");
    CHECK_STR(back.driver, "pcf8575-i2c");
    CHECK_INT(back.i2c_bus, 3);
    CHECK_INT(back.i2c_addr, 0x21);
    CHECK_INT(back.n_rows, 4);
    CHECK_INT(back.n_cols, 4);
    for (i = 0u; i < 4u; i++) {
        CHECK_INT(back.row_pins[i], km.row_pins[i]);
        CHECK_INT(back.col_pins[i], km.col_pins[i]);
    }
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        CHECK_INT(back.matrix_to_code[i / 4u][i % 4u],
                  nd_keycode_for_name(nd_kpsetup_targets[i].name));
    }
    /* Nothing else in the 16x16 was invented. */
    {
        size_t r;
        size_t c;
        size_t mapped = 0u;

        for (r = 0u; r < ND_KEYMAP_MAX_ROWS; r++) {
            for (c = 0u; c < ND_KEYMAP_MAX_COLS; c++) {
                if (back.matrix_to_code[r][c] >= 0)
                    mapped++;
            }
        }
        CHECK_INT(mapped, ND_KPSETUP_N_TARGETS);
    }

    /* Written through the temp file, and the temp file is gone. */
    CHECK(!nd_path_exists("/NeoDCT/User/keymap.json.tmp"));
}

/* ------------------------------------------------------------------ *
 * The gates
 * ------------------------------------------------------------------ */

static void test_gate_check(void)
{
    /* QEMU and dev boxes: no bus node, no FIQ console. Silence, because
     * announcing a missing keypad on a machine that has never had one is
     * noise on every developer's console. */
    CHECK_INT(nd_kpsetup_gate_check(3), ND_KPSETUP_GATE_QUIET);

    /* On hardware every skip is announced, so a silently dead keypad can be
     * diagnosed without a serial cable -- which is the whole reason
     * _is_real_hardware() exists. */
    pt_write_text(ND_PATH_SERIAL_FIQ, "");
    CHECK_INT(nd_kpsetup_gate_check(3), ND_KPSETUP_GATE_WAIT_FOR_BUS);

    /* A bus node that IS there skips the coldplug grace entirely. */
    pt_write_text("/dev/i2c-3", "");
    CHECK_INT(nd_kpsetup_gate_check(3), ND_KPSETUP_GATE_PROBE);
    /* ...and the override really does select a different node. */
    CHECK_INT(nd_kpsetup_gate_check(7), ND_KPSETUP_GATE_WAIT_FOR_BUS);

    /* A phone that already works is never asked to do this again, whatever
     * else is or is not plugged in. */
    pt_write_text(ND_PATH_KEYMAP, "{}");
    CHECK_INT(nd_kpsetup_gate_check(3), ND_KPSETUP_GATE_HAVE_KEYMAP);
}

static void test_maybe_run_skips_a_phone_that_already_works(void)
{
    pt_write_text(ND_PATH_KEYMAP, "{}");
    /* Through the real entry point, with no framebuffer -- which is also the
     * nd-core --headless case. It must return promptly and touch nothing. */
    CHECK(!nd_kpsetup_maybe_run(NULL, false));
}

static void test_maybe_run_is_quiet_on_a_dev_box(void)
{
    /* No keymap, no bus, no FIQ console: the wizard must not draw, must not
     * sleep, and must not print. */
    CHECK(!nd_kpsetup_maybe_run(NULL, false));
}

static void test_a_nonsense_bus_override_disables_setup(void)
{
    /* The other half of the int() quirk, end to end: a bad override unwinds
     * out of maybe_run_first_time_setup in the Python and skips the wizard
     * here, rather than quietly probing bus 3. */
    CHECK_INT(setenv(ND_KPSETUP_ENV_BUS, "not-a-bus", 1), 0);
    CHECK(!nd_kpsetup_maybe_run(NULL, false));
    CHECK_INT(unsetenv(ND_KPSETUP_ENV_BUS), 0);
}

/* ------------------------------------------------------------------ *
 * The whole wizard
 * ------------------------------------------------------------------ */

static void test_the_whole_wizard(void)
{
    fake_chip fc;
    chip_drain_thread drain;
    nd_pcf8575 chip;
    nd_fb *fb = NULL;
    nd_keymap back;
    nd_kpsetup_pair pressed[1];
    const uint8_t *bytes;
    size_t nbytes = 0u;
    size_t lit = 0u;
    size_t i;
    bool have_font = install_font();

    fake_chip_open(&fc);

    /* One press pass and three release passes per key, sixteen times over --
     * which is exactly what the wizard consumes when every key is pressed
     * once and let go cleanly. Seeding it up front is legitimate: each
     * transaction is two bytes on a socket, and the driver reads them in the
     * order a chip would have produced them. */
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        size_t j;

        pressed[0].a = (uint8_t)(i / 4u);
        pressed[0].b = (uint8_t)(4u + (i % 4u));
        chip_seed_pass(&fc, pressed, 1u);
        for (j = 0u; j < (size_t)ND_KPSETUP_RELEASE_SCANS; j++)
            chip_seed_pass(&fc, NULL, 0u);
    }

    /* 240x175 at 32bpp is the geometry neodct_displayd leaves the panel in,
     * so nd_fb_update() takes the single-memcpy path the phone takes. */
    CHECK_INT(nd_fb_open_mem(&fb, ND_KPSETUP_UI_W, ND_KPSETUP_UI_H, 32, 0u), ND_OK);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    chip_drain_start(&drain, &fc);

    /* restart=false: the shipping path re-execs nd-core here and does not
     * return, which a unit test obviously cannot do. Everything up to the
     * execv is the same code. */
    CHECK(nd_kpsetup_run_wizard(fb, &chip, 0x22, 3, false));

    CHECK_INT(nd_keymap_load(ND_PATH_KEYMAP, &back), ND_OK);
    CHECK_INT(back.i2c_bus, 3);
    CHECK_INT(back.i2c_addr, 0x22);
    CHECK_INT(back.n_rows, 4);
    CHECK_INT(back.n_cols, 4);
    CHECK_STR(back.driver, "pcf8575-i2c");
    for (i = 0u; i < 4u; i++) {
        CHECK_INT(back.row_pins[i], i);
        CHECK_INT(back.col_pins[i], 4u + i);
    }
    for (i = 0u; i < (size_t)ND_KPSETUP_N_TARGETS; i++) {
        CHECK_INT(back.matrix_to_code[i / 4u][i % 4u],
                  nd_keycode_for_name(nd_kpsetup_targets[i].name));
    }

    /* The panel. The last frame is "Keymap saved!" over "16 keys mapped." and
     * "Restarting UI...", white on black, so a framebuffer with no lit
     * subpixel at all means the pre-UI drawing path never reached it. Alpha
     * is skipped: the BGRA packer writes 255 into it for every pixel. */
    bytes = nd_fb_mem_bytes(fb, &nbytes);
    CHECK(bytes != NULL);
    if (bytes != NULL) {
        for (i = 0u; i < nbytes; i++) {
            if ((i % 4u) != 3u && bytes[i] != 0u)
                lit++;
        }
    }
    if (have_font)
        CHECK(lit > 100u);
    else
        fprintf(stderr, "note: no font.ttf found; the pixel check was skipped\n");

    nd_pcf8575_close(&chip);
    chip_drain_stop(&drain);
    nd_fb_close(fb);
    fake_chip_close(&fc);
}

static void test_the_wizard_aborts_when_nobody_presses_anything(void)
{
    fake_chip fc;
    nd_pcf8575 chip;
    size_t i;

    /* A bus that answers but a keypad nobody touches. The Python shows
     * "Setup aborted / No key was pressed." and boots without a keymap rather
     * than sitting on the wizard forever -- the timeout is not an error path,
     * it is the way out for someone who cannot press anything.
     *
     * Sixty-four empty passes and a first-key timeout the test shortens by
     * driving run_wizard's inner call directly; run_wizard itself waits the
     * Python's 120 s, which no unit test can sit through. */
    fake_chip_open(&fc);
    CHECK_INT(nd_pcf8575_attach(&chip, fc.driver_fd), ND_OK);
    for (i = 0u; i < 64u; i++)
        chip_seed_pass(&fc, NULL, 0u);

    {
        nd_kpsetup_pair got;
        bool found = true;

        CHECK_INT(nd_kpsetup_wait_new_pair(&chip, 0.05, &got, &found), ND_OK);
        CHECK(!found);
    }
    /* Nothing was written. */
    CHECK(!nd_path_exists(ND_PATH_KEYMAP));

    fake_chip_close(&fc);
}

static void test_null_safety(void)
{
    nd_kpsetup_pair pair;
    nd_keymap km;
    uint8_t rows[ND_KPSETUP_MAX_PINS];
    uint8_t cols[ND_KPSETUP_MAX_PINS];
    size_t n = 0u;
    bool found = false;
    char err[ND_KPSETUP_ERR_MAX];

    CHECK_INT(nd_kpsetup_scan_pairs(NULL, &pair, 1u, &n), ND_ERR_INVAL);
    CHECK_INT(nd_kpsetup_wait_new_pair(NULL, 0.0, &pair, &found), ND_ERR_INVAL);
    CHECK_INT(nd_kpsetup_wait_release(NULL, 0.0), ND_ERR_INVAL);
    CHECK(!nd_kpsetup_bipartition(NULL, 0u, rows, &n, cols, &n, NULL, NULL));
    CHECK_INT(nd_kpsetup_build_keymap(NULL, NULL, NULL, 3, 32, err, sizeof err), ND_ERR_INVAL);
    CHECK_INT(nd_kpsetup_probe(NULL, 3, NULL), ND_ERR_INVAL);
    CHECK(!nd_kpsetup_bus_from_env("3", NULL));
    CHECK(!nd_kpsetup_run_wizard(NULL, NULL, 32, 3, false));

    /* An empty edge list is a legal (if useless) graph, not a failure. */
    CHECK(nd_kpsetup_bipartition(&pair, 0u, rows, &n, cols, &n, NULL, NULL));
    memset(&km, 0, sizeof km);
    ND_UNUSED(km);
}

int main(void)
{
    RUN(test_key_targets);
    RUN(test_constants);
    RUN(test_the_lines_that_carry_a_number);
    RUN(test_bus_from_env);

    RUN(test_scan_pairs_drives_every_pin_and_releases);
    RUN(test_scan_pairs_reports_each_switch_once);
    RUN(test_scan_pairs_refuses_to_guess_at_a_dead_bus);

    RUN(test_wait_new_pair_wants_exactly_one);
    RUN(test_wait_new_pair_times_out_into_the_abort_path);
    RUN(test_wait_release_needs_three_consecutive_empty_scans);

    RUN(test_bipartition_splits_a_matrix);
    RUN(test_bipartition_ignores_unwired_pins);
    RUN(test_bipartition_refuses_a_graph_that_is_not_a_matrix);

    RUN(test_build_keymap);
    RUN(test_build_keymap_tolerates_a_part_enrolment);
    RUN(test_build_keymap_reports_a_conflict_the_python_way);

    RUN(test_what_it_writes_the_core_can_read);

    RUN(test_gate_check);
    RUN(test_maybe_run_skips_a_phone_that_already_works);
    RUN(test_maybe_run_is_quiet_on_a_dev_box);
    RUN(test_a_nonsense_bus_override_disables_setup);

    RUN(test_the_whole_wizard);
    RUN(test_the_wizard_aborts_when_nobody_presses_anything);
    RUN(test_null_safety);

    return pt_report("test_keypadsetup");
}
