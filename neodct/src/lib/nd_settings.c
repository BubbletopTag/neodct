/* nd_settings.c -- SettingsStorage, layered three deep.
 *
 *     DEFAULTS  <  /NeoDCT/User/settings.prop  <  /NeoDCT/System/version.prop
 *
 * version.prop wins because it describes the IMAGE. Before the split, the
 * version number lived in settings.prop, which survives an update on its own
 * partition -- so an updated phone kept reporting the version it shipped
 * with, forever. test_settings_version_layering.py exists for that bug and
 * this file is the port of the module it tests.
 *
 * ============ THE WRITE-ON-EVERY-READ QUIRK (R-24) ============
 *
 * DEFAULTS holds three "system.os.*" keys and the writer strips exactly those
 * before saving, so they are never in the stored file, so the "missing keys"
 * branch of load_with_defaults() is permanently true, so every get_setting()
 * rewrites settings.prop -- temp file, fsync, rename. Measured: five
 * consecutive reads produced five full rewrites.
 *
 * That is what the phone does today and it is reproduced here, but it is
 * reproduced in exactly one place -- nd_settings_flush_if_needed() -- so that
 * the approved fix (C-5 in OPEN-QUESTIONS.md: skip the write when the bytes
 * are unchanged) is a change to one function and not an audit of every reader.
 *
 * ============ THE KEY LIST ============
 *
 * Every settings key in the whole overlay, from a grep of get_setting( and
 * set_setting(. The eight in DEFAULTS are the table below; the rest are read
 * with a call-site default and are declared in nd_settings.h:
 *
 *   system.audio.ringtone         DEFAULTS   NotifyService      Tones app
 *   system.ui.wallpaper           DEFAULTS   core              Settings app
 *   system.ui.engineering_mode    DEFAULTS   core, Settings,   Settings app
 *                                            Update
 *   system.os.versionnumber       DEFAULTS   launcher, Settings, Update,
 *                                            Downgrade          never written
 *   system.os.versionname         DEFAULTS   Settings           never written
 *   system.os.platform            DEFAULTS   Update, Downgrade  never written
 *   system.hw.battery_i2c_bus     DEFAULTS   BatteryService     never written
 *   system.hw.battery_i2c_addr    DEFAULTS   BatteryService     never written
 *   system.os.buildtime           version.prop only, Settings
 *   system.os.buildepoch          version.prop only, ClockService reads the
 *                                 FILE directly, not through get_setting
 *   system.hw.modem_at_port       ModemService  ("AUTO")
 *   system.hw.modem_pcm_rate      ModemService  ("16000")
 *   system.hw.modem_pcm_port      ModemService  ("AUTO")
 *   system.hw.modem_mic_device    ModemService  ("AUTO")
 *   system.modem.allow_calls      ModemService  ("ON")
 *   calllog.duration.last         CallLog app   ("0")   read and written
 *   calllog.duration.received     CallLog app   ("0")   read and written
 *   calllog.duration.dialed       CallLog app   ("0")   read and written
 *   games.snake.level             Games app     (5)     read and written
 *   games.snake.topscore          Games app     (0)     read and written
 *   games.memory.topscore         Games app     (0)     read and written
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_log.h"
#include "nd_paths.h"
#include <sys/stat.h>
#include "nd_settings.h"

/* The tag the Python printed: print(f"[Settings] Failed to ...") */
#define SETTINGS_TAG "Settings"

typedef struct {
    const char *key;
    const char *value;
} default_entry;

/* SettingsStorage.DEFAULTS, verbatim and in source order. The order does not
 * reach the file -- the writer sorts -- but keeping it makes the two readable
 * side by side. */
static const default_entry DEFAULTS[] = {
    {ND_SET_AUDIO_RINGTONE, ND_SET_AUDIO_RINGTOME_DFLT},
    {ND_SET_UI_WALLPAPER, ND_SET_UI_WALLPAPER_DFLT},
    {ND_SET_UI_ENGINEERING, ND_SET_UI_ENG_MODE_DFLT},
    {ND_SET_OS_VERSIONNUMBER, ND_SET_OS_VERSIONNUMBER_DFLT},
    {ND_SET_OS_VERSIONNAME, ND_SET_OS_VERSIONNAME_DFLT},
    {ND_SET_OS_PLATFORM, ND_SET_OS_PLATFORM_DFLT},
    {ND_SET_HW_BATT_I2C_BUS, ND_SET_HW_BATT_I2C_BUS_DFLT},
    {ND_SET_HW_BATT_I2C_ADDR, ND_SET_HW_BATT_I2C_ADDR_DFLT},
};

/* Overridable so the host tests can point at a scratch directory, which is
 * what test_settings_version_layering.py does by monkeypatching the module
 * constants. Both are interpreted in the same virtual namespace as every
 * other path in the system, i.e. they still pass through ND_ROOT. */
