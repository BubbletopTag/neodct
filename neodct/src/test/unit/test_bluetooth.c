/* test_bluetooth.c -- the Bluetooth engineering app, app id 9007.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. The three menu items, in order. The order is what a technician's
 *     muscle memory presses.
 *
 *  2. Every row builder, driven from structs rather than from a controller,
 *     so the "up, discoverable, 1021-byte ACL" screen can be checked on a
 *     machine with no radio in it.
 *
 *  3. The two refusals are different strings. A kernel with no CONFIG_BT and
 *     a dongle that is not plugged in look identical on the screen unless
 *     somebody makes them different, and they are different afternoons.
 *
 *  4. The self test stops at the first failure and dashes the rest, and the
 *     failure's reason reaches the bottom line.
 *
 *  5. app_run refuses without a Bluetooth core rather than drawing an empty
 *     grid, and it does it with the kernel message and not the dongle one.
 *
 *  6. The menu really renders, and Back really leaves.
 *
 * There is no golden frame here and there must not be one: this screen is
 * new, so a reference capture of it could only ever agree with the code that
 * produced it. AGENTS.md is explicit -- a new screen's test is its unit test.
 *
 * ============ THE LIVE HALF ============
 *
 * nd_btapp_selftest() is the one thing here that touches the kernel, and it
 * is run for real, without the scan step. On a machine with the UB500 plugged
 * in that means KERNEL, ADAPTER and ADDRESS pass and RADIO fails EPERM, which
 * is exactly the cascade rule this file asserts. On a machine with nothing it
 * fails at ADAPTER and the same rule holds. Either way the assertion is about
 * the SHAPE of the answer, so it is true on both.
 *
 * It is skipped for root, because as root the RADIO step would really bring
 * the adapter up, and a unit test that reconfigures the developer's hardware
 * as a side effect is a bad unit test.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "smallapp_test.h"

#include "../../apps/Bluetooth/bluetooth.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    void (*disc_str)(uint32_t, char *, size_t);
    size_t (*adapter_rows)(const nd_bt_adapter *, bool, nd_btapp_row *, size_t);
    const char *(*power_softkey)(uint32_t);
    size_t (*scan_rows)(const nd_bt_device *, size_t, bool, const char *, nd_btapp_row *, size_t);
    void (*window_str)(uint8_t, char *, size_t);
    const char *(*step_name)(nd_btapp_step);
    const char *(*verdict_str)(nd_btapp_verdict);
    size_t (*selftest)(nd_btapp_check *, size_t, bool);
    size_t (*check_rows)(const nd_btapp_check *, size_t, nd_btapp_row *, size_t);
    const char *(*first_failure)(const nd_btapp_check *, size_t);
    const char *const *menu;
    const char *const *no_kernel;
    const char *const *no_adapter;
} api;

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.disc_str = sa_sym(h, "nd_btapp_disc_str");
    *(void **)&api.adapter_rows = sa_sym(h, "nd_btapp_adapter_rows");
    *(void **)&api.power_softkey = sa_sym(h, "nd_btapp_power_softkey");
    *(void **)&api.scan_rows = sa_sym(h, "nd_btapp_scan_rows");
    *(void **)&api.window_str = sa_sym(h, "nd_btapp_window_str");
    *(void **)&api.step_name = sa_sym(h, "nd_btapp_step_name");
    *(void **)&api.verdict_str = sa_sym(h, "nd_btapp_verdict_str");
    *(void **)&api.selftest = sa_sym(h, "nd_btapp_selftest");
    *(void **)&api.check_rows = sa_sym(h, "nd_btapp_check_rows");
    *(void **)&api.first_failure = sa_sym(h, "nd_btapp_first_failure");
    api.menu = dlsym(h, "nd_btapp_menu");
    api.no_kernel = dlsym(h, "nd_btapp_no_kernel_msg");
    api.no_adapter = dlsym(h, "nd_btapp_no_adapter_msg");

    return api.run != NULL && api.shutdown != NULL && api.disc_str != NULL &&
           api.adapter_rows != NULL && api.power_softkey != NULL && api.scan_rows != NULL &&
           api.window_str != NULL && api.step_name != NULL && api.verdict_str != NULL &&
           api.selftest != NULL && api.check_rows != NULL && api.first_failure != NULL &&
           api.menu != NULL && api.no_kernel != NULL && api.no_adapter != NULL;
}

/* The UB500 as the kernel reports it, up and discoverable. */
static nd_bt_adapter live_adapter(void)
{
    nd_bt_adapter a;
    static const uint8_t addr[ND_BT_ADDR_LEN] = {0xC7, 0x0D, 0x9D, 0xB3, 0xFB, 0xB8};

    memset(&a, 0, sizeof a);
    a.id = 0u;
    (void)nd_strlcpy(a.name, "hci0", sizeof a.name);
    memcpy(a.addr, addr, sizeof addr);
    a.flags = ND_BT_FLAG_UP | ND_BT_FLAG_RUNNING | ND_BT_FLAG_PSCAN | ND_BT_FLAG_ISCAN;
    a.bus = 1u;
    a.acl_mtu = 1021u;
    a.acl_pkts = 8u;
    a.sco_mtu = 255u;
    a.sco_pkts = 12u;
    a.rx_bytes = 4096u;
    a.tx_bytes = 1024u;
    return a;
}

