/* test_signing.c -- the C verifier must agree with the Python verifier.
 *
 * Not "looks right to me". Every expectation below was established by
 * running System/core/UpdateService/signing.py on the same bytes, and the
 * fixtures carry that record in neodct/tests/signing/README.md. A verifier
 * only its author has ever agreed with is worth nothing, and this one
 * decides whether an image reaches dd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static char g_dir[ND_PATH_MAX];

/* The fixtures sit beside the source tree, not under a phone root, so this
 * walks up from the test binary the way the browser-argv test does. */
static bool fixtures(void)
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
    /* build/<variant>/test -> ../../../../tests/signing */
    if (nd_snprintf(g_dir, sizeof g_dir, "%s/../../../../tests/signing", self) != ND_OK)
        return false;
    return true;
}

static unsigned char *slurp(const char *name, size_t *len)
{
    char path[ND_PATH_MAX];
    unsigned char *buf;
    long size;
    FILE *f;

    *len = 0u;
    if (nd_snprintf(path, sizeof path, "%s/%s", g_dir, name) != ND_OK)
        return NULL;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    (void)fseek(f, 0, SEEK_END);
    size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        (void)fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size);
    if (buf != NULL && fread(buf, 1u, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    (void)fclose(f);
    if (buf != NULL)
        *len = (size_t)size;
    return buf;
}

/* Three rotating buffers, NOT one static.
 *
 * verify_detached() takes three paths and every call site passes three
 * fixture_path() results in one argument list. With a single static buffer
 * all three arguments alias it and hold whichever was evaluated last -- so
 * the test asked "does data.bin verify against data.bin with data.bin as
 * the key", got false, and looked like a verifier bug. It was a test bug.
 * The C is unchanged; only this is. */
static char *fixture_path(const char *name)
{
    static char slots[3][ND_PATH_MAX];
    static unsigned next;
    char *path = slots[next % 3u];

    next++;
    if (nd_snprintf(path, ND_PATH_MAX, "%s/%s", g_dir, name) != ND_OK)
        return NULL;
    return path;
}

int main(void)
{
    nd_pubkey *key;
    nd_pubkey *other;
    unsigned char *data;
    unsigned char *tampered;
    unsigned char *sig;
    size_t data_len = 0u;
    size_t tampered_len = 0u;
    size_t sig_len = 0u;

    /* These fixtures are HOST build artefacts, not phone paths.
     *
     * nd_sign_load_public_key() resolves through the path layer, which is
     * right in production -- the key really is /NeoDCT/System/keys/... and
     * must follow ND_ROOT. But `make test` points ND_ROOT at a scratch
     * directory, so an absolute host path gets that prefix bolted on and
     * the fixture vanishes. Running the binary by hand passed; running it
     * through make failed four checks, which is the same ND_ROOT trap
     * test_mediawidget.c hit earlier today.
     *
     * Clearing the root for this test's duration is the honest fix: it is
     * testing a verifier against files on this machine, not a phone. */
    (void)nd_path_set_root(NULL);

    if (!fixtures()) {
        (void)fprintf(stderr, "SKIP signing: cannot locate fixtures\n");
        return 0;
    }
    data = slurp("data.bin", &data_len);
    tampered = slurp("tampered.bin", &tampered_len);
    sig = slurp("data.sig", &sig_len);
    if (data == NULL || tampered == NULL || sig == NULL) {
        (void)fprintf(stderr, "SKIP signing: fixtures missing from %s\n", g_dir);
        free(data);
        free(tampered);
        free(sig);
        return 0;
    }

    key = nd_sign_load_public_key(fixture_path("k.pub"));
    other = nd_sign_load_public_key(fixture_path("other.pub"));
    CHECK(key != NULL, "the 2048-bit PEM SPKI test key loads");
    CHECK(other != NULL, "the second key loads");

    /* ---- the five verdicts, each one the Python's ---- */
    CHECK(nd_sign_verify(data, data_len, sig, sig_len, key), "a valid signature verifies");
    CHECK(!nd_sign_verify(tampered, tampered_len, sig, sig_len, key),
          "one changed character breaks it");
    CHECK(!nd_sign_verify(data, data_len, sig, sig_len, other), "the wrong key does not verify");
    CHECK(!nd_sign_verify(data, data_len, sig, sig_len - 1u, key),
          "a signature one byte short is refused");
    CHECK(!nd_sign_verify(data, data_len, sig, 0u, key), "an empty signature is refused");

    /* ---- shapes the caller can get wrong ---- */
    CHECK(!nd_sign_verify(data, data_len, sig, sig_len, NULL), "a NULL key verifies nothing");
    CHECK(!nd_sign_verify(NULL, 5u, sig, sig_len, key), "NULL data with a length is refused");
    CHECK(!nd_sign_verify(data, data_len, NULL, sig_len, key), "a NULL signature is refused");

    /* A signature one byte LONG is not the modulus size either. The Python
     * compares against key.size exactly; so does this. */
    {
        unsigned char *big = malloc(sig_len + 1u);

        if (big != NULL) {
            memcpy(big, sig, sig_len);
            big[sig_len] = 0u;
            CHECK(!nd_sign_verify(data, data_len, big, sig_len + 1u, key),
                  "a signature one byte long is refused");
            free(big);
        }
    }

    /* Every byte of the signature matters: flipping one bit anywhere in it
     * must break verification. Bleichenbacher forgeries live in the bytes a
     * lazy verifier never looks at, so this walks the whole block. */
    {
        size_t i;
        size_t accepted = 0u;

        for (i = 0u; i < sig_len; i++) {
            unsigned char *bad = malloc(sig_len);

            if (bad == NULL)
                break;
            memcpy(bad, sig, sig_len);
            bad[i] ^= 0x01u;
            if (nd_sign_verify(data, data_len, bad, sig_len, key))
                accepted++;
            free(bad);
        }
        g_checks++;
        if (accepted != 0u) {
            (void)fprintf(stderr, "FAIL %s:%d  %zu of %zu single-bit forgeries ACCEPTED\n",
                          __FILE__, __LINE__, accepted, sig_len);
            g_fail++;
        } else {
            (void)fprintf(stderr, "  all %zu single-bit signature forgeries refused\n", sig_len);
        }
    }

    /* ---- detached, by path ---- */
    CHECK(nd_sign_verify_detached(fixture_path("data.bin"), fixture_path("data.sig"),
                                  fixture_path("k.pub")),
          "verify_detached accepts the real triple");
    CHECK(!nd_sign_verify_detached(fixture_path("tampered.bin"), fixture_path("data.sig"),
                                   fixture_path("k.pub")),
          "verify_detached rejects tampered data");
    CHECK(!nd_sign_verify_detached(fixture_path("data.bin"), fixture_path("data.sig"),
                                   fixture_path("nope.pub")),
          "a missing key file verifies nothing");
    CHECK(!nd_sign_verify_detached(fixture_path("data.bin"), fixture_path("nope.sig"),
                                   fixture_path("k.pub")),
          "a missing signature file verifies nothing");

    /* ---- the key the phone actually ships ---- */
    {
        nd_pubkey *release =
            nd_sign_load_public_key("../overlay/NeoDCT/System/keys/neodct-release.pub");

        CHECK(release != NULL, "the shipped 4096-bit release key loads");
        CHECK(!nd_sign_verify(data, data_len, sig, sig_len, release),
              "and does not verify a signature made by a different key");
        nd_sign_free_public_key(release);
    }

    CHECK(nd_sign_load_public_key(NULL) == NULL, "a NULL path is not a key");
    CHECK(nd_sign_load_public_key(fixture_path("data.bin")) == NULL,
          "a file that is not a key is not a key");
    nd_sign_free_public_key(NULL); /* must not crash */

    nd_sign_free_public_key(key);
    nd_sign_free_public_key(other);
    free(data);
    free(tampered);
    free(sig);

    if (g_fail != 0) {
        (void)fprintf(stderr, "test_signing: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    (void)fprintf(stderr, "test_signing: %d checks passed\n", g_checks);
    return 0;
}
