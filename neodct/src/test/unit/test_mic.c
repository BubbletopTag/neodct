/* test_mic.c -- capture-device discovery and waveform reduction.
 *
 * The scan is pointed at a directory the test builds rather than at the real
 * /proc/asound, which is why nd_mic_scan() takes a root at all. nd_modem_audio.c
 * hardcodes the real one and is untestable for exactly that reason.
 *
 * The mixer half at the bottom is the one that matters most, because it is the
 * fix for a fault nothing else in the tree could see: S17audio sets the capture
 * switch and level ONCE at boot, and a USB card that re-enumerates comes back
 * muted with every layer still reporting success. The parser, the argv builder
 * and the settings parse are all here; nd_mic_apply_gain() is exercised against
 * a stand-in amixer that records what it was asked for.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "nd_mic.h"
#include "nd_paths.h"
#include "nd_vclock.h"

#include "platform_test.h"

/* The real path behind a virtual one, which is what the scan has to be given:
 * it walks a directory tree with opendir() and knows nothing about ND_ROOT. */
static const char *asound_root(void)
{
    static char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, "/asound") != ND_OK)
        return "";
    return resolved;
}

/* A card with a capture PCM is found, and named the way `arecord -D` takes it.
 * "plughw" and not "hw": the plug layer converts rate and format, and a USB
 * microphone that only does 48000 stereo would otherwise refuse the 8000 mono
 * a phone asks for. */
static void test_scan_finds_a_capture_device(void)
{
    nd_mic_device found[8];
    size_t n;

    pt_mkdir("/asound/card2/pcm0c");
    pt_write_text("/asound/card2/id", "D100009002\n");

    n = nd_mic_scan(asound_root(), found, 8u);

    CHECK_INT(n, 1);
    CHECK_STR(found[0].device, "plughw:2,0");
}

/* A playback-only card is not a microphone, and offering it as one wastes the
 * only screen this app has. QEMU's USB Audio is exactly this: card 0, pcm0p and
 * nothing else, which is why "default" is a trap for capture on the emulator. */
static void test_scan_ignores_a_playback_only_card(void)
{
    nd_mic_device found[8];
    size_t n;

    pt_mkdir("/asound/card0/pcm0p");
    pt_write_text("/asound/card0/id", "UsbAudio\n");

    n = nd_mic_scan(asound_root(), found, 8u);

    CHECK_INT(n, 0);
}

/* Each column carries the extremes of the samples that fell in it. Half the
 * buffer to each of two columns, so the arithmetic is checkable by eye. */
static void test_reduce_keeps_the_extremes_of_each_column(void)
{
    static const int16_t samples[8] = {0, 100, -50, 0, 0, -300, 200, 0};
    nd_mic_column columns[2];
    size_t n;

    n = nd_mic_reduce(samples, 8u, columns, 2u);

    CHECK_INT(n, 2);
    CHECK_INT(columns[0].min, -50);
    CHECK_INT(columns[0].max, 100);
    CHECK_INT(columns[1].min, -300);
    CHECK_INT(columns[1].max, 200);
}

/* Fewer samples than columns is a short read, not an error: the first read off
 * a freshly started arecord routinely is one. Draw the columns there are
 * samples for and stop, rather than stretching a handful of samples across the
 * screen and showing a waveform that was never captured. */
static void test_reduce_fills_only_what_it_has(void)
{
    static const int16_t samples[3] = {10, -20, 30};
    nd_mic_column columns[8];
    size_t n;

    n = nd_mic_reduce(samples, 3u, columns, 8u);

    CHECK_INT(n, 3);
    CHECK_INT(columns[0].min, 10);
    CHECK_INT(columns[0].max, 10);
    CHECK_INT(columns[1].min, -20);
    CHECK_INT(columns[2].max, 30);
}

/* The capture command, word for word. -D and not a positional argument, and no
 * output file at all: arecord writes the stream to stdout, which is the pipe
 * the waveform is read from. */