static char g_settings_path[ND_PATH_MAX] = ND_PATH_SETTINGS_PROP;
static char g_version_path[ND_PATH_MAX] = ND_PATH_VERSION_PROP;

/* nd_settings_get() hands back a pointer with a documented lifetime of "until
 * the next call", which is this. A caller that wants to keep the string uses
 * nd_settings_get_copy(). */
static char g_value_buf[ND_PROP_VALUE_MAX];

void nd_settings_set_paths(const char *settings_path, const char *version_path)
{
    (void)nd_strlcpy(g_settings_path, settings_path != NULL ? settings_path : ND_PATH_SETTINGS_PROP,
                     sizeof g_settings_path);
    (void)nd_strlcpy(g_version_path, version_path != NULL ? version_path : ND_PATH_VERSION_PROP,
                     sizeof g_version_path);
}

nd_err nd_settings_init(void)
{
    /* The DEFAULTS table is static const, so there is nothing to build. The
     * function exists because nd_ui construction calls it and because a
     * future defaults source (a per-platform overlay, say) would need it. */
    return ND_OK;
}

/* load_settings(): missing file is {} in silence; an unreadable one is {} with
 * a line on the serial log, matching the Python's two branches. */
static nd_props *load_settings(void)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_exists(g_settings_path) &&
        nd_path_resolve(resolved, sizeof resolved, g_settings_path) == ND_OK &&
        access(resolved, R_OK) != 0)
        nd_log(SETTINGS_TAG, "Failed to read %s: %s", g_settings_path, strerror(errno));

    return nd_props_parse_settings(g_settings_path);
}

/* load_version(): any failure is {}, silently. An image with no version.prop
 * is broken, but the boot splash is the last thing that should be what
 * crashes over it. */
static nd_props *load_version(void)
{
    return nd_props_parse_settings(g_version_path);
}

/* The RESOLVED directory settings.prop lives in, which is what both guards
 * below actually ask their question about: nd_props_write_atomic() writes a
 * temp file beside the target and renames it, so every property of the write
 * -- who will own the result, and whether it can be created at all -- is a
 * property of the directory rather than of the file. */
static bool settings_dir(char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    const char *slash;
    size_t dir_len;

    if (nd_path_resolve(resolved, sizeof resolved, g_settings_path) != ND_OK)
        return false;
    slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved)
        return false;
    dir_len = (size_t)(slash - resolved);
    resolved[dir_len] = '\0';
    return nd_strlcpy(out, resolved, out_sz) < out_sz;
}

/* ============ WHY ROOT DOES NOT WRITE THIS FILE ============
 *
 * nd-core is root for about a second at boot -- from exec until it becomes
 * ndusr (core/nd_main.c step 4b) -- and in that window it starts the clock
 * service and the remote shell, BOTH of which read a setting. Every read
 * rewrites the file, because `missing` in nd_settings_load() is always true
 * (R-24, and deliberate).
 *
 * So on a phone whose user partition is FRESH -- a new phone, or one whose
 * data was wiped -- root created /NeoDCT/User/settings.prop owned root:root
 * 0640, from run_neodct.sh's umask. nd-core then dropped to ndusr and could
 * never read its own settings file again:
 *
 *     [Settings] Failed to read /NeoDCT/User/settings.prop: Permission denied
 *
 * which is every preference on the phone silently falling back to its default
 * and refusing to stick. It self-heals on the SECOND boot, because S00userdata
 * then finds the file and chowns it -- which is precisely why it survived:
 * a developer who reboots constantly never sees it, and a person opening a new
 * phone sees nothing else.
 *
 * Found by booting the thing, not by reading it. The unit tests all run as one
 * user and cannot express "and then the process became somebody else".
 *
 * The condition is not a bare `geteuid() == 0`. An image built without the
 * users table has no ndusr, nd-core stays root for its whole life and the
 * partition is root's -- there, root writing this file is correct and the only
 * thing that will ever write it. So the question asked is the exact one that
 * matters: am I about to create a file that the user who owns this directory
 * will not be able to read?
 */
static bool root_would_orphan_the_file(void)
{
    char dir[ND_PATH_MAX];
    struct stat st;

    if (geteuid() != 0)
        return false;

    if (!settings_dir(dir, sizeof dir))
        return false;

    if (stat(dir, &st) != 0)
        return false;
    /* Somebody else owns it, and that somebody is who nd-core is about to
     * become. Leave the file to them. */
    return st.st_uid != 0u;
}

