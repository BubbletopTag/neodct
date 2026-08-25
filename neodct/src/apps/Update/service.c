/* apps/Update/service.c -- the boundary with UpdateService, and the reason
 * every function in it says no.
 *
 * System/core/UpdateService/ is 1,272 lines of Python in seven modules:
 *
 *     __init__.py    the three refusal classes
 *     signing.py     DER parsing, RSA PKCS#1 v1.5 / SHA-256 verification
 *     manifest.py    manifest.json parsing, validation, compatibility rules
 *     package.py     opening .ndsw zips, streaming extraction, the thumbnail
 *     staging.py     the KEY=value records (ported -- see staging.c)
 *     remote.py      GitHub release discovery, resumable HTTPS download
 *     verity.py      dm-verity hash-tree construction and the dm table line
 *
 * spec-update-system.md maps six of those onto nd_rsa.c, nd_bignum.c,
 * nd_manifest.c, nd_zip.c, nd_package.c, nd_remote.c and nd_verity.c, in a
 * shared library called libndupdate.so whose second consumer is the
 * engineering Downgrade app. NONE OF THOSE FILES EXIST. The whole of the C
 * update system today is include/nd_update.h -- an enum, some string
 * constants, and one function (nd_update_message()) that lib/ never defines.
 *
 * ============ WHY THIS FILE IS STUBS AND NOT AN IMPLEMENTATION ============
 *
 * Because the shape of the failure matters more than the presence of the
 * feature. Everything above this boundary -- the screens, the refusals, the
 * ordering, the staging record -- fails safe: a bug there shows the wrong
 * words or refuses a good update. Everything below it fails unsafe: a zip
 * reader that misreads a local header, or an RSA verifier that checks the
 * DigestInfo by searching the decrypted block instead of rebuilding the
 * padding whole, hands `dd` an image that nobody signed and turns a phone
 * into a brick that cannot be talked out of it afterwards.
 *
 * The Python knows this. signing.py's docstring names Bleichenbacher'06
 * forgeries; the app's own docstring says of the BAD SIGNATURE case that
 * "an image nobody signed is how you end up stuck on a phone that will not
 * boot". Writing a second, unreviewed copy of that check inside one app's
 * .so -- while a first copy is specified to live in a shared library that
 * Downgrade will also link -- is worse than having none.
 *
 * So the boundary is honest. nd_upd_service_available() is false, every entry
 * point returns ND_UPDSVC_UNAVAILABLE with a reason, and nd_update_install()
 * stops on its first call and says so on screen. When libndupdate lands, this
 * file is the only one that changes: the declarations in update_app.h are
 * already the Python's own call shapes.
 *
 * ============ WHAT A REPLACEMENT HAS TO GET RIGHT ============
 *
 * Recorded here so it is not re-derived from scratch. All of it is in
 * spec-update-system.md sections 1-4 and was read out of the Python:
 *
 *   - Members: rootfs.squashfs (ZIP_STORED in a real package, ZIP_DEFLATED in
 *     the test fixtures -- both methods are mandatory), manifest.json,
 *     manifest.sig, thumbnail.png.
 *   - The data offset of a member comes from the LOCAL header's name and
 *     extra lengths, never the central directory's.
 *   - Zip64 must be tolerated, duplicate names resolve to the last entry, and
 *     the CRC-32 of every member is checked at EOF.
 *   - The signature is over the manifest bytes EXACTLY AS STORED, never a
 *     re-encoding of the parsed object.
 *   - The image is never held in memory: 51 MB against 53 MB usable.
 *   - read_thumbnail() returns art only when manifest.thumbnail_sha256 is
 *     present AND the bytes hash to it, because the signature covers
 *     manifest.json alone.
 */

#include <string.h>

#include "nd_types.h"

#include "update_app.h"

/* The single sentence every entry point hands back as str(exc). It reaches
 * the screen in two places: after "Download failed.\n", and inside the "No
 * connection" page where the Python prints the NetworkError. */
static const char *const UNAVAILABLE_WHY = "this build has no update package reader";

static nd_updsvc_err unavailable(char *why, size_t why_sz)
{
    if (why != NULL && why_sz > 0u)
        (void)nd_strlcpy(why, UNAVAILABLE_WHY, why_sz);
    return ND_UPDSVC_UNAVAILABLE;
}

