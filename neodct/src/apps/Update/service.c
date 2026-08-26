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
 * ============ IT IS NO LONGER STUBS ============
 *
 * The library landed. lib/nd_package.c reads the zip, lib/nd_manifest.c
 * validates and answers check_compatible(), and lib/nd_signing.c verifies
 * the RSA signature through OpenSSL. This file is now the thin adapter it
 * was always meant to be: it maps their error taxonomy onto the app's,
 * copies the manifest into the app's own struct, and owns the one thing
 * neither side does -- writing the thumbnail somewhere the widget layer can
 * open by path.
 *
 * What has NOT changed is the reason the boundary exists. Everything above
 * it fails safe: a bug shows the wrong words or refuses a good update.
 * Everything below it fails unsafe. So the rules the stubs were written to
 * protect are still the rules, and each one is enforced by the library
 * rather than restated here:
 *
 *   - the signature is over the manifest bytes EXACTLY AS STORED
 *     (nd_package_manifest_raw), never a re-encoding of the parsed object;
 *   - check_compatible is THE BRICK CASE and has exactly one
 *     implementation, in nd_manifest.c, which this file delegates to
 *     rather than reproducing forty lines of string comparison;
 *   - the image is never held in memory: 51 MB against 53 MB usable.
 *
 * ============ AND THE REMOTE HALF LANDED TOO ============
 *
 * lib/nd_remote.c is the port of remote.py. It gets its HTTP and its TLS by
 * spawning /usr/bin/curl -- which is already in both defconfigs along with
 * the CA bundle -- rather than by linking libcurl into libneodct.so, because
 * linking OpenSSL for the signature verifier already cost +1.3 MB of idle
 * RSS in every process that maps this library, and a download happens a few
 * times a year. nd_remote.h has the whole argument.
 *
 * So all four remote.* entry points below are now real. What they still do
 * NOT do is decide whether what they downloaded is trustworthy: the file
 * lands on the card and the ordinary card path -- open, verify, stage --
 * takes it from there, signature check included.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_manifest.h"
#include "nd_package.h"
#include "nd_paths.h"
#include "nd_remote.h"
#include "nd_signing.h"
#include "nd_types.h"
#include "nd_update.h"
#include "nd_widgets.h"

#include "update_app.h"

/* nd_update.h's taxonomy onto __init__.py's three exception classes.
 *
 * UNREADABLE, BAD_ZIP and BAD_MANIFEST all collapse into InvalidUpdate,
 * exactly as Python flattens them -- the app says "INVALID UPDATE! UPDATE
 * MAY BE CORRUPT!!" for all three. They stayed apart in the library so the
 * serial log can say which it was; they must not stay apart here, because
 * the app has three refusal screens and not six.
 *
 * The default is INVALID and not UNAVAILABLE on purpose: an error this
 * function has not been taught about is a broken package, not a missing
 * feature, and telling the owner their build cannot read updates when it
 * can is the more confusing of the two lies. */
static nd_updsvc_err map_err(nd_update_err e)
{
    switch (e) {
    case ND_UPD_OK:
        return ND_UPDSVC_OK;
    case ND_UPD_ERR_INCOMPATIBLE:
        return ND_UPDSVC_INCOMPATIBLE;
    case ND_UPD_ERR_BAD_SIGNATURE:
        return ND_UPDSVC_BAD_SIGNATURE;
    default:
        return ND_UPDSVC_INVALID;
    }
}

/* The app's manifest struct is not the library's. Every char array is sized
 * identically on both sides (nd_manifest.h and update_app.h agree field for
 * field), so these copies cannot truncate -- with ONE exception, called out
 * below. */
