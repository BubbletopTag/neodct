/* nd_inventory.c -- what is on THIS machine, written down the same way twice.
 *
 *     nd-inventory [--root DIR] [--self-check] [--raw] [section ...]
 *     exit 0   the capture is complete
 *     exit 1   the capture is incomplete: a record could not be written
 *     exit 2   the arguments are wrong
 *     exit 4   nothing was captured at all
 *
 * ============ WHY THIS IS A PROGRAM AND NOT AN ash SCRIPT ============
 *
 * The loudest argument is the ioctl and it is not the strongest one.
 *
 * THE IOCTL. FBIOGET_VSCREENINFO and FBIOGET_FSCREENINFO cannot be done from
 * busybox ash at all, and EMPIRICAL-FINDINGS 12 is why they are the most
 * valuable records in the file: the panel divergence neodctDisplay.c:423
 * documents is the PIXEL FORMAT, and after force_mode() both machines read
 * back 240x175 bpp=32 line_len=960 with red at offset 0. That is the whole
 * Stage 3 claim in eleven numbers. A shell script that shelled out to a small
 * C helper for that one record would have the C tool's deployment problem AND
 * the shell's problems, so it buys nothing.
 *
 * BYTE-STABILITY IS THE PRODUCT AND busybox IS A BAD INSTRUMENT FOR IT. The
 * prototype booted for this design printed modes as `ls -la` strings, which is
 * already wrong: a symbolic mode encodes setuid and sticky ambiguously and the
 * rendering varies with the applet config. `sort` without LC_ALL=C is
 * locale-dependent. `printf` of a 32-bit size differs between applet builds.
 * Every one of those is a READER difference that the diff would report as a
 * MACHINE difference -- and the two busyboxes are identical today only because
 * both come from the same defconfig, which is exactly the assumption this
 * whole exercise refuses to make.
 *
 * AND libneodct HOLDS ANSWERS NOTHING ELSE CAN GIVE. The resolved platform and
 * nd_platform_mismatch() (DECISIONS.md D2 -- the compiled constant is not
 * readable from a shell at all) and nd_modem__board_should_have_a_radio()
 * (D3). neodct/overlay/bin/nd-platform already documents in its own header
 * what re-deriving those costs: "it will happily answer hw on an image the C
 * has already refused to call anything." An inventory built on a reader with
 * that gap cannot see a mis-assembled image, which is the one thing D1 and D2
 * exist to make visible.
 *
 * THE SHELL CASE, ANSWERED RATHER THAN DISMISSED. "A script can be pasted into
 * a serial console on a phone whose rootfs you cannot rebuild" is true and
 * mostly irrelevant: a phone running an image from this branch onward HAS this
 * tool, shipped in the rootfs exactly as nd-selftest is. The paste case only
 * arises for a phone running an OLDER image -- and an inventory of an older
 * image is an inventory of a different machine, which the format= line in the
 * preamble refuses as a baseline, correctly.
 *
 * ============ IT NEVER FORKS, NEVER DROPS, AND HAS NO VERDICTS ============
 *
 * That is the line against nd-selftest, and it is meant to be enforced by
 * SHAPE rather than by discipline:
 *
 *     nd-selftest asks the kernel to DECIDE.
 *     nd-inventory asks the machine to DESCRIBE.
 *
 * nd-selftest forks, calls nd_priv_become(), performs one operation and
 * reports through an exit status; its output is verdicts and it exits 1 when
 * one fails. This program performs no operation whose success is the answer,
 * changes no euid, starts no child, and every line it prints is a fact. Its
 * exit status says only whether the capture is COMPLETE.
 *
 * THE RULE THAT KEEPS THE LINE SHARP AS BOTH FILES GROW, and it is in both
 * headers: nd-inventory may not contain the word FAIL, and nd-selftest may not
 * print a record. A check that wants a verdict belongs in nd-selftest; a fact
 * that wants recording belongs here.
 *
 * The rule is absolute rather than nuanced so that `grep -c FAIL` over this
 * file is the whole audit -- the first draft broke it three hundred lines
 * below the paragraph stating it. --self-check REFUSES a machine rather than
 * failing on it, and that vocabulary is the capture scripts', for the same
 * reason they use it: a refusal is not a measurement, and a recorded capture
 * that is not a measurement is worse, because it becomes the reference.
 *
 * The mounts overlap is real and deliberate. Both read /proc/mounts, and the
 * duplication is worth having because the two fail differently: nd-selftest
 * fails on THIS PHONE TONIGHT ("your user partition is missing nosuid"), this
 * fails in a PULL REQUEST NEXT WEEK ("the phone and the emulator no longer
 * mount /NeoDCT/User the same way"). A divergence that neither of them is
 * wrong about is exactly the thing nobody was catching.
 *
 * ============ TWO HASHES, BECAUSE THEY ANSWER TWO QUESTIONS ============
 *
 * The capture comes back down a serial console that turns LF into CRLF and
 * interleaves printk. So every body line carries the INV| sentinel, the body
 * is bracketed, and the trailer carries a hash over everything transmitted
 * (transport integrity) AND a hash over the compared subset alone (the
 * capture's identity). Two, because an informational line changing must not
 * invalidate a capture, while a printk chewing an informational line must
 * still be caught. This is test_update_ubi.sh's grab()-plus-sha discipline
 * reused rather than reinvented, and it is what makes the ssh capture and the
 * serial capture produce byte-identical artefacts.
 *
 * ============ --self-check TURNS THE CLAIM INTO AN ASSERTION ============
 *
 * The tool collects twice in one process and compares. Both capture scripts
 * run it first and refuse to record a capture that fails it. Byte-stability
 * therefore stops being a property this design CLAIMS and becomes one the
 * tool PROVES about every machine it ever stands on -- including the phone,
 * where nobody here can run the experiment.
 *
 * ============ WHAT platform.compiled IS NOT, AND WHY ============
 *
 * The design asked for a `platform.compiled` record holding the compiled-in
 * constant on its own. There is no way to read it and that is deliberate:
 * nd_platform.h refuses to publish an nd_platform_build(), arguing that "a
 * caller reading the build value directly is a caller bypassing the
 * mismatch". Adding one for this tool would be adding the bypass that header
 * exists to prevent, in the one program whose whole job is to notice that a
 * bypass happened. So the library half is recorded as what the library
 * actually answers -- platform.resolved, platform.board, platform.mismatch --
 * and the record half comes from /NeoDCT/platform. A build and a record that
 * disagree are already visible as platform.mismatch=true with
 * platform.resolved=unknown, which is the same fact in the shape the library
 * chose to publish it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_inventory.h"

#ifndef ND_INVENTORY_NO_LIBNEODCT
#include "nd_platform.h"
/* nd_modem__board_should_have_a_radio() is nd_modem's private predicate, the
 * one five sites already share, and it is reached the way nd_selftest.c
 * reaches nd_notify_priv.h: through the library's own private header rather
 * than through a copy. A copy would be a sixth site, and D3's whole point is
 * that there is one. */
