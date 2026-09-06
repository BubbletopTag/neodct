/* test_storage.c -- a case-for-case port of neodct/tests/test_storage.py.
 *
 * SD card state as the apps see it. The mount helper (/bin/neodct-sdcard)
 * publishes what it found to /run/neodct/sdcard.prop; this module turns that
 * into the questions the UI actually asks: is there a card, is it one of ours,
 * where do I look for music, and is there an update on it.
 *
 * The pytest monkeypatches MOUNT_POINT and STATE_FILE onto a tmp_path;
 * nd_storage_set_paths() is the same hook, and ND_ROOT keeps it all inside a
 * scratch directory.
 */

#include <stdio.h>
#include <string.h>

#include "nd_storage.h"

#include "platform_test.h"

#define MOUNT "/sdcard"
#define STATE "/sdcard.prop"

static const char *const FOLDERS[5] = {"wallpapers", "tones", "backup_db", "music", "update"};

static void use_scratch_paths(void)
{
    nd_storage_set_paths(MOUNT, STATE);
}

/* insert(tmp_path, state=..., folders=(...), **fields) from the pytest. */
static void insert(const char *state, const char *fstype, const char *label,
                   const char *const *folders, size_t nfolders)
{
    char body[512];
    size_t i;

    CHECK_INT(nd_snprintf(body, sizeof body, "state=%s\ndevice=/dev/vdc\nfstype=%s\nlabel=%s\n",
                          state, fstype, label),
              ND_OK);
    pt_write_text(STATE, body);

    pt_mkdir(MOUNT);
    for (i = 0u; i < nfolders; i++) {
        char path[ND_PATH_MAX];

        CHECK_INT(nd_snprintf(path, sizeof path, "%s/%s", MOUNT, folders[i]), ND_OK);
        pt_mkdir(path);
    }
}

static void insert_default(const char *const *folders, size_t nfolders)
{
    insert("mounted", "vfat", "NEODCT", folders, nfolders);
}

static void test_no_card_when_nothing_is_mounted(void)
{
    nd_card card;

    use_scratch_paths();
    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_ABSENT);
    CHECK(nd_storage_is_ready() == false);
}

static void test_a_neodct_card_is_ready(void)
{
    nd_card card;

    use_scratch_paths();
    insert_default(FOLDERS, 5u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_READY);
    CHECK(nd_storage_is_ready() == true);
    CHECK_STR(card.device, "/dev/vdc");
    CHECK_STR(card.mountpoint, MOUNT);
}

/* A card straight out of a camera: mountable, but not ours yet. */
static void test_a_plain_fat_card_needs_setting_up(void)
{
    static const char *const dcim[1] = {"DCIM"};
    nd_card card;

    use_scratch_paths();
    insert_default(dcim, 1u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_NEEDS_SETUP);
    CHECK(nd_storage_is_ready() == false);
}

static void test_a_card_missing_one_folder_needs_setting_up(void)
{
    static const char *const four[4] = {"wallpapers", "tones", "music", "update"};
    nd_card card;

    use_scratch_paths();
    insert_default(four, 4u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_NEEDS_SETUP);
}

static void test_an_unmountable_card_wants_formatting(void)
{
    nd_card card;

    use_scratch_paths();
    insert("unmountable", "ext4", "", NULL, 0u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_UNFORMATTED);
    /* The device survives this branch; it is what the format dialog names. */
    CHECK_STR(card.device, "/dev/vdc");
}

/* The helper has two words for this and they mean the same thing to the UI:
 * try_mount() says `unmountable` when nothing it tried would mount, and
 * do_format() says `unformatted` when the image has no mke2fs to make a card
 * with. One branch here, or the second spelling would read as an absent card
 * -- which blanks the device, which is the one thing the format dialog needs.
 */
static void test_the_other_spelling_means_the_same_thing(void)
{
    nd_card card;

    use_scratch_paths();
    insert("unformatted", "", "", NULL, 0u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_UNFORMATTED);
    CHECK_STR(card.device, "/dev/vdc");
}

