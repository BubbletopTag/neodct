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
 *   web.ndb ...                         lib/host-x86_64/app.so
 *                                       web.ndb ...
 *
 * In the first shape manifest.json MUST carry "arch": "<tag>" naming the
 * phone app.so was built for. In the second, lib/<tag>/app.so exists once
 * per phone the package supports and the installer copies the one matching
 * this phone to app.so; the lib/ tree itself is never installed. A package
 * with no app.so for this phone is refused before anything is written --
 * that is the whole point of the tag: a package built for an ABI this
 * machine does not have gets "not for this phone" rather than a dlopen
 * error at first launch.
 *
 * The tags are ND_NAP_ARCH_* below. They name an ABI rather than the bare
 * ISA, because "armv7" alone would not say hard-float, Thumb-2 or which
 * libc.
 *
 * The universal shape survives the two images collapsing onto one ABI, and
 * it is worth saying why rather than leaving it looking vestigial. There is
 * one PHONE tag now, but there has always been a second live tag --
 * host-x86_64, which nd-shoot and the unit suite are in every day -- and the
 * shape is what a future board plugs into: a new machine is a new lib/<tag>/
 * entry in packages that already exist, not a new package format.
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

/* The ABIs. nd_nap_arch_for_machine() maps uname(2)'s machine field onto one
 * of these; a package names them in lib/<tag>/ or in "arch".
 *
 * ============ THE TAG NAMES AN ABI, AND NO LONGER A BOX ============
 *
 * It used to name the target, and there were two targets: the phone was
 * armv7l and the emulator was aarch64. DECISIONS.md D1 made the emulator
 * armv7 Cortex-A7 carrying the phone's musl / hard-float / NEON-VFPv4 /
 * Thumb-2 ABI, so there is now ONE ABI, one app.so per app, and one tag on
 * both machines. That is the prize and not a tidying: a 32-bit time_t, a
 * pointer that no longer holds a size_t or an unaligned NEON load now
 * surfaces in the emulator instead of on the bench.
 *
 * It is still SPELLED luckfox-armv7, deliberately. The tag is a wire format:
 * it is the "arch" key in every manifest ever written, the lib/<tag>/ path in
 * every universal package, mknap.py's --list output and the table in
 * docs/NAP-PACKAGES.md. Renaming it to neodct-armv7 would strand every
 * package built to the published spec, and the only way to un-strand them
 * would be an install-time alias -- the exact mechanism the retired tag below
 * exists to refuse, in the same change that refuses it. So it names an ABI
 * and is spelled after the machine that ABI was chosen for, the way x86_64 is
 * spelled after a chip nobody's laptop contains any more.
 *
 * ============ AND WHY THE RETIRED ONE IS STILL WRITTEN DOWN ============
 *
 * qemu-aarch64 is not an ABI this tree builds, and will not be one again. It
 * is kept as a NAMED REFUSAL. Packages carrying it exist -- one of them is in
 * this repository, which makes it the package a developer is most likely to
 * try first -- and without the constant such a package falls into the generic
 * "not for this phone", which sends its owner looking for a different phone.
 * The truth is that the machine is gone and one rebuild now serves both, and
 * nd_nap_why_no_arch() can only say so if the tag has a name to be keyed on.
 * Naming it is also what gives mknap.py something to refuse, which is what
 * stops a third such package being made.
 *
 * Accepting it as an INSTALL ALIAS for luckfox-armv7 is the one thing that
 * must never happen. The app.so inside such a package is an EM_AARCH64 ELF
 * (measured, in neodct/packages/), so an alias would unpack machine code the
 * loader cannot run, carry the old app's data/ across, put the app in the
 * menu and fail in dlopen() one launch later -- verbatim the failure this tag
 * exists to prevent, moved one step further from its cause. An .ndsw platform
 * key is an identity string and can in principle be aliased; a .nap tag is a
 * claim about a file INSIDE the archive and cannot be.
 *
 * It is never produced: nd_nap_arch_for_machine() cannot return it, and
 * test_nap.c walks every machine string this tree can meet to say so. */
#define ND_NAP_ARCH_LUCKFOX "luckfox-armv7" /* armv7l: the phone and the emulator */
#define ND_NAP_ARCH_HOST    "host-x86_64"   /* x86_64: a host build               */
/* Not a machine any more. Named so it can be refused BY NAME; never made. */
#define ND_NAP_ARCH_RETIRED_QEMU_AARCH64 "qemu-aarch64"

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

/* The one refusal that is not about the package at all, hoisted out of
 * nd_nap.c for the same reason ND_UI_MODEM_FAULT_MESSAGE is hoisted out of
 * nd_ui.c: a message the phone really shows needs a test that can NAME it and
 * measure it, and nd_msgdialog clips silently with a glyph the font does not
 * have. test_widgets_dialogs.c measures this one.
 *
 * It exists at all because 0.5.14a said "Cannot read the package." for EACCES
 * as well as ENOENT, which sent the owner to look at two packages that were
 * perfectly good -- they were 0640 root:root from an engineering app running
 * as root under umask 0027, in a folder ndusr owns. The repair named here is
 * real: re-seating the card runs neodct-sdcard's apply_layout(), whose
 * untrusted/ pass restates exactly these modes. */