static void copy_manifest(nd_upd_manifest *dst, const nd_manifest *src)
{
    memset(dst, 0, sizeof *dst);
    (void)nd_strlcpy(dst->version, src->version, sizeof dst->version);
    dst->buildtime = src->buildtime;
    (void)nd_strlcpy(dst->platform, src->platform, sizeof dst->platform);
    (void)nd_strlcpy(dst->sha256, src->sha256, sizeof dst->sha256);

    /* THE exception. A release note is the one field with no natural bound,
     * so the library heap-allocates it; the app declared a 1024-byte array
     * long before the library existed. Truncating here is right: the app
     * cannot show more than its own screen holds, and the alternative is
     * changing a struct the whole Update app is written against. */
    (void)nd_strlcpy(dst->changelog, src->changelog != NULL ? src->changelog : "",
                     sizeof dst->changelog);

    (void)nd_strlcpy(dst->min_kernel, src->min_kernel, sizeof dst->min_kernel);
    (void)nd_strlcpy(dst->thumbnail_sha256, src->thumbnail_sha256, sizeof dst->thumbnail_sha256);
    (void)nd_strlcpy(dst->verity_root_hash, src->verity_root_hash, sizeof dst->verity_root_hash);
    dst->verity_block_size = src->verity_block_size;
    dst->verity_image_blocks = src->verity_image_blocks;
    (void)nd_strlcpy(dst->verity_salt, src->verity_salt, sizeof dst->verity_salt);
}

struct nd_upd_package {
    nd_package *pkg;
    nd_upd_manifest m;
    /* "" until the thumbnail is asked for. Unlinked by close(). */
    char thumb_path[ND_PATH_MAX];
};

bool nd_upd_service_available(void)
{
    return true;
}

/* ------------------------------------------------------------------ *
 * package.py
 * ------------------------------------------------------------------ */

nd_updsvc_err nd_upd_package_open(const char *path, nd_upd_package **out, char *why, size_t why_sz)
{
    nd_upd_package *p;
    nd_update_err rc;

    if (out == NULL)
        return ND_UPDSVC_INVALID;
    *out = NULL;

    p = calloc(1u, sizeof *p);
    if (p == NULL) {
        if (why != NULL && why_sz > 0u)
            (void)nd_strlcpy(why, "out of memory", why_sz);
        return ND_UPDSVC_INVALID;
    }

    rc = nd_package_open(path, &p->pkg, why, why_sz);
    if (rc != ND_UPD_OK) {
        free(p);
        return map_err(rc);
    }
    copy_manifest(&p->m, nd_package_manifest(p->pkg));
    *out = p;
    return ND_UPDSVC_OK;
}

void nd_upd_package_close(nd_upd_package *pkg)
{
    if (pkg == NULL)
        return;
    /* The thumbnail is ours and nobody else's; it does not outlive the
     * package that produced it. */
    if (pkg->thumb_path[0] != '\0') {
        char resolved[ND_PATH_MAX];

        if (nd_path_resolve(resolved, sizeof resolved, pkg->thumb_path) == ND_OK)
            (void)unlink(resolved);
    }
    nd_package_close(pkg->pkg);
    free(pkg);
}

const nd_upd_manifest *nd_upd_package_manifest(const nd_upd_package *pkg)
{
    return pkg != NULL ? &pkg->m : NULL;
}

const char *nd_upd_package_path(const nd_upd_package *pkg)
{
    return pkg != NULL ? nd_package_path(pkg->pkg) : "";
}

int64_t nd_upd_package_image_size(const nd_upd_package *pkg)
{
    return pkg != NULL ? nd_package_image_size(pkg->pkg) : -1;
}

bool nd_upd_package_signed(const nd_upd_package *pkg)
{
    return pkg != NULL && nd_package_is_signed(pkg->pkg);
}

nd_updsvc_err nd_upd_package_verify_signature(nd_upd_package *pkg, const char *key_path, char *why,
                                              size_t why_sz)
{
    const uint8_t *raw;
    size_t raw_len = 0u;
    uint8_t *sig = NULL;
    size_t sig_len = 0u;
    nd_pubkey *key;
    nd_update_err rc;
    bool ok;

    if (pkg == NULL)
        return ND_UPDSVC_INVALID;

    /* THE BYTES AS STORED. Not a re-encoding of the parsed manifest --
     * re-serialising is how a verifier starts accepting documents nobody
     * signed, and package.py is explicit about it. */
    raw = nd_package_manifest_raw(pkg->pkg, &raw_len);
    if (raw == NULL || raw_len == 0u) {
        if (why != NULL && why_sz > 0u)
            (void)nd_strlcpy(why, "update is not signed", why_sz);
        return ND_UPDSVC_BAD_SIGNATURE;
    }

    rc = nd_package_read_signature(pkg->pkg, &sig, &sig_len, why, why_sz);
    if (rc != ND_UPD_OK)
        return map_err(rc);

    key = nd_sign_load_public_key(key_path);
    if (key == NULL) {
        /* No key on this phone verifies nothing, and it must NOT read as
         * "unsigned" -- engineering mode may acknowledge an unsigned
         * package and continue, and a missing release key is not a thing
         * an operator should be able to click past. */
        free(sig);
        if (why != NULL && why_sz > 0u)
            (void)nd_strlcpy(why, "no release key on this phone", why_sz);
        return ND_UPDSVC_INVALID;
    }

    ok = nd_sign_verify(raw, raw_len, sig, sig_len, key);
    nd_sign_free_public_key(key);
    free(sig);

    if (!ok) {
        if (why != NULL && why_sz > 0u)
            (void)nd_strlcpy(why, "signature does not match", why_sz);
        return ND_UPDSVC_BAD_SIGNATURE;
    }
    nd_package_mark_signed(pkg->pkg);
    return ND_UPDSVC_OK;
}

