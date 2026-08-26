/* nd_remote.h -- finding and fetching an update over the network.
 *
 * A one-to-one port of System/core/UpdateService/remote.py (321 lines). The
 * phone asks GitHub for the releases of its own repository, looks for the
 * asset built for its own platform, and downloads it onto the SD card.
 *
 * ============ WHAT THIS MODULE DELIBERATELY DOES NOT DO ============
 *
 * It stops at "there is a file on the card". It does not decide whether that
 * file is trustworthy: the signature check is nd_signing.c's job, reached
 * through nd_package.c, and it already works. A package built by anybody
 * else fails it and the phone says BAD SIGNATURE, which is the correct
 * outcome and not a bug. remote.py's own docstring is explicit about this --
 * "it is the only part that touches the network, and it has no business also
 * deciding whether a package is trustworthy".
 *
 * It also never downloads into /NeoDCT/User. That partition is 8 MB on the
 * Luckfox and a package is nearly 60 MB; the card is the only place with
 * room, and it is where a card-installed package would live anyway, so an
 * interrupted download leaves a file the existing flow can still find.
 *
 * ============ THE TRANSPORT IS /usr/bin/curl, SPAWNED ============
 *
 * NOT libcurl linked into libneodct.so. The reason is measured rather than
 * aesthetic: linking OpenSSL into this library for the signature verifier
 * cost +1.3 MB of idle RSS in EVERY process that maps it, because nd-core
 * maps libneodct at boot and never verifies a signature. A download happens
 * a few times a year. curl's memory should exist only while one is running,
 * and a separate process is how that is arranged on a 53 MB phone.
 *
 * curl is already in both defconfigs (BR2_PACKAGE_LIBCURL_CURL) along with
 * the CA bundle (BR2_PACKAGE_CA_CERTIFICATES), and it carries three things
 * this module would otherwise have to write: GitHub's asset URLs 302 to
 * objects.githubusercontent.com, TLS verification against that bundle, and
 * byte-range requests. This codebase already spawns aplay, mpv and atcmd for
 * exactly this class of work; nd_proc_spawn() is the house way to do it and
 * is what is used here.
 *
 * ============ THE TEST SEAM IS PATH ============
 *
 * spec-update-system.md asked for nd_remote_set_transport(). There is none:
 * with the transport in another executable, PATH already is the seam, and a
 * stand-in `curl` that serves committed fixtures off the disk exercises the
 * real code -- the real argv, the real pipes, the real header parsing, the
 * real resume arithmetic -- rather than a second implementation of it. See
 * test/unit/test_remote.c and neodct/tests/remote/.
 *
 * nd_remote_set_sleep_fn() does still exist, because the backoff is real on
 * the phone and five to sixty seconds of it in a unit test is not.
 */

#ifndef ND_REMOTE_H_INCLUDED
#define ND_REMOTE_H_INCLUDED

#include "nd_types.h"
#include "nd_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The constants at the top of remote.py, unchanged
 * ------------------------------------------------------------------ */

/* The project's own repository. Overridable so a fork can be pointed at
 * without rebuilding the image. Only the owner/name part is overridable --
 * the HOST is always api.github.com, exactly as in the Python. */
#define ND_REMOTE_DEFAULT_REPO "BubbletopTag/neodct"
#define ND_REMOTE_REPO_ENV     "NEODCT_UPDATE_REPO"
#define ND_REMOTE_API_ALL      "https://api.github.com/repos/%s/releases?per_page=%d"

/* GitHub REJECTS requests with no User-Agent. This is not decoration. */
#define ND_REMOTE_USER_AGENT "NeoDCT-Update/1.0 (+https://github.com/BubbletopTag/neodct)"
#define ND_REMOTE_ACCEPT     "application/vnd.github+json"

#define ND_REMOTE_CONNECT_TIMEOUT 20
/* Reading the package is not the same job as reaching GitHub. This phone is
 * on a carrier, through an antenna glued inside a plastic back cover,
 * fetching 53 MB. A gap between packets is normal there; treating it as a
 * failure after 20 seconds is what made a slow download an impossible one. */
#define ND_REMOTE_DOWNLOAD_TIMEOUT 120

/* How many times to pick the download back up before giving up. Each attempt
 * RESUMES, so these are not restarts -- five is a lot of rope. */
#define ND_REMOTE_DOWNLOAD_ATTEMPTS 5

/* Wait between attempts, doubling: 0, 5, 10, 20, 40 for the default five.
 * Retrying instantly is worse than not retrying at all -- when the bearer
 * drops the whole data session goes, DNS included ("Temporary failure in
 * name resolution"), and five immediate attempts are five failures in under
 * a second, after which the phone has given up on a connection that would
 * have come back on its own. */
#define ND_REMOTE_RETRY_BACKOFF     5
#define ND_REMOTE_RETRY_BACKOFF_MAX 60

#define ND_REMOTE_CHUNK (64u * 1024u)

/* Leave the card room to breathe rather than filling it exactly. */
#define ND_REMOTE_SPACE_MARGIN (8 * 1024 * 1024)

/* per_page. Bounds how much attacker-influenced JSON arrives at all. */
#define ND_RELEASES_LIMIT 30