/* ============ WHY THE WRITE IS ASKED FOR PERMISSION FIRST ============
 *
 * R-24 (top of this file) makes the rewrite fire on EVERY read, and every
 * process on the phone reads a setting -- including every untrusted app, which
 * runs as ndusr_ut and cannot create a file in /NeoDCT/User at all. So each
 * such app printed two lines on its way up: the "Failed to read" from
 * load_settings(), and then a "Failed to write" from a rewrite that never had
 * any chance of landing. The second says nothing the first did not, and it
 * says it about an operation the process was never supposed to perform.
 *
 * Two lines per app launch is not a cosmetic problem here. core.log is what
 * the owner reads to debug everything else, it lives on an 8 MiB partition,
 * and a browser session that starts a helper per page turns this into the
 * bulk of the file.
 *
 * access(2) rather than "try it and see", because trying it IS the log line.
 * W_OK|X_OK for the reason nd_storage_card_is_writable() asks for both: a
 * directory that cannot be entered cannot be written into either. It answers
 * for the REAL uid, which for every process here is the effective one, and it
 * is advisory -- the partition can go read-only between the question and the
 * write, and then the write fails and IS logged, which is right.
 *
 * ============ A DIRECTORY THAT IS NOT THERE IS NOT A REFUSAL ============
 *
 * nd_props_write_atomic() mkdir -p's the directory itself before writing into
 * it, so "it does not exist" is a case that SUCCEEDS today and has to keep
 * succeeding. A guard that treated a missing directory as unwritable would
 * quietly stop every preference on a phone whose user partition is empty from
 * ever being saved -- the exact fault this file already carries one long
 * comment about, reintroduced by the fix for its log spam.
 *
 * So the question is narrower than "can I write here": it is "is there a
 * directory here that I am not allowed to write to". Only that answers yes. */
static bool settings_dir_blocks_the_write(void)
{
    char dir[ND_PATH_MAX];
    struct stat st;

    if (!settings_dir(dir, sizeof dir))
        return false;
    /* stat() and access() on the RESOLVED path, never nd_path_is_dir(): the
     * string is already through nd_path_resolve() and putting it through again
     * would prepend a test root that is already in it. */
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return access(dir, W_OK | X_OK) != 0;
}

/* save_settings(): drop every system.os.* key, then write atomically. All
 * failures are swallowed -- an unwritable user partition must not stop the
 * phone booting, it just means preferences do not stick. */
static void save_settings(const nd_props *settings)
{
    nd_props *out;
    size_t i;
    nd_err rc;

    /* Both refusals come BEFORE the allocation. They used to come after it,
     * and the orphan guard's `return` then dropped a whole nd_props on the
     * floor every time root declined to write -- once per setting read, on
     * every boot of a phone with a fresh user partition. */
    if (root_would_orphan_the_file())
        return;
    if (settings_dir_blocks_the_write())
        return;

    /* owned here; freed on every path out of this function */
    out = nd_props_new();
    if (out == NULL)
        return;

    for (i = 0u; i < nd_props_count(settings); i++) {
        const char *key = nd_props_key_at(settings, i);

        if (strncmp(key, ND_SETTINGS_SYSTEM_PREFIX, strlen(ND_SETTINGS_SYSTEM_PREFIX)) == 0)
            continue;
        if (nd_props_set(out, key, nd_props_value_at(settings, i)) != ND_OK)
            goto done;
    }

    rc = nd_props_write_atomic(g_settings_path, out, true);
    if (rc != ND_OK)
        nd_log(SETTINGS_TAG, "Failed to write %s: %s", g_settings_path, nd_strerror(rc));

done:
    nd_props_free(out);
}

nd_err nd_settings_flush_if_needed(nd_props *effective, const nd_props *stored)
{
    bool stale = false;
    bool missing = false;
    size_t i;

    if (effective == NULL)
        return ND_ERR_INVAL;

    /* stale: a system.os.* key that somehow got persisted by an older build.
     * Dropping it is the whole point of the settings/version split. */
    for (i = 0u; i < nd_props_count(stored); i++) {
        if (strncmp(nd_props_key_at(stored, i), ND_SETTINGS_SYSTEM_PREFIX,
                    strlen(ND_SETTINGS_SYSTEM_PREFIX)) == 0) {
            stale = true;
            break;
        }
    }

    /* missing: a DEFAULTS key not present in the stored file. Three of the
     * eight are system.os.* and are stripped by the writer, so this is
     * ALWAYS true -- that is R-24, and it is deliberate here. */
    for (i = 0u; i < ND_ARRAY_LEN(DEFAULTS); i++) {
        if (!nd_props_has(stored, DEFAULTS[i].key)) {
            missing = true;
            break;
        }
    }

    if (stale || missing || !nd_path_exists(g_settings_path))
        save_settings(effective);

    return ND_OK;
}

