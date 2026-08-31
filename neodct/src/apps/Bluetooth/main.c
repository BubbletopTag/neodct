/* apps/Bluetooth/main.c -- is there a radio, what is its address, who is out
 * there.
 *
 * App id 9007, engineering menu, manifest name "Bluetooth". Not a port of
 * anything: the Python OS never had Bluetooth, so this screen is new and
 * there is deliberately no golden frame for it (AGENTS.md: a new screen's
 * test is its unit test, not a picture of itself that can only ever agree).
 *
 * Three pages off a VerticalList:
 *
 *   Adapter    the controller's own registers at 1 Hz -- name, BD_ADDR, the
 *              flag words, the ACL buffer it reported and its byte counters.
 *              The softkey toggles the radio.
 *   Scan       a general inquiry, 5.1 s, then the addresses and device
 *              classes that answered.
 *   Self test  the five things that have to be true for a dongle to work, in
 *              the order they depend on each other, stopping at the first
 *              one that is not.
 *
 * ============ THE SELF TEST IS THE POINT OF THE APP ============
 *
 * Bringing a Bluetooth dongle up on a new kernel fails in five distinct
 * places and four of them look identical from the outside -- "no Bluetooth".
 * The five steps exist so that a technician with the phone in one hand gets
 * the ONE that is actually wrong:
 *
 *   KERNEL    socket(AF_BLUETOOTH) -- CONFIG_BT is off. Needs a new kernel,
 *             and on this device that means a full reflash, because an
 *             .ndsw carries no kernel (AGENTS.md).
 *   ADAPTER   HCIGETDEVLIST is empty -- btusb did not bind. Either
 *             CONFIG_BT_HCIBTUSB is off or the dongle is not plugged in.
 *   ADDRESS   the controller registered with BD_ADDR 00:00:00:00:00:00 --
 *             btrtl asked for rtl_bt/rtl8761bu_fw.bin and did not get it.
 *             This is the failure that looks most like working hardware,
 *             because hci0 exists.
 *   RADIO     HCIDEVUP failed -- the controller is there and does not answer.
 *   SCAN      the inquiry itself. The only step that proves the radio
 *             transmitted; everything above it proves the software stack.
 *
 * Each step is only attempted if the one before it passed, and the rest are
 * dashed rather than failed. Five failures caused by one unplugged dongle
 * tell you less than one failure and four dashes.
 *
 * ============ THE SCAN BLOCKS, AND THE FRAME GOES OUT FIRST ============
 *
 * HCIINQUIRY does not return until the controller has finished listening.
 * There is no poll, no partial result and no progress. So the "Scanning..."
 * frame is committed BEFORE the ioctl, not after -- otherwise the screen
 * holds whatever was on it for five seconds and the phone looks hung.
 *
 * The wait is interruptible: the kernel waits on the HCI_INQUIRY bit with
 * TASK_INTERRUPTIBLE, and nd-apprun installs SIGTERM without SA_RESTART, so
 * an incoming call lands as EINTR and app_shutdown() runs on time. That is
 * the teardown contract in nd_app.h, and it is the reason a five-second
 * blocking ioctl is acceptable here at all.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_bt.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "bluetooth.h"

#define BT_KEY_NAV  ND_KEY_ENTER
#define BT_KEY_BACK ND_KEY_BACK

/* The two column origins. The adapter and self-test pages are FuelGauge's
 * 8/70 split; the scan list is 8/168 -- see bluetooth.h. */
#define BT_COL_LABEL 8
#define BT_COL_VALUE 70
#define BT_COL_CLASS 168

const char *const nd_btapp_menu[ND_BTAPP_MENU_N] = {"Adapter", "Scan", "Self test"};

/* The CONFIG_BT symbol names used to be in here and are now in the log line
 * beside the call site: they are 18 characters of kernel trivia that a person
 * holding a phone cannot act on, and keeping them cost the sentence that says
 * a reflash is needed -- which was being cut off. 4 lines of 5. */
const char *const nd_btapp_no_kernel_msg = "No Bluetooth here.\n\nEnabling it needs a reflash.";

const char *const nd_btapp_no_adapter_msg =
    "No Bluetooth controller.\n\nNothing on USB, or no rtl_bt firmware.";