bool nd_upd_service_available(void)
{
    return false;
}

/* ------------------------------------------------------------------ *
 * package.py
 * ------------------------------------------------------------------ */

nd_updsvc_err nd_upd_package_open(const char *path, nd_upd_package **out, char *why, size_t why_sz)
{
    ND_UNUSED(path);
    if (out != NULL)
        *out = NULL;
    return unavailable(why, why_sz);
}

void nd_upd_package_close(nd_upd_package *pkg)
{
    /* Nothing can be open, so there is nothing to close. Kept because the
     * caller's `with pkg:` has to have a matching call somewhere, and a
     * replacement will need it. */
    ND_UNUSED(pkg);
}

const nd_upd_manifest *nd_upd_package_manifest(const nd_upd_package *pkg)
{
    ND_UNUSED(pkg);
    return NULL;
}

const char *nd_upd_package_path(const nd_upd_package *pkg)
{
    ND_UNUSED(pkg);
    return "";
}

int64_t nd_upd_package_image_size(const nd_upd_package *pkg)
{
    ND_UNUSED(pkg);
    return -1;
}

bool nd_upd_package_signed(const nd_upd_package *pkg)
{
    ND_UNUSED(pkg);
    return false;
}

nd_updsvc_err nd_upd_package_verify_signature(nd_upd_package *pkg, const char *key_path, char *why,
                                              size_t why_sz)
{
    ND_UNUSED(pkg);
    ND_UNUSED(key_path);
    return unavailable(why, why_sz);
}

nd_updsvc_err nd_upd_package_thumbnail_path(nd_upd_package *pkg, char *out, size_t out_sz)
{
    ND_UNUSED(pkg);
    if (out != NULL && out_sz > 0u)
        out[0] = '\0';
    return ND_UPDSVC_UNAVAILABLE;
}

/* ------------------------------------------------------------------ *
 * manifest.py
 * ------------------------------------------------------------------ */

nd_updsvc_err nd_upd_manifest_check_compatible(const nd_upd_manifest *m, const char *platform,
                                               const char *kernel, char *why, size_t why_sz)
{
    ND_UNUSED(m);
    ND_UNUSED(platform);
    ND_UNUSED(kernel);
    /* Deliberately NOT implemented here even though it is pure string
     * comparison and would compile in forty lines. It is the check that
     * decides whether an image built for other hardware is written over this
     * phone's system partition, and the Python's own comment calls it "the
     * brick case". Two implementations of that -- one here, one in the
     * nd_manifest.c the spec asks for -- is exactly how the two drift apart.
     * It belongs beside the manifest parser that produces the struct it
     * reads. */
    return unavailable(why, why_sz);
}

/* ------------------------------------------------------------------ *
 * remote.py
 * ------------------------------------------------------------------ */

nd_updsvc_err nd_upd_remote_latest(const char *platform, nd_upd_release *out, char *why,
                                   size_t why_sz)
{
    ND_UNUSED(platform);
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return unavailable(why, why_sz);
}

bool nd_upd_remote_is_newer(const char *found, const char *installed)
{
    ND_UNUSED(found);
    ND_UNUSED(installed);
    /* remote.is_newer() compares two release tags with rules of its own
     * (remote.py). Answering "yes" here would offer a download that cannot
     * happen; answering "no" would claim the phone is up to date on no
     * evidence. Neither is reachable -- nd_upd_remote_latest() fails first --
     * so this answers the one that never puts a claim on screen. */
    return false;
}

nd_updsvc_err nd_upd_remote_asset_name(const char *platform, char *out, size_t out_sz)
{
    ND_UNUSED(platform);
    if (out != NULL && out_sz > 0u)
        out[0] = '\0';
    return ND_UPDSVC_UNAVAILABLE;
}

nd_updsvc_err nd_upd_remote_download(const nd_upd_release *rel, const char *destination,
                                     nd_progress *progress, char *why, size_t why_sz)
{
    ND_UNUSED(rel);
    ND_UNUSED(destination);
    ND_UNUSED(progress);
    return unavailable(why, why_sz);
}