/* ------------------------------------------------------------------ *
 * Bounds the Python does not have
 * ------------------------------------------------------------------ *
 *
 * Every one of these covers text GitHub sent over the network onto a phone
 * that will hand the result to dd. Nothing here is a hint; a field that does
 * not fit means the release is skipped, never truncated into use.
 */

/* Matches ND_UPDATE_VERSION_MAX and ND_MANIFEST_VERSION_MAX, so copying a
 * version between the three structs cannot truncate. */
#define ND_REMOTE_VERSION_MAX 64
#define ND_REMOTE_TAG_MAX     64
/* Matches nd_upd_release.url, which is ND_PATH_MAX. */
#define ND_REMOTE_URL_MAX 512
/* Release notes have no natural bound; this is what the update page can
 * show, and matches ND_UPDATE_CHANGELOG_MAX. Notes ARE truncated -- unlike
 * every other field -- because losing the tail of a paragraph costs nothing
 * and refusing a release over its prose would be absurd. */
#define ND_REMOTE_NOTES_MAX 1024
/* "UPDATE-" + platform + ".ndsw" with the platform at its own 64-byte cap. */
#define ND_REMOTE_ASSET_MAX 128
/* The longest composed refusal: "download stopped early (%lld of %lld
 * bytes)" plus curl's own reason text. */
#define ND_REMOTE_WHY_MAX 192

/* The biggest release listing that will be read at all. The parser has its
 * own ceiling (ND_JSON_MAX_BYTES); this one stops the read before the bytes
 * are ever in memory. */
#define ND_REMOTE_BODY_MAX (1024u * 1024u)

/* ------------------------------------------------------------------ *
 * A release
 * ------------------------------------------------------------------ */

/* One entry of what all_releases() builds. `size` is int64 rather than
 * uint64 because it is compared against a file offset and against a
 * subtraction that can go negative in an intermediate step; a size GitHub
 * reports as absent or unreadable is 0, exactly as `int(asset.get("size") or
 * 0)` gives.
 *
 * sizeof is ~1.7 kB, so an array of ND_RELEASES_LIMIT of them belongs on the
 * heap and not on a thread stack. nd_remote_latest() needs no array at all. */
typedef struct {
    char version[ND_REMOTE_VERSION_MAX]; /* the tag with leading 'v's stripped */
    char tag[ND_REMOTE_TAG_MAX];
    char url[ND_REMOTE_URL_MAX];
    int64_t size;
    char notes[ND_REMOTE_NOTES_MAX];
    bool prerelease;
} nd_release;

/* ------------------------------------------------------------------ *
 * Naming
 * ------------------------------------------------------------------ */

/* The repository to ask about: $NEODCT_UPDATE_REPO if it is set and not
 * empty, else ND_REMOTE_DEFAULT_REPO. Never NULL, never allocated.
 *
 * The value is NOT validated here -- nd_remote_all_releases() refuses one
 * that cannot go into a URL, so that a bad override is a message on screen
 * rather than a silently different repository. */
const char *nd_remote_repo(void);

/* remote.asset_name(platform) -> "UPDATE-<platform>.ndsw".
 *
 * The qemu and luckfox images are different builds carrying different
 * platform strings, so a release has one asset per platform and the phone
 * picks by name. Installing the wrong one is refused by
 * nd_manifest_check_compatible() anyway, but downloading 60 MB to find that
 * out would be a poor way to learn it. ND_ERR_TOOLONG rather than a
 * truncated name, which would match no asset and read as "nothing
 * published". */
nd_err nd_remote_asset_name(const char *platform, char *out, size_t out_sz);

/* ------------------------------------------------------------------ *
 * Version ordering
 * ------------------------------------------------------------------ *
 *
 * remote.version_key(). Versions look like 0.3.10a: numbers with an optional
 * letter suffix, split on '.', '-' and '_'. Compared PIECEWISE, because a
 * plain string comparison puts 0.3.9a above 0.3.10a and the phone would then
 * sit on 0.3.9a forever telling its owner it was up to date.
 *
 * A chunk that is not <digits><letters> sorts as (-1, chunk) -- below every
 * numeric chunk. An empty chunk contributes nothing.
 */

#define ND_VERKEY_MAX_PARTS  8
#define ND_VERKEY_MAX_SUFFIX 16

typedef struct {
    int64_t num; /* -1 for a chunk that is not <digits><letters> */
    char suf[ND_VERKEY_MAX_SUFFIX];
} nd_verpart;

typedef struct {
    nd_verpart p[ND_VERKEY_MAX_PARTS];
    size_t n;
} nd_verkey;

/* `text` may be NULL, which parses as the empty key. Leading and trailing
 * ASCII whitespace is stripped, as Python's str.strip() does.
 *
 * Three bounded-C divergences from the Python, each written up at its site
 * in nd_remote.c and each in the direction of sorting LOWER (so a version
 * that hits one is not offered, rather than wrongly offered):
 *   - components past the eighth are dropped;
 *   - a suffix past 15 characters is truncated;
 *   - a number past INT64_MAX saturates instead of growing a bignum.
 * No version anybody has published comes close to any of them. */
