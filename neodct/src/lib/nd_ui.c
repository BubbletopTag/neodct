/* nd_ui.c -- NeoDCT_UI: the context object, and the home screen.
 *
 * Ported from System/core/main.py:516 (class NeoDCT_UI). In Python this is a
 * duck-typed grab-bag that framework.py alone touches 142 times; in C it is
 * one struct and the functions that fill it in, keep it current, and paint the
 * two screens the core owns.
 *
 * ============ CONSTRUCTION ORDER IS LOAD-BEARING ============
 *
 * nd_ui_init() walks the eighteen steps of spec-core-loop.md section 6 in
 * order and says so at each one. The step that catches people out is 13: the
 * alpha security notice is a BLOCKING MODAL DRAWN FROM INSIDE THE
 * CONSTRUCTOR, and it runs BEFORE engineering_mode, home_layout, wallpaper
 * and apps have been assigned. A widget drawn at that moment sees a half-built
 * context. That is correct -- it is what the first-boot screen looks like.
 *
 * ============ WHY SO MANY #pragma weak ============
 *
 * The core is the one place that touches every subsystem, and those subsystems
 * are being written in parallel. Rather than stubbing another agent's module
 * (which would collide the moment it lands) or referencing a symbol that may
 * not exist (which would fail at load), every cross-subsystem entry point is
 * declared weak and NULL-checked at the call. When the real module lands the
 * reference resolves and the branch simply starts being taken; nothing here
 * needs editing. Every one of these guards is also a genuine runtime case:
 * an APP process has no modem, no battery and no notify handle at all
 * (nd_ui.h), so the NULL-service path has to be correct regardless.
 *
 * ============ WHERE THE HOME SCREEN'S NUMBERS COME FROM ============
 *
 * Every coordinate below is the Python's, and the ones that look odd are
 * load-bearing:
 *
 *   - the two status sprites are 26x131 at y 17, so they reach y 148, three
 *     rows PAST content_bottom (145); the softkey bar covers them afterwards;
 *   - the envelope sits at (int(46 * H/240) + 7, int(10 * H/240)) = (40, 7),
 *     and that stray "+ 7" is in the Python;
 *   - the banner starts at max(46, int(145 * 0.34)) = 49 and steps by 24;
 *   - the envelope blinks on int(time() * 2) % 2 == 0, i.e. 500 ms on,
 *     500 ms off, driven by the same virtual clock the capture harness uses.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_bench.h"
#include "nd_crash.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_layout.h"
#include "nd_log.h"
#include "nd_modem.h"
#include "nd_notify.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_svc.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* ------------------------------------------------------------------ *
 * Cross-subsystem entry points -- see the header comment
 * ------------------------------------------------------------------ */

#pragma weak nd_modem_open
#pragma weak nd_modem_close
#pragma weak nd_modem_signal_level
#pragma weak nd_modem_operator_display
#pragma weak nd_modem_state
#pragma weak nd_modem_caller_id
#pragma weak nd_modem_take_pending_event
#pragma weak nd_modem_requeue_event
#pragma weak nd_modem_fetch_sms
#pragma weak nd_modem_read_stored_sms
#pragma weak nd_modem_dial
#pragma weak nd_modem_answer
#pragma weak nd_modem_hangup
#pragma weak nd_modem_last_call_secs

#pragma weak nd_battery_open
#pragma weak nd_battery_close
#pragma weak nd_battery_level
#pragma weak nd_battery_has_hardware
#pragma weak nd_battery_poll
#pragma weak nd_battery_take_pending_warning
#pragma weak nd_battery_vcell

#pragma weak nd_notify_open
#pragma weak nd_notify_close
#pragma weak nd_notify_active
#pragma weak nd_notify_banner_lines
#pragma weak nd_notify_dismiss
#pragma weak nd_notify_count
#pragma weak nd_notify_kind
#pragma weak nd_notify_latest_data
#pragma weak nd_notify_post_sms
#pragma weak nd_notify_start_ring
#pragma weak nd_notify_stop_ring
#pragma weak nd_notify_play_tone

#pragma weak nd_appsel_init
#pragma weak nd_appsel_show

#pragma weak nd_msgdialog_init
#pragma weak nd_msgdialog_set_title
#pragma weak nd_msgdialog_set_icon
#pragma weak nd_msgdialog_set_button
#pragma weak nd_msgdialog_show

#pragma weak nd_proc_launch_app

/* The two Dialer screens are declared in nd_widgets.h and belong to the UI
 * framework work package. */
#pragma weak nd_dialer_show_calling
#pragma weak nd_dialer_show_incoming

/* The shared contact picker has no declaration anywhere in the frozen header
 * set -- main.py imports System.apps.PhoneBook.shared.list_ui directly, and
 * nothing in include/ names it. This spelling follows the project's
 * convention and is weak, so the home screen's up/down key behaves correctly
 * (it does nothing) until PhoneBook's work package defines it, and needs a
 * one-line rename here if that package picks a different name. Recorded in
 * OPEN-QUESTIONS.md as U-4. */
bool nd_contacts_show_selector(nd_ui *ui, const char *title, const char *btn_text, nd_contact *out);
#pragma weak nd_contacts_show_selector

/* ------------------------------------------------------------------ *
 * Process-global bits of NeoDCT_UI that nd_ui.h has no field for
 * ------------------------------------------------------------------ */

/* There is exactly one nd_ui per process (the core has one, an app has one),
 * so instance state with nowhere to live in the frozen struct lives here. */
static double g_ring_seen_at; /* self._ring_seen_at, 0.0 == None */
static char g_carrier[64];    /* lifetime of nd_ui_status_carrier()'s result */

typedef struct {
    bool status_active;
    int32_t battery_level;
    int32_t signal_level;
    char carrier[64];
    bool banner_active;
    int32_t banner_count;
} ui_sim_state;

static ui_sim_state g_sim;

/* ------------------------------------------------------------------ *
 * Geometry -- framework.py's _ui_width / _ui_height / _softkey_height
 * ------------------------------------------------------------------ */

/* The Python spells these getattr(ui, "W", DEFAULT_UI_W), so a context that
 * has not reached construction step 5 answers with the default rather than
 * with zero. A zero field is exactly that case.
 *
 * ALL FIVE ARE WEAK. lib/nd_ui_metrics.c (the UI-framework work package)
 * defines the same five strongly and landed first; the two implementations
 * agree, and weak-vs-strong resolves to theirs with no duplicate symbol and
 * no edit needed in either file if one of them later goes away. */
__attribute__((weak)) int32_t nd_ui_width(const nd_ui *ui)
{
    return (ui != NULL && ui->w > 0) ? ui->w : ND_UI_W;
}

__attribute__((weak)) int32_t nd_ui_height(const nd_ui *ui)
{
    return (ui != NULL && ui->h > 0) ? ui->h : ND_UI_H;
}

__attribute__((weak)) int32_t nd_ui_softkey_height(const nd_ui *ui)
{
    return (ui != NULL && ui->softkey_h > 0) ? ui->softkey_h : ND_SOFTKEY_H;
}

__attribute__((weak)) int32_t nd_ui_content_bottom(const nd_ui *ui)
{
    return nd_ui_height(ui) - nd_ui_softkey_height(ui);
}

/* max(30, int(H * 0.11)) -- 30 on this panel only because the floor wins;
 * int(175 * 0.11) is 19. Anyone hard-coding 30 breaks a taller display. */
__attribute__((weak)) int32_t nd_ui_header_divider_y(const nd_ui *ui)
{
    return nd_max32(30, nd_trunc32((double)nd_ui_height(ui) * 0.11));
}

/* ------------------------------------------------------------------ *
 * The status readouts (nd_ui_sim.h)
 * ------------------------------------------------------------------ */

int32_t nd_ui_status_battery_level(const nd_ui *ui)
{
    if (g_sim.status_active)
        return g_sim.battery_level;
    if (ui != NULL && ui->battery != NULL && nd_battery_level != NULL)
        return nd_battery_level(ui->battery);
    /* BatteryService.__init__: _level = 3 "matches the pre-0.2.4a static
     * gauge until first poll". */
    return 3;
}