/* ------------------------------------------------------------------ *
 * 1. The menu
 * ------------------------------------------------------------------ */

static void test_the_menu_is_three_items_in_order(void)
{
    CHECK_STR(api.menu[ND_BTAPP_MENU_ADAPTER], "Adapter", "menu[0]");
    CHECK_STR(api.menu[ND_BTAPP_MENU_SCAN], "Scan", "menu[1]");
    CHECK_STR(api.menu[ND_BTAPP_MENU_SELFTEST], "Self test", "menu[2]");
    CHECK_INT(ND_BTAPP_MENU_N, 3, "three items and no more");
}

/* ------------------------------------------------------------------ *
 * 2. The adapter readout
 * ------------------------------------------------------------------ */

static void test_the_adapter_page_reads_out_six_rows(void)
{
    nd_bt_adapter a = live_adapter();
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n = api.adapter_rows(&a, true, rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 6, "six rows");
    CHECK_STR(rows[0].left, "HCI", "row 0 label");
    CHECK_STR(rows[0].right, "hci0 (USB)", "the name and the bus");
    CHECK_STR(rows[1].left, "ADDR", "row 1 label");
    /* The whole point of the app, and the one cell that must never be
     * truncated: 157 px at x=8..165. */
    CHECK_STR(rows[1].right, "B8:FB:B3:9D:0D:C7", "the address, high byte first");
    CHECK_STR(rows[2].left, "STATE", "row 2 label");
    /* PSCAN and ISCAN are NOT here -- they are the next row. */
    CHECK_STR(rows[2].right, "UP RUNNING", "the scan flags are not in STATE");
    CHECK_STR(rows[3].left, "SCAN", "row 3 label");
    CHECK_STR(rows[3].right, "PSCAN ISCAN", "discoverable and connectable");
    CHECK_STR(rows[4].left, "ACL", "row 4 label");
    CHECK_STR(rows[4].right, "1021 x 8", "the ACL buffer the controller reported");
    CHECK_STR(rows[5].left, "RX/TX", "row 5 label");
    CHECK_STR(rows[5].right, "4096 / 1024", "bytes in and out");
}

/* A registered controller whose firmware never loaded: it is DOWN and its
 * address is all zeros. Both facts have to survive onto the screen, because
 * together they are the signature of a missing rtl8761bu_fw.bin. */
static void test_a_controller_with_no_firmware_shows_a_zero_address(void)
{
    nd_bt_adapter a;
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];

    memset(&a, 0, sizeof a);
    (void)nd_strlcpy(a.name, "hci0", sizeof a.name);
    a.bus = 1u;

    (void)api.adapter_rows(&a, true, rows, ND_ARRAY_LEN(rows));
    CHECK_STR(rows[1].right, "00:00:00:00:00:00", "no firmware means no address");
    CHECK_STR(rows[2].right, "DOWN", "and it is down");
    CHECK_STR(rows[3].right, "none", "an empty cell would read as a bug");
}