nd_updsvc_err nd_upd_package_thumbnail_path(nd_upd_package *pkg, char *out, size_t out_sz)
{
    uint8_t *art = NULL;
    size_t art_len = 0u;
    char resolved[ND_PATH_MAX];
    FILE *f;

    if (out == NULL || out_sz == 0u)
        return ND_UPDSVC_INVALID;
    out[0] = '\0';
    if (pkg == NULL)
        return ND_UPDSVC_INVALID;

    /* Already extracted for this package: hand back the same file. */
    if (pkg->thumb_path[0] != '\0')
        return nd_strlcpy(out, pkg->thumb_path, out_sz) == ND_OK ? ND_UPDSVC_OK : ND_UPDSVC_INVALID;

    /* nd_package_read_thumbnail() returns art ONLY when the manifest names a
     * thumbnail_sha256 and the bytes hash to it -- the signature covers
     * manifest.json alone, so unhashed art is unsigned art. Any refusal here
     * is "there is no picture", which the caller draws around. */
    if (nd_package_read_thumbnail(pkg->pkg, &art, &art_len, NULL, 0u) != ND_UPD_OK || art == NULL ||
        art_len == 0u) {
        free(art);
        return ND_UPDSVC_INVALID;
    }

    /* Why a file at all: nd_detailpage_init() takes a PATH, and the widget
     * fills its image field during layout, so handing it bytes afterwards
     * lays the page out for the wrong size (update_app.h records this).
     *
     * Why HERE: /NeoDCT/User/.ndsys is the update system's own state
     * directory. It is on the writable partition, it already exists by the
     * time a package is open, and it is the one place a leftover file would
     * be looked for. The name is fixed rather than temporary so a crash
     * leaves exactly one stale file that the next run overwrites, instead of
     * a directory that fills up. */
    if (nd_snprintf(pkg->thumb_path, sizeof pkg->thumb_path, "%s/thumbnail.png",
                    ND_UPDATE_STATE_DIR) != ND_OK ||
        nd_path_resolve(resolved, sizeof resolved, pkg->thumb_path) != ND_OK) {
        free(art);
        pkg->thumb_path[0] = '\0';
        return ND_UPDSVC_INVALID;
    }

    f = fopen(resolved, "wb");
    if (f == NULL || fwrite(art, 1u, art_len, f) != art_len) {
        if (f != NULL)
            (void)fclose(f);
        free(art);
        pkg->thumb_path[0] = '\0';
        return ND_UPDSVC_INVALID;
    }
    (void)fclose(f);
    free(art);

    return nd_strlcpy(out, pkg->thumb_path, out_sz) == ND_OK ? ND_UPDSVC_OK : ND_UPDSVC_INVALID;
}

/* ------------------------------------------------------------------ *
 * manifest.py
 * ------------------------------------------------------------------ */

