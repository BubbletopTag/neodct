/* nd_matrix.c -- the edge-detecting keypad matrix scanner and the keymapped
 * input backend on top of it.
 *
 * Ported from System/hw/pcf8575_keypad.py, classes I2CMatrixScanner and
 * I2CMatrixKeypadInput.
 *
 * ============ WHY THE WHOLE MATRIX IS SCANNED EVERY PASS ============
 *
 * Stopping at the first hit would be faster and would break key rollover:
 * pressing a second key while one is still held must be seen, or a game
 * misses every direction change made without letting go first. So every row
 * is driven and read on every pass, and the result is a SET of positions.
 *
 * ============ WHY RELEASES ARE DEBOUNCED AND PRESSES ARE NOT ============
 *
 * A membrane contact chatters on the way up, not on the way down. A key
 * therefore counts as released only after ND_RELEASE_SCANS consecutive scans
 * without it -- about 15 ms at the 5 ms poll cadence -- while a press is
 * reported the instant it appears.
 *
 * ============ THE SINGLE-KEY LIMITATION, PRECISELY ============
 *
 *   1. scan_once() reports PRESS EDGES ONLY. There is no release event on
 *      this path; nd_input derives one by watching nd_matrix_held().
 *   2. It returns ONE position per call. Simultaneous presses are queued and
 *      drip out one per call, so a caller polling at 30 Hz sees them spread
 *      over several frames.
 *   3. The physical matrix has no diodes, so three keys in an L shape produce
 *      a fourth phantom press. Nothing in software compensates for that and
 *      nothing should start to.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_keypad.h"
#include "nd_log.h"

#define HELD_NONE (-1)

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_us(long usec)
{
    struct timespec req;

    if (usec <= 0)
        return;
    req.tv_sec = usec / 1000000L;
    req.tv_nsec = (usec % 1000000L) * 1000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ; /* a signal must not shorten a settle time */
}

static nd_err validate_pins(const uint8_t *row_pins, size_t n_rows, const uint8_t *col_pins,
                            size_t n_cols)
{
    bool seen[ND_MATRIX_MAX_PINS];
    size_t i;

    if (row_pins == NULL || col_pins == NULL)
        return ND_ERR_INVAL;
    if (n_rows == 0u || n_cols == 0u)
        return ND_ERR_INVAL;
    if (n_rows > ND_MATRIX_MAX_PINS || n_cols > ND_MATRIX_MAX_PINS)
        return ND_ERR_INVAL;

    memset(seen, 0, sizeof seen);
    for (i = 0u; i < n_rows + n_cols; i++) {
        uint8_t pin = (i < n_rows) ? row_pins[i] : col_pins[i - n_rows];

        if (pin >= ND_MATRIX_MAX_PINS) {
            nd_log_err(ND_LOG_INPUT, "expander pin %u out of range 0-15", (unsigned)pin);
            return ND_ERR_INVAL;
        }
        if (seen[pin]) {
            nd_log_err(ND_LOG_INPUT, "expander pin %u listed twice", (unsigned)pin);
            return ND_ERR_INVAL;
        }
        seen[pin] = true;
    }
    return ND_OK;
}

/* How many times construction's release word is offered to a chip that does
 * not answer, and how long it waits between offers. Half a second in total.
 *
 * ============ WHY THE FIRST WRITE GETS RETRIES AND A SCAN DOES NOT ============
 *
 * This write is a PROBE, not a scan. It is the first transaction of the boot
 * and it happens while the expander's supply rail may still be rising, while
 * the rk3x controller may still be losing arbitration to the fuel gauge being
 * read on the same bus a few milliseconds earlier, and while the i2c core may
 * still be returning -EAGAIN from a bus it has only just registered. Every
 * one of those clears by itself in tens of milliseconds -- and every one of
 * them used to be a permanent "the keypad did not open" for the whole boot,
 * because this single two-byte transfer was the entire verdict.
 *
 * A scan is different: by then the bus has answered at least once, so a
 * failure means something changed, and absorbing it silently is what hid a
 * dead bus for a whole session. That one is counted, not retried -- see
 * ND_MATRIX_DEAD_SCANS. */
#define PROBE_TRIES    10
#define PROBE_RETRY_US 50000L