nd_props *nd_settings_effective(void)
{
    nd_props *stored = NULL;
    nd_props *version = NULL;
    nd_props *merged = NULL;
    size_t i;

    /* owned by the caller; free with nd_props_free() */
    merged = nd_props_new();
    if (merged == NULL)
        goto fail;

    for (i = 0u; i < ND_ARRAY_LEN(DEFAULTS); i++) {
        if (nd_props_set(merged, DEFAULTS[i].key, DEFAULTS[i].value) != ND_OK)
            goto fail;
    }

    stored = load_settings();
    if (stored == NULL)
        goto fail;
    if (nd_props_update(merged, stored) != ND_OK)
        goto fail;

    /* Image facts always win, so an update is visible immediately even though
     * settings.prop outlived it. */
    version = load_version();
    if (version == NULL)
        goto fail;
    if (nd_props_update(merged, version) != ND_OK)
        goto fail;

    (void)nd_settings_flush_if_needed(merged, stored);

    nd_props_free(stored);
    nd_props_free(version);
    return merged;

fail:
    nd_props_free(stored);
    nd_props_free(version);
    nd_props_free(merged);
    return NULL;
}

const char *nd_settings_get(const char *key, const char *dflt)
{
    nd_props *eff = nd_settings_effective();
    const char *found;

    if (eff == NULL)
        return dflt;

    found = nd_props_get(eff, key, NULL);
    if (found == NULL) {
        nd_props_free(eff);
        return dflt;
    }

    (void)nd_strlcpy(g_value_buf, found, sizeof g_value_buf);
    nd_props_free(eff);
    return g_value_buf;
}

nd_err nd_settings_get_copy(const char *key, const char *dflt, char *out, size_t out_sz)
{
    nd_props *eff;
    const char *found;
    size_t n;

    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;

    eff = nd_settings_effective();
    found = eff != NULL ? nd_props_get(eff, key, NULL) : NULL;
    if (found == NULL)
        found = dflt;
    if (found == NULL) {
        out[0] = '\0';
        nd_props_free(eff);
        return ND_ERR_NOTFOUND;
    }

    n = nd_strlcpy(out, found, out_sz);
    nd_props_free(eff);
    return n >= out_sz ? ND_ERR_TOOLONG : ND_OK;
}

nd_err nd_settings_set(const char *key, const char *value)
{
    nd_props *eff;
    nd_err rc;

    if (key == NULL || value == NULL)
        return ND_ERR_INVAL;

    eff = nd_settings_effective();
    if (eff == NULL)
        return ND_ERR_NOMEM;

    /* system.os.* is accepted here and then stripped by the writer, exactly
     * as in the Python -- set_setting() has no idea the prefix is special. */
    rc = nd_props_set(eff, key, value);
    if (rc == ND_OK)
        save_settings(eff);

    nd_props_free(eff);
    return rc;
}

/* ------------------------------------------------------------------ *
 * The three boolean parsers. Yes, three. They disagree.
 * ------------------------------------------------------------------ */

/* Strips and lowercases into a small fixed buffer. Anything longer than the
 * buffer cannot match one of the literals anyway. */
static void fold(const char *value, char *out, size_t out_sz, bool upper)
{
    size_t start = 0u;
    size_t end;
    size_t i;
    size_t w = 0u;

    out[0] = '\0';
    if (value == NULL)
        return;

    end = strlen(value);
    while (start < end && isspace((unsigned char)value[start]))
        start++;
    while (end > start && isspace((unsigned char)value[end - 1u]))
        end--;

    for (i = start; i < end && w + 1u < out_sz; i++) {
        int c = (unsigned char)value[i];

        out[w++] = (char)(upper ? toupper(c) : tolower(c));
    }
    out[w] = '\0';
}

static bool in_list(const char *needle, const char *const *list, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(needle, list[i]) == 0)
            return true;
    }
    return false;
}

bool nd_setting_is_enabled(const char *value, bool dflt)
{
    static const char *const yes[] = {"1", "true", "on", "yes", "enabled"};
    static const char *const no[] = {"0", "false", "off", "no", "disabled"};
    char buf[32];

    if (value == NULL)
        return dflt;

    fold(value, buf, sizeof buf, false);
    if (in_list(buf, yes, ND_ARRAY_LEN(yes)))
        return true;
    if (in_list(buf, no, ND_ARRAY_LEN(no)))
        return false;

    /* An unrecognised value is neither -- it falls back to the caller's
     * default. This is the only one of the three that has a default at all. */
    return dflt;
}

bool nd_setting_modem_truthy(const char *value)
{
    static const char *const yes[] = {"ON", "1", "TRUE", "YES"};
    char buf[32];

    if (value == NULL)
        return false;

    fold(value, buf, sizeof buf, true);
    return in_list(buf, yes, ND_ARRAY_LEN(yes));
}

bool nd_setting_update_truthy(const char *value, const char *literal_upper)
{
    char buf[64];

    if (value == NULL || literal_upper == NULL)
        return false;

    fold(value, buf, sizeof buf, true);
    return strcmp(buf, literal_upper) == 0;
}
