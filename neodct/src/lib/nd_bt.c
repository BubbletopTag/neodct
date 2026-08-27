/* nd_bt.c -- the kernel's Bluetooth core, reached by ioctl and nothing else.
 *
 * Why there is no BlueZ under this, and why the address is stored backwards,
 * are both in nd_bt.h. What is here is the part that has to be written down
 * beside the code: the ABI.
 *
 * ============ THESE STRUCTS ARE COPIED, NOT INCLUDED ============
 *
 * `struct hci_dev_info` and friends live in the kernel's
 * include/net/bluetooth/hci_sock.h, which is NOT a UAPI header -- nothing
 * installs it. The only userspace copy ships with bluez-libs, and depending
 * on bluez-libs to ask "is there an adapter" is the dependency this whole
 * module exists to avoid.
 *
 * So they are declared again here, and their correctness is a fact about the
 * layout rather than a matter of opinion. Every one was checked against
 * linux-6.12.47's own declaration AND measured on the machine:
 *
 *     sizeof(hci_dev_info)      92     name @2  bdaddr @10  flags @16
 *                                      type @20  features @21  pkt_type @32
 *     sizeof(hci_dev_req)        8
 *     sizeof(hci_inquiry_req)   10
 *     sizeof(inquiry_info)      14     packed -- the entries in the reply are
 *                                      NOT aligned, hence the memcpy below
 *
 * Nothing in them is a pointer or a `long`, so the layout is identical on
 * 32-bit ARM and on the x86-64 the tests run on -- which is why the same
 * numbers hold for the phone and for `make test`.
 *
 * The ioctl numbers are COMPUTED with _IOR/_IOW rather than pasted, so the
 * encoding comes from the running architecture's own macros. They agree with
 * bluez: HCIGETDEVLIST is 0x800448d2 on both.
 *
 * ============ WHY A NEW SOCKET PER CALL ============
 *
 * Every entry point opens an AF_BLUETOOTH socket, does its ioctl and closes
 * it. The socket carries no state that outlives the call -- it is a handle on
 * the subsystem, not on a device -- and an app that asks about the adapter
 * once a second would otherwise hold a descriptor open for a whole session
 * for no reason. The cost is one socket() per call, which is nothing beside a
 * 5-second inquiry.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nd_bt.h"
#include "nd_log.h"
#include "nd_types.h"

/* musl and glibc both define these; the guard is for a toolchain whose
 * headers predate the protocol, where the numbers are still ABI. */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define ND_BTPROTO_HCI 1

/* The kernel's HCI ioctls, from include/net/bluetooth/hci_sock.h. */
#define ND_HCIDEVUP      _IOW('H', 201, int)
#define ND_HCIDEVDOWN    _IOW('H', 202, int)
#define ND_HCIGETDEVLIST _IOR('H', 210, int)
#define ND_HCIGETDEVINFO _IOR('H', 211, int)
#define ND_HCIINQUIRY    _IOR('H', 240, int)

#define ND_IREQ_CACHE_FLUSH 0x0001u

/* HCI_MAX_DEV. The list ioctl is asked for all of them even though only
 * ND_BT_MAX_ADAPTERS are returned, so "there are more than you can see" is a
 * fact the caller could report rather than one the ioctl hid. */
#define ND_HCI_MAX_DEV 16

/* hci_dev_info.type is the bus in its low nibble; the high nibble is
 * HCI_PRIMARY/HCI_AMP, which no longer varies and is not shown. */
#define ND_HCI_BUS_MASK 0x0Fu

struct nd_hci_dev_req {
    uint16_t dev_id;
    uint32_t dev_opt;
};

/* The kernel's declaration ends in a flexible array; a fixed sixteen is the
 * same bytes and keeps the whole request on the stack. sizeof is 4 + 16*8. */
struct nd_hci_dev_list_req {
    uint16_t dev_num;
    struct nd_hci_dev_req dev_req[ND_HCI_MAX_DEV];
};

struct nd_hci_dev_stats {
    uint32_t err_rx;
    uint32_t err_tx;
    uint32_t cmd_tx;
    uint32_t evt_rx;
    uint32_t acl_tx;
    uint32_t acl_rx;
    uint32_t sco_tx;
    uint32_t sco_rx;
    uint32_t byte_rx;
    uint32_t byte_tx;
};

struct nd_hci_dev_info {
    uint16_t dev_id;
    char name[8];

    uint8_t bdaddr[ND_BT_ADDR_LEN];

    uint32_t flags;
    uint8_t type;

    uint8_t features[8];

    uint32_t pkt_type;
    uint32_t link_policy;
    uint32_t link_mode;

    uint16_t acl_mtu;
    uint16_t acl_pkts;
    uint16_t sco_mtu;
    uint16_t sco_pkts;

