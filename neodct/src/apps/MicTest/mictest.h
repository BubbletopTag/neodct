/* mictest.h -- the parts of the MicTest app a unit test can reach.
 *
 * The app is a device picker over nd_mic.h and then a screen that draws what
 * the chosen device is hearing. Almost all of it is widget calls and one loop
 * around a pipe; the decisions -- which devices exist, what arecord is asked
 * for, how samples become columns and columns become rows -- are all in
 * nd_mic.c, where test_mic.c reaches them without an app process.
 *
 * ============ WHAT IS DELIBERATELY NOT TESTED ============
 *
 * app_run(). It forks a real arecord and reads a real pipe for as long as the
 * screen is up. A unit test that did that would need a capture device on the
 * machine running it -- a CI runner has none, and a developer's would record
 * the room. The hole is the same one remote_app.h names around nd_rs_start(),
 * and it is named here rather than discovered.
 *
 * What covers this instead is running it: `neodct/tools/run_qemu.sh` with an
 * audio input wired through, and the waveform either moves when you speak or
 * it does not.
 */

#ifndef ND_MICTEST_H_INCLUDED
#define ND_MICTEST_H_INCLUDED

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

/* A tenth of a second of 8 kHz mono: small enough that the screen keeps up on
 * a 64 MB phone, large enough that a column is an average of several samples
 * rather than a single one. */
#define ND_MICTEST_CHUNK 800

#ifdef __cplusplus
}
#endif

#endif /* ND_MICTEST_H_INCLUDED */
