/* test_verify.c -- nd-verify and nd_signing must never disagree.
 *
 * There are two RSA verifiers in this tree and there have to be. libneodct's
 * (lib/nd_signing.c) is what the Update app calls; nd-verify (tools/
 * nd_verify.c) is what the initramfs calls, and it cannot link libneodct
 * because it has to be a static binary small enough to pack into a kernel
 * image -- nd_verify.c's header sets out both reasons in full.
 *
 * Two verifiers is a hazard, and this file is what makes it safe. Every
 * fixture in neodct/tests/signing/ is put through BOTH, and the check is not
 * "each produced the expected answer" but "they produced the SAME answer".
 * A divergence in either direction is a bug:
 *
 *   nd_signing accepts, nd-verify refuses -> a genuine signed update the app
 *       installs and the initramfs then discards on reboot. Safe, and a
 *       support nightmare nobody would diagnose from the phone.
 *   nd-verify accepts, nd_signing refuses -> worse. It means the boot-time
 *       gate is looser than the one in front of the user, which is the whole
 *       thing this gate exists to be.
 *
 * The single-file, single-purpose shape of nd-verify is also asserted here,
 * because it is a security property and not a style choice: the program that
 * decides whether an image reaches dd should have nothing else in it.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_signing.h"
#include "nd_types.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, what)                                                          \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            (void)fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
            g_fail++;                                                              \
        }                                                                          \
    } while (0)

static char g_dir[ND_PATH_MAX];  /* neodct/tests/signing            */
static char g_bin[ND_PATH_MAX];  /* the nd-verify binary just built */

/* Both live relative to the test binary, which is build/<variant>/test/. */
static bool locate(void)
{
    char self[ND_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1u);
    char *slash;

    if (n <= 0)
        return false;
    self[n] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    if (nd_snprintf(g_dir, sizeof g_dir, "%s/../../../../tests/signing", self) != ND_OK)
        return false;
    return nd_snprintf(g_bin, sizeof g_bin, "%s/../bin/nd-verify", self) == ND_OK;
}

static const char *fixture(const char *name)
{
    static char path[ND_PATH_MAX];

    if (nd_snprintf(path, sizeof path, "%s/%s", g_dir, name) != ND_OK)
        return "";
    return path;
}

/* $TMPDIR if the environment has one, /tmp otherwise. Deliberately not
 * NEODCT_ROOT: that is the fake /NeoDCT a test writes settings into, and a
 * forged signature is not phone state. */
static const char *tmpdir(void)
{
    const char *dir = getenv("TMPDIR");

    return (dir != NULL && dir[0] == '/') ? dir : "/tmp";
}

/* Between fork and exec, so no stdio: reopening /dev/null onto fd 2 by hand
 * is both async-signal-safe and free of the return value glibc insists is
 * checked. A failure here only means the child is noisy. */
static void quiet(void)
{
    int null = open("/dev/null", O_WRONLY);

    if (null >= 0) {
        (void)dup2(null, STDERR_FILENO);
        if (null != STDERR_FILENO)
            (void)close(null);
    }
}