#include "../lib/nd_modem_priv.h"
#endif

/* ------------------------------------------------------------------ *
 * The library half
 *
 * ND_INVENTORY_NO_LIBNEODCT exists for ONE caller: the cross-compiled probe
 * capture, which boots the repo's own armv7 kernel in a busybox initramfs
 * because there is no Buildroot image yet and a full build is hours. libneodct
 * cannot be cross-compiled without freetype, sqlite, libpng and libjpeg for
 * the target, so that build carries these six records as UNAVAILABLE(nolib)
 * rather than omitting them -- a hole you can see, never a shorter file -- and
 * stamps capture.method=nd-inventory-nolib in the preamble so parity_diff.py
 * refuses it as a gating baseline for the same reason it refuses the shell
 * fallback. A WEAKER INSTRUMENT MUST NEVER BE ABLE TO BECOME THE REFERENCE.
 * ------------------------------------------------------------------ */

static const char *capture_method(void)
{
#ifdef ND_INVENTORY_NO_LIBNEODCT
    return "nd-inventory-nolib";
#else
    return "nd-inventory";
#endif
}

/* Gated by section like every other collector. `platform.*` belongs to `os`,
 * beside the /NeoDCT/platform record it is the other half of, and `modem.*` to
 * `modem`, beside the candidate ports its verdict turns on. Running them
 * unconditionally would make `nd-inventory memory` print a platform record,
 * and a --sections capture that quietly contains more than it was asked for is
 * a capture nobody can reason about. */
