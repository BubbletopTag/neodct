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
#define ND_SET_AUDIO_RINGTOME_DFLT "/NeoDCT/System/tones/Low.mp3"
/* Was "NONE", verbatim from the Python's SettingsStorage.DEFAULTS, and this
 * is a deliberate divergence from it.
 *
 * "NONE" was right for a monochrome OS: a phone that booted to white type on
 * black and let its owner add a picture if they wanted one. The theme's
 * ground is not black any more (nd_ui_paint_chrome paints a sky), so a
 * first boot with no wallpaper is no longer plain -- it is just missing the
 * one asset the rest of the look was drawn to sit on.
 *
 * Anyone who preferred the empty ground still has it: the wallpaper picker's
 * first entry is "None", and choosing it writes "NONE" here. Every other
 * shipped wallpaper is one row further down the same list. */
#define ND_SET_UI_WALLPAPER_DFLT     "NONE"
#define ND_SET_UI_THEME_DFLT         "classic"
#define ND_SET_UI_ENG_MODE_DFLT      "ON"
#define ND_SET_UI_WP_EVERYWHERE_DFLT "ON"
/* Was 0.75, on top of a home wallpaper already dimmed to 0.3 -- so chrome sat
 * at an effective 0.225 and the picture behind a list was barely a texture.
 *
 * The home dim is ND_UI_WALLPAPER_BRIGHTNESS now (0.88, see nd_ui.c), so 0.68
 * puts chrome at an effective 0.60 -- nearly three times what the old build
 * gave it, and still a visible step down from the home screen. That step is
 * the whole reason there are two numbers: the home screen is where the
 * picture is the point, and a screen with a list of words on it is not. */
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
#define ND_SET_UI_THEME         "system.ui.theme"
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
#define ND_SET_MODEM_BOOT_GRACE  "system.modem.boot_grace_s"  /* "30"      */

/* HOW LOUD THE MICROPHONE IS, as an ALSA capture percentage.
 *
 * There has only ever been one gain lever on this phone and it was a constant
 * in a boot script: S17audio raised every "Capture Volume" control on the USB
 * card to 80% once, at boot, and nothing else in the image ever touched the
 * mixer again. 80% was a guess made against the one microphone on the
 * developer's desk, and an electret on a C-Media adapter is quieter than that
 * guess allowed for. So the number stops being a constant here.
 *
 * Read by THREE things that cannot share a cache, which is why it is a
 * setting and not a build-time number:
 *
 *   S17audio        at boot, to bring the card up at the owner's level.
 *   nd_modem_audio  once per core process, cached in nd_modem, and re-applied
 *                   to the card before every call -- see start_mic_pipe().
 *   MicTest         live, so the owner can hear the change while turning it.
 *
 * NOT in DEFAULTS, for the reason the wallpaper and brightness keys give at
 * length: R-24 means every nd_settings_get() rewrites settings.prop with an
 * fsync whatever key it was asked for, and the table is what each of those
 * rewrites has to carry. Keeping it out costs nothing -- the call sites all
 * pass ND_SET_HW_MIC_GAIN_DFLT -- and keeps this key off the write path of
 * every other read in the phone.
 *
 * 100 rather than the old 80, and 100 rather than "whatever the driver came
 * up with". A capture control at full scale is not a distortion risk the way
 * a playback one is: it is the preamp in front of an 8 kHz voice codec, the
 * far end's AGC is downstream of it, and every report of this phone's audio
 * has been "they cannot hear me", never "I am too loud".
 *
 * Bounded 0..100 by nd_mic_gain_from_setting() before it reaches an argv,
 * because the value is a string on a writable partition and amixer takes
 * arguments. Anything unparseable, negative or above 100 falls back here. */
#define ND_SET_HW_MIC_GAIN      "system.hw.mic_gain" /* "100" */
#define ND_SET_HW_MIC_GAIN_DFLT "100"