    struct nd_hci_dev_stats stat;
};

struct nd_hci_inquiry_req {
    uint16_t dev_id;
    uint16_t flags;
    uint8_t lap[3];
    uint8_t length;
    uint8_t num_rsp;
};

/* The reply entries follow the request in the SAME buffer, packed, so entry N
 * starts at an offset that is not a multiple of 2. Read with memcpy; a cast
 * would fault on ARM (CODING-STANDARDS.md 6). */
#define ND_INQUIRY_INFO_SIZE 14u

/* The General/Unlimited Inquiry Access Code, 0x9E8B33, least significant byte
 * first like everything else on this wire. */
static const uint8_t ND_GIAC[3] = {0x33u, 0x8Bu, 0x9Eu};

/* ------------------------------------------------------------------ *
 * Pure
 * ------------------------------------------------------------------ */

void nd_bt_addr_str(const uint8_t *addr, char *out, size_t out_sz)
{
    int n;

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if (addr == NULL || out_sz < (size_t)ND_BT_ADDR_STR)
        return;
    n = snprintf(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2],
                 addr[1], addr[0]);
    if (n < 0 || (size_t)n >= out_sz)
        out[0] = '\0';
}

bool nd_bt_addr_is_zero(const uint8_t *addr)
{
    size_t i;

    if (addr == NULL)
        return false;
    for (i = 0u; i < (size_t)ND_BT_ADDR_LEN; i++) {
        if (addr[i] != 0u)
            return false;
    }
    return true;
}

const char *nd_bt_bus_name(uint8_t bus)
{
    /* HCI_VIRTUAL..HCI_VIRTIO, in the kernel's order. */
    static const char *const names[] = {"Virtual", "USB",  "PCCARD", "UART",  "RS232", "PCI",
                                        "SDIO",    "SPI",  "I2C",    "SMD",   "VIRTIO"};
    /* A bus the kernel grew after this was written keeps its number, because
     * the number is what to look up. Not reentrant, and deliberately so: the
     * one caller draws the string immediately. */
    static char other[16];

    if ((size_t)bus < ND_ARRAY_LEN(names))
        return names[bus];
    (void)snprintf(other, sizeof other, "Bus %u", (unsigned)bus);
    return other;
}

const char *nd_bt_cod_major(uint32_t cod)
{
    /* Bits 8..12 of the 24-bit class of device: Bluetooth Assigned Numbers,
     * "Major Device Class". 0x1F is the spec's own "uncategorized". */
    static const char *const majors[] = {
        "Miscellaneous", "Computer", "Phone",    "Network", "Audio/Video",
        "Peripheral",    "Imaging",  "Wearable", "Toy",     "Health",
    };
    uint32_t major = (cod >> 8) & 0x1Fu;

    if (major == 0x1Fu)
        return "Uncategorized";
    if (major < ND_ARRAY_LEN(majors))
        return majors[major];
    return "Reserved";
}

void nd_bt_flags_str(uint32_t flags, char *out, size_t out_sz)
{
    static const struct {
        uint32_t bit;
        const char *word;
    } words[] = {
        {ND_BT_FLAG_UP, "UP"},           {ND_BT_FLAG_INIT, "INIT"},
        {ND_BT_FLAG_RUNNING, "RUNNING"}, {ND_BT_FLAG_PSCAN, "PSCAN"},
        {ND_BT_FLAG_ISCAN, "ISCAN"},     {ND_BT_FLAG_AUTH, "AUTH"},
        {ND_BT_FLAG_ENCRYPT, "ENCRYPT"}, {ND_BT_FLAG_INQUIRY, "INQUIRY"},
        {ND_BT_FLAG_RAW, "RAW"},
    };
    size_t i;

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    /* An adapter that is not up is DOWN and nothing else. Listing the eight
     * other things it also is not would bury the one fact being looked for. */
    if ((flags & ND_BT_FLAG_UP) == 0u) {
        (void)nd_strlcpy(out, "DOWN", out_sz);
        return;
    }
    for (i = 0u; i < ND_ARRAY_LEN(words); i++) {
        if ((flags & words[i].bit) == 0u)
            continue;
        if (out[0] != '\0')
            (void)nd_strlcat(out, " ", out_sz);
        (void)nd_strlcat(out, words[i].word, out_sz);
    }
}

/* ------------------------------------------------------------------ *
 * The kernel
 * ------------------------------------------------------------------ */