/* ------------------------------------------------------------------ *
 * Rows -- pure, and everything that decides what is on screen
 * ------------------------------------------------------------------ */

static void row_set(nd_btapp_row *r, const char *left, const char *right)
{
    (void)nd_strlcpy(r->left, left, sizeof r->left);
    (void)nd_strlcpy(r->right, right, sizeof r->right);
}

void nd_btapp_disc_str(uint32_t flags, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if ((flags & ND_BT_FLAG_PSCAN) != 0u)
        (void)nd_strlcat(out, "PSCAN", out_sz);
    if ((flags & ND_BT_FLAG_ISCAN) != 0u) {
        if (out[0] != '\0')
            (void)nd_strlcat(out, " ", out_sz);
        (void)nd_strlcat(out, "ISCAN", out_sz);
    }
    if (out[0] == '\0')
        (void)nd_strlcpy(out, "none", out_sz);
}

size_t nd_btapp_adapter_rows(const nd_bt_adapter *a, bool have, nd_btapp_row *out, size_t max)
{
    char buf[ND_BTAPP_CELL_MAX];
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    if (!have || a == NULL) {
        row_set(&out[n++], "HCI", "none");
        return n;
    }

    (void)snprintf(buf, sizeof buf, "%s (%s)", a->name, nd_bt_bus_name(a->bus));
    row_set(&out[n++], "HCI", buf);
    if (n >= max)
        return n;

    nd_bt_addr_str(a->addr, buf, sizeof buf);
    row_set(&out[n++], "ADDR", buf);
    if (n >= max)
        return n;

    /* PSCAN and ISCAN are masked out here and get their own row below --
     * "UP RUNNING PSCAN ISCAN" is 217 px and the value column has 170. */
    nd_bt_flags_str(a->flags & ~(uint32_t)(ND_BT_FLAG_PSCAN | ND_BT_FLAG_ISCAN), buf, sizeof buf);
    row_set(&out[n++], "STATE", buf);
    if (n >= max)
        return n;

    nd_btapp_disc_str(a->flags, buf, sizeof buf);
    row_set(&out[n++], "SCAN", buf);
    if (n >= max)
        return n;

    /* The buffer the controller advertised in its Read Buffer Size reply:
     * bytes per ACL packet by how many it will hold. Zero on a controller
     * that has never been up, which is itself the reading. */
    (void)snprintf(buf, sizeof buf, "%u x %u", (unsigned)a->acl_mtu, (unsigned)a->acl_pkts);
    row_set(&out[n++], "ACL", buf);
    if (n >= max)
        return n;

    (void)snprintf(buf, sizeof buf, "%lu / %lu", (unsigned long)a->rx_bytes,
                   (unsigned long)a->tx_bytes);
    row_set(&out[n++], "RX/TX", buf);
    return n;
}

const char *nd_btapp_power_softkey(uint32_t flags)
{
    return ((flags & ND_BT_FLAG_UP) != 0u) ? "Radio Off" : "Radio On";
}

size_t nd_btapp_scan_rows(const nd_bt_device *devs, size_t n, bool ok, const char *error,
                          nd_btapp_row *out, size_t max)
{
    char addr[ND_BT_ADDR_STR];
    size_t written = 0u;
    size_t i;

    if (out == NULL || max == 0u)
        return 0u;

    if (!ok) {
        row_set(&out[written++], "ERROR", error != NULL ? error : "");
        return written;
    }
    /* An inquiry that heard nothing is a working radio in an empty room, and
     * the screen has to say that rather than looking broken. */
    if (n == 0u || devs == NULL) {
        row_set(&out[written++], "no devices", "");
        return written;
    }

    for (i = 0u; i < n && written < max; i++) {
        nd_bt_addr_str(devs[i].addr, addr, sizeof addr);
        row_set(&out[written++], addr, nd_bt_cod_major(devs[i].cod));
    }
    return written;
}

void nd_btapp_window_str(uint8_t units, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    /* One unit is 1.28 s exactly; printing the product rather than the unit
     * count means the line says what the radio did, not what it was asked. */
    (void)snprintf(out, out_sz, "%.1f s window", (double)units * 1.28);
}

