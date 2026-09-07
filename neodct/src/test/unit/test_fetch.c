/* test_fetch.c -- Fetch's pure half: what a name is, where it goes, and what
 * the server's listing text actually says.
 *
 * The transport lives in test_fetch_app.c, which drives real curl invocations
 * against a stand-in. This file needs neither a network nor a UI, so it is
 * the one that runs in a millisecond and catches the mistakes that matter --
 * every function here decides where an attacker-supplied name lands on the
 * owner's memory card.
 *
 * The app is dlopen()ed rather than recompiled: apps/Fetch/app.so is the
 * artefact that ships, and testing a second copy built with different flags
 * would be testing something else. Same reason as test_phonebook.c.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_types.h"
#include "platform_test.h"

/* Kept in step with apps/Fetch/fetch_app.h -- the test may not include a
 * header out of an app directory, so the surface under test is restated here
 * and a change to either side shows up as a link-time or an assertion
 * failure. */
#define ND_FETCH_NAME_MAX 96
/* Restated for the same reason, and this one is a STRING rather than a bound:
 * a copy that drifts from the app's would leave the sentence untested while
 * this file went on passing, so the comparison below is the thing that
 * notices. test_widgets_dialogs.c measures the header's own copy. */
#define FETCH_NOTE_NO_PSX "PSX is not installed, so it waits in Downloads."

typedef struct {
    char name[ND_FETCH_NAME_MAX];
    int64_t size;
    bool is_dir;
} fetch_entry;

typedef enum {
    FETCH_DEST_MUSIC = 0,
    FETCH_DEST_GAME,
    FETCH_DEST_BIOS,
    FETCH_DEST_NAP,
    FETCH_DEST_OTHER
} fetch_dest_kind;

static struct {
    void *h;
    fetch_dest_kind (*classify)(const char *);
    bool (*name_is_safe)(const char *);
    nd_err (*dest_path)(const char *, const char *, bool, char *, size_t, fetch_dest_kind *);
    const char *(*dest_note)(const char *, bool);
    nd_err (*prepare_dir)(const char *);
    nd_err (*give_to_reader)(const char *, fetch_dest_kind);
    nd_err (*give_fd_to_reader)(int, const char *, fetch_dest_kind);
    nd_err (*write_cue)(const char *);
    bool (*parse_line)(const char *, fetch_entry *);
    size_t (*parse_listing)(const char *, fetch_entry *, size_t);
    void (*format_size)(int64_t, char *, size_t);
    nd_err (*build_url)(const char *, const char *, const char *, char *, size_t);
} api;

/* build/<variant>/test/test_fetch -> build/<variant>/apps/Fetch/app.so, so an
 * ASan run loads the ASan app and never a stale default-variant one. */
static bool resolve_app_so(char *out, size_t sz)
{
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(out, sz, "%s/../apps/Fetch/app.so", exe) == ND_OK;
}

static void *need(const char *name)
{
    void *p = dlsym(api.h, name);

    if (p == NULL)
        fprintf(stderr, "test_fetch: app.so has no symbol %s\n", name);
    return p;
}

static bool api_open(void)
{
    char so[ND_PATH_MAX];

    if (!resolve_app_so(so, sizeof so))
        return false;
    api.h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (api.h == NULL) {
        fprintf(stderr, "test_fetch: dlopen %s: %s -- run `make` first\n", so, dlerror());
        return false;
    }
    *(void **)&api.classify = need("fetch_classify");
    *(void **)&api.name_is_safe = need("fetch_name_is_safe");
    *(void **)&api.dest_path = need("fetch_dest_path");
    *(void **)&api.dest_note = need("fetch_dest_note");
    *(void **)&api.prepare_dir = need("fetch_prepare_dir");
    *(void **)&api.give_to_reader = need("fetch_give_to_reader");
    *(void **)&api.give_fd_to_reader = need("fetch_give_fd_to_reader");
    *(void **)&api.write_cue = need("fetch_write_cue");
    *(void **)&api.parse_line = need("fetch_parse_list_line");
    *(void **)&api.parse_listing = need("fetch_parse_listing");
    *(void **)&api.format_size = need("fetch_format_size");
    *(void **)&api.build_url = need("fetch_build_url");
    return api.classify != NULL && api.name_is_safe != NULL && api.dest_path != NULL &&
           api.dest_note != NULL && api.prepare_dir != NULL && api.give_to_reader != NULL &&
           api.give_fd_to_reader != NULL && api.write_cue != NULL &&
           api.parse_line != NULL && api.parse_listing != NULL && api.format_size != NULL &&
           api.build_url != NULL;
}

/* ------------------------------------------------------------------ *
 * What a file is
 * ------------------------------------------------------------------ */

