/* nd_manifest.h -- manifest.json: what an update claims to be, and whether
 * this phone will take it.
 *
 * A one-to-one port of System/core/UpdateService/manifest.py (114 lines). The
 * schema is nd_update.h's ND_UPD_KEY_* macros and nothing else:
 *
 *     REQUIRED        = ("version", "buildtime", "platform", "sha256",
 *                        "verity")
 *     REQUIRED_VERITY = ("root_hash", "block_size", "image_blocks")
 *
 * plus the optional changelog, min_kernel, thumbnail_sha256 and verity.salt.
 *
 * ============ WHY THE VALIDATION ORDER IS PART OF THE CONTRACT ============
 *
 * Every refusal below is a string a person reads on the phone, and
 * neodct/tests/test_update_manifest.py matches on the field name inside it.
 * The checks therefore run in manifest.py's order and produce manifest.py's
 * wording, so that a manifest broken in two ways names the same field the
 * Python would have named. Reordering them is a user-visible change.
 *
 * ============ THE HEX RULES ARE PYTHON'S, INCLUDING THE AWKWARD ONES ======
 *
 * manifest.py validates every hash with bytes.fromhex(), and that function
 * has behaviour a hand-written checker does not guess:
 *
 *   - ASCII whitespace (space \t \n \v \f \r) is skipped BETWEEN byte pairs
 *     but not INSIDE one, so "aa bb" is two bytes and "a b" is an error;
 *   - uppercase is accepted;
 *   - an odd number of digits is an error.
 *
 * And the value stored is THE ORIGINAL STRING, unmodified -- uppercase stays
 * uppercase, spaces stay spaces. A later comparison against a computed digest
 * (which is lowercase and unspaced) is a byte comparison and will therefore
 * fail. That is reproduced deliberately: normalising here would make a
 * manifest that the signer never produced start matching an image.
 *
 * ============ WHERE THIS DELIBERATELY DIVERGES FROM THE PYTHON ============
 *
 * Python has arbitrary-precision integers, unbounded strings and no notion of
 * a hostile input. C has none of those luxuries and this file reads an
 * attacker-supplied SD card. Every divergence is in the direction of refusing
 * a manifest Python would have accepted, never the reverse, and each is
 * marked "DIVERGENCE" at its check in nd_manifest.c. In summary:
 *
 *   1. Every string field has a length cap and a manifest that exceeds it is
 *      refused rather than truncated. A truncated version string would be
 *      written into pending.prop and a truncated salt would build the wrong
 *      dm-verity table.
 *   2. A JSON integer outside int64 is refused by nd_json before this file
 *      sees it, so "buildtime": 10**30 is "not valid JSON" here and a valid
 *      timestamp in Python.
 *   3. "image_blocks": true is 1 in Python (bool is a subclass of int) and is
 *      refused here. nd_json.h keeps ND_JSON_BOOL distinct precisely so this
 *      is possible.
 *   4. changelog and min_kernel that are present, truthy and NOT strings are
 *      refused. Python str()s them later, and for min_kernel that difference
 *      decides whether an update is installed.
 */

#ifndef ND_MANIFEST_H_INCLUDED
#define ND_MANIFEST_H_INCLUDED

#include "nd_types.h"
#include "nd_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Bounds
 * ------------------------------------------------------------------ *
 *
 * These match apps/Update/update_app.h's nd_upd_manifest field by field, so
 * that copying one into the other is a plain nd_strlcpy that cannot truncate.
 * Changing one without the other reintroduces exactly the truncation this is
 * here to prevent.
 */
#define ND_MANIFEST_VERSION_MAX  64
#define ND_MANIFEST_PLATFORM_MAX 64
#define ND_MANIFEST_HEX_MAX      65  /* 64 hex digits of sha256, plus NUL     */
#define ND_MANIFEST_SALT_MAX     513 /* verity.py caps the salt at 256 bytes  */

/* Long enough for the longest composed refusal:
 * "update is for <63>, this is <63>" plus its fixed text. */
#define ND_MANIFEST_WHY_MAX 192

/* ------------------------------------------------------------------ *
 * Kernel versions
 * ------------------------------------------------------------------ *
 *
 * manifest.py's _version_tuple(): split on '.', take the leading run of
 * ASCII digits from each chunk, and STOP ENTIRELY at the first chunk that
 * has none -- do not skip it. So "6.12.47-rt" is (6, 12, 47), "6.x.3" is
 * (6,) and "" is (). Comparison is Python tuple ordering: element by
 * element, first difference decides, and a prefix is smaller than what it is
 * a prefix of, so (6,12) < (6,12,47).
 */