/* ============ A CARD FROM BEFORE THE CARD WAS EXT4 ============
 *
 * FAT32, labelled NEODCT, mounted, and the owner's music on it plays. What it
 * cannot do is hold an installed app: FAT records no ownership, so "an app
 * may read its own app.so but not write it" and "the untrusted set may write
 * this directory and not that one" are sentences the filesystem cannot store.
 *
 * So it is its own state and not UNFORMATTED. The remedy differs in KIND: a
 * card that will not mount can only be reformatted and nothing is lost by it,
 * while this card works and reformatting it ERASES the owner's music. That is
 * theirs to accept, so the phone reports and offers rather than acting.
 */
static void test_a_legacy_fat_card_is_its_own_state(void)
{
    nd_card card;

    use_scratch_paths();
    insert("legacy", "vfat", "NEODCT", FOLDERS, 5u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_LEGACY_FORMAT);
    /* Not READY, even with all five folders present -- the folders are not
     * what is wrong with it. */
    CHECK(nd_storage_is_ready() == false);
    /* Everything the dialog names survives the branch: the device is what
     * the format is pointed at, and the fstype is what makes it legacy. */
    CHECK_STR(card.device, "/dev/vdc");
    CHECK_STR(card.fstype, "vfat");
    CHECK_STR(card.label, "NEODCT");
    CHECK(card.removable == true);
}

/* ============ A CARD FROM SOMEBODY ELSE'S COMPUTER ============
 *
 * ext4, mounted, healthy, full of files -- and every one of them owned by a
 * uid this phone does not have. neodct-sdcard mounts an ext card deliberately
 * WITHOUT uid=/gid=, so the ownership on the card is the ownership the phone
 * honours, and a card mkfs.ext4 left as root:root is one this process can read
 * nothing out of.
 *
 * It is its own state for the same reason `legacy` is: the remedy differs in
 * KIND. NEEDS_SETUP is five mkdirs and keeps everything; this needs either a
 * chown on the computer that owns it or a reformat here, and the reformat
 * erases the card. Before it had a state, the phone offered "Set up", watched
 * the mkdirs fail, and reported "It may be locked or damaged" -- which is
 * neither.
 */
static void test_a_card_from_another_computer_is_its_own_state(void)
{
    nd_card card;

    use_scratch_paths();
    /* All five folders present, which is the point: the folders are not what
     * is wrong with it and having them does not make it READY. */
    insert("foreign", "ext4", "backup", FOLDERS, 5u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_FOREIGN);
    CHECK(nd_storage_is_ready() == false);
    /* Everything the dialog and the format offer need survives the branch. */
    CHECK_STR(card.device, "/dev/vdc");
    CHECK_STR(card.fstype, "ext4");
    CHECK_STR(card.label, "backup");
    CHECK(card.removable == true);
    /* No arrival partition on a card the phone did not lay out, and the
     * caller's answer to that must be to refuse rather than fall back. */
    CHECK_STR(card.untrusted, "");
}

/* The media gate says no, and it is the one state where that needs saying out
 * loud: this is a mounted card with the owner's files on it, unlike every
 * other false answer, and it still cannot be read. A media app handed a path
 * into it would show an empty list and no reason for it. */
