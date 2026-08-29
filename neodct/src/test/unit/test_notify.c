/* test_notify.c -- NotifyService: the banner, the beeps and the ringer.
 *
 * NotifyService has no Python test of any kind (spec-core-services.md says so
 * in as many words), so there is no recorded oracle to transcribe. The oracle
 * used here is the Python source itself, read line by line: every case below
 * names the behaviour it pins and, where a number or a string looks odd, says
 * which line of System/core/NotifyService/__init__.py it came from.
 *
 * ============ WHAT CAN AND CANNOT BE TESTED ON THE HOST ============
 *
 * There is no sound card on the machine the golden frames are rendered on,
 * and no aplay or mpv either. That is not a problem, because the interesting
 * parts of this module are not the sound card:
 *
 *   * the banner state machine is four fields and no I/O at all;
 *   * ringtone_path() is a six-step file-system search whose ORDER is the
 *     whole specification, and a scratch root reproduces every step;
 *   * the streaming decoder -- the part that replaces the Python's 6.4 MB
 *     whole-file decode -- is a pure function from a file to a stream of
 *     frames, and its loop point is checkable to the sample;
 *   * the player is reached by execve, so a shell script called "aplay" on
 *     PATH is indistinguishable from the real one as far as this module is
 *     concerned. `head -c 65536 > file` is a perfectly good sound card if all
 *     you want to know is what the bytes were.
 *
 * The last one is how test_the_ringer_streams_the_tone_to_the_player checks
 * 16384 output frames against the source, wrap included. What is NOT covered
 * here is ALSA itself -- whether the phone's card accepts S16_LE stereo at
 * 44100 -- and that belongs in tests/hw, on the device.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_notify.h"
#include "nd_settings.h"
#include "nd_types.h"

#include "../../lib/nd_notify_priv.h"

#include "platform_test.h"

#define SETTINGS_PROP "/User/settings.prop"
#define VERSION_PROP  "/System/version.prop"

/* ------------------------------------------------------------------ *
 * Scaffolding
 * ------------------------------------------------------------------ */

static char g_saved_path[4096];
static bool g_path_saved;

static void use_scratch_settings(void)
{
    nd_settings_set_paths(SETTINGS_PROP, VERSION_PROP);
    pt_write_text(VERSION_PROP, "system.os.versionnumber=0.3.2a\n");
}

static void set_ringtone(const char *value)
{
    char body[ND_PATH_MAX + 64];

    (void)nd_snprintf(body, sizeof body, "system.audio.ringtone=%s\n", value);
    pt_write_text(SETTINGS_PROP, body);
}

/* PATH is restored after every case by pt_new_case()'s caller below, so a
 * test that hides aplay cannot leak that into the next one. */
static void save_path(void)
{
    const char *p = getenv("PATH");

    (void)nd_strlcpy(g_saved_path, p != NULL ? p : "", sizeof g_saved_path);
    g_path_saved = true;
}

static void restore_path(void)
{
    if (!g_path_saved)
        return;
    (void)setenv("PATH", g_saved_path, 1);
    g_path_saved = false;
}

/* A directory of fake players, first on PATH. `with_real_tools` decides
 * whether the scripts inside can still find head/sleep by name -- some cases
 * want a PATH with nothing on it but the fake. */
static void use_fake_bin(bool with_real_tools)
{
    char resolved[ND_PATH_MAX];
    char value[4096];

    pt_mkdir("/fakebin");
    if (nd_path_resolve(resolved, sizeof resolved, "/fakebin") != ND_OK)
        return;
    if (!g_path_saved)
        save_path();
    if (with_real_tools)
        (void)nd_snprintf(value, sizeof value, "%s:%s", resolved, g_saved_path);
    else
        (void)nd_strlcpy(value, resolved, sizeof value);
    (void)setenv("PATH", value, 1);
}

static void write_script(const char *name, const char *body)
{
    char virt[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];

    (void)nd_snprintf(virt, sizeof virt, "/fakebin/%s", name);
    pt_write_text(virt, body);
    if (nd_path_resolve(resolved, sizeof resolved, virt) == ND_OK)
        (void)chmod(resolved, 0755u);
}

/* head and sleep are used by the fake players. Absolute, because a case that
 * empties PATH still needs them. */
static const char *find_tool(const char *name)
{
    static char buf[ND_PATH_MAX];
    static const char *const DIRS[] = {"/bin", "/usr/bin", "/usr/local/bin"};
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(DIRS); i++) {
        if (nd_snprintf(buf, sizeof buf, "%s/%s", DIRS[i], name) == ND_OK && access(buf, X_OK) == 0)
            return buf;
    }
    return NULL;
}

/* --- stdout capture, so a test can assert on what reached the console --- */

static int g_saved_stdout = -1;

static void capture_begin(void)
{
    char resolved[ND_PATH_MAX];
    int fd;

    pt_write_text("/capture.log", "");
    (void)fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    if (nd_path_resolve(resolved, sizeof resolved, "/capture.log") != ND_OK)
        return;
    fd = open(resolved, O_WRONLY | O_TRUNC);
    if (fd >= 0) {
        (void)dup2(fd, STDOUT_FILENO);
        (void)close(fd);
    }
}

