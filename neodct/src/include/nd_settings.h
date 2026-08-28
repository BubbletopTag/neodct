/* nd_settings.h -- the phone's preferences, layered three deep.
 *
 * Precedence, lowest to highest:
 *
 *     DEFAULTS  <  /NeoDCT/User/settings.prop  <  /NeoDCT/System/version.prop
 *
 * version.prop wins because it describes the IMAGE -- version number, build
 * time, platform -- and a user preference must never be able to lie about
 * which build is running.
 *
 * ============ THE WRITE-ON-EVERY-READ QUIRK (R-24, measured) ============
 *
 * DEFAULTS contains three "system.os.*" keys. save_settings() strips exactly
 * those before writing. So they are never present in the stored file, so the
 * "missing keys" test in load_with_defaults() is permanently true, so
 * EVERY get_setting() CALL REWRITES settings.prop -- temp file, fsync, rename.
 * Five consecutive get_setting() calls were measured producing five full
 * rewrites.
 *
 * ModemService, NotifyService and BatteryService all call get_setting() from
 * hot paths. On UBIFS/NAND this is real flash wear. It is nonetheless what the
 * phone does today, so it is what the port does today: implement it exactly,
 * but keep the write behind nd_settings_flush_if_needed() so that switching to
 * "write only when the content changed" is a one-line change once
 * OPEN-QUESTIONS.md is answered.
 *
 * ============ BOOLEANS ARE PARSED THREE DIFFERENT WAYS ============
 *
 * Do not introduce one shared nd_setting_bool(). See the three functions at
 * the bottom of this header; each has exactly one caller family and they
 * disagree about what an unrecognised value means.
 */

#ifndef ND_SETTINGS_H_INCLUDED
#define ND_SETTINGS_H_INCLUDED

#include "nd_props.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Anything under this prefix describes the image, never a user preference,
 * and is stripped before settings.prop is written. */
#define ND_SETTINGS_SYSTEM_PREFIX "system.os."

/* ---- SettingsStorage.DEFAULTS, verbatim ---- */
#define ND_SET_AUDIO_RINGTOME_DFLT   "/NeoDCT/System/tones/Low.mp3"
#define ND_SET_UI_WALLPAPER_DFLT     "NONE"
#define ND_SET_UI_ENG_MODE_DFLT      "ON"
#define ND_SET_UI_WP_EVERYWHERE_DFLT "ON"
#define ND_SET_UI_WP_APP_DIM_DFLT    "0.75"
#define ND_SET_OS_VERSIONNUMBER_DFLT "0.3.1a"
#define ND_SET_OS_VERSIONNAME_DFLT   "NeoDCT System v0.3.1a"
#define ND_SET_OS_PLATFORM_DFLT      "unknown"
#define ND_SET_HW_BATT_I2C_BUS_DFLT  "3"
#define ND_SET_HW_BATT_I2C_ADDR_DFLT "0x36"

/* ---- every key the overlay reads or writes, grepped ---- */

/* In DEFAULTS */
#define ND_SET_AUDIO_RINGTONE   "system.audio.ringtone"
#define ND_SET_UI_WALLPAPER     "system.ui.wallpaper"
#define ND_SET_UI_ENGINEERING   "system.ui.engineering_mode"
#define ND_SET_OS_VERSIONNUMBER "system.os.versionnumber"
#define ND_SET_OS_VERSIONNAME   "system.os.versionname"
#define ND_SET_OS_PLATFORM      "system.os.platform"
#define ND_SET_HW_BATT_I2C_BUS  "system.hw.battery_i2c_bus"
#define ND_SET_HW_BATT_I2C_ADDR "system.hw.battery_i2c_addr"

/* Only ever in version.prop */
#define ND_SET_OS_BUILDTIME  "system.os.buildtime"
#define ND_SET_OS_BUILDEPOCH "system.os.buildepoch"

/* Read with a call-site default, never in DEFAULTS */
#define ND_SET_HW_MODEM_AT_PORT  "system.hw.modem_at_port"    /* "AUTO"    */
#define ND_SET_HW_MODEM_PCM_RATE "system.hw.modem_pcm_rate"   /* "16000"   */
#define ND_SET_HW_MODEM_PCM_PORT "system.hw.modem_pcm_port"   /* "AUTO"    */
#define ND_SET_HW_MODEM_MIC_DEV  "system.hw.modem_mic_device" /* "AUTO"    */
#define ND_SET_MODEM_ALLOW_CALLS "system.modem.allow_calls"   /* "ON"      */