static void collect_library(inv_out *o, const char *const *sections)
{
    bool want_platform = inv_section_wanted(sections, "os");
    bool want_modem = inv_section_wanted(sections, "modem");

#ifdef ND_INVENTORY_NO_LIBNEODCT
    if (want_platform) {
        (void)inv_add(o, INV_CMP, "platform.resolved", "UNAVAILABLE(nolib)");
        (void)inv_add(o, INV_CMP, "platform.board", "UNAVAILABLE(nolib)");
        (void)inv_add(o, INV_CMP, "platform.mismatch", "UNAVAILABLE(nolib)");
        (void)inv_add(o, INV_RAW, "platform.origin", "UNAVAILABLE(nolib)");
    }
    if (want_modem) {
        (void)inv_add(o, INV_CMP, "modem.board_expects_radio", "UNAVAILABLE(nolib)");
        (void)inv_add(o, INV_CMP, "modem.cold_verdict", "UNAVAILABLE(nolib)");
    }
#else
    if (want_platform) {
        (void)inv_add(o, INV_CMP, "platform.resolved", "%s", nd_platform_name());
        (void)inv_add(o, INV_CMP, "platform.board", "%s",
                      nd_platform_board()[0] != '\0' ? nd_platform_board() : "ABSENT");
        (void)inv_add(o, INV_CMP, "platform.mismatch", "%s",
                      nd_platform_mismatch() ? "true" : "false");
        /* Informational: it is a SENTENCE, and a sentence reworded by a future
         * edit is a diff that is not a machine difference. The three records
         * above are the facts; this is the working shown. */
        (void)inv_add(o, INV_RAW, "platform.origin", "%s", nd_platform_origin());
    }
    if (!want_modem)
        return;

    /* Asked once, here, and never twice: nd_modem__board_should_have_a_radio()
     * is nd_platform() behind a predicate, and the two records below have to
     * be two views of ONE reading or a capture could contain a verdict that
     * disagrees with the input it was derived from. */
    {
        bool radio = nd_modem__board_should_have_a_radio();

        (void)inv_add(o, INV_CMP, "modem.board_expects_radio", "%s", radio ? "true" : "false");
        /* The acceptance test for cc651992 in one line: what nd_modem_link_state()
     * returns for a modem with nothing adopted. It MIRRORS nd_modem.c's final
     * return rather than calling it, because calling it needs a live nd_modem
     * -- a thread and a port open -- and this program opens nothing under
     * /dev but /dev/fb0. The predicate it turns on is recorded on the line
     * above, so if the two ever stop agreeing, the file shows both halves and
     * a reader can see which one moved.
     *
     * The LIVE state is refused outright: it is PROBING for the boot grace
     * and then LIVE or ABSENT, so recording it would be recording whether the
     * SIM had registered by the time somebody ran the tool. */
        (void)inv_add(o, INV_CMP, "modem.cold_verdict", "%s", radio ? "ABSENT" : "SIM");
    }
#endif
}

/* ------------------------------------------------------------------ *
 * CLI
 * ------------------------------------------------------------------ */

static void usage(void)
{
    size_t i;

    (void)fprintf(stderr, "nd-inventory [--root DIR] [--self-check] [--raw] [section ...]\n"
                          "\n"
                          "  Writes down what is on this machine, byte-identically twice, so\n"
                          "  that a capture from the phone and a capture from the emulator can\n"
                          "  be diffed in a pull request. It decides nothing and it has no\n"
                          "  verdicts: every line is a fact. nd-selftest is the tool that\n"
                          "  decides -- see the header of either file for the line between\n"
                          "  them.\n"
                          "\n"
                          "  It opens /dev/fb0 O_RDONLY for two GET ioctls and opens nothing\n"
                          "  else under /dev, which is what makes it safe to run on a phone\n"
                          "  somebody is holding.\n"
                          "\n"
                          "  sections:");
    for (i = 0u; INV_SECTIONS[i] != NULL; i++)
        (void)fprintf(stderr, " %s", INV_SECTIONS[i]);
    (void)fprintf(stderr, "\n"
                          "  --root DIR    reprefix /proc /sys /dev /etc /NeoDCT. For the unit\n"
                          "                tests. Stamped into the preamble, and parity_diff.py\n"
                          "                refuses any capture whose root is not / as a baseline\n"
                          "  --self-check  collect twice and compare; exit 1 if they differ\n"
                          "  --raw         the records with no sentinel, preamble or hashes.\n"
                          "                For reading. It is not a capture and cannot be\n"
                          "                parsed as one\n"
                          "\n"
                          "  exit 0        the capture is complete\n"
                          "  exit 1        the capture is incomplete: a record did not fit\n"
                          "  exit 2        bad arguments\n"
                          "  exit 4        nothing was captured at all\n");
}