static void capture_end(char *out, size_t out_sz)
{
    (void)fflush(stdout);
    if (g_saved_stdout >= 0) {
        (void)dup2(g_saved_stdout, STDOUT_FILENO);
        (void)close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (pt_read_text("/capture.log", out, out_sz) == (size_t)-1)
        out[0] = '\0';
}

/* --- a mono 16-bit PCM WAV, written byte by byte ------------------- *
 *
 * Explicit little-endian assembly rather than a struct write:
 * CODING-STANDARDS.md section 6 bans endianness assumptions in file formats,
 * and a test fixture that is only correct on x86 is worse than no fixture. */

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_wav_mono16(const char *virt_path, uint32_t rate, const int16_t *samples,
                             size_t count)
{
    uint8_t *buf;
    uint32_t data_bytes = (uint32_t)(count * 2u);
    size_t i;

    buf = malloc(44u + count * 2u);
    if (buf == NULL) {
        fprintf(stderr, "out of memory building a WAV fixture\n");
        exit(1);
    }
    memcpy(buf, "RIFF", 4u);
    put_u32le(buf + 4, 36u + data_bytes);
    memcpy(buf + 8, "WAVEfmt ", 8u);
    put_u32le(buf + 16, 16u); /* PCM fmt chunk size   */
    put_u16le(buf + 20, 1u);  /* WAVE_FORMAT_PCM      */
    put_u16le(buf + 22, 1u);  /* channels             */
    put_u32le(buf + 24, rate);
    put_u32le(buf + 28, rate * 2u); /* byte rate           */
    put_u16le(buf + 32, 2u);        /* block align          */
    put_u16le(buf + 34, 16u);       /* bits per sample      */
    memcpy(buf + 36, "data", 4u);
    put_u32le(buf + 40, data_bytes);
    for (i = 0u; i < count; i++)
        put_u16le(buf + 44u + i * 2u, (uint16_t)samples[i]);

    pt_write(virt_path, buf, 44u + count * 2u);
    free(buf);
}

/* A ramp that visits both signs and never repeats inside a short file, so a
 * wrap that lands one frame early or late is visible. */
static int16_t ramp(size_t i)
{
    return (int16_t)((int32_t)(i * 37u % 60000u) - 30000);
}

static void fill_ramp(int16_t *dst, size_t count)
{
    size_t i;

    for (i = 0u; i < count; i++)
        dst[i] = ramp(i);
}

static void nap_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static off_t virt_size(const char *virt_path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, virt_path) != ND_OK)
        return -1;
    if (stat(resolved, &st) != 0)
        return -1;
    return st.st_size;
}

/* Poll rather than sleep a fixed time: the feeder is a thread racing a child
 * process and a fixed sleep is either flaky or slow. */
static bool wait_for_size(const char *virt_path, off_t want, long timeout_ms)
{
    long waited = 0;

    while (waited < timeout_ms) {
        if (virt_size(virt_path) >= want)
            return true;
        nap_ms(10);
        waited += 10;
    }
    return virt_size(virt_path) >= want;
}

static void reap_everything(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ------------------------------------------------------------------ *
 * The banner
 * ------------------------------------------------------------------ */

static void test_a_fresh_service_has_nothing_to_show(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(n != NULL);
    CHECK(!nd_notify_active(n));
    CHECK(nd_notify_kind(n) == NULL);
    CHECK_INT(nd_notify_count(n), 0);
    /* latest_data() is None in Python; -1 is the header's spelling of it. */
    CHECK_INT(nd_notify_latest_data(n), -1);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 0);
    CHECK(!nd_notify_ringing(n));
    nd_notify_close(n);
}

static void test_one_message_is_singular(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_sms(n, 41, false);

    CHECK(nd_notify_active(n));
    CHECK_STR(nd_notify_kind(n), ND_NOTIFY_KIND_SMS);
    CHECK_INT(nd_notify_count(n), 1);
    CHECK_INT(nd_notify_latest_data(n), 41);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "1 message");
    CHECK_STR(l2, "received");
    nd_notify_close(n);
}

static void test_more_than_one_message_is_plural_and_keeps_the_newest_row(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_sms(n, 7, false);
    nd_notify_post_sms(n, 8, false);
    nd_notify_post_sms(n, 9, false);

    CHECK_INT(nd_notify_count(n), 3);
    /* _latest_data is overwritten, not accumulated: pressing Read opens the
     * NEWEST arrival, which is what the 3310 did. */
    CHECK_INT(nd_notify_latest_data(n), 9);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "3 messages");
    CHECK_STR(l2, "received");
    nd_notify_close(n);
}

static void test_dismiss_clears_the_banner_and_only_the_banner(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_sms(n, 12, false);
    nd_notify_post_sms(n, 13, false);
    nd_notify_dismiss(n);

    CHECK(!nd_notify_active(n));
    CHECK(nd_notify_kind(n) == NULL);
    CHECK_INT(nd_notify_count(n), 0);
    CHECK_INT(nd_notify_latest_data(n), -1);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 0);

    /* The next arrival counts from one again -- the counter covers the
     * banner, not the inbox. The envelope keeps flashing off the unread
     * count, which lives in SQL and not here. */
    nd_notify_post_sms(n, 14, false);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "1 message");
    nd_notify_close(n);
}

static void test_the_banner_buffers_are_cleared_even_when_there_is_no_banner(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    memset(l1, 'x', sizeof l1);
    memset(l2, 'x', sizeof l2);
    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 0);
    /* The caller draws whatever the count says it may draw, but leaving
     * uninitialised bytes in a buffer this module was handed is how a stale
     * banner appears on a later frame. */
    CHECK_INT(l1[0], 0);
    CHECK_INT(l2[0], 0);
    nd_notify_close(n);
}

/* ------------------------------------------------------------------ *
 * The second kind
 * ------------------------------------------------------------------ *
 *
 * NotifyService had exactly one kind of banner and the string "sms" was the
 * only value `kind` ever held. A calendar reminder is the second, and it uses
 * the same banner on purpose -- same two lines, same position, same Read-or-
 * Clear on the home screen -- because that is the one "something happened"
 * idiom this phone has.
 *
 * These cases fix the shape of that reuse, and in particular the one decision
 * that could reasonably have gone the other way: what happens when both kinds
 * are live at once.
 */

/* A reminder alone: its name, and the clock reading of the occurrence. That
 * second line is the OCCURRENCE's time and not the alarm's -- an alarm set
 * fifteen minutes early still says half past ten, because half past ten is
 * when the appointment is. */
