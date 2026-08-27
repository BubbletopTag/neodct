/* MicTest -- pick a capture device, watch what it hears.
 *
 * An engineering app for the question "is the microphone working at all?",
 * which on this phone has never had a straight answer. The modem's call audio
 * picks a capture device by itself and reports failure only in a log line, so a
 * dead microphone and a working one look identical from the outside until
 * somebody is on a call complaining they cannot be heard.
 *
 * Two screens. The first lists every capture device the kernel is offering,
 * because "default" is not a useful answer here: QEMU's USB Audio is a
 * playback-only card 0, and a phone with a USB microphone plugged in has at
 * least two cards that have to be told apart. The second draws what the chosen
 * one is capturing, as a waveform, live.
 *
 * ============ NO IN-PROCESS ALSA ============
 *
 * alsa-lib is not in the image; alsa-utils is. So capture is `arecord` on a
 * pipe, which is exactly how nd_modem_audio.c does call audio, and that file is
 * the precedent for the format, the rate and the plughw device string.
 *
 * The child writes raw S16_LE to a pipe and this reads it. The pipe is the
 * back-pressure: when the screen is slower than 8 kHz the pipe fills, arecord
 * blocks, and the waveform shows the most recent chunk rather than falling
 * further and further behind. That is the right failure for a level display --
 * a stale waveform is worse than a dropped one.
 *
 * ============ WHAT IS NOT HERE ============
 *
 * No recording to a file, no playback, no gain control. This app answers one
 * question. Tones and MusicPlayer own the speaker; the mixer is amixer's job.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "mictest.h"

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_log.h"
#include "nd_mic.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_vclock.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

const char *const nd_mictest_title = "MicTest";
const char *const nd_mictest_no_device =
    "No capture device.\n\nNothing on this phone has a pcm*c node -- there is no "
    "microphone for ALSA to offer.";
const char *const nd_mictest_no_arecord = "This build has no arecord.";

#define MICTEST_KEY_BACK 14
#define MICTEST_APP_ID   9002

/* ------------------------------------------------------------------ *
 * The capture child
 * ------------------------------------------------------------------ */

/* nd_proc_spawn() execve()s a path rather than searching PATH, so the search
 * happens here -- the same shape as nd_modem_audio.c's which_exec(). */
static bool which_arecord(char *out, size_t out_sz)
{
    static const char *const dirs[] = {"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(dirs); i++) {
        if (nd_snprintf(out, out_sz, "%s/arecord", dirs[i]) != ND_OK)
            continue;
        if (access(out, X_OK) == 0)
            return true;
    }
    return false;
}

/* Start arecord on `device` with its stdout on a pipe. Returns the read end, or
 * -1; `pid_out` gets the child so the screen can stop it on the way out. */
static int start_capture(const char *device, pid_t *pid_out)
{
    nd_mic_command cmd;
    nd_proc_spec spec;
    char exe[ND_PATH_MAX];
    int pipefd[2];
    int devnull;
    pid_t pid = -1;

    *pid_out = -1;

    if (nd_mic_record_command(&cmd, device, ND_MIC_RATE) != ND_OK)
        return -1;
    if (!which_arecord(exe, sizeof exe))
        return -1;
    /* O_CLOEXEC on both: the child gets its ends by dup2 below, and inheriting
     * the READ end would mean this never sees end-of-file when arecord dies. */
    if (pipe2(pipefd, O_CLOEXEC) != 0)
        return -1;
    devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        return -1;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = cmd.argv;
    spec.owner = ND_OWNER_AUDIO;
    spec.fds[0].child_fd = 1;
    spec.fds[0].our_fd = pipefd[1];
    /* stderr to /dev/null even though -q is set: a device that disappears mid
     * capture makes arecord chatty, and that chatter would be drawn over the
     * serial console during a test that is already hard enough to read. */
    spec.fds[1].child_fd = 2;
    spec.fds[1].our_fd = devnull;
    spec.n_fds = 2u;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        (void)close(pipefd[0]);
        (void)close(pipefd[1]);
        (void)close(devnull);
        return -1;
    }

    (void)close(pipefd[1]);
    (void)close(devnull);
    *pid_out = pid;
    return pipefd[0];
}

static void stop_capture(int fd, pid_t pid)
{
    nd_proc_status st;

    if (pid > 0) {
        /* SIGKILL, not SIGTERM, for nd_modem_audio.c's reason: an arecord stuck
         * in D-state on a device that has been unplugged would hold the card
         * open, and the next start would find it busy. */
        (void)kill(pid, SIGKILL);
        (void)nd_proc_wait(pid, 1.0, &st);
    }
    if (fd >= 0)
        (void)close(fd);
}

/* ------------------------------------------------------------------ *
 * The waveform screen
 * ------------------------------------------------------------------ */

