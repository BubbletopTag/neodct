/* nd_nap.h -- .nap packages: how an app gets onto the memory card.
 *
 * ============ WHAT A .nap IS ============
 *
 * A NeoDCT Application Package -- that is what the letters stand for -- is a
 * plain, uncompressed POSIX ustar archive with a .nap extension, holding
 * one app: its manifest.json, its icon, its app.so and whatever files the
 * app reads. Installing one means unpacking it into
 *
 *     /NeoDCT/User/sdcard/apps/<Name>/
 *
 * which is the directory nd_ui_scan_apps() reads and nd_proc.c confines --
 * everything under it runs as ndusr_ut with a private mount namespace and no
 * service socket, and the only thing the app may write is its own data/.
 * Nothing in this file changes that: a package is a way of getting files
 * into a directory whose rules were already written, and the rules are about
 * WHERE the code is, not about what the package says.
 *
 * Why tar and not zip: the update system's .ndsw is a zip because it carries
 * one 51 MB member that has to be streamed and hashed, and zip's central
 * directory is what makes "find manifest.json without reading the image"
 * possible. A .nap is a few files that are all installed, so the archive is
 * read once from the front, and the 512-byte ustar header is the simplest
 * container that carries a file name, a size and a mode. Anyone can make one
 * with tar(1); neodct/tools/mknap.py makes one that follows the rules below
 * exactly.
 *
 * Why uncompressed: the phone already reads the card, and an app's bulk is
 * usually one data file the author packed the way they wanted -- the Bible's
 * text is zlib per chapter behind its own index. Compressing that again buys
 * nothing and costs an inflate the installer would have to bound.
 *
 * ============ THE LAYOUT INSIDE THE ARCHIVE ============
 *
 * Every entry is relative to the app's directory; there is no top-level
 * folder. Two shapes are accepted, and both come out the same on the card:
 *
 *   ONE PHONE (the simple case)         ANY PHONE (universal)
 *
 *   manifest.json                       manifest.json
 *   icon.png                            icon.png
 *   app.so                              lib/luckfox-armv7/app.so
 *   web.ndb ...                         lib/qemu-aarch64/app.so
 *                                       web.ndb ...
 *
 * In the first shape manifest.json MUST carry "arch": "<tag>" naming the
 * phone app.so was built for. In the second, lib/<tag>/app.so exists once
 * per phone the package supports and the installer copies the one matching
 * this phone to app.so; the lib/ tree itself is never installed. A package
 * with no app.so for this phone is refused before anything is written --
 * that is the whole point of the tag: a QEMU image trying an armv7 package
 * gets "not for this phone" rather than a dlopen error at first launch.
 *
 * The tags are ND_NAP_ARCH_* below. They name the TARGET rather than the
 * bare ISA because "armv7" alone would not say hard-float, Thumb-2 or which
 * libc, and the two builds this tree makes are already called luckfox and
 * qemu everywhere else.
 *
 * ============ WHAT IS REFUSED, AND WHY ============
 *
 * The archive is a file off a removable card, so it is an attacker's file
 * and the reader refuses rather than copes:
 *
 *   - any entry that is not a regular file or a directory. Symlinks and hard
 *     links are how an archive reaches outside the directory it is unpacked
 *     into; devices and fifos have no business in an app;
 *   - an absolute name, a ".." component, an empty component or a backslash;
 *   - anything under data/. That directory is the app's writable storage and
 *     the CORE makes it (neodct-sdcard's apply_layout), owned so that the
 *     app can write it; a package pre-seeding it would arrive with the wrong
 *     owner and, worse, would let a package overwrite what an earlier
 *     version of the app saved;
 *   - anything under lib/ that is not lib/<tag>/app.so, so that the name
 *     stays reserved for what it means;
 *   - pax and GNU extension headers ('x', 'g', 'L', 'K'). mknap.py writes
 *     plain ustar and so does `tar --format=ustar`; a package that needs a
 *     long-name extension has a path longer than the card layout wants
 *     anyway;
 *   - a header whose checksum does not match, which is either corruption or
 *     not a tar at all;
 *   - more than ND_NAP_MAX_ENTRIES entries, or a file over
 *     ND_NAP_MAX_FILE_BYTES, so a crafted header cannot fill the card.
 *
 * ============ THE INSTALL IS STAGED ============
 *
 * Files are unpacked into a sibling directory, and manifest.json is written
 * LAST. nd_ui_scan_apps() shows a directory the moment it has a manifest, so
 * a half-unpacked app never appears in the menu: an install that dies half
 * way leaves a directory with no manifest, which is invisible, and the next
 * install of the same package removes it.
 *
 * A package that is already installed is REPLACED, and its data/ is carried
 * across: the old directory is renamed aside, its data/ is moved into the
 * staged one, the staged one takes the name, and only then is the old one
 * removed. An upgrade that fails at any step before the final rename puts
 * the old directory back. The app's saved state survives an upgrade because
 * that is what an owner expects an upgrade to mean.
 *
 * ============ WHO RUNS THIS ============
 *
 * The Settings app, as ndusr. apps/ on the card is 0755 ndusr:ndusr, so
 * ndusr can create the directory and every file in it, and the modes set
 * here (0755 directories, 0644 files) are the ones apply_layout() restates
 * on every mount. What ndusr CANNOT do is make data/ belong to ndusr_ut --
 * that needs CAP_CHOWN, which the core gave up -- so after a successful
 * install the caller asks the core for nd_svc_layout_card(), which runs the
 * helper as root and creates data/ with the right owner. Until that runs the
 * app is in the menu and cannot save anything; the next card mount would fix
 * it too.
 *
 * Every path here is a LOGICAL one and goes through nd_path_resolve(), so
 * the host tests drive the whole of it inside a scratch ND_ROOT.
 */

