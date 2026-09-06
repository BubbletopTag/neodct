/* sleepy.h -- the numbers and strings apps/Sleepy/main.c draws, exposed so a
 * test can name them without dlopen()ing the app.
 *
 * The same arrangement as apps/MicTest/mictest.h and apps/LCDTest/lcdtest.h:
 * anything an app decides that is worth checking lives either in lib/ (the
 * hardware decisions -- nd_cpufreq.h and the backlight half of nd_fb.h) or
 * here (the presentation ones). What is left in main.c is the key loop.
 */

#ifndef SLEEPY_H_INCLUDED
#define SLEEPY_H_INCLUDED

#include "nd_types.h"

/* Next after Bluetooth's 9007. The engineering menu is sorted by id, so this
 * puts Sleepy at the end of it -- which is where a brand-new bring-up app
 * belongs until it has earned a place beside the ones that work. */
#define SLEEPY_APP_ID 9008

/* How long BLANK! holds the panel dark. Ten seconds is long enough to be sure
 * the screen really went off rather than flickered, and short enough that a
 * phone whose backlight never comes back is only ten seconds of confusion
 * rather than a reboot. */
#define SLEEPY_BLANK_SECONDS 10.0

/* The two root rows, in the order they are drawn. */
#define SLEEPY_ROOT_ITEMS 2
#define SLEEPY_DISPLAY_ITEMS 2

/* Zero is reserved for the timed blank, so the picker always leaves a way
 * to see the menu. These ten steps map onto the backlight's native range. */
#define SLEEPY_BRIGHTNESS_LEVELS 10

extern const char *const nd_sleepy_title;
extern const char *const nd_sleepy_root_items[SLEEPY_ROOT_ITEMS];
extern const char *const nd_sleepy_display_items[SLEEPY_DISPLAY_ITEMS];

extern const char *const nd_sleepy_no_cpufreq;
extern const char *const nd_sleepy_no_backlight;

#endif /* SLEEPY_H_INCLUDED */
