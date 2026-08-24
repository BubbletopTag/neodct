/* nd_crash.c -- what you see, and what gets written down, when something
 * breaks.
 *
 * Ported from System/core/CrashHandler/__init__.py. Two halves that never run
 * in the same process:
 *
 *   THE CHILD half (nd_crash_install_child) runs inside nd-apprun. It installs
 *   handlers for the five fatal signals, and each one writes a fixed-size
 *   binary record to the inherited crash-report descriptor and re-raises with
 *   the default disposition restored -- so the core's waitpid() still reports
 *   the real signal instead of a synthetic exit code. The handler uses only
 *   write(2) on a POD struct: no snprintf, no malloc, nothing that is not
 *   async-signal-safe.
 *
 *   THE CORE half reads that record, formats it, appends it to the log and
 *   draws the screen.
 *
 * ============ WHY THE RECORD IS BINARY ============
 *
 * The Python wrote a traceback, which it could format at leisure because it
 * was already unwinding. A C signal handler may not call snprintf at all, so
 * the child ships the four facts it has (signo, si_code, si_addr, and which
 * entry point was running) and the CORE turns them into the one-line summary
 * that goes on the screen. Same information, formatted on the side of the
 * boundary that is allowed to format.
 *
 * ============ THE ONE PLACE THE PORT CANNOT BE 1:1 ============
 *
 * nd_crash.h says it and it is worth repeating: a Python traceback names the
 * file, the line and every value on the way down; a C crash gives a signal
 * number and an address. Process isolation buys back the part that matters --
 * the core survives and can say what died and how -- but the crash LOG is
 * genuinely thinner. The SCREEN is not: it is pixel-for-pixel the Python's.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_crash.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* CONTINUE_KEYS = {14, 28, 46, 50, 96} -- BACKSPACE, ENTER, C, M, KP_ENTER. */
const int32_t ND_CRASH_CONTINUE_KEYS[ND_CRASH_CONTINUE_KEY_COUNT] = {14, 28, 46, 50, 96};

/* _exc_summary() caps its line at 90 characters, truncating to 87 plus "...". */
#define CRASH_SUMMARY_MAX 91

/* ------------------------------------------------------------------ *
 * The report record that crosses the pipe
 * ------------------------------------------------------------------ */

/* Written by a signal handler with one write(2) and read by the core with one
 * read(2). Fixed width and fixed field order because both ends are the same
 * build on the same machine -- the pipe never leaves the process family. */
#define CRASH_REPORT_MAGIC 0x4e44435288aa5501ull

typedef struct {
    uint64_t magic;
    int32_t signo;
    int32_t si_code;
    uint64_t fault_addr;
    char entry[64]; /* the entry point that was running, for the log */
} crash_report;

/* Set once, before the app is loaded, so the handler needs no allocation. */
static int g_report_fd = -1;
static crash_report g_report;

bool nd_crash_is_simulation(void)
{
    /* The launcher's own detection: real Rockchip/Luckfox hardware exposes the
     * FIQ debug console, QEMU does not. A DEVICE path, so no ND_ROOT. */
    return access(ND_PATH_SERIAL_FIQ, F_OK) != 0;
}

/* ------------------------------------------------------------------ *
 * The child half
 * ------------------------------------------------------------------ */

static void fatal_handler(int signo, siginfo_t *info, void *ctx)
{
    ND_UNUSED(ctx);

    if (g_report_fd >= 0) {
        g_report.signo = signo;
        g_report.si_code = info != NULL ? info->si_code : 0;
        g_report.fault_addr = info != NULL ? (uint64_t)(uintptr_t)info->si_addr : 0u;
        /* One write of a struct smaller than PIPE_BUF, so it is atomic and
         * cannot interleave with anything. The return value is deliberately
         * ignored: there is nothing a dying process can do about it, and the
         * core treats a missing report as "signal, no detail". */
        (void)!write(g_report_fd, &g_report, sizeof g_report);
    }

    /* SA_RESETHAND already restored the default disposition, so re-raising
     * kills us with the real signal and waitpid() reports WIFSIGNALED rather
     * than a synthetic 128+n exit status -- which is the whole reason the
     * child re-raises instead of exiting.
     *
     * The unblock is not optional. sigaction() blocks the delivered signal for
     * the duration of its own handler, so without this the raise() merely
     * marks it pending, the handler returns into _exit(), and the core sees a
     * plain exit of 139. Both sigprocmask and raise are async-signal-safe. */
    {
        sigset_t only;

        (void)sigemptyset(&only);
        (void)sigaddset(&only, signo);
        (void)sigprocmask(SIG_UNBLOCK, &only, NULL);
    }
    (void)raise(signo);
    _exit(128 + signo);
}

