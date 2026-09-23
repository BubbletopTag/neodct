/* apps/Modem/main.c -- three pages of live SIM7600 status.
 *
 * A one-to-one port of System/engineering/apps/Modem/main.py. App id 9005,
 * engineering menu, manifest name "ModemInfo". Three pages walked with the
 * softkey (Next, Next, then Exit); Back leaves from anywhere.
 *
 *   RADIO  operator / registration / CSQ+dBm / bars / call state, 1 Hz
 *   SIM    CPIN, +CNUM, IMEI, ICCID, IMSI, firmware -- queried ONCE, on the
 *          first visit, because identity does not change mid-session
 *   DATA   S45modem's status file, the wwan interface, its global IPv6, the
 *          configured APN and the resolver, 1 Hz
 *
 * ============ IT DELIBERATELY RUNS WITH NO MODEM ============
 *
 * FuelGauge refuses to start without hardware. This one does not, and the
 * Python says why: "when the modem is missing, seeing *why* (which ttyUSB
 * nodes exist, what the boot script logged) from inside the environment is
 * the whole point -- serial consoling the real hardware is annoying." That is
 * also what golden/eng-modem.png is a picture of: a phone with no modem,
 * showing OPER/REG/CSQ as "--", the PORTS row that only appears when there is
 * no hardware, and SIMULATION along the bottom.
 *
 * ============ WHICH READOUT EACH ROW COMES FROM ============
 *
 * status_snapshot() mixes raw attributes with method calls, and the capture
 * harness patches the methods and not the attributes -- so the distinction
 * decides two rows of the reference frame and has to be kept exactly:
 *
 *   OPER   snap["operator"], the RAW attribute. simulate_status() patches
 *          operator_display(), NOT this, so the frame says "--" even though
 *          the home screen beside it says "Tello".
 *   BARS   snap["bars"], which IS self.signal_level() -- patched. The frame
 *          says 4/4. So this row goes through nd_ui_status_signal_level(),
 *          the one place that consults the override (nd_ui_sim.h), and OPER
 *          goes through the snapshot.
 *
 * Getting those two the same way round is the difference between a frame that
 * matches and one that is wrong in two places at once.
 *
 * ============ struct nd_lines HAD TO COME OUT OF lib/ ============
 *
 * The SIM page is six AT transactions and nd_modem_send_at() takes a
 * `struct nd_lines *`, which no public header completed -- so this app, the
 * only caller that function exists for, could not allocate one. That is
 * OPEN-QUESTIONS.md M-3 and it is fixed there rather than worked around here:
 * the definition moved into nd_modem.h. It is 4 KB, so the one instance below
 * is static rather than a stack frame (CODING-STANDARDS.md 1.5), which is
 * safe because an app process draws from one thread.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_modem.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "modem_app.h"
#include "modem_probe.h"

#define MODEM_KEY_NAV  ND_KEY_ENTER
#define MODEM_KEY_BACK ND_KEY_BACK

/* main.py: send_at(..., timeout=3.0) on every identity query. */
#define MODEM_AT_TIMEOUT 3.0

const char *const nd_modemapp_pages[ND_MODEMAPP_N_PAGES] = {"RADIO", "SIM", "DATA"};

const char *const nd_modemapp_reg_names[ND_MODEMAPP_N_REG_NAMES] = {
    "NOT REG", "HOME", "SEARCHING", "DENIED", "UNKNOWN", "ROAMING",
};

const char *const nd_modemapp_no_service_msg = "ModemService is not running.";

/* ------------------------------------------------------------------ *
 * Rows
 * ------------------------------------------------------------------ */

static void row_set(nd_modemapp_row *r, const char *label, const char *value)
{
    (void)nd_strlcpy(r->label, label, sizeof r->label);
    (void)nd_strlcpy(r->value, value, sizeof r->value);
}

const char *nd_modemapp_state_name(nd_call_state st)
{
    switch (st) {
    case ND_CALL_CALLING:
        return "CALLING";
    case ND_CALL_RINGING:
        return "RINGING";
    case ND_CALL_CONNECTED:
        return "CONNECTED";
    case ND_CALL_IDLE:
    default:
        return "IDLE";
    }
}