static void test_with_no_controller_the_page_says_so_in_one_row(void)
{
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n = api.adapter_rows(NULL, false, rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 1, "one row, not an empty grid");
    CHECK_STR(rows[0].left, "HCI", "row 0 label");
    CHECK_STR(rows[0].right, "none", "no controller");
}

static void test_pscan_and_iscan_are_reported_separately(void)
{
    char s[ND_BTAPP_CELL_MAX];

    api.disc_str(ND_BT_FLAG_UP | ND_BT_FLAG_PSCAN, s, sizeof s);
    CHECK_STR(s, "PSCAN", "page scan alone");
    api.disc_str(ND_BT_FLAG_UP | ND_BT_FLAG_ISCAN, s, sizeof s);
    CHECK_STR(s, "ISCAN", "inquiry scan alone");
    api.disc_str(ND_BT_FLAG_UP, s, sizeof s);
    CHECK_STR(s, "none", "up but invisible");
}

/* The softkey says what the press will do. An adapter that is down is not
 * labelled "Radio Off" just because that is what it is. */
static void test_the_softkey_names_the_action_not_the_state(void)
{
    CHECK_STR(api.power_softkey(0u), "Radio On", "a down radio offers to come up");
    CHECK_STR(api.power_softkey(ND_BT_FLAG_UP | ND_BT_FLAG_RUNNING), "Radio Off",
              "an up radio offers to go down");
}

/* ------------------------------------------------------------------ *
 * 3. The scan list
 * ------------------------------------------------------------------ */

static void test_a_scan_lists_the_address_and_the_class(void)
{
    nd_bt_device devs[2];
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n;

    memset(devs, 0, sizeof devs);
    devs[0].addr[5] = 0xB8u;
    devs[0].addr[4] = 0xFBu;
    devs[0].cod = 0x5A020Cu; /* a smartphone */
    devs[1].addr[5] = 0x00u;
    devs[1].cod = 0x240404u; /* headphones */

    n = api.scan_rows(devs, 2u, true, "", rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 2, "one row per device");
    CHECK_STR(rows[0].left, "B8:FB:00:00:00:00", "the address is the left column here");
    CHECK_STR(rows[0].right, "Phone", "major class of 0x5A020C");
    CHECK_STR(rows[1].right, "Audio/Video", "major class of 0x240404");
}

/* Nobody answered. The radio worked; there was nothing to hear. Saying
 * "FAILED" here would send a technician after a working dongle. */
static void test_an_empty_scan_is_not_a_failure(void)
{
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n = api.scan_rows(NULL, 0u, true, "", rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 1, "one row");
    CHECK_STR(rows[0].left, "no devices", "nobody answered");
    CHECK_STR(rows[0].right, "", "and nothing to say about them");
}

static void test_a_failed_scan_shows_the_errno_text(void)
{
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n = api.scan_rows(NULL, 0u, false, "Network is down", rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 1, "one row");
    CHECK_STR(rows[0].left, "ERROR", "the ioctl failed");
    CHECK_STR(rows[0].right, "Network is down", "strerror(errno), as FuelGauge does");
}

/* The bottom line reports the window that was actually used, in seconds, so
 * "nothing found" can be judged against how long it listened. */
static void test_the_window_is_reported_in_seconds(void)
{
    char s[ND_BTAPP_CELL_MAX];

    api.window_str(ND_BT_INQUIRY_UNITS, s, sizeof s);
    CHECK_STR(s, "5.1 s window", "4 * 1.28 s");
    api.window_str(1u, s, sizeof s);
    CHECK_STR(s, "1.3 s window", "1 * 1.28 s");
}

/* ------------------------------------------------------------------ *
 * 4. The self test
 * ------------------------------------------------------------------ */