bool nd_ui_status_battery_hardware(const nd_ui *ui)
{
    if (g_sim.status_active)
        return true; /* simulate_status() sets ui.battery.hardware = True */
    if (ui != NULL && ui->battery != NULL && nd_battery_has_hardware != NULL)
        return nd_battery_has_hardware(ui->battery);
    /* An APP process: no handle, but possibly a core to ask. Reached only
     * when the override is inactive AND the handle is NULL, so neither the
     * core nor nd-shoot's in-process app runs ever get here and no reference
     * frame can move. nd_svc.h; OPEN-QUESTIONS.md MSG-1. */
    if (nd_svc_client_active()) {
        nd_svc_battery b;

        if (nd_svc_battery_read(ui, &b))
            return b.hardware;
    }
    return false;
}

int32_t nd_ui_status_signal_level(const nd_ui *ui)
{
    if (g_sim.status_active)
        return g_sim.signal_level;
    if (ui != NULL && ui->modem != NULL && nd_modem_signal_level != NULL)
        return nd_modem_signal_level(ui->modem);
    /* See nd_ui_status_battery_hardware() for why this cannot reach the
     * capture harness. The core's ModemService derives its own bars the same
     * way, so the app sees the number the home screen would draw. */
    if (nd_svc_client_active()) {
        nd_modem_status st;

        if (nd_svc_modem_status(ui, &st))
            return st.signal_level;
    }
    return -1; /* "unknown", which is NOT zero bars */
}

const char *nd_ui_status_carrier(const nd_ui *ui)
{
    const char *name = NULL;

    if (g_sim.status_active)
        return g_sim.carrier;
    if (ui != NULL && ui->modem != NULL && nd_modem_operator_display != NULL)
        name = nd_modem_operator_display(ui->modem);
    (void)nd_strlcpy(g_carrier, name != NULL ? name : "", sizeof g_carrier);
    return g_carrier;
}

bool nd_ui_status_notify_active(const nd_ui *ui)
{
    if (ui != NULL && ui->notify != NULL && nd_notify_active != NULL)
        return nd_notify_active(ui->notify);
    return g_sim.banner_active;
}

size_t nd_ui_status_banner_lines(const nd_ui *ui, char l1[ND_NOTIFY_LINE_MAX],
                                 char l2[ND_NOTIFY_LINE_MAX])
{
    if (ui != NULL && ui->notify != NULL && nd_notify_banner_lines != NULL)
        return nd_notify_banner_lines(ui->notify, l1, l2);
    if (!g_sim.banner_active)
        return 0u;
    /* NotifyService.banner_lines(): ("%d message[s]" % count, "received"). */
    (void)nd_snprintf(l1, ND_NOTIFY_LINE_MAX, "%d %s", g_sim.banner_count,
                      g_sim.banner_count == 1 ? "message" : "messages");
    (void)nd_strlcpy(l2, "received", ND_NOTIFY_LINE_MAX);
    return 2u;
}

void nd_ui_sim_status(nd_ui *ui, int32_t battery_level, int32_t signal_level, const char *carrier)
{
    ND_UNUSED(ui);
    g_sim.status_active = true;
    g_sim.battery_level = battery_level;
    g_sim.signal_level = signal_level;
    (void)nd_strlcpy(g_sim.carrier, carrier != NULL ? carrier : "", sizeof g_sim.carrier);
}

void nd_ui_sim_sms_banner(nd_ui *ui, int32_t count)
{
    if (ui != NULL && ui->notify != NULL && nd_notify_post_sms != NULL) {
        int32_t i;

        for (i = 0; i < count; i++)
            nd_notify_post_sms(ui->notify, (int64_t)i + 1, false);
        return;
    }
    g_sim.banner_active = count > 0;
    g_sim.banner_count = count;
}

void nd_ui_sim_clear(nd_ui *ui)
{
    ND_UNUSED(ui);
    memset(&g_sim, 0, sizeof g_sim);
}

/* ------------------------------------------------------------------ *
 * get_text_size / get_image
 * ------------------------------------------------------------------ */

void nd_ui_text_size(const nd_ui *ui, const char *text, const nd_font *f, int32_t *w, int32_t *h)
{
    ND_UNUSED(ui);
    if (w != NULL)
        *w = 0;
    if (h != NULL)
        *h = 0;
    if (f == NULL || text == NULL)
        return;
    nd_text_size(f, text, w, h);
}

/* The "/home" fixup, reproduced from get_image():
 *
 *     if path.startswith("/home"):
 *         if "System" in path:
 *             clean = "/NeoDCT" + path.split("NeoDCT")[-1]
 *
 * A development left-over -- some manifests may still carry one. Note the
 * guard tests for "System", not for "NeoDCT", and that split()[-1] returns the
 * WHOLE path when "NeoDCT" does not occur, so such a path is prefixed rather
 * than rewritten. Both are the Python's behaviour; nd_ui.h's summary of this
 * is loose. Recorded in OPEN-QUESTIONS.md as U-1. */
static const char *clean_image_path(const char *path, char *buf, size_t buf_sz)
{
    const char *last = NULL;
    const char *p;
    const char *rel;

    if (path == NULL)
        return NULL;
    if (strncmp(path, "/home", 5) != 0)
        return path;
    if (strstr(path, "System") == NULL)
        return path;

    for (p = strstr(path, "NeoDCT"); p != NULL; p = strstr(p + 6, "NeoDCT"))
        last = p;
    rel = last != NULL ? last + 6 : path;

    if (nd_snprintf(buf, buf_sz, "/NeoDCT%s", rel) != ND_OK)
        return path;
    return buf;
}

static const nd_image *ui_get_image(nd_ui *ui, const char *path, int32_t max_size, double scale)
{
    char clean[ND_PATH_MAX];
    const char *use;

    if (ui == NULL || ui->image_cache == NULL || path == NULL)
        return NULL;
    use = clean_image_path(path, clean, sizeof clean);
    return nd_imgcache_get(ui->image_cache, use, max_size, scale);
}

const nd_image *nd_ui_get_image(nd_ui *ui, const char *path)
{
    return ui_get_image(ui, path, 0, 0.0);
}

const nd_image *nd_ui_get_image_max(nd_ui *ui, const char *path, int32_t max_size)
{
    return ui_get_image(ui, path, max_size, 0.0);
}

const nd_image *nd_ui_get_image_scaled(nd_ui *ui, const char *path, double scale)
{
    return ui_get_image(ui, path, 0, scale);
}

/* ------------------------------------------------------------------ *
 * Wallpaper
 * ------------------------------------------------------------------ */

/* owned by the caller; free with nd_image_free() */
nd_image *nd_ui_load_wallpaper(const char *path)
{
    nd_image *img;
    nd_image *rgb;
    nd_image *scaled;

    if (path == NULL || !nd_path_exists(path)) {
        nd_log(ND_LOG_UI, "No wallpaper found.");
        return NULL;
    }

    nd_log(ND_LOG_UI, "Loading wallpaper: %s", path);

    /* LOAD_TRUNCATED_IMAGES is set globally by the Python at import; the C
     * decoder already decodes as far as a truncated JPEG got. */
    img = nd_image_open(path);
    if (img == NULL) {
        nd_log(ND_LOG_UI, "Wallpaper load error: %s", path);
        return NULL;
    }

    rgb = nd_image_convert(img, ND_PIXFMT_RGB888);
    nd_image_free(img);
    if (rgb == NULL)
        return NULL;

    if (rgb->w != ND_UI_W || rgb->h != ND_UI_H) {
        scaled = nd_image_resize_lanczos(rgb, ND_UI_W, ND_UI_H);
        nd_image_free(rgb);
        if (scaled == NULL)
            return NULL;
    } else {
        scaled = rgb;
    }

    /* ImageEnhance.Brightness(img).enhance(0.3) -- TRUNCATING, per
     * nd_image.h's formula (b). Rounding mismatches 128 of 256 values. */
    if (nd_image_brightness(scaled, 0.3) != ND_OK) {
        nd_image_free(scaled);
        return NULL;
    }
    return scaled;
}

/* Case-insensitive equality and suffix tests, because the Python spells the
 * two checks `setting.upper() != "NONE"` and
 * `setting.lower().endswith((".jpg", ".jpeg"))`. ASCII only, which is all
 * str.upper()/lower() can differ from for a filesystem path here. */