size_t nd_modemapp_radio_rows(const nd_modem_status *st, int32_t bars, nd_modemapp_row *out,
                              size_t max)
{
    char buf[ND_MODEMAPP_VALUE_MAX];
    size_t n = 0u;

    if (st == NULL || out == NULL || max == 0u)
        return 0u;

    row_set(&out[n++], "OPER", st->operator_name[0] != '\0' ? st->operator_name : "--");
    if (n >= max)
        return n;

    /* REG_NAMES.get(stat, str(stat)), then "  (CEREG %s)" appended for any
     * stat that is not None -- including one the table does not name. */
    if (st->reg_stat < 0)
        (void)nd_strlcpy(buf, "--", sizeof buf);
    else {
        char name[24];

        if (st->reg_stat < ND_MODEMAPP_N_REG_NAMES)
            (void)nd_strlcpy(name, nd_modemapp_reg_names[st->reg_stat], sizeof name);
        else
            (void)nd_snprintf(name, sizeof name, "%d", st->reg_stat);
        (void)nd_snprintf(buf, sizeof buf, "%s  (CEREG %d)", name, st->reg_stat);
    }
    row_set(&out[n++], "REG", buf);
    if (n >= max)
        return n;

    /* csq None -> "--", 99 -> "99 (no signal)", else the raw value and the
     * dBm it maps to. -113 + 2 * rssi is the 3GPP 27.007 table. */
    if (st->csq_rssi < 0)
        (void)nd_strlcpy(buf, "--", sizeof buf);
    else if (st->csq_rssi == 99)
        (void)nd_strlcpy(buf, "99 (no signal)", sizeof buf);
    else
        (void)nd_snprintf(buf, sizeof buf, "%d/31  %d dBm", st->csq_rssi, -113 + 2 * st->csq_rssi);
    row_set(&out[n++], "CSQ", buf);
    if (n >= max)
        return n;

    if (bars < 0)
        (void)nd_strlcpy(buf, "--", sizeof buf);
    else
        (void)nd_snprintf(buf, sizeof buf, "%d/4", bars);
    row_set(&out[n++], "BARS", buf);
    if (n >= max)
        return n;

    row_set(&out[n++], "CALL", nd_modemapp_state_name(st->state));
    if (n >= max)
        return n;

    /* The PORTS row exists ONLY without hardware: with a modem attached the
     * port is already on the bottom line, and with none the question is which
     * nodes the kernel did enumerate. */
    if (!st->hardware) {
        char ttys[ND_MODEMAPP_VALUE_MAX];

        if (nd_modemapp_ttyusb_list(ttys, sizeof ttys) == 0u)
            (void)nd_strlcpy(ttys, "no ttyUSB nodes!", sizeof ttys);
        row_set(&out[n++], "PORTS", ttys);
        if (n >= max)
            return n;

        /* WHY, not just what. The nodes existing and the modem answering are
         * different questions, and this app exists to be read on a phone that
         * has no serial console attached -- so the probe's own reason comes
         * across the wire and is drawn here rather than only being printed
         * where nobody can see it. */
        if (st->probe_why[0] != '\0')
            row_set(&out[n++], "WHY", st->probe_why);
    }
    return n;
}

size_t nd_modemapp_sim_rows_absent(nd_modemapp_row *out, size_t max)
{
    static const char *const LABELS[6] = {"SIM", "NUM", "IMEI", "ICCID", "IMSI", "FW"};
    size_t i;

    if (out == NULL)
        return 0u;
    for (i = 0u; i < ND_ARRAY_LEN(LABELS) && i < max; i++)
        row_set(&out[i], LABELS[i], "n/a (sim)");
    return i;
}

size_t nd_modemapp_data_rows(nd_modemapp_row *out, size_t max)
{
    char iface[ND_MODEMAPP_IFNAME_MAX];
    char buf[ND_MODEMAPP_VALUE_MAX];
    char shortened[ND_MODEMAPP_VALUE_MAX];
    char ipv6[64];
    bool have_iface;
    bool have_ipv6 = false;
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    if (!nd_modemapp_read_file(ND_MODEMAPP_BOOT_STATUS_FILE, buf, sizeof buf) || buf[0] == '\0')
        (void)nd_strlcpy(buf, "(no S45modem run)", sizeof buf);
    row_set(&out[n++], "BOOT", buf);
    if (n >= max)
        return n;

    have_iface = nd_modemapp_wwan_interface(iface, sizeof iface);
    if (have_iface) {
        (void)nd_snprintf(buf, sizeof buf, "%s %s", iface,
                          nd_modemapp_iface_up(iface) ? "UP" : "DOWN");
        have_ipv6 = nd_modemapp_global_ipv6(iface, ipv6, sizeof ipv6);
    } else {
        (void)nd_strlcpy(buf, "none found", sizeof buf);
    }
    row_set(&out[n++], "IF", buf);
    if (n >= max)
        return n;

    row_set(&out[n++], "IPV6",
            have_ipv6
                ? nd_modemapp_shorten(ipv6, ND_MODEMAPP_SHORTEN_LIMIT, shortened, sizeof shortened)
                : "--");
    if (n >= max)
        return n;

    nd_modemapp_configured_apn(buf, sizeof buf);
    row_set(&out[n++], "APN",
            nd_modemapp_shorten(buf, ND_MODEMAPP_SHORTEN_LIMIT, shortened, sizeof shortened));
    if (n >= max)
        return n;

    nd_modemapp_dns_row(buf, sizeof buf);
    row_set(&out[n++], "DNS",
            nd_modemapp_shorten(buf, ND_MODEMAPP_SHORTEN_LIMIT, shortened, sizeof shortened));
    return n;
}

