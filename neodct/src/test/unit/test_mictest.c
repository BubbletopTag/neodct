/* test_mictest.c -- the MicTest engineering app, app id 9002.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. UP and DOWN move the capture level by ND_MIC_GAIN_STEP and clamp at
 *     both ends, and EVERY OTHER KEY leaves it exactly where it was --
 *     including the -1 nd_ui_read_keypress() returns when nothing was
 *     pressed, which the waveform loop hands straight in twenty times a
 *     second.
 *
 *  2. THE WRITE HAPPENS ONCE, AT THE END, AND ONLY IF THE NUMBER MOVED.
 *     R-24 (nd_settings.h) makes every nd_settings_set() a full rewrite of
 *     settings.prop with an fsync, so a per-keypress save would be eight
 *     rewrites of the user partition for two seconds of a held key. The
 *     shape is what proves it: nd_mictest_step() is pure and takes no
 *     settings at all, so a design that saved per keypress could not have
 *     been written this way -- and a whole scripted session of presses ends
 *     with one call to nd_mictest_save_gain(), which the file then shows.
 *
 *  3. A session that put the level back where it found it writes NOTHING.
 *     Looking at the gain is not changing it, and on UBIFS the difference is
 *     an erase block.
 *
 *  4. The peak readout comes out of the same columns the waveform is drawn
 *     from, counts both ends of the signal, and does not lose the sample at
 *     -32768 -- which is the one a clipping microphone produces first.
 *
 * ============ WHAT IS NOT HERE ============
 *
 * app_run() and watch(). They fork a real arecord and read a real pipe;
 * mictest.h names that hole and says why. The three functions below are every
 * decision that loop makes -- what a key does, what the level owes
 * settings.prop, what the readout says -- and what is left over is drawing.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nd_keycodes.h"
#include "nd_mic.h"
#include "nd_settings.h"

#include "smallapp_test.h"

#include "../../apps/MicTest/mictest.h"

static struct {
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    int32_t (*step)(int32_t, int32_t);
    bool (*save_gain)(int32_t, int32_t);
    int32_t (*peak)(const nd_mic_column *, size_t);
    const char *const *title;
    const char *const *no_device;
    const char *const *no_arecord;
} api;

static char g_root[ND_PATH_MAX];
static char g_saved_root[ND_PATH_MAX];

static bool api_open(void *h)
{
    *(void **)&api.run = sa_sym(h, "app_run");
    *(void **)&api.shutdown = sa_sym(h, "app_shutdown");
    *(void **)&api.step = sa_sym(h, "nd_mictest_step");
    *(void **)&api.save_gain = sa_sym(h, "nd_mictest_save_gain");
    *(void **)&api.peak = sa_sym(h, "nd_mictest_peak_percent");
    api.title = dlsym(h, "nd_mictest_title");
    api.no_device = dlsym(h, "nd_mictest_no_device");
    api.no_arecord = dlsym(h, "nd_mictest_no_arecord");

    return api.run != NULL && api.shutdown != NULL && api.step != NULL && api.save_gain != NULL &&
           api.peak != NULL && api.title != NULL && api.no_device != NULL && api.no_arecord != NULL;
}

/* ------------------------------------------------------------------ *
 * 1. The keys
 * ------------------------------------------------------------------ */

static void test_up_and_down_move_the_level(void)
{
    CHECK_INT(api.step(50, ND_KEY_UP), 55, "UP is one step louder");
    CHECK_INT(api.step(50, ND_KEY_DOWN), 45, "DOWN is one step quieter");
    CHECK_INT(ND_MIC_GAIN_STEP, 5, "and a step is 5%");
}

/* Both ends stop rather than wrap. A gain that wrapped from 100 to 0 on one
 * extra press would mute the microphone at the exact moment its owner was
 * trying to make it louder. */
static void test_the_level_clamps_at_both_ends(void)
{
    CHECK_INT(api.step(100, ND_KEY_UP), 100, "100 is the top");
    CHECK_INT(api.step(98, ND_KEY_UP), 100, "and a partial step lands on it");
    CHECK_INT(api.step(0, ND_KEY_DOWN), 0, "0 is the bottom");
    CHECK_INT(api.step(2, ND_KEY_DOWN), 0, "and a partial step lands on it");
    CHECK_INT(ND_MIC_GAIN_MIN, 0, "the floor is 0, not 1 -- a muted card is a real state");
    CHECK_INT(ND_MIC_GAIN_MAX, 100, "the ceiling is a percentage");
}