/* ============ ioctl's SECOND ARGUMENT HAS TWO TYPES ============
 *
 * musl declares `int ioctl(int, int, ...)`; glibc declares
 * `int ioctl(int, unsigned long, ...)`. Three of the requests above have bit
 * 31 set, because _IOR puts _IOC_READ there -- HCIGETDEVLIST is 0x800448D2
 * and HCIINQUIRY is 0x800448F0.
 *
 * So there is no single type for the constant that is silent under
 * -Wconversion on both: as an unsigned int it is a sign CHANGE on musl, and
 * as an int it is a sign EXTENSION on glibc. The phone is musl and the host
 * tests are glibc, so both have to be right.
 *
 * One wrapper with one explicit cast each way. The bits the kernel sees are
 * identical: sys_ioctl takes the request as `unsigned int`, so 0x800448F0 and
 * (int)0x800448F0 arrive as the same thirty-two bits.
 */
#if defined(__GLIBC__)
typedef unsigned long nd_ioctl_req;
#else
typedef int nd_ioctl_req;
#endif

/* The requests that take a pointer: everything except HCIDEVUP/HCIDEVDOWN. */
static int bt_ioctl_ptr(int fd, unsigned long req, void *arg)
{
    return ioctl(fd, (nd_ioctl_req)req, arg);
}

/* HCIDEVUP and HCIDEVDOWN take the device id BY VALUE -- hci_dev_open(arg)
 * casts the argument straight to an index rather than dereferencing it. */
static int bt_ioctl_val(int fd, unsigned long req, unsigned long arg)
{
    return ioctl(fd, (nd_ioctl_req)req, arg);
}

/* -1 with errno set, and errno is the answer: EAFNOSUPPORT means the kernel
 * was built without CONFIG_BT. */
static int bt_socket(void)
{
    return socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, ND_BTPROTO_HCI);
}

bool nd_bt_available(void)
{
    int fd = bt_socket();

    if (fd < 0)
        return false;
    (void)close(fd);
    return true;
}

/* ENODEV from any of these ioctls means "no hciN", which is a different thing
 * from "the ioctl failed" and the app words it differently. */
static nd_err errno_to_err(void)
{
    switch (errno) {
    case ENODEV:
        return ND_ERR_NOTFOUND;
    case EPERM:
    case EACCES:
        return ND_ERR_IO;
    default:
        return ND_ERR_HARDWARE;
    }
}

static void fill_adapter(nd_bt_adapter *out, const struct nd_hci_dev_info *di)
{
    memset(out, 0, sizeof *out);
    out->id = di->dev_id;
    /* The kernel's field is char[8] and is NOT guaranteed to be terminated;
     * copying 8 into a 9-byte buffer that starts zeroed always is. */
    memcpy(out->name, di->name, sizeof di->name);
    memcpy(out->addr, di->bdaddr, (size_t)ND_BT_ADDR_LEN);
    out->flags = di->flags;
    out->bus = (uint8_t)(di->type & ND_HCI_BUS_MASK);
    out->acl_mtu = di->acl_mtu;
    out->acl_pkts = di->acl_pkts;
    out->sco_mtu = di->sco_mtu;
    out->sco_pkts = di->sco_pkts;
    out->rx_bytes = di->stat.byte_rx;
    out->tx_bytes = di->stat.byte_tx;
    out->rx_errors = di->stat.err_rx;
    out->tx_errors = di->stat.err_tx;
}

static nd_err info_on_fd(int fd, uint16_t dev_id, nd_bt_adapter *out)
{
    struct nd_hci_dev_info di;

    memset(&di, 0, sizeof di);
    di.dev_id = dev_id;
    if (bt_ioctl_ptr(fd, ND_HCIGETDEVINFO, &di) < 0)
        return errno_to_err();
    fill_adapter(out, &di);
    return ND_OK;
}

nd_err nd_bt_list(nd_bt_adapter *out, size_t max, size_t *n_out)
{
    struct nd_hci_dev_list_req dl;
    nd_err rc = ND_OK;
    size_t written = 0u;
    size_t i;
    int fd;

    if (n_out != NULL)
        *n_out = 0u;
    if (out == NULL || max == 0u)
        return ND_ERR_INVAL;

    fd = bt_socket();
    if (fd < 0) {
        nd_log_err(ND_BT_LOG_TAG, "no bluetooth in this kernel: %s", strerror(errno));
        return ND_ERR_HARDWARE;
    }

    memset(&dl, 0, sizeof dl);
    dl.dev_num = (uint16_t)ND_HCI_MAX_DEV;
    if (bt_ioctl_ptr(fd, ND_HCIGETDEVLIST, &dl) < 0) {
        nd_log_err(ND_BT_LOG_TAG, "HCIGETDEVLIST: %s", strerror(errno));
        rc = errno_to_err();
        goto done;
    }

    for (i = 0u; i < (size_t)dl.dev_num && written < max; i++) {
        /* An adapter can be unplugged between the list and the info, which is
         * ND_ERR_NOTFOUND and not a reason to abandon the ones that are still
         * there. */
        if (info_on_fd(fd, dl.dev_req[i].dev_id, &out[written]) == ND_OK)
            written++;
    }
    if (n_out != NULL)
        *n_out = written;
done:
    (void)close(fd);
    return rc;
}