static void test_record_command_is_raw_mono_on_stdout(void)
{
    nd_mic_command cmd;
    size_t i = 0u;

    CHECK_INT(nd_mic_record_command(&cmd, "plughw:2,0", 8000), ND_OK);
    /* Nothing below is safe to read if the command was not built: argv holds
     * whatever was on the stack. */
    if (nd_mic_record_command(&cmd, "plughw:2,0", 8000) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], "arecord");
    CHECK_STR(cmd.argv[i++], "-q");
    CHECK_STR(cmd.argv[i++], "-t");
    CHECK_STR(cmd.argv[i++], "raw");
    CHECK_STR(cmd.argv[i++], "-f");
    CHECK_STR(cmd.argv[i++], "S16_LE");
    CHECK_STR(cmd.argv[i++], "-r");
    CHECK_STR(cmd.argv[i++], "8000");
    CHECK_STR(cmd.argv[i++], "-c");
    CHECK_STR(cmd.argv[i++], "1");
    CHECK_STR(cmd.argv[i++], "-D");
    CHECK_STR(cmd.argv[i++], "plughw:2,0");
    CHECK(cmd.argv[i] == NULL);
}

/* The menu needs a name, not a device string. /proc/asound/cardN/id is the
 * kernel's own short name for the card -- "D100009002" for the ONN microphone
 * on the developer's desk -- and it is per-card, so it needs no parsing of the
 * /proc/asound/cards table. */
static void test_scan_labels_a_card_with_its_kernel_id(void)
{
    nd_mic_device found[8];
    size_t n;

    pt_mkdir("/asound/card2/pcm0c");
    pt_write_text("/asound/card2/id", "D100009002\n");

    n = nd_mic_scan(asound_root(), found, 8u);

    CHECK_INT(n, 1);
    CHECK_STR(found[0].label, "D100009002");
}

/* Silence in the middle, positive up. A band 101 rows tall starting at row 20
 * has its middle at 70, and the arithmetic has to put full scale exactly on
 * the edges rather than one short of them. */
static void test_sample_y_puts_silence_in_the_middle(void)
{
    CHECK_INT(nd_mic_sample_y(0, 20, 101), 70);
    CHECK_INT(nd_mic_sample_y(32767, 20, 101), 20);
    CHECK_INT(nd_mic_sample_y(-32768, 20, 101), 120);
}

/* ------------------------------------------------------------------ *
 * The mixer
 * ------------------------------------------------------------------ */

/* What a real C-Media adapter (0d8c:0014) publishes, in the order it lists
 * them -- the same listing neodct/tests/test_s17audio.py drives the boot
 * script with, because the shape is the whole point. "Mic Capture Volume" and
 * "Mic Playback Volume" are separate kernel controls that ALSA's simple mixer
 * merges into one control called "Mic", so `amixer sset Mic 100%` raises the
 * MONITOR path as well and puts the microphone into the earpiece. */
static const char CMEDIA_CONTROLS[] = "numid=1,iface=MIXER,name='Speaker Playback Switch'\n"
                                      "numid=2,iface=MIXER,name='Speaker Playback Volume'\n"
                                      "numid=3,iface=MIXER,name='Mic Playback Switch'\n"
                                      "numid=4,iface=MIXER,name='Mic Playback Volume'\n"
                                      "numid=5,iface=MIXER,name='Mic Capture Switch'\n"
                                      "numid=6,iface=MIXER,name='Mic Capture Volume'\n"
                                      "numid=7,iface=MIXER,name='Auto Gain Control'\n";

static void test_parse_controls_finds_the_capture_pair(void)
{
    nd_mic_control ctl[ND_MIC_CONTROLS_MAX];
    size_t n = nd_mic_parse_controls(CMEDIA_CONTROLS, ctl, ND_ARRAY_LEN(ctl));

    CHECK_INT(n, 2);
    if (n != 2u)
        return;
    CHECK_INT(ctl[0].numid, 5);
    CHECK_INT(ctl[0].kind, ND_MIC_CTL_SWITCH);
    CHECK_INT(ctl[1].numid, 6);
    CHECK_INT(ctl[1].kind, ND_MIC_CTL_VOLUME);
}

/* The monitor path, the speaker and the AGC are all left alone, and each for
 * its own reason: 3 and 4 feed the mic back into the earpiece, 1 and 2 belong
 * to MusicPlayer, and 7 is a taste question this does not get to answer. */
