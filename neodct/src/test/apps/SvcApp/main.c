/* test/apps/SvcApp/main.c -- an app that exists to prove the service channel.
 *
 * The claim nd_svc.h makes is not one a unit test of a serialiser can check:
 *
 *     AN APP IN ITS OWN PROCESS CAN REACH THE CORE'S MODEM AND BATTERY.
 *
 * So this is a real app.so, dlopen()ed by the real nd-apprun, in a real child
 * process forked and exec'd by the real nd_proc_launch_app(), whose parent is
 * a real core holding a real nd_modem and nd_battery. It calls every one of
 * the six service nd_svc_* entry points and writes what it got to a file the
 * parent then checks against its OWN live services -- so the test cannot
 * pass by the two sides agreeing on a wrong answer, only by them agreeing on
 * the same answer.
 *
 * It also asks for a POWEROFF, which is the one request here whose honest
 * success would be that this machine stopped. The parent installs
 * nd_svc_halt_simulate() before launching, which replaces the core's spawn
 * and nothing else -- so the request, the validation, the resolution and the
 * reply are all real and only the last line is not. See
 * docs/c-rewrite/spec-app-services.md section 9.9.
 *
 * It lives in test/apps/ rather than apps/ for the reason CrashApp does: no
 * shipped image may contain a program whose whole job is to poke at the OS.
 *
 * The report is `key=value` lines, one per fact, so a failure names the fact
 * rather than a byte offset. test_svc.c parses it.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_log.h"
#include "nd_modem.h"
#include "nd_paths.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"

/* Under ND_ROOT, which nd_proc_launch_app() passes to the child, so the
 * report lands inside the parent's scratch root and not in a real /NeoDCT. */
#define SVCAPP_REPORT "/NeoDCT/User/svcapp-report.txt"

static FILE *g_out;

static void say(const char *fmt, ...) ND_PRINTF(1, 2);

static void say(const char *fmt, ...)
{
    va_list ap;

    if (g_out == NULL)
        return;
    va_start(ap, fmt);
    (void)vfprintf(g_out, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g_out);
}

int app_run(nd_ui *ui)
{
    char resolved[ND_PATH_MAX];
    char detail[ND_MODEM_DETAIL_MAX];
    nd_modem_status st;
    nd_svc_battery batt;

    if (nd_path_resolve(resolved, sizeof resolved, SVCAPP_REPORT) != ND_OK)
        return 1;
    g_out = fopen(resolved, "w");
    if (g_out == NULL) {
        nd_log_err(ND_LOG_OS, "SvcApp: cannot write %s", resolved);
        return 1;
    }

    /* 1. This really is the far side of a process boundary: nd_app.h's rule
     *    still holds and the handles are still NULL. If these two ever come
     *    back non-NULL the rest of the report proves nothing. */
    say("modem_handle=%d", ui->modem != NULL);
    say("battery_handle=%d", ui->battery != NULL);
    say("channel=%d", nd_svc_client_active());

    /* 2. Presence. */
    say("modem_present=%d", nd_svc_modem_present(ui));
    say("battery_present=%d", nd_svc_battery_present(ui));

    /* 3. THE ONE THAT MATTERS: an SMS, sent from a child process, through the
     *    core, into the core's ModemService. With no hardware that service
     *    answers "simulated" -- which is still the real code path all the way
     *    down to do_send_sms()'s own simulation branch. */
    detail[0] = '\0';
    say("sms_ok=%d",
        nd_svc_send_sms(ui, "0871234567", "hello from a child process", detail, sizeof detail));
    say("sms_detail=%s", detail);

    /* 4. A number the core must refuse. The app-side code sends whatever it
     *    is handed; the REJECTION happens in the core, which is the point --
     *    the boundary does not trust the child. */
    detail[0] = '\0';
    say("bad_number_ok=%d", nd_svc_send_sms(ui, "0871234567\r\nATH\r\n", "and hang up the call",
                                            detail, sizeof detail));
    say("bad_number_detail=%s", detail);

    /* 5. An empty body: refused by the same validator, for a different rule. */
    detail[0] = '\0';
    say("empty_body_ok=%d", nd_svc_send_sms(ui, "0871234567", "", detail, sizeof detail));

    /* 6. The modem snapshot. The parent compares these three against its own. */
    memset(&st, 0, sizeof st);
    say("status_ok=%d", nd_svc_modem_status(ui, &st));
    say("status_hardware=%d", st.hardware);
    say("status_port=%s", st.port);
    say("status_reg_stat=%d", (int)st.reg_stat);

    /* 7. The battery, in one round trip. */
    memset(&batt, 0, sizeof batt);
    say("batt_read=%d", nd_svc_battery_read(ui, &batt));
    say("batt_ok=%d", batt.ok);
    say("batt_hardware=%d", batt.hardware);
    say("batt_level=%d", (int)batt.level);
    say("batt_bus=%d", batt.snap.bus);
    say("batt_addr=%d", batt.snap.addr);

    /* 8. The one write on the channel. With no gauge it must answer false,
     *    which still proves the request reached nd_battery_quickstart(). */
    say("quickstart=%d", nd_svc_battery_quickstart(ui));

    /* 9. THE VERB THAT ENDS THE SESSION. This is the one call in this file
     *    whose real consequence is that the machine stops, so the PARENT --
     *    which is the process that serves this request -- has replaced the
     *    spawn with a fake before launching us (nd_svc_halt_simulate()).
     *    Everything on this side is real: a genuine request from a genuine
     *    child process, over the socket nd_proc_launch_app() handed us.
     *
     *    Poweroff and not reboot, so that a fake that was somehow NOT
     *    installed would take the machine down in the way a developer
     *    notices immediately rather than the way that looks like a flaky
     *    CI runner. spec-app-services.md 9.9. */
    say("poweroff=%d", nd_svc_poweroff());

    say("done=1");
    (void)fclose(g_out);
    g_out = NULL;
    return 0;
}

void app_shutdown(void)
{
    if (g_out != NULL) {
        (void)fclose(g_out);
        g_out = NULL;
    }
}