static void test_classify(void)
{
    CHECK(api.classify("A Forest.mp3") == FETCH_DEST_MUSIC);
    CHECK(api.classify("track.FLAC") == FETCH_DEST_MUSIC); /* case does not matter */
    CHECK(api.classify("Crash Bandicoot.bin") == FETCH_DEST_GAME);
    CHECK(api.classify("Crash Bandicoot.cue") == FETCH_DEST_GAME);
    CHECK(api.classify("disc.chd") == FETCH_DEST_GAME);
    /* bios.bin is the console ROM, not a game, whatever its case. */
    CHECK(api.classify("bios.bin") == FETCH_DEST_BIOS);
    CHECK(api.classify("BIOS.BIN") == FETCH_DEST_BIOS);
    CHECK(api.classify("scph1001.bin") == FETCH_DEST_GAME); /* only bios.bin, by name */
    CHECK(api.classify("Bible-qemu-aarch64.nap") == FETCH_DEST_NAP);
    /* Not refused -- just unsorted. It goes where the browser's downloads go. */
    CHECK(api.classify("notes.txt") == FETCH_DEST_OTHER);
    CHECK(api.classify("README") == FETCH_DEST_OTHER);
    CHECK(api.classify("weird.") == FETCH_DEST_OTHER);
    CHECK(api.classify(".hidden") == FETCH_DEST_OTHER);
    CHECK(api.classify(NULL) == FETCH_DEST_OTHER);
}

/* ------------------------------------------------------------------ *
 * What a name may be
 * ------------------------------------------------------------------ *
 *
 * This is the boundary. Everything below arrives from an FTP server and is
 * about to become a path on the card.
 */

static void test_name_safety(void)
{
    char too_long[ND_FETCH_NAME_MAX + 8];

    CHECK(api.name_is_safe("A Forest.mp3"));
    CHECK(api.name_is_safe("Bible-qemu-aarch64.nap"));

    CHECK(!api.name_is_safe(""));
    CHECK(!api.name_is_safe(NULL));
    CHECK(!api.name_is_safe("."));
    CHECK(!api.name_is_safe(".."));
    /* The escape everything else is guarding against. */
    CHECK(!api.name_is_safe("../../etc/passwd"));
    CHECK(!api.name_is_safe("music/track.mp3"));
    CHECK(!api.name_is_safe("dir\\track.mp3"));
    /* A name curl would read as an option if it ever reached an argv. */
    CHECK(!api.name_is_safe("-o"));
    CHECK(!api.name_is_safe("--config"));
    /* A newline would split a netrc line, and a NUL-adjacent control byte has
     * no business in a file name either way. */
    CHECK(!api.name_is_safe("track\nmachine evil.example"));
    CHECK(!api.name_is_safe("track\ttab.mp3"));

    memset(too_long, 'a', sizeof too_long - 1u);
    too_long[sizeof too_long - 1u] = '\0';
    CHECK(!api.name_is_safe(too_long));
}

/* ------------------------------------------------------------------ *
 * Where a file goes
 * ------------------------------------------------------------------ */

static void expect_dest(const char *name, bool psx, const char *want, fetch_dest_kind want_kind)
{
    char got[ND_PATH_MAX];
    fetch_dest_kind kind = FETCH_DEST_OTHER;

    if (api.dest_path("/NeoDCT/User/sdcard", name, psx, got, sizeof got, &kind) != ND_OK) {
        g_checks++;
        g_failures++;
        fprintf(stderr, "FAIL %s:%d  dest_path refused %s\n", __FILE__, __LINE__, name);
        return;
    }
    CHECK_STR(got, want);
    CHECK(kind == want_kind);
}

static void test_destinations(void)
{
    expect_dest("A Forest.mp3", true, "/NeoDCT/User/sdcard/music/A Forest.mp3", FETCH_DEST_MUSIC);
    expect_dest("Bible-qemu-aarch64.nap", true,
                "/NeoDCT/User/sdcard/untrusted/Bible-qemu-aarch64.nap", FETCH_DEST_NAP);
    expect_dest("notes.txt", true, "/NeoDCT/User/sdcard/untrusted/notes.txt", FETCH_DEST_OTHER);

    /* A disc gets a folder of its own, named after it -- the shape the PSX
     * app reads. Both halves of a two-file disc land in the same folder. */
    expect_dest("Crash Bandicoot.bin", true,
                "/NeoDCT/User/sdcard/apps/PSX/games/Crash Bandicoot/Crash Bandicoot.bin",
                FETCH_DEST_GAME);
    expect_dest("Crash Bandicoot.cue", true,
                "/NeoDCT/User/sdcard/apps/PSX/games/Crash Bandicoot/Crash Bandicoot.cue",
                FETCH_DEST_GAME);

    /* A BIOS goes to the emulator's bios/ under the one name the core is sure
     * to find, so an uploaded bios.bin is usable with nothing further to do. */
    expect_dest("bios.bin", true, "/NeoDCT/User/sdcard/apps/PSX/bios/scph1001.bin",
                FETCH_DEST_BIOS);

    /* With no PSX app installed there is no games/ or bios/ worth making, so
     * a disc or a BIOS waits in downloads instead of creating a folder
     * nothing will read. */
    expect_dest("Crash Bandicoot.bin", false, "/NeoDCT/User/sdcard/untrusted/Crash Bandicoot.bin",
                FETCH_DEST_OTHER);
    expect_dest("bios.bin", false, "/NeoDCT/User/sdcard/untrusted/bios.bin", FETCH_DEST_OTHER);

    /* And the boundary holds here too: an unsafe name has no destination at
     * all rather than a repaired one. */
    {
        char got[ND_PATH_MAX];

        CHECK(api.dest_path("/NeoDCT/User/sdcard", "../../etc/passwd", true, got, sizeof got,
                            NULL) == ND_ERR_INVAL);
        CHECK(api.dest_path("/NeoDCT/User/sdcard", "a.mp3", true, got, 8u, NULL) ==
              ND_ERR_TOOLONG);
    }
}

