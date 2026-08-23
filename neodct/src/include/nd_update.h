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

/* The message shown for each. Owned by libneodct; never NULL. */
const char *nd_update_message(nd_update_err err);

/* Where staged updates and their records live. The initramfs applier
 * (ndsys-apply.sh) reads these paths, so they are a contract with a shell
 * script and cannot move independently. */
#define ND_UPDATE_STATE_DIR   "/NeoDCT/User/.ndsys"
#define ND_UPDATE_PENDING     "/NeoDCT/User/.ndsys/pending"
#define ND_UPDATE_RESULT      "/NeoDCT/User/.ndsys/result"
#define ND_UPDATE_PACKAGE_EXT ".ndsw"

/* Manifest member names, spelled once. */
#define ND_UPD_KEY_FORMAT    "format"
#define ND_UPD_KEY_VERSION   "version"
#define ND_UPD_KEY_PLATFORM  "platform"
#define ND_UPD_KEY_BUILDTIME "buildtime"
#define ND_UPD_KEY_IMAGE     "image"
#define ND_UPD_KEY_IMAGE_SHA "image_sha256"
#define ND_UPD_KEY_SIGNATURE "signature"
#define ND_UPD_KEY_NOTES     "notes"

/* buildtime must be a JSON INTEGER. Not 1785160800.0, not "tuesday", and not
 * true. That is why nd_json.h keeps ND_JSON_INT, ND_JSON_REAL and ND_JSON_BOOL
 * as three distinct types. */

#ifdef __cplusplus
}
#endif

#endif /* ND_UPDATE_H_INCLUDED */
