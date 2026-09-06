/* test_pcf8575.c -- what the expander driver REMEMBERS about a failure, and
 * the two descriptor handovers the keypad's boot depends on.
 *
 * test_keypad.c already covers the happy path of this driver: the byte order
 * of a write, the reassembly of a read, and the release word on close. This
 * file is about the other half, which had no test at all and which the
 * hardware turned out to depend on completely.
 *
 * ============ WHY A FAILURE HAS TO BE REMEMBERED ============
 *
 * An i2c keypad that does not work fails in one of four places -- open, the
 * I2C_SLAVE ioctl, a write or a read -- and the repair is different for every
 * one of them. Until 0.5.8b all four reached the owner as one sentence, "the
 * keypad on /dev/i2c-3 did not open (a permission or wiring problem)", which
 * names two of the four; and nd_pcf8575_write16(), the transfer that carries
 * the very first byte to ever reach the wires, returned its failure with no
 * log line at all. On the owner's phone that turned "the expander's rail was
 * still rising" into "your keypad is broken", on some boots and not others.
 *
 * So the driver now records the errno and the stage, and the layers above it
 * read them back out. These cases are the contract that they can.
 *
 * ============ WHY ADOPT AND DETACH EXIST ============
 *
 * nd-core opens the keypad's bus while it is still root and hands the
 * descriptor across the privilege drop, because open(2) checks permission
 * once and the I2C_SLAVE address is per-descriptor state. adopt() is the far
 * end of that handover and detach() is the near end, and both have exactly
 * one rule that matters: NEITHER MAY CLOSE THE DESCRIPTOR. A descriptor that
 * is closed on the way past cannot be reopened afterwards -- the process is
 * not root any more -- and the phone loses its keypad for the rest of the
 * boot with no way back.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_keypad.h"
#include "nd_paths.h"

#include "platform_test.h"

/* A descriptor that is open and readable and will refuse every write, with a
 * deterministic errno on every libc. Nothing about the driver cares that it
 * is not a bus. */
static int write_only_fails_fd(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);

    CHECK(fd >= 0);
    return fd;
}

static void test_stage_names_are_the_words_a_person_reads(void)
{
    CHECK_STR(nd_pcf8575_stage_name(ND_PCF_STAGE_OPEN), "open");
    CHECK_STR(nd_pcf8575_stage_name(ND_PCF_STAGE_SLAVE), "I2C_SLAVE");
    CHECK_STR(nd_pcf8575_stage_name(ND_PCF_STAGE_WRITE), "write");
    CHECK_STR(nd_pcf8575_stage_name(ND_PCF_STAGE_READ), "read");
    /* Never NULL, so a caller can put it straight into a format string. */
    CHECK_STR(nd_pcf8575_stage_name(ND_PCF_STAGE_NONE), "");
}

static void test_a_failed_write_records_where_and_why(void)
{
    nd_pcf8575 c;
    int fd = write_only_fails_fd();

    CHECK_INT(nd_pcf8575_attach(&c, fd), ND_OK);
    CHECK_INT(c.last_stage, ND_PCF_STAGE_NONE);
    CHECK_INT(c.last_errno, 0);

    /* THE BUG THIS PINS: this used to `return ND_ERR_IO;` and nothing else.
     * The errno was the only thing that could have told the owner whether the
     * keypad was unplugged or the udev rule had not landed, and it was
     * dropped on the floor. */
    CHECK_INT(nd_pcf8575_write16(&c, 0xFFFFu), ND_ERR_IO);
    CHECK_INT(c.last_stage, ND_PCF_STAGE_WRITE);
    CHECK_INT(c.last_errno, EBADF);

    (void)close(fd);
}

