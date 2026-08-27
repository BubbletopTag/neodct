/* nd_mic.h -- finding capture devices, and turning what they capture into
 * something a 240x175 screen can show.
 *
 * The engineering app MicTest is the only consumer today. The decisions live
 * here rather than in the app for the reason nd_remoteshell.h gives: what a
 * test can reach is what gets tested, and an app's .so is the awkward half.
 *
 * There is no in-process ALSA here and there is not meant to be. alsa-lib is
 * not in the image -- alsa-utils is -- so capture goes through arecord on a
 * pipe, exactly as call audio does in nd_modem_audio.c. That file is the
 * precedent for every choice below.
 */

#ifndef ND_MIC_H_INCLUDED
#define ND_MIC_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the kernel lists its sound cards. A parameter rather than a constant
 * at the call site below, because a test cannot write to the real one. */
#define ND_MIC_ASOUND_DIR "/proc/asound"

/* "plughw:10,0" is the longest realistic form by a wide margin. */
#define ND_MIC_DEVICE_MAX 32

/* One capture device the phone can record from. */
/* The kernel's own short name is the longest thing shown; 64 is generous. */
#define ND_MIC_LABEL_MAX 64

typedef struct {
    char device[ND_MIC_DEVICE_MAX]; /* what `arecord -D` takes */
    char label[ND_MIC_LABEL_MAX];   /* what the menu shows */
} nd_mic_device;

/* scan(): every capture device under `asound_root`, in the order the kernel
 * lists them. Returns how many were written to `out`.
 *
 * Capture, not playback: a card is interesting here only if it has a pcm*c
 * node. QEMU's USB Audio is playback-only card 0, so a phone with a real
 * microphone plugged in has cards that must be told apart -- which is the
 * whole reason this app exists.
 */
size_t nd_mic_scan(const char *asound_root, nd_mic_device *out, size_t max);

/* ------------------------------------------------------------------ *
 * The waveform
 * ------------------------------------------------------------------ */

/* One screen column: the lowest and highest sample that landed in it. A
 * waveform is drawn as a vertical line between the two, which is what makes a
 * loud passage a thick band and a quiet one a thin thread -- an average would
 * show neither. */
typedef struct {
    int16_t min;
    int16_t max;
} nd_mic_column;

/* reduce(): `n_samples` signed 16-bit mono samples into `columns` columns.
 * Returns how many columns were filled, which is fewer than asked for when
 * there are not enough samples to go round -- a partial screen is honest and
 * a stretched one is not. */
size_t nd_mic_reduce(const int16_t *samples, size_t n_samples, nd_mic_column *out, size_t columns);

/* ------------------------------------------------------------------ *
 * The capture command
 * ------------------------------------------------------------------ */

/* arecord, and the numbers that keep it cheap. 8 kHz mono is the modem's own
 * rate and is far more than a 240-pixel waveform can show; asking for 48 kHz
 * stereo would cost twelve times the bytes to draw the same picture on a phone
 * with 64 MB. */
#define ND_MIC_RATE     8000
#define ND_MIC_FORMAT   "S16_LE"
#define ND_MIC_ARGV_MAX 16

/* The command, and the buffer its numeric argument points into -- an argv of
 * `const char *` cannot own a formatted number, and a caller that formats it
 * into a local is a dangling pointer waiting to happen. */
typedef struct {
    const char *argv[ND_MIC_ARGV_MAX];
    char rate[16];
} nd_mic_command;

/* record_command(): `arecord` writing raw mono to stdout, for the given
 * device. Refuses an empty device rather than falling back to "default",
 * which on this hardware is a playback-only card. */
nd_err nd_mic_record_command(nd_mic_command *out, const char *device, int rate);

/* sample_y(): where a sample sits inside a band `height` pixels tall starting
 * at row `top`. Silence is the middle row, full positive is the top row and
 * full negative the bottom, because a waveform is drawn the way an
 * oscilloscope draws one -- up for positive. */
int32_t nd_mic_sample_y(int16_t sample, int32_t top, int32_t height);

#ifdef __cplusplus
}
#endif

#endif /* ND_MIC_H_INCLUDED */
