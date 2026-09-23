/* nd_launchfx.h -- the glass themes' app launch: the app's first frame blurs
 * into focus and pops up to size over the screen it is replacing.
 *
 * ============ WHAT IT LOOKS LIKE ============
 *
 * Three things move at once, over ND_LAUNCHFX_STEPS frames:
 *
 *   - the app's frame starts heavily blurred and sharpens, so it reads as
 *     glass coming into focus rather than a picture being pasted on;
 *   - it starts at ND_LAUNCHFX_SCALE0 of full size and grows past full size
 *     by a hair before settling -- the overshoot is the "pop", and without it
 *     the zoom reads as a slow camera move;
 *   - it fades in over the outgoing screen, which itself defocuses and dims a
 *     little, so the eye is pulled to the thing arriving.
 *
 * The last frame is the app's frame EXACTLY, byte for byte. Nothing is left
 * half-blurred if a step is dropped, because the caller always finishes with
 * a plain present.
 *
 * ============ WHY IT IS A THEME SWITCH AND NOT A SETTING ============
 *
 * It is part of the look, the way gloss is: iOS 6 glass zooms, a 3310 does
 * not. So it is ND_TH_LAUNCH_ANIM, off in the built-in Classic and on in the
 * two glass themes, and a theme that says nothing gets the instant launch.
 *
 * ============ COST ============
 *
 * Five RGB images the size of the canvas -- both frames as they are, a
 * defocused copy of each for the frame being drawn, and the blur's halfway
 * point -- plus two line buffers: about 630 KB for the 200 ms of the
 * transition, freed before the app reads its first key. Keeping the pristine
 * copies is what lets every frame blur from the original instead of blurring
 * a blur, which would only ever get softer and could never come back into
 * focus. Per frame it is at most two blurs and one bilinear resample of a
 * 240x175 band, about 1 ms on an x86 host and a few on the Cortex-A7 -- the
 * frame pacing, not the arithmetic, sets the length.
 */

#ifndef ND_LAUNCHFX_H_INCLUDED
#define ND_LAUNCHFX_H_INCLUDED

#include "nd_image.h"
#include "nd_types.h"

/* Ten frames at 20 ms: 200 ms is long enough to be seen as a transition and
 * short enough that nobody waits for it. It is added to a launch that already
 * costs the dlopen, so it must not be longer. */
#define ND_LAUNCHFX_STEPS    10
#define ND_LAUNCHFX_FRAME_MS 20

/* Where the pop starts, as a fraction of full size. */
#define ND_LAUNCHFX_SCALE0 0.80

/* The widest blur, in pixels of box radius, on the arriving frame and on the
 * departing one at the end. The arriving one is wider because it has to read
 * as out of focus at a glance; the departing one only has to recede. */
#define ND_LAUNCHFX_BLUR_IN  8
#define ND_LAUNCHFX_BLUR_OUT 4

typedef struct nd_launchfx nd_launchfx;

/* Snapshots both frames, so the caller may reuse either buffer afterwards.
 * Both must be the same size and RGB888 or RGBA8888. NULL on bad input or no
 * memory, and a launch simply happens without the transition. */
nd_launchfx *nd_launchfx_new(const nd_image *before, const nd_image *after);
void nd_launchfx_free(nd_launchfx *fx);

/* Compose frame `step` of `steps` into dst, which must be the same size. Step
 * 0 is the outgoing screen untouched, step == steps is `after` exactly. */
nd_err nd_launchfx_frame(nd_launchfx *fx, nd_image *dst, int32_t step, int32_t steps);

/* The curves, exposed for the tests: t in [0, 1]. */
double nd_launchfx_scale(double t);
double nd_launchfx_ease(double t);

#endif /* ND_LAUNCHFX_H_INCLUDED */