static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool eq_ci(const char *a, const char *b)
{
    size_t i;

    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i]))
            return false;
    }
    return a[i] == b[i];
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t fl = strlen(suffix);

    return sl >= fl && eq_ci(s + (sl - fl), suffix);
}

/* Construction step 16, and re-run after every app exit. */
static nd_image *load_configured_wallpaper(void)
{
    char setting[ND_PATH_MAX];

    if (nd_settings_get_copy(ND_SET_UI_WALLPAPER, ND_SET_UI_WALLPAPER_DFLT, setting,
                             sizeof setting) != ND_OK)
        return NULL;

    /* `if wallpaper_setting and wallpaper_setting.upper() != "NONE":` -- and
     * when it IS "NONE", load_wallpaper() is never called, so the
     * "[UI] No wallpaper found." line does not appear either. */
    if (setting[0] == '\0' || eq_ci(setting, "NONE"))
        return NULL;

    if ((ends_with_ci(setting, ".jpg") || ends_with_ci(setting, ".jpeg")) &&
        nd_path_exists(setting))
        return nd_ui_load_wallpaper(setting);

    nd_log(ND_LOG_UI, "Invalid wallpaper setting: %s", setting);
    if (nd_path_exists(ND_PATH_WALLPAPER))
        return nd_ui_load_wallpaper(ND_PATH_WALLPAPER);
    return NULL;
}

/* ------------------------------------------------------------------ *
 * The app scan
 * ------------------------------------------------------------------ */

/* `int(data.get("id", 999))`: the shipped manifests spell the id as a STRING
 * ("id": "1"), and Python's int() takes either. A value int() would reject
 * raises inside the try block, which skips the whole app -- so an unparseable
 * id is a dropped entry, not a 999. */
static bool manifest_id(const nd_json_val *o, int32_t *out)
{
    const nd_json_val *v = nd_json_get(o, "id");
    int64_t n;
    const char *s;

    if (v == NULL) {
        *out = 999;
        return true;
    }
    if (nd_json_int(v, &n)) {
        *out = (int32_t)n;
        return true;
    }
    if (nd_json_str(v, &s) && s != NULL) {
        char *end = NULL;
        long parsed;

        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (*s == '\0')
            return false;
        parsed = strtol(s, &end, 10);
        if (end == s)
            return false;
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
            end++;
        if (*end != '\0')
            return false;
        *out = (int32_t)parsed;
        return true;
    }
    return false;
}

size_t nd_ui_scan_apps(const char *dir, nd_app_entry *out, size_t max)
{
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *de;
    size_t written = 0u;

    if (dir == NULL || out == NULL || max == 0u)
        return 0u;

    /* `if not os.path.exists(app_dir): os.makedirs(app_dir)`, and a failure
     * there returns silently. */
    if (!nd_path_exists(dir)) {
        if (nd_mkdir_p(dir, 0755u) != ND_OK)
            return 0u;
    }

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return 0u;
    d = opendir(resolved);
    if (d == NULL)
        return 0u;

    /* os.listdir() order, i.e. filesystem order. The later sort by id is what
     * makes the menu deterministic. */
    while (written < max && (de = readdir(d)) != NULL) {
        char manifest[ND_PATH_MAX];
        char appdir[ND_APP_PATH_MAX];
        nd_json_doc *doc = NULL;
        const nd_json_val *root;
        nd_app_entry *e;
        const char *icon;
        int32_t id;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (nd_snprintf(appdir, sizeof appdir, "%s/%s", dir, de->d_name) != ND_OK)
            continue;
        if (nd_snprintf(manifest, sizeof manifest, "%s/manifest.json", appdir) != ND_OK)
            continue;
        if (!nd_path_exists(manifest))
            continue;
        if (nd_json_parse_file(manifest, &doc, NULL, 0u) != ND_OK)
            continue;

        root = nd_json_root(doc);
        if (root == NULL || nd_json_type_of(root) != ND_JSON_OBJECT || !manifest_id(root, &id)) {
            nd_json_free(doc);
            continue;
        }

        e = &out[written];
        memset(e, 0, sizeof *e);
        (void)nd_strlcpy(e->name, nd_json_get_str(root, "name", de->d_name), sizeof e->name);
        icon = nd_json_get_str(root, "icon", "icon.png");
        (void)nd_snprintf(e->icon, sizeof e->icon, "%s/%s", appdir, icon);
        (void)nd_strlcpy(e->path, appdir, sizeof e->path);
        (void)nd_strlcpy(e->exec, nd_json_get_str(root, "exec", "main.py"), sizeof e->exec);
        e->id = id;
        written++;

        nd_json_free(doc);
    }

    (void)closedir(d);
    return written;
}

/* `self.apps.sort(key=lambda x: x["id"])`. Python's sort is STABLE, so equal
 * ids keep filesystem order; qsort is not, hence the insertion sort. n is at
 * most 64. */
static void sort_apps_by_id(nd_app_entry *apps, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_app_entry key = apps[i];
        size_t j = i;

        while (j > 0u && apps[j - 1u].id > key.id) {
            apps[j] = apps[j - 1u];
            j--;
        }
        apps[j] = key;
    }
}

static void rescan_apps(nd_ui *ui)
{
    size_t n;

    ui->home_.n_apps = 0u;
    n = nd_ui_scan_apps(ND_PATH_APPS_DIR, ui->home_.apps, ND_APP_MAX);
    ui->home_.n_apps = n;
    if (nd_ui_engineering_mode(ui) && ui->home_.n_apps < ND_APP_MAX) {
        n = nd_ui_scan_apps(ND_PATH_ENG_APPS_DIR, &ui->home_.apps[ui->home_.n_apps],
                            ND_APP_MAX - ui->home_.n_apps);
        ui->home_.n_apps += n;
    }
    sort_apps_by_id(ui->home_.apps, ui->home_.n_apps);
}

/* ------------------------------------------------------------------ *
 * The home screen's state, loaded on first read -- see nd_ui.h
 * ------------------------------------------------------------------ */

nd_image *nd_ui_wallpaper(nd_ui *ui)
{
    if (ui == NULL)
        return NULL;
    if (!ui->home_.wallpaper_ready) {
        /* Set BEFORE the load, not after: a wallpaper that fails to decode
         * must stay NULL rather than being retried on every frame. */
        ui->home_.wallpaper_ready = true;
        ui->home_.wallpaper = load_configured_wallpaper();
    }
    return ui->home_.wallpaper;
}

const nd_home_layout *nd_ui_home_layout(nd_ui *ui)
{
    if (ui == NULL)
        return NULL;
    if (!ui->home_.home_layout_ready) {
        ui->home_.home_layout_ready = true;
        ui->home_.home_layout = nd_layout_load(ND_PATH_HOME_LAYOUT);
    }
    return ui->home_.home_layout;
}

bool nd_ui_engineering_mode(nd_ui *ui)
{
    if (ui == NULL)
        return false;
    if (!ui->home_.eng_mode_ready) {
        ui->home_.eng_mode_ready = true;
        ui->home_.engineering_mode = nd_setting_is_enabled(
            nd_settings_get(ND_SET_UI_ENGINEERING, ND_SET_UI_ENG_MODE_DFLT), true);
    }
    return ui->home_.engineering_mode;
}

const nd_app_entry *nd_ui_app_list(nd_ui *ui, size_t *n_out)
{
    if (ui == NULL) {
        if (n_out != NULL)
            *n_out = 0u;
        return NULL;
    }
    if (!ui->home_.apps_ready) {
        ui->home_.apps_ready = true;
        rescan_apps(ui);
    }
    if (n_out != NULL)
        *n_out = ui->home_.n_apps;
    return ui->home_.apps;
}

int32_t nd_ui_unread_sms(nd_ui *ui)
{
    if (ui == NULL)
        return 0;
    if (!ui->home_.unread_sms_ready) {
        ui->home_.unread_sms_ready = true;
        ui->home_.unread_sms = nd_db_count_unread_sms();
    }
    return ui->home_.unread_sms;
}

void nd_ui_set_unread_sms(nd_ui *ui, int32_t n)
{
    if (ui == NULL)
        return;
    ui->home_.unread_sms = n;
    ui->home_.unread_sms_ready = true;
}

