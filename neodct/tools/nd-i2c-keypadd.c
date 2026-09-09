/* nd-i2c-keypadd -- the emulator's PCF8575 keypad, on the HOST side of a real
 * i2c bus.
 *
 * It is a vhost-user backend. QEMU's `vhost-user-i2c-device` is a virtio-i2c
 * adapter whose transfers are serviced by a process outside QEMU over a unix
 * socket; the guest kernel binds i2c-virtio to it, registers a real i2c
 * adapter, devtmpfs makes a real /dev/i2c-N, and the phone's own
 * nd_pcf8575.c does a real ioctl(I2C_SLAVE) and a real two-byte write and
 * read against it. Nothing in the guest is a stub.
 *
 * ============ WHY THE HOST AND NOT A DAEMON IN THE GUEST ============
 *
 * A guest process that read /dev/input/event0 and pushed port bits into a
 * mock adapter would be QEMU-ONLY CODE INSIDE THE ARTEFACT THE PARITY HARNESS
 * EXISTS TO KEEP HONEST -- the same object, in the same place, that the
 * storage stage refused for ndflash and put in a separate overlay cpio. It
 * would also cost eight inventory records for ever: measured, a machine with
 * no virtio keyboard reports `class.input []` and has no class.input.*.name,
 * no dev.input, no dev.input/event0 and none of the three input.* records --
 * which is what nd_input.c says the phone looks like ("On a Luckfox the
 * keypad is the i2c matrix and /dev/input is empty"). And the keys would
 * enter on the opposite side from where the picture leaves: vfb has no
 * scanout, so the panel already comes out as a byte stream that
 * st7789_replay.py decodes on the host.
 *
 * ============ WHY NOT i2c-stub OR i2c-gpio, WHICH ARE FREE ============
 *
 * Both were built and booted before this was written, and both are dead ends.
 *
 * CONFIG_I2C_STUB gives an adapter whose open() and ioctl(I2C_SLAVE) succeed
 * and whose first wire transaction does not: measured funcs=0x0c7f0000 with no
 * I2C_FUNC_I2C bit, because i2c-stub.c's algorithm declares .smbus_xfer and
 * never .master_xfer, and __i2c_transfer() returns -EOPNOTSUPP when that
 * member is NULL. The repository's own nd_pcf8575_write16() got errno=95 at
 * stage=write. It is also `depends on m`, so it could only reach the phone's
 * image as a .ko inside a read-only verity-covered squashfs.
 *
 * CONFIG_I2C_GPIO on -M virt's pl061 does give a real adapter with
 * I2C_FUNC_I2C, and nothing can ever answer on it: CONFIG_I2C_SLAVE is off,
 * i2c-algo-bit has no reg_slave so no bitbanged adapter can hold a slave
 * whatever the config says, and QEMU's pl061 lines are connected to nothing.
 * Measured errno=6 (ENXIO) for ever. That ENXIO is genuinely valuable -- see
 * THE ERRNO GAP below -- and it is not a keypad.
 *
 * ============ THE CHIP MODEL IS ONE ELECTRICAL RULE ============
 *
 * Not a lookup table from a driven row to a set of column bits. The PCF8575
 * is quasi-bidirectional with no direction register and no command byte: a
 * pin written 1 is released to a weak pull-up and reads high, a pin written 0
 * is driven hard low, and a pressed key is a switch shorting two pins.
 * Therefore
 *
 *     read16() = 0xFFFF & ~mask(every pin in a connected component that
 *                               contains at least one pin driven low)
 *
 * over the graph whose edges are the currently pressed keys. Fifteen lines of
 * union-find, and three behaviours fall out of it rather than being coded:
 *
 *   - with nothing pressed, a read during a scan returns 0xFFFF with ONLY the
 *     driven row bit low. Not a flat 0xFFFF, which is what a model that
 *     starts from "clear the column bits" returns and is a lie about the chip;
 *   - after the closing write16(0xFFFF) every pin reads high EVEN WITH A KEY
 *     HELD, which is why nd_pcf8575_close() writes it;
 *   - ghosting is free. Three keys in an L short a fourth into the same
 *     component and the fourth reads pressed. spec-hw-input.md and nd_matrix.c
 *     both say the matrix has no diodes and that nothing in software
 *     compensates; nd_kpsetup_wait_new_pair()'s documented "a phantom third
 *     pair from a ghosting three-key press is ignored the same way" has never
 *     executed anywhere.
 *
 * The one rule is also what lets the FIRST-BOOT WIZARD run. The shipping
 * scanner drives row pins only; nd_kpsetup_scan_pairs() drives EACH OF THE
 * SIXTEEN PINS IN TURN and records every other pin that came back low,
 * because it cannot know which pins are rows -- that is the entire point of
 * it. A "row -> columns" model cannot run the wizard at all.
 *
 * And it is deliberately a SECOND IMPLEMENTATION, written from the chip's
 * electrical behaviour rather than from what nd_matrix.c expects to see, for
 * the reason st7789_replay.py's decoder is written from the ST7789 datasheet:
 * a model derived from the scanner can only ever agree with it.
 *
 * ============ THE ERRNO GAP, WHICH THIS DOES NOT CLOSE ============
 *
 * virtio-i2c's status byte is OK-or-ERR with no error code.
 * virtio_i2c_complete_reqs() returns a COUNT of successful messages,
 * i2c_master_send() turns a zero count into a zero-length transfer,
 * i2cdev_write() returns 0 and never touches errno. So an address nobody
 * answers reaches the phone's own code as `short write on /dev/i2c-3 (got 0
 * of 2 bytes)` with errno 0, where a real controller gives ENXIO or
 * EREMOTEIO. Measured, with the repository's own binary, against both
 * transports.
 *
 * The consequence points the wrong way and is written down rather than
 * papered over: nd_input_errno_is_transient(0) is false BY DESIGN ("a failure
 * with no errno behind it was not the kernel refusing us anything"), so the
 * emulator classifies an unanswered expander PERMANENT and never retries,
 * while the phone gets ENXIO, classifies it TRANSIENT and runs the bounded
 * self-heal. Do NOT teach nd_pcf8575.c to synthesise an errno from a short
 * count: on the phone i2c_master_send() returns 2 or a negative errno and
 * never a short count, so a short-count-means-transient rule would be an
 * emulator-only branch encoded in shipping code. test_keypad.c pins the
 * divergence instead, and allow.txt records it.
 *
 * ============ THE FUEL GAUGE ANSWERS, AND THAT IS NOT A BONUS ============
 *
 * The phone's MAX17048 shares this bus at 0x36, and nd_battery.c opens it a
 * few lines BEFORE the keypad in nd_ui_init(). Today under QEMU there is no
 * node at all, so ENOENT, so nd_battery_errno_means_absent() is true and the
 * meter simulates. The moment /dev/i2c-3 exists that stops being true, and
 * nd_battery.c is explicit about what happens next: "Past open() the node
 * demonstrably exists, so every remaining failure is UNREADABLE by
 * construction -- there is no errno here that can mean 'no gauge in this
 * phone'." So a model that NAKs 0x36 does not leave the emulator simulating;
 * it moves it from SIM to UNREADABLE -- a phone that says it cannot read its
 * own battery. Answering with a modelled gauge is the honest option of the
 * two, and it makes nd_battery.c's LIVE path reachable here for the first
 * time. The values are fixed and documented at ND_GAUGE_* below; they are a
 * model and not a measurement, and nothing should be tuned against them.
 *
 * Every other address NAKs, which is load-bearing in its own right:
 * nd_keypadsetup.c probes 0x20 through 0x27 and must find EXACTLY ONE chip.
 *
 * ============ AND IF THIS PROCESS DIES, THE GUEST DOES NOT ============
 *
 * It FREEZES. QEMU refuses to start against a socket that is not there, which
 * is why the callers start this first -- but that covers t=0 only. After the
 * connection, i2c-virtio waits in wait_for_completion_interruptible() with the
 * i2c bus lock held, and there is no timeout anywhere in that path: a segfault
 * or an OOM kill here leaves every later transfer outstanding for ever.
 * Measured on a booted guest -- kill -9 at scan pass 5, and pass 6 never
 * printed. On a real image that read is nd_input_read_key() ->
 * nd_matrix_scan_once(), i.e. the UI's own key loop, and nd_battery's poll
 * blocks behind the same lock, so the emulator stops dead with nothing on the
 * console. run_qemu.sh watches this pid and stops QEMU with a message rather
 * than leaving that silent; a caller that does not watch it inherits the
 * freeze.
 *
 * Build: cc -O2 -o nd-i2c-keypadd nd-i2c-keypadd.c   (host tool, no libraries)
 * `make -C neodct/src keypadd` builds it under the project's warning set.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * The chip, and the matrix wired to it
 * ------------------------------------------------------------------ */

