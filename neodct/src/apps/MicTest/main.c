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
 * ============ THE GAIN, AND WHY IT MOVED IN HERE ============
 *
 * This used to say "no gain control ... the mixer is amixer's job", and for
 * as long as the mixer was set once at boot that was true. It is not:
 * /etc/init.d/S17audio raises the capture switch and level ONCE, out of rcS,
 * and nothing else in the image ever touches them again -- no alsactl, no
 * asound.state, no udev rule. A USB card that re-enumerates comes back with
 * the driver's defaults, which on the C-Media adapter the electret is
 * soldered to means capture switched OFF and the level at ZERO, and arecord
 * reports a clean recording of nothing.
 *
 * So this app now owns the mixer for the device it is listening to. It
 * re-applies switch and level BEFORE arecord opens the card -- which is the
 * same thing nd_modem_audio.c does before every call, through the same
 * nd_mic_apply_gain() -- and UP and DOWN move the level while you listen, so
 * "is the microphone working" and "is it loud enough" get answered on one
 * screen by one person with no serial console.
 *
 * The level is written to settings.prop ONCE, when the screen closes. Never
 * per keypress: R-24 in nd_settings.h means one write is one full rewrite of
 * the file with an fsync, and holding UP would be eight of them.
 *
 * ============ WHAT IS STILL NOT HERE ============
 *
 * No recording to a file and no playback. This app answers one question.
 * Tones and MusicPlayer own the speaker. Auto Gain Control is left alone for
 * the reason nd_mic.h gives: it pumps, and that is a taste question with no
 * right answer where the level is a correctness one.
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
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_mic.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_theme.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

const char *const nd_mictest_title = "MicTest";
const char *const nd_mictest_no_device = "No capture device.\n\nNothing here has a pcm*c node.";
const char *const nd_mictest_no_arecord = "This build has no arecord.";

/* nd_keycodes.h's names rather than the local 14 this used to carry: UP and
 * DOWN join Back on this screen and spelling one of the three differently
 * from the other two would be an invitation to get one of them wrong. */
#define MICTEST_APP_ID 9002

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
 * The three decisions -- see mictest.h
 * ------------------------------------------------------------------ */

int32_t nd_mictest_step(int32_t gain, int32_t key)
{
    if (key == ND_KEY_UP)
        gain += ND_MIC_GAIN_STEP;
    else if (key == ND_KEY_DOWN)
        gain -= ND_MIC_GAIN_STEP;
    /* Every other key -- including the -1 nd_ui_read_keypress() returns when
     * nothing was pressed -- falls straight through to the clamp, which hands
     * an in-range gain back unchanged. That is what lets the loop pass this
     * whatever it read without a branch of its own. */
    return nd_clamp32(gain, ND_MIC_GAIN_MIN, ND_MIC_GAIN_MAX);
}

bool nd_mictest_save_gain(int32_t gain, int32_t loaded)
{
    char text[16];

    if (gain == loaded)
        return false;
    if (gain < ND_MIC_GAIN_MIN || gain > ND_MIC_GAIN_MAX)
        return false;
    if (nd_snprintf(text, sizeof text, "%d", (int)gain) != ND_OK)
        return false;
    if (nd_settings_set(ND_SET_HW_MIC_GAIN, text) != ND_OK) {
        nd_log_err(ND_LOG_MICTEST, "could not save the mic gain (%d%%)", (int)gain);
        return false;
    }
    nd_log(ND_LOG_MICTEST, "Mic gain saved: %d%%", (int)gain);
    return true;
}

int32_t nd_mictest_peak_percent(const nd_mic_column *columns, size_t n)
{
    int32_t peak = 0;
    size_t i;

    if (columns == NULL)
        return 0;
    for (i = 0u; i < n; i++) {
        /* Negated in int32_t, not in int16_t: -(-32768) does not fit an
         * int16_t and the sample that clips hardest is exactly the one a
         * level display must not silently lose. */
        int32_t hi = columns[i].max;
        int32_t lo = -(int32_t)columns[i].min;

        if (hi > peak)
            peak = hi;
        if (lo > peak)
            peak = lo;
    }
    /* Scaled against POSITIVE full scale, so the two halves read the same
     * number for the same loudness. -32768 works out at 100.003, which
     * integer division truncates to 100 on its own; the clamp is there so
     * that a reading above 100 can never appear on a level meter whatever a
     * future caller hands in. */
    return nd_min32((int32_t)(((int64_t)peak * 100) / 32767), 100);
}