/* ------------------------------------------------------------------ *
 * The self test
 * ------------------------------------------------------------------ */

const char *nd_btapp_step_name(nd_btapp_step step)
{
    static const char *const names[ND_BTAPP_STEP_N] = {"KERNEL", "ADAPTER", "ADDRESS", "RADIO",
                                                       "SCAN"};

    if ((size_t)step < ND_ARRAY_LEN(names))
        return names[step];
    return "?";
}

const char *nd_btapp_verdict_str(nd_btapp_verdict v)
{
    switch (v) {
    case ND_BTAPP_PASS:
        return "PASS";
    case ND_BTAPP_FAIL:
        return "FAIL";
    case ND_BTAPP_SKIP:
    default:
        return "--";
    }
}

static void check_set(nd_btapp_check *c, nd_btapp_step step, nd_btapp_verdict v, const char *detail)
{
    c->step = step;
    c->verdict = v;
    (void)nd_strlcpy(c->detail, detail != NULL ? detail : "", sizeof c->detail);
}

size_t nd_btapp_selftest(nd_btapp_check *out, size_t max, bool do_scan)
{
    nd_bt_adapter list[ND_BT_MAX_ADAPTERS];
    nd_bt_device seen[ND_BT_MAX_DEVICES];
    char detail[ND_BTAPP_CELL_MAX];
    size_t n_adapters = 0u;
    size_t n_seen = 0u;
    size_t i;
    uint16_t id = 0u;

    if (out == NULL || max < (size_t)ND_BTAPP_STEP_N)
        return 0u;

    /* Everything starts dashed, so a step that is never reached is already
     * correct and no failure path has to remember to write it. */
    for (i = 0u; i < (size_t)ND_BTAPP_STEP_N; i++)
        check_set(&out[i], (nd_btapp_step)i, ND_BTAPP_SKIP, "");

    if (!nd_bt_available()) {
        check_set(&out[ND_BTAPP_STEP_KERNEL], ND_BTAPP_STEP_KERNEL, ND_BTAPP_FAIL, strerror(errno));
        return (size_t)ND_BTAPP_STEP_N;
    }
    check_set(&out[ND_BTAPP_STEP_KERNEL], ND_BTAPP_STEP_KERNEL, ND_BTAPP_PASS, "CONFIG_BT is on");

    if (nd_bt_list(list, ND_ARRAY_LEN(list), &n_adapters) != ND_OK || n_adapters == 0u) {
        check_set(&out[ND_BTAPP_STEP_ADAPTER], ND_BTAPP_STEP_ADAPTER, ND_BTAPP_FAIL,
                  "no controller registered");
        return (size_t)ND_BTAPP_STEP_N;
    }
    id = list[0].id;
    (void)snprintf(detail, sizeof detail, "%s on %s", list[0].name, nd_bt_bus_name(list[0].bus));
    check_set(&out[ND_BTAPP_STEP_ADAPTER], ND_BTAPP_STEP_ADAPTER, ND_BTAPP_PASS, detail);

    /* The one that looks like working hardware: hci0 exists, and btrtl never
     * got its firmware, so the controller has no address. */
    if (nd_bt_addr_is_zero(list[0].addr)) {
        check_set(&out[ND_BTAPP_STEP_ADDRESS], ND_BTAPP_STEP_ADDRESS, ND_BTAPP_FAIL,
                  "no BD_ADDR: firmware?");
        return (size_t)ND_BTAPP_STEP_N;
    }
    nd_bt_addr_str(list[0].addr, detail, sizeof detail);
    check_set(&out[ND_BTAPP_STEP_ADDRESS], ND_BTAPP_STEP_ADDRESS, ND_BTAPP_PASS, detail);

    if ((list[0].flags & ND_BT_FLAG_UP) != 0u)
        check_set(&out[ND_BTAPP_STEP_RADIO], ND_BTAPP_STEP_RADIO, ND_BTAPP_PASS, "already up");
    else if (nd_bt_power(id, true) != ND_OK) {
        check_set(&out[ND_BTAPP_STEP_RADIO], ND_BTAPP_STEP_RADIO, ND_BTAPP_FAIL, strerror(errno));
        return (size_t)ND_BTAPP_STEP_N;
    } else {
        /* HCIDEVUP returning 0 is the kernel accepting the request; read the
         * flags back rather than trusting it, because a controller that
         * fails its own init leaves HCI_UP clear. */
        nd_bt_adapter after;

        if (nd_bt_info(id, &after) != ND_OK || (after.flags & ND_BT_FLAG_UP) == 0u) {
            check_set(&out[ND_BTAPP_STEP_RADIO], ND_BTAPP_STEP_RADIO, ND_BTAPP_FAIL,
                      "up accepted, still down");
            return (size_t)ND_BTAPP_STEP_N;
        }
        check_set(&out[ND_BTAPP_STEP_RADIO], ND_BTAPP_STEP_RADIO, ND_BTAPP_PASS, "brought up");
    }

    /* The caller decides whether to spend five seconds. See bluetooth.h. */
    if (!do_scan)
        return (size_t)ND_BTAPP_STEP_N;

    if (nd_bt_inquiry(id, ND_BT_INQUIRY_UNITS, seen, ND_ARRAY_LEN(seen), &n_seen) != ND_OK) {
        check_set(&out[ND_BTAPP_STEP_SCAN], ND_BTAPP_STEP_SCAN, ND_BTAPP_FAIL, strerror(errno));
        return (size_t)ND_BTAPP_STEP_N;
    }
    /* Hearing nobody is a pass. The inquiry ran; the room was empty. */
    (void)snprintf(detail, sizeof detail, "%zu device(s) heard", n_seen);
    check_set(&out[ND_BTAPP_STEP_SCAN], ND_BTAPP_STEP_SCAN, ND_BTAPP_PASS, detail);
    return (size_t)ND_BTAPP_STEP_N;
}