#define ND_KEYPAD_ADDR 0x20 /* ND_I2C_ADDR_DEFAULT, and ND_KPSETUP_PROBE_FIRST */
#define ND_GAUGE_ADDR  0x36 /* the MAX17048, ND_BATT_DEFAULT_I2C_ADDR         */

#define ND_PINS 16
#define ND_ROWS 4
#define ND_COLS 4

/* The tree's own ND_ROW_PINS_DEFAULT / ND_COL_PINS_DEFAULT
 * (spec-hw-input.md:485-486). They are a parameter of the model and not a
 * constant of it, so that a scrambled layout is a test case rather than a
 * second layout nobody uses. */
static const uint8_t ND_ROW_PINS[ND_ROWS] = {0, 1, 2, 3};
static const uint8_t ND_COL_PINS[ND_COLS] = {4, 5, 6, 7};

/* Sixteen keys in nd_kpsetup_targets[] order, row-major. Enrolment order
 * rather than the Nokia face, because there is no physical face here and
 * enrolment order is the only ordering this tree defines: the wizard's
 * sixteen prompts then sweep the pad in the order somebody would press them.
 * nd_kpsetup_bipartition() colours the smallest pin of each component 0, so
 * pin 0 lands on the row side and an enrolled keymap comes out row_pins
 * [0,1,2,3] col_pins [4,5,6,7] -- the numbers the spec prints. */
static const char *const ND_KEY_NAMES[ND_ROWS][ND_COLS] = {
    {"navikey", "clear", "up", "down"},
    {"num_1", "num_2", "num_3", "num_4"},
    {"num_5", "num_6", "num_7", "num_8"},
    {"num_9", "num_0", "star", "hash"},
};

/* The gauge's answers. A MAX17048 at 3.850 V and 72 %, which is a plausible
 * mid-charge cell and is DELIBERATELY CONSTANT: a modelled gauge that drifted
 * would invite somebody to test a discharge curve against a number this file
 * made up. VCELL is 78.125 uV per LSB (ND_VCELL_LSB), SOC is %/256. */
#define ND_GAUGE_VERSION 0x0012u
#define ND_GAUGE_VCELL   0xC080u /* 49280 * 78.125 uV = 3.850 V */
#define ND_GAUGE_SOC     0x4800u /* 0x48 = 72 %                 */
#define ND_GAUGE_CRATE   0x0000u /* 0 %/hr: neither charging nor discharging */
#define ND_GAUGE_CONFIG  0x971Cu /* the part's own reset value  */

typedef struct {
    uint16_t latch; /* what the guest last wrote: 1 = released, 0 = driven low */
    /* Shorted pin pairs. A pressed key is one of these; `short a b` from the
     * key channel is another, which is how a test reaches a pin pair no key
     * occupies. */
    uint8_t edge_a[ND_PINS];
    uint8_t edge_b[ND_PINS];
    size_t n_edges;
    /* The gauge's register pointer, which a MAX17048 remembers between the
     * one-byte write and the two-byte read nd_battery.c's read16() does. */
    uint8_t gauge_reg;
} nd_chip;

static void chip_init(nd_chip *c)
{
    memset(c, 0, sizeof *c);
    c->latch = 0xFFFFu; /* power-on state: every pin released to its pull-up */
}

/* Union-find over the sixteen pins. Small enough that path compression is not
 * worth the lines; the whole thing runs once per read. */
static uint8_t uf_find(uint8_t *parent, uint8_t x)
{
    while (parent[x] != x)
        x = parent[x];
    return x;
}

/* THE RULE, and the only place the chip is modelled. */
static uint16_t chip_read16(const nd_chip *c)
{
    uint8_t parent[ND_PINS];
    bool low[ND_PINS];
    uint16_t value = 0xFFFFu;
    uint8_t pin;
    size_t i;

    for (pin = 0u; pin < ND_PINS; pin++) {
        parent[pin] = pin;
        low[pin] = false;
    }
    for (i = 0u; i < c->n_edges; i++) {
        uint8_t ra = uf_find(parent, c->edge_a[i]);
        uint8_t rb = uf_find(parent, c->edge_b[i]);
        if (ra != rb)
            parent[ra] = rb;
    }
    /* A component is pulled down if ANY pin in it is being driven low. */
    for (pin = 0u; pin < ND_PINS; pin++) {
        if (((c->latch >> pin) & 1u) == 0u)
            low[uf_find(parent, pin)] = true;
    }
    for (pin = 0u; pin < ND_PINS; pin++) {
        if (low[uf_find(parent, pin)])
            value = (uint16_t)(value & (uint16_t)~(1u << pin));
    }
    return value;
}

static bool chip_has_edge(const nd_chip *c, uint8_t a, uint8_t b, size_t *at)
{
    size_t i;

    for (i = 0u; i < c->n_edges; i++) {
        if ((c->edge_a[i] == a && c->edge_b[i] == b) || (c->edge_a[i] == b && c->edge_b[i] == a)) {
            if (at != NULL)
                *at = i;
            return true;
        }
    }
    return false;
}

static void chip_press(nd_chip *c, uint8_t a, uint8_t b)
{
    if (chip_has_edge(c, a, b, NULL) || c->n_edges >= ND_PINS)
        return;
    c->edge_a[c->n_edges] = a;
    c->edge_b[c->n_edges] = b;
    c->n_edges++;
}

static void chip_release(nd_chip *c, uint8_t a, uint8_t b)
{
    size_t at;

    if (!chip_has_edge(c, a, b, &at))
        return;
    c->edge_a[at] = c->edge_a[c->n_edges - 1u];
    c->edge_b[at] = c->edge_b[c->n_edges - 1u];
    c->n_edges--;
}

/* ============ A TAP CARRIES THE PAIR IT PRESSED ============
 *
 * It used to be one `double release_at` and an expiry that did
 * `chip.n_edges = 0`, i.e. every tap let go of EVERY shorted pair. That made
 * the one behaviour this whole bus exists to exercise unreachable through the
 * documented verb: nd_matrix.c's header says the whole-matrix scan is there
 * because "pressing a second key while one is still held must be seen, or a
 * game misses every direction change made without letting go first", and
 * `press hash` followed by `tap num_5` silently released the hash 60 ms
 * later. It also erased a `short a b` -- the wizard's world -- and a second
 * tap overwrote the first one's deadline, so two overlapping taps collapsed
 * into one release.
 *
 * So the deadlines are an ARRAY, one entry per possible key, and `release
 * all` stays the only verb that clears everything. Sixteen is the whole pad;
 * a seventeenth pending tap cannot exist because a pair already pending is
 * re-armed rather than appended.
 *
 * ONE EDGE PER PIN PAIR AND NOT A COUNT, which is what the chip has: a tap of
 * a key that `press` is already holding releases it at the deadline. That is
 * the switch's own behaviour -- two contacts are shorted or they are not --
 * and a refcount here would be a model of the test harness rather than of the
 * keypad. */
typedef struct {
    uint8_t a;
    uint8_t b;
    double at; /* CLOCK_MONOTONIC deadline */
} nd_tap;

typedef struct {
    nd_tap t[ND_ROWS * ND_COLS];
    size_t n;
} nd_taps;

static void taps_cancel(nd_taps *ts, uint8_t a, uint8_t b)
{
    size_t i;

    for (i = 0u; i < ts->n; i++) {
        if ((ts->t[i].a == a && ts->t[i].b == b) || (ts->t[i].a == b && ts->t[i].b == a)) {
            ts->t[i] = ts->t[ts->n - 1u];
            ts->n--;
            return;
        }
    }
}

static void taps_arm(nd_taps *ts, uint8_t a, uint8_t b, double at)
{
    /* Re-tapping a key that is already pending EXTENDS it rather than
     * queueing a second deadline for the same pair, which is what a finger
     * does and what keeps the array bounded by the pad. */
    taps_cancel(ts, a, b);
    if (ts->n >= sizeof ts->t / sizeof ts->t[0])
        return;
    ts->t[ts->n].a = a;
    ts->t[ts->n].b = b;
    ts->t[ts->n].at = at;
    ts->n++;
}

/* Releases every tap whose deadline has passed, and NOTHING else. Returns the
 * number released so the caller can decide whether to say anything. */
static size_t taps_expire(nd_chip *c, nd_taps *ts, double now)
{
    size_t i = 0u;
    size_t released = 0u;

    while (i < ts->n) {
        if (now >= ts->t[i].at) {
            chip_release(c, ts->t[i].a, ts->t[i].b);
            ts->t[i] = ts->t[ts->n - 1u];
            ts->n--;
            released++;
        } else {
            i++;
        }
    }
    return released;
}

/* ------------------------------------------------------------------ *
 * Logging
 * ------------------------------------------------------------------ */

