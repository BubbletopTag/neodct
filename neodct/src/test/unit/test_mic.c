/* test_mic.c -- capture-device discovery and waveform reduction.
 *
 * The scan is pointed at a directory the test builds rather than at the real
 * /proc/asound, which is why nd_mic_scan() takes a root at all. nd_modem_audio.c
 * hardcodes the real one and is untestable for exactly that reason.
 */

#include <stdio.h>
#include <string.h>

#include "nd_mic.h"
#include "nd_paths.h"

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

int main(void)
{
    RUN(test_scan_finds_a_capture_device);
    RUN(test_scan_ignores_a_playback_only_card);
    RUN(test_reduce_keeps_the_extremes_of_each_column);
    RUN(test_reduce_fills_only_what_it_has);
    RUN(test_record_command_is_raw_mono_on_stdout);
    RUN(test_scan_labels_a_card_with_its_kernel_id);
    RUN(test_sample_y_puts_silence_in_the_middle);
    return pt_report("test_mic");
}