static void test_one_reminder_names_itself_and_its_time(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];
    struct tm tm_in;
    time_t when;

    /* Composed as local wall clock, which is what the calendar stores and
     * what nd_timeset_format_clock() reads back. */
    memset(&tm_in, 0, sizeof tm_in);
    tm_in.tm_year = 2026 - 1900;
    tm_in.tm_mon = 7;
    tm_in.tm_mday = 29;
    tm_in.tm_hour = 10;
    tm_in.tm_min = 30;
    tm_in.tm_isdst = -1;
    when = mktime(&tm_in);

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_event(n, 7, "Dentist", (int64_t)when, false);

    CHECK(nd_notify_active(n));
    CHECK_STR(nd_notify_kind(n), ND_NOTIFY_KIND_EVENT);
    CHECK_INT(nd_notify_count(n), 1);
    /* latest_data is the EVENT's rowid, which is what the "View" softkey
     * hands to app_open_event(). */
    CHECK_INT(nd_notify_latest_data(n), 7);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "Dentist");
    CHECK_STR(l2, "10:30 am");
    nd_notify_close(n);
}

/* A banner is never blank. An event saved with no name still has to say
 * something, and "Reminder" is what it says. */
static void test_an_untitled_reminder_still_says_something(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_event(n, 1, "", 0, false);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "Reminder");

    nd_notify_dismiss(n);
    nd_notify_post_event(n, 2, NULL, 0, false);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "Reminder");
    nd_notify_close(n);
}

/* Several at once count, exactly as several messages do -- the banner has two
 * lines and cannot name three things, so it says how many there are and the
 * app shows the list. */
static void test_more_than_one_reminder_is_counted(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_event(n, 1, "Dentist", 0, false);
    nd_notify_post_event(n, 2, "Team call", 0, false);
    nd_notify_post_event(n, 3, "Bins out", 0, false);

    CHECK_INT(nd_notify_count(n), 3);
    CHECK_INT(nd_notify_latest_data(n), 3); /* the newest, as with texts */
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "3 reminders");
    CHECK_STR(l2, "due");
    nd_notify_close(n);
}

/* THE DECISION THAT COULD HAVE GONE THE OTHER WAY. One banner, the newest
 * news, and the count starts again -- rather than two counters and a home
 * screen that has to say two things in two lines.
 *
 * Nothing is lost by it: the text is still unread in the inbox with the
 * envelope flashing over it, and the appointment is still in the calendar. */
static void test_a_second_kind_takes_the_banner_over(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_sms(n, 41, false);
    nd_notify_post_sms(n, 42, false);
    CHECK_INT(nd_notify_count(n), 2);

    nd_notify_post_event(n, 7, "Dentist", 0, false);
    CHECK_STR(nd_notify_kind(n), ND_NOTIFY_KIND_EVENT);
    CHECK_INT(nd_notify_count(n), 1); /* NOT three */
    CHECK_INT(nd_notify_latest_data(n), 7);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "Dentist");

    /* And the same the other way. */
    nd_notify_post_sms(n, 43, false);
    CHECK_STR(nd_notify_kind(n), ND_NOTIFY_KIND_SMS);
    CHECK_INT(nd_notify_count(n), 1);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "1 message");
    CHECK_STR(l2, "received");
    nd_notify_close(n);
}

/* Two reminders in a row keep counting: take_over() must fire on a CHANGE of
 * kind and not on every post, or a second reminder would reset to one. */
static void test_the_same_kind_twice_does_not_reset(void)
{
    nd_notify *n = NULL;

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_event(n, 1, "A", 0, false);
    nd_notify_post_event(n, 2, "B", 0, false);
    CHECK_INT(nd_notify_count(n), 2);
    nd_notify_close(n);
}

/* Dismiss clears the event's own two fields too, so a later reminder cannot
 * inherit a stale name -- the same reason the two line buffers are cleared
 * even when there is no banner. */
static void test_dismiss_clears_the_reminder_as_well(void)
{
    nd_notify *n = NULL;
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    nd_notify_post_event(n, 7, "Dentist", 0, false);
    nd_notify_dismiss(n);

    CHECK(!nd_notify_active(n));
    CHECK(nd_notify_kind(n) == NULL);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 0);

    nd_notify_post_event(n, 8, "", 0, false);
    CHECK_INT(nd_notify_banner_lines(n, l1, l2), 2);
    CHECK_STR(l1, "Reminder"); /* not "Dentist" */
    nd_notify_close(n);
}

/* ------------------------------------------------------------------ *
 * Beeps
 * ------------------------------------------------------------------ */

static void test_a_missing_tone_is_reported_and_not_played(void)
{
    nd_notify *n = NULL;
    char log[4096];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK(!nd_notify_play_tone(n, ND_TONES_DIR "/nothing-here.wav"));
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Tone missing: " ND_TONES_DIR "/nothing-here.wav") != NULL);
    nd_notify_close(n);
}

static void test_an_existing_tone_is_handed_to_aplay_and_not_waited_for(void)
{
    nd_notify *n = NULL;
    int16_t samples[32];
    const char *head = find_tool("head");
    char script[ND_PATH_MAX * 2];
    char marker[ND_PATH_MAX];

    if (head == NULL) {
        fprintf(stderr, "SKIP: no head(1) to build a fake player with\n");
        return;
    }
    use_fake_bin(true);
    if (nd_path_resolve(marker, sizeof marker, "/played.txt") != ND_OK)
        return;
    /* The fake aplay records the arguments it was given. That is the whole
     * assertion: the Python spawns ["aplay", "-q", path] and nothing else. */
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\n", marker);
    write_script("aplay", script);

    fill_ramp(samples, ND_ARRAY_LEN(samples));
    write_wav_mono16(ND_TONES_DIR "/sms.wav", 44100u, samples, ND_ARRAY_LEN(samples));

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(nd_notify_play_tone(n, ND_SMS_TONE));
    CHECK(wait_for_size("/played.txt", 1, 3000));

    {
        char got[1024];
        char want[ND_PATH_MAX];

        (void)pt_read_text("/played.txt", got, sizeof got);
        CHECK(strstr(got, "-q\n") != NULL);
        if (nd_path_resolve(want, sizeof want, ND_SMS_TONE) == ND_OK)
            CHECK(strstr(got, want) != NULL);
    }
    nd_notify_close(n);
    reap_everything();
    restore_path();
}

