/* nd_verify.c -- "is this file signed by us?", for the one program that has
 * to ask before the operating system exists.
 *
 *     nd-verify <data> <signature> <public-key>
 *     exit 0   the signature is a valid RSA/SHA-256 PKCS#1 v1.5 signature
 *              over <data> by <public-key>
 *     exit 1   it is not, for any reason at all
 *     exit 2   the arguments are wrong
 *
 * ============ WHY THIS EXISTS AT ALL ============
 *
 * SECURITY-AUDIT.md section 3 is the critical finding: the initramfs writes
 * a staged image to the system partition and checks no signature, then
 * records the verity root hash the same writable partition told it to. A
 * process that can write /NeoDCT/User therefore installs its own operating
 * system, permanently, and every later boot verifies cleanly against it.
 * SECURITY-PLAN.md puts the fix in Phase 0.
 *
 * The initramfs is the right place for the check because it is the only
 * component in the chain an attacker cannot rewrite: it is built into the
 * kernel image and replaced only by a reflash. But it is busybox ash with
 * no crypto in it, so the check needs a program. This is that program.
 *
 * ============ WHY IT DOES NOT LINK libneodct ============
 *
 * lib/nd_signing.c is the same verification and it is the authority on what
 * "signed" means here. This file does not call it, for two reasons that both
 * have to be true at once:
 *
 *   1. It has to link STATICALLY. Dynamic means shipping libcrypto.so.3 in
 *      the initramfs beside it, which is 5.8 MB against a static binary's
 *      4.3 MB -- and libneodct drags in freetype, sqlite, libpng and libjpeg,
 *      none of which can be statically linked into a boot-time tool without
 *      making it larger than the kernel.
 *   2. nd_signing.c opens files through nd_path_resolve(), which honours
 *      NEODCT_ROOT so the test suite can redirect /NeoDCT. An initramfs tool
 *      must open the literal path it was handed and nothing else.
 *
 * So the EVP sequence is repeated here, deliberately and in full, and
 * test/unit/test_verify.c runs BOTH implementations over the same fixtures
 * in neodct/tests/signing/ and fails if they ever disagree about a file.
 * Duplication with a test that pins the agreement beats a shared function
 * that cannot be linked.
 *
 * Everything nd_signing.h promises is promised here, for the same reasons
 * (its header explains each one against the Python it was ported from):
 *
 *   - SHA-256 only; there is no digest negotiation to get wrong;
 *   - a signature whose length is not the modulus size is refused before any
 *     arithmetic happens;
 *   - RSA keys only, so an EC key is a refusal rather than a key that loads
 *     and then verifies nothing;
 *   - every failure -- unreadable file, malformed key, bad padding, forged
 *     signature -- is the same exit code. A caller that could tell "broken"
 *     from "forged" would be tempted to treat one of them as acceptable.
 */

#include <stdio.h>
#include <stdlib.h>

#include <openssl/decoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

/* A signature is a few hundred bytes, a key a few thousand, and a manifest a
 * few kilobytes. Nothing this program should ever open is larger, so a
 * hostile file does not get to decide how much is allocated. */
#define ND_VERIFY_FILE_MAX (1u << 20) /* 1 MiB */

#define ND_VERIFY_OK      0
#define ND_VERIFY_BAD     1
#define ND_VERIFY_USAGE   2

static unsigned char *read_all(const char *path, size_t *out_len)
{
    unsigned char *buf = NULL;
    FILE *f;
    long size;

    *out_len = 0u;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        goto fail;
    size = ftell(f);
    if (size < 0 || (unsigned long)size > ND_VERIFY_FILE_MAX)
        goto fail;
    if (fseek(f, 0, SEEK_SET) != 0)
        goto fail;

    buf = malloc((size_t)size + 1u); /* +1 so a zero-length file still allocs */
    if (buf == NULL)
        goto fail;
    if (size > 0 && fread(buf, 1u, (size_t)size, f) != (size_t)size)
        goto fail;
    (void)fclose(f);
    *out_len = (size_t)size;
    return buf;

fail:
    free(buf);
    (void)fclose(f);
    return NULL;
}

/* PEM or DER, SPKI or bare PKCS#1 -- one decoder for all four shapes, and
 * "RSA" pins the key type. Mirrors nd_sign_load_public_key(). */
static EVP_PKEY *load_public_key(const char *path)
{
    unsigned char *raw;
    size_t raw_len = 0u;
    EVP_PKEY *pkey = NULL;
    OSSL_DECODER_CTX *dctx;
    const unsigned char *p;
    size_t left;

    raw = read_all(path, &raw_len);
    if (raw == NULL)
        return NULL;

    p = raw;
    left = raw_len;
    dctx = OSSL_DECODER_CTX_new_for_pkey(&pkey, NULL, NULL, "RSA",
                                         OSSL_KEYMGMT_SELECT_PUBLIC_KEY, NULL, NULL);
    if (dctx != NULL) {
        (void)OSSL_DECODER_from_data(dctx, &p, &left);
        OSSL_DECODER_CTX_free(dctx);
    }
    free(raw);
    ERR_clear_error();
    return pkey;
}

static int verify(const unsigned char *data, size_t data_len, const unsigned char *sig,
                  size_t sig_len, EVP_PKEY *pkey)
{
    EVP_MD_CTX *ctx;
    EVP_PKEY_CTX *pctx = NULL;
    int ok = 0;

    /* Before any arithmetic, exactly as signing.py does it. */
    if (sig_len == 0u || sig_len != (size_t)EVP_PKEY_size(pkey))
        return 0;

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL)
        return 0;
    if (EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) == 1 &&
        EVP_DigestVerify(ctx, sig, sig_len, data != NULL ? data : (const unsigned char *)"",
                         data_len) == 1) {
        ok = 1;
    }
    ERR_clear_error();
    EVP_MD_CTX_free(ctx);
    return ok;
}

static void usage(FILE *out)
{
    (void)fprintf(out, "usage: nd-verify <data> <signature> <public-key>\n"
                       "  exit 0  signed by that key   exit 1  not signed\n");
}

int main(int argc, char **argv)
{
    unsigned char *data = NULL;
    unsigned char *sig = NULL;
    size_t data_len = 0u;
    size_t sig_len = 0u;
    EVP_PKEY *pkey;
    int ok = 0;

    if (argc == 2 && (argv[1][0] == '-')) {
        usage(stdout);
        return ND_VERIFY_USAGE;
    }
    if (argc != 4) {
        usage(stderr);
        return ND_VERIFY_USAGE;
    }

    pkey = load_public_key(argv[3]);
    if (pkey == NULL) {
        /* A phone with no key installed must verify NOTHING, not everything.
         * Same exit code as a forgery: the caller refuses either way. */
        (void)fprintf(stderr, "nd-verify: no usable RSA public key in %s\n", argv[3]);
        return ND_VERIFY_BAD;
    }

    data = read_all(argv[1], &data_len);
    sig = read_all(argv[2], &sig_len);
    if (data != NULL && sig != NULL)
        ok = verify(data, data_len, sig, sig_len, pkey);

    free(data);
    free(sig);
    EVP_PKEY_free(pkey);
    return ok ? ND_VERIFY_OK : ND_VERIFY_BAD;
}