size_t nd_btapp_check_rows(const nd_btapp_check *checks, size_t n, nd_btapp_row *out, size_t max)
{
    size_t written = 0u;
    size_t i;

    if (out == NULL || max == 0u || checks == NULL)
        return 0u;
    for (i = 0u; i < n && written < max; i++)
        row_set(&out[written++], nd_btapp_step_name(checks[i].step),
                nd_btapp_verdict_str(checks[i].verdict));
    return written;
}

const char *nd_btapp_first_failure(const nd_btapp_check *checks, size_t n)
{
    size_t i;

    if (checks == NULL)
        return "";
    for (i = 0u; i < n; i++) {
        if (checks[i].verdict == ND_BTAPP_FAIL)
            return checks[i].detail;
    }
    return "";
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

/* FuelGauge's pitch rule, and the same floor: six rows over 93 px is 15
 * exactly, so the max() is what keeps a seventh legible if one ever appears. */
static int32_t line_h(int32_t bottom, int32_t y, size_t n_rows)
{
    int32_t divisor = n_rows < 1u ? 1 : (int32_t)n_rows;
    int32_t h = (bottom - y - 16) / divisor;

    return h < 15 ? 15 : h;
}

static void draw_page(nd_ui *ui, const char *title, const nd_btapp_row *rows, size_t n_rows,
                      int32_t right_col, const char *footer)
{
    int32_t screen_w = nd_ui_width(ui);
    int32_t bottom = nd_ui_content_bottom(ui);
    int32_t y = 36;
    int32_t pitch;
    size_t i;

    nd_ui_paint_chrome_content(ui);
    (void)nd_draw_text(ui->draw, 5, 0, title, ui->font_xl, ND_WHITE);
    (void)nd_draw_line(ui->draw, 0, 30, screen_w, 30, ND_WHITE, 1);

    pitch = line_h(bottom, y, n_rows);
    for (i = 0u; i < n_rows; i++) {
        (void)nd_draw_text(ui->draw, BT_COL_LABEL, y, rows[i].left, ui->font_s,
                           right_col == BT_COL_VALUE ? ND_GRAY : ND_WHITE);
        (void)nd_draw_text(ui->draw, right_col, y, rows[i].right, ui->font_s,
                           right_col == BT_COL_VALUE ? ND_WHITE : ND_GRAY);
        y += pitch;
    }
    if (footer != NULL && footer[0] != '\0')
        (void)nd_draw_text(ui->draw, BT_COL_LABEL, bottom - 14, footer, ui->font_s, ND_GRAY);
}

/* ------------------------------------------------------------------ *
 * The pages
 * ------------------------------------------------------------------ */

/* Every page needs the first adapter and they all handle its absence the same
 * way, so the dialog lives here rather than three times over. */
static bool first_adapter(nd_ui *ui, nd_bt_adapter *out)
{
    nd_bt_adapter list[ND_BT_MAX_ADAPTERS];
    size_t n = 0u;

    if (nd_bt_list(list, ND_ARRAY_LEN(list), &n) == ND_OK && n > 0u) {
        *out = list[0];
        return true;
    }
    {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, ui, nd_btapp_no_adapter_msg);
        (void)nd_msgdialog_show(&dlg);
    }
    return false;
}

