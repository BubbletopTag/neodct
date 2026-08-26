/* nd_signing.c -- see nd_signing.h. */

#include "nd_signing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/core_names.h>
#include <openssl/decoder.h>
#include <openssl/rsa.h>

#include "nd_log.h"
#include "nd_paths.h"

struct nd_pubkey {
    EVP_PKEY *pkey;
};

/* Read a whole file. Bounded: a signature is a few hundred bytes and a key
 * a few thousand, so anything larger is not one of them and there is no
 * reason to let a hostile file decide how much we allocate. */
#define ND_SIGN_FILE_MAX (1u << 20) /* 1 MiB */

static unsigned char *read_all(const char *path, size_t *out_len)
{
    char resolved[ND_PATH_MAX];
    unsigned char *buf = NULL;
    FILE *f = NULL;
    long size;

    *out_len = 0u;
    if (path == NULL || nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return NULL;
    f = fopen(resolved, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        goto fail;
    size = ftell(f);
    if (size < 0 || (unsigned long)size > ND_SIGN_FILE_MAX)
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
    if (f != NULL)
        (void)fclose(f);
    return NULL;
}

nd_pubkey *nd_sign_load_public_key(const char *path)
{
    nd_pubkey *key;
    unsigned char *raw;
    size_t raw_len = 0u;
    EVP_PKEY *pkey = NULL;
    OSSL_DECODER_CTX *dctx;
    const unsigned char *p;
    size_t left;

    raw = read_all(path, &raw_len);
    if (raw == NULL)
        return NULL;

    /* One decoder, all four shapes.
     *
     * load_public_key() accepts PEM or DER, SPKI or bare PKCS#1. The
     * obvious C translation is four calls -- PEM_read_bio_PUBKEY,
     * PEM_read_bio_RSAPublicKey, d2i_PUBKEY, d2i_RSAPublicKey -- but half
     * of those are the low-level RSA API that OpenSSL 3.0 deprecated, and
     * this project builds with -Werror=deprecated-declarations. That is the
     * toolchain being right: OSSL_DECODER does the same job, sniffs the
     * input format itself, and is the interface that will still exist in
     * OpenSSL 4.
     *
     * NULL format and NULL structure mean "work it out", which covers the
     * PEM/DER and SPKI/PKCS#1 axes in one pass. "RSA" pins the key type, so
     * an EC key is refused here rather than loading and then verifying
     * nothing -- signing.py's "public key is not an RSA key". */
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

    if (pkey == NULL)
        return NULL;

    key = calloc(1u, sizeof *key);
    if (key == NULL) {
        EVP_PKEY_free(pkey);
        return NULL;
    }
    key->pkey = pkey;
    return key;
}

void nd_sign_free_public_key(nd_pubkey *key)
{
    if (key == NULL)
        return;
    EVP_PKEY_free(key->pkey);
    free(key);
}

bool nd_sign_verify(const void *data, size_t len, const void *sig, size_t sig_len,
                    const nd_pubkey *key)
{
    EVP_MD_CTX *ctx;
    EVP_PKEY_CTX *pctx = NULL;
    bool ok = false;

    if (key == NULL || key->pkey == NULL || sig == NULL || sig_len == 0u)
        return false;
    if (data == NULL && len != 0u)
        return false;

    /* "if not signature or len(signature) != key.size: return False" --
     * refused before any arithmetic. EVP would reject it too, but doing it
     * here keeps the rule visible next to the Python it comes from. */
    if (sig_len != (size_t)EVP_PKEY_size(key->pkey))
        return false;

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL)
        return false;

    if (EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), NULL, key->pkey) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) == 1 &&
        EVP_DigestVerify(ctx, sig, sig_len, data != NULL ? data : (const unsigned char *)"",
                         len) == 1) {
        ok = true;
    }

    /* Whatever happened, leave no error behind: the next caller's failure
     * must be its own. */
    ERR_clear_error();
    EVP_MD_CTX_free(ctx);
    return ok;
}

bool nd_sign_verify_detached(const char *path, const char *sig_path, const char *key_path)
{
    nd_pubkey *key;
    unsigned char *data = NULL;
    unsigned char *sig = NULL;
    size_t data_len = 0u;
    size_t sig_len = 0u;
    bool ok = false;

    key = nd_sign_load_public_key(key_path);
    if (key == NULL)
        return false;

    data = read_all(path, &data_len);
    sig = read_all(sig_path, &sig_len);
    if (data != NULL && sig != NULL)
        ok = nd_sign_verify(data, data_len, sig, sig_len, key);

    free(data);
    free(sig);
    nd_sign_free_public_key(key);
    return ok;
}