/* Run nd-verify and give back its exit status, or -1 if it could not run. */
static int run_verify(const char *data, const char *sig, const char *key)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Quiet: a refusal prints a line, and this test causes many. */
        quiet();
        (void)execl(g_bin, "nd-verify", data, sig, key, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* One case, run through both verifiers, compared against each other. */
static void agree(const char *data, const char *sig, const char *key, const char *what)
{
    bool lib = nd_sign_verify_detached(data, sig, key);
    int tool = run_verify(data, sig, key);
    char msg[256];

    if (tool == 127 || tool < 0) {
        (void)nd_snprintf(msg, sizeof msg, "nd-verify did not run for: %s", what);
        CHECK(false, msg);
        return;
    }
    (void)nd_snprintf(msg, sizeof msg, "nd_signing says %s, nd-verify says %s: %s",
                      lib ? "yes" : "no", tool == 0 ? "yes" : "no", what);
    CHECK(lib == (tool == 0), msg);
}

int main(void)
{
    if (!locate()) {
        (void)fprintf(stderr, "test_verify: cannot locate the fixtures\n");
        return 1;
    }
    if (access(g_bin, X_OK) != 0) {
        /* `make test` builds everything first, so this means the tools were
         * skipped -- say which binary, not "something is missing". */
        (void)fprintf(stderr, "test_verify: no nd-verify at %s\n", g_bin);
        return 1;
    }

    /* ---- the cases the fixtures exist for ---- */
    agree(fixture("data.bin"), fixture("data.sig"), fixture("k.pub"),
          "the real triple");
    agree(fixture("tampered.bin"), fixture("data.sig"), fixture("k.pub"),
          "one byte of the data changed");
    agree(fixture("data.bin"), fixture("data.sig"), fixture("other.pub"),
          "signed by a key that is not this one");
    agree(fixture("data.bin"), fixture("data.sig"), fixture("nope.pub"),
          "no key file at all");
    agree(fixture("data.bin"), fixture("nope.sig"), fixture("k.pub"),
          "no signature file at all");
    agree(fixture("nope.bin"), fixture("data.sig"), fixture("k.pub"),
          "no data file at all");
    /* A file that is not a key must not load as one -- signing.py's "public
     * key is not an RSA key", and the case an EC key would fall into. */
    agree(fixture("data.bin"), fixture("data.sig"), fixture("data.bin"),
          "a key file that is not a key");
    /* The signature offered as its own data: same length, wrong bytes. */
    agree(fixture("data.sig"), fixture("data.sig"), fixture("k.pub"),
          "the signature offered as the data");
    /* The key the image actually ships, which did not sign these fixtures. */
    agree(fixture("data.bin"), fixture("data.sig"),
          "../overlay/NeoDCT/System/keys/neodct-release.pub",
          "the shipped release key over somebody else's signature");

    /* ---- forgeries, in bulk ---- */
    {
        /* Flip one bit of the signature at a time and require both verifiers
         * to refuse every one. test_signing.c does this for the library; the
         * point here is that the initramfs's copy is no more forgiving. */
        unsigned char sig[1024];
        size_t sig_len;
        FILE *f = fopen(fixture("data.sig"), "rb");
        char forged[ND_PATH_MAX];
        size_t bit;
        int disagreed = 0;

        if (f == NULL) {
            CHECK(false, "the signature fixture is readable");
        } else {
            sig_len = fread(sig, 1u, sizeof sig, f);
            (void)fclose(f);
            CHECK(sig_len > 0u, "the signature fixture is not empty");
            /* Not beside the fixtures: a test that writes into the source
             * tree leaves a file behind when it is interrupted, and this one
             * is deliberately interruptible -- it forks 256 times. */
            (void)nd_snprintf(forged, sizeof forged, "%s/nd-forged-%ld.sig",
                              tmpdir(), (long)getpid());

            /* One flipped bit per byte rather than all eight: 256 forgeries
             * is enough to catch a verifier that is not looking, and each
             * one costs a fork and a 4096-bit exponentiation. */
            for (bit = 0u; bit < sig_len; bit++) {
                bool lib;
                int tool;

                sig[bit] ^= 0x01u;
                f = fopen(forged, "wb");
                if (f == NULL)
                    break;
                (void)fwrite(sig, 1u, sig_len, f);
                (void)fclose(f);
                sig[bit] ^= 0x01u;

                lib = nd_sign_verify_detached(fixture("data.bin"), forged, fixture("k.pub"));
                tool = run_verify(fixture("data.bin"), forged, fixture("k.pub"));
                if (lib || tool == 0 || lib != (tool == 0))
                    disagreed++;
            }
            (void)remove(forged);
            CHECK(disagreed == 0, "both verifiers refuse every single-bit forgery");
            (void)fprintf(stderr, "  %zu single-bit forgeries refused by both\n", sig_len);
        }
    }

    /* ---- usage, which the shell relies on to tell "no" from "broken" ---- */
    {
        pid_t pid = fork();
        int status = 0;

        if (pid == 0) {
            quiet();
            (void)execl(g_bin, "nd-verify", (char *)NULL);
            _exit(127);
        }
        (void)waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2,
              "no arguments is exit 2, which is neither yes nor no");
    }

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_verify: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_verify: %d checks passed\n", g_checks);
    return 0;
}