static void test_a_foreign_card_hands_out_no_media(void)
{
    char path[ND_PATH_MAX];
    char found[2][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    insert("foreign", "ext4", "backup", FOLDERS, 5u);

    CHECK(nd_storage_media_available() == false);
    CHECK(nd_storage_folder("music", path, sizeof path) == false);
    CHECK_INT(nd_storage_find_updates(found, 2u), 0);
}

static void test_a_legacy_card_has_nowhere_for_a_download(void)
{
    nd_card card;
    char path[ND_PATH_MAX];

    use_scratch_paths();
    insert("legacy", "vfat", "NEODCT", FOLDERS, 5u);

    nd_storage_card(&card);
    /* A directory ndusr_ut may write cannot exist on a filesystem with no
     * notion of who owns what, so there is none -- and the caller's answer
     * must be to REFUSE the download rather than fall back to /NeoDCT/User,
     * which is 8 MiB on the phone and holds the databases. */
    CHECK_STR(card.untrusted, "");
    CHECK(!nd_storage_untrusted_dir(path, sizeof path));
}

/* ============ THE CARD THE OWNER ACTUALLY HAS ============
 *
 * This is the regression that read, from the owner's side, as "it now refuses
 * to recognize an sdcard at all". Their card is a card 0.4.x formatted, so it
 * is FAT32 with the NeoDCT folders on it -- exactly the card the state above
 * describes -- and across 0.5.0b every one of these came back empty: no music,
 * no ringtones, no wallpapers, no update offered.
 *
 * Nothing was wrong with the card and nothing was wrong with the classification
 * either: `legacy` is the truthful word for it. What was wrong is that
 * nd_storage_folder() asked nd_storage_is_ready(), which is READY and nothing
 * else, so the ownership question stood in for the readability question in
 * every media path on the phone. Opening a .mp3 does not care who owns it.
 *
 * The test is the four folders, one per app that lost its content, because the
 * shape of the bug was that they all went at once through one predicate.
 */
static void test_a_legacy_card_still_hands_out_its_media(void)
{
    char out[ND_PATH_MAX];

    use_scratch_paths();
    insert("legacy", "vfat", "NEODCT", FOLDERS, 5u);

    CHECK(nd_storage_media_available() == true);
    /* And it is still not READY: an app installed onto a FAT card could not be
     * confined, which is the one thing that genuinely does not work here. */
    CHECK(nd_storage_is_ready() == false);

    CHECK(nd_storage_folder("music", out, sizeof out) == true);
    CHECK_STR(out, MOUNT "/music");
    CHECK(nd_storage_folder("tones", out, sizeof out) == true);
    CHECK_STR(out, MOUNT "/tones");
    CHECK(nd_storage_folder("wallpapers", out, sizeof out) == true);
    CHECK_STR(out, MOUNT "/wallpapers");
    CHECK(nd_storage_folder("update", out, sizeof out) == true);
    CHECK_STR(out, MOUNT "/update");
}

/* An .ndsw is a file. Installing one off a FAT card is what 0.4.x did. */
static void test_a_legacy_card_still_offers_its_updates(void)
{
    char found[8][ND_STORAGE_PATH_MAX];
    char dirs[4][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    insert("legacy", "vfat", "NEODCT", FOLDERS, 5u);
    pt_write_text(MOUNT "/update/UPDATE.ndsw", "zip");
    pt_mkdir("/system-tones");

    CHECK_INT(nd_storage_find_updates(found, 8u), 1);
    CHECK_STR(found[0], MOUNT "/update/UPDATE.ndsw");

    /* And the media directories, which is the same predicate one call up. */
    CHECK_INT(nd_storage_media_dirs("tones", "/system-tones", dirs, 4u), 2);
    CHECK_STR(dirs[0], "/system-tones");
    CHECK_STR(dirs[1], MOUNT "/tones");
}

/* The other side of the widening, and the reason it is a second predicate
 * rather than a looser is_ready(). A card that mounted but has none of the
 * folders has nothing to hand out: nd_storage_folder() would be naming a
 * directory that does not exist, and every caller would have to stat it
 * again. UNFORMATTED and ABSENT have no filesystem at all. */
static void test_only_a_readable_card_counts_as_media(void)
{
    static const char *const dcim[1] = {"DCIM"};

    use_scratch_paths();
    insert_default(dcim, 1u);
    CHECK(nd_storage_media_available() == false);

    pt_new_case();
    use_scratch_paths();
    insert("unmountable", "", "", NULL, 0u);
    CHECK(nd_storage_media_available() == false);

    pt_new_case();
    use_scratch_paths();
    CHECK(nd_storage_media_available() == false);
}

/* ============ "NO CARD" AND "I CANNOT READ THE ANSWER" ============
 *
 * A state file written by root with a umask of 0027 is 0640 root:root, and the
 * core that reads it has been ndusr since 0.5.0b. nd_props_parse_lenient()
 * cannot report that -- it returns an empty map for every failure it can have
 * -- so the phone said "No memory card." about a card it had just formatted,
 * and went on saying it until the next boot, because /run is a tmpfs.
 *
 * The mode is fixed at the writer. What is asserted here is that the reader
 * can tell the difference at all, because the next thing to publish that file
 * from an unexpected context will make the same mistake and the phone should
 * say so rather than blame an empty slot.
 *
 * Skipped as root, which ignores the mode bits: there is no way to ask the
 * question then. That is the same limit test_settings.c runs into and it is
 * worth knowing about -- a suite running as root cannot see a permission bug
 * of any kind.
 */
static void test_a_state_file_that_cannot_be_read_is_not_an_absent_card(void)
{
    char resolved[ND_PATH_MAX];
    nd_card card;

    if (geteuid() == 0u)
        return; /* the question cannot be asked; see above */

    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, STATE), ND_OK);
    CHECK_INT(chmod(resolved, 0u), 0);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_UNKNOWN);
    /* Nothing is claimed about a card nothing can be read about -- in
     * particular there is no device for a format to be pointed at. */
    CHECK_STR(card.device, "");
    CHECK_STR(card.untrusted, "");
    /* And every "may I use the card" question still says no, exactly as it
     * did when this was ABSENT. The change is what the phone can SAY, not
     * what it will do. */
    CHECK(nd_storage_is_ready() == false);
    CHECK(nd_storage_media_available() == false);

    /* Left readable so the case root can be torn down. */
    CHECK_INT(chmod(resolved, 0600u), 0);
}

