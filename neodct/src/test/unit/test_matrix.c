/* test_matrix.c -- what the matrix backend does when the BUS fails, which is
 * the half test_keypad.c does not reach.
 *
 * test_keypad.c drives this scanner against a fake chip that always answers,
 * and asserts the scan arithmetic, the edge detection, the release debounce
 * and the keymap lookup. All of that is right and none of it is here. This
 * file is about the other case, the one the phone actually hit:
 *
 * ============ A SCAN ERROR IS NOT "NO KEY WAS PRESSED" ============
 *
 * matrix_poll() used to turn every failed scan into ND_KEY_NONE and throw the
 * error away. A bus that died after opening -- a loose ribbon, an expander
 * browning out, the i2c controller wedging -- was therefore indistinguishable
 * from a phone nobody was typing on. The matrix stayed "open", so nd_input's
 * reopen path refused to run (it only fires when there is no matrix),
 * nd_input_has_backend() went on answering yes, and the core never drew
 * anything. The phone was dead to every key and said nothing about it, on the
 * screen or in the log, for the rest of the session. That is the surviving
 * form of the owner's oldest complaint: a phone that is on, that looks fine,
 * and that ignores the keypad.
 *
 * So failures are counted, and ND_MATRIX_DEAD_SCANS of them in a row means
 * the bus is gone rather than glitching. The two cases below are that
 * threshold from both sides, because both sides are load-bearing: too eager
 * and a keypad that drops one scan in a thousand gets torn down mid-keypress;
 * too slow and the phone stays silently dead.
 *
 * The fake chip is a socketpair, the same shape test_keypad.c uses and for
 * the same reason: it fakes THE CHIP, not the driver, so the whole scan path
 * including the bit arithmetic is the code that ships. A scan with nothing
 * seeded to read is a chip that did not answer.
 */

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"

#include "platform_test.h"

static const uint8_t ROWS[4] = {0, 1, 2, 3};
static const uint8_t COLS[4] = {4, 5, 6, 7};

typedef struct {
    int driver_fd;
    int chip_fd;
} fake_chip;