static void draw_waveform(nd_ui *ui, const char *label, const nd_mic_column *columns, size_t n,
                          bool live)
{
    nd_rect band;
    size_t i;

    band.x0 = 0;
    band.y0 = 0;
    band.x1 = ND_UI_W - 1;
    band.y1 = ND_UI_H - 1;
    (void)nd_draw_rect_fill(ui->draw, band, ND_BLACK);

    (void)nd_draw_text(ui->draw, 5, 0, nd_mictest_title, ui->font_xl, ND_WHITE);
    (void)nd_draw_text(ui->draw, 5, 22, label, ui->font_s, ND_GRAY);

    /* The silence line, drawn first so the waveform sits on top of it. Without
     * it a dead microphone draws nothing at all, and nothing at all looks like
     * a crashed app rather than like silence. */
    {
        int32_t middle = nd_mic_sample_y(0, ND_MICTEST_BAND_TOP, ND_MICTEST_BAND_HEIGHT);

        (void)nd_draw_line(ui->draw, 8, middle, 8 + ND_MICTEST_COLUMNS - 1, middle, ND_GRAY, 1);
    }

    for (i = 0u; i < n; i++) {
        int32_t x = 8 + (int32_t)i;
        int32_t y_hi = nd_mic_sample_y(columns[i].max, ND_MICTEST_BAND_TOP, ND_MICTEST_BAND_HEIGHT);
        int32_t y_lo = nd_mic_sample_y(columns[i].min, ND_MICTEST_BAND_TOP, ND_MICTEST_BAND_HEIGHT);

        (void)nd_draw_line(ui->draw, x, y_hi, x, y_lo, ND_WHITE, 1);
    }

    if (!live) {
        (void)nd_draw_text(ui->draw, 8, ND_UI_H - 30, "no samples -- is the device in use?",
                           ui->font_s, ND_GRAY);
    }
}

/* One screenful of samples, or as many as have arrived. Returns how many
 * columns were filled; 0 means the pipe had nothing this time round, which is
 * normal and is not an error. */
static size_t read_columns(int fd, int16_t *samples, nd_mic_column *columns)
{
    ssize_t got;

    got = read(fd, samples, ND_MICTEST_CHUNK * sizeof samples[0]);
    if (got <= 0)
        return 0u;
    return nd_mic_reduce(samples, (size_t)got / sizeof samples[0], columns, ND_MICTEST_COLUMNS);
}

static void watch(nd_ui *ui, const nd_mic_device *device)
{
    static int16_t samples[ND_MICTEST_CHUNK];
    static nd_mic_column columns[ND_MICTEST_COLUMNS];
    nd_softkey softkey;
    size_t n_columns = 0u;
    double last_sound = 0.0;
    bool drawn = false;
    pid_t pid = -1;
    int fd;

    fd = start_capture(device->device, &pid);
    if (fd < 0) {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, ui, nd_mictest_no_arecord);
        nd_msgdialog_set_title(&dlg, nd_mictest_title);
        (void)nd_msgdialog_show(&dlg);
        return;
    }
    nd_log(ND_LOG_MICTEST, "MicTest: capturing from %s", device->device);

    /* Blocking reads would freeze the screen -- and the Back key with it -- for
     * as long as the device took to produce a chunk. A device that produces
     * nothing at all is exactly the case this app exists to show. */
    (void)fcntl(fd, F_SETFL, O_NONBLOCK);

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        size_t got = read_columns(fd, samples, columns);
        int32_t key;

        if (got > 0u) {
            n_columns = got;
            last_sound = nd_time_monotonic();
        }

        /* Redraw only when there is something new to draw, plus once at the
         * start so the screen is not blank while waiting for the first chunk.
         * The read is non-blocking, so without this the loop would repaint the
         * whole 240x175 band twenty times a second whether or not a sample had
         * arrived -- on a single-core Cortex-A7 that is a lot of work to
         * produce an identical frame. arecord delivers a chunk about ten times
         * a second, so this settles at the rate the audio actually arrives. */
        if (got > 0u || !drawn) {
            draw_waveform(ui, device->label[0] != '\0' ? device->label : device->device, columns,
                          n_columns, last_sound > 0.0);
            nd_softkey_update(&softkey, "Back", false);
            if (nd_ui_present(ui) != ND_OK)
                break;
            drawn = true;
        }

        key = nd_ui_read_keypress(ui, 0.05);
        if (key == MICTEST_KEY_BACK)
            break;
        if (nd_app_should_exit())
            break;
    }

    stop_capture(fd, pid);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_mic_device devices[ND_MICTEST_MAX_DEVICES];
    size_t n_devices;

    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return 1;

    for (;;) {
        char lines[ND_MICTEST_MAX_DEVICES][ND_MIC_LABEL_MAX + ND_MIC_DEVICE_MAX + 4];
        const char *items[ND_MICTEST_MAX_DEVICES];
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;
        size_t i;

        /* Rescanned every time round rather than once: a USB microphone can be
         * plugged in while this screen is up, and the obvious thing to do after
         * plugging one in is to look at the list again. */
        n_devices = nd_mic_scan(ND_MIC_ASOUND_DIR, devices, ND_ARRAY_LEN(devices));
        if (n_devices == 0u) {
            nd_msgdialog dlg;

            nd_msgdialog_init(&dlg, ui, nd_mictest_no_device);
            nd_msgdialog_set_title(&dlg, nd_mictest_title);
            (void)nd_msgdialog_show(&dlg);
            return 0;
        }

        for (i = 0u; i < n_devices; i++) {
            if (devices[i].label[0] != '\0')
                (void)nd_snprintf(lines[i], sizeof lines[i], "%s  %s", devices[i].device,
                                  devices[i].label);
            else
                (void)nd_strlcpy(lines[i], devices[i].device, sizeof lines[i]);
            items[i] = lines[i];
        }

        nd_vlist_init(&menu, ui, nd_mictest_title, items, n_devices, MICTEST_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Listen", false);

        choice = nd_vlist_show(&menu);
        if (choice < 0)
            return 0;
        if ((size_t)choice < n_devices)
            watch(ui, &devices[choice]);

        if (nd_app_should_exit())
            return 0;
    }
}

/* arecord is killed before the waveform screen is left, so by the time this can
 * be called there is no child and no pipe. The symbol exists because nd_app.h
 * requires every app to export one. */
void app_shutdown(void) {}