/* Wallpaper behind the framework's own chrome -- lists, dialogs, text boxes,
 * every screen that used to be flat black. ON by default, because that is the
 * point of the feature; a phone whose owner wants the old look turns it off
 * here and nothing else changes.
 *
 * NOT in DEFAULTS, deliberately. A key in that table is written into
 * settings.prop on the first read, and these two are read from inside the
 * render path -- see the R-24 quirk above, which would turn every frame into
 * a flash write. Read them with nd_settings_get(key, dflt) instead, which
 * touches nothing.
 *
 * There is also deliberately no Settings screen for either. They are taste,
 * not policy, and the wallpaper picker is already where a person goes to
 * change how the phone looks.
 *
 * ND_SET_UI_WP_APP_DIM is a SECOND brightness multiplier applied on top of
 * the 0.3 the home screen already uses, so 0.45 here means chrome sits on
 * 0.3 * 0.45 = 0.135 of the original picture. Chrome carries text at every
 * size the phone has; the home screen carries a clock. Parsed with strtod and
 * clamped to [0, 1]; anything unparseable reads as the default rather than
 * blanking the screen. */
#define ND_SET_UI_WP_EVERYWHERE "system.ui.wpeverywhere"     /* "ON"      */
#define ND_SET_UI_WP_APP_DIM    "system.ui.wpeverywhere_dim" /* "0.45"   */

/* NTP sync, owned by the Clock app and read by the clock service at boot.
 * Defaults to ON: a phone whose clock is wrong fails every TLS "not valid
 * before" check, so the useful default is the one that fixes itself.
 *
 * Turning it OFF stops the BACKGROUND SYNC ONLY. The boot floor still
 * applies -- see nd_clock.h -- because a clock stuck in 1970 breaks the
 * update system's signature check, and "I set my own time" is not a request
 * to let that happen. */
#define ND_SET_CLOCK_NTP      "system.clock.ntp_sync" /* "ON"      */
#define ND_SET_CLOCK_NTP_DFLT "ON"

/* App-owned */
#define ND_SET_CALLLOG_DUR_LAST      "calllog.duration.last"
#define ND_SET_CALLLOG_DUR_RECEIVED  "calllog.duration.received"
#define ND_SET_CALLLOG_DUR_DIALED    "calllog.duration.dialed"
#define ND_SET_GAMES_SNAKE_LEVEL     "games.snake.level"
#define ND_SET_GAMES_SNAKE_TOPSCORE  "games.snake.topscore"
#define ND_SET_GAMES_MEMORY_TOPSCORE "games.memory.topscore"

/* ------------------------------------------------------------------ *
 * The API
 * ------------------------------------------------------------------ */

/* Populates the DEFAULTS table. Idempotent; called from nd_ui construction. */
nd_err nd_settings_init(void);

/* get_setting(key, dflt). The returned pointer is owned by libneodct and is
 * valid until the next nd_settings_get() or nd_settings_set() on any thread --
 * COPY IT if you are going to keep it. dflt is returned unchanged when the key
 * is absent everywhere. */
const char *nd_settings_get(const char *key, const char *dflt);

/* Copy into a caller buffer, which is what almost every call site actually
 * wants and what makes the lifetime rule above harmless. */
nd_err nd_settings_get_copy(const char *key, const char *dflt, char *out, size_t out_sz);

/* set_setting(key, value): merge, assign, write. "system.os.*" keys are
 * accepted and then stripped by the writer, exactly as in Python. */
nd_err nd_settings_set(const char *key, const char *value);

/* load_with_defaults(DEFAULTS) -- the fully merged map. Owned by the caller;
 * free with nd_props_free(). */
nd_props *nd_settings_effective(void);

/* The one place the R-24 quirk lives. Called from inside nd_settings_get();
 * exposed so that turning the quirk off is a one-line change here rather than
 * an audit of every reader. */
nd_err nd_settings_flush_if_needed(nd_props *effective, const nd_props *stored);

/* Host test harness only, mirroring the monkeypatching in
 * test_settings_version_layering.py. NULL restores the real paths. */
void nd_settings_set_paths(const char *settings_path, const char *version_path);

/* ------------------------------------------------------------------ *
 * The three boolean parsers. Yes, three.
 * ------------------------------------------------------------------ */

/* Settings/main.py _setting_is_enabled(value, default):
 *   lowercase the stripped string
 *   "1" "true" "on" "yes" "enabled"      -> true
 *   "0" "false" "off" "no" "disabled"    -> false
 *   ANYTHING ELSE, and NULL              -> default
 * Used for system.ui.engineering_mode. */
bool nd_setting_is_enabled(const char *value, bool dflt);

/* ModemService: strip, uppercase, then membership of {"ON","1","TRUE","YES"}.
 * Anything else is FALSE -- there is no default to fall back to.
 * Careful: in the Python, an exception while READING the setting returns true,
 * while an unrecognised value returns false. Both paths are reproduced by
 * passing dflt_on_error at the call site. Used for system.modem.allow_calls. */
bool nd_setting_modem_truthy(const char *value);

/* Update/main.py: str(value).strip().upper() compared against a literal.
 * See spec-update-system.md for the exact comparison; it is a third form and
 * it stays a third form. */
bool nd_setting_update_truthy(const char *value, const char *literal_upper);

#ifdef __cplusplus
}
#endif

#endif /* ND_SETTINGS_H_INCLUDED */
