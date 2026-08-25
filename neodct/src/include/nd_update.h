/* nd_update.h -- the error taxonomy and the verbatim on-screen strings for
 * the software update system.
 *
 * The rest of the update system -- signature verification, the zip reader,
 * the manifest validator, the staging records, dm-verity -- lives behind
 * nd_package.h and friends, owned by the update work package. THIS header
 * holds only the two things everyone else needs to agree on: what can go
 * wrong, and exactly what the phone says when it does.
 *
 * The strings are verbatim because they are user-visible and because
 * test_systemupdate_app.py asserts on several of them. Changing the wording
 * is a product decision, not a porting one.
 */

#ifndef ND_UPDATE_H_INCLUDED
#define ND_UPDATE_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ND_UPD_OK = 0,
    ND_UPD_ERR_NO_CARD,       /* no SD card, or it is not set up             */
    ND_UPD_ERR_NO_PACKAGE,    /* nothing matching *.ndsw in update/          */
    ND_UPD_ERR_UNREADABLE,    /* the file is there and will not open         */
    ND_UPD_ERR_BAD_ZIP,       /* not a zip, or a corrupt central directory   */
    ND_UPD_ERR_BAD_MANIFEST,  /* missing or malformed manifest.json          */
    ND_UPD_ERR_INCOMPATIBLE,  /* right format, wrong platform or version     */
    ND_UPD_ERR_BAD_SIGNATURE, /* the signature did not verify                */
    ND_UPD_ERR_NO_SPACE,      /* not enough room to stage it                 */
    ND_UPD_ERR_WRITE_FAILED,  /* staging or commit failed part way           */
    ND_UPD_ERR_NETWORK,       /* the online check or download failed         */
    ND_UPD_ERR_CANCELLED      /* the user backed out                         */
} nd_update_err;

/* There is deliberately NO nd_update_message(err) here.
 *
 * This header used to declare one, "owned by libneodct; never NULL", and
 * libneodct never defined it. Nothing called it, which is the only reason
 * that was harmless: an app that did would have linked, shipped, and then
 * failed dlopen(RTLD_NOW) on the phone with an unresolved symbol -- the
 * app simply would not start, and the crash screen would say nothing
 * useful about why.
 *
 * It is not being defined, because the messages it promised do not exist
 * in that shape. main.py builds each refusal at the point of failure with
 * the context that caused it -- "WRONG UPDATE FOR THIS PHONE!\n%s" % exc,
 * "Update to %s failed.\n%s" -- so a static table keyed on the enum could
 * only ever hold a wording the phone does not actually show. A second,
 * divergent source of user-visible strings is worse than none, and
 * apps/Update/ already carries the real ones verbatim.
 *
 * The taxonomy below stays: it is what the update work package will want
 * when libndupdate lands (spec-update-system.md), and an enum with no
 * users is inert in a way a missing symbol is not. */

/* Where staged updates and their records live. The initramfs applier
 * (ndsys-apply.sh) reads these paths, so they are a contract with a shell
 * script and cannot move independently.
 *
 * The three record names were WRONG here until the Update app was ported:
 * this header said "pending" and "result", while staging.py writes and
 * ndsys-apply.sh reads pending.prop, pending.img and last_result.prop. The
 * shell script runs at boot before any of this code exists, so the shell
 * script is right and the header was not. Nothing had used the bad macros
 * -- the app defined its own, correct, copies rather than trust them --
 * which is the only reason this was a latent trap and not a phone that
 * staged updates into a file the applier never looks at. */
#define ND_UPDATE_STATE_DIR      "/NeoDCT/User/.ndsys"
#define ND_UPDATE_PENDING_RECORD "/NeoDCT/User/.ndsys/pending.prop"
#define ND_UPDATE_PENDING_IMAGE  "/NeoDCT/User/.ndsys/pending.img"
#define ND_UPDATE_RESULT_RECORD  "/NeoDCT/User/.ndsys/last_result.prop"
#define ND_UPDATE_PACKAGE_EXT    ".ndsw"

/* Manifest member names, spelled once.
 *
 * These were wrong too, and in a way that would have looked like a corrupt
 * package rather than a bug: "format", "image", "image_sha256",
 * "signature" and "notes" are not fields manifest.py has ever written --
 * "format" does not appear in that file at all. The real schema is
 *
 *     REQUIRED        = ("version", "buildtime", "platform", "sha256",
 *                        "verity")
 *     REQUIRED_VERITY = ("root_hash", "block_size", "image_blocks")
 *
 * (System/core/UpdateService/manifest.py:7-8), plus the optional
 * changelog, min_kernel, thumbnail_sha256 and the verity salt. */
#define ND_UPD_KEY_VERSION    "version"
#define ND_UPD_KEY_BUILDTIME  "buildtime"
#define ND_UPD_KEY_PLATFORM   "platform"
#define ND_UPD_KEY_SHA256     "sha256"
#define ND_UPD_KEY_VERITY     "verity"

/* Inside the "verity" object. */
#define ND_UPD_KEY_ROOT_HASH    "root_hash"
#define ND_UPD_KEY_BLOCK_SIZE   "block_size"
#define ND_UPD_KEY_IMAGE_BLOCKS "image_blocks"
#define ND_UPD_KEY_SALT         "salt"

/* Optional. */
#define ND_UPD_KEY_CHANGELOG  "changelog"
#define ND_UPD_KEY_MIN_KERNEL "min_kernel"
#define ND_UPD_KEY_THUMB_SHA  "thumbnail_sha256"

/* buildtime must be a JSON INTEGER. Not 1785160800.0, not "tuesday", and not
 * true. That is why nd_json.h keeps ND_JSON_INT, ND_JSON_REAL and ND_JSON_BOOL
 * as three distinct types. */

#ifdef __cplusplus
}
#endif

#endif /* ND_UPDATE_H_INCLUDED */