/* ------------------------------------------------------------------ *
 * The SIM page's six transactions
 * ------------------------------------------------------------------ */

/* 4 KB and change; see the header comment for why it is not on the stack. */
static struct nd_lines g_lines;

/* One send_at, with the reply's intermediate lines pointed at by `view`.
 * Returns the final result line, "" when there was none -- which is Python's
 * `final is None`, i.e. no modem or a locked port (OPEN-QUESTIONS.md M-12). */
static const char *transact(nd_modem *m, const char *cmd, char *final_out, size_t final_sz,
                            const char *(*view)[ND_MODEM_LINES_MAX], size_t *n_view)
{
    size_t i;

    final_out[0] = '\0';
    *n_view = 0u;
    nd_modem__lines_reset(&g_lines);
    if (nd_modem_send_at(m, cmd, MODEM_AT_TIMEOUT, final_out, final_sz, &g_lines) != ND_OK)
        final_out[0] = '\0';
    for (i = 0u; i < g_lines.n && i < ND_MODEM_LINES_MAX; i++)
        (*view)[i] = nd_modem__lines_get(&g_lines, i);
    *n_view = i;
    return final_out;
}

static size_t sim_rows_present(nd_modem *m, const nd_modem_status *st, nd_modemapp_row *out,
                               size_t max)
{
    const char *view[ND_MODEM_LINES_MAX];
    size_t n_view = 0u;
    char final[64];
    char sim[ND_MODEMAPP_VALUE_MAX];
    char number[ND_MODEMAPP_VALUE_MAX];
    char iccid[ND_MODEMAPP_VALUE_MAX];
    char imsi[ND_MODEMAPP_VALUE_MAX];
    char fw[ND_MODEMAPP_VALUE_MAX];
    char shortened[ND_MODEMAPP_VALUE_MAX];
    size_t i;
    size_t n = 0u;

    (void)transact(m, "AT+CPIN?", final, sizeof final, &view, &n_view);
    if (strcmp(final, "OK") == 0) {
        if (!nd_modemapp_first_content(view, n_view, "+CPIN:", sim, sizeof sim) || sim[0] == '\0')
            (void)nd_strlcpy(sim, "?", sizeof sim);
    } else if (final[0] == '\0')
        (void)nd_strlcpy(sim, "no reply", sizeof sim);
    else
        (void)nd_strlcpy(sim, "NOT DETECTED", sizeof sim);

    /* "(not on SIM)" unless +CNUM gives a non-empty fourth quoted field. The
     * Python breaks after the FIRST +CNUM line whether or not it yielded a
     * number, so a second entry is never consulted. Kept. */
    (void)nd_strlcpy(number, "(not on SIM)", sizeof number);
    (void)transact(m, "AT+CNUM", final, sizeof final, &view, &n_view);
    if (strcmp(final, "OK") == 0) {
        for (i = 0u; i < n_view; i++) {
            const char *line = view[i];
            const char *p;
            size_t quotes = 0u;
            const char *third = NULL;
            const char *fourth = NULL;

            if (strncmp(line, "+CNUM:", 6u) != 0)
                continue;
            for (p = line; *p != '\0'; p++) {
                if (*p != '"')
                    continue;
                quotes++;
                if (quotes == 3u)
                    third = p + 1;
                else if (quotes == 4u)
                    fourth = p;
            }
            if (quotes >= 4u && third != NULL && fourth > third) {
                size_t len = (size_t)(fourth - third);

                if (len + 1u <= sizeof number) {
                    memcpy(number, third, len);
                    number[len] = '\0';
                }
            }
            break;
        }
    }

    /* +CICCID first, then the older +CCID -- and the fallback runs when the
     * first returned OK with nothing usable, not only when it failed. */
    iccid[0] = '\0';
    (void)transact(m, "AT+CICCID", final, sizeof final, &view, &n_view);
    if (strcmp(final, "OK") == 0)
        (void)nd_modemapp_first_content(view, n_view, "+ICCID:", iccid, sizeof iccid);
    if (iccid[0] == '\0') {
        (void)transact(m, "AT+CCID", final, sizeof final, &view, &n_view);
        if (strcmp(final, "OK") == 0)
            (void)nd_modemapp_first_content(view, n_view, "+CCID:", iccid, sizeof iccid);
    }

    imsi[0] = '\0';
    (void)transact(m, "AT+CIMI", final, sizeof final, &view, &n_view);
    if (strcmp(final, "OK") == 0)
        (void)nd_modemapp_first_content(view, n_view, "+CIMI:", imsi, sizeof imsi);

    fw[0] = '\0';
    (void)transact(m, "AT+CGMR", final, sizeof final, &view, &n_view);
    if (strcmp(final, "OK") == 0)
        (void)nd_modemapp_first_content(view, n_view, "+CGMR:", fw, sizeof fw);

    if (max == 0u)
        return 0u;
    row_set(&out[n++], "SIM", sim);
    if (n < max)
        row_set(&out[n++], "NUM", number);
    if (n < max)
        row_set(&out[n++], "IMEI", st->imei[0] != '\0' ? st->imei : "--");
    if (n < max)
        row_set(&out[n++], "ICCID", iccid[0] != '\0' ? iccid : "--");
    if (n < max)
        row_set(&out[n++], "IMSI", imsi[0] != '\0' ? imsi : "--");
    if (n < max) {
        /* _shorten(fw or "--"): the fallback happens BEFORE the shortening,
         * so "--" is what gets shortened, harmlessly. */
        row_set(&out[n++], "FW",
                nd_modemapp_shorten(fw[0] != '\0' ? fw : "--", ND_MODEMAPP_SHORTEN_LIMIT, shortened,
                                    sizeof shortened));
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

int32_t nd_modemapp_line_h(int32_t bottom, int32_t y, size_t n_rows)
{
    int32_t divisor = n_rows < 1u ? 1 : (int32_t)n_rows;
    int32_t h = (bottom - y - 16) / divisor;

    return h < 15 ? 15 : h;
}

void nd_modemapp_draw_page(nd_ui *ui, const nd_modem_status *st, bool linked, int32_t page,
                           const nd_modemapp_row *rows, size_t n_rows)
{
    int32_t screen_w;
    int32_t bottom;
    int32_t y = 36;
    int32_t line_h;
    int32_t tw = 0;
    int32_t th = 0;
    const char *page_name;
    char pos[16];
    size_t i;

    if (ui == NULL || ui->draw == NULL || st == NULL)
        return;

    screen_w = nd_ui_width(ui);
    bottom = nd_ui_content_bottom(ui);
    page_name = nd_modemapp_pages[page];

    nd_ui_paint_chrome_content(ui);
    (void)nd_draw_text(ui->draw, 5, 0, "Modem", ui->font_xl, ND_WHITE);
    nd_ui_text_size(ui, page_name, ui->font_s, &tw, &th);
    /* y = 8, not 0: the page tag is small type sitting on the title's
     * baseline rather than its ascender. */
    (void)nd_draw_text(ui->draw, screen_w - 5 - tw, 8, page_name, ui->font_s, ND_GRAY);
    (void)nd_draw_line(ui->draw, 0, 30, screen_w, 30, ND_WHITE, 1);

    line_h = nd_modemapp_line_h(bottom, y, n_rows);
    for (i = 0u; i < n_rows; i++) {
        (void)nd_draw_text(ui->draw, 8, y, rows[i].label, ui->font_s, ND_GRAY);
        (void)nd_draw_text(ui->draw, 70, y, rows[i].value, ui->font_s, ND_WHITE);
        y += line_h;
    }

    /* Three states, not two. "SIMULATION" is a claim about the phone; making
     * it when the core never answered says the modem is missing on a phone
     * whose modem is fine, which is exactly the bug this app was reported
     * for. */
    (void)nd_draw_text(ui->draw, 8, bottom - 14,
                       !linked        ? ND_MODEMAPP_NO_LINK
                       : st->hardware ? st->port
                                      : ND_MODEMAPP_SIMULATION,
                       ui->font_s, ND_GRAY);
    (void)nd_snprintf(pos, sizeof pos, "%d/%d", page + 1, ND_MODEMAPP_N_PAGES);
    nd_ui_text_size(ui, pos, ui->font_s, &tw, &th);
    (void)nd_draw_text(ui->draw, screen_w - 5 - tw, bottom - 14, pos, ui->font_s, ND_GRAY);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_softkey softkey;
    nd_modemapp_row rows[ND_MODEMAPP_MAX_ROWS];
    nd_modemapp_row sim_cache[ND_MODEMAPP_MAX_ROWS];
    size_t n_sim_cache = 0u;
    bool have_sim_cache = false;
    int32_t page = 0;
    double last_draw = 0.0;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    /* `if modem is None`. In an app process ui->modem is NULL by nd_app.h's
     * rules, so this used to be the whole app: the dialog fired with a modem
     * plugged in and a SIM registered. nd_svc_modem_present() asks the core
     * instead, and the dialog is left for the case the Python wrote it for --
     * a phone with no ModemService. OPEN-QUESTIONS.md MSG-1. */
    if (!nd_svc_modem_present(ui)) {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, ui, nd_modemapp_no_service_msg);
        (void)nd_msgdialog_show(&dlg);
        return 0;
    }

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        double now = nd_time_monotonic();
        int32_t key;

        if (now - last_draw >= ND_MODEMAPP_REFRESH_S) {
            nd_modem_status st;
            size_t n_rows;
            bool linked;

            /* The return value used to be thrown away. It is the difference
             * between "the core says there is no modem" and "the core did not
             * answer", and the second was being drawn as the first. */
            linked = nd_svc_modem_status(ui, &st);
            if (page == ND_MODEMAPP_PAGE_RADIO) {
                /* BARS is the PATCHED signal_level(); see the header. */
                n_rows = nd_modemapp_radio_rows(&st, nd_ui_status_signal_level(ui), rows,
                                                ND_ARRAY_LEN(rows));
            } else if (page == ND_MODEMAPP_PAGE_SIM) {
                if (!have_sim_cache) {
                    /* ui->modem is NULL in an app process, and RAW AT IS
                     * DELIBERATELY NOT ON THE SERVICE WIRE -- an app that
                     * could choose the command could type ATH at a live call
                     * (spec-app-services.md section 4).
                     *
                     * nd_modem_send_at(NULL, ...) returns ND_ERR_INVAL, which
                     * transact() already turns into final == "" -- the
                     * Python's `final is None`, the no-modem-or-locked-port
                     * case this app was written to render. So the page comes
                     * out SIM "no reply", NUM "(not on SIM)", ICCID/IMSI/FW
                     * "--", with IMEI still real because it comes from the
                     * status snapshot beside them. Every one of those strings
                     * already existed for a modem that would not answer, and
                     * that is what has happened: it was not asked. */
                    n_sim_cache =
                        st.hardware
                            ? sim_rows_present(ui->modem, &st, sim_cache, ND_ARRAY_LEN(sim_cache))
                            : nd_modemapp_sim_rows_absent(sim_cache, ND_ARRAY_LEN(sim_cache));
                    have_sim_cache = true;
                }
                memcpy(rows, sim_cache, sizeof rows);
                n_rows = n_sim_cache;
            } else {
                n_rows = nd_modemapp_data_rows(rows, ND_ARRAY_LEN(rows));
            }

            nd_modemapp_draw_page(ui, &st, linked, page, rows, n_rows);
            nd_softkey_update(&softkey, page < ND_MODEMAPP_N_PAGES - 1 ? "Next" : "Exit", false);
            if (nd_ui_present(ui) != ND_OK)
                return 0;
            last_draw = now;
        }

        key = nd_ui_read_keypress(ui, 0.1);
        if (key == MODEM_KEY_BACK)
            return 0;
        if (key == MODEM_KEY_NAV) {
            page++;
            if (page >= ND_MODEMAPP_N_PAGES)
                return 0;
            last_draw = 0.0;
        }
        if (nd_app_should_exit())
            return 0;
    }
}

/* No sound card, no child process, no open file: transact() closes nothing
 * because nd_modem owns the port. Exported because nd_app.h requires it. */
void app_shutdown(void) {}
