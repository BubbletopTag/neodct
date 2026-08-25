/* nd_signing.h -- "is this update signed by us?", and nothing else.
 *
 * The phone only ever VERIFIES. It never signs. Releases are signed on a
 * workstation with
 *
 *     openssl dgst -sha256 -sign neodct-release.key -out manifest.sig manifest.json
 *
 * and the public half ships at /NeoDCT/System/keys/neodct-release.pub as a
 * 4096-bit RSA key in PEM SPKI form.
 *
 * ============ WHY OPENSSL AND NOT THE PYTHON'S OWN ARITHMETIC ============
 *
 * System/core/UpdateService/signing.py implements RSA PKCS#1 v1.5 from
 * scratch -- s^e mod n plus a byte comparison -- and its docstring explains
 * the one thing that matters:
 *
 *     Deliberately strict: the expected encoded message is rebuilt in full
 *     and compared whole. Verifiers that instead go looking for a
 *     DigestInfo inside the decrypted block are the ones that fall to
 *     Bleichenbacher'06 forgeries.
 *
 * That is exactly right, and it is exactly why this file does NOT rewrite
 * it in C. Python has arbitrary-precision integers; C does not, so a
 * faithful port means hand-written modular exponentiation over a 4096-bit
 * modulus -- new, unreviewed, security-critical arithmetic guarding the
 * step that hands an image to dd. Three separate agents were asked to build
 * this system and all three refused to hand-roll this function. They were
 * right.
 *
 * libcrypto is already in the image (OpenSSH pulls it in, so it costs no
 * space) and EVP_DigestVerify with RSA_PKCS1_PADDING performs precisely the
 * full-encoded-message reconstruction signing.py hand-built, in code that
 * has been read by more people than this project will ever have.
 *
 * The behaviour that must match, and is asserted in test_signing.c against
 * signatures the real Python accepts and rejects:
 *
 *   - only SHA-256; any other digest is a refusal, not a fallback;
 *   - a signature whose length is not the modulus size is refused before
 *     any maths happens;
 *   - an integer >= the modulus is refused;
 *   - a padding run shorter than RFC 8017's eight octets is refused;
 *   - the comparison is over the WHOLE encoded block.
 */

#ifndef ND_SIGNING_H_INCLUDED
#define ND_SIGNING_H_INCLUDED

#include <stddef.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque public key. NULL is always "no key", never "any key". */
typedef struct nd_pubkey nd_pubkey;

/* Read a public key. PEM or DER, SPKI or bare PKCS#1 -- the four shapes
 * load_public_key() accepts. NULL on any failure, including a missing file:
 * a phone with no key installed must verify nothing, not everything. */
nd_pubkey *nd_sign_load_public_key(const char *path);
void nd_sign_free_public_key(nd_pubkey *key);

/* True ONLY if `sig` is a valid SHA-256 PKCS#1 v1.5 signature over `data`
 * by `key`. Every failure -- bad key, wrong size, malformed padding, an
 * unreadable file -- is false. There is no error channel on purpose: a
 * caller that could tell "broken" from "forged" would be tempted to treat
 * one of them as acceptable. */
bool nd_sign_verify(const void *data, size_t len, const void *sig, size_t sig_len,
                    const nd_pubkey *key);

/* verify_detached(): a file, its .sig, and the key, by path. False if any
 * of the three cannot be read. */
bool nd_sign_verify_detached(const char *path, const char *sig_path, const char *key_path);

#ifdef __cplusplus
}
#endif

#endif /* ND_SIGNING_H_INCLUDED */
