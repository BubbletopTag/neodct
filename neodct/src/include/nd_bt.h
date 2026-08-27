/* nd_bt.h -- the Bluetooth adapter, straight off the kernel's HCI socket.
 *
 * ============ WHY THERE IS NO BlueZ UNDER THIS ============
 *
 * The obvious way to talk to a dongle is bluetoothd, and it is the wrong way
 * here. BR2_PACKAGE_BLUEZ5_UTILS selects dbus and libglib2, so "find out
 * whether the adapter works" would cost a message bus, a 1.5 MB object
 * library and a resident daemon on a phone with 64 MB of RAM and no
 * Bluetooth feature to justify any of it yet.
 *
 * Everything this header offers -- is there an adapter, what is its address,
 * bring it up, who is nearby -- is a kernel ioctl on an AF_BLUETOOTH socket.
 * The kernel's own BlueZ core implements them; nothing in userspace has to
 * exist. So the phone ships the kernel side (CONFIG_BT, CONFIG_BT_HCIBTUSB)
 * plus 44 KB of Realtek firmware, and this file, and that is the whole stack.
 *
 * If pairing, audio or RFCOMM is ever wanted, that argument stops holding and
 * bluetoothd becomes the right answer. It does not hold today.
 *
 * ============ THE ADDRESS IS STORED BACKWARDS ============
 *
 * A BD_ADDR crosses the HCI wire least-significant byte first, and the kernel
 * hands it over exactly as it arrived. So `addr` below is in wire order and
 * the human-readable form is its REVERSE. nd_bt_addr_str() is the only place
 * that reversal happens; everything else treats the six bytes as opaque.
 * Getting this backwards produces a plausible-looking address that no other
 * tool agrees with, which is why the unit test pins it against the real
 * UB500's address as printed by `hciconfig` and by its own USB serial number.
 *
 * ============ errno SURVIVES THESE CALLS ============
 *
 * The ioctl wrappers return nd_err like everything else, and they also leave
 * errno set the way the failing syscall left it. That is deliberate: the
 * engineering app shows strerror(errno) the way FuelGauge does, and
 * "Operation not permitted" and "Network is down" are different bring-up
 * problems that ND_ERR_IO alone cannot tell apart.
 *
 * ============ nd_bt_inquiry() BLOCKS ============
 *
 * The inquiry ioctl does not return until the controller has finished
 * scanning -- units * 1.28 seconds, and there is no way to poll it. A caller
 * with a screen must commit a "scanning" frame BEFORE calling, because it
 * will not get another chance until the scan is over. See apps/Bluetooth.
 */

#ifndef ND_BT_H_INCLUDED
#define ND_BT_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The serial-log tag. Deliberately NOT added to nd_log.h's registered list:
 * "Bluetooth" already has a colour, 72, from nd_log.c's unregistered band,
 * and test_nd_log.c has been pinning that exact value since before this
 * module existed. Registering it now would repaint a tag whose colour is
 * already an asserted fact. */
#define ND_BT_LOG_TAG "Bluetooth"

/* A BD_ADDR is six bytes; "00:11:22:33:44:55" plus a NUL is eighteen. */
#define ND_BT_ADDR_LEN 6
#define ND_BT_ADDR_STR 18

/* HCI_MAX_DEV is 16 in the kernel. A phone has one dongle and the menu shows
 * one line each, so four is already more than the screen can use; the list
 * call still asks the kernel for the full sixteen so that "you have more
 * adapters than this" is a fact it can report rather than silently lose. */
#define ND_BT_MAX_ADAPTERS 4

/* One inquiry, one screen. 240x175 shows six rows of the small font. */
#define ND_BT_MAX_DEVICES 12

/* The inquiry length is in units of 1.28 seconds; the HCI spec allows 1..48.
 * Four is 5.12 s, which is the shortest window that reliably catches a phone
 * in discoverable mode and is short enough to sit through on a dead screen. */
#define ND_BT_INQUIRY_UNITS 4

/* Local device flags -- the kernel's HCI_UP..HCI_RAW enum, as returned in
 * nd_bt_adapter.flags. Named here because the numbers are ABI: they are what
 * HCIGETDEVINFO puts in the field, and no header ships them to userspace
 * unless BlueZ is installed. */
#define ND_BT_FLAG_UP      (1u << 0)
#define ND_BT_FLAG_INIT    (1u << 1)
#define ND_BT_FLAG_RUNNING (1u << 2)
#define ND_BT_FLAG_PSCAN   (1u << 3)
#define ND_BT_FLAG_ISCAN   (1u << 4)
#define ND_BT_FLAG_AUTH    (1u << 5)
#define ND_BT_FLAG_ENCRYPT (1u << 6)
#define ND_BT_FLAG_INQUIRY (1u << 7)
#define ND_BT_FLAG_RAW     (1u << 8)