void nd_verkey_parse(const char *text, nd_verkey *out);

/* -1, 0 or +1, with Python tuple ordering: element by element, numbers
 * first, then suffixes by byte value; if every common element is equal the
 * shorter key is the smaller one. */
int nd_verkey_cmp(const nd_verkey *a, const nd_verkey *b);

/* remote.is_newer(candidate, installed). A phone with no recorded version is
 * offered anything: `if not installed: return True`. */
bool nd_remote_is_newer(const char *candidate, const char *installed);

/* ------------------------------------------------------------------ *
 * Room on the card
 * ------------------------------------------------------------------ */

/* remote.enough_space(). `directory` is ND_ROOT-resolved.
 *
 * TRUE when statvfs itself fails: "cannot tell; let the write fail
 * honestly". Refusing a download because the card could not be measured
 * would be a refusal nobody could act on. */
bool nd_remote_enough_space(const char *directory, int64_t size);

/* ------------------------------------------------------------------ *
 * Asking GitHub
 * ------------------------------------------------------------------ *
 *
 * Both of these return:
 *
 *   ND_UPD_OK               -- and *out is filled in
 *   ND_UPD_ERR_NO_PACKAGE   -- remote.NoRelease: reached GitHub, and there
 *                              is nothing here for THIS phone. The caller
 *                              shows "Nothing published", not an error.
 *   ND_UPD_ERR_NETWORK      -- remote.NetworkError, in all its forms.
 *
 * `why` receives str(exc) in the Python's own wording and may be NULL.
 */

/* remote.all_releases(platform, limit).
 *
 * Newest-published first, as GitHub returns them -- NOT sorted by version.
 * The Downgrade tool wants the whole list; the point of it is going
 * backwards. A release with no asset for this platform is SKIPPED, not
 * listed and then refused: an entry you cannot pick is worse than one that
 * is not there.
 *
 * `out` holds at most max_out entries and *n_out receives how many were
 * written. Releases beyond max_out are counted in neither -- they are simply
 * not returned, which is the same thing GitHub's per_page already does. */
nd_update_err nd_remote_all_releases(const char *platform, int32_t limit, nd_release *out,
                                     size_t max_out, size_t *n_out, char *why, size_t why_sz);

/* remote.latest(platform): the newest published package for this platform.
 *
 * Built on all_releases and NOT on GitHub's /releases/latest, which looks
 * like the obvious endpoint and is the wrong one: it ignores prereleases,
 * and every NeoDCT release is a prerelease because the software is alpha.
 * With no stable release ever published that endpoint answered 404 for this
 * repository, and the phone concluded there was nothing to install -- for
 * every release ever made.
 *
 * Ordered by VERSION and not by publication date, so re-publishing an old
 * tag cannot make it the newest thing on offer. Ties keep the earlier entry,
 * which is Python's max(). */
nd_update_err nd_remote_latest(const char *platform, nd_release *out, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * Downloading
 * ------------------------------------------------------------------ */

/* done/total in bytes, called as the bytes land. `total` is 0 when the size
 * is not known. */
typedef void (*nd_remote_progress_fn)(void *ctx, int64_t done, int64_t total);

/* remote.download(url, destination, size, progress, attempts).
 *
 * `destination` is ND_ROOT-resolved and is on the CARD. Returns the bytes
 * written in *written_out when that is non-NULL.
 *
 * ============ IT RESUMES, AND THAT IS THE WHOLE POINT ============
 *
 * The bytes are written to "<destination>.part" and renamed only once the
 * download is complete, so a half-finished file is never mistaken for an
 * installable package. Each attempt asks for the REST with a Range header,
 * and progress accumulates across dropped connections.
 *
 * The version this replaced deleted its partial file on any network error,
 * so every attempt began at zero and a link that could not carry the whole
 * package in one run could never carry it at all -- it did not matter how
 * many times the owner pressed the button.
 *
 * The partial is thrown away in exactly two cases: it is longer than the
 * package (so it is left over from a different, larger one), or the server
 * ignored the Range and is sending the whole file again.
 *
 * Errors, beyond the two above:
 *   ND_UPD_ERR_NO_SPACE      -- refused before a byte is fetched
 *   ND_UPD_ERR_WRITE_FAILED  -- the CARD failed, not the network. The
 *                               partial is discarded, because bytes that
 *                               could not be written are not progress.
 */
nd_update_err nd_remote_download(const char *url, const char *destination, int64_t size,
                                 nd_remote_progress_fn progress, void *ctx, int32_t attempts,
                                 int64_t *written_out, char *why, size_t why_sz);

/* ------------------------------------------------------------------ *
 * The one seam
 * ------------------------------------------------------------------ */

/* remote._sleep. Pass NULL to restore the real one. Process-global and not
 * thread-safe, which is what a test hook should be -- it is set once at the
 * top of a test and cleared at the bottom, and the phone never calls it. */
typedef void (*nd_remote_sleep_fn)(void *ctx, uint32_t seconds);
void nd_remote_set_sleep_fn(nd_remote_sleep_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ND_REMOTE_H_INCLUDED */