#ifndef ND_NAP_H_INCLUDED
#define ND_NAP_H_INCLUDED

#include "nd_storage.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_NAP_SUFFIX ".nap"

/* The phones. nd_nap_arch_for_machine() maps uname(2)'s machine field onto
 * one of these; a package names them in lib/<tag>/ or in "arch". */
#define ND_NAP_ARCH_LUCKFOX "luckfox-armv7" /* armv7l: the Luckfox Pico */
#define ND_NAP_ARCH_QEMU    "qemu-aarch64"  /* aarch64: the QEMU image  */
#define ND_NAP_ARCH_HOST    "host-x86_64"   /* x86_64: a host build     */

#define ND_NAP_ARCH_MAX   32
#define ND_NAP_ARCHES_MAX 8

/* The directory name on the card. Derived from the manifest's "name" by
 * nd_nap_dir_from_name(), which keeps letters, digits, '_' and '-' and drops
 * everything else -- so "Phone Book" installs as PhoneBook. Bounded because
 * it becomes part of an nd_app_entry path (ND_APP_PATH_MAX). */
#define ND_NAP_DIR_MAX 48

/* manifest "version", "author" and "description": all optional, shown on the
 * install screen, which falls back to sensible text when a field is absent. */
#define ND_NAP_VERSION_MAX 24
#define ND_NAP_AUTHOR_MAX  48
#define ND_NAP_DESC_MAX    256
#define ND_NAP_ICON_MAX    100 /* the icon file's name; a ustar name at most */

/* Bounds on the archive itself. 4096 entries is forty times the largest
 * stock app; 64 MiB per file is the biggest thing a card this phone takes
 * could sensibly carry as one file, and the point of both is that a crafted
 * header cannot ask for more than that. */
#define ND_NAP_MAX_ENTRIES    4096u
#define ND_NAP_MAX_FILE_BYTES (64u * 1024u * 1024u)

/* manifest.json is read into memory to be parsed; the update manifest has
 * the same cap in nd_json.h and an app's is smaller. */
#define ND_NAP_MANIFEST_MAX (64u * 1024u)

/* A reason a package was refused, in words a person can read on the phone.
 * nd_msgdialog shows five lines of 14 px; every string written here fits. */
#define ND_NAP_WHY_MAX 128

