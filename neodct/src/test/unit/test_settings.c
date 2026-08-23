/* test_settings.c -- a case-for-case port of
 * neodct/tests/test_settings_version_layering.py.
 *
 * System facts live in the image; user settings live on the user partition.
 * Before the immutable-rootfs work, system.os.versionnumber was stored in
 * /NeoDCT/User/settings.prop. That only appeared to work because an update
 * replaced the whole filesystem including settings.prop. With /NeoDCT/User on
 * its own partition that file survives an update, so the phone would keep
 * reporting the version it shipped with forever.
 *
 * The pytest monkeypatches SETTINGS_PATH and VERSION_PATH onto a tmp_path;
 * here nd_settings_set_paths() does the same job, and ND_ROOT keeps the whole
 * thing inside a scratch directory.
 *
 * The last two pytests exercise launcher.splash_version(), which is not part
 * of this module. They are ported as far as this module reaches -- the value
 * the splash formats -- and the formatting itself belongs with the launcher.
 */

#include <stdio.h>
#include <string.h>

#include "nd_settings.h"

#include "platform_test.h"

#define SETTINGS "/User/settings.prop"
#define VERSION  "/System/version.prop"

static void use_scratch_paths(void)
{
    nd_settings_set_paths(SETTINGS, VERSION);
}

/* write_version(tmp_path, **values) from the pytest. */
static void write_version(const char *body)
{
    pt_write_text(VERSION, body);
}

static void test_version_prop_supplies_the_installed_version(void)
{
    use_scratch_paths();
    write_version("system.os.versionnumber=0.3.2a\n"
                  "system.os.versionname=NeoDCT System v0.3.2a\n"
                  "system.os.platform=qemu-aarch64\n"
                  "system.os.buildtime=1785160800\n");

    CHECK_STR(nd_settings_get(ND_SET_OS_VERSIONNUMBER, NULL), "0.3.2a");
    CHECK_STR(nd_settings_get(ND_SET_OS_PLATFORM, NULL), "qemu-aarch64");
}

/* The exact bug this split exists to prevent. */
static void test_version_prop_wins_over_a_stale_user_settings_file(void)
{
    use_scratch_paths();
    pt_write_text(SETTINGS, "system.os.versionnumber=0.3.0a\n"
                            "system.ui.wallpaper=/some/where.jpg\n");
    write_version("system.os.versionnumber=0.3.2a\n");

    CHECK_STR(nd_settings_get(ND_SET_OS_VERSIONNUMBER, NULL), "0.3.2a");
}

static void test_user_settings_are_untouched_by_the_split(void)
{
    use_scratch_paths();
    write_version("system.os.versionnumber=0.3.2a\n");

    CHECK_INT(nd_settings_set(ND_SET_UI_WALLPAPER, "/NeoDCT/User/w.jpg"), ND_OK);
    CHECK_STR(nd_settings_get(ND_SET_UI_WALLPAPER, NULL), "/NeoDCT/User/w.jpg");
}

/* Otherwise the image facts would go stale again the moment they were
 * persisted -- which is the whole failure this file is about. */
static void test_system_facts_are_never_written_into_settings_prop(void)
{
    char body[1024];

    use_scratch_paths();
    write_version("system.os.versionnumber=0.3.2a\n");

    CHECK_INT(nd_settings_set(ND_SET_UI_WALLPAPER, "NONE"), ND_OK);
    CHECK(pt_read_text(SETTINGS, body, sizeof body) != (size_t)-1);
    CHECK(strstr(body, "system.os.") == NULL);
}

static void test_stale_system_keys_are_dropped_from_an_existing_settings_file(void)
{
    char body[1024];

    use_scratch_paths();
    pt_write_text(SETTINGS, "system.os.versionnumber=0.1.0a\n"
                            "system.ui.engineering_mode=OFF\n");
    write_version("system.os.versionnumber=0.3.2a\n");

    CHECK_INT(nd_settings_set(ND_SET_UI_WALLPAPER, "NONE"), ND_OK);
    CHECK(pt_read_text(SETTINGS, body, sizeof body) != (size_t)-1);
    CHECK(strstr(body, "0.1.0a") == NULL);
    CHECK(strstr(body, "system.ui.engineering_mode=OFF") != NULL);
}

/* Host tests and pre-split images have no version.prop; do not crash. */
static void test_falls_back_to_defaults_when_version_prop_is_missing(void)
{
    use_scratch_paths();

    CHECK_STR(nd_settings_get(ND_SET_OS_VERSIONNUMBER, NULL), ND_SET_OS_VERSIONNUMBER_DFLT);
}