static int known_section(const char *name)
{
    size_t i;

    for (i = 0u; INV_SECTIONS[i] != NULL; i++) {
        if (strcmp(INV_SECTIONS[i], name) == 0)
            return 1;
    }
    return 0;
}

/* ONE TRUNCATION POLICY FOR THE ONE BUFFER main() BUILDS, and it is here
 * because the two sites that had their own were the two that were wrong. They
 * did `used += (written > 0) ? written : 0`, and snprintf returns the length
 * it WOULD have written -- so a --root long enough to fill the buffer walked
 * `used` past the end of it, and the next call got a `sizeof preamble - used`
 * that had underflowed to about SIZE_MAX. Measured: a --root of 873, 874 or
 * 875 characters writes "INV|BEGIN\n" off the end of a 1024-byte stack array,
 * which ASAN reports and which glibc's FORTIFY aborts on. The phone's musl
 * build has neither, so there it is a silent smash of the frame holding
 * `body`, `compared` and `whole`.
 *
 * str_fmt() in nd_inventory_collect.c already encodes exactly this rule for
 * every collector. This is the same rule for the one buffer on the other side
 * of the libneodct line, rather than a second policy that has to be got right
 * at each of four call sites. */
static bool pre_add(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int written;

    va_start(ap, fmt);
    written = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= cap - *used)
        return false;
    *used += (size_t)written;
    return true;
}

static inv_out *collect_once(const inv_cfg *cfg, const char *const *sections)
{
    inv_out *o = inv_out_new();

    if (o == NULL)
        return NULL;
    inv_collect(o, cfg, sections);
    collect_library(o, sections);
    inv_sort(o);
    return o;
}