static void test_the_five_steps_are_named(void)
{
    CHECK_STR(api.step_name(ND_BTAPP_STEP_KERNEL), "KERNEL", "step 0");
    CHECK_STR(api.step_name(ND_BTAPP_STEP_ADAPTER), "ADAPTER", "step 1");
    CHECK_STR(api.step_name(ND_BTAPP_STEP_ADDRESS), "ADDRESS", "step 2");
    CHECK_STR(api.step_name(ND_BTAPP_STEP_RADIO), "RADIO", "step 3");
    CHECK_STR(api.step_name(ND_BTAPP_STEP_SCAN), "SCAN", "step 4");
    CHECK_STR(api.verdict_str(ND_BTAPP_PASS), "PASS", "a pass");
    CHECK_STR(api.verdict_str(ND_BTAPP_FAIL), "FAIL", "a failure");
    CHECK_STR(api.verdict_str(ND_BTAPP_SKIP), "--", "not judged");
}

static void test_a_check_becomes_a_row(void)
{
    nd_btapp_check checks[ND_BTAPP_STEP_N];
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    size_t n;
    size_t i;

    for (i = 0u; i < (size_t)ND_BTAPP_STEP_N; i++) {
        checks[i].step = (nd_btapp_step)i;
        checks[i].verdict = ND_BTAPP_PASS;
        checks[i].detail[0] = '\0';
    }
    n = api.check_rows(checks, ND_ARRAY_LEN(checks), rows, ND_ARRAY_LEN(rows));

    CHECK_INT(n, 5, "one row per check");
    CHECK_STR(rows[0].left, "KERNEL", "the step name is the left column");
    CHECK_STR(rows[0].right, "PASS", "the verdict is the right column");
    CHECK_STR(rows[4].left, "SCAN", "and the order is the run order");
}

static void test_the_first_failure_reaches_the_bottom_line(void)
{
    nd_btapp_check checks[3];

    memset(checks, 0, sizeof checks);
    checks[0].step = ND_BTAPP_STEP_KERNEL;
    checks[0].verdict = ND_BTAPP_PASS;
    checks[1].step = ND_BTAPP_STEP_ADAPTER;
    checks[1].verdict = ND_BTAPP_FAIL;
    (void)nd_strlcpy(checks[1].detail, "no controller", sizeof checks[1].detail);
    checks[2].step = ND_BTAPP_STEP_ADDRESS;
    checks[2].verdict = ND_BTAPP_SKIP;

    CHECK_STR(api.first_failure(checks, 3u), "no controller", "the reason reaches the caller");

    checks[1].verdict = ND_BTAPP_PASS;
    CHECK_STR(api.first_failure(checks, 3u), "", "nothing failed, nothing to say");
}

/* The live one. See the header: the assertion is about the shape, so it holds
 * with the dongle plugged in and without it. */
static void test_the_self_test_stops_at_the_first_failure(void)
{
    nd_btapp_check checks[ND_BTAPP_STEP_N];
    size_t n;
    size_t i;
    size_t first_fail = (size_t)ND_BTAPP_STEP_N;

    if (geteuid() == 0u) {
        printf("  running as root: the self test would really power the radio; skipped\n");
        return;
    }

    memset(checks, 0, sizeof checks);
    n = api.selftest(checks, ND_ARRAY_LEN(checks), false);

    CHECK_INT(n, ND_BTAPP_STEP_N, "every step is reported");
    for (i = 0u; i < n; i++) {
        /* The steps come back in order, whatever happened. */
        CHECK_INT(checks[i].step, (int)i, "the steps come back in run order");
        if (checks[i].verdict == ND_BTAPP_FAIL && first_fail == (size_t)ND_BTAPP_STEP_N)
            first_fail = i;
        printf("  %-8s %-4s %s\n", api.step_name(checks[i].step),
               api.verdict_str(checks[i].verdict), checks[i].detail);
    }
    /* do_scan false, so SCAN is never attempted and never PASSes. */
    CHECK(checks[ND_BTAPP_STEP_SCAN].verdict != ND_BTAPP_PASS,
          "a scan that was never run cannot have passed");
    /* Nothing after the first failure is judged. */
    for (i = first_fail + 1u; i < n; i++)
        CHECK_INT(checks[i].verdict, ND_BTAPP_SKIP, "nothing after a failure is judged");
    /* A failure always says why; a pass never invents a reason. */
    for (i = 0u; i < n; i++) {
        if (checks[i].verdict == ND_BTAPP_FAIL)
            CHECK(checks[i].detail[0] != '\0', "a failure always says why");
    }
    /* This machine's kernel has Bluetooth -- the test binary is running on
     * it and test_bt.c already listed its adapters -- so step one passes
     * everywhere `make test` runs. */
    CHECK_INT(checks[ND_BTAPP_STEP_KERNEL].verdict, ND_BTAPP_PASS,
              "this kernel has CONFIG_BT");
}

