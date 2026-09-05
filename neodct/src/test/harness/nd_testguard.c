/* nd_testguard.c -- linked into EVERY test binary and test app by the
 * Makefile, never into anything that ships.
 *
 * Two jobs, both done in a constructor so that no test's main() has to
 * remember either:
 *
 *   1. nd_svc_halt_disarm(): this process can never resolve a real poweroff
 *      or reboot, whatever its environment says. The 2026-09-04 shutdown came
 *      through a test binary run by hand without NEODCT_ROOT, the variable the
 *      interlock of the day keyed on; a latch set by the binary itself cannot
 *      be left behind by the person starting it.
 *
 *   2. Refuse to run outside the harness. `make test` and `make test-one`
 *      start the suite inside test/harness/sandbox.sh -- no D-Bus, no
 *      network, a minimal /dev -- with test/harness/fakebin first on $PATH,
 *      and mark the environment NEODCT_TEST_HARNESS=1. A binary started any
 *      other way stops here, before main(), with a message saying how to run
 *      it. NEODCT_ALLOW_BARE=1 overrides that for somebody who has read this
 *      and accepts running it on the machine in front of them; the disarm
 *      above still applies.
 *
 * Only async-signal-safe calls, no allocation, nothing that could itself
 * misbehave: this runs before the C library's own setup is necessarily done.
 * The exit status 3 is what test_harness.c expects from a bare run. */

#include <stdlib.h>
#include <unistd.h>

#include "nd_svc.h"

#define ND_TESTGUARD_EXIT 3

static const char ND_TESTGUARD_MSG[] =
    "nd_testguard: this is a NeoDCT test binary and it is not running under the\n"
    "nd_testguard: test harness. Run it with `make test` or `make test-one T=<name>`\n"
    "nd_testguard: (cd neodct/src), which sandbox it, or set NEODCT_ALLOW_BARE=1 if\n"
    "nd_testguard: you have read test/harness/nd_testguard.c and accept running it\n"
    "nd_testguard: on this machine.\n";

__attribute__((constructor)) static void nd_testguard_init(void)
{
    nd_svc_halt_disarm();

    if (getenv("NEODCT_TEST_HARNESS") != NULL || getenv("NEODCT_ALLOW_BARE") != NULL)
        return;

    (void)!write(2, ND_TESTGUARD_MSG, sizeof ND_TESTGUARD_MSG - 1u);
    _exit(ND_TESTGUARD_EXIT);
}