/* And the case that must NOT become UNKNOWN: there is genuinely no card, so
 * there is genuinely no state file. Every phone with an empty slot is here on
 * every boot, and calling that a fault would put a permanent error on the
 * memory-card screen of a phone with nothing wrong with it. */
static void test_a_missing_state_file_is_still_an_absent_card(void)
{
    nd_card card;

    use_scratch_paths();
    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_ABSENT);
}

/* ============ "LOCKED OR DAMAGED" WAS NEITHER ============
 *
 * A card formatted on a PC mounts, reports NEEDS_SETUP, and its root directory
 * is root:root 0755 as mkfs.ext4 left it -- an ext mount carries real numeric
 * ownership and is deliberately given no uid=/gid=. The ndusr core cannot
 * write a byte to it, so "Set up" fails, and the sentence the owner got was
 * "Could not write to the card. It may be locked or damaged."
 *
 * This is the predicate that tells the two apart. It answers for THIS process,
 * which is the only useful answer: the same card is writable to root and not
 * to ndusr, and which of those the phone is has changed under this code once
 * already. */
static void test_the_writability_probe_answers_for_this_process(void)
{
    char resolved[ND_PATH_MAX];

    use_scratch_paths();
    /* No mount point at all: nothing to write into. */
    CHECK(nd_storage_card_is_writable() == false);

    insert_default(FOLDERS, 5u);
    CHECK(nd_storage_card_is_writable() == true);

    if (geteuid() == 0u)
        return; /* root writes into a 0555 directory; see the case above */

    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, MOUNT), ND_OK);
    CHECK_INT(chmod(resolved, 0555u), 0);
    CHECK(nd_storage_card_is_writable() == false);
    /* Traversable but not writable is the exact shape a foreign card arrives
     * in, so the folders cannot be made either -- which is what the screen
     * above this now says out loud. */
    CHECK(nd_storage_setup_folders() == false);
    CHECK_INT(chmod(resolved, 0755u), 0);
}

/* The QEMU convenience path: a host folder, no label, no formatting. */
static void test_a_virtiofs_share_counts_as_a_ready_card(void)
{
    nd_card card;

    use_scratch_paths();
    insert("share", "virtiofs", "", FOLDERS, 5u);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_READY);
    CHECK(card.removable == false);
}