nd_updsvc_err nd_upd_manifest_check_compatible(const nd_upd_manifest *m, const char *platform,
                                               const char *kernel, char *why, size_t why_sz)
{
    nd_manifest probe;

    if (m == NULL)
        return ND_UPDSVC_INVALID;

    /* DELEGATED, not reimplemented.
     *
     * This is the brick check -- it decides whether an image built for other
     * hardware is written over this phone's system partition -- and two
     * copies of it is exactly how the two drift apart. nd_manifest.c reads
     * only `platform` and `min_kernel` (verified, not assumed), so a zeroed
     * stack manifest carrying those two fields reaches the same code the
     * package path reaches, with the same wording. */
    memset(&probe, 0, sizeof probe);
    (void)nd_strlcpy(probe.platform, m->platform, sizeof probe.platform);
    (void)nd_strlcpy(probe.min_kernel, m->min_kernel, sizeof probe.min_kernel);

    return map_err(nd_manifest_check_compatible(&probe, platform, kernel, why, why_sz));
}

/* ------------------------------------------------------------------ *
 * remote.py
 * ------------------------------------------------------------------ */

/* nd_remote.c's errors onto the app's, and it is a DIFFERENT mapping from
 * map_err() above.
 *
 * The one distinction _check_online actually tests for is NoRelease, which
 * it draws as "Nothing published" rather than as a failure -- a release
 * whose assets are still uploading is a normal Tuesday. Everything else is
 * "the download did not happen, and here is the sentence to print": the
 * carrier dropped, GitHub answered 503, the card is full, the card refused
 * the write. One screen, one value.
 *
 * ND_UPDSVC_UNAVAILABLE is deliberately not reachable from here any more.
 * It means "this build cannot do updates", which is no longer true. */
static nd_updsvc_err map_remote_err(nd_update_err e)
{
    switch (e) {
    case ND_UPD_OK:
        return ND_UPDSVC_OK;
    case ND_UPD_ERR_NO_PACKAGE:
        return ND_UPDSVC_INVALID; /* remote.NoRelease */
    default:
        return ND_UPDSVC_NETWORK;
    }
}

nd_updsvc_err nd_upd_remote_latest(const char *platform, nd_upd_release *out, char *why,
                                   size_t why_sz)
{
    nd_release found;
    nd_update_err rc;

    if (out == NULL)
        return ND_UPDSVC_NETWORK;
    memset(out, 0, sizeof *out);

    rc = nd_remote_latest(platform, &found, why, why_sz);
    if (rc != ND_UPD_OK)
        return map_remote_err(rc);

    /* Both structs size these fields identically -- ND_REMOTE_VERSION_MAX is
     * ND_UPDATE_VERSION_MAX and ND_REMOTE_URL_MAX is ND_PATH_MAX, and
     * nd_remote.h says so at each of them -- so neither copy can truncate.
     * The app has no field for the tag, the notes or the prerelease flag;
     * nothing above this line reads them. */
    (void)nd_strlcpy(out->version, found.version, sizeof out->version);
    (void)nd_strlcpy(out->url, found.url, sizeof out->url);
    out->size = found.size;
    return ND_UPDSVC_OK;
}

bool nd_upd_remote_is_newer(const char *found, const char *installed)
{
    return nd_remote_is_newer(found, installed);
}

nd_updsvc_err nd_upd_remote_asset_name(const char *platform, char *out, size_t out_sz)
{
    /* ND_ERR_TOOLONG rather than a truncated name: a truncated asset name
     * matches nothing and would read as "nothing published for this phone",
     * which is a lie about the release rather than about the buffer. */
    return nd_remote_asset_name(platform, out, out_sz) == ND_OK ? ND_UPDSVC_OK : ND_UPDSVC_NETWORK;
}

/* The ProgressScreen behind nd_remote's callback. nd_progress_draw() already
 * refuses to repaint when the percentage has not moved, so calling it once
 * per 64 KiB chunk costs one integer division for all but a hundred of them
 * -- which is why the download does not need its own throttle. */
static void progress_bridge(void *ctx, int64_t done, int64_t total)
{
    (void)nd_progress_draw((nd_progress *)ctx, done, total);
}

nd_updsvc_err nd_upd_remote_download(const nd_upd_release *rel, const char *destination,
                                     nd_progress *progress, char *why, size_t why_sz)
{
    nd_update_err rc;

    if (rel == NULL || destination == NULL)
        return ND_UPDSVC_NETWORK;

    rc = nd_remote_download(rel->url, destination, rel->size,
                            (progress != NULL) ? progress_bridge : NULL, progress,
                            ND_REMOTE_DOWNLOAD_ATTEMPTS, NULL, why, why_sz);
    return map_remote_err(rc);
}