size_t nd_ui_app_count(nd_ui *ui)
{
    size_t n = 0u;

    (void)nd_ui_app_list(ui, &n);
    return n;
}

void nd_ui_set_wallpaper(nd_ui *ui, nd_image *img)
{
    if (ui == NULL)
        return;
    if (ui->home_.wallpaper != img)
        nd_image_free(ui->home_.wallpaper);
    ui->home_.wallpaper = img;
    ui->home_.wallpaper_ready = true;
}

/* ------------------------------------------------------------------ *
 * Construction step 13 -- the alpha security notice
 * ------------------------------------------------------------------ */

#define ALPHA_NOTICE                                                        \
    "This is alpha software. Consider it extremely insecure and unstable. " \
    "Don't store important data on this device."

/* ErrorScreen.show_alpha_security_notice_once(). Returns true when it showed.
 * The ack file is written ONLY after the dialog was actually displayed -- if
 * the widget layer is not linked in yet, boot continues and the notice is
 * still owed. */
static bool show_alpha_security_notice_once(nd_ui *ui)
{
    nd_msgdialog dlg;
    char resolved[ND_PATH_MAX];
    FILE *f;

    if (nd_path_exists(ND_PATH_ACK_SECURITY))
        return false;
    if (nd_msgdialog_init == NULL || nd_msgdialog_show == NULL) {
        nd_log(ND_LOG_UI, "Security notice deferred: no dialog widget linked.");
        return false;
    }

    nd_msgdialog_init(&dlg, ui, ALPHA_NOTICE);
    if (nd_msgdialog_set_title != NULL)
        nd_msgdialog_set_title(&dlg, "Notice");
    if (nd_msgdialog_set_icon != NULL)
        nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
    if (nd_msgdialog_set_button != NULL)
        nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);

    /* Best-effort persistence, exactly as the Python's bare except. */
    (void)nd_mkdir_p(ND_PATH_USER, 0755u);
    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_ACK_SECURITY) == ND_OK) {
        f = fopen(resolved, "w");
        if (f != NULL) {
            (void)fprintf(f, "%lld", (long long)nd_time_now());
            (void)fclose(f);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * Construction and teardown
 * ------------------------------------------------------------------ */

static nd_err ui_build_surfaces(nd_ui *ui, nd_fb *fb)
{
    /* 240 * 175 * 3 = 126,000 bytes -- the one long-lived UI surface. */
    ui->fb = fb;
    ui->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (ui->canvas == NULL)
        return ND_ERR_NOMEM;

    ui->draw = calloc(1u, sizeof *ui->draw);
    if (ui->draw == NULL)
        return ND_ERR_NOMEM;
    if (nd_draw_bind(ui->draw, ui->canvas) != ND_OK)
        return ND_ERR_INVAL;

    /* 240 * 145 * 3 = 104,400 bytes, lent to DetailPage so it does not
     * allocate a column per frame the way the Python does. */
    ui->scratch = nd_image_new_filled(ND_UI_W, ND_UI_H - ND_SOFTKEY_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (ui->scratch == NULL)
        return ND_ERR_NOMEM;
    return ND_OK;
}

static void ui_load_fonts(nd_ui *ui)
{
    char font[ND_PATH_MAX];

    /* nd_font_load() takes a REAL filesystem path -- the same split
     * nd_t9_dict_open() uses, so the ND_ROOT hook is applied by the caller
     * that owns the constant. See I-8 in OPEN-QUESTIONS.md. */
    if (nd_path_resolve(font, sizeof font, ND_PATH_FONT) != ND_OK)
        font[0] = '\0';

    ui->font_s = nd_font_load(font, ND_FONT_PX_S);
    ui->font_md = nd_font_load(font, ND_FONT_PX_MD);
    ui->font_n = nd_font_load(font, ND_FONT_PX_N);
    ui->font_xl = nd_font_load(font, ND_FONT_PX_XL);

    if (ui->font_s != NULL && ui->font_md != NULL && ui->font_n != NULL && ui->font_xl != NULL) {
        nd_log(ND_LOG_UI, "Custom font loaded.");
        return;
    }
    /* The Python falls back to ImageFont.load_default(), a bundled bitmap
     * face C has no equivalent of. Every face is left NULL and every text
     * draw becomes a no-op, which is at least visibly wrong rather than
     * subtly wrong. Recorded in OPEN-QUESTIONS.md as U-2. */
    nd_log(ND_LOG_UI, "Font load failed, using default.");
    nd_font_free(ui->font_s);
    nd_font_free(ui->font_md);
    nd_font_free(ui->font_n);
    nd_font_free(ui->font_xl);
    ui->font_s = NULL;
    ui->font_md = NULL;
    ui->font_n = NULL;
    ui->font_xl = NULL;
}

/* Steps 5, 9, 10, 11, 12, 14, 15, 16, 17, 18 -- everything both the core and
 * an app process need. */
/* ------------------------------------------------------------------ *
 * has_matrix_keypad, in one place for both init paths
 * ------------------------------------------------------------------ */

/* framework._t9_active(). T9 -- multi-tap, predictive, the # mode cycle and
 * the mode indicator -- runs on the i2c keypad only, because a QWERTY dev
 * keyboard has real letters and takes the DEV_KEYMAP path instead.
 *
 * `detected` is what the process can see for itself: the real backend in the
 * core, the core's word for it in an app (nd_app.h). NEODCT_T9 overrides
 * both, and is the only way to exercise T9 on a keyboard. Unset in every
 * golden capture, so the reference frames are unchanged -- OPEN-QUESTIONS.md
 * T-5 still holds. */
static bool t9_active(bool detected)
{
    const char *env = getenv(ND_ENV_T9);

    if (env == NULL || env[0] == '\0')
        return detected;
    /* Exactly "0" is off; anything else non-empty is on. A developer who
     * exports NEODCT_T9=0 to turn it off must not get it turned on. */
    return !(env[0] == '0' && env[1] == '\0');
}

/* The core's own answer, handed to an app in NEODCT_KEYPAD_MATRIX because the
 * app's input is a pipe and cannot be asked. */
static bool matrix_from_env(void)
{
    const char *env = getenv(ND_ENV_KEYPAD_MATRIX);

    return env != NULL && env[0] == '1' && env[1] == '\0';
}

static nd_err ui_common_init(nd_ui *ui, nd_fb *fb)
{
    nd_err rc;

    /* --- step 5: geometry --- */
    ui->w = ND_UI_W;
    ui->h = ND_UI_H;
    ui->softkey_h = ND_SOFTKEY_H;
    ui->content_bottom = ui->h - ui->softkey_h;

    /* --- step 9: the ONE transparent softkey bar in the system. It is built
     * BEFORE ui->softkey exists, which is how the Python's hasattr() trick
     * decides it is the system bar. nd_ui.h forbids emulating hasattr, so the
     * boolean is passed explicitly and softkey_exists is set at the same
     * point -- every bar built after this one is opaque. --- */
    ui->softkey = calloc(1u, sizeof *ui->softkey);
    if (ui->softkey == NULL)
        return ND_ERR_NOMEM;
    nd_softkey_init(ui->softkey, ui, /*transparent=*/true);
    ui->softkey_exists = true;

    /* --- step 10: surfaces --- */
    rc = ui_build_surfaces(ui, fb);
    nd_bench_mark("ui_common: surfaces");
    if (rc != ND_OK)
        return rc;

    /* --- step 11 --- */
    ui->state = ND_UI_STATE_HOME;

    /* --- step 12 --- */
    ui_load_fonts(ui);
    nd_bench_mark("ui_common: 4 faces");

    /* --- step 13 is CORE ONLY; see nd_ui_init --- */

    /* --- step 15 --- */
    ui->image_cache = nd_imgcache_new(ND_IMGCACHE_MAX);
    if (ui->image_cache == NULL)
        return ND_ERR_NOMEM;

    /* --- steps 14, 16, 17 and 18 -- home_layout, wallpaper, engineering_mode
     * and the app scan -- are NOT done here any more. They are the home
     * screen's state and they load on first read; see "Lazy home state" in
     * nd_ui.h. Their ORDER relative to each other is unchanged, because
     * nd_ui_app_list() still asks nd_ui_engineering_mode() first, which is
     * the only dependency among the four. --- */
    return ND_OK;
}

nd_err nd_ui_init(nd_ui *ui, nd_fb *fb)
{
    nd_err rc;

    if (ui == NULL)
        return ND_ERR_INVAL;
    memset(ui, 0, sizeof *ui);
    ui->keypad_fd = -1;
    g_ring_seen_at = 0.0;

    (void)nd_settings_init();

    /* --- step 1 --- */
    (void)nd_db_init_all();

    /* --- step 2: the three services. Each is optional at link time and each
     * runs in simulation when its hardware is absent, which is what makes the
     * whole UI drivable on a desktop. --- */
    if (nd_modem_open != NULL)
        (void)nd_modem_open(&ui->modem);
    if (nd_battery_open != NULL)
        (void)nd_battery_open(&ui->battery, -1, -1);
    if (nd_notify_open != NULL)
        (void)nd_notify_open(&ui->notify);

    /* --- step 3 --- */
    /* --- step 3 is the unread-SMS count, and it is lazy now: see
     * nd_ui_unread_sms(). --- */

    /* --- step 4 --- */
    ui->handling_call = false;
    ui->shutting_down = false;
    ui->dial_buffer[0] = '\0';

    /* --- steps 6, 7, 8: the keypad. nd_input owns the whole discovery,
     * keymap and matrix story (see nd_input.h and nd_keypad.h); the core just
     * asks for a backend and remembers the descriptor widgets poll directly
     * to flush pending keys before their first draw. --- */
    if (nd_input_open(&ui->input) == ND_OK && ui->input != NULL) {
        ui->keypad_fd = nd_input_fd(ui->input);
        ui->has_matrix_keypad = t9_active(nd_input_has_matrix(ui->input));
    } else {
        ui->input = NULL;
        ui->keypad_fd = -1;
        ui->has_matrix_keypad = t9_active(false);
    }

    rc = ui_common_init(ui, fb);
    if (rc != ND_OK) {
        nd_ui_teardown(ui);
        return rc;
    }

    /* --- step 13, in its real position: AFTER the fonts and the canvas, and
     * BEFORE home_layout, wallpaper, engineering_mode and apps have any
     * meaning. That used to be an ordering difference from the Python --
     * ui_common_init had already assigned all four -- recorded as U-3 in
     * OPEN-QUESTIONS.md. Making them lazy closed it: at this point none of
     * them has been loaded, which is exactly where launcher.py stood. --- */
    (void)show_alpha_security_notice_once(ui);

    return ND_OK;
}

nd_err nd_ui_init_app(nd_ui *ui, nd_fb *fb, int keypad_fd)
{
    nd_err rc;

    if (ui == NULL)
        return ND_ERR_INVAL;
    memset(ui, 0, sizeof *ui);
    ui->keypad_fd = keypad_fd;
    g_ring_seen_at = 0.0;

    (void)nd_settings_init();
    nd_bench_mark("ui_init_app: settings");

    /* No modem, no battery, no notify: those live in the core and an app that
     * needs them asks across the boundary. An incoming call arrives here as
     * SIGTERM, not as a key -- see nd_app.h.
     *
     * THIS is the asking: NEODCT_SERVICE_FD, the fourth inherited descriptor,
     * carrying the four operations in nd_svc.h. Absent is not an error -- a
     * hand-run nd-apprun has no core to ask, and every nd_svc_* call answers
     * "not present", which is the sentence Messages and the Modem app already
     * draw. OPEN-QUESTIONS.md MSG-1. */
    nd_svc_client_open_from_env();
    if (keypad_fd >= 0 && nd_input_open_pipe(&ui->input, keypad_fd) != ND_OK)
        ui->input = NULL;

    /* NOT nd_input_has_matrix(ui->input): ui->input is the inherited PIPE and
     * has no matrix by construction, which is why this flag used to be false
     * in every app on every device and took multi-tap, predictive, the # mode
     * cycle and the mode indicator down with it. The core knows, and says so.
     * nd_app.h, and OPEN-QUESTIONS.md BR-3. */
    ui->has_matrix_keypad = t9_active(matrix_from_env());

    nd_bench_mark("ui_init_app: input");

    rc = ui_common_init(ui, fb);
    nd_bench_mark("ui_init_app: fonts+canvas");
    if (rc != ND_OK) {
        nd_ui_teardown(ui);
        return rc;
    }
    return ND_OK;
}

void nd_ui_teardown(nd_ui *ui)
{
    if (ui == NULL)
        return;

    /* Idempotent, and a no-op in the core, which never opened one. */
    nd_svc_client_close();

    if (ui->notify != NULL && nd_notify_close != NULL)
        nd_notify_close(ui->notify);
    if (ui->battery != NULL && nd_battery_close != NULL)
        nd_battery_close(ui->battery);
    if (ui->modem != NULL && nd_modem_close != NULL)
        nd_modem_close(ui->modem);
    ui->notify = NULL;
    ui->battery = NULL;
    ui->modem = NULL;

    if (ui->input != NULL)
        nd_input_close(ui->input);
    ui->input = NULL;
    ui->keypad_fd = -1;

    nd_imgcache_free(ui->image_cache);
    ui->image_cache = NULL;
    nd_layout_free(ui->home_.home_layout);
    ui->home_.home_layout = NULL;
    nd_image_free(ui->home_.wallpaper);
    ui->home_.wallpaper = NULL;

    nd_font_free(ui->font_s);
    nd_font_free(ui->font_md);
    nd_font_free(ui->font_n);
    nd_font_free(ui->font_xl);
    ui->font_s = NULL;
    ui->font_md = NULL;
    ui->font_n = NULL;
    ui->font_xl = NULL;

    nd_image_free(ui->scratch);
    ui->scratch = NULL;
    free(ui->draw);
    ui->draw = NULL;
    nd_image_free(ui->canvas);
    ui->canvas = NULL;
    free(ui->softkey);
    ui->softkey = NULL;
    ui->softkey_exists = false;
    ui->fb = NULL;
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

nd_err nd_ui_present(nd_ui *ui)
{
    if (ui == NULL || ui->fb == NULL || ui->canvas == NULL)
        return ND_ERR_INVAL;
    return nd_fb_update(ui->fb, ui->canvas);
}

/* _battery_tick: the 3.20 V cutoff holds system-wide because every screen
 * funnels through read_keypress. The poll is rate-limited inside the service. */
static void battery_tick(nd_ui *ui)
{
    const char *event;

    if (ui->shutting_down || ui->battery == NULL || nd_battery_poll == NULL)
        return;
    event = nd_battery_poll(ui->battery, false);
    if (event != NULL && strcmp(event, "shutdown") == 0)
        ui->shutting_down = true;
}

static bool handle_modem_event(nd_ui *ui, const nd_modem_event *ev)
{
    nd_sms_rec rec;
    int64_t row_id;

    if (ev->kind == ND_MODEM_EV_SMS) {
        nd_sms_st st;

        if (nd_modem_fetch_sms == NULL)
            return true;
        st = nd_modem_fetch_sms(ui->modem, ev->index, &rec);
        if (st == ND_SMS_BUSY) {
            if (nd_modem_requeue_event != NULL)
                nd_modem_requeue_event(ui->modem, ev);
            return false; /* port busy: retry next tick */
        }
        if (st == ND_SMS_OK) {
            row_id = nd_db_store_incoming_sms(rec.sender, rec.text);
            if (ui->notify != NULL && nd_notify_post_sms != NULL)
                nd_notify_post_sms(ui->notify, row_id, true);
            nd_ui_set_unread_sms(ui, nd_ui_unread_sms(ui) + 1);
        }
    }
    /* Call events are left to _ring_tick and handle_incoming_call. */
    return true;
}

/* _modem_tick, minus the poll: OPEN-QUESTIONS decision 1 moved the polling
 * onto the modem's own thread, so the UI only drains the event queue. */
static void modem_tick(nd_ui *ui)
{
    nd_modem_event ev;

    if (ui->modem == NULL || nd_modem_take_pending_event == NULL)
        return;
    while (nd_modem_take_pending_event(ui->modem, &ev)) {
        if (!handle_modem_event(ui, &ev))
            break;
    }
}

/* _ring_tick. Returns true when the Python would have raised IncomingCall. */
static bool ring_tick(nd_ui *ui)
{
    const char *caller;

    if (ui->handling_call)
        return false;
    if (ui->modem == NULL || nd_modem_state == NULL)
        return false;
    if (nd_modem_state(ui->modem) != ND_CALL_RINGING) {
        g_ring_seen_at = 0.0;
        return false;
    }
    if (g_ring_seen_at == 0.0)
        g_ring_seen_at = nd_time_monotonic();

    /* +CLIP lands a beat after RING; give it one poll cycle so the screen
     * opens with the caller's name instead of "Unknown". */
    caller = nd_modem_caller_id != NULL ? nd_modem_caller_id(ui->modem) : NULL;
    if (caller == NULL && (nd_time_monotonic() - g_ring_seen_at) < 0.6)
        return false;
    return true;
}

int32_t nd_ui_read_keypress(nd_ui *ui, double timeout_s)
{
    if (ui == NULL)
        return ND_KEY_NONE;

    /* In the CORE these run first, every call. In an APP all three services
     * are NULL and every one is a no-op, which is the plain read nd_ui.h
     * promises. */
    battery_tick(ui);
    modem_tick(ui);
    if (ring_tick(ui))
        return ND_KEY_INCOMING_CALL;

    if (ui->input == NULL) {
        /* Nothing above waited, so sleep out the timeout rather than
         * busy-looping at 100% CPU inside wait_for_key(). */
        if (timeout_s > 0.0) {
            struct timespec ts;

            ts.tv_sec = (time_t)timeout_s;
            ts.tv_nsec = (long)((timeout_s - (double)ts.tv_sec) * 1e9);
            (void)nanosleep(&ts, NULL);
        }
        return ND_KEY_NONE;
    }
    return nd_input_read_key(ui->input, timeout_s);
}

int32_t nd_ui_wait_for_key(nd_ui *ui)
{
    for (;;) {
        int32_t key = nd_ui_read_keypress(ui, 0.1);

        if (key != ND_KEY_NONE)
            return key;
    }
}

/* ------------------------------------------------------------------ *
 * The home screen
 * ------------------------------------------------------------------ */

static int32_t floordiv2(int32_t v)
{
    return v >= 0 ? v / 2 : -(((-v) + 1) / 2);
}

void nd_ui_render_home(nd_ui *ui)
{
    int32_t w;
    int32_t h;
    bool notify_active;
    size_t i;
    const nd_image *paper;
    const nd_home_layout *layout;

    if (ui == NULL || ui->canvas == NULL || ui->draw == NULL)
        return;
    w = nd_ui_width(ui);
    h = nd_ui_height(ui);
    notify_active = nd_ui_status_notify_active(ui);
    paper = nd_ui_wallpaper(ui);
    layout = nd_ui_home_layout(ui);

    /* --- 1. background --- */
    if (paper != NULL) {
        (void)nd_image_blit(ui->canvas, paper, 0, 0);
    } else if (layout != NULL && layout->background != NULL) {
        (void)nd_image_blit(ui->canvas, layout->background, 0, 0);
    } else {
        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, w, h), ND_BLACK);
    }

    /* --- 2. elements, in array order, which is paint order --- */
    if (layout != NULL) {
        for (i = 0u; i < layout->n_elements; i++) {
            const nd_element *el = &layout->elements[i];

            /* The carrier line makes room for the "N messages received"
             * banner, like on the 3310. */
            if (notify_active && el->type == ND_EL_TEXT && strcmp(el->text, "No Service") == 0)
                continue;
            nd_home_render_element(ui, el);
        }
    } else if (ui->font_s != NULL) {
        /* The Python passes no font here and gets Pillow's built-in bitmap
         * face; see U-2 in OPEN-QUESTIONS.md. */
        (void)nd_draw_text(ui->draw, 10, 10, "No Layout Found", ui->font_s, ND_RGB(255, 0, 0));
    }

    /* --- 3. notification layer: a flashing envelope while unread mail
     * exists, a banner while it is undismissed --- */
    if (notify_active || nd_ui_unread_sms(ui) > 0) {
        /* 500 ms on, 500 ms off. */
        if (((int64_t)(nd_time_now() * 2.0)) % 2 == 0) {
            double icon_scale = (double)h / 240.0;
            const nd_image *env = nd_ui_get_image_scaled(ui, ND_PATH_ENVELOPE, icon_scale);

            if (env != NULL) {
                (void)nd_image_blit_alpha(ui->canvas, env, nd_trunc32(46.0 * icon_scale) + 7,
                                          nd_trunc32(10.0 * icon_scale));
            }
        }
    }
    if (notify_active && ui->font_n != NULL) {
        char l1[ND_NOTIFY_LINE_MAX];
        char l2[ND_NOTIFY_LINE_MAX];
        size_t n = nd_ui_status_banner_lines(ui, l1, l2);
        const char *lines[2];
        int32_t y = nd_max32(46, nd_trunc32((double)nd_ui_content_bottom(ui) * 0.34));

        lines[0] = l1;
        lines[1] = l2;
        for (i = 0u; i < n && i < 2u; i++) {
            (void)nd_draw_text(ui->draw, 30, y, lines[i], ui->font_n, ND_WHITE);
            y += 24;
        }
    }
}

void nd_ui_render_home_dialing(nd_ui *ui)
{
    if (ui == NULL || ui->canvas == NULL || ui->draw == NULL)
        return;

    {
        const nd_image *paper = nd_ui_wallpaper(ui);

        if (paper != NULL)
            (void)nd_image_blit(ui->canvas, paper, 0, 0);
        else
            (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_height(ui)),
                                    ND_BLACK);
    }

    /* No status icons, no clock and no carrier on this screen. */
    if (ui->dial_buffer[0] != '\0' && ui->font_xl != NULL) {
        int32_t w = 0;
        int32_t h = 0;
        int32_t y = nd_max32(50, nd_trunc32((double)nd_ui_content_bottom(ui) * 0.35));

        nd_ui_text_size(ui, ui->dial_buffer, ui->font_xl, &w, &h);
        (void)nd_draw_text(ui->draw, floordiv2(nd_ui_width(ui) - w), y, ui->dial_buffer,
                           ui->font_xl, ND_WHITE);
    }
}

/* ------------------------------------------------------------------ *
 * Menu, app launch, and the per-frame dispatcher
 * ------------------------------------------------------------------ */

void nd_ui_render_menu(nd_ui *ui)
{
    nd_appsel menu;
    int32_t choice;
    const nd_app_entry *apps;
    size_t n_apps = 0u;

    if (ui == NULL)
        return;
    if (nd_appsel_init == NULL || nd_appsel_show == NULL) {
        nd_log_err(ND_LOG_OS, "Menu unavailable: no app selector linked.");
        ui->state = ND_UI_STATE_HOME;
        return;
    }

    apps = nd_ui_app_list(ui, &n_apps);
    nd_appsel_init(&menu, ui, "Main Menu", apps, n_apps, nd_ui_wallpaper(ui));
    choice = nd_appsel_show(&menu);
    if (choice != ND_WIDGET_BACK && (size_t)choice < n_apps) {
        /* The Python prints the INDEX here, not the manifest id, despite the
         * wording. Port the message as it is. */
        nd_log(ND_LOG_OS, "Launching App ID: %d", choice);
        /* entry NULL means app_run(), nd_app.h's default entry point. The
         * manifest's "exec" is not passed: with process-per-app the code
         * always lives in ND_APP_SO_NAME beside the manifest, and every
         * shipped manifest still says "main.py". See U-6 in
         * OPEN-QUESTIONS.md. */
        if (nd_proc_launch_app != NULL) {
            (void)nd_proc_launch_app(ui, &apps[choice], NULL, NULL, NULL);
        } else {
            nd_log_err(ND_LOG_OS, "App launcher not linked; ignoring %s", apps[choice].name);
        }
    }
    /* Always unwind, so one bad app or menu event cannot trap the core loop. */
    ui->state = ND_UI_STATE_HOME;
}

void nd_ui_update(nd_ui *ui)
{
    if (ui == NULL)
        return;

    switch (ui->state) {
    case ND_UI_STATE_HOME:
        nd_ui_render_home(ui);
        /* present=false: the softkey bar's own flush is suppressed so exactly
         * ONE framebuffer write happens per home frame. */
        nd_softkey_update(ui->softkey, nd_ui_status_notify_active(ui) ? "Read" : "Menu", false);
        (void)nd_ui_present(ui);
        break;
    case ND_UI_STATE_HOME_DIALING:
        nd_ui_render_home_dialing(ui);
        nd_softkey_update(ui->softkey, "Call", false);
        (void)nd_ui_present(ui);
        break;
    case ND_UI_STATE_MENU:
        nd_ui_render_menu(ui);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The call log
 * ------------------------------------------------------------------ *
 *
 * Every call this phone places or takes passes through this file, so this is
 * the only place a row can be written from: the CallLog app is a separate
 * process that is almost never running when a call arrives, and nd_modem.h has
 * the core signal and reap whatever app WAS running before it draws the call
 * screen. Recording happens where an outcome is finally known: the two dial
 * paths in nd_ui_handle_input(), and the three ways
 * nd_ui_handle_incoming_call() can finish.
 *
 * The Python wrote no rows at all -- `calls` was created by the first READ and
 * stayed empty -- which is why all three lists have said "No numbers" on every
 * phone shipped so far.
 *
 * A DECLINED call is filed as missed, the same as one whose caller gave up.
 * That is the 5110's own behaviour and the reason "Missed calls" is the first
 * entry in the menu: what it means is "calls I did not speak on", not "calls I
 * failed to notice".
 */

/* int(get_setting(key, "0") or 0), folded to 0 on anything unparseable --
 * which is what the app's own py_int() does with the same three keys, and
 * whitespace either side is accepted for the same reason it is there. A total
 * this side cannot read is a total this side then overwrites, so the two
 * readers agreeing about what a value means is worth the four lines. */
static int64_t timer_read(const char *key)
{
    char buf[32];
    const char *p;
    char *end = NULL;
    long long v;

    if (nd_settings_get_copy(key, "0", buf, sizeof buf) != ND_OK)
        return 0;
    v = strtoll(buf, &end, 10);
    if (end == NULL || end == buf)
        return 0;
    for (p = end; *p == ' ' || (*p >= '\t' && *p <= '\r'); p++) {}
    if (*p != '\0')
        return 0;
    return (v > 0) ? (int64_t)v : 0;
}

static void timer_store(const char *key, int64_t secs)
{
    char buf[32];

    if (nd_snprintf(buf, sizeof buf, "%lld", (long long)secs) != ND_OK)
        return;
    if (nd_settings_set(key, buf) != ND_OK)
        nd_log_err(ND_LOG_CALLLOG, "Timer write failed: cannot store %s", key);
}

/* The two totals accumulate and `last` is replaced, which is what the three
 * rows of "Show call duration" have always claimed to be. A call that never
 * connected -- a missed one, or a dial nobody picked up -- leaves all three
 * alone rather than zeroing "Last call duration" with it. */
static void note_duration(const char *type, int64_t secs)
{
    if (secs <= 0)
        return;

    timer_store(ND_SET_CALLLOG_DUR_LAST, secs);

    if (strcmp(type, ND_CALL_TYPE_DIALED) == 0)
        timer_store(ND_SET_CALLLOG_DUR_DIALED, timer_read(ND_SET_CALLLOG_DUR_DIALED) + secs);
    else if (strcmp(type, ND_CALL_TYPE_RECEIVED) == 0)
        timer_store(ND_SET_CALLLOG_DUR_RECEIVED, timer_read(ND_SET_CALLLOG_DUR_RECEIVED) + secs);
}

/* One finished call. secs comes from the modem, which latched it as the line
 * went down -- by the time we get here the call is over and a live readout
 * would say -1.
 *
 * Nothing here can fail in a way the caller should act on. A phone whose
 * userdata partition is full still has to be able to make the next call, so a
 * refused write is logged by nd_db_record_call() and dropped. */
static void record_call(nd_ui *ui, const char *type, const char *number)
{
    int64_t secs = 0;

    if (ui != NULL && ui->modem != NULL && nd_modem_last_call_secs != NULL)
        secs = (int64_t)nd_modem_last_call_secs(ui->modem);

    (void)nd_db_record_call(type, number, (int64_t)nd_time_now(), secs);
    note_duration(type, secs);
}

/* The caller ID as it stands NOW, which is not necessarily what the ring
 * started with: +CLIP lands just after the first RING, and show_incoming()
 * picks it up mid-screen for the same reason. The RING handler clears the
 * field, so this can never be the previous caller. */
static void incoming_number(nd_ui *ui, const char *given, char *out, size_t out_sz)
{
    const char *cid;

    (void)nd_strlcpy(out, (given != NULL) ? given : "", out_sz);
    if (out[0] != '\0')
        return;
    if (ui == NULL || ui->modem == NULL || nd_modem_caller_id == NULL)
        return;
    cid = nd_modem_caller_id(ui->modem);
    if (cid != NULL)
        (void)nd_strlcpy(out, cid, out_sz);
}

/* ------------------------------------------------------------------ *
 * Keys
 * ------------------------------------------------------------------ */

static void play_dtmf(nd_ui *ui, char ch)
{
    char path[ND_PATH_MAX];
    const char *name = NULL;
    char digit[2];

    if (ui->notify == NULL || nd_notify_play_tone == NULL)
        return;
    if (ch == '*')
        name = "star";
    else if (ch == '#')
        name = "hash";
    else if (ch >= '0' && ch <= '9') {
        digit[0] = ch;
        digit[1] = '\0';
        name = digit;
    } else {
        return;
    }
    if (nd_snprintf(path, sizeof path, "%s/%s.wav", ND_PATH_DTMF_DIR, name) != ND_OK)
        return;
    (void)nd_notify_play_tone(ui->notify, path);
}

/* NotifyService "Read" softkey: jump to the new message, or the inbox.
 *
 * _open_notification reads kind, count and latest_data BEFORE dismissing --
 * dismiss() zeroes all three -- and only then decides which of Messages' two
 * entry points to call. Keep that order. */
static void open_notification(nd_ui *ui)
{
    const nd_app_entry *messages = NULL;
    const char *kind = ND_NOTIFY_KIND_SMS;
    int32_t count = 1;
    int64_t target = -1;
    char arg[32];
    size_t i;

    if (ui->notify != NULL) {
        kind = nd_notify_kind != NULL ? nd_notify_kind(ui->notify) : NULL;
        count = nd_notify_count != NULL ? nd_notify_count(ui->notify) : 0;
        target = nd_notify_latest_data != NULL ? nd_notify_latest_data(ui->notify) : -1;
        if (nd_notify_dismiss != NULL)
            nd_notify_dismiss(ui->notify);
    } else {
        nd_ui_sim_sms_banner(ui, 0);
    }

    /* `if kind != "sms": return` -- the banner is dismissed either way. */
    if (kind == NULL || strcmp(kind, ND_NOTIFY_KIND_SMS) != 0)
        return;

    for (i = 0u; i < ui->home_.n_apps; i++) {
        if (strcmp(ui->home_.apps[i].name, "Messages") == 0) {
            messages = &ui->home_.apps[i];
            break;
        }
    }
    if (messages == NULL || nd_proc_launch_app == NULL) {
        /* The Python falls back to the hard-coded Messages path here; with
         * process-per-app there is nothing to fall back to, so all that is
         * left is to re-count. */
        ui->home_.unread_sms_ready = false;
        return;
    }

    /* nd_app.h splits the two entry points the Python called by name:
     * open_message(ui, id) for a single new text, open_inbox(ui) otherwise. */
    if (count == 1 && target >= 0 &&
        nd_snprintf(arg, sizeof arg, "%lld", (long long)target) == ND_OK) {
        (void)nd_proc_launch_app(ui, messages, ND_APP_ENTRY_OPEN_MESSAGE, arg, NULL);
    } else {
        (void)nd_proc_launch_app(ui, messages, ND_APP_ENTRY_OPEN_INBOX, NULL, NULL);
    }
    ui->home_.unread_sms_ready = false;
}

void nd_ui_handle_input(nd_ui *ui, int32_t code)
{
    char ch;

    if (ui == NULL)
        return;

    /* The banner owns enter and C on the home screen: Read opens the message,
     * C clears the banner (the mail stays unread and the envelope keeps
     * flashing). Everything else acts as usual. */
    if (ui->state == ND_UI_STATE_HOME && nd_ui_status_notify_active(ui)) {
        if (code == ND_KEY_ENTER) {
            open_notification(ui);
            return;
        }
        if (code == ND_KEY_CLEAR) {
            if (ui->notify != NULL && nd_notify_dismiss != NULL)
                nd_notify_dismiss(ui->notify);
            else
                nd_ui_sim_sms_banner(ui, 0);
            return;
        }
    }

    if (code == ND_KEY_ENTER) {
        if (ui->state == ND_UI_STATE_HOME) {
            ui->state = ND_UI_STATE_MENU;
        } else if (ui->state == ND_UI_STATE_HOME_DIALING) {
            if (ui->modem != NULL && nd_modem_dial != NULL)
                (void)nd_modem_dial(ui->modem, ui->dial_buffer);
            if (nd_dialer_show_calling != NULL)
                nd_dialer_show_calling(ui, ui->dial_buffer, NULL);
            /* Logged whether or not the modem took the dial. "Dialed calls"
             * is a record of what was dialled from this handset, and a number
             * that failed to connect is the one you most want to find again. */
            record_call(ui, ND_CALL_TYPE_DIALED, ui->dial_buffer);
            ui->dial_buffer[0] = '\0';
            ui->state = ND_UI_STATE_HOME;
        }
        return;
    }

    if (code == ND_KEY_CLEAR) {
        if (ui->state == ND_UI_STATE_HOME_DIALING) {
            size_t n = strlen(ui->dial_buffer);

            if (n > 0u)
                ui->dial_buffer[n - 1u] = '\0';
            if (ui->dial_buffer[0] == '\0')
                ui->state = ND_UI_STATE_HOME;
        }
        return;
    }

    if ((code == ND_KEY_UP || code == ND_KEY_DOWN) && ui->state == ND_UI_STATE_HOME) {
        nd_contact target;

        if (nd_contacts_show_selector != NULL &&
            nd_contacts_show_selector(ui, "Select", "Call", &target)) {
            if (ui->modem != NULL && nd_modem_dial != NULL)
                (void)nd_modem_dial(ui->modem, target.number);
            if (nd_dialer_show_calling != NULL)
                nd_dialer_show_calling(ui, target.number, target.name);
            /* The NUMBER, not the contact's name: the log stores numbers and
             * the app resolves them, so a contact renamed tomorrow does not
             * rewrite yesterday's calls. */
            record_call(ui, ND_CALL_TYPE_DIALED, target.number);
        }
        return;
    }

    ch = nd_key_dial_char(code);
    if (ch != '\0' && (ui->state == ND_UI_STATE_HOME || ui->state == ND_UI_STATE_HOME_DIALING)) {
        size_t n = strlen(ui->dial_buffer);

        if (n + 1u < sizeof ui->dial_buffer) {
            ui->dial_buffer[n] = ch;
            ui->dial_buffer[n + 1u] = '\0';
        }
        ui->state = ND_UI_STATE_HOME_DIALING;
        play_dtmf(ui, ch);
    }
}

/* ------------------------------------------------------------------ *
 * Calls and the low-battery modal
 * ------------------------------------------------------------------ */

void nd_ui_handle_incoming_call(nd_ui *ui, const char *number)
{
    /* ND_CALL_GONE is the Python's "the caller hung up before we answered",
     * which is also what happens when the Dialer is not linked in. */
    nd_incoming_result result = ND_CALL_GONE;
    char logged[ND_MODEM_NUMBER_MAX];

    if (ui == NULL)
        return;
    ui->handling_call = true;
    ui->state = ND_UI_STATE_HOME;

    nd_log(ND_LOG_CORE, "Incoming call from %s",
           (number != NULL && number[0] != '\0') ? number : "unknown");
    if (ui->notify != NULL && nd_notify_start_ring != NULL)
        (void)nd_notify_start_ring(ui->notify);

    if (nd_dialer_show_incoming != NULL)
        result = nd_dialer_show_incoming(ui, number, NULL);

    if (ui->notify != NULL && nd_notify_stop_ring != NULL)
        nd_notify_stop_ring(ui->notify);

    /* Read AFTER the ring screen, not before: `number` is NULL on the path the
     * core actually uses (nd_main.c passes it), and a +CLIP arriving during
     * the ring is the only thing that ever names the caller. */
    incoming_number(ui, number, logged, sizeof logged);

    if (result == ND_CALL_ANSWERED) {
        bool ok = ui->modem != NULL && nd_modem_answer != NULL && nd_modem_answer(ui->modem);

        if (ok) {
            if (nd_dialer_show_calling != NULL)
                nd_dialer_show_calling(ui, number != NULL ? number : "", NULL);
        } else {
            nd_log(ND_LOG_CORE, "Answer failed; releasing the call.");
        }
        /* show_calling returns on End or on a remote hangup; make sure the
         * line is really down either way. */
        if (ui->modem != NULL && nd_modem_hangup != NULL)
            (void)nd_modem_hangup(ui->modem);
        /* AFTER the hangup, because that is what latches the duration. An
         * answer the modem refused never became a call, so it is filed where
         * a call you did not speak on belongs. */
        record_call(ui, ok ? ND_CALL_TYPE_RECEIVED : ND_CALL_TYPE_MISSED, logged);
    } else if (result == ND_CALL_DECLINED) {
        nd_log(ND_LOG_CORE, "Call declined.");
        if (ui->modem != NULL && nd_modem_hangup != NULL)
            (void)nd_modem_hangup(ui->modem);
        record_call(ui, ND_CALL_TYPE_MISSED, logged);
    } else {
        nd_log(ND_LOG_CORE, "Caller hung up before we answered.");
        record_call(ui, ND_CALL_TYPE_MISSED, logged);
    }

    if (ui->notify != NULL && nd_notify_stop_ring != NULL)
        nd_notify_stop_ring(ui->notify);
    ui->handling_call = false;
    ui->state = ND_UI_STATE_HOME;
}

void nd_ui_show_pending_battery_warning(nd_ui *ui)
{
    const char *warning;
    const char *message;
    nd_msgdialog dlg;
    double vcell = 0.0;

    if (ui == NULL)
        return;
    /* Deferred to the home loop so a modal never lands mid-frame inside an
     * app; latched warnings pop as soon as we are back on HOME. */
    if (ui->state != ND_UI_STATE_HOME && ui->state != ND_UI_STATE_HOME_DIALING)
        return;
    if (ui->battery == NULL || nd_battery_take_pending_warning == NULL)
        return;
    warning = nd_battery_take_pending_warning(ui->battery);
    if (warning == NULL)
        return;

    message =
        strcmp(warning, ND_BATT_WARN_CRITICAL) == 0 ? "BATTERY CRITICALLY LOW!" : "LOW BATTERY!";
    if (nd_battery_vcell != NULL)
        (void)nd_battery_vcell(ui->battery, &vcell);
    nd_log(ND_LOG_BATT, "Warning: %s (VCELL=%.3f V)", message, vcell);

    if (nd_msgdialog_init == NULL || nd_msgdialog_show == NULL)
        return;
    nd_msgdialog_init(&dlg, ui, message);
    if (nd_msgdialog_set_title != NULL)
        nd_msgdialog_set_title(&dlg, "Battery");
    if (nd_msgdialog_set_icon != NULL)
        nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
    if (nd_msgdialog_set_button != NULL)
        nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);
}

/* ------------------------------------------------------------------ *
 * After every app exit
 * ------------------------------------------------------------------ */

/* OPEN-QUESTIONS decision 3. Settings no longer writes into the core's live
 * memory; it writes the setting and the core re-reads it here, exactly as
 * launch_app already re-read the unread-SMS count. Nothing an app changed is
 * observable before the app returns, so this is behaviour-identical. */
void nd_ui_refresh_after_app(nd_ui *ui)
{
    if (ui == NULL)
        return;

    nd_image_free(ui->home_.wallpaper);
    ui->home_.wallpaper = NULL;
    ui->home_.wallpaper_ready = false;
    ui->home_.eng_mode_ready = false;
    ui->home_.apps_ready = false;

    /* Messages may have been read (or arrived) inside the app. */
    ui->home_.unread_sms_ready = false;
}