static void page_adapter(nd_ui *ui)
{
    nd_softkey softkey;
    nd_bt_adapter a;
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    double last_draw = 0.0;
    const char *flash = "";
    double flash_until = 0.0;

    if (!first_adapter(ui, &a))
        return;

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        double now = nd_time_monotonic();
        int32_t key;

        if (flash[0] != '\0' && now >= flash_until) {
            flash = "";
            last_draw = 0.0;
        }
        if (now - last_draw >= ND_BTAPP_REFRESH_S) {
            nd_bt_adapter fresh;
            bool have = nd_bt_info(a.id, &fresh) == ND_OK;
            size_t n;

            if (have)
                a = fresh;
            n = nd_btapp_adapter_rows(&a, have, rows, ND_ARRAY_LEN(rows));
            draw_page(ui, "Bluetooth", rows, n, BT_COL_VALUE, flash);
            nd_softkey_update(&softkey, nd_btapp_power_softkey(a.flags), false);
            if (nd_ui_present(ui) != ND_OK)
                return;
            last_draw = now;
        }

        key = nd_ui_read_keypress(ui, 0.1);
        if (key == BT_KEY_BACK)
            return;
        if (key == BT_KEY_NAV) {
            bool want_up = (a.flags & ND_BT_FLAG_UP) == 0u;
            nd_err rc = nd_bt_power(a.id, want_up);

            /* strerror(errno), the way FuelGauge reports a failed i2c read:
             * on a phone this is always root so it works, and on anything
             * else "Operation not permitted" is the whole explanation. */
            flash = rc == ND_OK ? (want_up ? "radio on" : "radio off") : strerror(errno);
            flash_until = nd_time_monotonic() + 2.0;
            last_draw = 0.0;
        }
        if (nd_app_should_exit())
            return;
    }
}

static void page_scan(nd_ui *ui)
{
    nd_softkey softkey;
    nd_bt_adapter a;
    nd_bt_device seen[ND_BT_MAX_DEVICES];
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    char footer[ND_BTAPP_CELL_MAX];
    bool scan_now = true;

    if (!first_adapter(ui, &a))
        return;

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t key;

        if (scan_now) {
            size_t n_seen = 0u;
            nd_err rc;
            size_t n_rows;
            int saved_errno;

            /* The frame goes out BEFORE the ioctl. See the header: there is
             * no progress to report and no chance to draw again until the
             * controller has finished listening. */
            nd_btapp_window_str(ND_BT_INQUIRY_UNITS, footer, sizeof footer);
            row_set(&rows[0], "Scanning...", "");
            draw_page(ui, "Scan", rows, 1u, BT_COL_CLASS, footer);
            nd_softkey_update(&softkey, "Rescan", false);
            if (nd_ui_present(ui) != ND_OK)
                return;

            errno = 0;
            rc = nd_bt_inquiry(a.id, ND_BT_INQUIRY_UNITS, seen, ND_ARRAY_LEN(seen), &n_seen);
            saved_errno = errno;
            /* A SIGTERM during the inquiry arrives as EINTR, and the app owes
             * the core a prompt exit rather than a screen full of results. */
            if (nd_app_should_exit())
                return;

            n_rows = nd_btapp_scan_rows(seen, n_seen, rc == ND_OK, strerror(saved_errno), rows,
                                        ND_ARRAY_LEN(rows));
            if (rc == ND_OK)
                (void)snprintf(footer, sizeof footer, "%zu found", n_seen);
            draw_page(ui, "Scan", rows, n_rows, BT_COL_CLASS, footer);
            nd_softkey_update(&softkey, "Rescan", false);
            if (nd_ui_present(ui) != ND_OK)
                return;
            scan_now = false;
        }

        key = nd_ui_read_keypress(ui, 0.1);
        if (key == BT_KEY_BACK)
            return;
        if (key == BT_KEY_NAV)
            scan_now = true;
        if (nd_app_should_exit())
            return;
    }
}