static void test_parse_controls_leaves_playback_and_agc_alone(void)
{
    nd_mic_control ctl[ND_MIC_CONTROLS_MAX];
    size_t n = nd_mic_parse_controls(CMEDIA_CONTROLS, ctl, ND_ARRAY_LEN(ctl));
    size_t i;

    for (i = 0u; i < n; i++) {
        CHECK(ctl[i].numid != 1 && ctl[i].numid != 2);
        CHECK(ctl[i].numid != 3 && ctl[i].numid != 4);
        CHECK(ctl[i].numid != 7);
    }
}

/* A codec with a master capture control AS WELL AS a per-input one holds the
 * microphone at zero through whichever of the two was left there, so both have
 * to come back. This is the case a "first match wins" parser gets wrong and
 * the failure is the same silence with more steps. */
static void test_parse_controls_returns_every_match(void)
{
    static const char TEXT[] = "numid=2,iface=MIXER,name='Capture Switch'\n"
                               "numid=3,iface=MIXER,name='Capture Volume'\n"
                               "numid=8,iface=MIXER,name='Mic Capture Switch'\n"
                               "numid=9,iface=MIXER,name='Mic Capture Volume'\n";
    nd_mic_control ctl[ND_MIC_CONTROLS_MAX];

    CHECK_INT(nd_mic_parse_controls(TEXT, ctl, ND_ARRAY_LEN(ctl)), 4);
}

/* The closing quote is part of the suffix. Without it "Mic Capture Volume
 * Enum" -- the input SOURCE on some codecs, not its level -- would match and
 * be cset to a percentage it has no idea what to do with. */
static void test_parse_controls_wants_the_whole_name(void)
{
    static const char TEXT[] = "numid=4,iface=MIXER,name='Mic Capture Volume Enum'\n"
                               "numid=5,iface=MIXER,name='Capture Switcher'\n"
                               "not a control line at all\n"
                               "numid=x,iface=MIXER,name='Mic Capture Volume'\n"
                               "numid6,iface=MIXER,name='Mic Capture Switch'\n";
    nd_mic_control ctl[ND_MIC_CONTROLS_MAX];

    CHECK_INT(nd_mic_parse_controls(TEXT, ctl, ND_ARRAY_LEN(ctl)), 0);
}

/* Nothing at all, from a card that publishes no capture control -- which the
 * RV1103's own codec really does. Empty is an answer, not a crash. */
static void test_parse_controls_survives_nothing(void)
{
    nd_mic_control ctl[ND_MIC_CONTROLS_MAX];

    CHECK_INT(nd_mic_parse_controls("", ctl, ND_ARRAY_LEN(ctl)), 0);
    CHECK_INT(nd_mic_parse_controls(NULL, ctl, ND_ARRAY_LEN(ctl)), 0);
    CHECK_INT(nd_mic_parse_controls(CMEDIA_CONTROLS, NULL, 4u), 0);
    CHECK_INT(nd_mic_parse_controls(CMEDIA_CONTROLS, ctl, 0u), 0);
}

/* The command, word for word, and by numid rather than by name: `-c N cset
 * numid=K P%` cannot be confused about which of two merged controls it meant. */
static void test_cset_command_is_by_numid(void)
{
    nd_mic_control ctl = {6, ND_MIC_CTL_VOLUME};
    nd_mic_mixer_command cmd;
    size_t i = 0u;

    CHECK_INT(nd_mic_cset_command(&cmd, 1, &ctl, 75), ND_OK);
    if (nd_mic_cset_command(&cmd, 1, &ctl, 75) != ND_OK)
        return;

    CHECK_STR(cmd.argv[i++], "amixer");
    CHECK_STR(cmd.argv[i++], "-c");
    CHECK_STR(cmd.argv[i++], "1");
    CHECK_STR(cmd.argv[i++], "cset");
    CHECK_STR(cmd.argv[i++], "numid=6");
    CHECK_STR(cmd.argv[i++], "75%");
    CHECK(cmd.argv[i] == NULL);
}

/* A switch takes "on" and never a percentage. Its numid is a switch because
 * the card said so, not because the caller passed a level. */