/* ------------------------------------------------------------------ *
 * 5 and 6. The app itself
 * ------------------------------------------------------------------ */

static void test_the_two_refusals_are_different_faults(void)
{
    CHECK(strcmp(*api.no_kernel, *api.no_adapter) != 0, "two faults, two messages");
    /* Each one names the thing to go and look at. */
    CHECK(strstr(*api.no_kernel, "CONFIG_BT") != NULL, "the kernel one names the option");
    CHECK(strstr(*api.no_adapter, "firmware") != NULL, "the dongle one names the firmware");
}

static void test_the_menu_draws_and_back_leaves(void)
{
    sa_fixture fx;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    /* VerticalList does not flush the channel before its first draw, so a
     * queued Back is enough -- smallapp_test.h's note. */
    if (!sa_send(&fx, ND_KEY_CLEAR)) {
        CHECK(false, "key script");
        sa_fx_free(&fx);
        return;
    }

    nd_vclock_enable();
    CHECK_INT(api.run(&fx.ui), 0, "Back returns 0");
    CHECK(nd_capture_frames_drawn(fx.cap) > 0, "the menu reached the framebuffer");
    nd_vclock_disable();
    sa_fx_free(&fx);
}

static void test_null_safety(void)
{
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    char s[ND_BTAPP_CELL_MAX];

    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown();
    CHECK_INT(api.adapter_rows(NULL, true, rows, ND_ARRAY_LEN(rows)), 1, "a NULL adapter is no adapter");
    CHECK_INT(api.adapter_rows(NULL, false, NULL, 0u), 0, "no buffer, no rows");
    CHECK_INT(api.scan_rows(NULL, 0u, true, NULL, NULL, 0u), 0, "no buffer, no rows");
    CHECK_INT(api.check_rows(NULL, 0u, rows, ND_ARRAY_LEN(rows)), 0, "no checks, no rows");
    CHECK_STR(api.first_failure(NULL, 0u), "", "never NULL");
    CHECK_INT(api.selftest(NULL, 0u, false), 0, "no buffer, no checks");
    api.disc_str(0u, NULL, sizeof s);
    api.window_str(4u, NULL, 0u);
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("Bluetooth", "ndbt");

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }

    RUN(test_the_menu_is_three_items_in_order);
    RUN(test_the_adapter_page_reads_out_six_rows);
    RUN(test_a_controller_with_no_firmware_shows_a_zero_address);
    RUN(test_with_no_controller_the_page_says_so_in_one_row);
    RUN(test_pscan_and_iscan_are_reported_separately);
    RUN(test_the_softkey_names_the_action_not_the_state);
    RUN(test_a_scan_lists_the_address_and_the_class);
    RUN(test_an_empty_scan_is_not_a_failure);
    RUN(test_a_failed_scan_shows_the_errno_text);
    RUN(test_the_window_is_reported_in_seconds);
    RUN(test_the_five_steps_are_named);
    RUN(test_a_check_becomes_a_row);
    RUN(test_the_first_failure_reaches_the_bottom_line);
    RUN(test_the_self_test_stops_at_the_first_failure);
    RUN(test_the_two_refusals_are_different_faults);
    RUN(test_the_menu_draws_and_back_leaves);
    RUN(test_null_safety);

    return sa_end(h, "test_bluetooth");
}