nd_err nd_bt_info(uint16_t dev_id, nd_bt_adapter *out)
{
    nd_err rc;
    int fd;

    if (out == NULL)
        return ND_ERR_INVAL;

    fd = bt_socket();
    if (fd < 0)
        return ND_ERR_HARDWARE;
    rc = info_on_fd(fd, dev_id, out);
    (void)close(fd);
    return rc;
}

nd_err nd_bt_power(uint16_t dev_id, bool up)
{
    nd_err rc = ND_OK;
    int fd = bt_socket();

    if (fd < 0)
        return ND_ERR_HARDWARE;
    if (bt_ioctl_val(fd, up ? ND_HCIDEVUP : ND_HCIDEVDOWN, (unsigned long)dev_id) < 0) {
        /* EALREADY is the kernel saying it is already in the state asked for,
         * which is what the caller wanted and not a failure. */
        if (errno == EALREADY)
            rc = ND_OK;
        else {
            nd_log_err(ND_BT_LOG_TAG, "hci%u %s: %s", (unsigned)dev_id, up ? "up" : "down",
                       strerror(errno));
            rc = errno_to_err();
        }
    }
    (void)close(fd);
    return rc;
}

nd_err nd_bt_inquiry(uint16_t dev_id, uint8_t units, nd_bt_device *out, size_t max, size_t *n_out)
{
    /* Request header followed by up to `max` packed 14-byte replies, in ONE
     * buffer, because that is the shape the ioctl reads and writes. Sized for
     * ND_BT_MAX_DEVICES so nothing here is sized by input. */
    uint8_t buf[sizeof(struct nd_hci_inquiry_req) + (ND_BT_MAX_DEVICES * ND_INQUIRY_INFO_SIZE)];
    struct nd_hci_inquiry_req ir;
    nd_err rc = ND_OK;
    size_t want;
    size_t i;
    int fd;

    if (n_out != NULL)
        *n_out = 0u;
    if (out == NULL || max == 0u)
        return ND_ERR_INVAL;

    want = max < (size_t)ND_BT_MAX_DEVICES ? max : (size_t)ND_BT_MAX_DEVICES;

    /* The spec allows 1..0x30 units of 1.28 s; the kernel refuses over 60
     * with EINVAL. Clamping here means a caller cannot turn a typo into a
     * minute of dead screen. */
    if (units == 0u)
        units = 1u;
    if (units > 0x30u)
        units = 0x30u;

    memset(&ir, 0, sizeof ir);
    ir.dev_id = dev_id;
    /* IREQ_CACHE_FLUSH: without it the kernel answers out of an inquiry cache
     * that can be a minute old, and "scan" on an engineering screen has to
     * mean the radio just transmitted, not that it remembers transmitting. */
    ir.flags = (uint16_t)ND_IREQ_CACHE_FLUSH;
    memcpy(ir.lap, ND_GIAC, sizeof ir.lap);
    ir.length = units;
    ir.num_rsp = (uint8_t)want;

    memset(buf, 0, sizeof buf);
    memcpy(buf, &ir, sizeof ir);

    fd = bt_socket();
    if (fd < 0)
        return ND_ERR_HARDWARE;

    /* BLOCKS for units * 1.28 s. A SIGTERM lands as EINTR, because the kernel
     * waits on the HCI_INQUIRY bit with TASK_INTERRUPTIBLE and nd-apprun
     * installs its handler without SA_RESTART -- which is what lets an app
     * mid-scan still honour the teardown contract when a call comes in. */
    if (bt_ioctl_ptr(fd, ND_HCIINQUIRY, buf) < 0) {
        nd_log_err(ND_BT_LOG_TAG, "inquiry on hci%u: %s", (unsigned)dev_id, strerror(errno));
        rc = errno_to_err();
        goto done;
    }

    /* The kernel wrote the count back into the request it was given. */
    memcpy(&ir, buf, sizeof ir);
    if ((size_t)ir.num_rsp < want)
        want = ir.num_rsp;

    for (i = 0u; i < want; i++) {
        const uint8_t *e = buf + sizeof ir + (i * ND_INQUIRY_INFO_SIZE);

        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].addr, e, (size_t)ND_BT_ADDR_LEN);
        /* dev_class is three bytes at offset 9, least significant first. Read
         * byte by byte: no endianness assumption, no unaligned load. */
        out[i].cod = (uint32_t)e[9] | ((uint32_t)e[10] << 8) | ((uint32_t)e[11] << 16);
    }
    if (n_out != NULL)
        *n_out = want;
done:
    (void)close(fd);
    return rc;
}