#define ND_NAP_WHY_UNREADABLE \
    "Not allowed to read\nthe package.\nTake the card out and\nput it back in."

/* The two ways a package can be for a machine this is not. Hoisted for the
 * same reason and measured by the same test, and there are TWO of them
 * because they send the reader to two different places.
 *
 * "Not for this phone" is true of a package built for hardware that exists,
 * and the cure is to go and find the right package. A retired tag is not
 * that: the machine it names is gone, no package for it will ever be
 * published again, and the only cure is its author rebuilding once -- after
 * which the one package serves both machines. Saying "not for this phone"
 * there sends its owner hunting for a phone that would take it, and there is
 * none.
 *
 * Neither sentence says "QEMU" or "64-bit". A qemu-aarch64 .nap lands on a
 * phone's card as easily as on an emulator's -- the browser puts downloads in
 * untrusted/ either way -- so both have to read true in both hands.
 *
 * Four lines, longest 19 characters, inside the shape ND_NAP_WHY_UNREADABLE
 * already proved at four and 21. nd_msgdialog clips silently, so the width is
 * not a guess: test_widgets_dialogs.c measures both of these. */
#define ND_NAP_WHY_WRONG_PHONE "This package is not for\nthis phone."
#define ND_NAP_WHY_RETIRED_ARCH \
    "This package is for\nan older build.\nAsk its author for\na new one."

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
    /* The install worked and the note to the core did not. The app is on the
     * card and will not be in the menu until the phone restarts; the caller
     * has to say so, because nothing else will. See nd_appgen_bump(). */
    bool needs_restart_to_appear;
} nd_nap_info;

/* ---- what phone is this ---------------------------------------------- */

/* uname(2)'s machine field as a package tag, or "" for a machine no package
 * can name. Cached on first use.
 *
 * ============ uname(2) NO LONGER SEPARATES THE TWO IMAGES ============
 *
 * It used to: the phone said armv7l, the emulator said aarch64, and every
 * .nap was labelled by that difference. Both say armv7l now, so this returns
 * luckfox-armv7 on both -- which is correct, and is the entire point of
 * collapsing the ABI. The consequence is worth stating in one sentence
 * because nothing else in the tree will: nd_platform() is now the ONLY thing
 * that can tell a phone from an emulator. test_nap.c's "one ABI, two
 * identities" case is what stops either half of that coming quietly undone --
 * the day the ABI stops being shared, or the day the platform flag stops
 * discriminating.
 *
 * DELIBERATELY NOT nd_platform.h, although /NeoDCT/platform's image= key
 * carries one of these strings and the duplication is real. Three reasons it
 * must stay uname(2), and the collapse strengthens each of them:
 *
 *   The question is an ABI question. A .nap carries native app.so files, so
 *   what decides which of them can be loaded is the machine executing this
 *   code -- not what an image once recorded about itself. host-x86_64 is the
 *   proof: it is a legitimate answer here, nd-shoot and the unit tests are in
 *   it every day, and no image= will ever say it.
 *
 *   UNKNOWN has no safe meaning in a package path. A phone whose flag was
 *   missing would resolve to "" and every install and every arch match would
 *   fail -- so a cheap-looking substitution turns a missing four-line file
 *   into a phone that cannot install anything. uname(2) does not have a
 *   missing case.
 *
 *   And a MISMATCH is worse than a missing flag. Two build artefacts that
 *   name different machines make nd_platform() UNKNOWN for the life of the
 *   process; routing the ABI answer through it would take a phone that is
 *   still a perfectly ordinary armv7l and stop it installing packages it can
 *   certainly run, over a disagreement about which image it is.
 *
 * ============ THE TAG AND system.os.platform NOW DIVERGE ============
 *
 * Said out loud because the first reader to see both strings will assume one
 * of them is a typo. On the emulator the .nap tag is luckfox-armv7 and
 * system.os.platform is qemu-armv7. They are two keys in two namespaces -- an
 * ABI claim about a file inside an archive, and the update system's
 * compatibility key -- which happened to coincide on both images until now,
 * and the coincidence ending is what proves they were always separate.
 * Unifying them either strands every armv7 package or deletes the update
 * discriminator D1 exists to keep. nd_platform.h's "What this is not" block
 * carries the same sentence from the other side. */
const char *nd_nap_phone_arch(void);