static void test_with_no_player_installed_the_beep_is_reported_not_fatal(void)
{
    nd_notify *n = NULL;
    int16_t samples[32];
    char log[4096];

    use_fake_bin(false); /* an empty PATH: nothing called aplay exists */
    fill_ramp(samples, ND_ARRAY_LEN(samples));
    write_wav_mono16(ND_TONES_DIR "/sms.wav", 44100u, samples, ND_ARRAY_LEN(samples));

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK(!nd_notify_play_tone(n, ND_SMS_TONE));
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Tone playback unavailable") != NULL);
    nd_notify_close(n);
    restore_path();
}

/* ------------------------------------------------------------------ *
 * Which tone rings -- the six-step search, in order
 * ------------------------------------------------------------------ */

static void test_the_configured_ringtone_wins_when_it_exists(void)
{
    nd_notify *n = NULL;
    int16_t s[8];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Chosen.wav", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/Chosen.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Chosen.wav");
    nd_notify_close(n);
}

static void test_the_setting_is_stripped_before_it_is_used(void)
{
    nd_notify *n = NULL;
    int16_t s[8];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Chosen.wav", 44100u, s, ND_ARRAY_LEN(s));
    /* str(get_setting(...)).strip(), line 128. A trailing space in
     * settings.prop is exactly what a hand-edited file has. */
    set_ringtone("  " ND_TONES_DIR "/Chosen.wav  ");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Chosen.wav");
    nd_notify_close(n);
}

static void test_a_reencoded_tone_is_found_under_the_other_extensions(void)
{
    nd_notify *n = NULL;
    int16_t s[8];
    char log[4096];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    /* The user picked a .wma; a later release re-encoded the same tone as an
     * .mp3 and deleted the original. It still rings. */
    write_wav_mono16(ND_TONES_DIR "/Bossanova.mp3", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/Bossanova.wma");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Bossanova.mp3");
    capture_end(log, sizeof log);
    /* {configured!r} is a Python repr, so the path is quoted. */
    CHECK(strstr(log, "Ringtone missing: '" ND_TONES_DIR "/Bossanova.wma'") != NULL);
    CHECK(strstr(log, "Using " ND_TONES_DIR "/Bossanova.mp3 instead.") != NULL);
    nd_notify_close(n);
}

static void test_the_extension_retry_order_is_mp3_first(void)
{
    nd_notify *n = NULL;
    int16_t s[8];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Tune.wav", 44100u, s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Tune.mp3", 44100u, s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Tune.ogg", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/Tune.flac");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    /* ND_RING_EXT_RETRY is .mp3, .wav, .wma, .flac, .ogg -- in that order. */
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Tune.mp3");
    nd_notify_close(n);
}

static void test_a_dotfile_keeps_its_leading_dot_when_the_stem_is_taken(void)
{
    nd_notify *n = NULL;
    int16_t s[8];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    /* os.path.splitext("/x/.hidden") is ("/x/.hidden", "") -- the dots that
     * START a basename are part of the name. A stem taken with a naive
     * "up to the last dot" would look for "/x/.mp3". */
    write_wav_mono16(ND_TONES_DIR "/.hidden.mp3", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/.hidden");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/.hidden.mp3");
    nd_notify_close(n);
}

static void test_the_fallback_chain_runs_in_its_declared_order(void)
{
    nd_notify *n = NULL;
    int16_t s[8];
    char log[4096];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    /* Low.mp3 and Nokia Tune.mp3 are both absent, so the third entry wins.
     * "Ring Ring.mp3" also happens to be the one with a space in it, which
     * is the case the Python docstring calls out: nothing here goes near a
     * shell, so nothing splits it. */
    write_wav_mono16(ND_TONES_DIR "/Ring Ring.mp3", 44100u, s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/sms.wav", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/Gone.mp3");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Ring Ring.mp3");
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Falling back to ringtone " ND_TONES_DIR "/Ring Ring.mp3.") != NULL);
    nd_notify_close(n);
}

static void test_the_last_resort_sweep_takes_the_first_playable_name_in_sort_order(void)
{
    nd_notify *n = NULL;
    int16_t s[8];
    char log[4096];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    /* No configured tone and no fallback exists. sorted(listdir()) puts
     * "Zulu.mp3" before "alpha.wav" -- capitals sort first in byte order, and
     * that IS what Python's sorted() does to these names. Do not "fix" it
     * into a case-insensitive or natural sort. */
    write_wav_mono16(ND_TONES_DIR "/alpha.wav", 44100u, s, ND_ARRAY_LEN(s));
    write_wav_mono16(ND_TONES_DIR "/Zulu.mp3", 44100u, s, ND_ARRAY_LEN(s));
    pt_write_text(ND_TONES_DIR "/README.txt", "not a tone");
    set_ringtone(ND_TONES_DIR "/Gone.mp3");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Zulu.mp3");
    capture_end(log, sizeof log);
    /* Step 5 deliberately says nothing. */
    CHECK(strstr(log, "Falling back") == NULL);
    nd_notify_close(n);
}

static void test_the_sweep_matches_extensions_case_insensitively(void)
{
    nd_notify *n = NULL;
    int16_t s[8];

    use_scratch_settings();
    fill_ramp(s, ND_ARRAY_LEN(s));
    /* name.lower().endswith((".mp3", ".wav", ".wma")), line 151. */
    write_wav_mono16(ND_TONES_DIR "/Shout.WAV", 44100u, s, ND_ARRAY_LEN(s));
    set_ringtone(ND_TONES_DIR "/Gone.mp3");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK_STR(nd_notify_ringtone_path(n), ND_TONES_DIR "/Shout.WAV");
    nd_notify_close(n);
}

static void test_nothing_playable_anywhere_rings_silently(void)
{
    nd_notify *n = NULL;
    char log[4096];

    use_scratch_settings();
    pt_mkdir(ND_TONES_DIR);
    /* .flac and .ogg are in the extension RETRY list but not in the sweep
     * list. That asymmetry is the Python's and it is what makes this case
     * end in None rather than in a tone. */
    pt_write_text(ND_TONES_DIR "/x.ogg", "not audio");
    pt_write_text(ND_TONES_DIR "/y.flac", "not audio");
    set_ringtone(ND_TONES_DIR "/Gone.mp3");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(nd_notify_ringtone_path(n) == NULL);

    capture_begin();
    CHECK(!nd_notify_start_ring(n));
    capture_end(log, sizeof log);
    CHECK(strstr(log, "No ringtone available; ringing silently.") != NULL);
    CHECK(!nd_notify_ringing(n));
    nd_notify_close(n);
}

