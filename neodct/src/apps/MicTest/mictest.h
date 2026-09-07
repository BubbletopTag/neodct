/* mictest.h -- the parts of the MicTest app a unit test can reach.
 *
 * The app is a device picker over nd_mic.h and then a screen that draws what
 * the chosen device is hearing, with the capture level under the owner's
 * thumb. Almost all of it is widget calls and one loop around a pipe; the
 * decisions -- which devices exist, what arecord is asked for, which mixer
 * controls a card has and what may be handed to amixer, how samples become
 * columns and columns become rows -- are all in nd_mic.c, where test_mic.c
 * reaches them without an app process. The three below are what is left over
 * once that is true, and they are exported for the same reason.
 *
 * ============ WHAT IS DELIBERATELY NOT TESTED ============
 *
 * app_run(). It forks a real arecord and reads a real pipe for as long as the
 * screen is up. A unit test that did that would need a capture device on the
 * machine running it -- a CI runner has none, and a developer's would record
 * the room. The hole is the same one remote_app.h names around nd_rs_start(),
 * and it is named here rather than discovered.
 *
 * The three functions below are the parts of that loop that CAN be judged
 * without a microphone: what a key does to the level, what the level owes
 * settings.prop when the screen closes, and what the readout says about the
 * loudest sample in the frame. Between them they are every decision the loop
 * makes; what is left is the drawing and the pipe.
 *
 * What covers the rest is running it: `neodct/tools/run_qemu.sh` with an
 * audio input wired through, and the waveform either moves when you speak or
 * it does not.
 */

#ifndef ND_MICTEST_H_INCLUDED
#define ND_MICTEST_H_INCLUDED

#include "nd_mic.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "MicTest" is 96px in font_xl against the 136 the header leaves beside the
 * breadcrumb counter, so it fits without the framework having to trim it. */
extern const char *const nd_mictest_title;

/* The two messages this app puts on screen by itself. */
extern const char *const nd_mictest_no_device;
extern const char *const nd_mictest_no_arecord;

/* How many capture devices the picker will show. More than this on a phone
 * would mean something is wrong with the kernel, not with the list. */
#define ND_MICTEST_MAX_DEVICES 8

/* The waveform band, in the 240x175 UI. The header owns the top, the softkey
 * bar the bottom, and the level readout one line above that. */
#define ND_MICTEST_BAND_TOP    36
#define ND_MICTEST_BAND_HEIGHT 101
#define ND_MICTEST_COLUMNS     224

/* The readout row -- the line mictest's layout has always reserved between
 * the waveform and the softkey bar. It used to carry one message and now it
 * carries the gain on the left and the peak on the right, because those are
 * the two numbers that answer "is it working, and is it loud enough". */
#define ND_MICTEST_READOUT_Y (ND_UI_H - 30)

/* A tenth of a second of 8 kHz mono: small enough that the screen keeps up on
 * a 64 MB phone, large enough that a column is an average of several samples
 * rather than a single one. */
#define ND_MICTEST_CHUNK 800

/* ------------------------------------------------------------------ *
 * The three decisions the waveform loop makes
 * ------------------------------------------------------------------ */

/* step(): the capture level after one key. UP and DOWN move it by
 * ND_MIC_GAIN_STEP and clamp to ND_MIC_GAIN_MIN..ND_MIC_GAIN_MAX; EVERY OTHER
 * KEY LEAVES IT ALONE, so the loop can hand this whatever
 * nd_ui_read_keypress() returned, including the -1 that means "nothing was
 * pressed", without a branch of its own.
 *
 * Up and down rather than a slider screen, because that is already this
 * phone's idiom for a level -- MusicPlayer's Now Playing takes UP and DOWN
 * for volume without leaving the screen -- and because the whole point here
 * is to hear and see the change while making it. A picker you have to leave
 * the waveform to reach would answer the question you were not asking. */
int32_t nd_mictest_step(int32_t gain, int32_t key);

/* save_gain(): the ONE settings write this app makes.
 *
 * It is a function rather than three lines at the bottom of the loop so that
 * a test can prove where it is called from, and it is called from ONE place:
 * after the loop has ended, once. Not per keypress. R-24 (nd_settings.h) is
 * why -- every nd_settings_set() rewrites the whole of settings.prop with an
 * fsync, and holding UP for two seconds is eight presses, which would be
 * eight full rewrites of the user partition to move one number. On UBIFS over
 * SPI NAND that is measurable wear for nothing.
 *
 * Writes only when `gain` differs from the `loaded` value the screen opened
 * with, so a session that looked at the level and put it back writes nothing
 * at all. Returns whether it wrote. */
bool nd_mictest_save_gain(int32_t gain, int32_t loaded);

/* peak_percent(): the loudest sample in the frame, 0..100, out of the columns
 * the waveform is already drawn from -- so the number and the picture can
 * never disagree. Both ends count: a signal clipping negative is as loud as
 * one clipping positive, and an electret with a DC offset does one before the
 * other. */
int32_t nd_mictest_peak_percent(const nd_mic_column *columns, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ND_MICTEST_H_INCLUDED */