static bool g_verbose;
static FILE *g_log;

static void logline(const char *fmt, ...)
{
    va_list ap;
    FILE *out = (g_log != NULL) ? g_log : stderr;

    va_start(ap, fmt);
    (void)fputs("nd-i2c-keypadd: ", out);
    (void)vfprintf(out, fmt, ap);
    (void)fputc('\n', out);
    va_end(ap);
    (void)fflush(out);
}

static void trace(const char *fmt, ...)
{
    va_list ap;
    FILE *out = (g_log != NULL) ? g_log : stderr;

    if (!g_verbose)
        return;
    va_start(ap, fmt);
    (void)fputs("nd-i2c-keypadd: ", out);
    (void)vfprintf(out, fmt, ap);
    (void)fputc('\n', out);
    va_end(ap);
    (void)fflush(out);
}

/* ------------------------------------------------------------------ *
 * Explicit little-endian accessors
 * ------------------------------------------------------------------ *
 *
 * CODING-STANDARDS.md section 6: no endianness assumptions in a wire format,
 * read and write bytes explicitly. virtio is little-endian on the wire; this
 * daemon happens to run on an x86-64 host and the guest happens to be a
 * little-endian ARM, so a struct cast would work today and would be wrong.
 */

static uint16_t ld_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t ld_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ld_le64(const uint8_t *p)
{
    return (uint64_t)ld_le32(p) | ((uint64_t)ld_le32(p + 4) << 32);
}

static void st_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void st_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ *
 * The virtio split virtqueue
 * ------------------------------------------------------------------ */

#define VRING_DESC_F_NEXT     1u
#define VRING_DESC_F_WRITE    2u
#define VRING_DESC_F_INDIRECT 4u
#define VRING_AVAIL_F_NO_INTERRUPT 1u

#define VRING_DESC_SIZE 16u /* addr(8) len(4) flags(2) next(2) */

/* virtio_i2c.h, quoted rather than included so this file builds on a host
 * with no kernel headers. */
#define VIRTIO_I2C_FLAGS_M_RD 0x00000002u
#define VIRTIO_I2C_MSG_OK     0u
#define VIRTIO_I2C_MSG_ERR    1u
#define VIRTIO_I2C_OUT_HDR_SIZE 8u

/* ------------------------------------------------------------------ *
 * vhost-user
 * ------------------------------------------------------------------ */

#define VHOST_USER_VERSION      1u
#define VHOST_USER_REPLY_MASK   (1u << 2)
#define VHOST_USER_NEED_REPLY   (1u << 3)
#define VHOST_USER_VRING_NOFD   (1u << 8)

enum {
    VHOST_USER_GET_FEATURES = 1,
    VHOST_USER_SET_FEATURES = 2,
    VHOST_USER_SET_OWNER = 3,
    VHOST_USER_RESET_OWNER = 4,
    VHOST_USER_SET_MEM_TABLE = 5,
    VHOST_USER_SET_LOG_BASE = 6,
    VHOST_USER_SET_LOG_FD = 7,
    VHOST_USER_SET_VRING_NUM = 8,
    VHOST_USER_SET_VRING_ADDR = 9,
    VHOST_USER_SET_VRING_BASE = 10,
    VHOST_USER_GET_VRING_BASE = 11,
    VHOST_USER_SET_VRING_KICK = 12,
    VHOST_USER_SET_VRING_CALL = 13,
    VHOST_USER_SET_VRING_ERR = 14,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM = 17,
    VHOST_USER_SET_VRING_ENABLE = 18,
    VHOST_USER_SET_STATUS = 39,
    VHOST_USER_GET_STATUS = 40
};

/* The feature bits offered to the guest.
 *
 * VIRTIO_I2C_F_ZERO_LENGTH_REQUEST (bit 0) is MANDATORY and its absence is a
 * one-line probe failure that looks like a missing driver:
 * virtio_i2c_probe() opens with `if (!virtio_has_feature(vdev,
 * VIRTIO_I2C_F_ZERO_LENGTH_REQUEST)) { dev_err("Zero-length request feature
 * is mandatory"); return -EINVAL; }`.
 *
 * VIRTIO_F_VERSION_1 (bit 32) is what makes this a MODERN virtio device.
 * run_qemu.sh already passes -global virtio-mmio.force-legacy=false for the
 * keyboard, for the same reason and with EMPIRICAL-FINDINGS 10 behind it.
 *
 * VHOST_USER_F_PROTOCOL_FEATURES (bit 30) is a vhost-user negotiation bit and
 * never reaches the guest; QEMU only asks for the protocol feature set when
 * it is set. */
#define ND_VIRTIO_I2C_F_ZERO_LENGTH_REQUEST (1ULL << 0)
#define ND_VIRTIO_F_VERSION_1               (1ULL << 32)
#define ND_VHOST_USER_F_PROTOCOL_FEATURES   (1ULL << 30)

#define ND_BACKEND_FEATURES                                                                        \
    (ND_VIRTIO_I2C_F_ZERO_LENGTH_REQUEST | ND_VIRTIO_F_VERSION_1 |                                 \
     ND_VHOST_USER_F_PROTOCOL_FEATURES)

/* ============ TWO RING FEATURES THIS BACKEND NEVER OFFERED ============
 *
 * And gets anyway. QEMU's hw/virtio/vhost-user-i2c.c passes only
 * VIRTIO_I2C_F_ZERO_LENGTH_REQUEST in its feature_bits[] list, and
 * vhost_get_features() masks ONLY the bits in that list -- so
 * VIRTIO_RING_F_INDIRECT_DESC and VIRTIO_RING_F_EVENT_IDX, which the virtio
 * device class turns on by default, sail straight past GET_FEATURES and are
 * negotiated with a guest whose backend said nothing about them. There is no
 * device property to switch them off: vhost-user-i2c-device's only property
 * is `chardev`.
 *
 * MEASURED, and both failures are silent hangs rather than errors:
 *
 *   guest features 0x170000001   -- bits 0, 28, 29, 30, 32
 *
 *   INDIRECT_DESC: every request arrived as one descriptor with
 *   VRING_DESC_F_INDIRECT pointing at a three-descriptor table. A backend
 *   that walks only the direct chain finds no out header, writes no status
 *   byte, and the guest reads back the zeroed buffer it started with -- so
 *   nd_pcf8575_read16() returned SUCCESS with 0x0000, which is a value the
 *   chip can produce and therefore a lie nothing downstream can catch.
 *
 *   EVENT_IDX: the DRIVER decides whether to kick by reading `avail_event`,
 *   the u16 the DEVICE writes after the used ring, through
 *   vring_need_event(). A backend that never writes it leaves it at 0, and
 *   from the second request onward vring_need_event(0, new, old) is false --
 *   so the guest stops ringing the doorbell and waits for ever on a request
 *   this daemon has never been told about. Measured: exactly four transfers
 *   completed, each on a wake-up that happened for some other reason, and the
 *   fifth hung until the timeout killed QEMU.
 */
#define ND_VIRTIO_RING_F_INDIRECT_DESC (1ULL << 28)
#define ND_VIRTIO_RING_F_EVENT_IDX     (1ULL << 29)

/* Deliberately none. Every protocol feature QEMU can negotiate here is an
 * extra message this daemon would have to answer correctly -- MQ, CONFIG,
 * REPLY_ACK, STATUS, CONFIGURE_MEM_SLOTS -- and a virtio-i2c adapter with one
 * queue and no config space needs none of them. Measured: QEMU 8.2 completes
 * the whole handshake against a zero here. */
#define ND_PROTOCOL_FEATURES 0ULL

#define ND_MAX_MEM_REGIONS 8

typedef struct {
    uint64_t guest_phys;
    uint64_t size;
    uint64_t qemu_va; /* QEMU's own userspace address for this region */
    uint8_t *host;    /* where WE mapped it */
    void *mmap_base;
    size_t mmap_len;
} nd_mem_region;

typedef struct {
    int sock;      /* the accepted vhost-user connection */
    int kick_fd;   /* the guest's doorbell                */
    int call_fd;   /* our interrupt line back to the guest */
    int err_fd;

    nd_mem_region mem[ND_MAX_MEM_REGIONS];
    size_t n_mem;

    uint32_t vring_num;      /* queue size, in descriptors */
    uint8_t *desc;
    uint8_t *avail;
    uint8_t *used;
    uint16_t last_avail_idx;
    bool enabled;
    bool started;

    uint64_t features;
    nd_chip chip;
} nd_daemon;

/* guest physical -> our mapping. Descriptor addresses are GPAs. */
static uint8_t *gpa_to_host(const nd_daemon *d, uint64_t gpa, uint64_t len)
{
    size_t i;

    for (i = 0u; i < d->n_mem; i++) {
        const nd_mem_region *r = &d->mem[i];
        if (gpa >= r->guest_phys && gpa + len <= r->guest_phys + r->size)
            return r->host + (gpa - r->guest_phys);
    }
    return NULL;
}

