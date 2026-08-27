/* nd_mic.c -- see nd_mic.h. */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "nd_mic.h"

static bool all_digits(const char *s, size_t n)
{
    size_t i;

    if (n == 0u)
        return false;
    for (i = 0u; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

/* "pcm0c" -> "0", and only when the node really is a capture one. The trailing
 * c is the whole distinction between a microphone and a speaker here. */
static bool capture_index(const char *name, char *out, size_t out_sz)
{
    size_t len = strlen(name);

    if (len < 5u || strncmp(name, "pcm", 3u) != 0 || name[len - 1u] != 'c')
        return false;
    if (!all_digits(&name[3], len - 4u))
        return false;
    if (len - 4u >= out_sz)
        return false;
    memcpy(out, &name[3], len - 4u);
    out[len - 4u] = '\0';
    return true;
}

/* /proc/asound/cardN/id, stripped of its newline. Empty when the file is not
 * there, which the caller shows as the device string instead -- a card with no
 * name is still a card you can record from. */
static void read_card_id(const char *card_dir, char *out, size_t out_sz)
{
    char path[512];
    FILE *f;
    size_t n;

    out[0] = '\0';
    if (snprintf(path, sizeof path, "%s/id", card_dir) < 0)
        return;
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    n = fread(out, 1u, out_sz - 1u, f);
    out[n] = '\0';
    (void)fclose(f);
    while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' ')) {
        n--;
        out[n] = '\0';
    }
}

size_t nd_mic_scan(const char *asound_root, nd_mic_device *out, size_t max)
{
    DIR *root;
    struct dirent *card;
    size_t found = 0u;

    if (asound_root == NULL || out == NULL || max == 0u)
        return 0u;

    root = opendir(asound_root);
    if (root == NULL)
        return 0u;

    while (found < max && (card = readdir(root)) != NULL) {
        char dir[512];
        char number[8];
        DIR *inside;
        struct dirent *node;
        size_t len = strlen(card->d_name);

        if (len < 5u || strncmp(card->d_name, "card", 4u) != 0)
            continue;
        if (!all_digits(&card->d_name[4], len - 4u))
            continue;
        /* Copied into a bounded buffer rather than pointed at: d_name is 256
         * bytes and the compiler is right that "card" plus 251 digits would
         * not fit the device string. No such card exists; refusing it costs
         * nothing and keeps the format checkable. */
        if (len - 4u >= sizeof number)
            continue;
        memcpy(number, &card->d_name[4], len - 4u);
        number[len - 4u] = '\0';
        if (snprintf(dir, sizeof dir, "%s/%s", asound_root, card->d_name) < 0)
            continue;

        inside = opendir(dir);
        if (inside == NULL)
            continue;
        while (found < max && (node = readdir(inside)) != NULL) {
            char index[8];

            if (!capture_index(node->d_name, index, sizeof index))
                continue;
            (void)snprintf(out[found].device, sizeof out[found].device, "plughw:%s,%s", number,
                           index);
            read_card_id(dir, out[found].label, sizeof out[found].label);
            found++;
        }
        (void)closedir(inside);
    }
    (void)closedir(root);
    return found;
}

size_t nd_mic_reduce(const int16_t *samples, size_t n_samples, nd_mic_column *out, size_t columns)
{
    size_t per;
    size_t c;

    if (samples == NULL || out == NULL || columns == 0u)
        return 0u;

    /* Fewer samples than columns is a short read, which the first read off a
     * freshly started arecord routinely is. One sample per column then, and
     * only as many columns as there are samples: stretching them across the
     * whole screen would draw a waveform nobody captured. */
    if (n_samples < columns)
        columns = n_samples;
    if (columns == 0u)
        return 0u;
    per = n_samples / columns;

    for (c = 0u; c < columns; c++) {
        const int16_t *block = &samples[c * per];
        int16_t lo = block[0];
        int16_t hi = block[0];
        size_t i;

        for (i = 1u; i < per; i++) {
            if (block[i] < lo)
                lo = block[i];
            if (block[i] > hi)
                hi = block[i];
        }
        out[c].min = lo;
        out[c].max = hi;
    }
    return columns;
}

nd_err nd_mic_record_command(nd_mic_command *out, const char *device, int rate)
{
    size_t n = 0u;

    /* No fallback to "default". On a phone with QEMU's playback-only card 0 in
     * it, "default" is a device arecord cannot capture from, and the failure
     * arrives as silence rather than as an error. */
    if (out == NULL || device == NULL || device[0] == '\0' || rate <= 0)
        return ND_ERR_INVAL;

    if (nd_snprintf(out->rate, sizeof out->rate, "%d", rate) != ND_OK)
        return ND_ERR_TOOLONG;

    out->argv[n++] = "arecord";
    out->argv[n++] = "-q"; /* its chatter would land in the middle of the PCM */
    out->argv[n++] = "-t";
    out->argv[n++] = "raw";
    out->argv[n++] = "-f";
    out->argv[n++] = ND_MIC_FORMAT;
    out->argv[n++] = "-r";
    out->argv[n++] = out->rate;
    out->argv[n++] = "-c";
    out->argv[n++] = "1";
    out->argv[n++] = "-D";
    out->argv[n++] = device;
    out->argv[n] = NULL; /* no output file: the stream comes back on stdout */
    return ND_OK;
}

int32_t nd_mic_sample_y(int16_t sample, int32_t top, int32_t height)
{
    int32_t middle;
    int32_t half;
    int32_t offset;

    if (height <= 0)
        return top;

    /* An odd height has a true middle row; an even one rounds down, which puts
     * silence one row above centre rather than between two rows. */
    half = (height - 1) / 2;
    middle = top + half;

    /* The two halves are scaled against different denominators, deliberately.
     * Signed 16-bit runs -32768..32767, so there is no positive twin for the
     * most negative sample: one divisor for both ends leaves one of them a row
     * short of the edge. A loud signal touching the edge is the whole point of
     * a level display, so each end is scaled against its own full scale. */
    if (sample >= 0)
        offset = (int32_t)(((int64_t)sample * half) / 32767);
    else
        offset = (int32_t)(((int64_t)sample * half) / 32768);
    return middle - offset;
}