static void test_cset_command_switches_on(void)
{
    nd_mic_control ctl = {5, ND_MIC_CTL_SWITCH};
    nd_mic_mixer_command cmd;

    CHECK_INT(nd_mic_cset_command(&cmd, 1, &ctl, 0), ND_OK);
    if (nd_mic_cset_command(&cmd, 1, &ctl, 0) != ND_OK)
        return;
    CHECK_STR(cmd.argv[4], "numid=5");
    CHECK_STR(cmd.argv[5], "on");
}

static void test_controls_command_lists(void)
{
    nd_mic_mixer_command cmd;
    size_t i = 0u;

    CHECK_INT(nd_mic_controls_command(&cmd, 10), ND_OK);
    if (nd_mic_controls_command(&cmd, 10) != ND_OK)
        return;
    CHECK_STR(cmd.argv[i++], "amixer");
    CHECK_STR(cmd.argv[i++], "-c");
    CHECK_STR(cmd.argv[i++], "10");
    CHECK_STR(cmd.argv[i++], "controls");
    CHECK(cmd.argv[i] == NULL);
}

/* REFUSED, not clamped, and this is the security-shaped half of the change.
 * The percentage arrives from settings.prop, which lives on the writable
 * partition, and it ends up as an execve() argument -- so the builder is the
 * choke point and it says no rather than guessing. The caller that has a
 * string turns it into a number with nd_mic_gain_from_setting() first. */