/* ------------------------------------------------------------------ *
 * The streaming decoder
 * ------------------------------------------------------------------ */

static void test_a_44100_mono_source_comes_out_bit_for_bit_in_both_channels(void)
{
    nd_tone_src *src = NULL;
    int16_t in[256];
    int16_t out[256 * 2];
    char resolved[ND_PATH_MAX];
    size_t i;

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16("/t.wav", 44100u, in, ND_ARRAY_LEN(in));
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/t.wav"), ND_OK);

    CHECK_INT(nd_tone_src_open(&src, resolved), ND_OK);
    CHECK_INT(nd_tone_src_rate(src), 44100);
    CHECK_INT(nd_tone_src_channels(src), 1);
    /* At the output rate the resampler's fraction is zero on every frame, so
     * this must be a copy and not an interpolation. */
    CHECK_INT(nd_tone_src_read(src, out, 256u), 256);
    for (i = 0u; i < 256u; i++) {
        CHECK_INT(out[2u * i], in[i]);
        CHECK_INT(out[2u * i + 1u], in[i]);
    }
    nd_tone_src_close(src);
}

static void test_the_loop_wraps_to_frame_zero_with_no_gap(void)
{
    nd_tone_src *src = NULL;
    int16_t in[97];
    int16_t out[400 * 2];
    char resolved[ND_PATH_MAX];
    size_t i;

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16("/t.wav", 44100u, in, ND_ARRAY_LEN(in));
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/t.wav"), ND_OK);

    CHECK_INT(nd_tone_src_open(&src, resolved), ND_OK);
    /* Four times round a 97-frame file in one call: the read must not stop
     * at the end of the file, and frame 97 must be frame 0 exactly -- no
     * repeated sample, no dropped one, no silence. That is the property the
     * Python's _loop_generator went to the trouble of getting right, and it
     * is the difference between a ringtone and a ringtone with a click in
     * it. */
    CHECK_INT(nd_tone_src_read(src, out, 400u), 400);
    for (i = 0u; i < 400u; i++) {
        CHECK_INT(out[2u * i], in[i % ND_ARRAY_LEN(in)]);
        CHECK_INT(out[2u * i + 1u], in[i % ND_ARRAY_LEN(in)]);
    }
    nd_tone_src_close(src);
}

static void test_the_wrap_survives_being_read_in_awkward_pieces(void)
{
    nd_tone_src *src = NULL;
    int16_t in[97];
    int16_t out[400 * 2];
    char resolved[ND_PATH_MAX];
    size_t done = 0u;
    size_t i;
    const size_t PIECES[] = {1u, 3u, 96u, 1u, 199u, 100u};

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16("/t.wav", 44100u, in, ND_ARRAY_LEN(in));
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/t.wav"), ND_OK);

    CHECK_INT(nd_tone_src_open(&src, resolved), ND_OK);
    /* The device asks for whatever its buffer needs, which is not a multiple
     * of anything. The cursor is carried across calls, so the stream is the
     * same one whatever the chunking. */
    for (i = 0u; i < ND_ARRAY_LEN(PIECES); i++) {
        CHECK_INT(nd_tone_src_read(src, out + done * 2u, PIECES[i]), (long long)PIECES[i]);
        done += PIECES[i];
    }
    CHECK_INT(done, 400);
    for (i = 0u; i < done; i++)
        CHECK_INT(out[2u * i], in[i % ND_ARRAY_LEN(in)]);
    nd_tone_src_close(src);
}

static void test_a_48000_source_is_resampled_to_44100(void)
{
    nd_tone_src *src = NULL;
    int16_t in[4800];
    int16_t out[4410 * 2];
    char resolved[ND_PATH_MAX];
    size_t i;

    /* A constant signal: linear interpolation between two equal samples is
     * that sample, so every output frame must be exactly 1234 whatever the
     * ratio does. Anything else means the cursor left the buffer. */
    for (i = 0u; i < ND_ARRAY_LEN(in); i++)
        in[i] = 1234;
    write_wav_mono16("/t48.wav", 48000u, in, ND_ARRAY_LEN(in));
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/t48.wav"), ND_OK);

    CHECK_INT(nd_tone_src_open(&src, resolved), ND_OK);
    CHECK_INT(nd_tone_src_rate(src), 48000);
    CHECK_INT(nd_tone_src_read(src, out, 4410u), 4410);
    {
        size_t bad = 0u;

        for (i = 0u; i < 4410u; i++) {
            if (out[2u * i] != 1234 || out[2u * i + 1u] != 1234)
                bad++;
        }
        CHECK_INT(bad, 0);
    }
    nd_tone_src_close(src);
}

static void test_the_resampler_starts_on_the_files_first_sample(void)
{
    nd_tone_src *src = NULL;
    int16_t in[480];
    int16_t out[8];
    char resolved[ND_PATH_MAX];

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16("/t48.wav", 48000u, in, ND_ARRAY_LEN(in));
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/t48.wav"), ND_OK);

    CHECK_INT(nd_tone_src_open(&src, resolved), ND_OK);
    CHECK_INT(nd_tone_src_read(src, out, 4u), 4);
    /* The fraction is zero on the first output frame, so however the ratio
     * behaves afterwards the tone starts where the file starts -- no half a
     * sample of the previous silence, which is audible as a tick. */
    CHECK_INT(out[0], in[0]);
    CHECK_INT(out[1], in[0]);
    nd_tone_src_close(src);
}

