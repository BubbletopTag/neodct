/* test_bt.c -- the HCI layer, against the kernel that is actually running.
 *
 * ============ HALF OF THIS TEST TALKS TO REAL HARDWARE ============
 *
 * lib/nd_bt.c is ioctls on an AF_BLUETOOTH socket, and there is no seam to
 * inject: the kernel either has a controller or it does not. So the live
 * half of this file is written to be TRUE EITHER WAY.
 *
 *   * /sys/class/bluetooth is the independent oracle. Whatever the kernel
 *     lists there, nd_bt_list() must report the same count -- zero on a build
 *     machine with no dongle, one on the developer's desk with the UB500
 *     plugged in. A test that only asserted "> 0" would pass by accident on
 *     one machine and fail the build on the next.
 *
 *   * Whatever it does find, it then checks properly: the name is hciN for
 *     the id it reported, and the address is not all zeros, which is the one
 *     thing that says the controller's firmware actually loaded.
 *
 * That is deliberately not a skip. A skip on the machine that HAS the
 * hardware is how a Bluetooth stack ships broken.
 *
 * ============ WHAT IS NOT EXERCISED HERE ============
 *
 * nd_bt_power() up and nd_bt_inquiry() on a live adapter both need
 * CAP_NET_ADMIN and neither can run in `make test`, which is unprivileged.
 * Their failure paths are tested instead -- a device id that cannot exist --
 * because an engineering app is judged on what it says when things go wrong,
 * and "hciconfig hci0 up" as root is what tests/hw is for.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "nd_bt.h"
#include "nd_types.h"

#include "platform_test.h"

/* ------------------------------------------------------------------ *
 * Addresses
 * ------------------------------------------------------------------ */

/* The bytes the kernel hands over for the TP-Link UB500 on this desk. The
 * printed form is their reverse, and it agrees with two independent things:
 * `hciconfig -a`, and the dongle's own USB iSerial, B8FBB39D0DC7. */
static const uint8_t UB500[ND_BT_ADDR_LEN] = {0xC7, 0x0D, 0x9D, 0xB3, 0xFB, 0xB8};

static void test_an_address_prints_in_reverse_of_wire_order(void)
{
    char s[ND_BT_ADDR_STR];

    nd_bt_addr_str(UB500, s, sizeof s);

    CHECK_STR(s, "B8:FB:B3:9D:0D:C7");
}

static void test_an_address_that_does_not_fit_gives_an_empty_string(void)
{
    char s[8];

    (void)nd_strlcpy(s, "STALE", sizeof s);
    nd_bt_addr_str(UB500, s, sizeof s);

    CHECK_STR(s, "");
}

static void test_the_all_zero_address_means_the_firmware_did_not_load(void)
{
    static const uint8_t zero[ND_BT_ADDR_LEN] = {0, 0, 0, 0, 0, 0};

    CHECK(nd_bt_addr_is_zero(zero));
    CHECK(!nd_bt_addr_is_zero(UB500));
}

/* ------------------------------------------------------------------ *
 * Naming the numbers
 * ------------------------------------------------------------------ */

static void test_the_bus_is_named(void)
{
    CHECK_STR(nd_bt_bus_name(1), "USB");
    CHECK_STR(nd_bt_bus_name(0), "Virtual");
    CHECK_STR(nd_bt_bus_name(3), "UART");
    /* Unrecognised buses keep their number rather than becoming "Unknown":
     * the number is what to look up, and hiding it helps nobody. */
    CHECK_STR(nd_bt_bus_name(11), "Bus 11");
}

/* Major device class is bits 8..12 of the 24-bit CoD. The three values below
 * are real: a smartphone, a pair of headphones and a laptop. */
static void test_the_class_of_device_names_the_major_class(void)
{
    CHECK_STR(nd_bt_cod_major(0x5A020Cu), "Phone");
    CHECK_STR(nd_bt_cod_major(0x240404u), "Audio/Video");
    CHECK_STR(nd_bt_cod_major(0x00010Cu), "Computer");
    CHECK_STR(nd_bt_cod_major(0x000000u), "Miscellaneous");
    /* 0x1F is the spec's "uncategorized", and it is what a device that has
     * not been configured reports. */
    CHECK_STR(nd_bt_cod_major(0x001F00u), "Uncategorized");
}