static void test_cset_command_refuses_an_impossible_argument(void)
{
    nd_mic_control vol = {6, ND_MIC_CTL_VOLUME};
    nd_mic_control bad = {-1, ND_MIC_CTL_VOLUME};
    nd_mic_mixer_command cmd;

    CHECK_INT(nd_mic_cset_command(&cmd, 1, &vol, 101), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(&cmd, 1, &vol, -1), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(&cmd, -1, &vol, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(&cmd, ND_MIC_CARD_MAX + 1, &vol, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(&cmd, 1, &bad, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(&cmd, 1, NULL, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_cset_command(NULL, 1, &vol, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_controls_command(&cmd, ND_MIC_CARD_MAX + 1), ND_ERR_INVAL);

    /* The two ends are legal. 0 is what a muted card reports and the owner is
     * allowed to reproduce it. */
    CHECK_INT(nd_mic_cset_command(&cmd, 0, &vol, 0), ND_OK);
    CHECK_INT(nd_mic_cset_command(&cmd, ND_MIC_CARD_MAX, &vol, 100), ND_OK);
}

/* settings.prop is a text file on the partition an attacker who has got one
 * file onto this phone can write, and this value reaches an argv. Digits
 * only, at most 100, nothing trailing -- everything else is the default,
 * which is the same rule system.ui.brightness follows so that a hand-edited
 * file cannot leave the phone unusable. */
static void test_gain_from_setting_takes_numbers_and_nothing_else(void)
{
    CHECK_INT(nd_mic_gain_from_setting("0"), 0);
    CHECK_INT(nd_mic_gain_from_setting("55"), 55);
    CHECK_INT(nd_mic_gain_from_setting("100"), 100);
    CHECK_INT(nd_mic_gain_from_setting("  70  "), 70);

    CHECK_INT(nd_mic_gain_from_setting(NULL), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting(""), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("101"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("999999999999999999999"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("-5"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("loud"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("80%"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("80 dB"), ND_MIC_GAIN_DEFAULT);
    /* The shape somebody trying to smuggle a second argument in would use. */
    CHECK_INT(nd_mic_gain_from_setting("50 --quiet"), ND_MIC_GAIN_DEFAULT);
    CHECK_INT(nd_mic_gain_from_setting("50; reboot"), ND_MIC_GAIN_DEFAULT);
}

/* The same sscanf apps/Settings/main.c uses to find the Bluetooth fallback
 * card, applied to what nd_mic_scan() produces. */
static void test_card_of_reads_a_plughw_string(void)
{
    int32_t card = -1;

    CHECK(nd_mic_card_of("plughw:2,0", &card));
    CHECK_INT(card, 2);
    CHECK(nd_mic_card_of("plughw:10,3", &card));
    CHECK_INT(card, 10);

    /* A device the owner named by hand carries no card number, and that is
     * not a failure -- it is the mixer being left alone on purpose. */
    CHECK(!nd_mic_card_of("hw:1,0", &card));
    CHECK(!nd_mic_card_of("default", &card));
    CHECK(!nd_mic_card_of("plughw:99,0", &card));
    CHECK(!nd_mic_card_of("", &card));
    CHECK(!nd_mic_card_of(NULL, &card));
}

/* ------------------------------------------------------------------ *
 * apply_gain, against a stand-in amixer
 * ------------------------------------------------------------------ */

/* The harness's own fakebin amixer exits 0 and prints nothing, which is the
 * right answer for a suite that must not touch the machine's sound card and
 * the wrong one here, where what it was ASKED for is the whole assertion. So
 * a stand-in goes on the FRONT of $PATH and records every invocation.
 *
 * PREPENDED, not replaced -- which is where test_tones.c's mpv stub and this
 * one part company. That one can own $PATH outright because the thing it
 * starts is a binary; this one is a shell script, and a script whose $PATH
 * holds nothing but its own directory cannot find `cat`, so the heredoc that
 * answers `controls` produces nothing and the script still exits 0. That is a
 * stub reporting success with no output, which is the exact shape of the bug
 * being tested for, and it cost an afternoon once. The log is what proves our
 * amixer was the one that ran; being first on the path is what makes it so.
 * Prepending also leaves the harness's fakebin where it is. */
static char g_bindir[ND_PATH_MAX];
static char g_amixer_log[ND_PATH_MAX];
static char g_saved_path[ND_PATH_MAX];

/* Write `script` as the amixer on the front of $PATH, and point g_amixer_log
 * at the file it appends to. Shared by the two stubs below: one answers
 * instantly and one hangs, and only the script differs. */
static bool install_amixer(const char *script)
{
    char resolved[ND_PATH_MAX];
    FILE *f;

    if (nd_snprintf(resolved, sizeof resolved, "%s/amixer", g_bindir) != ND_OK)
        return false;

    f = fopen(resolved, "w");
    if (f == NULL)
        return false;
    (void)fputs(script, f);
    (void)fclose(f);
    if (chmod(resolved, 0755) != 0)
        return false;

    {
        const char *path = getenv("PATH");
        char ahead[ND_PATH_MAX * 2];

        (void)nd_strlcpy(g_saved_path, path != NULL ? path : "", sizeof g_saved_path);
        if (nd_snprintf(ahead, sizeof ahead, "%s:%s", g_bindir, g_saved_path) != ND_OK)
            return false;
        return setenv("PATH", ahead, 1) == 0;
    }
}

/* The directory the stub and its log live in. Made once per stub so that a
 * case which installs a second stub does not inherit the first one's log. */
static bool stub_dir(void)
{
    pt_mkdir("/bin");
    if (nd_path_resolve(g_bindir, sizeof g_bindir, "/bin") != ND_OK)
        return false;
    return nd_snprintf(g_amixer_log, sizeof g_amixer_log, "%s/amixer.log", g_bindir) == ND_OK;
}

static bool stub_amixer(void)
{
    char script[ND_PATH_MAX + 1024];

    if (!stub_dir())
        return false;
    if (nd_snprintf(script, sizeof script,
                    "#!/bin/sh\n"
                    "echo \"$*\" >> '%s'\n"
                    "if [ \"$1\" = -c ]; then shift 2; fi\n"
                    "if [ \"$1\" = controls ]; then\n"
                    "cat <<'EOF'\n%s"
                    "EOF\n"
                    "fi\n"
                    "exit 0\n",
                    g_amixer_log, CMEDIA_CONTROLS) != ND_OK)
        return false;
    return install_amixer(script);
}

static void stub_amixer_done(void)
{
    (void)setenv("PATH", g_saved_path, 1);
}

static size_t amixer_log(char *out, size_t out_sz)
{
    FILE *f = fopen(g_amixer_log, "rb");
    size_t n;

    out[0] = '\0';
    if (f == NULL)
        return 0u;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    return n;
}

/* THE FIX, end to end: the switch is turned on and the level set, on the card
 * it was given, and the monitor path is not touched. */
static void test_apply_gain_sets_the_capture_path(void)
{
    char log[2048];

    if (!stub_amixer()) {
        CHECK(false);
        return;
    }

    CHECK_INT(nd_mic_apply_gain(1, 100), ND_OK);
    (void)amixer_log(log, sizeof log);

    CHECK(strstr(log, "-c 1 controls") != NULL);
    CHECK(strstr(log, "-c 1 cset numid=5 on") != NULL);
    CHECK(strstr(log, "-c 1 cset numid=6 100%") != NULL);
    /* Mic Playback -- the mic in the earpiece. */
    CHECK(strstr(log, "numid=3") == NULL);
    CHECK(strstr(log, "numid=4") == NULL);
    /* Auto Gain Control. */
    CHECK(strstr(log, "numid=7") == NULL);
    /* MusicPlayer's. */
    CHECK(strstr(log, "numid=1 ") == NULL);
    CHECK(strstr(log, "numid=2 ") == NULL);

    stub_amixer_done();
}

static void test_apply_gain_passes_the_level_through(void)
{
    char log[2048];

    if (!stub_amixer()) {
        CHECK(false);
        return;
    }

    CHECK_INT(nd_mic_apply_gain(1, 45), ND_OK);
    (void)amixer_log(log, sizeof log);
    CHECK(strstr(log, "cset numid=6 45%") != NULL);

    stub_amixer_done();
}

/* THE SPLIT MicTest LEANS ON. It lists the card once when its screen opens
 * and then, on every UP or DOWN, csets only the VOLUME controls: the arrows
 * repeat every 120 ms, and re-listing a card that cannot have changed and
 * re-asserting a switch that is already on would be twenty-four amixer
 * children a second on a single-core Cortex-A7. */
static void test_the_controls_can_be_listed_once_and_set_many_times(void)
{
    nd_mic_control found[ND_MIC_CONTROLS_MAX];
    nd_mic_control levels[ND_MIC_CONTROLS_MAX];
    char log[2048];
    size_t n = 0u;
    size_t n_levels = 0u;
    size_t i;

    if (!stub_amixer()) {
        CHECK(false);
        return;
    }

    CHECK_INT(nd_mic_list_controls(1, found, ND_ARRAY_LEN(found), &n), ND_OK);
    CHECK_INT(n, 2);
    for (i = 0u; i < n; i++) {
        if (found[i].kind == ND_MIC_CTL_VOLUME)
            levels[n_levels++] = found[i];
    }
    CHECK_INT(n_levels, 1);

    CHECK_INT(nd_mic_set_controls(1, levels, n_levels, 30), ND_OK);
    (void)amixer_log(log, sizeof log);

    /* Exactly one listing, and the level moved without the switch. */
    CHECK(strstr(log, "-c 1 controls") != NULL);
    CHECK(strstr(strstr(log, "-c 1 controls") + 1, "-c 1 controls") == NULL);
    CHECK(strstr(log, "cset numid=6 30%") != NULL);
    CHECK(strstr(log, "cset numid=5 on") == NULL);

    stub_amixer_done();
}

/* A card with nothing to list says so once, and says it the same way whether
 * the caller asked for the listing or for the whole apply. The RV1103's own
 * codec really is this card. */
static void test_a_card_with_no_capture_control_is_not_a_failure_to_report(void)
{
    nd_mic_control found[ND_MIC_CONTROLS_MAX];
    size_t n = 99u;

    /* The harness's own fakebin amixer, which exits 0 and prints nothing. */
    CHECK_INT(nd_mic_list_controls(1, found, ND_ARRAY_LEN(found), &n), ND_ERR_NOTFOUND);
    CHECK_INT(n, 0);
    CHECK_INT(nd_mic_apply_gain(1, 50), ND_ERR_NOTFOUND);

    CHECK_INT(nd_mic_list_controls(1, NULL, 4u, &n), ND_ERR_INVAL);
    CHECK_INT(nd_mic_list_controls(1, found, 0u, &n), ND_ERR_INVAL);
    CHECK_INT(nd_mic_list_controls(1, found, 4u, NULL), ND_ERR_INVAL);
    CHECK_INT(nd_mic_set_controls(1, NULL, 2u, 50), ND_ERR_INVAL);
    CHECK_INT(nd_mic_set_controls(1, found, 0u, 50), ND_ERR_NOTFOUND);
    CHECK_INT(nd_mic_set_controls(1, found, 1u, 101), ND_ERR_INVAL);
}

/* ------------------------------------------------------------------ *
 * The aggregate bound
 * ------------------------------------------------------------------ */

/* Eight capture controls, which is ND_MIC_CONTROLS_MAX -- a codec with a
 * master capture pair as well as one per input really does publish this many,
 * and it is the worst case apply_gain() has to spawn a cset for. The two
 * playback controls are here so the count is a real filter and not the whole
 * listing. */
static const char CROWDED_CONTROLS[] = "numid=1,iface=MIXER,name='Speaker Playback Switch'\n"
                                       "numid=2,iface=MIXER,name='Mic Playback Volume'\n"
                                       "numid=3,iface=MIXER,name='Mic Capture Switch'\n"
                                       "numid=4,iface=MIXER,name='Mic Capture Volume'\n"
                                       "numid=5,iface=MIXER,name='Line Capture Switch'\n"
                                       "numid=6,iface=MIXER,name='Line Capture Volume'\n"
                                       "numid=7,iface=MIXER,name='Aux Capture Switch'\n"
                                       "numid=8,iface=MIXER,name='Aux Capture Volume'\n"
                                       "numid=9,iface=MIXER,name='Master Capture Switch'\n"
                                       "numid=10,iface=MIXER,name='Master Capture Volume'\n";

/* How long the hanging stub sits in each cset. Longer than the whole budget,
 * so the FIRST cset exhausts it and every later one has to be skipped rather
 * than merely cut short -- which is the half a per-child timeout gets wrong. */
#define STUB_HANG_S 3

/* The stand-in, taught to hang. The listing answers at once -- a card that
 * cannot even be listed never reaches the csets, and it is the csets that
 * multiply -- and then every cset sleeps for longer than the budget.
 *
 * `sleep` is looked up on the inherited $PATH, which is why the stub goes on
 * the FRONT of it rather than replacing it; the note above stub_amixer() has
 * the afternoon that cost. */
static bool stub_amixer_that_hangs(void)
{
    char script[ND_PATH_MAX + 2048];

    if (!stub_dir())
        return false;
    if (nd_snprintf(script, sizeof script,
                    "#!/bin/sh\n"
                    "echo \"$*\" >> '%s'\n"
                    "if [ \"$1\" = -c ]; then shift 2; fi\n"
                    "if [ \"$1\" = controls ]; then\n"
                    "cat <<'EOF'\n%s"
                    "EOF\n"
                    "exit 0\n"
                    "fi\n"
                    "sleep %d\n"
                    "exit 0\n",
                    g_amixer_log, CROWDED_CONTROLS, STUB_HANG_S) != ND_OK)
        return false;
    return install_amixer(script);
}

/* ============ THE NUMBER THE CALL PATH IS PROMISED ============
 *
 * apply_gain() runs on the modem thread inside one nd_modem_poll() tick, and
 * nd_modem.c's submit() makes the UI's END key wait for that tick. So what
 * matters is not what one amixer child costs but what the whole call costs,
 * and those were different numbers: a per-child bound of two seconds, applied
 * to one listing plus up to ND_MIC_CONTROLS_MAX csets, each of which could
 * also spend two seconds in nd_proc_wait() and another 2.5 s being
 * terminated, is fifty-eight seconds of frozen phone that nothing wrote down.
 *
 * So this case is the bound, not a benchmark of this host: a stub that sleeps
 * for longer than the budget on EVERY cset, a card with the maximum number of
 * them, and one assertion that the whole thing still returns inside
 * ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S. Before the shared deadline it
 * took eight sleeps rather than one.
 *
 * ============ THE SLACK, WHICH USED TO BE MOST OF THE ASSERTION ==========
 *
 * The headroom here was a flat 2.0 s against a bound of 2.5 s -- so the
 * assertion really said 4.5 s, and a regression that nearly doubled the real
 * cost of the call path would have passed it without a murmur. A test whose
 * tolerance is the same size as the thing it is measuring is not defending
 * the number, it is describing the machine it was written on.
 *
 * MIXER_BOUND_SLACK_S is what the HOST can add and the phone cannot: two
 * fork/execs of a shell, a SIGKILL, and poll(2) granularity. Half a second is
 * generous for that under ASan in the sandbox, and it is half the bound
 * rather than twice it, so the assertion is now mostly about nd_mic.c.
 */
#define MIXER_BOUND_SLACK_S 0.5
static void test_a_hanging_amixer_cannot_hold_the_call_path(void)
{
    char log[2048];
    double started;
    double spent;

    if (!stub_amixer_that_hangs()) {
        CHECK(false);
        return;
    }

    started = nd_time_monotonic();
    /* The return value is deliberately not asserted: a card that ate the
     * budget may have got its first cset in or may not, and either answer is
     * honest. The DURATION is the contract. */
    (void)nd_mic_apply_gain(1, 100);
    spent = nd_time_monotonic() - started;

    CHECK(spent <= ND_MIC_MIXER_BUDGET_S + ND_MIC_MIXER_REAP_S + MIXER_BOUND_SLACK_S);

    /* And it really was the crowded card, really answering slowly -- without
     * this the case would pass just as well against a stub that was never
     * found on $PATH at all.
     *
     * The floor is DERIVED, not typed: it was a flat 1.0 s, which was true of
     * a 2.0 s budget and would have started failing the moment the budget
     * dropped to 0.5. The first cset sleeps STUB_HANG_S, which is longer than
     * the whole budget, so run_amixer() sits in poll(2) until the deadline and
     * the call cannot return before ND_MIC_MIXER_BUDGET_S has really passed.
     * Half of it is a wide margin for clock granularity and still two orders
     * of magnitude above the few milliseconds an amixer that was never found
     * would cost. */
    (void)amixer_log(log, sizeof log);
    CHECK(strstr(log, "-c 1 controls") != NULL);
    CHECK(strstr(log, "cset numid=3 on") != NULL);
    CHECK(spent >= ND_MIC_MIXER_BUDGET_S / 2.0);

    stub_amixer_done();
}

/* An out-of-range level never reaches a child at all -- the refusal happens
 * before anything is spawned, so the log is empty rather than short. */
static void test_apply_gain_refuses_an_impossible_level(void)
{
    char log[2048];

    if (!stub_amixer()) {
        CHECK(false);
        return;
    }

    CHECK_INT(nd_mic_apply_gain(1, 250), ND_ERR_INVAL);
    CHECK_INT(nd_mic_apply_gain(1, -1), ND_ERR_INVAL);
    CHECK_INT(nd_mic_apply_gain(-3, 50), ND_ERR_INVAL);
    CHECK_INT(amixer_log(log, sizeof log), 0);

    stub_amixer_done();
}

int main(void)
{
    RUN(test_scan_finds_a_capture_device);
    RUN(test_scan_ignores_a_playback_only_card);
    RUN(test_reduce_keeps_the_extremes_of_each_column);
    RUN(test_reduce_fills_only_what_it_has);
    RUN(test_record_command_is_raw_mono_on_stdout);
    RUN(test_scan_labels_a_card_with_its_kernel_id);
    RUN(test_sample_y_puts_silence_in_the_middle);

    RUN(test_parse_controls_finds_the_capture_pair);
    RUN(test_parse_controls_leaves_playback_and_agc_alone);
    RUN(test_parse_controls_returns_every_match);
    RUN(test_parse_controls_wants_the_whole_name);
    RUN(test_parse_controls_survives_nothing);
    RUN(test_cset_command_is_by_numid);
    RUN(test_cset_command_switches_on);
    RUN(test_controls_command_lists);
    RUN(test_cset_command_refuses_an_impossible_argument);
    RUN(test_gain_from_setting_takes_numbers_and_nothing_else);
    RUN(test_card_of_reads_a_plughw_string);
    RUN(test_apply_gain_sets_the_capture_path);
    RUN(test_apply_gain_passes_the_level_through);
    RUN(test_apply_gain_refuses_an_impossible_level);
    RUN(test_the_controls_can_be_listed_once_and_set_many_times);
    RUN(test_a_card_with_no_capture_control_is_not_a_failure_to_report);
    RUN(test_a_hanging_amixer_cannot_hold_the_call_path);

    return pt_report("test_mic");
}