static void test_removable_is_computed_before_the_absent_branch(void)
{
    nd_card card;

    /* Not in the pytest as its own case, but it is the ordering the spec
     * calls out: `removable` is worked out before card() decides to blank
     * everything else, so an absent card still reports removable == true. */
    use_scratch_paths();
    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_ABSENT);
    CHECK(card.removable == true);
    CHECK_STR(card.device, "");
    CHECK_STR(card.fstype, "");
}

static void test_folders_are_only_offered_once_the_card_is_ready(void)
{
    static const char *const dcim[1] = {"DCIM"};
    char out[ND_PATH_MAX];

    use_scratch_paths();
    insert_default(dcim, 1u);

    CHECK(nd_storage_folder("music", out, sizeof out) == false);
}

static void test_folder_returns_the_path_on_a_ready_card(void)
{
    char out[ND_PATH_MAX];

    use_scratch_paths();
    insert_default(FOLDERS, 5u);

    CHECK(nd_storage_folder("music", out, sizeof out) == true);
    CHECK_STR(out, MOUNT "/music");
}

static void test_setting_up_a_card_creates_the_neodct_folders(void)
{
    static const char *const dcim[1] = {"DCIM"};
    nd_card card;
    size_t i;

    use_scratch_paths();
    insert_default(dcim, 1u);

    CHECK(nd_storage_setup_folders() == true);
    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_READY);
    for (i = 0u; i < 5u; i++) {
        char path[ND_PATH_MAX];

        CHECK_INT(nd_snprintf(path, sizeof path, "%s/%s", MOUNT, FOLDERS[i]), ND_OK);
        CHECK(nd_path_is_dir(path));
    }
    /* Existing content is left alone. */
    CHECK(nd_path_is_dir(MOUNT "/DCIM"));
}

/* The pytest points MOUNT_POINT at /proc/nope/sdcard. Under ND_ROOT that is
 * just an empty directory the test could create, so instead a regular FILE
 * stands where the mount point has to be -- which mkdir cannot get past for
 * any user, root included. */
static void test_setting_up_reports_failure_on_a_read_only_card(void)
{
    use_scratch_paths();
    insert_default(NULL, 0u);
    nd_storage_set_paths("/blocked", STATE);
    pt_write_text("/blocked", "I am a file, not a directory");

    CHECK(nd_storage_setup_folders() == false);
}

/* Stock tones ship in the image; the card only adds to them. */
static void test_media_dirs_puts_system_content_first(void)
{
    char dirs[4][ND_STORAGE_PATH_MAX];
    size_t n;

    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    pt_mkdir("/system-tones");

    n = nd_storage_media_dirs("tones", "/system-tones", dirs, 4u);
    CHECK_INT(n, 2);
    CHECK_STR(dirs[0], "/system-tones");
    CHECK_STR(dirs[1], MOUNT "/tones");
}