/* The default is written twice -- once as a number for the C callers and once
 * as a string for nd_settings_get()'s call-site default -- so the two are
 * asserted equal here. They would otherwise drift silently, and the symptom
 * would be a phone whose gain differs depending on which of the three readers
 * asked first. */
static void test_the_two_spellings_of_the_default_agree(void)
{
    CHECK_INT(nd_mic_gain_from_setting(ND_SET_HW_MIC_GAIN_DFLT), ND_MIC_GAIN_DEFAULT,
              "ND_SET_HW_MIC_GAIN_DFLT parses to ND_MIC_GAIN_DEFAULT");
    CHECK_INT(ND_MIC_GAIN_DEFAULT, 100, "and it is 100, not the 80 this shipped with");
}

/* The loop calls this with whatever nd_ui_read_keypress() returned, which is
 * -1 far more often than it is anything else. Every one of those has to be a
 * no-op or the gain would drift while nobody touched the phone. */
static void test_every_other_key_leaves_it_alone(void)
{
    static const int32_t OTHERS[] = {-1, ND_KEY_BACK, ND_KEY_ENTER, ND_KEY_1, ND_KEY_HASH, 0};
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(OTHERS); i++)
        CHECK_INT(api.step(70, OTHERS[i]), 70, "an unrelated key moves nothing");
}

/* ------------------------------------------------------------------ *
 * 2 and 3. The one write
 * ------------------------------------------------------------------ */

static void expect_setting(const char *want, const char *what)
{
    char got[64];

    (void)nd_settings_get_copy(ND_SET_HW_MIC_GAIN, "<absent>", got, sizeof got);
    CHECK_STR(got, want, what);
}

/* A whole session: open at 50, hold UP until it stops, then one DOWN, then
 * leave. Thirteen presses, ONE write, and settings.prop holds the last value
 * and not one of the twelve it passed through on the way. */
static void test_a_session_writes_once_at_the_end(void)
{
    static const int32_t SESSION[] = {ND_KEY_UP, ND_KEY_UP, ND_KEY_UP,  ND_KEY_UP, ND_KEY_UP,
                                      ND_KEY_UP, ND_KEY_UP, ND_KEY_UP,  ND_KEY_UP, ND_KEY_UP,
                                      ND_KEY_UP, ND_KEY_UP, ND_KEY_DOWN};
    const int32_t loaded = 50;
    int32_t gain = loaded;
    size_t i;

    CHECK_INT(nd_settings_set(ND_SET_HW_MIC_GAIN, "50"), ND_OK, "the level the screen opened on");

    /* The loop, without the pipe. step() takes no settings and touches none,
     * which is the structural half of the claim: there is nowhere in here for
     * a per-keypress write to hide. */
    for (i = 0u; i < ND_ARRAY_LEN(SESSION); i++)
        gain = api.step(gain, SESSION[i]);

    CHECK_INT(gain, 95, "twelve ups clamp at 100, then one down");
    expect_setting("50", "and nothing has been written yet");

    CHECK(api.save_gain(gain, loaded), "leaving the screen writes");
    expect_setting("95", "the value that is stored is the one on the screen");
}

/* Look and put it back: no write at all. The comparison is against the value
 * the screen OPENED with, not against the last one seen, so a round trip
 * costs the flash nothing. */
static void test_an_unchanged_level_writes_nothing(void)
{
    int32_t gain;

    CHECK_INT(nd_settings_set(ND_SET_HW_MIC_GAIN, "60"), ND_OK, "a level to start from");

    gain = api.step(60, ND_KEY_UP);
    gain = api.step(gain, ND_KEY_DOWN);
    CHECK_INT(gain, 60, "up then down is where it started");

    CHECK(!api.save_gain(gain, 60), "and nothing is owed to settings.prop");
    expect_setting("60", "the file is untouched");
}