static void test_a_file_the_decoder_cannot_read_is_unsupported_not_a_crash(void)
{
    nd_tone_src *src = (nd_tone_src *)(void *)&src;
    char resolved[ND_PATH_MAX];

    pt_write_text("/notaudio.ogg", "OggS this is not an mp3 or a wav\n");
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/notaudio.ogg"), ND_OK);

    /* ND_ERR_UNSUPPORTED is the branch that sends the ringer to mpv, and it
     * is the same branch a .wma takes in the Python -- miniaudio cannot read
     * one either. */
    CHECK_INT(nd_tone_src_open(&src, resolved), ND_ERR_UNSUPPORTED);
    CHECK(src == NULL);
}

static void test_a_file_that_is_not_there_at_all_is_unsupported(void)
{
    nd_tone_src *src = (nd_tone_src *)(void *)&src;

    CHECK_INT(nd_tone_src_open(&src, "/no/such/tone.mp3"), ND_ERR_UNSUPPORTED);
    CHECK(src == NULL);
}

/* The one case that uses a real shipped ringtone, because "it decodes MP3"
 * is not something a hand-built WAV can demonstrate. Skipped rather than
 * failed when the overlay is not beside the golden set, so the test stays
 * runnable from an installed tree. */
static void test_the_real_default_ringtone_decodes_and_loops(void)
{
    nd_tone_src *src = NULL;
    const char *golden = getenv("NEODCT_GOLDEN");
    char path[ND_PATH_MAX];
    int16_t *out;
    size_t n;

    if (golden == NULL || golden[0] == '\0') {
        fprintf(stderr, "SKIP: NEODCT_GOLDEN unset; cannot find the shipped tones\n");
        return;
    }
    if (nd_snprintf(path, sizeof path, "%s/../../overlay/NeoDCT/System/tones/Low.mp3", golden) !=
            ND_OK ||
        access(path, R_OK) != 0) {
        fprintf(stderr, "SKIP: %s not readable\n", path);
        return;
    }

    /* NOT through nd_path_resolve: this is the repository, not the phone. */
    CHECK_INT(nd_tone_src_open(&src, path), ND_OK);
    /* Measured, and the reason the whole-file decode had to go: 163840
     * frames at 48 kHz is 3.41 s, which miniaudio would have materialised as
     * 599 kB of 44.1 kHz stereo int16. Tchaikovsky.mp3 is 6.4 MB the same
     * way. This decodes 0.37 s at a time instead. */
    CHECK_INT(nd_tone_src_rate(src), 48000);
    CHECK_INT(nd_tone_src_channels(src), 1);

    out = malloc(ND_RING_CHUNK_BYTES);
    CHECK(out != NULL);
    if (out != NULL) {
        /* Read past the end of a 3.41 s tone: 10 s of output, which wraps
         * twice, in 372 ms pieces. Every one must be full. */
        for (n = 0u; n < 27u; n++)
            CHECK_INT(nd_tone_src_read(src, out, ND_RING_CHUNK_FRAMES), ND_RING_CHUNK_FRAMES);
        free(out);
    }
    nd_tone_src_close(src);
}

/* ------------------------------------------------------------------ *
 * The ringer, end to end
 * ------------------------------------------------------------------ */

static void test_the_ringer_streams_the_looped_tone_to_the_player(void)
{
    nd_notify *n = NULL;
    int16_t in[997];
    const char *head = find_tool("head");
    char script[ND_PATH_MAX * 2];
    char capture[ND_PATH_MAX];
    char log[4096];
    size_t i;
    bool got;

    if (head == NULL) {
        fprintf(stderr, "SKIP: no head(1) to build a fake player with\n");
        return;
    }
    use_scratch_settings();
    use_fake_bin(true);
    if (nd_path_resolve(capture, sizeof capture, "/pcm.raw") != ND_OK)
        return;

    /* A sound card that records exactly one chunk and then hangs up. The
     * hang-up matters as much as the recording: it is what proves the feeder
     * survives the player disappearing under it, which on a pipe would be a
     * SIGPIPE and the end of nd-core. */
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nexec '%s' -c 65536 > '%s'\n", head,
                      capture);
    write_script("aplay", script);

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16(ND_TONES_DIR "/Loop.wav", 44100u, in, ND_ARRAY_LEN(in));
    set_ringtone(ND_TONES_DIR "/Loop.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK(nd_notify_start_ring(n));
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Ringing: " ND_TONES_DIR "/Loop.wav") != NULL);
    CHECK(nd_notify_ringing(n));

    got = wait_for_size("/pcm.raw", 65536, 10000);
    CHECK(got);

    capture_begin();
    nd_notify_stop_ring(n);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Ringer stopped.") != NULL);
    CHECK(!nd_notify_ringing(n));

    if (got) {
        char resolved[ND_PATH_MAX];
        FILE *f;
        int16_t *pcm = malloc(65536u);

        CHECK(pcm != NULL);
        CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/pcm.raw"), ND_OK);
        f = fopen(resolved, "rb");
        CHECK(f != NULL);
        if (f != NULL && pcm != NULL) {
            size_t bad = 0u;

            CHECK_INT(fread(pcm, 1u, 65536u, f), 65536);
            /* 16384 stereo frames of a 997-frame tone: sixteen and a bit
             * times round the loop, checked frame by frame against the
             * source. A wrap that is one sample out shows up here. */
            for (i = 0u; i < 16384u; i++) {
                int16_t want = in[i % ND_ARRAY_LEN(in)];

                if (pcm[2u * i] != want || pcm[2u * i + 1u] != want) {
                    if (bad == 0u)
                        fprintf(stderr, "  first bad frame %zu: got %d,%d want %d\n", i,
                                (int)pcm[2u * i], (int)pcm[2u * i + 1u], (int)want);
                    bad++;
                }
            }
            CHECK_INT(bad, 0);
        }
        if (f != NULL)
            (void)fclose(f);
        free(pcm);
    }

    nd_notify_close(n);
    reap_everything();
    restore_path();
}