/* The downgrade above is correct and it was SILENT, which is a combination
 * that reads as a fault: the owner downloaded a disc image, the confirm dialog
 * offered to put it in Downloads with no reason given, and they went looking
 * for a bug there was not. So the rule explains itself, and only when there is
 * something to explain -- a note on every download would be noise the owner
 * learns to skip past, which is the same thing as saying nothing. */
static void test_the_downgrade_says_why(void)
{
    /* The two kinds fetch_dest_path() downgrades, and only when it does. */
    CHECK_STR(api.dest_note("Crash Bandicoot.bin", false), FETCH_NOTE_NO_PSX);
    CHECK_STR(api.dest_note("Crash Bandicoot.cue", false), FETCH_NOTE_NO_PSX);
    CHECK_STR(api.dest_note("bios.bin", false), FETCH_NOTE_NO_PSX);
    CHECK(api.dest_note("Crash Bandicoot.bin", true) == NULL);
    CHECK(api.dest_note("bios.bin", true) == NULL);

    /* Everything that lands where it asked to gets no sentence at all. */
    CHECK(api.dest_note("A Forest.mp3", false) == NULL);
    CHECK(api.dest_note("Bible-qemu-aarch64.nap", false) == NULL);
    CHECK(api.dest_note("notes.txt", false) == NULL);
    CHECK(api.dest_note(NULL, false) == NULL);

    /* It names the app the owner has to install, because "not installed" with
     * no subject is a sentence they cannot act on. */
    CHECK(strstr(FETCH_NOTE_NO_PSX, "PSX") != NULL);
}

static void test_prepare_dir_makes_the_whole_chain(void)
{
    char path[ND_PATH_MAX];

    CHECK(api.dest_path("/card", "Crash Bandicoot.bin", true, path, sizeof path, NULL) == ND_OK);
    CHECK(api.prepare_dir(path) == ND_OK);
    CHECK(nd_path_is_dir("/card/apps/PSX/games/Crash Bandicoot"));
    /* Idempotent: a second download into the same folder must not fail. */
    CHECK(api.prepare_dir(path) == ND_OK);

    /* It makes the directory, never the file. */
    CHECK(!nd_path_is_file(path));
}

/* ------------------------------------------------------------------ *
 * The modes and the owner a download is left with
 * ------------------------------------------------------------------ */

/* ============ WHY THIS FILE HAD NO PERMISSION CASE UNTIL NOW ============
 *
 * Every other case here runs as one uid against a scratch tree, so a mode was
 * never load-bearing and the suite could not see the 0.5.14a bug: Fetch is
 * root on the phone, umask 0027 turned curl's 0666 into 0640 and mkdir's 0755
 * into 0750, and the readers -- MusicPlayer, PCSX-ReARMed, Settings'
 * installer -- are all somebody else. The owner watched two .nap packages
 * download and then could not install either.
 *
 * The MODE half is testable as an ordinary user, because chmod on a file one
 * owns needs nothing, and it is the half that decides three different answers.
 * The OWNER half needs root and an ndusr, and says so when it cannot run. */

static unsigned int mode_of(const char *path)
{
    char resolved[ND_PATH_MAX];
    struct stat st;

    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return 0u;
    if (stat(resolved, &st) != 0)
        return 0u;
    return (unsigned int)(st.st_mode & 07777u);
}

/* A file in the state curl leaves one in: root's, and whatever the umask
 * allowed. 0600 stands in for that here -- what matters is only that it is NOT
 * any of the answers below, so a passing assertion means the code ran. */
static void a_downloaded_file(const char *path)
{
    char resolved[ND_PATH_MAX];

    pt_write_text(path, "bytes");
    CHECK(nd_path_resolve(resolved, sizeof resolved, path) == ND_OK);
    CHECK_INT(chmod(resolved, 0600u), 0);
}

/* file uid/gid == the directory's, which is the claim
 * nd_path_give_to_dir_owner() makes and the one that fails on the phone. As an
 * ordinary user both sides are already the caller, so this passes without
 * proving much; the root case below is where it bites. */
static void owner_follows_the_directory(const char *path, const char *dir)
{
    char rf[ND_PATH_MAX];
    char rd[ND_PATH_MAX];
    struct stat f;
    struct stat d;

    CHECK(nd_path_resolve(rf, sizeof rf, path) == ND_OK);
    CHECK(nd_path_resolve(rd, sizeof rd, dir) == ND_OK);
    CHECK_INT(stat(rf, &f), 0);
    CHECK_INT(stat(rd, &d), 0);
    CHECK_INT(f.st_uid, d.st_uid);
    CHECK_INT(f.st_gid, d.st_gid);
}