nd_err nd_crash_install_child(int report_fd)
{
    static const int FATAL[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    size_t i;

    g_report_fd = report_fd;
    memset(&g_report, 0, sizeof g_report);
    g_report.magic = CRASH_REPORT_MAGIC;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fatal_handler;
    (void)sigemptyset(&sa.sa_mask);
    /* SA_RESETHAND so the re-raise is fatal rather than recursive; SA_NODEFER
     * would let a fault inside the handler loop forever. */
    /* The cast is because SA_RESETHAND is 0x80000000u and sa_flags is a
     * signed int -- glibc's own header makes the constant unsigned. */
    sa.sa_flags = (int)((unsigned int)SA_SIGINFO | (unsigned int)SA_RESETHAND);

    for (i = 0u; i < ND_ARRAY_LEN(FATAL); i++) {
        if (sigaction(FATAL[i], &sa, NULL) != 0) {
            nd_log_err(ND_LOG_CRASH, "sigaction(%d): %s", FATAL[i], strerror(errno));
            return ND_ERR_IO;
        }
    }
    return ND_OK;
}

void nd_crash_set_entry(const char *entry)
{
    if (entry == NULL)
        return;
    (void)nd_strlcpy(g_report.entry, entry, sizeof g_report.entry);
}

bool nd_crash_read_report(int report_fd, nd_crash_info *out)
{
    crash_report rep;
    ssize_t n;

    if (report_fd < 0 || out == NULL)
        return false;

    memset(&rep, 0, sizeof rep);
    do {
        n = read(report_fd, &rep, sizeof rep);
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof rep || rep.magic != CRASH_REPORT_MAGIC)
        return false;

    out->from_signal = true;
    out->signo = rep.signo;
    out->si_code = rep.si_code;
    out->fault_addr = (void *)(uintptr_t)rep.fault_addr;
    (void)nd_snprintf(out->detail, sizeof out->detail, "%s in %s at %p (si_code %d)",
                      nd_crash_signal_name(rep.signo),
                      rep.entry[0] != '\0' ? rep.entry : "(unknown entry)",
                      (void *)(uintptr_t)rep.fault_addr, rep.si_code);
    return true;
}

const char *nd_crash_signal_name(int signo)
{
    switch (signo) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTERM: return "SIGTERM";
    case SIGKILL: return "SIGKILL";
    case SIGPIPE: return "SIGPIPE";
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    default:      return "signal";
    }
}

/* ------------------------------------------------------------------ *
 * The summary line
 * ------------------------------------------------------------------ */

/* _exc_summary(): "TypeError: ..." capped at 90 chars with "..." at 87.
 *
 * The C equivalent of the exception type and message is the crash detail the
 * child sent, and when there is none it is what waitpid told us. Truncation
 * is by BYTE, exactly as the Python's text[:87] is by character -- the strings
 * this formats are ASCII in every path that can reach it. */
size_t nd_crash_summary(const nd_crash_info *info, char *out, size_t out_sz)
{
    char full[256];

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';
    if (info == NULL)
        return 0u;

    if (info->detail[0] != '\0') {
        (void)nd_strlcpy(full, info->detail, sizeof full);
    } else if (info->from_signal) {
        (void)nd_snprintf(full, sizeof full, "%s: killed by signal %d",
                          nd_crash_signal_name(info->signo), info->signo);
    } else if (info->exit_status != 0) {
        (void)nd_snprintf(full, sizeof full, "AppExit: exited with status %d",
                          info->exit_status);
    } else {
        return 0u;
    }

    if (strlen(full) > 90u) {
        full[87] = '\0';
        (void)nd_strlcat(full, "...", sizeof full);
    }
    return nd_strlcpy(out, full, out_sz);
}

/* ------------------------------------------------------------------ *
 * The log
 * ------------------------------------------------------------------ */

static void read_first_field(const char *path, char *out, size_t out_sz, const char *dflt)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    char line[256];

    (void)nd_strlcpy(out, dflt, out_sz);
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return;
    f = fopen(resolved, "r");
    if (f == NULL)
        return;
    if (fgets(line, sizeof line, f) != NULL) {
        char *sp = strchr(line, ' ');

        if (sp != NULL)
            *sp = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0')
            (void)nd_strlcpy(out, line, out_sz);
    }
    (void)fclose(f);
}

