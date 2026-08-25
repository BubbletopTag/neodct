/* nd_package.h -- reading .ndsw packages off an SD card.
 *
 * A port of System/core/UpdateService/package.py (172 lines) together with
 * the zip reader Python got from its standard library and C has to write.
 *
 * An .ndsw is an ordinary zip holding four things:
 *
 *     rootfs.squashfs   the whole new root filesystem, ~51 MB, with the
 *                       dm-verity hash area appended
 *     manifest.json     version, buildtime, platform, sha256, verity
 *     manifest.sig      detached RSA/SHA-256 signature over manifest.json
 *     thumbnail.png     optional, a small square picture for the update page
 *
 * Only manifest.json is signed. It carries the sha256 of the image and of the
 * thumbnail, so one signature over one small file authenticates every other
 * member -- which is why every hash comparison in here is load-bearing and
 * none of them may be skipped when a member "looks fine".
 *
 * ============ THIS CODE READS AN ATTACKER'S FILE ============
 *
 * The package arrives on a FAT32 card that anybody can write, and what comes
 * out of it is handed to `dd` at the next boot. So the reader refuses rather
 * than copes, and the refusals below are the ones a hand-written zip reader
 * usually gets wrong:
 *
 *   - the data offset of a member comes from the LOCAL header's name and
 *     extra lengths, never the central directory's. They routinely differ,
 *     and this is the single most common bug in hand-written zip readers;
 *   - the local header's name must equal the central directory's, which is
 *     what stops one entry describing another's bytes;
 *   - the declared uncompressed size is a CEILING, not a hint: inflate stops
 *     there, and a stream that still has output left is refused rather than
 *     allowed to expand a 4 KB member into the whole of RAM;
 *   - a member whose content is shorter than it declares is refused, not
 *     silently returned short;
 *   - the CRC-32 of every member is checked at EOF, over every byte;
 *   - encrypted members are refused, because nothing can decrypt them and a
 *     reader that ignored the flag would hash ciphertext;
 *   - an entry whose name is absolute, contains "..", or contains a
 *     backslash is refused for the WHOLE ARCHIVE. Nothing here extracts by
 *     name, so this cannot be exploited today; it is refused anyway because
 *     an .ndsw containing a path-traversal entry was not produced by
 *     mkupdate.py and the next person to add an extract-all is not going to
 *     re-derive this.
 *
 * ============ NOTHING READS THE IMAGE INTO MEMORY ============
 *
 * The image is 51 MB and the phone has 53 MB usable. There is deliberately no
 * "read the image member" entry point: nd_package_extract_image() streams it
 * to a file in ND_PACKAGE_CHUNK pieces, hashing on the way past, and the
 * whole-member reads (manifest, signature, thumbnail) are each capped.
 *
 * ============ WHAT IS NOT HERE ============
 *
 * nd_package_verify_signature() is ABSENT ON PURPOSE. package.py's
 * verify_signature() is two lines of zip reading wrapped around an RSA
 * PKCS#1 v1.5 verification, and that verification lives with the RSA code --
 * a second, unreviewed copy of the check that decides whether an image is
 * genuine is worse than none (see apps/Update/service.c). What this module
 * owes that code is the two byte strings it needs and somewhere to record
 * the answer: nd_package_manifest_raw(), nd_package_read_signature() and
 * nd_package_mark_signed().
 */

#ifndef ND_PACKAGE_H_INCLUDED
#define ND_PACKAGE_H_INCLUDED

#include "nd_manifest.h"
#include "nd_types.h"
#include "nd_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The members, spelled once
 * ------------------------------------------------------------------ */

#define ND_PACKAGE_IMAGE_MEMBER     "rootfs.squashfs"
#define ND_PACKAGE_MANIFEST_MEMBER  "manifest.json"
#define ND_PACKAGE_SIGNATURE_MEMBER "manifest.sig"
#define ND_PACKAGE_THUMBNAIL_MEMBER "thumbnail.png"

/* package.py's three constants, unchanged.
 *
 * MAX_THUMBNAIL_BYTES: "A 64x64 PNG is a couple of kilobytes; the cap only
 * exists so a package cannot ask a 64MB phone to decompress a photo album."
 * SPACE_MARGIN: "Leave room so staging an update can never be what fills the
 * partition." */