/* ------------------------------------------------------------------ *
 * The waveform screen
 * ------------------------------------------------------------------ */

static void draw_waveform(nd_ui *ui, const char *label, const nd_mic_column *columns, size_t n)
{
    nd_rect band;
    size_t i;

    band.x0 = 0;
    band.y0 = 0;
    band.x1 = ND_UI_W - 1;
    band.y1 = ND_UI_H - 1;
    nd_ui_paint_chrome(ui, band);

    nd_theme_text_light(ui->draw, 5, 0, nd_mictest_title, ui->font_xl);
    nd_theme_text(ui->draw, 5, 22, label, ui->font_s, ND_TH_SKY_TOP, ND_RGB(0x08, 0x1E, 0x33));

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
}

/* The reserved line between the waveform and the softkey bar: what the owner
 * is setting on the left, what the microphone is hearing on the right.
 *
 * Two draws rather than one string, and right-aligned rather than tabbed,
 * because the right half is sometimes "no samples -- device in use?" and
 * sometimes eight characters, and a single formatted line would have to be
 * short enough for the long case on a 240-pixel screen. The gain is white and
 * the reading grey: one of them is a control and the other is a measurement,
 * and on a screen with no cursor that difference has to be drawn. */
static void draw_readout(nd_ui *ui, int32_t gain, int32_t peak, bool live)
{
    char left[24];
    char right[32];
    int32_t w = 0;
    int32_t h = 0;

    (void)nd_snprintf(left, sizeof left, "Gain %d%%", (int)gain);
    nd_theme_text_light(ui->draw, 8, ND_MICTEST_READOUT_Y, left, ui->font_s);

    if (live)
        (void)nd_snprintf(right, sizeof right, "Peak %d%%", (int)peak);
    else
        (void)nd_strlcpy(right, "no samples -- device in use?", sizeof right);
    nd_text_size(ui->font_s, right, &w, &h);
    nd_theme_text(ui->draw, ND_UI_W - 8 - w, ND_MICTEST_READOUT_Y, right, ui->font_s, ND_TH_SKY_TOP,
                  ND_RGB(0x08, 0x1E, 0x33));
}

/* ============ THE MIXER, AND WHY IT IS LISTED ONCE ============
 *
 * The card is opened, listed and set up when the screen opens; after that
 * only the LEVEL controls move, and only when a key moves them.
 *
 * Every one of those is a fork, an exec and a wait, and UP and DOWN repeat
 * every 120 ms (ND_REPEAT_INTERVAL_S) once a key has been held for 400. A
 * screen that re-listed the card and re-asserted its switch on every press
 * would spend about twenty-four amixer children a second on a single-core
 * Cortex-A7 to answer a question that cannot have changed and to turn on a
 * switch that is already on. So the listing is kept, the switches are set
 * once, and a keypress costs one child per volume control -- which on the
 * card this was written for is one.
 *
 * A device the owner named by hand -- "hw:1,0", "default" -- carries no card
 * number nd_mic_card_of() can read. That is not an error: they asked for that
 * device and they get it, with whatever the mixer already had. `n_levels` is
 * then 0 and every call below is a no-op. */
typedef struct {
    int32_t card;
    nd_mic_control levels[ND_MIC_CONTROLS_MAX]; /* the volumes, without the switches */
    size_t n_levels;
} mic_mixer;

static void mixer_open(mic_mixer *mx, const nd_mic_device *device, int32_t gain)
{
    nd_mic_control all[ND_MIC_CONTROLS_MAX];
    size_t n = 0u;
    size_t i;

    memset(mx, 0, sizeof *mx);
    if (!nd_mic_card_of(device->device, &mx->card))
        return;
    if (nd_mic_list_controls(mx->card, all, ND_ARRAY_LEN(all), &n) != ND_OK)
        return;

    /* Switch AND level, once, before arecord opens the card. */
    (void)nd_mic_set_controls(mx->card, all, n, gain);

    for (i = 0u; i < n; i++) {
        if (all[i].kind == ND_MIC_CTL_VOLUME)
            mx->levels[mx->n_levels++] = all[i];
    }
}