/* MemAvailable, else MemFree, as one "key: value kB" string. */
static void read_mem_available(char *out, size_t out_sz)
{
    FILE *f;
    char line[256];

    (void)nd_strlcpy(out, "?", out_sz);
    f = fopen("/proc/meminfo", "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "MemAvailable", 12) == 0 || strncmp(line, "MemFree", 7) == 0) {
            /* " ".join(line.split()) -- collapse the runs of spaces. */
            size_t o = 0u;
            size_t i = 0u;
            bool gap = false;

            for (i = 0u; line[i] != '\0' && o + 1u < out_sz; i++) {
                char c = line[i];

                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    gap = o > 0u;
                    continue;
                }
                if (gap) {
                    out[o++] = ' ';
                    gap = false;
                }
                if (o + 1u < out_sz)
                    out[o++] = c;
            }
            out[o] = '\0';
            break;
        }
    }
    (void)fclose(f);
}

static void fsync_dir(const char *path)
{
    char resolved[ND_PATH_MAX];
    int dfd;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return;
    dfd = open(resolved, O_RDONLY);
    if (dfd < 0)
        return;
    (void)fsync(dfd);
    (void)close(dfd);
}

static void rotate_if_needed(void)
{
    char cur[ND_PATH_MAX];
    char old[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(cur, sizeof cur, ND_PATH_CRASH_LOG) != ND_OK)
        return;
    if (nd_path_resolve(old, sizeof old, ND_PATH_CRASH_LOG_1) != ND_OK)
        return;
    if (stat(cur, &st) == 0 && st.st_size > (off_t)ND_CRASH_LOG_MAX_BYTES)
        (void)rename(cur, old);
}

const char *nd_crash_log(const char *source, const nd_crash_info *info, const char *note)
{
    char path[ND_PATH_MAX];
    char uptime[64];
    char mem[128];
    char summary[CRASH_SUMMARY_MAX];
    char stamp[32];
    time_t now;
    struct tm tmv;
    FILE *f;
    bool sim;

    if (source == NULL)
        source = "app";
    sim = nd_crash_is_simulation();

    read_first_field("/proc/uptime", uptime, sizeof uptime, "?");
    if (strcmp(uptime, "?") != 0)
        (void)nd_strlcat(uptime, "s", sizeof uptime);
    /* /proc/uptime is NOT under ND_ROOT; read_first_field resolves, which is a
     * plain copy in production and, in a test root, correctly finds nothing
     * and reports "?" rather than a host figure that means nothing. */
    read_mem_available(mem, sizeof mem);
    (void)nd_crash_summary(info, summary, sizeof summary);

    now = time(NULL);
    (void)localtime_r(&now, &tmv);
    if (strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv) == 0u)
        (void)nd_strlcpy(stamp, "?", sizeof stamp);

    if (nd_mkdir_p(ND_PATH_LOG_DIR, 0755u) != ND_OK)
        return NULL;
    rotate_if_needed();

    if (nd_path_resolve(path, sizeof path, ND_PATH_CRASH_LOG) != ND_OK)
        return NULL;
    f = fopen(path, "a");
    if (f == NULL)
        return NULL;

    (void)fprintf(f, "============================================================\n");
    (void)fprintf(f, "time:   %s (epoch %lld)\n", stamp, (long long)now);
    (void)fprintf(f, "mode:   %s\n", sim ? "QEMU/simulation" : "hardware");
    (void)fprintf(f, "source: %s\n", source);
    (void)fprintf(f, "uptime: %s   mem: %s\n", uptime, mem);
    if (note != NULL && note[0] != '\0')
        (void)fprintf(f, "note:   %s\n", note);
    if (info == NULL) {
        (void)fprintf(f, "(no exception info available)\n");
    } else if (info->from_signal) {
        (void)fprintf(f, "signal: %d (%s)  si_code %d  addr %p\n", info->signo,
                      nd_crash_signal_name(info->signo), info->si_code, info->fault_addr);
        if (info->detail[0] != '\0')
            (void)fprintf(f, "detail: %s\n", info->detail);
    } else {
        (void)fprintf(f, "exit:   status %d\n", info->exit_status);
        if (info->detail[0] != '\0')
            (void)fprintf(f, "detail: %s\n", info->detail);
    }

    (void)fflush(f);
    /* Survive an immediate power pull -- a crash is exactly the moment the
     * battery might come out. */
    (void)fsync(fileno(f));
    (void)fclose(f);

    /* fsync(file) persists the data, but a newly created file's DIRECTORY
     * ENTRY is only durable once its parent is synced. CrashHandler is the one
     * writer in the project that does this, and it is right to. */
    fsync_dir(ND_PATH_LOG_DIR);
    fsync_dir(ND_PATH_USER);

    if (sim) {
        nd_log(ND_LOG_CRASH, "%s: %s (report -> %s)", source,
               summary[0] != '\0' ? summary : "(no exception info)", ND_PATH_CRASH_LOG);
    }
    return ND_PATH_CRASH_LOG;
}