#define ND_PACKAGE_MAX_THUMBNAIL_BYTES (256u * 1024u)
#define ND_PACKAGE_CHUNK               (256u * 1024u)
#define ND_PACKAGE_SPACE_MARGIN        (4u * 1024u * 1024u)

/* ------------------------------------------------------------------ *
 * Bounds the Python does not have
 * ------------------------------------------------------------------ *
 *
 * OPEN-QUESTIONS.md P-2's rule again: the Python has no limits here and the C
 * needs them, because the input arrives on a card. Each is far above anything
 * the project ships -- mkupdate.py writes four members with names no longer
 * than fifteen characters into a file of about 60 MB.
 */

/* Entries in the central directory. A package has four. */
#define ND_PACKAGE_MAX_MEMBERS 64

/* A member name. cp437 and UTF-8 names are both kept as raw bytes and
 * compared byte for byte, which is exact for the four ASCII names that
 * matter and is the only comparison the local-header check can make. */
#define ND_PACKAGE_NAME_MAX 256

/* The whole file. 60 MB is the real figure; a gigabyte is sixteen times that
 * and keeps every offset inside a 32-bit off_t on the target. */
#define ND_PACKAGE_MAX_FILE_BYTES (1024ull * 1024ull * 1024ull)

/* The ceiling on any read-the-whole-member call. manifest.json is under two
 * kilobytes and a 4096-bit signature is 512 bytes; this is the wall that
 * stops a package declaring a 40 MB manifest. */
#define ND_PACKAGE_MAX_WHOLE_MEMBER (1024u * 1024u)

/* Long enough for the longest refusal, which is
 * "image sha256 does not match the manifest (<64> != <64>)". */
#define ND_PACKAGE_WHY_MAX 224

/* ------------------------------------------------------------------ *
 * The package
 * ------------------------------------------------------------------ */

typedef struct nd_package nd_package;

/* package.extract_image(progress=...). Called after every chunk, with the
 * bytes written so far and the total expected. */
typedef void (*nd_package_progress_fn)(uint64_t done, uint64_t total, void *user);

/* package.open_package(path).
 *
 * `path` is a VIRTUAL path -- it goes through nd_path_resolve() like every
 * other path in the project, which is what nd_storage_find_updates() hands
 * back and what lets a host test point ND_ROOT at a scratch directory.
 *
 * On success *out is owned by the caller; close it with nd_package_close().
 * The manifest is parsed during open, exactly as Package.__init__ does, so a
 * package that opens has a valid manifest and one that does not never
 * existed.
 *
 * Failures, and the taxonomy they land in:
 *
 *   ND_UPD_ERR_UNREADABLE   the file is missing, will not open, or an
 *                           allocation failed. nd_update.h has no
 *                           ND_UPD_ERR_NOMEM and is not this module's to
 *                           extend, so `why` says which it was.
 *   ND_UPD_ERR_BAD_ZIP      not a zip, a corrupt or truncated central
 *                           directory, an unsafe member name, or the archive
 *                           has no rootfs.squashfs.
 *   ND_UPD_ERR_BAD_MANIFEST no manifest.json, or one that does not validate.
 *
 * Python flattens all three into InvalidUpdate and the app flattens them
 * back again into "INVALID UPDATE! UPDATE MAY BE CORRUPT!!"; they are kept
 * apart here so the serial log says which it was. */
nd_update_err nd_package_open(const char *path, nd_package **out, char *why, size_t why_sz);

void nd_package_close(nd_package *pkg);

/* Package.manifest. Never NULL for an open package. */
const nd_manifest *nd_package_manifest(const nd_package *pkg);

/* Package.path -- the virtual path it was opened with, not the resolved one.
 * staging records the BASENAME of this. Never NULL. */
const char *nd_package_path(const nd_package *pkg);

/* Package.image_size: the UNCOMPRESSED size of rootfs.squashfs, from the
 * central directory. -1 when there is no package. Note this is NOT
 * manifest.image_bytes, which is the padded squashfs without the verity hash
 * area; confusing the two truncates a system image. */
int64_t nd_package_image_size(const nd_package *pkg);

/* Package.signed. False until the RSA verifier says otherwise, and false on
 * the engineering-mode override path -- which is what makes the update page's
 * badge say "Not signed". */