static void fake_chip_open(fake_chip *fc)
{
    int fds[2];

    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fc->driver_fd = fds[0];
    fc->chip_fd = fds[1];
    /* Non-blocking on both ends: a scan that reads more words than the test
     * seeded must fail immediately rather than hang the suite. */
    CHECK(fcntl(fc->driver_fd, F_SETFL, fcntl(fc->driver_fd, F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK(fcntl(fc->chip_fd, F_SETFL, fcntl(fc->chip_fd, F_GETFL, 0) | O_NONBLOCK) == 0);
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

/* One 16-bit answer, low byte first, as the chip puts it on the wire. */
static void chip_reply(fake_chip *fc, uint16_t value)
{
    uint8_t b[2];

    b[0] = (uint8_t)(value & 0xFFu);
    b[1] = (uint8_t)((value >> 8) & 0xFFu);
    CHECK_INT(write(fc->chip_fd, b, 2u), 2);
}

/* The four answers one full pass consumes, with nothing pressed. */
static void chip_reply_quiet_pass(fake_chip *fc)
{
    size_t i;

    for (i = 0u; i < 4u; i++)
        chip_reply(fc, 0xFFFFu);
}

static void chip_discard(fake_chip *fc)
{
    uint8_t buf[512];

    while (read(fc->chip_fd, buf, sizeof buf) > 0) {}
}

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
    km->matrix_to_code[1][1] = ND_KEY_DOWN;
}

static void test_a_glitching_bus_is_not_a_dead_one(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    int i;

    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    chip_discard(&fc);

    /* One short of the threshold. A membrane keypad on a long ribbon really
     * does lose the odd transfer, and a phone that tore its keypad down over
     * one of them would be worse than the bug this fixes. */
    for (i = 0; i < ND_MATRIX_DEAD_SCANS - 1; i++) {
        CHECK_INT(nd_matrix_input_poll(&in), ND_KEY_NONE);
        CHECK(!nd_matrix_input_bus_dead(&in));
    }
    chip_discard(&fc);

    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

static void test_a_bus_that_stops_answering_is_declared_dead(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    int i;

    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    chip_discard(&fc);

    for (i = 0; i < ND_MATRIX_DEAD_SCANS; i++)
        CHECK_INT(nd_matrix_input_poll(&in), ND_KEY_NONE);

    /* THE BUG THIS PINS. Before this, the answer here was "the keypad is
     * fine, nobody is typing" -- for ever, silently, with the reopen path
     * disabled because a matrix was still nominally open. */
    CHECK(nd_matrix_input_bus_dead(&in));

    /* And the reason has survived, so the core can say WHICH bus and WHAT
     * the kernel said instead of guessing between permission and wiring. */
    CHECK_STR(nd_matrix_input_dev(&in), "/dev/i2c-3");
    CHECK_INT(nd_matrix_input_last_stage(&in), ND_PCF_STAGE_READ);
    CHECK(nd_matrix_input_last_errno(&in) != 0);

    chip_discard(&fc);
    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

static void test_one_good_scan_clears_the_streak(void)
{
    fake_chip fc;
    nd_keymap km;
    nd_matrix_input in;
    int i;

    fill_keymap(&km);
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fc.driver_fd), ND_OK);
    chip_discard(&fc);

    /* CONSECUTIVE is the word that matters. A phone that accumulated failures
     * over an hour of perfectly good use would eventually tear its own keypad
     * down for no reason, which is the failure mode a naive counter has. */
    for (i = 0; i < ND_MATRIX_DEAD_SCANS - 1; i++)
        (void)nd_matrix_input_poll(&in);
    CHECK(!nd_matrix_input_bus_dead(&in));

    chip_discard(&fc);
    chip_reply_quiet_pass(&fc);
    CHECK_INT(nd_matrix_input_poll(&in), ND_KEY_NONE);
    chip_discard(&fc);

    for (i = 0; i < ND_MATRIX_DEAD_SCANS - 1; i++)
        (void)nd_matrix_input_poll(&in);
    CHECK(!nd_matrix_input_bus_dead(&in));

    (void)nd_matrix_input_poll(&in);
    CHECK(nd_matrix_input_bus_dead(&in));

    chip_discard(&fc);
    nd_matrix_input_close(&in);
    fake_chip_close(&fc);
}

static void test_an_open_that_failed_still_says_why(void)
{
    nd_keymap km;
    nd_matrix_input in;
    int fd;

    /* The whole point of keeping the errno past a FAILED open: the caller has
     * no chip left to ask and an nd_err that says only "IO". A descriptor
     * that refuses writes stands in for an expander that does not answer. */
    fill_keymap(&km);
    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fd), ND_ERR_IO);
    CHECK_INT(nd_matrix_input_last_stage(&in), ND_PCF_STAGE_WRITE);
    CHECK(nd_matrix_input_last_errno(&in) != 0);
    CHECK_STR(nd_matrix_input_dev(&in), "/dev/i2c-3");
    (void)close(fd);
}

static void test_a_keymap_that_is_not_a_matrix_is_refused(void)
{
    nd_keymap km;
    nd_matrix_input in;
    int fds[2];

    /* Not a bus problem and never will be one: a file naming pin 4 as both a
     * row and a column describes wiring that cannot exist. It has to be
     * rejected the same way every time, so that nd_input can classify it as
     * permanent and stop retrying it. */
    fill_keymap(&km);
    km.col_pins[0] = 0; /* already a row pin */
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK_INT(nd_matrix_input_open_fd(&in, &km, fds[0]), ND_ERR_INVAL);
    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_the_adopted_scanner_reports_its_real_device(void)
{
    fake_chip fc;
    nd_matrix_scanner s;

    /* nd_matrix_scanner_init_fd() calls its chip "<attached>", which is right
     * for a test and useless in a log from a phone. The adopting form carries
     * the bus and the address through so the serial console names the node a
     * person would go and look at. */
    fake_chip_open(&fc);
    CHECK_INT(nd_matrix_scanner_adopt(&s, ROWS, 4u, COLS, 4u, fc.driver_fd, 3, 0x21), ND_OK);
    CHECK_STR(s.chip.dev_path, "/dev/i2c-3");
    CHECK_INT(s.chip.addr, 0x21);
    CHECK(!s.chip.owns_fd);

    nd_matrix_scanner_close(&s);
    /* Closing the scanner must not close a descriptor it does not own: on the
     * phone that descriptor is the one root opened before the drop, and there
     * is no way to open another. */
    CHECK(fcntl(fc.driver_fd, F_GETFD) >= 0);
    fake_chip_close(&fc);
}

int main(void)
{
    RUN(test_a_glitching_bus_is_not_a_dead_one);
    RUN(test_a_bus_that_stops_answering_is_declared_dead);
    RUN(test_one_good_scan_clears_the_streak);
    RUN(test_an_open_that_failed_still_says_why);
    RUN(test_a_keymap_that_is_not_a_matrix_is_refused);
    RUN(test_the_adopted_scanner_reports_its_real_device);
    return pt_report("test_matrix");
}