static void test_a_corrupt_version_prop_does_not_break_settings(void)
{
    static const unsigned char corrupt[] = {0x00u, 0xffu, ' ', 'n', 'o', 't', ' ', 'a', ' ',
                                            'p',   'r',   'o', 'p', ' ', 'f', 'i', 'l', 'e'};

    use_scratch_paths();
    pt_write(VERSION, corrupt, sizeof corrupt);

    CHECK_STR(nd_settings_get(ND_SET_UI_ENGINEERING, NULL), "ON");
}

/* SystemUpdate refuses packages built for other hardware using this. */
static void test_platform_is_available_for_update_compatibility_checks(void)
{
    use_scratch_paths();
    write_version("system.os.platform=luckfox-armv7\n");

    CHECK_STR(nd_settings_get(ND_SET_OS_PLATFORM, NULL), "luckfox-armv7");
}

/* If the user partition is missing, the UI must still boot.
 *
 * The pytest points SETTINGS_PATH at /proc/definitely/not/writable/. That
 * cannot be reused verbatim: under ND_ROOT it would resolve to an ordinary
 * empty directory the test could happily create. A regular FILE standing
 * where a directory has to be is unwritable for every user including root,
 * which is what makes this reproduce the pytest's intent rather than its
 * spelling. */
static void test_a_read_only_settings_path_still_reads(void)
{
    nd_settings_set_paths("/blocked/settings.prop", VERSION);
    pt_write_text("/blocked", "I am a file, not a directory");
    write_version("system.os.versionnumber=0.3.2a\n");

    CHECK(!nd_path_is_dir("/blocked"));
    CHECK_STR(nd_settings_get(ND_SET_OS_VERSIONNUMBER, NULL), "0.3.2a");
    CHECK_STR(nd_settings_get(ND_SET_UI_ENGINEERING, NULL), "ON");
    CHECK(!nd_path_exists("/blocked/settings.prop"));
}

/* The boot splash had the number typed into it, so it drifted a release
 * behind the moment anything else was bumped. splash_version() itself lives
 * in the launcher; what this module owes it is the number. */
static void test_the_boot_splash_reads_the_version_out_of_the_image(void)
{
    char splash[64];

    use_scratch_paths();
    write_version("system.os.versionnumber=9.9.9z\n");

    CHECK_INT(nd_snprintf(splash, sizeof splash, "System v%s",
                          nd_settings_get(ND_SET_OS_VERSIONNUMBER, "")),
              ND_OK);
    CHECK_STR(splash, "System v9.9.9z");
}

/* An image with no version.prop is broken, but the splash is the last place
 * that should be the thing to crash. */
static void test_the_splash_still_says_something_with_no_version_prop(void)
{
    char splash[64];

    use_scratch_paths();

    CHECK_INT(nd_snprintf(splash, sizeof splash, "System v%s",
                          nd_settings_get(ND_SET_OS_VERSIONNUMBER, "")),
              ND_OK);
    CHECK(strncmp(splash, "System v", 8u) == 0);
    CHECK(strlen(splash) > 8u);
}

/* ------------------------------------------------------------------ *
 * Beyond the pytest: the parts of the module it does not reach
 * ------------------------------------------------------------------ */

/* R-24 / C-5. DEFAULTS holds three system.os.* keys, the writer strips
 * exactly those, so "missing" is permanently true and every read rewrites the
 * file. This asserts the CURRENT behaviour, and is the test that has to change
 * when the approved one-line fix lands in nd_settings_flush_if_needed(). */
static void test_every_read_rewrites_settings_prop(void)
{
    struct stat before;
    struct stat after;
    char resolved[ND_PATH_MAX];

    use_scratch_paths();
    write_version("system.os.versionnumber=0.3.2a\n");

    /* The file does not exist yet, so the first read must create it. */
    CHECK(!nd_path_exists(SETTINGS));
    (void)nd_settings_get(ND_SET_UI_WALLPAPER, "NONE");
    CHECK(nd_path_exists(SETTINGS));

    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, SETTINGS), ND_OK);
    CHECK_INT(stat(resolved, &before), 0);
    /* Truncating the file and reading again must put it back: that is the
     * observable consequence of the write-on-read branch firing. */
    pt_write_text(SETTINGS, "");
    (void)nd_settings_get(ND_SET_UI_WALLPAPER, "NONE");
    CHECK_INT(stat(resolved, &after), 0);
    CHECK(after.st_size == before.st_size);
    CHECK(after.st_size > 0);
}