/* QEMU's userspace address -> our mapping. The vring addresses arrive this
 * way and not as GPAs, which is the single easiest thing to get wrong here:
 * both are 64-bit numbers and on a small guest they are not far apart. */
static uint8_t *qva_to_host(const nd_daemon *d, uint64_t qva, uint64_t len)
{
    size_t i;

    for (i = 0u; i < d->n_mem; i++) {
        const nd_mem_region *r = &d->mem[i];
        if (qva >= r->qemu_va && qva + len <= r->qemu_va + r->size)
            return r->host + (qva - r->qemu_va);
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Socket plumbing
 * ------------------------------------------------------------------ */

static bool read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0u;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t put = 0u;

    while (put < len) {
        ssize_t n = write(fd, p + put, len - put);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        put += (size_t)n;
    }
    return true;
}

/* Read a vhost-user header plus whatever file descriptors rode with it.
 * Returns the number of bytes of header read, or -1. */
static int recv_header(int fd, uint8_t hdr[12], int *fds, size_t max_fds, size_t *n_fds)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        uint8_t buf[CMSG_SPACE(sizeof(int) * ND_MAX_MEM_REGIONS)];
    } control;
    struct cmsghdr *cmsg;
    ssize_t n;

    *n_fds = 0u;
    memset(&msg, 0, sizeof msg);
    memset(&control, 0, sizeof control);
    iov.iov_base = hdr;
    iov.iov_len = 12u;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;

    do {
        n = recvmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return -1;

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            size_t i;
            for (i = 0u; i < count; i++) {
                int got;
                memcpy(&got, CMSG_DATA(cmsg) + i * sizeof(int), sizeof got);
                if (*n_fds < max_fds)
                    fds[(*n_fds)++] = got;
                else
                    (void)close(got);
            }
        }
    }
    return (int)n;
}

static bool send_reply(int fd, uint32_t request, const void *payload, uint32_t size)
{
    uint8_t hdr[12];

    st_le32(hdr, request);
    st_le32(hdr + 4, VHOST_USER_VERSION | VHOST_USER_REPLY_MASK);
    st_le32(hdr + 8, size);
    if (!write_exact(fd, hdr, sizeof hdr))
        return false;
    if (size > 0u)
        return write_exact(fd, payload, size);
    return true;
}

static bool send_reply_u64(int fd, uint32_t request, uint64_t value)
{
    uint8_t buf[8];

    st_le32(buf, (uint32_t)(value & 0xFFFFFFFFu));
    st_le32(buf + 4, (uint32_t)((value >> 32) & 0xFFFFFFFFu));
    return send_reply(fd, request, buf, 8u);
}

/* ------------------------------------------------------------------ *
 * The i2c transaction itself
 * ------------------------------------------------------------------ */

/* One virtio-i2c message. `buf` is the data segment (NULL when there is
 * none); returns VIRTIO_I2C_MSG_OK or VIRTIO_I2C_MSG_ERR.
 *
 * An ERR here is the wire NAK. It is what the guest sees for an address
 * nobody answers, and it is where THE ERRNO GAP in this file's header
 * happens: the guest's i2c core turns it into a short transfer with no errno
 * rather than into ENXIO. */
static uint8_t do_transfer(nd_daemon *d, uint16_t addr, bool is_read, uint8_t *buf, uint32_t len)
{
    if (addr == ND_KEYPAD_ADDR) {
        if (is_read) {
            uint16_t v = chip_read16(&d->chip);
            /* A read shorter than two bytes is not something the phone's
             * driver does, but the wizard's probe and a stray i2cdetect both
             * can: answer with as much of the word as was asked for, low byte
             * first, which is the order the chip clocks its two ports out
             * in. */
            if (len >= 1u)
                buf[0] = (uint8_t)(v & 0xFFu);
            if (len >= 2u)
                buf[1] = (uint8_t)((v >> 8) & 0xFFu);
            trace("keypad READ  -> 0x%04X (latch 0x%04X, %zu shorted pairs)", v, d->chip.latch,
                  d->chip.n_edges);
        } else if (len >= 2u) {
            d->chip.latch = ld_le16(buf);
            trace("keypad WRITE <- 0x%04X", d->chip.latch);
        } else if (len == 1u) {
            /* A one-byte write leaves the high port untouched, which is what
             * a real PCF8575 does: it latches P0 on the first byte and P1 on
             * the second, and a master that stops after one has written half
             * the port. */
            d->chip.latch = (uint16_t)((d->chip.latch & 0xFF00u) | buf[0]);
        }
        return VIRTIO_I2C_MSG_OK;
    }

    if (addr == ND_GAUGE_ADDR) {
        if (!is_read) {
            if (len >= 1u)
                d->chip.gauge_reg = buf[0];
            return VIRTIO_I2C_MSG_OK;
        }
        if (len >= 2u) {
            uint16_t v;
            switch (d->chip.gauge_reg) {
            case 0x02:
                v = ND_GAUGE_VCELL;
                break;
            case 0x04:
                v = ND_GAUGE_SOC;
                break;
            case 0x08:
                v = ND_GAUGE_VERSION;
                break;
            case 0x0C:
                v = ND_GAUGE_CONFIG;
                break;
            case 0x16:
                v = ND_GAUGE_CRATE;
                break;
            default:
                v = 0x0000u;
                break;
            }
            /* The MAX17048 is BIG-endian on the wire -- high byte first --
             * where the PCF8575 is low byte first. nd_battery.c's read16()
             * assembles d[0] << 8 | d[1] and nd_pcf8575.c's assembles
             * d[0] | d[1] << 8; two chips on one bus with opposite byte
             * orders is the phone's own arrangement and not a mistake here. */
            buf[0] = (uint8_t)((v >> 8) & 0xFFu);
            buf[1] = (uint8_t)(v & 0xFFu);
            trace("gauge  READ  reg 0x%02X -> 0x%04X", d->chip.gauge_reg, v);
        }
        return VIRTIO_I2C_MSG_OK;
    }

    /* Nobody is soldered there. nd_keypadsetup.c walks 0x20..0x27 and has to
     * find exactly one chip; a model that ACKed everything would enrol the
     * first address it tried and call it a keypad. */
    trace("NAK addr 0x%02X (%s, %u bytes)", addr, is_read ? "read" : "write", len);
    return VIRTIO_I2C_MSG_ERR;
}

/* One readable or writable run of guest memory out of a descriptor chain. */
typedef struct {
    uint8_t *addr;
    uint32_t len;
    bool writable;
} nd_seg;

#define ND_MAX_SEGS 8

/* Walk a descriptor table from `first`, appending each descriptor's buffer to
 * `segs`. Recursion is one level deep by construction -- the virtio spec
 * forbids an indirect table containing another indirect descriptor -- so this
 * is a loop with one nested call and not a general graph walk. */
static size_t collect_chain(const nd_daemon *d, const uint8_t *table, uint32_t table_len,
                            uint16_t first, nd_seg *segs, size_t n, bool allow_indirect)
{
    uint16_t idx = first;
    uint32_t entries = table_len / VRING_DESC_SIZE;
    uint32_t guard;

    for (guard = 0u; guard <= entries; guard++) {
        const uint8_t *desc;
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
        uint8_t *host;

        if (idx >= entries)
            break;
        desc = table + (uint32_t)idx * VRING_DESC_SIZE;
        addr = ld_le64(desc);
        len = ld_le32(desc + 8);
        flags = ld_le16(desc + 12);
        next = ld_le16(desc + 14);

        host = gpa_to_host(d, addr, len);
        if (host == NULL) {
            logline("descriptor 0x%llx+%u is outside every memory region",
                    (unsigned long long)addr, len);
            return n;
        }
        if ((flags & VRING_DESC_F_INDIRECT) != 0u) {
            if (!allow_indirect) {
                logline("an indirect descriptor inside an indirect table; the spec forbids it");
                return n;
            }
            n = collect_chain(d, host, len, 0u, segs, n, false);
        } else if (n < ND_MAX_SEGS) {
            segs[n].addr = host;
            segs[n].len = len;
            segs[n].writable = (flags & VRING_DESC_F_WRITE) != 0u;
            n++;
        } else {
            logline("more than %d segments in one request", ND_MAX_SEGS);
            return n;
        }

        if ((flags & VRING_DESC_F_NEXT) == 0u)
            break;
        idx = next;
    }
    return n;
}

/* Service one request. Returns the number of bytes written into the guest's
 * writable segments, which is what goes in the used ring's `len`. */