static void test_a_tone_the_decoder_cannot_read_falls_through_to_mpv(void)
{
    nd_notify *n = NULL;
    const char *sleeper = find_tool("sleep");
    char script[ND_PATH_MAX * 2];
    char log[4096];

    if (sleeper == NULL) {
        fprintf(stderr, "SKIP: no sleep(1) to build a fake mpv with\n");
        return;
    }
    use_scratch_settings();
    use_fake_bin(false); /* nothing on PATH but the fake mpv: no aplay */
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nexec '%s' 30\n", sleeper);
    write_script("mpv", script);

    /* An .ogg the decoder cannot read. In the Python this is a miniaudio
     * exception; here it is ND_ERR_UNSUPPORTED. Same branch, same next step. */
    pt_write_text(ND_TONES_DIR "/Weird.ogg", "OggS not really\n");
    set_ringtone(ND_TONES_DIR "/Weird.ogg");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK(nd_notify_start_ring(n));
    capture_end(log, sizeof log);

    CHECK(strstr(log, "Streaming ring failed (not MP3 or WAV); trying mpv.") != NULL);
    CHECK(strstr(log, "Ringing (mpv): " ND_TONES_DIR "/Weird.ogg") != NULL);
    CHECK(nd_notify_ringing(n));

    capture_begin();
    nd_notify_stop_ring(n);
    capture_end(log, sizeof log);
    CHECK(strstr(log, "Ringer stopped.") != NULL);
    CHECK(!nd_notify_ringing(n));

    nd_notify_close(n);
    reap_everything();
    restore_path();
}