bool nd_package_is_signed(const nd_package *pkg);

/* The one thing package.verify_signature() does to the package once the
 * signature checks out. Nothing else may call it. */
void nd_package_mark_signed(nd_package *pkg);

/* ------------------------------------------------------------------ *
 * The seam with the signature verifier
 * ------------------------------------------------------------------ */

/* The manifest bytes EXACTLY AS STORED. The signature is over these and
 * never over a re-encoding of the parsed object. Points into the package;
 * valid until nd_package_close(). */
const uint8_t *nd_package_manifest_raw(const nd_package *pkg, size_t *len_out);

/* manifest.sig, read whole. *out is owned by the caller (free()).
 *
 * ND_UPD_ERR_BAD_SIGNATURE with "update is not signed" when the member is
 * absent -- NOT ND_UPD_ERR_BAD_ZIP, because engineering mode may override a
 * missing signature and may never override a broken archive. */
nd_update_err nd_package_read_signature(nd_package *pkg, uint8_t **out, size_t *len, char *why,
                                        size_t why_sz);

/* ------------------------------------------------------------------ *
 * Members
 * ------------------------------------------------------------------ */

size_t nd_package_member_count(const nd_package *pkg);

/* The member's name, in central-directory order after duplicates have been
 * resolved. NULL past the end. */
const char *nd_package_member_name(const nd_package *pkg, size_t i);

/* The uncompressed size of a named member, or -1 when it is not there. */
int64_t nd_package_member_size(const nd_package *pkg, const char *name);

/* The compression method: 0 stored, 8 deflate. -1 when the member is absent.
 * A real package stores rootfs.squashfs and deflates the rest; the test
 * fixtures deflate everything. Both are mandatory. */
int32_t nd_package_member_method(const nd_package *pkg, const char *name);

/* Read a member whole, with a hard ceiling. Refuses anything whose declared
 * size exceeds `max` BEFORE reading a byte of it, and never allocates more
 * than the declared size. *out is NUL-terminated one byte past *len as a
 * convenience for text members; *len is the truth.
 *
 * ND_UPD_ERR_BAD_ZIP when the member is absent, oversized, or does not
 * survive its own CRC. */
nd_update_err nd_package_read_member(nd_package *pkg, const char *name, size_t max, uint8_t **out,
                                     size_t *len, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * Package.read_thumbnail()
 * ------------------------------------------------------------------ */

/* The package's picture, or *out == NULL with ND_UPD_OK when it hasn't got
 * one. Caller frees.
 *
 * "Only art the manifest vouches for comes back: the signature covers
 * manifest.json alone, so a thumbnail with no hash recorded there is an
 * unsigned attachment and is treated as absent." An oversized thumbnail is
 * refused before it is read; one whose bytes do not hash to the manifest's
 * value is ND_UPD_ERR_BAD_MANIFEST. */
nd_update_err nd_package_read_thumbnail(nd_package *pkg, uint8_t **out, size_t *len, char *why,
                                        size_t why_sz);

/* ------------------------------------------------------------------ *
 * Package.extract_image()
 * ------------------------------------------------------------------ */

/* Copy rootfs.squashfs to `dest` (a VIRTUAL path), verifying its sha256 on
 * the way past. Streams; nothing larger than ND_PACKAGE_CHUNK is ever held.
 *
 * `free_bytes` < 0 asks statvfs(dirname(dest)) instead; a statvfs that fails
 * skips the space check entirely, which is what the Python's `is not None`
 * guard does. `progress` may be NULL.
 *
 * ND_UPD_ERR_NO_SPACE when free_bytes < image_size + ND_PACKAGE_SPACE_MARGIN.
 *
 * ON ANY FAILURE WHATSOEVER `dest` IS UNLINKED before returning. A
 * half-written image must never be left where the applier could find it. */
nd_update_err nd_package_extract_image(nd_package *pkg, const char *dest,
                                       nd_package_progress_fn progress, void *user,
                                       int64_t free_bytes, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * sha256, for callers that have to compare against the manifest
 * ------------------------------------------------------------------ */

/* Lowercase hex, 64 digits plus NUL, so `out` needs ND_MANIFEST_HEX_MAX. */
void nd_package_sha256_hex(const void *data, size_t len, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_PACKAGE_H_INCLUDED */