static uint32_t service_chain(nd_daemon *d, uint16_t head)
{
    nd_seg segs[ND_MAX_SEGS];
    size_t n;
    size_t i;
    uint8_t *out_hdr = NULL;
    nd_seg *data = NULL;
    uint8_t *status = NULL;
    uint32_t written;

    memset(segs, 0, sizeof segs);
    n = collect_chain(d, d->desc, d->vring_num * VRING_DESC_SIZE, head, segs, 0u, true);

    /* The shape i2c-virtio builds is out_hdr, an optional data buffer, and a
     * one-byte status. The header is the first readable segment, the status
     * is the last writable one, and whatever is left in the middle is the
     * data -- which is direction-agnostic on purpose, because the same three
     * descriptors carry a write and a read with only the WRITE flag moved. */
    for (i = 0u; i < n; i++) {
        if (out_hdr == NULL && !segs[i].writable && segs[i].len >= VIRTIO_I2C_OUT_HDR_SIZE)
            out_hdr = segs[i].addr;
        else if (segs[i].writable && segs[i].len == 1u && i + 1u == n)
            status = segs[i].addr;
        else if (data == NULL)
            data = &segs[i];
    }
    if (out_hdr == NULL || status == NULL) {
        logline("malformed i2c request: %zu segments, %s%s", n,
                out_hdr == NULL ? "no out header " : "", status == NULL ? "no status byte" : "");
        return 0u;
    }

    {
        uint16_t raw_addr = ld_le16(out_hdr);
        uint32_t flags = ld_le32(out_hdr + 4);
        bool is_read = (flags & VIRTIO_I2C_FLAGS_M_RD) != 0u;
        /* The virtio-i2c out header carries the address ALREADY SHIFTED LEFT
         * one bit (i2c-virtio.c: `out_hdr.addr = cpu_to_le16(msgs[i].addr <<
         * 1)`), which is the read/write bit position on a real wire. */
        uint16_t addr = (uint16_t)(raw_addr >> 1);

        if (data != NULL && is_read != data->writable) {
            logline("i2c request direction disagrees with its buffer");
            *status = (uint8_t)VIRTIO_I2C_MSG_ERR;
            return 1u;
        }
        *status = do_transfer(d, addr, is_read, (data != NULL) ? data->addr : NULL,
                              (data != NULL) ? data->len : 0u);
        written = 1u;
        if (is_read && data != NULL && *status == (uint8_t)VIRTIO_I2C_MSG_OK)
            written += data->len;
    }
    return written;
}

static void ring_notify(nd_daemon *d)
{
    uint64_t one = 1u;

    if (d->call_fd < 0)
        return;
    /* VRING_AVAIL_F_NO_INTERRUPT is the guest asking not to be poked, and it
     * is only meaningful when EVENT_IDX was NOT negotiated -- with EVENT_IDX
     * the driver leaves the flag at zero and publishes `used_event` instead.
     * This daemon does not read used_event: notifying more often than
     * strictly required is always legal, and at nine transfers per matrix
     * scan the cost is nothing against the risk of a suppressed interrupt
     * that stalls the guest for ever. */
    if ((d->features & ND_VIRTIO_RING_F_EVENT_IDX) == 0u &&
        (ld_le16(d->avail) & VRING_AVAIL_F_NO_INTERRUPT) != 0u)
        return;
    if (write(d->call_fd, &one, sizeof one) != (ssize_t)sizeof one)
        logline("could not signal the guest: %s", strerror(errno));
}

/* THE OTHER HALF OF EVENT_IDX, AND THE ONE THAT IS NOT OPTIONAL.
 *
 * `avail_event` is the u16 immediately after the used ring, written by the
 * DEVICE and read by the DRIVER in virtqueue_kick_prepare_split(): the guest
 * rings the doorbell only when vring_need_event(avail_event, new, old) says
 * so. Leaving it at zero silences the doorbell from the second request
 * onward. Setting it to the index we have consumed up to means "tell me about
 * the next one", which is one notification per request -- the same rate a
 * backend without EVENT_IDX gets, and the only rate that is safe when a
 * transfer can be the only thing the guest is waiting on. */
static void publish_avail_event(nd_daemon *d)
{
    if ((d->features & ND_VIRTIO_RING_F_EVENT_IDX) == 0u)
        return;
    st_le16(d->used + 4u + (uint32_t)d->vring_num * 8u, d->last_avail_idx);
    /* ============ AND THE FENCE, WHICH IS THE HALF THAT IS NOT ORDERING
     * ============ FOR TIDINESS
     *
     * SEQ_CST and not RELEASE, because the pair to close is a STORE followed
     * by a LOAD -- this store of avail_event, and process_queue()'s re-read of
     * avail->idx immediately after it. StoreLoad is the one reordering x86-64
     * TSO permits, and it is exactly the pair the virtio spec puts a full
     * barrier between. Without it:
     *
     *   device  stores avail_event = N   (still in its store buffer)
     *   device  loads  avail->idx  = N   (pre-add), leaves the loop, poll()s
     *   driver  stores avail->idx  = N+1, virtio_mb()
     *   driver  loads  avail_event = N-1 (stale)
     *           vring_need_event(N-1, N+1, N) is 1 < 1, so it does NOT kick
     *
     * and the request is never serviced and never kicked again. The
     * consequence is not a dropped keystroke: i2c-virtio waits on a
     * completion with the bus lock held and no timeout, so the guest's key
     * loop and nd_battery's poll both block for ever with the daemon alive
     * and idle. Measured on a booted guest that EVENT_IDX is really
     * negotiated here -- `guest features 0x170000001`, bit 29 set -- so
     * avail_event is the ONLY thing making the guest ring the doorbell. The
     * fence lives in here rather than at the call site so that it cannot be
     * left out of a future one. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void process_queue(nd_daemon *d)
{
    uint16_t avail_idx;
    bool any = false;

    /* `enabled` used to be parsed and never read, which is dead state that
     * reads as coverage: this backend advertises
     * VHOST_USER_F_PROTOCOL_FEATURES, which is what makes QEMU send
     * SET_VRING_ENABLE at all, so asking for the message and ignoring its
     * content meant a ring disabled for a device reset kept being consumed
     * and kept getting used entries the driver was not accounting for. It
     * defaults to TRUE in maybe_start() rather than false, because a QEMU
     * that never sends the message must still get a working bus -- an
     * adapter that registers and never answers is the failure mode this whole
     * file is written to avoid. */
    if (!d->started || !d->enabled || d->desc == NULL || d->avail == NULL || d->used == NULL)
        return;

    avail_idx = ld_le16(d->avail + 2);
    while (d->last_avail_idx != avail_idx) {
        uint16_t slot = (uint16_t)(d->last_avail_idx % (uint16_t)d->vring_num);
        uint16_t head = ld_le16(d->avail + 4u + (uint32_t)slot * 2u);
        uint32_t written;
        uint16_t used_idx;
        uint8_t *elem;

        if (head >= d->vring_num) {
            logline("avail ring head %u is past the queue size %u", head, d->vring_num);
            break;
        }
        written = service_chain(d, head);

        used_idx = ld_le16(d->used + 2);
        elem = d->used + 4u + (uint32_t)(used_idx % (uint16_t)d->vring_num) * 8u;
        st_le32(elem, head);
        st_le32(elem + 4, written);
        /* The guest must never see the used index move before the element it
         * points at is complete. On x86-64 a compiler barrier is enough for
         * store-store; __atomic_thread_fence keeps that true if this is ever
         * built for the host it emulates. */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        st_le16(d->used + 2, (uint16_t)(used_idx + 1u));

        d->last_avail_idx++;
        publish_avail_event(d);
        any = true;
        avail_idx = ld_le16(d->avail + 2);
    }
    if (any)
        ring_notify(d);
}

/* ------------------------------------------------------------------ *
 * vhost-user message handling
 * ------------------------------------------------------------------ */

static void unmap_regions(nd_daemon *d)
{
    size_t i;

    for (i = 0u; i < d->n_mem; i++) {
        if (d->mem[i].mmap_base != NULL)
            (void)munmap(d->mem[i].mmap_base, d->mem[i].mmap_len);
    }
    d->n_mem = 0u;
    d->desc = NULL;
    d->avail = NULL;
    d->used = NULL;
}

/* Takes ownership of every descriptor in `fds` and leaves each entry -1,
 * whichever way it returns. The alternative -- closing what it mapped and
 * leaving the rest to the caller -- was written first and is a double close
 * waiting to happen: the caller cannot tell how far this function got, and an
 * fd closed twice in a process that is also accepting sockets eventually
 * closes something else. */