static void test_media_dirs_skips_directories_that_do_not_exist(void)
{
    char dirs[4][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    CHECK_INT(nd_storage_media_dirs("tones", "/no/such/place", dirs, 4u), 0);
}

static void test_media_dirs_without_a_card_is_just_the_system_dir(void)
{
    char dirs[4][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    pt_mkdir("/system-tones");

    CHECK_INT(nd_storage_media_dirs("tones", "/system-tones", dirs, 4u), 1);
    CHECK_STR(dirs[0], "/system-tones");
}

static void test_finds_an_update_package_on_the_card(void)
{
    char found[8][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    pt_write_text(MOUNT "/update/UPDATE.ndsw", "zip");

    CHECK_INT(nd_storage_find_updates(found, 8u), 1);
    CHECK_STR(found[0], MOUNT "/update/UPDATE.ndsw");
}

static void test_the_conventional_name_is_offered_first(void)
{
    char found[8][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    pt_write_text(MOUNT "/update/older-0.3.1a.ndsw", "zip");
    pt_write_text(MOUNT "/update/UPDATE.ndsw", "zip");

    CHECK_INT(nd_storage_find_updates(found, 8u), 2);
    CHECK_STR(found[0], MOUNT "/update/UPDATE.ndsw");
    CHECK_STR(found[1], MOUNT "/update/older-0.3.1a.ndsw");
}

static void test_remaining_packages_sort_case_insensitively(void)
{
    char found[8][ND_STORAGE_PATH_MAX];

    /* Not in the pytest, but it is the half of the two-pass sort the pytest
     * never exercises: the second key is name.lower(). */
    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    pt_write_text(MOUNT "/update/beta.ndsw", "zip");
    pt_write_text(MOUNT "/update/Alpha.NDSW", "zip");
    pt_write_text(MOUNT "/update/UPDATE.ndsw", "zip");

    CHECK_INT(nd_storage_find_updates(found, 8u), 3);
    CHECK_STR(found[0], MOUNT "/update/UPDATE.ndsw");
    CHECK_STR(found[1], MOUNT "/update/Alpha.NDSW");
    CHECK_STR(found[2], MOUNT "/update/beta.ndsw");
}

static void test_non_package_files_on_the_card_are_ignored(void)
{
    char found[8][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    insert_default(FOLDERS, 5u);
    pt_write_text(MOUNT "/update/readme.txt", "hi");

    CHECK_INT(nd_storage_find_updates(found, 8u), 0);
}

static void test_no_updates_without_a_card(void)
{
    char found[8][ND_STORAGE_PATH_MAX];

    use_scratch_paths();
    CHECK_INT(nd_storage_find_updates(found, 8u), 0);
}

static void test_a_corrupt_state_file_reads_as_no_card(void)
{
    static const unsigned char corrupt[] = {0x00u, 0xffu, 'g', 'a', 'r', 'b', 'a', 'g', 'e'};
    nd_card card;

    use_scratch_paths();
    pt_write(STATE, corrupt, sizeof corrupt);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_ABSENT);
}

/* ------------------------------------------------------------------ *
 * The untrusted directory
 * ------------------------------------------------------------------ *
 *
 * SECURITY-PLAN.md section 1. It was a second FAT32 PARTITION, because a FAT
 * filesystem has no ownership of its own and its mount options apply to the
 * whole of one -- so "downloads are writable by ndusr_ut and the owner's
 * music is not" needed two filesystems to say.
 *
 * The card is one ext4 partition now and it is a DIRECTORY, 0770
 * ndusr:ndusr_ut, sitting beside music/ and apps/ on the same filesystem.
 * Nothing here changes: neodct-sdcard still publishes it as `untrusted=`,
 * this module still passes it through, and a caller with nothing in that
 * field still has to refuse. What changed is that the field is now set for a
 * card with ONE partition on it, which is what the fstype below says.
 */

static void insert_with_untrusted(const char *untrusted)
{
    char body[512];

    CHECK_INT(nd_snprintf(body, sizeof body,
                          "state=mounted\ndevice=/dev/mmcblk1p1\nfstype=ext4\n"
                          "label=NEODCT\nuntrusted=%s\n",
                          untrusted),
              ND_OK);
    pt_write_text(STATE, body);
    pt_mkdir(MOUNT);
}

static void test_a_neodct_card_offers_somewhere_for_a_download(void)
{
    nd_card card;
    char path[ND_PATH_MAX];

    use_scratch_paths();
    insert_with_untrusted(MOUNT "/untrusted");

    nd_storage_card(&card);
    CHECK_STR(card.untrusted, MOUNT "/untrusted");
    CHECK(nd_storage_untrusted_dir(path, sizeof path));
    CHECK_STR(path, MOUNT "/untrusted");
}

static void test_a_foreign_card_offers_nowhere(void)
{
    nd_card card;
    char path[ND_PATH_MAX];

    use_scratch_paths();
    insert_default(FOLDERS, 5u);

    nd_storage_card(&card);
    CHECK_STR(card.untrusted, "");
    /* False, and the caller's answer must be to REFUSE the download rather
     * than fall back to /NeoDCT/User -- which is 8 MiB on the phone and is
     * where the settings, the messages and the call log live. */
    CHECK(!nd_storage_untrusted_dir(path, sizeof path));
}

static void test_a_state_file_from_before_any_of_this_still_reads(void)
{
    nd_card card;

    use_scratch_paths();
    pt_write_text(STATE, "state=mounted\ndevice=/dev/vdc\nfstype=vfat\nlabel=NEODCT\n");
    pt_mkdir(MOUNT);

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_NEEDS_SETUP);
    CHECK_STR(card.untrusted, "");
}

static void test_a_card_that_was_pulled_leaves_no_stale_path(void)
{
    nd_card card;
    char path[ND_PATH_MAX];

    use_scratch_paths();
    pt_write_text(STATE, "state=absent\ndevice=\nfstype=\nlabel=\n"
                         "untrusted=" MOUNT "/untrusted\n");

    nd_storage_card(&card);
    CHECK_INT(card.state, ND_CARD_ABSENT);
    /* A path to a card that is not there is a directory a download would be
     * written into and lost with the card. */
    CHECK_STR(card.untrusted, "");
    CHECK(!nd_storage_untrusted_dir(path, sizeof path));
}

static void test_the_untrusted_directory_is_not_gated_on_the_five_folders(void)
{
    char path[ND_PATH_MAX];

    use_scratch_paths();
    insert_with_untrusted(MOUNT "/untrusted");

    /* No wallpapers/tones/music/backup_db/update, so the card is
     * NEEDS_SETUP -- and the untrusted directory is offered regardless. The
     * five folders are what the MEDIA apps need; a download needs one
     * directory it may write, and whether the owner's music has anywhere to
     * live is a different question with a different answer. */
    CHECK(!nd_storage_is_ready());
    CHECK(nd_storage_untrusted_dir(path, sizeof path));
}

int main(void)
{
    RUN(test_no_card_when_nothing_is_mounted);
    RUN(test_a_neodct_card_is_ready);
    RUN(test_a_plain_fat_card_needs_setting_up);
    RUN(test_a_card_missing_one_folder_needs_setting_up);
    RUN(test_an_unmountable_card_wants_formatting);
    RUN(test_the_other_spelling_means_the_same_thing);
    RUN(test_a_legacy_fat_card_is_its_own_state);
    RUN(test_a_legacy_card_has_nowhere_for_a_download);
    RUN(test_a_card_from_another_computer_is_its_own_state);
    RUN(test_a_foreign_card_hands_out_no_media);
    RUN(test_a_legacy_card_still_hands_out_its_media);
    RUN(test_a_legacy_card_still_offers_its_updates);
    RUN(test_only_a_readable_card_counts_as_media);
    RUN(test_a_state_file_that_cannot_be_read_is_not_an_absent_card);
    RUN(test_a_missing_state_file_is_still_an_absent_card);
    RUN(test_the_writability_probe_answers_for_this_process);
    RUN(test_a_virtiofs_share_counts_as_a_ready_card);
    RUN(test_removable_is_computed_before_the_absent_branch);
    RUN(test_folders_are_only_offered_once_the_card_is_ready);
    RUN(test_folder_returns_the_path_on_a_ready_card);
    RUN(test_setting_up_a_card_creates_the_neodct_folders);
    RUN(test_setting_up_reports_failure_on_a_read_only_card);
    RUN(test_media_dirs_puts_system_content_first);
    RUN(test_media_dirs_skips_directories_that_do_not_exist);
    RUN(test_media_dirs_without_a_card_is_just_the_system_dir);
    RUN(test_finds_an_update_package_on_the_card);
    RUN(test_the_conventional_name_is_offered_first);
    RUN(test_remaining_packages_sort_case_insensitively);
    RUN(test_non_package_files_on_the_card_are_ignored);
    RUN(test_no_updates_without_a_card);
    RUN(test_a_corrupt_state_file_reads_as_no_card);
    RUN(test_a_neodct_card_offers_somewhere_for_a_download);
    RUN(test_a_foreign_card_offers_nowhere);
    RUN(test_a_state_file_from_before_any_of_this_still_reads);
    RUN(test_a_card_that_was_pulled_leaves_no_stale_path);
    RUN(test_the_untrusted_directory_is_not_gated_on_the_five_folders);

    nd_storage_set_paths(NULL, NULL);
    return pt_report("test_storage");
}