static void test_with_neither_player_the_ringer_says_so_and_gives_up(void)
{
    nd_notify *n = NULL;
    int16_t in[64];
    char log[4096];

    use_scratch_settings();
    use_fake_bin(false); /* an empty PATH: no aplay, no mpv */
    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16(ND_TONES_DIR "/Loop.wav", 44100u, in, ND_ARRAY_LEN(in));
    set_ringtone(ND_TONES_DIR "/Loop.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    CHECK(!nd_notify_start_ring(n));
    capture_end(log, sizeof log);

    CHECK(strstr(log, "trying mpv.") != NULL);
    CHECK(strstr(log, "Ringer unavailable: mpv:") != NULL);
    CHECK(!nd_notify_ringing(n));
    nd_notify_close(n);
    restore_path();
}

static void test_stopping_a_ringer_that_is_not_ringing_says_nothing(void)
{
    nd_notify *n = NULL;
    char log[4096];

    CHECK_INT(nd_notify_open(&n), ND_OK);
    capture_begin();
    nd_notify_stop_ring(n);
    nd_notify_stop_ring(n);
    capture_end(log, sizeof log);
    /* stop_ring() is idempotent and is called at the top of start_ring(). If
     * it logged unconditionally the console would say the ringer stopped
     * every time the phone rang. */
    CHECK(strstr(log, "Ringer stopped.") == NULL);
    nd_notify_close(n);
}

static void test_closing_the_service_takes_the_ringer_with_it(void)
{
    nd_notify *n = NULL;
    int16_t in[997];
    const char *head = find_tool("head");
    char script[ND_PATH_MAX * 2];
    char capture[ND_PATH_MAX];

    if (head == NULL) {
        fprintf(stderr, "SKIP: no head(1) to build a fake player with\n");
        return;
    }
    use_scratch_settings();
    use_fake_bin(true);
    if (nd_path_resolve(capture, sizeof capture, "/pcm.raw") != ND_OK)
        return;
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nexec '%s' -c 400000 > '%s'\n", head,
                      capture);
    write_script("aplay", script);

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16(ND_TONES_DIR "/Loop.wav", 44100u, in, ND_ARRAY_LEN(in));
    set_ringtone(ND_TONES_DIR "/Loop.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(nd_notify_start_ring(n));
    CHECK(wait_for_size("/pcm.raw", 1, 10000));
    /* Closing while the feeder thread is mid-send is the interesting case:
     * the thread has to be woken, joined and freed, and the player killed,
     * with nothing left running and nothing leaked. Under ASan this case is
     * the one that would say so. */
    nd_notify_close(n);
    reap_everything();
    restore_path();
}

static void test_starting_twice_stops_the_first_ringer(void)
{
    nd_notify *n = NULL;
    int16_t in[997];
    const char *head = find_tool("head");
    char script[ND_PATH_MAX * 2];
    char capture[ND_PATH_MAX];
    char log[4096];

    if (head == NULL) {
        fprintf(stderr, "SKIP: no head(1) to build a fake player with\n");
        return;
    }
    use_scratch_settings();
    use_fake_bin(true);
    if (nd_path_resolve(capture, sizeof capture, "/pcm.raw") != ND_OK)
        return;
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nexec '%s' -c 400000 > '%s'\n", head,
                      capture);
    write_script("aplay", script);

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16(ND_TONES_DIR "/Loop.wav", 44100u, in, ND_ARRAY_LEN(in));
    set_ringtone(ND_TONES_DIR "/Loop.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(nd_notify_start_ring(n));
    capture_begin();
    CHECK(nd_notify_start_ring(n));
    capture_end(log, sizeof log);
    /* start_ring() calls stop_ring() first (line 157), so a second incoming
     * call cannot leave two players fighting over the sound card. */
    CHECK(strstr(log, "Ringer stopped.") != NULL);
    CHECK(nd_notify_ringing(n));

    nd_notify_close(n);
    reap_everything();
    restore_path();
}

/* The case that decides whether the phone can hang up: a player that never
 * reads. A ringtone plays in real time, so within a second of the phone
 * ringing the socket is full and the feeder thread is blocked inside send().
 * If stop_ring() cannot get it out of there, answering a call deadlocks the
 * UI thread on pthread_join and the phone is a brick until the watchdog
 * notices. */
static void test_stopping_a_ringer_whose_player_never_reads_does_not_hang(void)
{
    nd_notify *n = NULL;
    int16_t in[997];
    const char *sleeper = find_tool("sleep");
    char script[ND_PATH_MAX * 2];
    struct timespec t0;
    struct timespec t1;
    double elapsed;

    if (sleeper == NULL) {
        fprintf(stderr, "SKIP: no sleep(1) to build a stalled player with\n");
        return;
    }
    use_scratch_settings();
    use_fake_bin(false);
    (void)nd_snprintf(script, sizeof script, "#!/bin/sh\nexec '%s' 30\n", sleeper);
    write_script("aplay", script);

    fill_ramp(in, ND_ARRAY_LEN(in));
    write_wav_mono16(ND_TONES_DIR "/Loop.wav", 44100u, in, ND_ARRAY_LEN(in));
    set_ringtone(ND_TONES_DIR "/Loop.wav");

    CHECK_INT(nd_notify_open(&n), ND_OK);
    CHECK(nd_notify_start_ring(n));
    CHECK(nd_notify_ringing(n));
    /* Long enough for the feeder to fill the socket buffer (roughly 208 kB
     * on Linux, so four 64 kB chunks) and block on the fifth. */
    nap_ms(400);

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    nd_notify_stop_ring(n);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    CHECK(!nd_notify_ringing(n));
    /* The 0.3 s grace before SIGKILL is the only thing that may be spent
     * here, and sleep(1) dies on the SIGTERM long before that. Two seconds
     * is a generous ceiling that a deadlock cannot slip under. */
    if (elapsed >= 2.0)
        fprintf(stderr, "  stop_ring took %.3f s\n", elapsed);
    CHECK(elapsed < 2.0);

    nd_notify_close(n);
    reap_everything();
    restore_path();
}

/* ------------------------------------------------------------------ *
 * Defensive
 * ------------------------------------------------------------------ */

static void test_every_entry_point_tolerates_a_null_service(void)
{
    char l1[ND_NOTIFY_LINE_MAX];
    char l2[ND_NOTIFY_LINE_MAX];

    /* nd_ui.c reaches this module through a weak symbol and a possibly-NULL
     * handle, so NULL is a value that genuinely arrives here. */
    nd_notify_post_sms(NULL, 1, false);
    nd_notify_post_event(NULL, 1, "Dentist", 0, false);
    nd_notify_dismiss(NULL);
    nd_notify_stop_ring(NULL);
    nd_notify_close(NULL);
    CHECK(!nd_notify_active(NULL));
    CHECK(nd_notify_kind(NULL) == NULL);
    CHECK_INT(nd_notify_count(NULL), 0);
    CHECK_INT(nd_notify_latest_data(NULL), -1);
    CHECK_INT(nd_notify_banner_lines(NULL, l1, l2), 0);
    CHECK(!nd_notify_start_ring(NULL));
    CHECK(!nd_notify_ringing(NULL));
    CHECK(nd_notify_ringtone_path(NULL) == NULL);
    CHECK(!nd_notify_play_tone(NULL, NULL));
    CHECK_INT(nd_notify_open(NULL), ND_ERR_INVAL);
    CHECK_INT(nd_tone_src_open(NULL, "/x"), ND_ERR_INVAL);
    nd_tone_src_close(NULL);
    CHECK_INT(nd_tone_src_read(NULL, NULL, 4u), 0);
    CHECK_INT(nd_tone_src_rate(NULL), 0);
    CHECK_INT(nd_tone_src_channels(NULL), 0);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    RUN(test_a_fresh_service_has_nothing_to_show);
    RUN(test_one_message_is_singular);
    RUN(test_more_than_one_message_is_plural_and_keeps_the_newest_row);
    RUN(test_dismiss_clears_the_banner_and_only_the_banner);
    RUN(test_the_banner_buffers_are_cleared_even_when_there_is_no_banner);

    RUN(test_one_reminder_names_itself_and_its_time);
    RUN(test_an_untitled_reminder_still_says_something);
    RUN(test_more_than_one_reminder_is_counted);
    RUN(test_a_second_kind_takes_the_banner_over);
    RUN(test_the_same_kind_twice_does_not_reset);
    RUN(test_dismiss_clears_the_reminder_as_well);

    RUN(test_a_missing_tone_is_reported_and_not_played);
    RUN(test_an_existing_tone_is_handed_to_aplay_and_not_waited_for);
    RUN(test_with_no_player_installed_the_beep_is_reported_not_fatal);

    RUN(test_the_configured_ringtone_wins_when_it_exists);
    RUN(test_the_setting_is_stripped_before_it_is_used);
    RUN(test_a_reencoded_tone_is_found_under_the_other_extensions);
    RUN(test_the_extension_retry_order_is_mp3_first);
    RUN(test_a_dotfile_keeps_its_leading_dot_when_the_stem_is_taken);
    RUN(test_the_fallback_chain_runs_in_its_declared_order);
    RUN(test_the_last_resort_sweep_takes_the_first_playable_name_in_sort_order);
    RUN(test_the_sweep_matches_extensions_case_insensitively);
    RUN(test_nothing_playable_anywhere_rings_silently);

    RUN(test_a_44100_mono_source_comes_out_bit_for_bit_in_both_channels);
    RUN(test_the_loop_wraps_to_frame_zero_with_no_gap);
    RUN(test_the_wrap_survives_being_read_in_awkward_pieces);
    RUN(test_a_48000_source_is_resampled_to_44100);
    RUN(test_the_resampler_starts_on_the_files_first_sample);
    RUN(test_a_file_the_decoder_cannot_read_is_unsupported_not_a_crash);
    RUN(test_a_file_that_is_not_there_at_all_is_unsupported);
    RUN(test_the_real_default_ringtone_decodes_and_loops);

    RUN(test_the_ringer_streams_the_looped_tone_to_the_player);
    RUN(test_a_tone_the_decoder_cannot_read_falls_through_to_mpv);
    RUN(test_with_neither_player_the_ringer_says_so_and_gives_up);
    RUN(test_stopping_a_ringer_that_is_not_ringing_says_nothing);
    RUN(test_closing_the_service_takes_the_ringer_with_it);
    RUN(test_starting_twice_stops_the_first_ringer);
    RUN(test_stopping_a_ringer_whose_player_never_reads_does_not_hang);

    RUN(test_every_entry_point_tolerates_a_null_service);

    restore_path();
    nd_settings_set_paths(NULL, NULL);
    return pt_report("test_notify");
}