/* Wallpaper behind the framework's own chrome -- lists, dialogs, text boxes,
 * every screen that used to be flat black. ON by default, because that is the
 * point of the feature; a phone whose owner wants the old look turns it off
 * here and nothing else changes.
 *
 * NOT in DEFAULTS, deliberately -- but NOT because reading them is free.
 * R-24 above is worse than it first looks: nd_settings_get() rewrites
 * settings.prop on EVERY call whatever key it was asked for, because the
 * "missing keys" test walks the DEFAULTS table rather than the key. So what
 * protects the render path is not the table, it is the CALLER: lib/nd_ui.c
 * reads both of these exactly once per process, and once more after each app
 * exit, in chrome_settings_load(), and never from a draw. Keeping them out of
 * DEFAULTS only avoids adding two more keys to what every such rewrite has to
 * carry.
 *
 * There is also deliberately no Settings screen for either. They are taste,
 * not policy, and the wallpaper picker is already where a person goes to
 * change how the phone looks.
 *
 * ND_SET_UI_WP_APP_DIM is a SECOND brightness multiplier applied on top of
 * the 0.3 the home screen already uses, so the default 0.75 puts chrome at
 * 0.3 * 0.75 = 0.225 of the original picture. Chrome carries text at every
 * size the phone has; the home screen carries a clock. Parsed with strtod;
 * unparseable, negative and above-1 values all fall back to the default
 * rather than being clamped to an extreme nobody can then explain. */
#define ND_SET_UI_WP_EVERYWHERE "system.ui.wpeverywhere"     /* "ON"     */
#define ND_SET_UI_WP_APP_DIM    "system.ui.wpeverywhere_dim" /* "0.75"   */

/* WHERE AN ANIMATED WALLPAPER IS ALLOWED TO RUN. Three values, because they
 * are three different costs, not three degrees of one:
 *
 *   ALWAYS  home screen, menus and apps. A widget is a blocking wait, so
 *           this is the one that keeps a decoder open in every app process
 *           and redraws the screen at the GIF's rate for as long as a menu
 *           is on it. The prettiest and the most expensive.
 *   HOME    the home screen only, which is what the phone did before menus
 *           learned to repaint themselves. An app opens no decoder at all
 *           under this, so it is 226 KB and a file descriptor cheaper as
 *           well as quieter.
 *   OFF     nowhere. A .gif wallpaper is its first frame and nothing else,
 *           which costs exactly what a .jpg costs.
 *
 * This exists because the honest answer to "what does the animation do to
 * the battery" is that nobody has measured it on the real hardware yet. It
 * is a way out that does not require picking a different wallpaper.
 *
 * Compared case-insensitively against those three words; anything else is
 * the default. Read once per process, like the two above. */
#define ND_SET_UI_WP_ANIMATE      "system.ui.wpanimate" /* "ALWAYS" */
#define ND_SET_UI_WP_ANIMATE_DFLT "ALWAYS"

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

/* HOW BRIGHT THE PANEL IS, as a percentage, written by Sleepy's Brightness
 * picker and re-applied by the core at boot.
 *
 * It is system.ui rather than sleepy. because of who has to read it. The
 * per-app namespace below is for state an app owns and reads back itself; a
 * brightness is applied by the CORE, before an app exists, and Sleepy is only
 * the screen that happens to set it today. Putting it under sleepy. would
 * mean the core reaching into an app's namespace for a value the app is not
 * even running to provide.
 *
 * NOT in DEFAULTS, for the reason the wallpaper keys give at length: a read
 * of any key rewrites settings.prop against the whole table, and this one is
 * read once at boot and written only when somebody moves the slider.
 *
 * 100 is the default and it is not merely "the brightest". A phone that has
 * never had a brightness set must come up at whatever the device tree asked
 * for, and a stored value only exists after somebody deliberately chose one.
 * Anything unparseable falls back here too -- a settings file that has been
 * hand-edited into nonsense must not be able to leave the screen dark. */
#define ND_SET_UI_BRIGHTNESS      "system.ui.brightness" /* "100" */
#define ND_SET_UI_BRIGHTNESS_DFLT "100"

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