static void test_each_destination_gets_the_mode_its_reader_needs(void)
{
    /* music/ is 0750 ndusr:ndusr and MusicPlayer IS ndusr, so owner-read is
     * the whole requirement. ndusr_ut is denied the media side on purpose --
     * neodct-sdcard's CARD_LAYOUT -- and o+r here would be this app arguing
     * with that. */
    a_downloaded_file("/card/music/A Forest.mp3");
    CHECK_INT(api.give_to_reader("/card/music/A Forest.mp3", FETCH_DEST_MUSIC), ND_OK);
    CHECK_INT(mode_of("/card/music/A Forest.mp3"), 0640u);
    owner_follows_the_directory("/card/music/A Forest.mp3", "/card/music");

    /* apps/PSX/ is 0755 ndusr:ndusr and the emulator runs as ndusr_ut, which
     * is OTHER in that tree. THIS is the one a blanket "0660 and the folder's
     * owner" gets wrong: it would look like a fix and leave PCSX-ReARMed
     * unable to open its own disc image. */
    a_downloaded_file("/card/apps/PSX/games/Disc/Disc.bin");
    CHECK_INT(api.give_to_reader("/card/apps/PSX/games/Disc/Disc.bin", FETCH_DEST_GAME), ND_OK);
    CHECK_INT(mode_of("/card/apps/PSX/games/Disc/Disc.bin"), 0644u);

    a_downloaded_file("/card/apps/PSX/bios/scph1001.bin");
    CHECK_INT(api.give_to_reader("/card/apps/PSX/bios/scph1001.bin", FETCH_DEST_BIOS), ND_OK);
    CHECK_INT(mode_of("/card/apps/PSX/bios/scph1001.bin"), 0644u);

    /* untrusted/ is 0770 ndusr:ndusr_ut: Settings' installer reads a .nap as
     * the owner and the browser writes here as the group, so both halves are
     * needed and 0660 is what apply_layout() restates. */
    a_downloaded_file("/card/untrusted/Bible.nap");
    CHECK_INT(api.give_to_reader("/card/untrusted/Bible.nap", FETCH_DEST_NAP), ND_OK);
    CHECK_INT(mode_of("/card/untrusted/Bible.nap"), 0660u);

    a_downloaded_file("/card/untrusted/notes.txt");
    CHECK_INT(api.give_to_reader("/card/untrusted/notes.txt", FETCH_DEST_OTHER), ND_OK);
    CHECK_INT(mode_of("/card/untrusted/notes.txt"), 0660u);

    /* Nothing to hand over is not a crash and not a success. */
    CHECK_INT(api.give_to_reader("/card/untrusted/gone.nap", FETCH_DEST_NAP), ND_ERR_IO);
    CHECK_INT(api.give_to_reader(NULL, FETCH_DEST_NAP), ND_ERR_INVAL);
}

/* The directories, which the same umask turns from 0755 into 0750 -- so a
 * games/<Title>/ Fetch has just made is one ndusr_ut cannot even traverse, and
 * the file inside it being 0644 buys nothing. */
static void test_a_folder_fetch_makes_can_be_walked_into(void)
{
    mode_t was = umask(0027u);

    CHECK_INT(api.prepare_dir("/card/apps/PSX/games/Crash Bandicoot/Crash Bandicoot.bin"), ND_OK);
    CHECK_INT(mode_of("/card/apps/PSX/games/Crash Bandicoot"), 0755u);
    CHECK_INT(mode_of("/card/apps/PSX/games"), 0755u);
    CHECK_INT(mode_of("/card/apps/PSX"), 0755u);

    /* And a directory that was ALREADY there keeps the mode the card gave it.
     * untrusted/ is 0770; a mkdir_p that restated 0755 on its way past would
     * take the write bit off the browser's own folder every time somebody
     * downloaded a disc image. */
    pt_mkdir("/card/untrusted");
    {
        char resolved[ND_PATH_MAX];

        CHECK(nd_path_resolve(resolved, sizeof resolved, "/card/untrusted") == ND_OK);
        CHECK_INT(chmod(resolved, 0770u), 0);
    }
    CHECK_INT(api.prepare_dir("/card/untrusted/Bible.nap"), ND_OK);
    CHECK_INT(mode_of("/card/untrusted"), 0770u);

    (void)umask(was);
}

/* The other half, which only a machine with root and the two users can answer:
 * does the file actually change hands? Skipped loudly rather than quietly, the
 * same rule nd-selftest states -- a SKIP is not a PASS. */