static nd_err scanner_finish_init(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                                  const uint8_t *col_pins, size_t n_cols)
{
    nd_err rc = ND_ERR_IO;
    int try_no;

    memcpy(s->row_pins, row_pins, n_rows);
    memcpy(s->col_pins, col_pins, n_cols);
    s->n_rows = n_rows;
    s->n_cols = n_cols;
    memset(s->held, HELD_NONE, sizeof s->held);
    s->pending_head = 0u;
    s->pending_len = 0u;

    /* Construction releases every pin, so a restart mid-scan cannot leave a
     * row driven low against a pressed key. */
    for (try_no = 0; try_no < PROBE_TRIES; try_no++) {
        if (try_no > 0)
            sleep_us(PROBE_RETRY_US);
        rc = nd_pcf8575_write16(&s->chip, 0xFFFFu);
        if (rc == ND_OK) {
            if (try_no > 0)
                nd_log(ND_LOG_INPUT, "expander at 0x%02X answered on attempt %d",
                       (unsigned)s->chip.addr, try_no + 1);
            return ND_OK;
        }
        /* A descriptor that is not a bus at all -- a closed chip, a test's
         * broken pipe -- will never answer, and ten attempts spread over half
         * a second would only delay the truth. Only the errnos a cold bus
         * actually produces are worth waiting on. */
        if (!nd_input_errno_is_transient(s->chip.last_errno))
            break;
    }
    return rc;
}

nd_err nd_matrix_scanner_init(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                              const uint8_t *col_pins, size_t n_cols, int bus, int addr)
{
    nd_err rc;

    if (s == NULL)
        return ND_ERR_INVAL;
    rc = validate_pins(row_pins, n_rows, col_pins, n_cols);
    if (rc != ND_OK)
        return rc;

    memset(s, 0, sizeof *s);
    rc = nd_pcf8575_open(&s->chip, bus, addr);
    if (rc != ND_OK)
        return rc;

    rc = scanner_finish_init(s, row_pins, n_rows, col_pins, n_cols);
    if (rc != ND_OK)
        nd_pcf8575_close(&s->chip);
    return rc;
}

nd_err nd_matrix_scanner_init_fd(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                                 const uint8_t *col_pins, size_t n_cols, int fd)
{
    nd_err rc;

    if (s == NULL)
        return ND_ERR_INVAL;
    rc = validate_pins(row_pins, n_rows, col_pins, n_cols);
    if (rc != ND_OK)
        return rc;

    memset(s, 0, sizeof *s);
    rc = nd_pcf8575_attach(&s->chip, fd);
    if (rc != ND_OK)
        return rc;

    return scanner_finish_init(s, row_pins, n_rows, col_pins, n_cols);
}

nd_err nd_matrix_scanner_adopt(nd_matrix_scanner *s, const uint8_t *row_pins, size_t n_rows,
                               const uint8_t *col_pins, size_t n_cols, int fd, int bus, int addr)
{
    nd_err rc;

    if (s == NULL)
        return ND_ERR_INVAL;
    rc = validate_pins(row_pins, n_rows, col_pins, n_cols);
    if (rc != ND_OK)
        return rc;

    memset(s, 0, sizeof *s);
    rc = nd_pcf8575_adopt(&s->chip, fd, bus, addr);
    if (rc != ND_OK)
        return rc;

    return scanner_finish_init(s, row_pins, n_rows, col_pins, n_cols);
}

void nd_matrix_scanner_close(nd_matrix_scanner *s)
{
    if (s == NULL)
        return;
    nd_pcf8575_close(&s->chip);
}

/* _raw_scan(): one full pass, into a [row][col] boolean grid. */
static nd_err raw_scan(nd_matrix_scanner *s, bool current[ND_MATRIX_MAX_PINS][ND_MATRIX_MAX_PINS])
{
    size_t row;
    nd_err rc;

    memset(current, 0, sizeof(bool) * ND_MATRIX_MAX_PINS * ND_MATRIX_MAX_PINS);

    for (row = 0u; row < s->n_rows; row++) {
        uint16_t value = 0u;
        size_t col;

        rc = nd_pcf8575_write16(&s->chip, (uint16_t)(0xFFFFu & ~(1u << s->row_pins[row])));
        if (rc != ND_OK)
            return rc;

        /* The i2c transactions themselves take ~0.5 ms at 100 kHz; the settle
         * guards against line capacitance on a long ribbon. */
        sleep_us(ND_SCAN_SETTLE_US);

        rc = nd_pcf8575_read16(&s->chip, &value);
        if (rc != ND_OK)
            return rc;

        for (col = 0u; col < s->n_cols; col++) {
            if (((((uint32_t)value) >> s->col_pins[col]) & 1u) == 0u)
                current[row][col] = true;
        }
    }

    return nd_pcf8575_write16(&s->chip, 0xFFFFu);
}