/* How many .nap files a scan of the card reports. */
#define ND_NAP_MAX_FOUND 64

typedef struct {
    char name[ND_APP_NAME_MAX]; /* manifest "name", as the menu shows it   */
    char dir[ND_NAP_DIR_MAX];   /* the directory under apps/               */
    int32_t id;                 /* manifest "id"; 999 when absent          */
    char arches[ND_NAP_ARCHES_MAX][ND_NAP_ARCH_MAX];
    size_t n_arches; /* the phones this package has an app.so for */
    bool has_icon;   /* the manifest's icon is in the package   */
    size_t n_files;  /* regular files, lib/ and manifest included */
    uint64_t bytes;  /* their total size                        */
    char version[ND_NAP_VERSION_MAX];  /* manifest "version"; "" when absent */
    char author[ND_NAP_AUTHOR_MAX];    /* manifest "author"; "" when absent  */
    char description[ND_NAP_DESC_MAX]; /* manifest "description"; "" absent   */
    char icon[ND_NAP_ICON_MAX];        /* the icon file's name in the package */
} nd_nap_info;

/* ---- what phone is this ---------------------------------------------- */

/* uname(2)'s machine field as a package tag, or "" for a machine no package
 * can name. Cached on first use. */
const char *nd_nap_phone_arch(void);

/* The mapping itself, exposed so a test can pin it: "armv7l" -> luckfox,
 * "aarch64" -> qemu, "x86_64" -> host, anything else -> "". Never NULL. */
const char *nd_nap_arch_for_machine(const char *machine);

/* ---- names ------------------------------------------------------------ */

/* "Phone Book" -> "PhoneBook". False when nothing usable is left, or the
 * result would not fit -- a name that is all punctuation is not an app. */
bool nd_nap_dir_from_name(const char *name, char *out, size_t out_sz);

/* The file name without its directory or its .nap, for the picker. Returns
 * out. */
const char *nd_nap_display_name(const char *path, char *out, size_t out_sz);

/* True when `arch` is one of info->arches. */
bool nd_nap_info_has_arch(const nd_nap_info *info, const char *arch);

/* ---- the archive -------------------------------------------------------- */

/* Read every header, validate every name, parse the manifest and report what
 * the package is. Writes nothing. ND_OK with *out filled, or an error with
 * `why` (when given) explaining it in the words the phone shows. */
nd_err nd_nap_inspect(const char *path, nd_nap_info *out, char *why, size_t why_sz);

/* Install `path` into `apps_dir`/<dir> for the phone named by `arch`,
 * replacing an earlier install and keeping its data/. `apps_dir` must
 * already exist -- it is the card's, and this never creates it.
 *
 * *out (optional) receives the same information nd_nap_inspect() would. A
 * failure after the staging directory was created removes it; a failure
 * during a replacement puts the old app back. */
nd_err nd_nap_install(const char *path, const char *apps_dir, const char *arch, nd_nap_info *out,
                      char *why, size_t why_sz);

/* Is there already an app in apps_dir/<dir>? "Already" means a manifest is
 * there -- a directory with no manifest is a dead install, not an app. */
bool nd_nap_is_installed(const char *apps_dir, const char *dir);

/* Write the manifest's icon file from the package to `dest` -- a path the
 * caller can write and the framebuffer image cache can read -- so the install
 * screen can show it before anything is unpacked. ND_OK when the icon was in
 * the package and written; an error (leaving no `dest`) when it was not, so
 * the caller shows the app with no picture. Writes only `dest`. */
nd_err nd_nap_extract_icon(const char *path, const char *dest);

/* ---- the card ------------------------------------------------------------ */

/* Every *.nap on the card, absolute, in the places an owner would put one:
 * the card's root, its apps/ folder, and untrusted/ -- which is where the
 * browser puts a download. Case-insensitively sorted by name. Nothing when
 * the card is not a ready NeoDCT card. */
size_t nd_nap_find(char out[][ND_STORAGE_PATH_MAX], size_t max);

#ifdef __cplusplus
}
#endif

#endif /* ND_NAP_H_INCLUDED */