static void test_a_root_writer_hands_the_file_over(void)
{
    struct passwd *pw = (geteuid() == 0u) ? getpwnam("ndusr") : NULL;
    char rd[ND_PATH_MAX];
    char rf[ND_PATH_MAX];
    struct stat st;

    if (pw == NULL) {
        fprintf(stderr,
                "SKIP the handover itself: needs root and an ndusr, so this run checked the "
                "modes only\n");
        return;
    }

    pt_mkdir("/card/untrusted");
    CHECK(nd_path_resolve(rd, sizeof rd, "/card/untrusted") == ND_OK);
    CHECK_INT(chown(rd, pw->pw_uid, pw->pw_gid), 0);

    /* Root's file in ndusr's directory: exactly what curl leaves behind. */
    a_downloaded_file("/card/untrusted/Bible.nap");
    CHECK(nd_path_resolve(rf, sizeof rf, "/card/untrusted/Bible.nap") == ND_OK);
    CHECK_INT(chown(rf, 0, 0), 0);

    CHECK_INT(api.give_to_reader("/card/untrusted/Bible.nap", FETCH_DEST_NAP), ND_OK);
    CHECK_INT(stat(rf, &st), 0);
    CHECK_INT(st.st_uid, pw->pw_uid);
    CHECK_INT(st.st_gid, pw->pw_gid);
    CHECK_INT(st.st_mode & 07777u, 0660u);
}

/* ============ AND WHAT IT REFUSES, WHICH IS THE HALF THAT HELD 0.5.15a ====
 *
 * Fetch is an engineering app and runs as ROOT. Its ordinary destination is
 * the card's untrusted/ -- 0770 ndusr:ndusr_ut, the one directory the
 * untrusted set can write -- so between this app writing a file and this app
 * stating its mode, the browser or any installed .nap app can unlink the name
 * and put something else there. chown(2) and chmod(2) both dereference, so
 * the something else that pays is /NeoDCT/User/settings.prop.
 *
 * Both cases below run as an ORDINARY user and still prove the refusal,
 * because what is asserted is that the DECOY was not touched -- and a decoy
 * this process owns is one it could perfectly well have chmodded. A run as
 * root would prove the same thing about a file it could not.
 */
static void test_a_planted_symlink_is_refused_rather_than_followed(void)
{
    char link_resolved[ND_PATH_MAX];
    struct stat st;

    pt_mkdir("/card");
    pt_mkdir("/card/untrusted");
    /* Standing in for /NeoDCT/User/settings.prop: something outside the
     * arrival area, at a mode that says whether root followed the link. */
    a_downloaded_file("/card/settings.prop");

    CHECK(nd_path_resolve(link_resolved, sizeof link_resolved, "/card/untrusted/Bible.nap") ==
          ND_OK);
    {
        char target[ND_PATH_MAX];

        CHECK(nd_path_resolve(target, sizeof target, "/card/settings.prop") == ND_OK);
        (void)unlink(link_resolved);
        CHECK_INT(symlink(target, link_resolved), 0);
    }

    CHECK_INT(api.give_to_reader("/card/untrusted/Bible.nap", FETCH_DEST_NAP), ND_ERR_IO);
    CHECK_INT(mode_of("/card/settings.prop"), 0600u);
    /* And the link itself is still a link: nothing was created over it and
     * nothing was unlinked, because this app's answer to a name that is not
     * its file is to stop. */
    CHECK_INT(lstat(link_resolved, &st), 0);
    CHECK(S_ISLNK(st.st_mode));
    (void)unlink(link_resolved);
}

/* The other way to keep a file after root has changed its owner. This image
 * sets no fs.protected_hardlinks, so an ndusr_ut process that can write
 * untrusted/ can link a name there to any file it can read on the same
 * filesystem -- and a chown of the file it linked to is a chown of both
 * names. One link is what a download has. */
static void test_a_second_hard_link_is_refused_too(void)
{
    char from[ND_PATH_MAX];
    char to[ND_PATH_MAX];

    pt_mkdir("/card");
    pt_mkdir("/card/untrusted");
    a_downloaded_file("/card/untrusted/notes.txt");
    CHECK(nd_path_resolve(from, sizeof from, "/card/untrusted/notes.txt") == ND_OK);
    CHECK(nd_path_resolve(to, sizeof to, "/card/untrusted/kept.txt") == ND_OK);
    (void)unlink(to);
    CHECK_INT(link(from, to), 0);

    CHECK_INT(api.give_to_reader("/card/untrusted/notes.txt", FETCH_DEST_OTHER), ND_ERR_IO);
    CHECK_INT(mode_of("/card/untrusted/notes.txt"), 0600u);
    (void)unlink(to);
}

/* The form ftp.c actually uses: the descriptor the download was written
 * through, never a name. There is no window at all in this one -- the object
 * cannot be substituted, only its name can -- which is why the transport
 * states the mode BEFORE the rename rather than after it. */
static void test_the_descriptor_form_states_the_same_mode(void)
{
    char resolved[ND_PATH_MAX];
    int fd;

    pt_mkdir("/card");
    pt_mkdir("/card/untrusted");
    a_downloaded_file("/card/untrusted/Bible.nap.part");
    CHECK(nd_path_resolve(resolved, sizeof resolved, "/card/untrusted/Bible.nap.part") == ND_OK);
    fd = open(resolved, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);

    /* The path names where the file is GOING -- the directory whose owner is
     * copied down -- while the descriptor is what is changed. */
    CHECK_INT(api.give_fd_to_reader(fd, "/card/untrusted/Bible.nap", FETCH_DEST_NAP), ND_OK);
    CHECK_INT(mode_of("/card/untrusted/Bible.nap.part"), 0660u);
    (void)close(fd);

    CHECK_INT(api.give_fd_to_reader(-1, "/card/untrusted/Bible.nap", FETCH_DEST_NAP),
              ND_ERR_INVAL);
}