/* A gain outside the range never reaches settings.prop either. Nothing in the
 * app can produce one -- step() clamps -- but save_gain() is the last gate in
 * front of a file the next boot's S17audio parses, and a gate that trusts its
 * caller is not a gate. */
static void test_an_impossible_level_is_not_written(void)
{
    CHECK_INT(nd_settings_set(ND_SET_HW_MIC_GAIN, "70"), ND_OK, "a level to start from");

    CHECK(!api.save_gain(101, 70), "above the ceiling");
    CHECK(!api.save_gain(-1, 70), "below the floor");
    expect_setting("70", "the stored level is the one that was there");
}

/* ------------------------------------------------------------------ *
 * 4. The peak readout
 * ------------------------------------------------------------------ */

static void test_peak_reads_both_ends_of_the_signal(void)
{
    nd_mic_column columns[3];

    columns[0].min = 0;
    columns[0].max = 0;
    columns[1].min = -16384;
    columns[1].max = 100;
    columns[2].min = -10;
    columns[2].max = 8192;

    /* The loudest thing in the frame is the NEGATIVE half of column 1, which
     * a "highest sample" reading would have missed entirely -- and an
     * electret with a DC offset swings one way before the other. */
    CHECK_INT(api.peak(columns, 3u), 50, "16384/32767 is 50%");

    columns[1].min = 0;
    CHECK_INT(api.peak(columns, 3u), 25, "8192/32767 is 25%");
}

/* Silence reads 0 and full scale reads 100. -32768 has no positive twin, so
 * the arithmetic has to hold it at 100 rather than spilling to 101 -- a
 * percentage above 100 on a level meter is a bug the owner can see. */
static void test_peak_ends_at_silence_and_at_full_scale(void)
{
    nd_mic_column one;

    one.min = 0;
    one.max = 0;
    CHECK_INT(api.peak(&one, 1u), 0, "silence");

    one.max = 32767;
    CHECK_INT(api.peak(&one, 1u), 100, "positive full scale");

    one.max = 0;
    one.min = -32768;
    CHECK_INT(api.peak(&one, 1u), 100, "negative full scale, and not 101");

    CHECK_INT(api.peak(NULL, 4u), 0, "no columns");
    CHECK_INT(api.peak(&one, 0u), 0, "no samples yet");
}

/* ------------------------------------------------------------------ *
 * Null safety
 * ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    CHECK_INT(api.run(NULL), 1, "app_run(NULL) refuses rather than faults");
    api.shutdown(); /* the symbol nd_app.h requires; there is no child to drop */
    api.shutdown();
    CHECK_STR(*api.title, "MicTest", "the title the header measured against font_xl");
    sa_checks++;
}

int main(void)
{
    void *h = sa_begin("MicTest", "ndmictest");
    int rc;

    if (h == NULL)
        return 1;
    if (!api_open(h)) {
        (void)dlclose(h);
        return 1;
    }
    if (!sa_tmpdir("ndmictest-root", g_root, sizeof g_root)) {
        (void)dlclose(h);
        return 1;
    }

    /* Everything under /NeoDCT is this test's own, so the settings writes
     * below land in a scratch file and never near a real phone's. */
    (void)nd_strlcpy(g_saved_root, nd_path_root(), sizeof g_saved_root);
    (void)nd_path_set_root(g_root);
    (void)nd_settings_init();

    RUN(test_up_and_down_move_the_level);
    RUN(test_the_level_clamps_at_both_ends);
    RUN(test_every_other_key_leaves_it_alone);
    RUN(test_the_two_spellings_of_the_default_agree);
    RUN(test_a_session_writes_once_at_the_end);
    RUN(test_an_unchanged_level_writes_nothing);
    RUN(test_an_impossible_level_is_not_written);
    RUN(test_peak_reads_both_ends_of_the_signal);
    RUN(test_peak_ends_at_silence_and_at_full_scale);
    RUN(test_null_safety);

    (void)nd_path_set_root(g_saved_root[0] != '\0' ? g_saved_root : NULL);
    rc = sa_end(h, "test_mictest");
    sa_rmtree(g_root);
    return rc;
}