/* ------------------------------------------------------------------ *
 * The screen
 * ------------------------------------------------------------------ */

static void flush_input(nd_ui *ui)
{
    /* CrashHandler._flush_input() polls with a 0.0 timeout, which is exactly
     * nd_input_drain()'s contract. MessageDialog uses the same 0.0 and
     * PagedList uses 0.01; nd_input.h insists those stay apart, and they do. */
    if (ui != NULL && ui->input != NULL)
        nd_input_drain(ui->input);
}

void nd_crash_draw_engineering(nd_ui *ui, const char *summary)
{
    int32_t screen_w;
    int32_t screen_h;
    int32_t content_bottom;
    nd_image *crash_img;
    bool painted = false;
    nd_softkey bar;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return;

    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);
    content_bottom = nd_ui_content_bottom(ui);

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);

    /* NOT through the image cache: the Python opens the file directly here,
     * and a 240x175 RGB frame has no business taking one of the 32 icon slots
     * on the way out of a crash. */
    crash_img = nd_image_open(ND_PATH_CRASH_IMAGE);
    if (crash_img != NULL) {
        nd_image *scaled = nd_image_resize_lanczos(crash_img, screen_w, screen_h);

        nd_image_free(crash_img);
        if (scaled != NULL) {
            /* Full frame; the softkey bar deliberately overlays the bottom. */
            painted = nd_image_blit(ui->canvas, scaled, 0, 0) == ND_OK;
            nd_image_free(scaled);
        }
    }

    if (!painted) {
        const nd_font *font = ui->font_xl != NULL
                                  ? ui->font_xl
                                  : (ui->font_n != NULL ? ui->font_n : ui->font_s);

        if (font != NULL) {
            int32_t w = 0;
            int32_t h = 0;

            nd_ui_text_size(ui, "CRASH", font, &w, &h);
            (void)nd_draw_text(ui->draw, (screen_w - w) / 2,
                               nd_max32(0, (content_bottom - h) / 2), "CRASH", font, ND_WHITE);
        }
    }

    /* One-line exception summary strip, so the error is visible on-device. */
    if (summary != NULL && summary[0] != '\0' && ui->font_s != NULL) {
        int32_t tw = 0;
        int32_t th = 0;

        nd_ui_text_size(ui, summary, ui->font_s, &tw, &th);
        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, th + 4), ND_BLACK);
        (void)nd_draw_text(ui->draw, 2, 2, summary, ui->font_s, ND_WHITE);
    }

    /* present=false, then one explicit flush: SoftKeyBar(ui).update("Continue",
     * present=False) followed by ui.fb.update(ui.canvas). */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Continue", false);
    (void)nd_ui_present(ui);
}

static void wait_for_continue(nd_ui *ui)
{
    for (;;) {
        int32_t key = nd_ui_wait_for_key(ui);
        size_t i;

        for (i = 0u; i < ND_CRASH_CONTINUE_KEY_COUNT; i++) {
            if (key == ND_CRASH_CONTINUE_KEYS[i])
                return;
        }
        /* An incoming call must not be trapped behind a crash screen. */
        if (key == ND_KEY_INCOMING_CALL)
            return;
    }
}

void nd_crash_show_app(struct nd_ui *ui, const char *message, const char *app_name,
                       const nd_crash_info *info)
{
    char summary[CRASH_SUMMARY_MAX];

    (void)nd_crash_log(app_name != NULL ? app_name : "app", info, NULL);

    if (ui == NULL)
        return;

    (void)nd_crash_summary(info, summary, sizeof summary);

    if (ui->engineering_mode) {
        flush_input(ui);
        nd_crash_draw_engineering(ui, summary[0] != '\0' ? summary : NULL);
        wait_for_continue(ui);
        return;
    }

    {
        nd_msgdialog dlg;
        char text[ND_CRASH_DETAIL_MAX + CRASH_SUMMARY_MAX + 2];

        if (message == NULL)
            message = ND_CRASH_DEFAULT_NOTICE;
        if (summary[0] != '\0')
            (void)nd_snprintf(text, sizeof text, "%s\n%s", message, summary);
        else
            (void)nd_strlcpy(text, message, sizeof text);

        nd_msgdialog_init(&dlg, ui, text);
        (void)nd_msgdialog_show(&dlg);
    }
}