static void test_flags_read_as_words(void)
{
    char s[64];

    nd_bt_flags_str(ND_BT_FLAG_UP | ND_BT_FLAG_RUNNING | ND_BT_FLAG_PSCAN, s, sizeof s);
    CHECK_STR(s, "UP RUNNING PSCAN");

    /* An adapter that is registered but not brought up is the state a phone
     * boots into, and "DOWN" is the whole answer. */
    nd_bt_flags_str(0u, s, sizeof s);
    CHECK_STR(s, "DOWN");

    nd_bt_flags_str(ND_BT_FLAG_UP | ND_BT_FLAG_RUNNING | ND_BT_FLAG_INQUIRY, s, sizeof s);
    CHECK_STR(s, "UP RUNNING INQUIRY");
}

/* ------------------------------------------------------------------ *
 * The kernel that is running right now
 * ------------------------------------------------------------------ */

/* The independent oracle: hciN directories under /sys/class/bluetooth. Read
 * with opendir and not through nd_path_resolve, because this is a real kernel
 * path and NEODCT_ROOT must not redirect it. */
static size_t sysfs_adapter_count(void)
{
    DIR *d = opendir("/sys/class/bluetooth");
    struct dirent *e;
    size_t n = 0u;

    if (d == NULL)
        return 0u;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hci", 3u) == 0)
            n++;
    }
    (void)closedir(d);
    return n;
}

static void test_the_adapter_list_agrees_with_sysfs(void)
{
    nd_bt_adapter found[ND_BT_MAX_ADAPTERS];
    size_t want = sysfs_adapter_count();
    size_t n = 99u;

    if (!nd_bt_available()) {
        /* No CONFIG_BT. Then sysfs has no adapters either, and the list call
         * must say ND_ERR_HARDWARE rather than "no dongle" -- they are
         * different bring-up problems. */
        CHECK_INT(want, 0);
        CHECK_INT(nd_bt_list(found, ND_ARRAY_LEN(found), &n), ND_ERR_HARDWARE);
        return;
    }

    CHECK_INT(nd_bt_list(found, ND_ARRAY_LEN(found), &n), ND_OK);
    if (want > ND_BT_MAX_ADAPTERS)
        want = ND_BT_MAX_ADAPTERS;
    CHECK_INT(n, want);
    printf("  nd_bt_list: %zu adapter(s), sysfs says %zu\n", n, want);
}

/* Whatever was found is then checked for real. This is the assertion that
 * would have caught a firmware that did not load. */
static void test_a_real_adapter_has_a_name_and_an_address(void)
{
    nd_bt_adapter found[ND_BT_MAX_ADAPTERS];
    size_t n = 0u;
    size_t i;

    if (!nd_bt_available() || nd_bt_list(found, ND_ARRAY_LEN(found), &n) != ND_OK || n == 0u) {
        printf("  no controller on this machine; the live checks have nothing to judge\n");
        return;
    }

    for (i = 0u; i < n; i++) {
        char want_name[16];
        char addr[ND_BT_ADDR_STR];
        char flags[64];

        (void)snprintf(want_name, sizeof want_name, "hci%u", (unsigned)found[i].id);
        CHECK_STR(found[i].name, want_name);
        CHECK(!nd_bt_addr_is_zero(found[i].addr));

        nd_bt_addr_str(found[i].addr, addr, sizeof addr);
        nd_bt_flags_str(found[i].flags, flags, sizeof flags);
        printf("  %s  %s  %s  %s\n", found[i].name, addr, nd_bt_bus_name(found[i].bus), flags);
    }
}

/* nd_bt_info() of the same adapter must say the same thing the list did --
 * the app reads one row from the list and then refreshes it by id. */
