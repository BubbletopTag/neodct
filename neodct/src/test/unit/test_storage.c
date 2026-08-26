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

int main(void)
{
    RUN(test_no_card_when_nothing_is_mounted);
    RUN(test_a_neodct_card_is_ready);
    RUN(test_a_plain_fat_card_needs_setting_up);
    RUN(test_a_card_missing_one_folder_needs_setting_up);
    RUN(test_an_unmountable_card_wants_formatting);
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

    nd_storage_set_paths(NULL, NULL);
    return pt_report("test_storage");
}