/* ------------------------------------------------------------------ *
 * Cue sheets
 * ------------------------------------------------------------------ */

static void write_zeros(const char *path, size_t len)
{
    char *buf = calloc(1u, len ? len : 1u);

    if (buf == NULL)
        return;
    pt_write(path, buf, len);
    free(buf);
}

static void test_cue_for_a_raw_image(void)
{
    char text[256];

    pt_mkdir("/card");
    write_zeros("/card/Disc.bin", 2352u * 4u);
    CHECK(api.write_cue("/card/Disc.bin") == ND_OK);
    CHECK(nd_path_is_file("/card/Disc.cue"));
    (void)pt_read_text("/card/Disc.cue", text, sizeof text);
    /* The file name inside is the BASE name: the cue sits beside the bin and
     * a path in it would break the moment the folder moved. */
    CHECK_STR(text, "FILE \"Disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
}

static void test_cue_refuses_what_is_not_a_disc(void)
{
    pt_mkdir("/card");
    /* Not a whole number of 2352-byte sectors: mkcue.sh refuses this and so
     * does this, because a cue claiming otherwise fails further away. */
    write_zeros("/card/NotADisc.bin", 5000u);
    CHECK(api.write_cue("/card/NotADisc.bin") == ND_ERR_UNSUPPORTED);
    CHECK(!nd_path_is_file("/card/NotADisc.cue"));

    CHECK(api.write_cue("/card/missing.bin") == ND_ERR_NOTFOUND);
}

static void test_cue_never_overwrites_a_real_one(void)
{
    char text[256];

    pt_mkdir("/card");
    write_zeros("/card/Disc.bin", 2352u * 4u);
    pt_write_text("/card/Disc.cue", "REM the owner's own, with audio tracks\n");
    CHECK(api.write_cue("/card/Disc.bin") == ND_OK);
    (void)pt_read_text("/card/Disc.cue", text, sizeof text);
    CHECK_STR(text, "REM the owner's own, with audio tracks\n");
}

/* The cue is the file the emulator actually opens, and this app writes it
 * itself rather than getting it from curl -- so it needs the same handover, or
 * a 0644 disc image sits beside a 0640 cue and the game still does not start.
 * The umask is the phone's here for the same reason as everywhere else: 0644
 * coming back is the only proof the chmod ran. */
static void test_the_cue_is_readable_by_the_emulator_too(void)
{
    mode_t was = umask(0027u);

    pt_mkdir("/card/apps/PSX/games/Disc");
    write_zeros("/card/apps/PSX/games/Disc/Disc.bin", 2352u * 4u);
    CHECK(api.write_cue("/card/apps/PSX/games/Disc/Disc.bin") == ND_OK);
    CHECK_INT(mode_of("/card/apps/PSX/games/Disc/Disc.cue"), 0644u);

    (void)umask(was);
}

/* ------------------------------------------------------------------ *
 * The listing
 * ------------------------------------------------------------------ */

static void test_parse_one_line(void)
{
    fetch_entry e;

    CHECK(api.parse_line("-rw-r--r--    1 1001     1001      4194304 Sep 05 12:01 A Forest.mp3",
                         &e));
    CHECK_STR(e.name, "A Forest.mp3"); /* spaces in the name survive */
    CHECK_INT(e.size, 4194304);
    CHECK(!e.is_dir);

    CHECK(api.parse_line("drwxr-xr-x    2 1001     1001         4096 Sep 05 12:00 music", &e));
    CHECK_STR(e.name, "music");
    CHECK(e.is_dir);
    CHECK_INT(e.size, -1); /* a directory's 4096 means nothing worth showing */

    /* An empty file is 0 bytes and a file whose size will not parse is
     * unknown. They must not collapse into the same thing: the progress bar
     * treats -1 as "no total" and 0 as "already done". */
    CHECK(api.parse_line("-rw-r--r--    1 1001 1001    0 Sep 01 18:30 empty.txt", &e));
    CHECK_INT(e.size, 0);
    CHECK(api.parse_line("-rw-r--r--    1 1001 1001 nnnn Sep 01 18:30 odd.mp3", &e));
    CHECK_INT(e.size, -1);
}

static void test_parse_skips_what_it_should(void)
{
    fetch_entry e;

    CHECK(!api.parse_line("total 24", &e));
    CHECK(!api.parse_line("", &e));
    /* A symlink is how a listing points at something outside the folder. */
    CHECK(!api.parse_line("lrwxrwxrwx 1 0 0 12 Sep 01 18:30 shortcut -> /etc/passwd", &e));
    CHECK(!api.parse_line("crw-rw-rw- 1 0 0 1, 3 Sep 01 18:30 null", &e));
    /* Too few fields, and a line with no name at all. */
    CHECK(!api.parse_line("-rw-r--r-- 1 1001", &e));
    CHECK(!api.parse_line("-rw-r--r-- 1 1001 1001 512 Sep 01 18:30", &e));
    /* And every name the safety rule refuses is refused here. */
    CHECK(!api.parse_line("-rw-r--r-- 1 1001 1001 512 Sep 01 18:30 ../escape.mp3", &e));
    CHECK(!api.parse_line("-rw-r--r-- 1 1001 1001 512 Sep 01 18:30 -o", &e));
}

static void test_parse_whole_listing(void)
{
    static const char TEXT[] =
        "total 24\r\n"
        "drwxr-xr-x    2 1001 1001     4096 Sep 05 12:00 roms\n"
        "-rw-r--r--    1 1001 1001  4194304 Sep 05 12:01 a forest.mp3\n"
        "lrwxrwxrwx    1 1001 1001       12 Sep 01 18:30 shortcut -> /etc/passwd\n"
        "drwxr-xr-x    2 1001 1001     4096 Sep 05 12:00 music\n"
        "-rw-r--r--    1 1001 1001    61440 Sep 01 18:30 Bible.nap\n";
    fetch_entry got[8];
    size_t n = api.parse_listing(TEXT, got, ND_ARRAY_LEN(got));

    /* Five parseable lines, one of them a symlink that is dropped. */
    CHECK_INT(n, 4);
    /* Directories first, then names ascending -- case-insensitively, so
     * "Bible.nap" and "a forest.mp3" sort the way a person expects rather
     * than the way ASCII does. */
    CHECK_STR(got[0].name, "music");
    CHECK_STR(got[1].name, "roms");
    CHECK_STR(got[2].name, "a forest.mp3");
    CHECK_STR(got[3].name, "Bible.nap");

    /* The cap is honoured: a server with a million files fills the array and
     * stops rather than writing past it. */
    CHECK_INT(api.parse_listing(TEXT, got, 2u), 2);
    CHECK_INT(api.parse_listing("", got, ND_ARRAY_LEN(got)), 0);
    CHECK_INT(api.parse_listing(NULL, got, ND_ARRAY_LEN(got)), 0);
}

/* ------------------------------------------------------------------ *
 * Sizes and URLs
 * ------------------------------------------------------------------ */

static void test_format_size(void)
{
    char s[16];

    api.format_size(0, s, sizeof s);
    CHECK_STR(s, "0 B");
    api.format_size(17, s, sizeof s);
    CHECK_STR(s, "17 B");
    api.format_size(1024, s, sizeof s);
    CHECK_STR(s, "1 kB");
    api.format_size(4u * 1024u * 1024u, s, sizeof s);
    CHECK_STR(s, "4.0 MB");
    api.format_size(734003200, s, sizeof s);
    CHECK_STR(s, "700.0 MB");
    /* Unknown is not zero, and must not read as an empty file. */
    api.format_size(-1, s, sizeof s);
    CHECK_STR(s, "?");
}

static void test_build_url(void)
{
    char url[512];

    /* The trailing slash is what makes curl LIST rather than RETR, so the
     * two shapes are not interchangeable. */
    CHECK(api.build_url("10.0.0.1", "", NULL, url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/");
    CHECK(api.build_url("10.0.0.1", "music", NULL, url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/music/");
    CHECK(api.build_url("10.0.0.1", "roms/psx", "Disc.bin", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/roms/psx/Disc.bin");
    CHECK(api.build_url("10.0.0.1", "", "Disc.bin", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/Disc.bin");

    /* ============ THE ONE THAT COST AN EVENING ============
     *
     * Real music has spaces in it. This URL used to be REFUSED, on the
     * reasoning that every path here was built from vetted names and so
     * nothing should need escaping -- which was true of every character
     * except the one that appears in almost every file name a person owns.
     * The app listed the folder perfectly and then said "URL rejected" for
     * every track in it. */
    CHECK(api.build_url("10.0.0.1", "music", "Drake - Make Them Pay.mp3", url, sizeof url) ==
          ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/music/Drake%20-%20Make%20Them%20Pay.mp3");
    /* Only the unreserved set survives; '-', '.', '_' and '~' are unreserved
     * and must NOT be escaped, or the server is asked for a different file. */
    CHECK(api.build_url("10.0.0.1", "", "a-b_c.d~e.mp3", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/a-b_c.d~e.mp3");
    /* A '%' in a name is itself escaped, so a name cannot smuggle an escape
     * sequence of its own into the URL. */
    CHECK(api.build_url("10.0.0.1", "", "50%25.mp3", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/50%2525.mp3");
    /* Directory segments are escaped too, and the separators between them
     * survive -- otherwise a folder with a space in it is unreachable. */
    CHECK(api.build_url("10.0.0.1", "my music/live sets", NULL, url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://10.0.0.1/my%20music/live%20sets/");

    /* ============ IPv6, WHICH IS THE ORDINARY CASE ON THIS PHONE ============
     *
     * T-Mobile's mobile data is IPv6-only, so the bearer the phone actually
     * has cannot reach an IPv4 literal at all. A colon already means "port" in
     * a URL authority, so the address has to be bracketed or curl reads
     * "ftp://2606:4700::1111/" as host 2606 port 4700 and fails on the rest.
     *
     * The host is STORED bare and bracketed only here -- curl matches a netrc
     * `machine` line against the unbracketed form, and the netrc is the whole
     * reason this string is compared rather than escaped. */
    CHECK(api.build_url("2606:4700::1111", "", NULL, url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://[2606:4700::1111]/");
    CHECK(api.build_url("2606:4700::1111", "roms/psx", "Disc.bin", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://[2606:4700::1111]/roms/psx/Disc.bin");
    /* A link-local-looking literal and a fully written-out one both survive;
     * the rule is "contains a colon", not a parse of the address. */
    CHECK(api.build_url("2001:0db8:0000:0000:0000:0000:0000:0001", "", "a.bin", url,
                        sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://[2001:0db8:0000:0000:0000:0000:0000:0001]/a.bin");
    /* A hostname is never bracketed -- DNS64 hands back a AAAA for one of
     * these and curl resolves it itself. */
    CHECK(api.build_url("ftp.example.org", "", "a.bin", url, sizeof url) == ND_OK);
    CHECK_STR(url, "ftp://ftp.example.org/a.bin");
    /* Brackets in the STORED host are refused, so there is exactly one
     * spelling of a host and it is the one the netrc will match. */
    CHECK(api.build_url("[2606:4700::1111]", "", NULL, url, sizeof url) == ND_ERR_INVAL);

    /* Refusal is kept for the thing escaping cannot make safe. */
    CHECK(api.build_url("10.0.0.1", "../..", NULL, url, sizeof url) == ND_ERR_INVAL);
    CHECK(api.build_url("10.0.0.1", "a/../b", NULL, url, sizeof url) == ND_ERR_INVAL);
    CHECK(api.build_url("10.0.0.1", "a/..", NULL, url, sizeof url) == ND_ERR_INVAL);
    CHECK(api.build_url("10.0.0.1", "/absolute", NULL, url, sizeof url) == ND_ERR_INVAL);
    CHECK(api.build_url("10.0.0.1", "music", "../../etc/passwd", url, sizeof url) ==
          ND_ERR_INVAL);
    CHECK(api.build_url("", "music", NULL, url, sizeof url) == ND_ERR_INVAL);
    /* A host is compared byte for byte against the netrc, so it is checked
     * rather than escaped. */
    CHECK(api.build_url("evil host/x", "music", NULL, url, sizeof url) == ND_ERR_INVAL);
}

/* The truncation rule: an array that fills up loses FILES, never folders.
 *
 * LIST comes back in the server's readdir order, so a one-pass fill would
 * drop whatever came last -- and a subfolder at the end of a directory of
 * nine hundred tracks would then be unreachable, with no key to press that
 * would ever reveal it. This is the case that says the two passes are load
 * bearing rather than tidy. */
static void test_truncation_never_costs_a_folder(void)
{
    static const char TEXT[] =
        "-rw-r--r-- 1 1 1 10 Sep 05 12:00 a.mp3\n"
        "-rw-r--r-- 1 1 1 10 Sep 05 12:00 b.mp3\n"
        "-rw-r--r-- 1 1 1 10 Sep 05 12:00 c.mp3\n"
        /* The folder is LAST, which is exactly where a server is free to put
         * it and where a one-pass fill would lose it. */
        "drwxr-xr-x 2 1 1 4096 Sep 05 12:00 live\n";
    fetch_entry got[2];
    size_t n = api.parse_listing(TEXT, got, ND_ARRAY_LEN(got));

    CHECK_INT(n, 2);
    CHECK_STR(got[0].name, "live");
    CHECK(got[0].is_dir);
    /* The second slot is a file, so files are not starved either -- the rule
     * is "directories first", not "directories only". */
    CHECK(!got[1].is_dir);

    /* With room for everything the result is unchanged by the two passes. */
    {
        fetch_entry all[8];

        CHECK_INT(api.parse_listing(TEXT, all, ND_ARRAY_LEN(all)), 4);
        CHECK_STR(all[0].name, "live");
        CHECK_STR(all[1].name, "a.mp3");
    }
}

int main(void)
{
    if (!api_open())
        return 1;

    RUN(test_classify);
    RUN(test_name_safety);
    RUN(test_destinations);
    RUN(test_the_downgrade_says_why);
    RUN(test_prepare_dir_makes_the_whole_chain);
    RUN(test_each_destination_gets_the_mode_its_reader_needs);
    RUN(test_a_folder_fetch_makes_can_be_walked_into);
    RUN(test_a_root_writer_hands_the_file_over);
    RUN(test_a_planted_symlink_is_refused_rather_than_followed);
    RUN(test_a_second_hard_link_is_refused_too);
    RUN(test_the_descriptor_form_states_the_same_mode);
    RUN(test_cue_for_a_raw_image);
    RUN(test_cue_refuses_what_is_not_a_disc);
    RUN(test_cue_never_overwrites_a_real_one);
    RUN(test_the_cue_is_readable_by_the_emulator_too);
    RUN(test_parse_one_line);
    RUN(test_parse_skips_what_it_should);
    RUN(test_parse_whole_listing);
    RUN(test_truncation_never_costs_a_folder);
    RUN(test_format_size);
    RUN(test_build_url);

    dlclose(api.h);
    return pt_report("test_fetch");
}