/* THE WORST CASE FOR ONE KEYPRESS IS 1.0 s, and it is worth knowing where.
 *
 * nd_mic_set_controls() gives all of its csets together ONE
 * ND_MIC_MIXER_BUDGET_S and pays at most one ND_MIC_MIXER_REAP_S on top for a
 * child that blew it and had to be SIGKILLed -- 1.0 s, however many levels
 * this card has. A working amixer answers in about 20 ms, so that number is
 * only ever reached by a card that has WEDGED (the likeliest place being
 * snd_ctl_open() on a USB card halfway through re-enumerating, which is the
 * same card this screen is recording from). It does not accumulate across a
 * held key: the repeat is 120 ms but the call is synchronous, so a wedged
 * card gives one 1.0 s pause per press rather than a queue of them. */
static void mixer_set_level(const mic_mixer *mx, int32_t gain)
{
    if (mx->n_levels == 0u)
        return;
    (void)nd_mic_set_controls(mx->card, mx->levels, mx->n_levels, gain);
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
    mic_mixer mixer;
    nd_softkey softkey;
    size_t n_columns = 0u;
    double last_sound = 0.0;
    bool drawn = false;
    bool dirty = false;
    int32_t loaded;
    int32_t gain;
    pid_t pid = -1;
    int fd;

    /* Read ONCE, here, and written at most once on the way out. R-24: a read
     * costs a full rewrite of settings.prop with an fsync just like a write
     * does, so this is the only place either happens. */
    loaded = nd_mic_gain_from_setting(nd_settings_get(ND_SET_HW_MIC_GAIN, ND_SET_HW_MIC_GAIN_DFLT));
    gain = loaded;

    /* BEFORE arecord, deliberately. A card that re-enumerated since boot has
     * its capture switch off and its level at zero, and opening it first
     * would draw a flat line that looks exactly like dead hardware -- which
     * is the very fault this app exists to tell apart from a working one. */
    mixer_open(&mixer, device, gain);

    fd = start_capture(device->device, &pid);
    if (fd < 0) {
        nd_msgdialog dlg;

        nd_msgdialog_init(&dlg, ui, nd_mictest_no_arecord);
        nd_msgdialog_set_title(&dlg, nd_mictest_title);
        (void)nd_msgdialog_show(&dlg);
        return;
    }
    nd_log(ND_LOG_MICTEST, "MicTest: capturing from %s at %d%%", device->device, (int)gain);

    /* Blocking reads would freeze the screen -- and the Back key with it -- for
     * as long as the device took to produce a chunk. A device that produces
     * nothing at all is exactly the case this app exists to show. */
    (void)fcntl(fd, F_SETFL, O_NONBLOCK);

    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        size_t got = read_columns(fd, samples, columns);
        int32_t key;
        int32_t moved;

        if (got > 0u) {
            n_columns = got;
            last_sound = nd_time_monotonic();
        }

        /* Redraw only when there is something new to draw, plus once at the
         * start so the screen is not blank while waiting for the first chunk,
         * plus once after a key has moved the gain -- a level that does not
         * repaint until the next chunk arrives reads as a key that did not
         * work, and on a silent microphone the next chunk is the whole point
         * of the press. The read is non-blocking, so without this the loop
         * would repaint the whole 240x175 band twenty times a second whether
         * or not a sample had arrived -- on a single-core Cortex-A7 that is a
         * lot of work to produce an identical frame. arecord delivers a chunk
         * about ten times a second, so this settles at the rate the audio
         * actually arrives. */
        if (got > 0u || !drawn || dirty) {
            draw_waveform(ui, device->label[0] != '\0' ? device->label : device->device, columns,
                          n_columns);
            draw_readout(ui, gain, nd_mictest_peak_percent(columns, n_columns), last_sound > 0.0);
            nd_softkey_update(&softkey, "Back", false);
            if (nd_ui_present(ui) != ND_OK)
                break;
            drawn = true;
            dirty = false;
        }

        key = nd_ui_read_keypress(ui, 0.05);
        if (key == ND_KEY_BACK)
            break;

        /* Every other key goes through step(), which returns the gain
         * unchanged for all of them. Applied at once so the owner hears the
         * difference while the key is still under their thumb; NOT saved
         * here, for R-24's sake. */
        moved = nd_mictest_step(gain, key);
        if (moved != gain) {
            gain = moved;
            mixer_set_level(&mixer, gain);
            dirty = true;
        } else if (key == ND_KEY_UP || key == ND_KEY_DOWN) {
            /* At an end stop. Repaint anyway: a press that changes nothing
             * still has to look like it was received. */
            dirty = true;
        }

        if (nd_app_should_exit())
            break;
    }

    stop_capture(fd, pid);
    /* ONE write, here, and only if the number actually moved. */
    (void)nd_mictest_save_gain(gain, loaded);
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