static void test_info_by_id_matches_the_list(void)
{
    nd_bt_adapter found[ND_BT_MAX_ADAPTERS];
    nd_bt_adapter one;
    size_t n = 0u;

    if (!nd_bt_available() || nd_bt_list(found, ND_ARRAY_LEN(found), &n) != ND_OK || n == 0u)
        return;

    memset(&one, 0, sizeof one);
    CHECK_INT(nd_bt_info(found[0].id, &one), ND_OK);
    CHECK_INT(one.id, found[0].id);
    CHECK_STR(one.name, found[0].name);
    CHECK(memcmp(one.addr, found[0].addr, ND_BT_ADDR_LEN) == 0);
}

/* ------------------------------------------------------------------ *
 * The failure paths, which is what an engineering app spends its life on
 * ------------------------------------------------------------------ */

/* 250 cannot exist: HCI_MAX_DEV is 16. The kernel answers ENODEV and the
 * wrapper must turn that into NOTFOUND rather than a generic IO error, so the
 * app can say "no such adapter" instead of "ioctl failed". */
static void test_a_device_id_that_cannot_exist_is_not_found(void)
{
    nd_bt_adapter one;

    if (!nd_bt_available())
        return;
    CHECK_INT(nd_bt_info(250u, &one), ND_ERR_NOTFOUND);
}

static void test_powering_a_device_that_does_not_exist_fails_with_errno_set(void)
{
    if (!nd_bt_available())
        return;
    errno = 0;
    CHECK(nd_bt_power(250u, true) != ND_OK);
    /* errno survives the wrapper -- the app prints strerror(errno) and the
     * difference between ENODEV and EPERM is the difference between "not
     * plugged in" and "not root". */
    CHECK(errno != 0);
}

/* The inquiry never runs against a live adapter here: it would block for over
 * five seconds and needs CAP_NET_ADMIN anyway. Against a device that does not
 * exist it must return promptly with nothing written. */
static void test_an_inquiry_on_a_missing_adapter_returns_no_devices(void)
{
    nd_bt_device seen[ND_BT_MAX_DEVICES];
    size_t n = 99u;

    if (!nd_bt_available())
        return;
    CHECK(nd_bt_inquiry(250u, ND_BT_INQUIRY_UNITS, seen, ND_ARRAY_LEN(seen), &n) != ND_OK);
    CHECK_INT(n, 0);
}

static void test_nothing_faults_on_a_null_argument(void)
{
    char s[ND_BT_ADDR_STR];
    size_t n = 99u;

    nd_bt_addr_str(NULL, s, sizeof s);
    CHECK_STR(s, "");
    nd_bt_addr_str(UB500, NULL, sizeof s);
    nd_bt_flags_str(0u, NULL, 8u);
    CHECK(!nd_bt_addr_is_zero(NULL));
    CHECK_INT(nd_bt_list(NULL, 4u, &n), ND_ERR_INVAL);
    CHECK_INT(nd_bt_info(0u, NULL), ND_ERR_INVAL);
    CHECK_INT(nd_bt_inquiry(0u, 4u, NULL, 4u, &n), ND_ERR_INVAL);
}

int main(void)
{
    RUN(test_an_address_prints_in_reverse_of_wire_order);
    RUN(test_an_address_that_does_not_fit_gives_an_empty_string);
    RUN(test_the_all_zero_address_means_the_firmware_did_not_load);
    RUN(test_the_bus_is_named);
    RUN(test_the_class_of_device_names_the_major_class);
    RUN(test_flags_read_as_words);
    RUN(test_the_adapter_list_agrees_with_sysfs);
    RUN(test_a_real_adapter_has_a_name_and_an_address);
    RUN(test_info_by_id_matches_the_list);
    RUN(test_a_device_id_that_cannot_exist_is_not_found);
    RUN(test_powering_a_device_that_does_not_exist_fails_with_errno_set);
    RUN(test_an_inquiry_on_a_missing_adapter_returns_no_devices);
    RUN(test_nothing_faults_on_a_null_argument);
    return pt_report("test_bt");
}