/* One local controller. A subset of the kernel's hci_dev_info: the fields a
 * technician reads off a screen, and no more. */
typedef struct {
    uint16_t id;                    /* N in hciN */
    char name[9];                   /* "hci0"; the kernel field is char[8] */
    uint8_t addr[ND_BT_ADDR_LEN];   /* BD_ADDR, WIRE ORDER -- see the header */
    uint32_t flags;                 /* ND_BT_FLAG_* */
    uint8_t bus;                    /* 1 = USB; nd_bt_bus_name() spells it */
    uint16_t acl_mtu;
    uint16_t acl_pkts;
    uint16_t sco_mtu;
    uint16_t sco_pkts;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;
} nd_bt_adapter;

/* One remote device seen by an inquiry. The inquiry ioctl carries no name and
 * no RSSI -- both need an event-driven HCI session, which is bluetoothd's job
 * -- so a scan reports the address and the class, which is enough to say "the
 * radio works and it can hear that phone over there". */
typedef struct {
    uint8_t addr[ND_BT_ADDR_LEN];
    uint32_t cod;  /* class of device, 24 bits, little-endian on the wire */
} nd_bt_device;

/* ------------------------------------------------------------------ *
 * Pure -- no kernel, testable anywhere
 * ------------------------------------------------------------------ */

/* "B8:FB:B3:9D:0D:C7", uppercase, from the six WIRE-ORDER bytes. Writes ""
 * rather than nothing when the buffer is too small, so a caller that ignores
 * the result still draws an empty field and not a stale one. */
void nd_bt_addr_str(const uint8_t *addr, char *out, size_t out_sz);

/* An adapter with no address has not been initialised by its firmware.
 * BD_ADDR 00:00:00:00:00:00 is what a Realtek part reports when the fw load
 * failed, so this is the one-line answer to "did rtl8761bu_fw.bin load". */
bool nd_bt_addr_is_zero(const uint8_t *addr);

/* "USB", "UART", "Virtual", ... for hci_dev_info.type & 0x0f. Never NULL;
 * an unrecognised bus is "Bus N". */
const char *nd_bt_bus_name(uint8_t bus);

/* The major device class of a 24-bit CoD -- "Phone", "Computer",
 * "Audio/Video" and so on. Never NULL. */
const char *nd_bt_cod_major(uint32_t cod);

/* "UP RUNNING PSCAN", space separated, in flag order; "DOWN" when the UP bit
 * is clear, because that is the fact a technician is looking for and a list
 * of everything else that is also false is not. */
void nd_bt_flags_str(uint32_t flags, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * The kernel
 * ------------------------------------------------------------------ */

/* True when this kernel has the Bluetooth core at all: socket(AF_BLUETOOTH)
 * succeeds. False means CONFIG_BT is off, which is a different problem from
 * having no dongle plugged in and has to be said differently. */
bool nd_bt_available(void);

/* Every local controller the kernel knows, whether up or down. `*n_out` is
 * how many were written, never more than `max`. ND_OK with *n_out == 0 is the
 * normal "nothing plugged in" answer, NOT an error. ND_ERR_HARDWARE when the
 * kernel has no Bluetooth core. */
nd_err nd_bt_list(nd_bt_adapter *out, size_t max, size_t *n_out);

/* One controller by id. ND_ERR_NOTFOUND when there is no hciN. */
nd_err nd_bt_info(uint16_t dev_id, nd_bt_adapter *out);

/* HCIDEVUP / HCIDEVDOWN. Needs CAP_NET_ADMIN, which an app has on the phone
 * (everything runs as root) and does not have on a developer's desktop --
 * errno is EPERM there, and the app says so rather than pretending. */
nd_err nd_bt_power(uint16_t dev_id, bool up);

/* A general inquiry: `units` * 1.28 seconds of scanning, then the cache
 * contents. BLOCKS for that whole time. `units` is clamped to 1..48.
 *
 * ND_ERR_HARDWARE with errno ENETDOWN when the adapter is not up -- the
 * commonest failure, and the one worth naming, because it means the firmware
 * did not load. */
nd_err nd_bt_inquiry(uint16_t dev_id, uint8_t units, nd_bt_device *out, size_t max,
                     size_t *n_out);

#ifdef __cplusplus
}
#endif

#endif /* ND_BT_H_INCLUDED */