#define ND_KVER_MAX_PARTS 8

typedef struct {
    uint32_t part[ND_KVER_MAX_PARTS];
    uint8_t n;
} nd_kver;

/* Components beyond the eighth are ignored and a component larger than
 * UINT32_MAX saturates there -- Python would keep both. No kernel has ever
 * had either, and a hostile manifest must not be able to overflow anything.
 * `text` may be NULL, which parses as the empty tuple. */
void nd_kver_parse(const char *text, nd_kver *out);

/* -1, 0 or +1, with Python tuple ordering. */
int nd_kver_cmp(const nd_kver *a, const nd_kver *b);

/* ------------------------------------------------------------------ *
 * The manifest
 * ------------------------------------------------------------------ */

typedef struct {
    char version[ND_MANIFEST_VERSION_MAX];
    int64_t buildtime; /* a JSON INTEGER; see nd_update.h            */
    char platform[ND_MANIFEST_PLATFORM_MAX];
    char sha256[ND_MANIFEST_HEX_MAX]; /* verbatim, case preserved    */

    /* body.get(k) or "" -- so JSON null, false, 0 and "" all collapse to the
     * empty string. Never NULL. `changelog` is heap-allocated because a
     * release note is the one field with no natural bound. */
    char *changelog;
    char min_kernel[ND_MANIFEST_VERSION_MAX];
    char thumbnail_sha256[ND_MANIFEST_HEX_MAX];

    /* manifest["verity"], which staging.py copies out field by field. */
    char verity_root_hash[ND_MANIFEST_HEX_MAX];
    int64_t verity_block_size;
    int64_t verity_image_blocks;
    char verity_salt[ND_MANIFEST_SALT_MAX]; /* "" when the package had none */

    /* THE EXACT BYTES THE SIGNATURE COVERS. Owned. package.py verifies over
     * "the manifest bytes exactly as stored, never a re-encoding of the
     * parsed object", and re-serialising a parsed manifest is how a verifier
     * starts accepting documents nobody signed. */
    uint8_t *raw;
    size_t raw_len;
} nd_manifest;

/* manifest.parse(raw).
 *
 * ND_UPD_OK and *out owned by the caller (free with nd_manifest_free()), or
 * ND_UPD_ERR_BAD_MANIFEST with *out NULL and `why` holding manifest.py's own
 * refusal. `why` may be NULL to discard it.
 *
 * ND_UPD_ERR_UNREADABLE means the manifest could not be read for a reason
 * that is NOT the package's fault -- today only an allocation this device
 * could not satisfy, which is a normal condition on 53 MB. nd_update.h's
 * taxonomy has no ND_UPD_ERR_NOMEM and that header is not this module's to
 * extend, so `why` says which it was. */
nd_update_err nd_manifest_parse(const uint8_t *raw, size_t len, nd_manifest **out, char *why,
                                size_t why_sz);

void nd_manifest_free(nd_manifest *m);

/* Manifest.hash_offset / Manifest.image_bytes -- the same number twice, and
 * both are the size of the PADDED SQUASHFS. Neither is the size of the
 * rootfs.squashfs member, which carries the verity hash area as well.
 * Confusing the two is how a system image gets truncated.
 *
 * Python multiplies two arbitrary-precision integers here. This saturates at
 * UINT64_MAX instead of overflowing; a manifest that reaches that is asking
 * for an image sixteen million times larger than the flash. */
uint64_t nd_manifest_hash_offset(const nd_manifest *m);
uint64_t nd_manifest_image_bytes(const nd_manifest *m);

/* Manifest.check_compatible(platform, kernel).
 *
 * ND_UPD_ERR_INCOMPATIBLE is THE BRICK CASE and is never overridable: it
 * means this image is built for other hardware, or needs a kernel newer than
 * the one running. `kernel` may be NULL or "" for the Python's kernel=None,
 * which skips the kernel gate entirely. A NULL `platform` is treated as ""
 * and therefore never matches. */
nd_update_err nd_manifest_check_compatible(const nd_manifest *m, const char *platform,
                                           const char *kernel, char *why, size_t why_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_MANIFEST_H_INCLUDED */