static void test_effective_map_is_layered_lowest_to_highest(void)
{
    nd_props *eff;

    use_scratch_paths();
    pt_write_text(SETTINGS, "system.ui.wallpaper=/user/pick.jpg\n"
                            "games.snake.topscore=42\n");
    write_version("system.os.versionname=NeoDCT System v9.9.9z\n");

    eff = nd_settings_effective();
    CHECK(eff != NULL);
    /* DEFAULTS, untouched by either file */
    CHECK_STR(nd_props_get(eff, ND_SET_AUDIO_RINGTONE, "?"), ND_SET_AUDIO_RINGTOME_DFLT);
    /* settings.prop beats DEFAULTS */
    CHECK_STR(nd_props_get(eff, ND_SET_UI_WALLPAPER, "?"), "/user/pick.jpg");
    /* an app-owned key with no default at all */
    CHECK_STR(nd_props_get(eff, ND_SET_GAMES_SNAKE_TOPSCORE, "?"), "42");
    /* version.prop beats both */
    CHECK_STR(nd_props_get(eff, ND_SET_OS_VERSIONNAME, "?"), "NeoDCT System v9.9.9z");
    nd_props_free(eff);
}

static void test_get_copy_and_absent_keys(void)
{
    char buf[8];

    use_scratch_paths();

    /* An absent key returns the caller's default pointer unchanged, which is
     * what lets a call site pass a literal and compare pointers if it likes. */
    CHECK(nd_settings_get("no.such.key", NULL) == NULL);
    CHECK_STR(nd_settings_get("no.such.key", "fallback"), "fallback");

    CHECK_INT(nd_settings_get_copy(ND_SET_UI_WALLPAPER, "?", buf, sizeof buf), ND_OK);
    CHECK_STR(buf, "NONE");
    /* Truncation is reported, not hidden. */
    CHECK_INT(nd_settings_get_copy(ND_SET_OS_VERSIONNAME, "?", buf, sizeof buf), ND_ERR_TOOLONG);
}

/* Three boolean parsers, three different answers for the same input. */
static void test_the_three_boolean_parsers_disagree(void)
{
    CHECK(nd_setting_is_enabled("ON", false));
    CHECK(nd_setting_is_enabled(" YeS ", false));
    CHECK(nd_setting_is_enabled("enabled", false));
    CHECK(!nd_setting_is_enabled("disabled", true));
    CHECK(!nd_setting_is_enabled("off", true));
    /* Unrecognised falls back to the caller's default -- both ways. */
    CHECK(nd_setting_is_enabled("banana", true));
    CHECK(!nd_setting_is_enabled("banana", false));
    CHECK(nd_setting_is_enabled(NULL, true));

    CHECK(nd_setting_modem_truthy("on"));
    CHECK(nd_setting_modem_truthy(" TRUE "));
    /* ...and here is the disagreement: "enabled" is true for Settings and
     * false for the modem, because the modem's list does not contain it. */
    CHECK(!nd_setting_modem_truthy("enabled"));
    CHECK(!nd_setting_modem_truthy("banana"));
    CHECK(!nd_setting_modem_truthy(NULL));

    CHECK(nd_setting_update_truthy(" on ", "ON"));
    CHECK(!nd_setting_update_truthy("on", "YES"));
    CHECK(!nd_setting_update_truthy(NULL, "ON"));
}

int main(void)
{
    CHECK_INT(nd_settings_init(), ND_OK);

    RUN(test_version_prop_supplies_the_installed_version);
    RUN(test_version_prop_wins_over_a_stale_user_settings_file);
    RUN(test_user_settings_are_untouched_by_the_split);
    RUN(test_system_facts_are_never_written_into_settings_prop);
    RUN(test_stale_system_keys_are_dropped_from_an_existing_settings_file);
    RUN(test_falls_back_to_defaults_when_version_prop_is_missing);
    RUN(test_a_corrupt_version_prop_does_not_break_settings);
    RUN(test_platform_is_available_for_update_compatibility_checks);
    RUN(test_a_read_only_settings_path_still_reads);
    RUN(test_the_boot_splash_reads_the_version_out_of_the_image);
    RUN(test_the_splash_still_says_something_with_no_version_prop);

    RUN(test_every_read_rewrites_settings_prop);
    RUN(test_effective_map_is_layered_lowest_to_highest);
    RUN(test_get_copy_and_absent_keys);
    RUN(test_the_three_boolean_parsers_disagree);

    nd_settings_set_paths(NULL, NULL);
    return pt_report("test_settings");
}
