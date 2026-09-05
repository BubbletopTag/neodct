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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nd_paths.h"
#include "nd_types.h"
#include "platform_test.h"

/* Kept in step with apps/Fetch/fetch_app.h -- the test may not include a
 * header out of an app directory, so the surface under test is restated here
 * and a change to either side shows up as a link-time or an assertion
 * failure. */
#define ND_FETCH_NAME_MAX 96

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
    nd_err (*prepare_dir)(const char *);
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
    *(void **)&api.prepare_dir = need("fetch_prepare_dir");
    *(void **)&api.write_cue = need("fetch_write_cue");
    *(void **)&api.parse_line = need("fetch_parse_list_line");
    *(void **)&api.parse_listing = need("fetch_parse_listing");
    *(void **)&api.format_size = need("fetch_format_size");
    *(void **)&api.build_url = need("fetch_build_url");
    return api.classify != NULL && api.name_is_safe != NULL && api.dest_path != NULL &&
           api.prepare_dir != NULL && api.write_cue != NULL && api.parse_line != NULL &&
           api.parse_listing != NULL && api.format_size != NULL && api.build_url != NULL;
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
    RUN(test_prepare_dir_makes_the_whole_chain);
    RUN(test_cue_for_a_raw_image);
    RUN(test_cue_refuses_what_is_not_a_disc);
    RUN(test_cue_never_overwrites_a_real_one);
    RUN(test_parse_one_line);
    RUN(test_parse_skips_what_it_should);
    RUN(test_parse_whole_listing);
    RUN(test_truncation_never_costs_a_folder);
    RUN(test_format_size);
    RUN(test_build_url);

    dlclose(api.h);
    return pt_report("test_fetch");
}
