/* nd_easteregg.h -- what happens if you dial the thing.
 *
 * A dialled code the home screen recognises, a screenful of official-looking
 * nonsense scrolling past, and then a video. It does nothing, it changes
 * nothing, and it is not in the changelog on purpose.
 *
 * CORE PROCESS ONLY, for the same reason nd_dialer.c is: it draws over the
 * home screen and it spawns a child, and an app process has neither the
 * canvas nor the broker for that.
 *
 * ============ WHY THE PIECES ARE SPLIT THIS WAY ============
 *
 * nd_easteregg_is_code() and nd_easteregg_line() are pure and exported so the
 * test can check them without a framebuffer: one decides whether a string is
 * the code, the other turns a line number into a line of text. Everything
 * that needs a screen or a process is behind nd_easteregg_run(), which the
 * test does not call.
 */

#ifndef ND_EASTEREGG_H_INCLUDED
#define ND_EASTEREGG_H_INCLUDED

#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The code, exactly as it must be dialled. Compared whole -- see
 * nd_easteregg_is_code() for why a prefix or a suffix must not match. */
#define ND_EASTEREGG_CODE "###22327753***"

/* Where the video lives on the read-only side. Absent on a build that did not
 * ship it (and on QEMU), which nd_easteregg_run() treats as "do nothing"
 * rather than as an error worth a dialog. */
#define ND_EASTEREGG_VIDEO "/NeoDCT/System/core/easteregg/badapple.mp4"

/* How the scroll is paced.
 *
 * MORE THAN ONE LINE PER FRAME, because the limit here is not how fast lines
 * can be generated -- it is the panel. A present is 240x175 pushed over SPI
 * and costs tens of milliseconds, so one line per present would crawl at
 * about twenty lines a second no matter what delay was asked for. Emitting a
 * few lines per frame is what makes it read as a machine talking to itself
 * rather than a list being typed out.
 *
 * The totals are chosen so the whole thing is over in a couple of seconds:
 * long enough to see, short enough that nobody waits through it. */
#define ND_EGG_LINES_TOTAL     96
#define ND_EGG_LINES_PER_FRAME 3
#define ND_EGG_FRAME_S         0.012

/* Visible rows, and the pitch between them. font_s is 14 px, so twelve rows
 * of 14 fill 168 of the panel's 175 and leave a small margin at the bottom
 * rather than a clipped thirteenth. */
#define ND_EGG_ROWS  12
#define ND_EGG_PITCH 14

/* Longest line the generator will produce, plus the NUL. Kept small enough
 * that twelve of them are a stack allocation nobody has to think about. */
#define ND_EGG_LINE_MAX 40

/* The video is three and a half minutes; this is the ceiling on waiting for
 * it, not an expectation. A player wedged on a decode must not take the phone
 * with it. */
#define ND_EGG_PLAY_TIMEOUT_S 300.0

/* Whether `dialed` is the code.
 *
 * A WHOLE-STRING match, not a prefix and not a suffix. The code is entered
 * into the same buffer as a phone number, so a match on "starts with" would
 * fire while somebody was still typing something longer, and a match on
 * "contains" would fire on a number that merely had it inside. NULL is false.
 */
bool nd_easteregg_is_code(const char *dialed);

/* Line number `n` as text, NUL-terminated, always written.
 *
 * Deterministic in `n` alone: the same line number gives the same line, every
 * run. That is what lets the test assert the shape of the output at all, and
 * it costs nothing -- the point is that it looks busy, not that it is
 * unpredictable. */
void nd_easteregg_line(uint32_t n, char *out, size_t out_sz);

/* The whole show: scroll, play, return. Blocks for as long as it takes, and
 * returns with the canvas dirty -- the caller repaints, which it was going to
 * do anyway on the way back to the home screen.
 *
 * Silent about everything that can go wrong. No video on this build, no mpv
 * in the image, a child that will not spawn: all of them just end the show
 * early. An easter egg that popped up an error dialog would be worse than one
 * that did nothing. */
void nd_easteregg_run(nd_ui *ui);

#ifdef __cplusplus
}
#endif

#endif /* ND_EASTEREGG_H_INCLUDED */
