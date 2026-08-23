/* nd_main.c -- nd-core's entry point.
 *
 * ============ THIS IS THE WALKING SKELETON ============
 *
 * Right now this file does the first two things launcher.py does -- pick the
 * serial console and print the boot banner -- and then stops. It exists so
 * that the headers, the library and the build are provably end-to-end before
 * anyone writes a widget: `make && ./build/default/bin/nd-core` produces the
 * real coloured boot output on a developer's terminal.
 *
 * The core-loop work package replaces the body of main() with the full boot
 * sequence from spec-core-loop.md section 1, IN THIS ORDER, because every step
 * depends on the one before it:
 *
 *   1. nd_log_redirect_serial()      -- before anything can print
 *   2. nd_clock_start(true, ...)     -- before anything can reach the network
 *   3. nd_rs_start_if_enabled()      -- the remote shell, if it was left on
 *   4. "[Launcher] Initializing Hardware...", then nd_fb_open()
 *   5. the boot splash, then EXACTLY one second
 *   6. "[Launcher] Starting UI...", then nd_core_run(fb)
 *
 * Every step's failure is caught and boot continues. A phone that will not
 * boot because NTP was unreachable is worse than a phone with the wrong time.
 */

#include <stdio.h>

#include "nd_log.h"
#include "nd_paths.h"

#define BANNER_RULE_WIDTH 72
#define BANNER_RULE_CHAR  '='
#define BANNER_COLOUR \
    46 /* the green the CORE tag uses, so the banner and
                              * the OS that follows it read as one voice */

static void print_banner(void)
{
    char line[256];
    char banner[24][ND_LOG_BANNER_COLS];
    size_t n;
    size_t i;

    (void)nd_log_rule(line, sizeof line, BANNER_RULE_CHAR, BANNER_RULE_WIDTH, BANNER_COLOUR);
    (void)printf("\n%s\n", line);

    /* Pre-rendered at build time by post-build-system-metadata.sh from the
     * same VERSION_ID the rest of the image uses, so nothing here parses a
     * version and nothing here can disagree with one. An image without the
     * file simply has no art. */
    n = nd_log_banner_lines(ND_PATH_BANNER, banner, ND_ARRAY_LEN(banner));
    for (i = 0u; i < n; i++) {
        (void)nd_log_paint(line, sizeof line, banner[i], BANNER_COLOUR, true);
        (void)printf("%s\n", line);
    }

    (void)nd_log_rule(line, sizeof line, BANNER_RULE_CHAR, BANNER_RULE_WIDTH, BANNER_COLOUR);
    (void)printf("%s\n\n", line);
}

int main(int argc, char **argv)
{
    ND_UNUSED(argc);
    ND_UNUSED(argv);

    print_banner();

    nd_log(ND_LOG_LAUNCHER, "Initializing Hardware...");
    nd_log(ND_LOG_FB, "no framebuffer yet -- walking skeleton");
    nd_log(ND_LOG_LAUNCHER, "Starting UI...");
    nd_log(ND_LOG_CORE, "Entering Main Loop...");
    nd_log(ND_LOG_CORE, "...and leaving it again: nd-core is not implemented yet.");

    return 0;
}