static void page_selftest(nd_ui *ui)
{
    nd_softkey softkey;
    nd_btapp_check checks[ND_BTAPP_STEP_N];
    nd_btapp_row rows[ND_BTAPP_MAX_ROWS];
    bool run_now = true;

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t key;

        if (run_now) {
            size_t n_checks;
            size_t n_rows;
            const char *why;

            /* Same reason as the scan: step five blocks for five seconds. */
            row_set(&rows[0], "Testing...", "");
            draw_page(ui, "Self test", rows, 1u, BT_COL_VALUE, "");
            nd_softkey_update(&softkey, "Again", false);
            if (nd_ui_present(ui) != ND_OK)
                return;

            n_checks = nd_btapp_selftest(checks, ND_ARRAY_LEN(checks), true);
            if (nd_app_should_exit())
                return;
            n_rows = nd_btapp_check_rows(checks, n_checks, rows, ND_ARRAY_LEN(rows));
            why = nd_btapp_first_failure(checks, n_checks);
            /* Nothing failed, so the footer carries the address instead --
             * the proof that the whole chain worked, rather than a blank
             * line that could equally mean the test never ran. */
            if (why[0] == '\0')
                why = checks[ND_BTAPP_STEP_ADDRESS].detail;
            draw_page(ui, "Self test", rows, n_rows, BT_COL_VALUE, why);
            nd_softkey_update(&softkey, "Again", false);
            if (nd_ui_present(ui) != ND_OK)
                return;
            run_now = false;
        }

        key = nd_ui_read_keypress(ui, 0.1);
        if (key == BT_KEY_BACK)
            return;
        if (key == BT_KEY_NAV)
            run_now = true;
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* No Bluetooth core at all is refused here rather than three pages down,
     * because none of the three has anything to say without one -- and the
     * fix is a kernel rebuild, which is a different conversation from a
     * dongle that is not plugged in. */
    if (!nd_bt_available()) {
        nd_msgdialog dlg;

        /* The detail the dialog no longer has room for. A reflash is the fix
         * either way; the symbol names only help whoever is doing it. */
        nd_log_err(ND_LOG_BLUETOOTH, "No Bluetooth core: CONFIG_BT and CONFIG_BT_HCIBTUSB "
                                     "have to be built in, and an update carries no kernel.");
        nd_msgdialog_init(&dlg, ui, nd_btapp_no_kernel_msg);
        (void)nd_msgdialog_show(&dlg);
        return 0;
    }

    for (;;) {
        nd_vlist menu;
        int32_t choice;

        nd_vlist_init(&menu, ui, "Bluetooth", nd_btapp_menu, (size_t)ND_BTAPP_MENU_N, ND_BTAPP_ID);
        choice = nd_vlist_show(&menu);
        if (choice == ND_WIDGET_BACK)
            return 0;

        switch (choice) {
        case ND_BTAPP_MENU_ADAPTER:
            page_adapter(ui);
            break;
        case ND_BTAPP_MENU_SCAN:
            page_scan(ui);
            break;
        case ND_BTAPP_MENU_SELFTEST:
            page_selftest(ui);
            break;
        default:
            break;
        }
        if (nd_app_should_exit())
            return 0;
    }
}

/* No sound card, no child process, no descriptor held open past a call --
 * every socket in nd_bt.c is closed before its function returns. Exported
 * because nd_app.h requires it, so a missing symbol always means the author
 * forgot rather than that there was nothing to do. */
void app_shutdown(void) {}