static void test_a_failed_read_records_where_and_why(void)
{
    nd_pcf8575 c;
    int fds[2];
    uint16_t v = 0u;

    /* A non-blocking socketpair with nothing seeded: the read finds no bytes,
     * which is exactly the shape of a chip that did not answer. */
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK_INT(nd_pcf8575_attach(&c, fds[0]), ND_OK);

    CHECK_INT(nd_pcf8575_read16(&c, &v), ND_ERR_IO);
    CHECK_INT(c.last_stage, ND_PCF_STAGE_READ);
    CHECK_INT(c.last_errno, EAGAIN);

    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_a_transfer_that_works_re_arms_the_log(void)
{
    nd_pcf8575 c;
    int fds[2];
    uint8_t seed[2] = {0x34u, 0x12u};
    uint16_t v = 0u;

    /* The log line is one per failure BURST -- a dead bus fails every
     * transfer of every scan two hundred times a second, and a line each
     * would bury the first one, which is the only line that says when the bus
     * went. The flag that suppresses the repeats has to be cleared by a
     * transfer that works, or a bus that dies TWICE only says so once. */
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK_INT(nd_pcf8575_attach(&c, fds[0]), ND_OK);

    CHECK_INT(nd_pcf8575_read16(&c, &v), ND_ERR_IO);
    CHECK(c.io_error_logged);

    CHECK_INT(write(fds[1], seed, sizeof seed), 2);
    CHECK_INT(nd_pcf8575_read16(&c, &v), ND_OK);
    CHECK_INT(v, 0x1234);
    CHECK(!c.io_error_logged);

    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_an_adopted_descriptor_knows_which_bus_it_is(void)
{
    nd_pcf8575 c;
    int fds[2];

    /* nd_pcf8575_attach() calls itself "<attached>" because a test's
     * socketpair has no bus number. The root-phase handover does, and a log
     * line reading "<attached>" instead of "/dev/i2c-3" is a line nobody can
     * act on. */
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK_INT(nd_pcf8575_adopt(&c, fds[0], 3, 0x21), ND_OK);
    CHECK_STR(c.dev_path, "/dev/i2c-3");
    CHECK_INT(c.bus, 3);
    CHECK_INT(c.addr, 0x21);
    CHECK(!c.owns_fd);

    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_closing_an_adopted_chip_leaves_the_descriptor_open(void)
{
    nd_pcf8575 c;
    int fds[2];

    /* THE RULE. The descriptor belongs to the root phase of the boot and has
     * to outlive every nd_pcf8575 built over it: closing it would need root
     * to reopen, and by then there is none. */
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK_INT(nd_pcf8575_adopt(&c, fds[0], 3, 0x20), ND_OK);
    nd_pcf8575_close(&c);
    CHECK_INT(c.fd, -1);
    CHECK(fcntl(fds[0], F_GETFD) >= 0);

    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_detach_hands_the_descriptor_over_and_says_nothing_on_the_bus(void)
{
    nd_pcf8575 c;
    int fds[2];
    uint8_t buf[8];
    ssize_t got;
    int handed;

    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK(fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK) == 0);
    CHECK_INT(nd_pcf8575_adopt(&c, fds[0], 3, 0x20), ND_OK);
    CHECK_INT(nd_pcf8575_write16(&c, 0x1234u), ND_OK);

    handed = nd_pcf8575_detach(&c);
    CHECK_INT(handed, fds[0]);
    CHECK_INT(c.fd, -1);
    CHECK(fcntl(fds[0], F_GETFD) >= 0);

    /* Exactly the two bytes the test wrote. detach() must NOT send the 0xFFFF
     * release word that close() sends: the caller is taking the descriptor on
     * precisely because it wants the bus left as it is, and an unexpected
     * write is a row released under a finger that is still on it. */
    got = read(fds[1], buf, sizeof buf);
    CHECK_INT(got, 2);
    CHECK_INT(buf[0], 0x34);
    CHECK_INT(buf[1], 0x12);

    (void)close(fds[0]);
    (void)close(fds[1]);
}

static void test_detach_of_nothing_is_minus_one(void)
{
    nd_pcf8575 c;

    memset(&c, 0, sizeof c);
    c.fd = -1;
    CHECK_INT(nd_pcf8575_detach(&c), -1);
    CHECK_INT(nd_pcf8575_detach(NULL), -1);
}

int main(void)
{
    RUN(test_stage_names_are_the_words_a_person_reads);
    RUN(test_a_failed_write_records_where_and_why);
    RUN(test_a_failed_read_records_where_and_why);
    RUN(test_a_transfer_that_works_re_arms_the_log);
    RUN(test_an_adopted_descriptor_knows_which_bus_it_is);
    RUN(test_closing_an_adopted_chip_leaves_the_descriptor_open);
    RUN(test_detach_hands_the_descriptor_over_and_says_nothing_on_the_bus);
    RUN(test_detach_of_nothing_is_minus_one);
    return pt_report("test_pcf8575");
}