static bool handle_mem_table(nd_daemon *d, const uint8_t *body, uint32_t size, int *fds,
                             size_t n_fds)
{
    uint32_t nregions;
    uint32_t i;
    bool ok = true;

    if (size < 8u) {
        ok = false;
        goto done;
    }
    nregions = ld_le32(body);
    if (nregions > ND_MAX_MEM_REGIONS || nregions > n_fds) {
        logline("SET_MEM_TABLE with %u regions and %zu fds", nregions, n_fds);
        ok = false;
        goto done;
    }
    /* The array is read at body + 8 + i*32, so a message that says four
     * regions and carries two entries would otherwise read uninitialised
     * stack and map whatever it found there. `size` is what read_exact()
     * actually filled; nothing past it is data. */
    if ((size_t)size < 8u + (size_t)nregions * 32u) {
        logline("SET_MEM_TABLE claims %u regions but carries only %u bytes", nregions, size);
        ok = false;
        goto done;
    }
    unmap_regions(d);
    for (i = 0u; i < nregions; i++) {
        const uint8_t *r = body + 8u + (size_t)i * 32u;
        uint64_t gpa = ld_le64(r);
        uint64_t len = ld_le64(r + 8);
        uint64_t qva = ld_le64(r + 16);
        uint64_t off = ld_le64(r + 24);
        void *base;

        base = mmap(NULL, (size_t)(len + off), PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (base == MAP_FAILED) {
            /* This is the failure mode of forgetting `-object
             * memory-backend-memfd,share=on`: QEMU's default anonymous guest
             * RAM is not shareable, so the fd it hands over cannot be mapped
             * and every transfer would silently do nothing. Say which flag. */
            logline("cannot map guest region %u (%s). Is the guest's RAM a shared "
                    "memory-backend-memfd?",
                    i, strerror(errno));
            ok = false;
            goto done;
        }
        d->mem[i].guest_phys = gpa;
        d->mem[i].size = len;
        d->mem[i].qemu_va = qva;
        d->mem[i].mmap_base = base;
        d->mem[i].mmap_len = (size_t)(len + off);
        d->mem[i].host = (uint8_t *)base + off;
        /* n_mem is bumped HERE and not once at the end, so that a mmap that
         * fails halfway leaves unmap_regions() something to free. It used to
         * be a single `d->n_mem = nregions` on the success path only, so the
         * failure path -- guest RAM that is not a shared memory-backend-memfd,
         * which is the documented and reachable case -- stranded every window
         * it had already mapped onto 64 MB of guest RAM for the life of the
         * process. Nothing would have connected the growing RSS to this: two
         * of the three callers keep one daemon across two boots. */
        d->n_mem = i + 1u;
        trace("region %u gpa 0x%llx len 0x%llx qva 0x%llx", i, (unsigned long long)gpa,
              (unsigned long long)len, (unsigned long long)qva);
        /* The mapping holds its own reference, so the descriptor is finished
         * with. QEMU sends a fresh memory table whenever guest RAM changes
         * shape, and a daemon that kept every fd would run out of them in a
         * long session rather than in a way anybody would connect to this. */
        (void)close(fds[i]);
        fds[i] = -1;
    }

done:
    for (i = 0u; i < (uint32_t)n_fds; i++) {
        if (fds[i] >= 0) {
            (void)close(fds[i]);
            fds[i] = -1;
        }
    }
    return ok;
}

static void maybe_start(nd_daemon *d)
{
    if (d->started)
        return;
    if (d->kick_fd < 0 || d->desc == NULL || d->avail == NULL || d->used == NULL)
        return;
    d->started = true;
    d->enabled = true;
    publish_avail_event(d);
    logline("virtqueue live: %u descriptors, the guest's keypad bus is up", d->vring_num);
}

/* Returns false when the connection is finished. */
static bool handle_message(nd_daemon *d)
{
    uint8_t hdr[12];
    uint8_t body[4096];
    int fds[ND_MAX_MEM_REGIONS];
    size_t n_fds = 0u;
    uint32_t request;
    uint32_t flags;
    uint32_t size;
    size_t i;
    bool ok = true;

    /* != 12 AND NOT < 0. recv_header() computes the byte count and says so in
     * its own contract; testing only for -1 accepted a 1..11-byte read as a
     * complete header and left `request`, `flags` and `size` as stack
     * garbage. The good outcome of that is a dropped connection; the bad one
     * is a read_exact() for the wrong number of bytes, which desynchronises
     * the vhost-user stream permanently -- every later message is misparsed,
     * the queue never starts, and the guest sees an adapter that registers
     * and never answers. That is the silent shape the default case below
     * exists to prevent, and unlike those it printed nothing.
     *
     * `goto sweep` and not `return false`: recv_header() has ALREADY taken
     * ownership of any descriptor that rode with the header. */
    if (recv_header(d->sock, hdr, fds, ND_MAX_MEM_REGIONS, &n_fds) != 12) {
        ok = false;
        goto sweep;
    }
    request = ld_le32(hdr);
    flags = ld_le32(hdr + 4);
    size = ld_le32(hdr + 8);
    if (size > sizeof body) {
        logline("vhost-user message %u carries %u bytes, more than we will read", request, size);
        ok = false;
        goto sweep;
    }
    if (size > 0u && !read_exact(d->sock, body, size)) {
        ok = false;
        goto sweep;
    }

    switch (request) {
    case VHOST_USER_GET_FEATURES:
        ok = send_reply_u64(d->sock, request, ND_BACKEND_FEATURES);
        break;
    case VHOST_USER_SET_FEATURES:
        if (size >= 8u)
            d->features = ld_le64(body);
        trace("guest features 0x%llx", (unsigned long long)d->features);
        break;
    case VHOST_USER_GET_PROTOCOL_FEATURES:
        ok = send_reply_u64(d->sock, request, ND_PROTOCOL_FEATURES);
        break;
    case VHOST_USER_SET_PROTOCOL_FEATURES:
        break;
    case VHOST_USER_GET_QUEUE_NUM:
        ok = send_reply_u64(d->sock, request, 1u);
        break;
    case VHOST_USER_SET_OWNER:
    case VHOST_USER_RESET_OWNER:
        break;
    case VHOST_USER_SET_MEM_TABLE:
        /* It takes every fd and leaves them all -1, so the sweep at the end of
         * this function has nothing left to do for them. */
        if (!handle_mem_table(d, body, size, fds, n_fds))
            ok = false;
        break;
    case VHOST_USER_SET_VRING_NUM:
        if (size >= 8u)
            d->vring_num = ld_le32(body + 4);
        break;
    case VHOST_USER_SET_VRING_BASE:
        if (size >= 8u)
            d->last_avail_idx = (uint16_t)ld_le32(body + 4);
        break;
    case VHOST_USER_GET_VRING_BASE: {
        uint8_t out[8];
        /* The queue stops here. QEMU asks for our position so the guest can
         * be resumed elsewhere; answering with a stale index would replay
         * transfers. */
        d->started = false;
        st_le32(out, 0u);
        st_le32(out + 4, d->last_avail_idx);
        ok = send_reply(d->sock, request, out, 8u);
        break;
    }
    case VHOST_USER_SET_VRING_ADDR:
        if (size >= 40u) {
            uint64_t desc_qva = ld_le64(body + 8);
            uint64_t used_qva = ld_le64(body + 16);
            uint64_t avail_qva = ld_le64(body + 24);
            d->desc = qva_to_host(d, desc_qva, (uint64_t)d->vring_num * VRING_DESC_SIZE);
            /* +2 on each: with EVENT_IDX the avail ring carries `used_event`
             * after its ring[] and the used ring carries `avail_event` after
             * its own, and this daemon writes the second of them. */
            d->avail = qva_to_host(d, avail_qva, 8u + (uint64_t)d->vring_num * 2u);
            d->used = qva_to_host(d, used_qva, 8u + (uint64_t)d->vring_num * 8u);
            if (d->desc == NULL || d->avail == NULL || d->used == NULL) {
                logline("a vring address fell outside the memory table");
                ok = false;
            }
            maybe_start(d);
        }
        break;
    case VHOST_USER_SET_VRING_KICK:
        if (d->kick_fd >= 0)
            (void)close(d->kick_fd);
        d->kick_fd = -1;
        if (size >= 8u && (ld_le64(body) & VHOST_USER_VRING_NOFD) == 0u && n_fds > 0u) {
            d->kick_fd = fds[0];
            fds[0] = -1;
        }
        maybe_start(d);
        break;
    case VHOST_USER_SET_VRING_CALL:
        if (d->call_fd >= 0)
            (void)close(d->call_fd);
        d->call_fd = -1;
        if (size >= 8u && (ld_le64(body) & VHOST_USER_VRING_NOFD) == 0u && n_fds > 0u) {
            d->call_fd = fds[0];
            fds[0] = -1;
        }
        break;
    case VHOST_USER_SET_VRING_ERR:
        if (d->err_fd >= 0)
            (void)close(d->err_fd);
        d->err_fd = -1;
        if (size >= 8u && (ld_le64(body) & VHOST_USER_VRING_NOFD) == 0u && n_fds > 0u) {
            d->err_fd = fds[0];
            fds[0] = -1;
        }
        break;
    case VHOST_USER_SET_VRING_ENABLE:
        if (size >= 8u)
            d->enabled = ld_le32(body + 4) != 0u;
        break;
    case VHOST_USER_SET_LOG_BASE:
        /* Dirty-page logging is for live migration, which nothing here does.
         * Answering is still required: QEMU waits for a reply. */
        ok = send_reply_u64(d->sock, request, 0u);
        break;
    case VHOST_USER_SET_LOG_FD:
    case VHOST_USER_SET_STATUS:
        break;
    case VHOST_USER_GET_STATUS:
        ok = send_reply_u64(d->sock, request, 0u);
        break;
    default:
        /* Loud rather than silent. A message this daemon does not know is a
         * QEMU version that wants something new, and the failure it would
         * otherwise produce is a guest with an adapter that never answers. */
        logline("unhandled vhost-user request %u (size %u)", request, size);
        if ((flags & VHOST_USER_NEED_REPLY) != 0u)
            ok = send_reply_u64(d->sock, request, (uint64_t)-1);
        break;
    }

    if ((flags & VHOST_USER_NEED_REPLY) != 0u && request != VHOST_USER_GET_FEATURES &&
        request != VHOST_USER_GET_PROTOCOL_FEATURES && request != VHOST_USER_GET_QUEUE_NUM &&
        request != VHOST_USER_GET_VRING_BASE && request != VHOST_USER_GET_STATUS &&
        request != VHOST_USER_SET_LOG_BASE) {
        ok = send_reply_u64(d->sock, request, ok ? 0u : (uint64_t)-1) && ok;
    }

sweep:
    /* EVERY exit runs this, including the three above that used to `return
     * false` straight past it. A peer that dies between the twelve-byte
     * header and its payload would otherwise leave the kick, call or err
     * eventfd that rode with that header open for good -- and this daemon
     * loops back to accept() and serves further sessions rather than exiting,
     * so they accumulate instead of being reclaimed at process exit. */
    for (i = 0u; i < n_fds; i++) {
        if (fds[i] >= 0)
            (void)close(fds[i]);
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * The key channel
 * ------------------------------------------------------------------ */

static bool key_lookup(const char *name, uint8_t *row, uint8_t *col)
{
    uint8_t r;
    uint8_t c;

    for (r = 0u; r < ND_ROWS; r++) {
        for (c = 0u; c < ND_COLS; c++) {
            if (strcmp(name, ND_KEY_NAMES[r][c]) == 0) {
                *row = r;
                *col = c;
                return true;
            }
        }
    }
    return false;
}

/* One line of the key channel. The grammar is deliberately tiny and it is a
 * TEST INTERFACE as much as a typing one:
 *
 *   press <key>          hold a key down
 *   release <key>        let it up
 *   release all          let everything up
 *   tap <key> [ms]       press, and release after ms (default 60, which is
 *                        twelve scans at ND_READ_POLL_US and comfortably more
 *                        than ND_RELEASE_SCANS)
 *   short <pinA> <pinB>  short two pins that no key joins -- the wizard's
 *                        world, where a pin is not yet known to be a row
 *   open <pinA> <pinB>   the other direction
 *   status               print the port word the guest would read now
 *
 * `short` exists because the first-boot wizard drives all sixteen pins and
 * enrols whatever pairs come back, so a keypad expressed only as named keys
 * could not present it a scrambled layout.
 *
 * TAPS OVERLAP AND EACH ONE RELEASES ONLY ITSELF, so `press hash` followed by
 * a run of `tap` digits is a rollover test rather than a hold that quietly
 * ends -- see nd_taps above. `release all` is the only verb that lets
 * everything up, which is why it exists separately. */
static void key_command(nd_daemon *d, char *line, nd_taps *taps, double now)
{
    char *verb;
    char *arg1;
    char *arg2;
    char *save = NULL;
    uint8_t row;
    uint8_t col;

    verb = strtok_r(line, " \t\r\n", &save);
    if (verb == NULL || verb[0] == '#')
        return;
    arg1 = strtok_r(NULL, " \t\r\n", &save);
    arg2 = strtok_r(NULL, " \t\r\n", &save);

    if (strcmp(verb, "status") == 0 || strcmp(verb, "read") == 0) {
        /* `read` is the machine-readable spelling and `status` the human one.
         * They print the same number through the same call, which is what
         * lets a host test drive the model without a guest. */
        if (strcmp(verb, "read") == 0)
            (void)printf("PORT %04X\n", chip_read16(&d->chip));
        else
            logline("port 0x%04X (latch 0x%04X, %zu shorted pairs)", chip_read16(&d->chip),
                    d->chip.latch, d->chip.n_edges);
        (void)fflush(stdout);
        return;
    }
    if (strcmp(verb, "drive") == 0 && arg1 != NULL) {
        /* What the guest would have written. A test drives the port directly
         * because the whole point of the model is that a read is a function
         * of the latch and the shorts and of nothing else. */
        d->chip.latch = (uint16_t)(strtoul(arg1, NULL, 0) & 0xFFFFu);
        return;
    }
    if ((strcmp(verb, "short") == 0 || strcmp(verb, "open") == 0) && arg1 != NULL && arg2 != NULL) {
        long a = strtol(arg1, NULL, 0);
        long b = strtol(arg2, NULL, 0);
        if (a < 0 || a >= ND_PINS || b < 0 || b >= ND_PINS || a == b) {
            logline("pin pair %ld,%ld is not two distinct pins in 0..15", a, b);
            return;
        }
        if (strcmp(verb, "short") == 0) {
            chip_press(&d->chip, (uint8_t)a, (uint8_t)b);
        } else {
            /* Cancel any tap pending on this pair as well, or its deadline
             * would later let go of a pair somebody has re-pressed since. */
            taps_cancel(taps, (uint8_t)a, (uint8_t)b);
            chip_release(&d->chip, (uint8_t)a, (uint8_t)b);
        }
        logline("%s P%02ld-P%02ld", verb, a, b);
        return;
    }
    if (strcmp(verb, "release") == 0 && arg1 != NULL && strcmp(arg1, "all") == 0) {
        d->chip.n_edges = 0u;
        taps->n = 0u;
        logline("release all");
        return;
    }
    if (arg1 == NULL || !key_lookup(arg1, &row, &col)) {
        logline("unknown key command: %s %s", verb, arg1 != NULL ? arg1 : "");
        return;
    }
    if (strcmp(verb, "press") == 0) {
        chip_press(&d->chip, ND_ROW_PINS[row], ND_COL_PINS[col]);
        logline("press %s (row %u col %u, P%02u-P%02u)", arg1, row, col, ND_ROW_PINS[row],
                ND_COL_PINS[col]);
    } else if (strcmp(verb, "release") == 0) {
        taps_cancel(taps, ND_ROW_PINS[row], ND_COL_PINS[col]);
        chip_release(&d->chip, ND_ROW_PINS[row], ND_COL_PINS[col]);
        logline("release %s", arg1);
    } else if (strcmp(verb, "tap") == 0) {
        double ms = (arg2 != NULL) ? strtod(arg2, NULL) : 60.0;
        chip_press(&d->chip, ND_ROW_PINS[row], ND_COL_PINS[col]);
        taps_arm(taps, ND_ROW_PINS[row], ND_COL_PINS[col], now + ms / 1000.0);
        logline("tap %s for %.0f ms (row %u col %u)", arg1, ms, row, col);
    } else {
        logline("unknown key command: %s", verb);
    }
}

static double monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ============ IS SOMEBODY ELSE ALREADY SERVING THIS SOCKET? ============
 *
 * The path is a fixed per-host default, so two emulator sessions on one
 * machine name the same one. This used to be an unconditional unlink() below,
 * which meant the SECOND session silently took the FIRST one's socket away:
 * measured, the incumbent stayed alive with its socket path gone while the
 * newcomer served QEMU. The same shape covers the case nobody arranges on
 * purpose -- a session whose terminal died hard leaves an orphan daemon that
 * still holds the socket and still polls the key fifo, which is exactly the
 * state run_qemu.sh's EXIT trap is supposed to prevent.
 *
 * A connect(2) is the test, and not `is there a file there`: a socket file
 * left behind by a SIGKILLed daemon is stale and must be cleared, or one bad
 * exit would block every later boot. Stale gives ECONNREFUSED, live gives
 * success -- and this is a listening socket with backlog 1, so a probe
 * connection that lands is closed immediately and the incumbent's poll loop
 * accepts and drops it on the next message. */
static bool socket_is_being_served(const char *path)
{
    struct sockaddr_un addr;
    int fd;
    bool live;

    if (strlen(path) >= sizeof addr.sun_path)
        return false;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)strncpy(addr.sun_path, path, sizeof addr.sun_path - 1u);
    live = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    (void)close(fd);
    return live;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(void)
{
    (void)fprintf(stderr,
                  "usage: nd-i2c-keypadd --socket PATH [--keys FIFO] [--log FILE] [-v]\n"
                  "\n"
                  "  --socket PATH  the vhost-user socket QEMU's -chardev connects to.\n"
                  "                 QEMU REFUSES TO START if it is not already there, so\n"
                  "                 this daemon has to be running before QEMU is.\n"
                  "  --keys FIFO    a fifo carrying key commands; created if absent.\n"
                  "                 With no --keys, commands are read from stdin.\n"
                  "  --ready FILE   touched once the socket is listening, so a script can\n"
                  "                 wait for it instead of sleeping.\n"
                  "  --log FILE     where this daemon talks; default stderr.\n"
                  "  -v             log every transfer.\n"
                  "  --check        no socket and no QEMU: read key commands on stdin\n"
                  "                 and answer `read` with the port word. This is how\n"
                  "                 the chip model is tested from the host, against the\n"
                  "                 SAME code path a guest transfer takes.\n");
}

int main(int argc, char **argv)
{
    const char *sock_path = NULL;
    const char *keys_path = NULL;
    const char *ready_path = NULL;
    const char *log_path = NULL;
    bool check_only = false;
    nd_daemon d;
    struct sockaddr_un addr;
    int listen_fd;
    int keys_fd = 0;
    nd_taps taps;
    int i;
    int rc = 0;

    taps.n = 0u;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            sock_path = argv[++i];
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc)
            keys_path = argv[++i];
        else if (strcmp(argv[i], "--ready") == 0 && i + 1 < argc)
            ready_path = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            g_verbose = true;
        else if (strcmp(argv[i], "--check") == 0)
            check_only = true;
        else {
            usage();
            return 2;
        }
    }
    if (sock_path == NULL && !check_only) {
        usage();
        return 2;
    }
    /* BEFORE the log is opened, and that ordering is the point: fopen(...,
     * "we") TRUNCATES, so a second session refused three lines from now would
     * otherwise have already destroyed the incumbent's log on its way to
     * saying it was refusing. This is the one message that has to reach
     * stderr rather than a log file. */
    if (!check_only && socket_is_being_served(sock_path)) {
        (void)fprintf(stderr,
                      "nd-i2c-keypadd: %s is already being served by another\n"
                      "  nd-i2c-keypadd. Refusing rather than taking its socket away.\n"
                      "  If that is an orphan from a session that died, kill it; to run a\n"
                      "  second emulator, give this one its own socket, fifo and log (from\n"
                      "  run_qemu.sh, set NEODCT_KEYS -- the other two follow it).\n",
                      sock_path);
        return 1;
    }
    if (log_path != NULL) {
        g_log = fopen(log_path, "we");
        if (g_log == NULL) {
            (void)fprintf(stderr, "nd-i2c-keypadd: cannot write %s: %s\n", log_path,
                          strerror(errno));
            return 1;
        }
    }
    if (!check_only && strlen(sock_path) >= sizeof addr.sun_path) {
        logline("socket path is longer than a sockaddr_un can hold");
        return 1;
    }

    /* SIGPIPE would kill this daemon the moment QEMU exits, which is exactly
     * when a caller most wants its log flushed. */
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);

    memset(&d, 0, sizeof d);
    d.sock = -1;
    d.kick_fd = -1;
    d.call_fd = -1;
    d.err_fd = -1;
    d.vring_num = 0u;
    chip_init(&d.chip);

    /* ============ THE MODEL, WITH NOTHING UNDERNEATH IT ============
     *
     * No socket, no QEMU, no guest. tests/test_qemu_keypadd.py drives this
     * and checks the sixteen-bit word against the PCF8575's electrical rule
     * worked out independently in Python -- which is the same discipline
     * st7789_replay.py follows for the panel, and for the same reason: two
     * implementations of one datasheet can disagree, and one implementation
     * checked against itself cannot. */
    if (check_only) {
        char line[512];

        /* No clock and no poll loop here, so a tap arms a deadline nothing
         * ever reaches -- which is what --check wants: every line is a state
         * change and `read` prints the state. */
        while (fgets(line, (int)sizeof line, stdin) != NULL)
            key_command(&d, line, &taps, 0.0);
        return 0;
    }

    if (keys_path != NULL) {
        if (mkfifo(keys_path, 0600) != 0 && errno != EEXIST) {
            logline("cannot create fifo %s: %s", keys_path, strerror(errno));
            return 1;
        }
        /* O_RDWR and not O_RDONLY: a read-only fifo returns EOF every time
         * the last writer closes, and a script that writes one key with echo
         * would then spin this poll loop at 100 % for the rest of the
         * session. Holding a writer open ourselves means the fifo never ends. */
        keys_fd = open(keys_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (keys_fd < 0) {
            logline("cannot open fifo %s: %s", keys_path, strerror(errno));
            return 1;
        }
    }

    /* Nothing is serving it -- checked above -- so anything here is a leftover
     * from a daemon that did not get to unlink its own. */
    (void)unlink(sock_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        logline("socket: %s", strerror(errno));
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    (void)strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1u);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listen_fd, 1) != 0) {
        logline("cannot listen on %s: %s", sock_path, strerror(errno));
        (void)close(listen_fd);
        return 1;
    }
    logline("listening on %s (keypad 0x%02X, fuel gauge 0x%02X)", sock_path, ND_KEYPAD_ADDR,
            ND_GAUGE_ADDR);
    if (ready_path != NULL) {
        int rf = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (rf >= 0)
            (void)close(rf);
    }

    while (!g_stop) {
        struct pollfd pfd[3];
        nfds_t n = 0u;
        int timeout;

        if (d.sock < 0) {
            pfd[n].fd = listen_fd;
            pfd[n].events = POLLIN;
            n++;
        } else {
            pfd[n].fd = d.sock;
            pfd[n].events = POLLIN;
            n++;
            if (d.kick_fd >= 0) {
                pfd[n].fd = d.kick_fd;
                pfd[n].events = POLLIN;
                n++;
            }
        }
        pfd[n].fd = keys_fd;
        pfd[n].events = POLLIN;
        n++;

        /* A pending `tap` needs a wake-up; otherwise wait indefinitely. */
        timeout = (taps.n > 0u) ? 5 : -1;
        if (poll(pfd, n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            logline("poll: %s", strerror(errno));
            rc = 1;
            break;
        }

        {
            nfds_t k;
            for (k = 0u; k < n; k++) {
                if ((pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                if (pfd[k].fd == listen_fd) {
                    int c = accept(listen_fd, NULL, NULL);
                    if (c < 0) {
                        logline("accept: %s", strerror(errno));
                        continue;
                    }
                    d.sock = c;
                    logline("QEMU connected");
                } else if (pfd[k].fd == d.sock) {
                    if (!handle_message(&d)) {
                        logline("QEMU disconnected");
                        (void)close(d.sock);
                        d.sock = -1;
                        d.started = false;
                        if (d.kick_fd >= 0)
                            (void)close(d.kick_fd);
                        if (d.call_fd >= 0)
                            (void)close(d.call_fd);
                        d.kick_fd = -1;
                        d.call_fd = -1;
                        unmap_regions(&d);
                    }
                } else if (pfd[k].fd == d.kick_fd && d.kick_fd >= 0) {
                    uint64_t ev;
                    if (read(d.kick_fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
                        process_queue(&d);
                } else if (pfd[k].fd == keys_fd) {
                    char buf[512];
                    ssize_t got = read(keys_fd, buf, sizeof buf - 1u);
                    if (got > 0) {
                        char *line;
                        char *save = NULL;
                        buf[got] = '\0';
                        for (line = strtok_r(buf, "\n", &save); line != NULL;
                             line = strtok_r(NULL, "\n", &save))
                            key_command(&d, line, &taps, monotonic_now());
                    } else if (got == 0 && keys_path == NULL) {
                        /* stdin closed and there is no fifo to wait on. */
                        g_stop = 1;
                    }
                }
            }
        }

        if (taps.n > 0u) {
            size_t gone = taps_expire(&d.chip, &taps, monotonic_now());
            if (gone > 0u)
                logline("%zu tap%s released, %zu shorted pair%s still down", gone,
                        gone == 1u ? "" : "s", d.chip.n_edges,
                        d.chip.n_edges == 1u ? "" : "s");
        }
        /* The guest may have queued while we were servicing a key. */
        process_queue(&d);
    }

    if (d.sock >= 0)
        (void)close(d.sock);
    unmap_regions(&d);
    (void)close(listen_fd);
    (void)unlink(sock_path);
    logline("stopped");
    return rc;
}