/* The mapping itself, exposed so a test can pin it: "armv7l" and "armv7" ->
 * luckfox-armv7, "x86_64" -> host-x86_64, anything else -> "". Never NULL.
 *
 * "aarch64" is in that "anything else" now, and that is a behaviour change on
 * an arm64 developer box: it used to answer qemu-aarch64 -- a host build
 * claiming to be the QEMU phone -- and now answers "", so Settings on such a
 * machine refuses every package. That is the honest answer. It is a host
 * build, and there has never been a host-aarch64 app.so tag; adding one would
 * put a fourth ABI into a tree that just spent a stage collapsing to one. */
const char *nd_nap_arch_for_machine(const char *machine);

/* Tests only, the sibling of nd_platform__set_build(): pretend uname(2) said
 * that. NULL restores the real reading. Drops the cache as part of the call,
 * so a case cannot forget.
 *
 * It exists because the hole this stage closes was unfakeable. All test_nap.c
 * could say about nd_nap_phone_arch() was that the answer was one of the
 * legal strings, which is true before AND after the day every .nap silently
 * re-labels -- so the one change that could quietly break every package in
 * the world was the one change no test could see.
 *
 * An environment variable was the cheaper seam and is refused for the reason
 * nd_platform.h spells out at length: NEODCT_PLATFORM can arrive from
 * /NeoDCT/User/env.sh, which is arbitrary shell run as root from the only
 * writable partition and survives updates because an update replaces only the
 * rootfs. After D1 the ABI answer is worth flipping for exactly the same
 * reasons the platform answer is. Code already linked into the test's own
 * address space is the honest framing, and like nd_platform__set_build() this
 * is a symbol worth grepping for in review. */
void nd_nap__set_machine(const char *machine);

/* ---- names ------------------------------------------------------------ */

/* "Phone Book" -> "PhoneBook". False when nothing usable is left, or the
 * result would not fit -- a name that is all punctuation is not an app. */
bool nd_nap_dir_from_name(const char *name, char *out, size_t out_sz);

/* The file name without its directory or its .nap, for the picker. Returns
 * out. */
const char *nd_nap_display_name(const char *path, char *out, size_t out_sz);

/* True when `arch` is one of info->arches. */
bool nd_nap_info_has_arch(const nd_nap_info *info, const char *arch);

/* Why a package has no app.so this machine can load, in the words the phone
 * shows: ND_NAP_WHY_RETIRED_ARCH when the only machines it names are ones
 * this tree has retired -- or when `arch` itself is one -- and
 * ND_NAP_WHY_WRONG_PHONE otherwise. Never NULL, never allocated; `info` may
 * be NULL and `arch` may be "".
 *
 * A package that names a retired tag AND a live one is the wrong package
 * rather than an old one, so it gets the generic sentence: its author has
 * already done the rebuild, and this owner is holding the wrong file.
 *
 * It exists because that sentence had two copies -- nd_nap_install()'s, and
 * Settings' pre-install check, which refuses FIRST and is therefore the one
 * an owner actually reads. A special case in one copy is a special case
 * nobody sees. */
const char *nd_nap_why_no_arch(const nd_nap_info *info, const char *arch);

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

/* ---- menu ids ---------------------------------------------------------- *
 *
 * ============ THE BAND, AND WHY THERE IS ONE ============
 *
 * nd_ui.c's rescan_apps() concatenates the stock list, the synthetic
 * Engineering tile and the installed apps, and sorts them by the manifest's
 * "id" with a STABLE insertion sort, so two apps that claim the same id keep
 * readdir order -- which is
 * inode order on ext4, i.e. install order, i.e. nothing an owner can see or
 * predict. Nothing allocated ids, nothing checked them, and both .nap
 * packages that exist today claim 13, because 13 was the next number after
 * Update's 12 and both authors counted the same way.
 *
 * So the numbers are banded. Stock apps keep 1-99 (they run 1-12 today), the
 * 9xx block stays reserved for the ones that must sort last (MusicPlayer 970,
 * Power 971, and the Engineering tile 972 -- see ND_UI_ENG_TILE_ID in nd_ui.h,
 * which is a menu the core synthesises rather than an app anybody installs),
 * and everything installed from a card belongs between these two.
 * A package outside the band still installs -- refusing an app over its
 * position in a menu would be absurd -- but it is logged, and Settings can
 * say so before it writes anything.
 *
 * Documented for third-party authors in docs/NAP-PACKAGES.md. */
#define ND_NAP_ID_MIN 100
#define ND_NAP_ID_MAX 899

/* True when some OTHER app already installed under `apps_dir` claims `id`.
 * `skip_dir` (may be NULL) is the directory about to be replaced, so
 * re-installing an app is never a collision with itself. When true and
 * `out_name` is given, it receives that app's manifest "name" -- the words to
 * put on the screen. Reads only manifests; writes nothing. */
bool nd_nap_id_conflict(const char *apps_dir, int32_t id, const char *skip_dir, char *out_name,
                        size_t out_sz);

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