int main(int argc, char **argv)
{
    const char *sections[32];
    size_t n_sections = 0u;
    inv_cfg cfg = {"/"};
    bool self_check = false;
    bool raw = false;
    inv_out *o;
    char *body;
    char *compared;
    char version[64];
    char preamble[1024];
    char hex_transport[65];
    char hex_compared[65];
    char *whole;
    size_t whole_len;
    int i;
    int rc = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            cfg.root = argv[++i];
        } else if (strcmp(argv[i], "--self-check") == 0) {
            self_check = true;
        } else if (strcmp(argv[i], "--raw") == 0) {
            raw = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 2;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!known_section(argv[i])) {
            (void)fprintf(stderr, "nd-inventory: no section called '%s'\n", argv[i]);
            usage();
            return 2;
        } else if (n_sections < 31u) {
            sections[n_sections++] = argv[i];
        }
    }
    sections[n_sections] = NULL;

    o = collect_once(&cfg, (n_sections > 0u) ? sections : NULL);
    if (o == NULL) {
        (void)fprintf(stderr, "nd-inventory: out of memory\n");
        return 4;
    }
    if (inv_count(o) == 0u) {
        (void)fprintf(stderr, "nd-inventory: nothing was captured\n");
        inv_out_free(o);
        return 4;
    }

    body = inv_render(o);
    compared = inv_render_compared(o);
    if (body == NULL || compared == NULL) {
        free(body);
        free(compared);
        inv_out_free(o);
        (void)fprintf(stderr, "nd-inventory: out of memory\n");
        return 4;
    }

    /* ============ COLLECT TWICE AND COMPARE ============
     *
     * Before anything is printed, so a capture that cannot be reproduced
     * within one process is never recorded. It catches the whole class of
     * readdir-order and unsorted-list bugs at the moment they are
     * introduced, on whatever machine introduced them. What it CANNOT catch
     * is a device that arrives a second after the capture ends -- an
     * attribute that appears late reads as ABSENT and would be committed as
     * such -- and that is a real gap, written down rather than papered over.
     */
    if (self_check) {
        inv_out *again = collect_once(&cfg, (n_sections > 0u) ? sections : NULL);
        char *body2 = (again != NULL) ? inv_render(again) : NULL;
        bool same = (body2 != NULL) && strcmp(body, body2) == 0;

        free(body2);
        inv_out_free(again);
        if (!same) {
            (void)fprintf(stderr,
                          "nd-inventory: --self-check REFUSES this machine -- two "
                          "collections in one process differ, so nothing captured here "
                          "can be a baseline\n");
            free(body);
            free(compared);
            inv_out_free(o);
            return 1;
        }
        (void)fprintf(stderr, "nd-inventory: --self-check ok (%zu records, twice)\n", inv_count(o));
    }

    if (raw) {
        /* No sentinel, no preamble, no hashes: this cannot be parsed as a
         * capture, which is the point. A "readable" variant that could be
         * mistaken for a capture would eventually be committed as one. */
        const char *p = body;

        while (*p != '\0') {
            const char *nl = strchr(p, '\n');
            size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

            if (len > 4u && strncmp(p, "INV|", 4u) == 0)
                (void)printf("%.*s\n", (int)(len - 4u), p + 4);
            if (nl == NULL)
                break;
            p = nl + 1;
        }
        free(body);
        free(compared);
        inv_out_free(o);
        return 0;
    }

    if (!inv_os_version_id(&cfg, version, sizeof version))
        (void)snprintf(version, sizeof version, "ABSENT");

    /* The preamble is read and never diffed. capture.root and
     * capture.sections are here because both are ways to produce a capture
     * that looks complete and is not, and the comparator refuses a baseline
     * carrying either. capture.euid is here so a capture taken as the wrong
     * user is visible rather than inferred from a file full of
     * UNREADABLE(EACCES). There is NO TIMESTAMP anywhere in this file: the
     * inventory reads no clock. */
    {
        size_t used = 0u;
        size_t s;
        bool ok;

        ok = pre_add(preamble, sizeof preamble, &used,
                     "INV|# format=%d\n"
                     "INV|# capture.method=%s\n"
                     "INV|# capture.root=%s\n"
                     "INV|# capture.euid=%lu\n"
                     "INV|# capture.os_version_id=%s\n"
                     "INV|# capture.sections=",
                     ND_INV_FORMAT, capture_method(), cfg.root, (unsigned long)geteuid(), version);
        if (ok) {
            if (n_sections == 0u) {
                ok = pre_add(preamble, sizeof preamble, &used, "all\n");
            } else {
                for (s = 0u; ok && s < n_sections; s++)
                    ok = pre_add(preamble, sizeof preamble, &used, "%s%s", s > 0u ? "," : "",
                                 sections[s]);
                if (ok)
                    ok = pre_add(preamble, sizeof preamble, &used, "\n");
            }
        }
        if (ok)
            ok = pre_add(preamble, sizeof preamble, &used, "INV|BEGIN\n");
        if (!ok) {
            (void)fprintf(stderr, "nd-inventory: the preamble does not fit -- --root is "
                                  "%zu bytes and nothing here may be truncated\n",
                          strlen(cfg.root));
            free(body);
            free(compared);
            inv_out_free(o);
            return 4;
        }
    }

    whole_len = strlen(preamble) + strlen(body) + strlen("INV|END\n");
    /* owned here; freed before return */
    whole = malloc(whole_len + 1u);
    if (whole == NULL) {
        free(body);
        free(compared);
        inv_out_free(o);
        return 4;
    }
    (void)snprintf(whole, whole_len + 1u, "%s%sINV|END\n", preamble, body);

    inv_sha256_hex(whole, whole_len, hex_transport);
    inv_sha256_hex(compared, strlen(compared), hex_compared);

    (void)fputs(whole, stdout);
    (void)printf("INV|# sha256.transport=%s\n", hex_transport);
    (void)printf("INV|# sha256.compared=%s\n", hex_compared);

    if (inv_dropped(o) > 0u) {
        (void)fprintf(stderr,
                      "nd-inventory: %zu record(s) could not be written -- this capture "
                      "is INCOMPLETE and must not be committed as a baseline\n",
                      inv_dropped(o));
        rc = 1;
    }

    free(whole);
    free(body);
    free(compared);
    inv_out_free(o);
    return rc;
}