nd_err nd_matrix_scan_once(nd_matrix_scanner *s, nd_matrix_pos *out, bool *found)
{
    bool current[ND_MATRIX_MAX_PINS][ND_MATRIX_MAX_PINS];
    nd_matrix_pos new_presses[ND_MATRIX_MAX_PINS * ND_MATRIX_MAX_PINS];
    size_t n_new = 0u;
    size_t row;
    size_t col;
    nd_err rc;

    if (s == NULL || out == NULL || found == NULL)
        return ND_ERR_INVAL;
    *found = false;

    rc = raw_scan(s, current);
    if (rc != ND_OK)
        return rc;

    /* Row-major iteration is already ascending by (row, col), which is the
     * order the Python's sort() produces -- so there is nothing to sort. */
    for (row = 0u; row < s->n_rows; row++) {
        for (col = 0u; col < s->n_cols; col++) {
            if (current[row][col] && s->held[row][col] == HELD_NONE) {
                new_presses[n_new].row = (uint8_t)row;
                new_presses[n_new].col = (uint8_t)col;
                n_new++;
            }
        }
    }

    for (row = 0u; row < ND_MATRIX_MAX_PINS; row++) {
        for (col = 0u; col < ND_MATRIX_MAX_PINS; col++) {
            if (current[row][col]) {
                s->held[row][col] = 0;
            } else if (s->held[row][col] != HELD_NONE) {
                s->held[row][col] = (int8_t)(s->held[row][col] + 1);
                if (s->held[row][col] >= (int8_t)ND_RELEASE_SCANS)
                    s->held[row][col] = HELD_NONE;
            }
        }
    }

    if (n_new > 0u) {
        size_t i;

        /* Everything after the first waits its turn: one position per call is
         * the contract every caller of this is written against. */
        for (i = 1u; i < n_new; i++) {
            if (s->pending_len < ND_ARRAY_LEN(s->pending)) {
                size_t slot = (s->pending_head + s->pending_len) % ND_ARRAY_LEN(s->pending);

                s->pending[slot] = new_presses[i];
                s->pending_len++;
            }
        }
        *out = new_presses[0];
        *found = true;
        return ND_OK;
    }

    if (s->pending_len > 0u) {
        *out = s->pending[s->pending_head];
        s->pending_head = (s->pending_head + 1u) % ND_ARRAY_LEN(s->pending);
        s->pending_len--;
        *found = true;
    }
    return ND_OK;
}

size_t nd_matrix_held(const nd_matrix_scanner *s, nd_matrix_pos *out, size_t max)
{
    size_t n = 0u;
    size_t row;
    size_t col;

    if (s == NULL)
        return 0u;
    for (row = 0u; row < s->n_rows; row++) {
        for (col = 0u; col < s->n_cols; col++) {
            if (s->held[row][col] == HELD_NONE)
                continue;
            if (out != NULL && n < max) {
                out[n].row = (uint8_t)row;
                out[n].col = (uint8_t)col;
            }
            n++;
        }
    }
    return n;
}

bool nd_matrix_is_held(const nd_matrix_scanner *s, uint8_t row, uint8_t col)
{
    if (s == NULL || row >= ND_MATRIX_MAX_PINS || col >= ND_MATRIX_MAX_PINS)
        return false;
    return s->held[row][col] != HELD_NONE;
}

/* ------------------------------------------------------------------ *
 * I2CMatrixKeypadInput
 * ------------------------------------------------------------------ */

static nd_err matrix_input_finish(nd_matrix_input *in, const nd_keymap *cfg)
{
    in->cfg = *cfg;
    in->have_last_unmapped = false;
    return ND_OK;
}

nd_err nd_matrix_input_open(nd_matrix_input *in, const nd_keymap *cfg)
{
    nd_err rc;

    if (in == NULL || cfg == NULL)
        return ND_ERR_INVAL;

    memset(in, 0, sizeof *in);
    rc = nd_matrix_scanner_init(&in->scanner, cfg->row_pins, cfg->n_rows, cfg->col_pins,
                                cfg->n_cols, cfg->i2c_bus, cfg->i2c_addr);
    if (rc != ND_OK)
        return rc;
    return matrix_input_finish(in, cfg);
}

nd_err nd_matrix_input_open_fd(nd_matrix_input *in, const nd_keymap *cfg, int fd)
{
    nd_err rc;

    if (in == NULL || cfg == NULL)
        return ND_ERR_INVAL;

    memset(in, 0, sizeof *in);
    /* adopt() rather than init_fd(): the keymap already says which bus and
     * which address this descriptor is, and a log line that names
     * "/dev/i2c-3 (0x20)" instead of "<attached>" is the difference between a
     * serial log somebody can act on and one they cannot. The two differ in
     * nothing else. */
    rc = nd_matrix_scanner_adopt(&in->scanner, cfg->row_pins, cfg->n_rows, cfg->col_pins,
                                 cfg->n_cols, fd, cfg->i2c_bus, cfg->i2c_addr);
    if (rc != ND_OK)
        return rc;
    return matrix_input_finish(in, cfg);
}

bool nd_matrix_input_bus_dead(const nd_matrix_input *in)
{
    return in != NULL && in->scan_errors >= (uint32_t)ND_MATRIX_DEAD_SCANS;
}

int nd_matrix_input_last_errno(const nd_matrix_input *in)
{
    return (in != NULL) ? in->scanner.chip.last_errno : 0;
}

nd_pcf8575_stage nd_matrix_input_last_stage(const nd_matrix_input *in)
{
    return (in != NULL) ? in->scanner.chip.last_stage : ND_PCF_STAGE_NONE;
}

const char *nd_matrix_input_dev(const nd_matrix_input *in)
{
    return (in != NULL) ? in->scanner.chip.dev_path : "";
}

void nd_matrix_input_close(nd_matrix_input *in)
{
    if (in == NULL)
        return;
    nd_matrix_scanner_close(&in->scanner);
}

/* One scan, mapped. ND_KEY_NONE when nothing mapped came out of it; exposed
 * to nd_input.c through nd_matrix_input_read_key(0) semantics but factored so
 * the caller can drive its own timing. */
static int32_t matrix_poll(nd_matrix_input *in)
{
    nd_matrix_pos pos;
    bool found = false;
    int32_t code;

    if (nd_matrix_scan_once(&in->scanner, &pos, &found) != ND_OK) {
        /* ============ A SCAN ERROR IS NOT "NO KEY" ============
         *
         * It used to be: this branch returned ND_KEY_NONE and threw the error
         * away. A bus that died after opening -- a loose ribbon, an expander
         * browning out, the i2c controller wedging -- therefore looked
         * exactly like a phone nobody was typing on. have_matrix stayed true,
         * so nd_input's reopen path refused to run (it only fires when there
         * is NO matrix), nd_input_has_backend() went on answering yes, and
         * the core never drew a thing. The phone was dead to every key and
         * said nothing about it, on the screen or in the log, for the rest of
         * the session.
         *
         * So the failures are counted here, and the count is what the caller
         * asks about. nd_pcf8575 has already logged the first one of the
         * burst with the errno; there is nothing to add per scan. */
        if (in->scan_errors < (uint32_t)ND_MATRIX_DEAD_SCANS)
            in->scan_errors++;
        return ND_KEY_NONE;
    }
    in->scan_errors = 0u;
    if (!found)
        return ND_KEY_NONE;

    code = in->cfg.matrix_to_code[pos.row][pos.col];
    if (code >= 0) {
        in->have_last_unmapped = false;
        return code;
    }

    /* Rate-limited to one line per DISTINCT position, and reset by the next
     * key that does map -- otherwise an unenrolled key floods the console at
     * 200 lines a second. */
    if (!in->have_last_unmapped || in->last_unmapped.row != pos.row ||
        in->last_unmapped.col != pos.col) {
        in->have_last_unmapped = true;
        in->last_unmapped = pos;
        nd_log(ND_LOG_INPUT, "I2C matrix key (%u, %u) has no mapping in %s", (unsigned)pos.row,
               (unsigned)pos.col, in->cfg.path);
    }
    return ND_KEY_NONE;
}

int32_t nd_matrix_input_read_key(nd_matrix_input *in, double timeout_s)
{
    double deadline;

    if (in == NULL)
        return ND_KEY_NONE;
    if (timeout_s < 0.0)
        timeout_s = 0.0;
    deadline = monotonic_now() + timeout_s;

    /* At least one scan even at timeout 0: the Browser's drain path calls
     * read_key(0) precisely to consume a press the scanner already queued. */
    for (;;) {
        int32_t code = matrix_poll(in);

        if (code != ND_KEY_NONE)
            return code;
        if (monotonic_now() >= deadline)
            return ND_KEY_NONE;
        sleep_us(ND_READ_POLL_US);
    }
}

/* nd_input.c needs a single non-blocking scan; giving it the static above
 * would mean a second copy of the unmapped-key rate limiter. */
int32_t nd_matrix_input_poll(nd_matrix_input *in)
{
    return (in != NULL) ? matrix_poll(in) : ND_KEY_NONE;
}
