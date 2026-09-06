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
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_app.h"
#include "nd_battery.h"
#include "nd_bench.h"
#include "nd_calendar.h"
#include "nd_crash.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_gif.h"
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
#include "nd_text.h"
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
#pragma weak nd_modem_link_state
#pragma weak nd_modem_take_pending_fault
#pragma weak nd_modem_state
#pragma weak nd_modem_caller_id
#pragma weak nd_modem_take_pending_event
#pragma weak nd_modem_requeue_event
#pragma weak nd_modem_fetch_sms
#pragma weak nd_modem_read_stored_sms
#pragma weak nd_modem_dial
#pragma weak nd_modem_answer
#pragma weak nd_modem_hangup

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
#pragma weak nd_notify_post_event
#pragma weak nd_notify_start_ring
#pragma weak nd_notify_stop_ring
#pragma weak nd_notify_play_tone

/* The calendar store. Weak for the same reason everything else here is: the
 * core is linked in configurations that do not carry every module, and a
 * phone with no calendar simply never notices one. */
#pragma weak nd_cal_due
#pragma weak nd_cal_mark_notified

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
#pragma weak nd_dialer_place_call
#pragma weak nd_dialer_answer_call

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

/* Which kind of banner is up, NULL for none. Not in nd_ui_sim.h because
 * nothing outside this file asks: the home screen needs it to decide between
 * an envelope and no envelope, the softkey needs it to decide between "Read"
 * and "View", and _open_notification needs it to decide which app to launch.
 *
 * The simulation hook only ever stands in for a TEXT banner -- it is
 * nd_ui_sim_sms_banner() and the golden frames it captures are message
 * banners -- so the fallback says "sms" and the reference frames are
 * untouched by any of this. */
static const char *banner_kind(const nd_ui *ui)
{
    if (ui != NULL && ui->notify != NULL && nd_notify_kind != NULL)
        return nd_notify_kind(ui->notify);
    return g_sim.banner_active ? ND_NOTIFY_KIND_SMS : NULL;
}

static bool banner_is(const nd_ui *ui, const char *kind)
{
    const char *k = banner_kind(ui);

    return k != NULL && strcmp(k, kind) == 0;
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

/* Everything that turns a decoded picture into a wallpaper: to RGB888, to the
 * panel's size with LANCZOS, then down to 30% brightness. Takes ownership of
 * `img` either way, so the two callers -- a still file and one frame of an
 * animation -- cannot disagree about what a wallpaper looks like. */
static nd_image *wallpaper_from_image(nd_image *img)
{
    nd_image *rgb;
    nd_image *scaled;

    if (img == NULL)
        return NULL;

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

/* The same three steps as wallpaper_from_image(), but INTO AN EXISTING
 * SURFACE. Two reasons it has to work this way for the animation:
 *
 *   1. Every consumer of nd_ui_wallpaper() holds the pointer. nd_appsel keeps
 *      it for the whole life of the menu (nd_appsel_init takes it as an
 *      argument), and the softkey bar reads it every repaint. Freeing the
 *      image and installing a new one on each frame would make all of those
 *      correct only for as long as nobody ever ticks the wallpaper from
 *      inside a widget loop -- an invariant nothing enforces and a plausible
 *      future change would break.
 *   2. CODING-STANDARDS.md section 4: nothing allocates in the render path.
 *      A panel-sized GIF, which is what the shipped wallpaper is and what the
 *      Settings help tells people to use, now allocates NOTHING per frame.
 *
 * `dst` must be ND_UI_W x ND_UI_H RGB888. A frame that is not panel-sized
 * still pays one LANCZOS temporary -- that is the case open_wallpaper logs a
 * warning about, and it is the resampling rather than the malloc that costs. */
static bool wallpaper_paint_frame(nd_image *dst, const nd_image *frame)
{
    if (dst == NULL || frame == NULL)
        return false;

    if (frame->w == dst->w && frame->h == dst->h) {
        /* RGBA8888 -> RGB888, dropping alpha without compositing, which is
         * what PIL does and what the decoder's transparent-black clear was
         * chosen to make correct: an undrawn GIF pixel lands as black. */
        if (nd_image_blit(dst, frame, 0, 0) != ND_OK)
            return false;
    } else {
        /* The expensive branch, and the reason open_wallpaper logs a warning
         * for a GIF that is not panel-sized.
         *
         * FLATTEN TO RGB888 FIRST. nd_image_resize_lanczos() on an RGBA
         * source premultiplies the whole source and unpremultiplies the whole
         * output around the resample (nd_resample.c) -- two extra full-image
         * passes and an extra copy, per frame, for an alpha channel that is
         * about to be discarded anyway. Converting first drops both passes
         * and shrinks every remaining one by a quarter. */
        nd_image *flat = nd_image_convert(frame, ND_PIXFMT_RGB888);
        nd_image *scaled;
        nd_err rc;

        if (flat == NULL)
            return false;
        scaled = nd_image_resize_lanczos(flat, dst->w, dst->h);
        nd_image_free(flat);
        if (scaled == NULL)
            return false;
        rc = nd_image_blit(dst, scaled, 0, 0);
        nd_image_free(scaled);
        if (rc != ND_OK)
            return false;
    }

    return nd_image_brightness(dst, 0.3) == ND_OK;
}

/* owned by the caller; free with nd_image_free() */
nd_image *nd_ui_load_wallpaper(const char *path)
{
    nd_image *img;

    if (path == NULL || !nd_path_exists(path)) {
        nd_log(ND_LOG_UI, "No wallpaper found.");
        return NULL;
    }

    nd_log(ND_LOG_UI, "Loading wallpaper: %s", path);

    /* LOAD_TRUNCATED_IMAGES is set globally by the Python at import; the C
     * decoder already decodes as far as a truncated JPEG got. A .gif arrives
     * here as its first frame -- nd_image.h's GIF branch -- which is exactly
     * what a still preview of an animated wallpaper should be. */
    img = nd_image_open(path);
    if (img == NULL) {
        nd_log(ND_LOG_UI, "Wallpaper load error: %s", path);
        return NULL;
    }
    return wallpaper_from_image(img);
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

/* The extensions system.ui.wallpaper will accept. Kept beside the loader
 * rather than in the Settings app, because the SETTING is what has to be
 * valid -- Settings only offers a list, and a value can also arrive from a
 * restored backup or a hand-edited settings.prop. */
static bool wallpaper_ext_ok(const char *path)
{
    return ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg") || ends_with_ci(path, ".gif");
}

/* Is an animation allowed to run in THIS process, under the configured mode?
 *
 * nd_app_dir() is "" in the core and the app's own directory in an app
 * process, which is the only distinction HOME needs and one the context
 * already carries -- see nd_app.h. */
static bool anim_allowed_here(nd_ui *ui)
{
    switch (nd_ui_anim_mode_of(ui)) {
    case ND_UI_ANIM_OFF:
        return false;
    case ND_UI_ANIM_HOME:
        return nd_app_dir()[0] == '\0';
    case ND_UI_ANIM_ALWAYS:
    default:
        return true;
    }
}

/* A .gif wallpaper, taken from ONE decoder.
 *
 * The obvious shape -- load the still with nd_ui_load_wallpaper(), then open a
 * decoder for the animation -- decodes the file twice and, worse, leaves the
 * decoder sitting on frame 0 with frame 0 already on screen, so the first tick
 * repaints the picture that is already there. One decoder: pull frame 0 for
 * the still, keep it open for everything after.
 *
 * A single-frame GIF closes it again. There is nothing to advance and 226 KB
 * held open to animate one frame is 226 KB spent on nothing.
 *
 * Returns false when the file is not a GIF or would not decode, in which case
 * nothing has been assigned and the ordinary still path should run. */
static bool load_gif_wallpaper(nd_ui *ui, const char *path)
{
    nd_gif *g;
    const nd_image *frame;
    nd_image *still;
    int32_t delay_ms = ND_GIF_DEFAULT_DELAY_MS;

    if (!ends_with_ci(path, ".gif"))
        return false;

    g = nd_gif_open(path);
    if (g == NULL)
        return false;

    frame = nd_gif_next(g, &delay_ms);
    still = frame != NULL ? wallpaper_from_image(nd_image_copy(frame)) : NULL;
    if (still == NULL) {
        nd_gif_close(g);
        return false;
    }
    ui->home_.wallpaper = still;

    /* Three reasons not to keep the decoder, all of which mean the same
     * thing: no second frame will ever be shown here, so holding it open
     * would cost 226 KB and a descriptor on the SD card for nothing.
     *
     *   - the file has one frame;
     *   - this context runs no loop that could advance it (a test fixture);
     *   - system.ui.wpanimate says not here. OFF means nowhere; HOME means
     *     the core only, and an app under HOME is exactly the case this is
     *     worth checking for, because an app would otherwise carry the
     *     decoder through its whole life to show one frame.
     */
    if (!nd_gif_animated(g) || !ui->drives_wallpaper || !anim_allowed_here(ui)) {
        nd_gif_close(g);
        return true;
    }

    nd_log(ND_LOG_UI, "Animated wallpaper: %zu frames, %d ms, %dx%d", nd_gif_frame_count(g),
           nd_gif_duration_ms(g), nd_gif_width(g), nd_gif_height(g));

    /* A GIF that is not already the panel's size costs a full LANCZOS resample
     * per frame -- 42,000 output pixels through a 3-lobe filter, every frame,
     * forever. It works, and on this CPU it is the difference between a
     * wallpaper and a space heater, so say so once where somebody choosing a
     * file will see it. */
    if (nd_gif_width(g) != ND_UI_W || nd_gif_height(g) != ND_UI_H)
        nd_log(ND_LOG_UI, "Wallpaper is %dx%d, not %dx%d: every frame will be resampled",
               nd_gif_width(g), nd_gif_height(g), ND_UI_W, ND_UI_H);

    ui->home_.wallpaper_gif = g;
    /* Frame 0 is ON SCREEN as of now, so the next one is owed after frame 0's
     * own delay -- not immediately, which would show frame 0 twice. */
    ui->home_.wallpaper_due = nd_time_now() + (double)delay_ms / 1000.0;
    return true;
}

/* Construction step 16, and re-run after every app exit. Assigns
 * ui->home_.wallpaper, and opens the decoder when the file animates. */
static void load_configured_wallpaper(nd_ui *ui)
{
    /* A LANCZOS resize of a full-size photograph to 240x175 and then a dim
     * pass over 126,000 bytes, or the first frame of a GIF off the card. It is
     * the most expensive single thing the home screen does and it happens with
     * nothing yet drawn, which is what makes it worth a name of its own. */
    const char *saved = nd_ui_watch_begin("loading the wallpaper");
    const char *from_core = getenv(ND_ENV_UI_WALLPAPER);
    char setting[ND_PATH_MAX];

    /* ============ WHY THE ENVIRONMENT COMES FIRST ============
     *
     * An app runs as ndusr_ut, and settings.prop is 0640 ndusr:ndusr -- so an
     * installed app reading it gets ENOENT and silently draws on the default
     * background instead of the owner's. Widening the file is not the answer:
     * it also holds engineering_mode, which gates LinuxShell and raw AT.
     *
     * So the core, which CAN read it, projects the presentation subset into
     * the child's environment (nd_proc.h, app_env). The value is the setting
     * verbatim and unparsed, "NONE" included, so the branch below is the same
     * branch for both sources and no parsing policy is duplicated across a
     * fork. Absent means no core set it -- an app started by hand, or the core
     * itself -- and the settings read is then still the right answer. */
    if (from_core != NULL) {
        if (nd_strlcpy(setting, from_core, sizeof setting) >= sizeof setting)
            goto done;
    } else if (nd_settings_get_copy(ND_SET_UI_WALLPAPER, ND_SET_UI_WALLPAPER_DFLT, setting,
                                    sizeof setting) != ND_OK) {
        goto done;
    }

    /* `if wallpaper_setting and wallpaper_setting.upper() != "NONE":` -- and
     * when it IS "NONE", load_wallpaper() is never called, so the
     * "[UI] No wallpaper found." line does not appear either. */
    if (setting[0] == '\0' || eq_ci(setting, "NONE"))
        goto done;

    if (wallpaper_ext_ok(setting) && nd_path_exists(setting)) {
        nd_log(ND_LOG_UI, "Loading wallpaper: %s", setting);
        if (!load_gif_wallpaper(ui, setting))
            ui->home_.wallpaper = nd_ui_load_wallpaper(setting);
        goto done;
    }

    nd_log(ND_LOG_UI, "Invalid wallpaper setting: %s", setting);
    if (nd_path_exists(ND_PATH_WALLPAPER))
        ui->home_.wallpaper = nd_ui_load_wallpaper(ND_PATH_WALLPAPER);

done:
    nd_ui_watch_end(saved);
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

/* An icon is a file UNDER the app's own directory. A subdirectory is fine --
 * "art/big.png" is a manifest the tree already contains -- so the rule is
 * about escaping, not about shape:
 *
 *   absolute      "/etc/shadow" ignores appdir entirely
 *   a ".." step   "../../../../etc/shadow" climbs out of it
 *
 * Rejecting rather than sanitising. A sanitiser has to be right about "..",
 * "//", trailing dots and symlinks; a refusal only has to be right about
 * what it refuses, and the caller falls back to the default name, so a
 * hostile manifest gets exactly the treatment a missing one gets.
 *
 * Symlinks inside the app directory are NOT covered here and do not need to
 * be: following one is the decoder opening a path the app owns, which it
 * could have filled with the same bytes directly. */
static bool icon_path_is_contained(const char *icon)
{
    const char *p;

    if (icon == NULL || icon[0] == '\0' || icon[0] == '/')
        return false;

    for (p = icon; p != NULL; p = strchr(p, '/')) {
        if (*p == '/')
            p++;
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
            return false;
        if (strchr(p, '/') == NULL)
            break;
    }
    return true;
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
        /* The manifest is the APP's file, and under the user apps directory
         * that means it is an attacker's file. "icon" was joined to appdir
         * with no containment at all, so "../../../../etc/shadow" or an
         * absolute path made the core's PNG and JPEG decoders read whatever
         * the manifest named -- as ndusr, in the core, on the menu draw, with
         * no app even launched. Failing to decode is the good case; the
         * decoders are the interesting case.
         *
         * A name, not a path. That is all an icon ever was. */
        icon = nd_json_get_str(root, "icon", "icon.png");
        if (!icon_path_is_contained(icon))
            icon = "icon.png";
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
    /* Three directory walks, one of them ON THE SD CARD, each opening and
     * parsing a manifest.json per app. A healthy card makes it invisible; a
     * card that is failing makes it the longest thing the core does without
     * touching the screen, and the reads are uninterruptible. Named, so a
     * phone that stops here says which card it stopped on. */
    const char *saved = nd_ui_watch_begin("scanning the app directories");
    size_t n;

    ui->home_.n_apps = 0u;
    n = nd_ui_scan_apps(ND_PATH_APPS_DIR, ui->home_.apps, ND_APP_MAX);
    ui->home_.n_apps = n;
    if (nd_ui_engineering_mode(ui) && ui->home_.n_apps < ND_APP_MAX) {
        n = nd_ui_scan_apps(ND_PATH_ENG_APPS_DIR, &ui->home_.apps[ui->home_.n_apps],
                            ND_APP_MAX - ui->home_.n_apps);
        ui->home_.n_apps += n;
    }
    /* Apps the owner installed. Scanned unconditionally -- NOT behind
     * engineering mode -- because this is meant to be an ordinary thing an
     * owner does, and a feature only reachable in engineering mode is a feature
     * nobody uses. What makes that safe is not a gate but the confinement:
     * everything here runs as ndusr_ut with a private mount namespace and no
     * service socket at all. See ND_PATH_USER_APPS_DIR. */
    if (ui->home_.n_apps < ND_APP_MAX) {
        n = nd_ui_scan_apps(ND_PATH_USER_APPS_DIR, &ui->home_.apps[ui->home_.n_apps],
                            ND_APP_MAX - ui->home_.n_apps);
        ui->home_.n_apps += n;
    }
    sort_apps_by_id(ui->home_.apps, ui->home_.n_apps);
    nd_ui_watch_end(saved);
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
        load_configured_wallpaper(ui);
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
        /* Opens SQLite. 60-70 ms of a 130 ms app launch when it was measured
         * on the phone, and unbounded when the database is on storage that has
         * stopped answering -- which is the case that ends up here. */
        const char *saved = nd_ui_watch_begin("counting unread messages");

        ui->home_.unread_sms_ready = true;
        ui->home_.unread_sms = nd_db_count_unread_sms();
        nd_ui_watch_end(saved);
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

    /* An injected still is a still. Leaving a decoder open would let the next
     * tick paint over the picture the caller just handed us, which is a
     * baffling thing for a test or for nd-shoot to have to debug. */
    if (ui->home_.wallpaper_gif != NULL) {
        nd_gif_close(ui->home_.wallpaper_gif);
        ui->home_.wallpaper_gif = NULL;
    }
    ui->home_.wallpaper_gen++;
}

/* ------------------------------------------------------------------ *
 * Animated wallpaper
 * ------------------------------------------------------------------ */

/* Is there an animation in this process at all?
 *
 * A plain field read, and deliberately the FIRST question every caller below
 * asks. The alternatives -- reading system.ui.wpanimate, or calling into the
 * decoder -- both cost something, and nd_settings_get() costs a rewrite of
 * settings.prop whatever key it is handed (the R-24 quirk in nd_settings.h).
 * An app that opted out of the wallpaper never opens a decoder and must go on
 * paying nothing, including from the key poll. */
static bool has_animation(const nd_ui *ui)
{
    return ui->home_.wallpaper_ready && ui->home_.wallpaper_gif != NULL;
}

bool nd_ui_tick_wallpaper(nd_ui *ui)
{
    const nd_image *frame;
    int32_t delay_ms = ND_GIF_DEFAULT_DELAY_MS;
    double now;

    if (ui == NULL)
        return false;
    /* Deliberately not nd_ui_wallpaper(): the tick must not be what FORCES
     * the lazy load. Every app process runs a loop; if ticking loaded the
     * wallpaper, every app would pay the load it was made lazy to avoid. */
    if (!has_animation(ui))
        return false;

    now = nd_time_now();
    if (now < ui->home_.wallpaper_due)
        return false;

    frame = nd_gif_next(ui->home_.wallpaper_gif, &delay_ms);
    if (frame == NULL) {
        /* Unplayable after all. Stop asking; whatever still is on screen
         * stays there rather than the wallpaper vanishing mid-use. */
        nd_gif_close(ui->home_.wallpaper_gif);
        ui->home_.wallpaper_gif = NULL;
        return false;
    }

    /* BEFORE the paint, and that ordering is load-bearing.
     *
     * Scheduled from NOW, not from the previous due time: a phone that spent
     * 400 ms inside a dialog must not then race through ten frames catching
     * up. Dropping frames is the right failure for a background.
     *
     * And it moves even if the paint below fails, which is the part worth
     * being careful about. A caller that asked because nd_ui_frame_timeout()
     * said zero will ask again the instant this returns; if a failed paint
     * left the deadline in the past, that is a 100% CPU spin -- and the way
     * the paint fails is an allocation failure on the non-panel-sized path,
     * so the spin would arrive exactly when the phone was already short of
     * memory. Moving the deadline first costs one dropped frame instead. */
    ui->home_.wallpaper_due = now + (double)delay_ms / 1000.0;

    /* IN PLACE -- see wallpaper_paint_frame(). The pointer every consumer
     * holds never changes for the life of the wallpaper, and a panel-sized
     * GIF allocates nothing at all per frame. */
    if (!wallpaper_paint_frame(ui->home_.wallpaper, frame))
        return false;
    ui->home_.wallpaper_gen++;
    return true;
}

double nd_ui_frame_timeout(nd_ui *ui, double dflt)
{
    double left;

    if (ui == NULL || !has_animation(ui))
        return dflt;

    left = ui->home_.wallpaper_due - nd_time_now();
    if (left < 0.0)
        left = 0.0;
    return left < dflt ? left : dflt;
}

/* ------------------------------------------------------------------ *
 * The background under the framework's own chrome
 * ------------------------------------------------------------------ */

/* Both settings, read once per process and again after every app exit.
 * Deliberately NOT read per frame: nd_settings_get() carries the R-24
 * write-on-read quirk (nd_settings.h), so a widget that consulted it while
 * clearing its background would turn every repaint into a flash write. */
static void chrome_settings_load(nd_ui *ui)
{
    const char *raw;
    const char *from_core;
    char *end = NULL;
    double v;

    ui->home_.chrome_ready = true;

    /* Both of these prefer the core's projection for the reason spelled out in
     * load_configured_wallpaper(): an ndusr_ut app cannot read settings.prop,
     * and drawing the framework's chrome the way the owner asked for it is not
     * worth widening a file that also carries engineering_mode. */
    from_core = getenv(ND_ENV_UI_WP_EVERYWHERE);
    ui->home_.chrome_enabled = nd_setting_is_enabled(
        from_core != NULL ? from_core
                          : nd_settings_get(ND_SET_UI_WP_EVERYWHERE, ND_SET_UI_WP_EVERYWHERE_DFLT),
        true);

    raw = getenv(ND_ENV_UI_WP_DIM);
    if (raw == NULL)
        raw = nd_settings_get(ND_SET_UI_WP_APP_DIM, ND_SET_UI_WP_APP_DIM_DFLT);
    if (raw == NULL)
        raw = ND_SET_UI_WP_APP_DIM_DFLT;
    v = strtod(raw, &end);
    /* strtod says "nothing consumed" by leaving end where it started. A value
     * outside [0,1] is a typo rather than a preference -- 0 is a black screen
     * and above 1 brightens a picture that was dimmed for contrast -- so both
     * fall back to the default instead of being clamped to an extreme nobody
     * can then explain. */
    if (end == raw || !(v >= 0.0) || v > 1.0)
        v = strtod(ND_SET_UI_WP_APP_DIM_DFLT, NULL);
    ui->home_.chrome_dim = v;

    /* Three words rather than a boolean, and compared here rather than
     * through one of nd_settings.h's three boolean parsers, because it is not
     * a boolean: see the setting's comment. Anything unrecognised is the
     * default, which is the prettiest of the three -- a typo must not quietly
     * turn the feature off. */
    raw = nd_settings_get(ND_SET_UI_WP_ANIMATE, ND_SET_UI_WP_ANIMATE_DFLT);
    if (raw != NULL && eq_ci(raw, "HOME"))
        ui->home_.anim_mode = ND_UI_ANIM_HOME;
    else if (raw != NULL && (eq_ci(raw, "OFF") || eq_ci(raw, "NEVER")))
        ui->home_.anim_mode = ND_UI_ANIM_OFF;
    else
        ui->home_.anim_mode = ND_UI_ANIM_ALWAYS;
}

nd_ui_anim_mode nd_ui_anim_mode_of(nd_ui *ui)
{
    if (ui == NULL)
        return ND_UI_ANIM_ALWAYS;
    if (!ui->home_.chrome_ready)
        chrome_settings_load(ui);
    return ui->home_.anim_mode;
}

void nd_ui_invalidate_chrome(nd_ui *ui)
{
    if (ui == NULL)
        return;
    nd_image_free(ui->home_.chrome);
    ui->home_.chrome = NULL;
    ui->home_.chrome_ready = false;
    ui->home_.chrome_gen = 0u;
}

const nd_image *nd_ui_chrome_wallpaper(nd_ui *ui)
{
    nd_image *paper;

    if (ui == NULL)
        return NULL;
    /* An app that opted out never asks whether a wallpaper exists, so it
     * never pays the lazy load. That is the 154 ms nd_ui.h's "Lazy home
     * state" section is about, and this feature must not spend it. */
    if (!ui->app_use_wallpaper)
        return NULL;

    if (!ui->home_.chrome_ready)
        chrome_settings_load(ui);
    if (!ui->home_.chrome_enabled)
        return NULL;

    paper = nd_ui_wallpaper(ui);
    if (paper == NULL)
        return NULL;

    /* chrome_gen is the wallpaper generation it was built from, plus one, so
     * that zero can keep meaning "never built". */
    if (ui->home_.chrome != NULL && ui->home_.chrome_gen == ui->home_.wallpaper_gen + 1u)
        return ui->home_.chrome;

    nd_image_free(ui->home_.chrome);
    /* 240x175 RGB888 = 126,000 bytes, one per process that draws chrome. */
    ui->home_.chrome = nd_image_copy(paper);
    if (ui->home_.chrome == NULL)
        return NULL;
    if (nd_image_brightness(ui->home_.chrome, ui->home_.chrome_dim) != ND_OK) {
        nd_image_free(ui->home_.chrome);
        ui->home_.chrome = NULL;
        return NULL;
    }
    ui->home_.chrome_gen = ui->home_.wallpaper_gen + 1u;
    return ui->home_.chrome;
}

void nd_ui_paint_chrome(nd_ui *ui, nd_rect r)
{
    const nd_image *paper;

    /* Both, because the two branches below use different ones: the black
     * fill goes through the draw context and the wallpaper goes straight at
     * the canvas the draw context is bound to. */
    if (ui == NULL || ui->draw == NULL || ui->canvas == NULL)
        return;

    paper = nd_ui_chrome_wallpaper(ui);
    if (paper == NULL) {
        (void)nd_draw_rect_fill(ui->draw, r, ND_BLACK);
        return;
    }

    /* The REGION, not the whole picture. A widget clearing rows 0..144 gets
     * the wallpaper's rows 0..144, so the softkey strip below it still lines
     * up with the photograph above it. */
    (void)nd_image_blit_region(ui->canvas, paper, r, r.x0, r.y0);
}

/* The two rectangles the call sites actually pass, spelled the way they spell
 * them: x1 and y1 are ONE PAST the edge here, not the last pixel. That is
 * wrong for an inclusive rectangle and it is what every widget in the project
 * has always written, so the fill has always been clipped by one row and one
 * column. Reproducing the off-by-one rather than fixing it keeps the painted
 * area identical to the black fill it replaces; both are clipped to the same
 * place. See rule 1 in nd_widgets.h -- partial clears are load-bearing. */
void nd_ui_paint_chrome_full(nd_ui *ui)
{
    if (ui != NULL)
        nd_ui_paint_chrome(ui, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_height(ui)));
}

void nd_ui_paint_chrome_content(nd_ui *ui)
{
    if (ui != NULL)
        nd_ui_paint_chrome(ui, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_content_bottom(ui)));
}

/* ------------------------------------------------------------------ *
 * Construction step 13 -- the alpha security notice
 * ------------------------------------------------------------------ */

/* Exactly 5 lines of 5 before, which is to say one word from being cut off
 * without saying so. 4 now. */
#define ALPHA_NOTICE                                                  \
    "This is alpha software. Extremely insecure and unstable. Don't " \
    "store important data on it."

/* Best-effort persistence, exactly as the Python's bare except. */
static void write_security_ack(void)
{
    char resolved[ND_PATH_MAX];
    FILE *f;

    (void)nd_mkdir_p(ND_PATH_USER, 0755u);
    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_ACK_SECURITY) != ND_OK)
        return;
    f = fopen(resolved, "w");
    if (f == NULL)
        return;
    (void)fprintf(f, "%lld", (long long)nd_time_now());
    (void)fclose(f);
}

/* ErrorScreen.show_alpha_security_notice_once(). Returns true when it showed.
 *
 * ============ WHY THE ACK IS WRITTEN BEFORE THE DIALOG BLOCKS ============
 *
 * nd_msgdialog_show() is `for (;;) { key = nd_ui_wait_for_key(...); }` with
 * no timeout, and this is the FIRST thing on the screen of a phone that has
 * just started. So on a phone whose keypad dies -- or never fully arrives --
 * between nd_ui_init()'s backend check and this call, the notice becomes a
 * permanent state: it cannot be dismissed, and because the acknowledgement
 * used to be written only after the dialog RETURNED, it was owed again on the
 * next boot, and the one after that. That is the original 0.5.x "stuck on the
 * alpha notice" symptom, and this cause of it was still live.
 *
 * Writing the file first turns "for ever" into "once". The cost is a phone
 * that loses power while the notice is up never seeing it again, which is a
 * fair trade for a notice, and it is the same trade nd_main.c's
 * ack_security_notice_for_measurement() already makes deliberately.
 *
 * Everything that decides NOT to show it still happens before the write, so
 * a build with no widget layer linked still owes the notice rather than
 * silently acknowledging one it never drew. */
bool nd_ui_show_security_notice_once(nd_ui *ui)
{
    nd_msgdialog dlg;

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

    write_security_ack();
    (void)nd_msgdialog_show(&dlg);
    return true;
}

/* ------------------------------------------------------------------ *
 * The soft watchdog -- the contract, and why it exists, are in nd_ui.h
 * ------------------------------------------------------------------ */

/* Five seconds. Nothing the UI does ON PURPOSE reaches it: the longest
 * deliberate block in the system is a key wait, and every one of those beats
 * at 0.1 s. Short enough that the line is in the log while the owner is still
 * deciding whether the phone is dead. */
#define ND_UI_WATCH_STALL_MS 5000u

/* One second between looks. The answer only has to be approximately right, and
 * this thread waking is the whole of what the watchdog costs while nothing is
 * wrong. Whole seconds so the deadline arithmetic below is one addition and
 * cannot be got wrong. */
#define ND_UI_WATCH_POLL_S 1

/* It formats one log line and does nothing else, so it takes 64 KiB against
 * the 128 KiB the modem, clock and service threads each ask for. */
#define ND_UI_WATCH_STACK (64u * 1024u)

/* Labels with static storage, as nd_ui.h requires of every label. */
#define ND_UI_WATCH_IDLE_LABEL    "the key loop"
#define ND_UI_WATCH_STARTUP_LABEL "starting up"
#define ND_UI_WATCH_APP_LABEL     "an app screen"

/* NEODCT_UI_WATCHDOG=0 turns it off. For the same audience NEODCT_BENCH has:
 * a debugger stops every thread in the process, so single-stepping cannot make
 * this fire, but a measurement run that wants nothing else awake can say so. */
#define ND_UI_WATCH_ENV "NEODCT_UI_WATCHDOG"

/* ============ WHY THESE ARE ATOMICS AND NOT A MUTEX ============
 *
 * The UI thread writes them and the checker reads them, and the UI thread must
 * never take a lock to say it is alive -- the one thing a watchdog may not do
 * is add a way for the watched thread to block. Relaxed ordering throughout:
 * nothing here publishes memory, the values ARE the message, and the checker is
 * allowed to see a beat one poll late.
 *
 * 32-bit and not 64-bit deliberately. A 64-bit atomic is not lock-free on
 * 32-bit ARM in every toolchain configuration, and a libatomic call on the key
 * loop is exactly what this must not cost. A millisecond counter in 32 bits
 * wraps every 49.7 days, and the subtraction that reads it is unsigned, so the
 * wrap is invisible: what is computed is a difference of less than a minute,
 * which is correct across it. */
static _Atomic uint32_t g_watch_beat_ms;
static _Atomic(const char *) g_watch_label = ND_UI_WATCH_IDLE_LABEL;
static _Atomic uint32_t g_watch_hold;
/* The beat that was current when the outermost hold began -- see the checker
 * for what it is for. */
static _Atomic uint32_t g_watch_hold_beat_ms;

/* The stop handshake, and NOTHING else, is what this pair guards. The checker
 * takes them; the UI thread never does. */
static pthread_mutex_t g_watch_mu;
static pthread_cond_t g_watch_cv;
static pthread_t g_watch_tid;
static bool g_watch_running;
static bool g_watch_quit;
static bool g_watch_cv_monotonic;

/* clock_gettime(CLOCK_MONOTONIC) and not nd_time_monotonic().
 *
 * Under the golden-frame capture nd_time_monotonic() reads the VIRTUAL clock,
 * which advances one tick per committed frame -- so a UI thread that has
 * stopped committing frames is precisely the one whose clock has also stopped,
 * and a watchdog reading it could never see a stall. This is the one place in
 * the system that has to ask the kernel itself. */
static uint32_t watch_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void nd_ui_watch_beat(void)
{
    atomic_store_explicit(&g_watch_beat_ms, watch_now_ms(), memory_order_relaxed);
}

const char *nd_ui_watch_begin(const char *what)
{
    const char *saved = atomic_load_explicit(&g_watch_label, memory_order_relaxed);

    atomic_store_explicit(&g_watch_label, what != NULL ? what : ND_UI_WATCH_IDLE_LABEL,
                          memory_order_relaxed);
    /* The five seconds are the OPERATION'S, not the leftovers of whenever the
     * last beat happened to land -- otherwise a section entered 4.9 s after a
     * beat would be reported 0.1 s in, under its own name, for something the
     * caller before it did. */
    nd_ui_watch_beat();
    return saved;
}

void nd_ui_watch_end(const char *saved)
{
    atomic_store_explicit(&g_watch_label, saved != NULL ? saved : ND_UI_WATCH_IDLE_LABEL,
                          memory_order_relaxed);
    nd_ui_watch_beat();
}

void nd_ui_watch_hold(void)
{
    /* The stamp is recorded, not cleared: a held section that turns out to
     * beat after all is watched like any other. The checker explains why. */
    atomic_store_explicit(&g_watch_hold_beat_ms,
                          atomic_load_explicit(&g_watch_beat_ms, memory_order_relaxed),
                          memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&g_watch_hold, 1u, memory_order_relaxed);
}

void nd_ui_watch_release(void)
{
    /* THE BEAT COMES FIRST, and the order is the whole of what makes this
     * safe. The other way round leaves an instant in which the hold is gone
     * and the stamp is still the minutes-old one the held section never
     * touched -- and a checker that samples there reports a stall that ended
     * before it started looking. Beating first cannot go wrong the same way:
     * a sample taken between the two sees a held section with a fresh stamp,
     * which is exactly what a healthy one looks like. */
    nd_ui_watch_beat();
    /* Read-then-decrement rather than a bare fetch_sub, so an unbalanced
     * release cannot wrap the counter and disarm the watchdog for the life of
     * the process. Not a race: only the UI thread touches this. */
    if (atomic_load_explicit(&g_watch_hold, memory_order_relaxed) > 0u)
        (void)atomic_fetch_sub_explicit(&g_watch_hold, 1u, memory_order_relaxed);
}

uint32_t nd_ui_watch_age_ms(const char **label_out, bool *held_out)
{
    /* The beat FIRST and the clock second. The other order lets a beat land
     * between the two reads, which makes the stamp newer than "now" and turns
     * the unsigned subtraction into 49 days of apparent stall. */
    uint32_t beat = atomic_load_explicit(&g_watch_beat_ms, memory_order_relaxed);
    uint32_t now = watch_now_ms();

    if (label_out != NULL)
        *label_out = atomic_load_explicit(&g_watch_label, memory_order_relaxed);
    if (held_out != NULL)
        *held_out = atomic_load_explicit(&g_watch_hold, memory_order_relaxed) != 0u;
    return now - beat;
}

static void *watch_thread(void *arg)
{
    const char *stall_label = ND_UI_WATCH_IDLE_LABEL;
    uint32_t stall_age_ms = 0u;
    bool reported = false;

    (void)arg;

    for (;;) {
        struct timespec deadline;
        const char *label;
        bool held;
        bool quit;
        uint32_t beat;
        uint32_t age;

        if (clock_gettime(g_watch_cv_monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &deadline) != 0)
            break;
        deadline.tv_sec += ND_UI_WATCH_POLL_S;

        (void)pthread_mutex_lock(&g_watch_mu);
        /* Checked inside the lock before waiting: a stop that arrived while
         * this thread was doing the work below must not be slept through. */
        if (!g_watch_quit)
            (void)pthread_cond_timedwait(&g_watch_cv, &g_watch_mu, &deadline);
        quit = g_watch_quit;
        (void)pthread_mutex_unlock(&g_watch_mu);
        if (quit)
            break;

        /* The beat FIRST and the clock second -- nd_ui_watch_age_ms() says why.
         * Read here rather than through that accessor because the hold test
         * below needs the raw stamp, not the age. */
        beat = atomic_load_explicit(&g_watch_beat_ms, memory_order_relaxed);
        age = watch_now_ms() - beat;
        label = atomic_load_explicit(&g_watch_label, memory_order_relaxed);
        held = atomic_load_explicit(&g_watch_hold, memory_order_relaxed) != 0u;

        /* ============ A HOLD THAT NOTICES IT IS NO LONGER NEEDED ============
         *
         * A hold is the UI thread saying it has handed the screen to a child
         * process and will not beat until it has it back. Today that is true:
         * nd_proc.c's wait loop, where the thread actually is, stamps nothing,
         * so five seconds into every app launch the watchdog would otherwise
         * report a stall that is not one.
         *
         * But silence for the whole of an app's life is a real cost, and the
         * fault this watchdog exists for lives exactly there. So the hold is
         * not "stop looking" -- it is "stop looking UNTIL SOMETHING IN HERE
         * BEATS". The stamp is remembered at the hold, and the moment it moves,
         * whatever is inside is demonstrably reporting for itself and gets
         * watched on the ordinary five-second rule.
         *
         * That makes the placeholder self-retiring. One nd_ui_watch_beat() in
         * nd_proc.c's wait loop and app runs become watched, here, with no
         * change to this file and nothing to remember to undo. */
        if (held && beat == atomic_load_explicit(&g_watch_hold_beat_ms, memory_order_relaxed)) {
            /* Any stall already reported ends unremarked rather than being
             * announced as fixed by something that only stopped looking. */
            reported = false;
            continue;
        }

        if (age >= ND_UI_WATCH_STALL_MS) {
            /* Measured every poll even though it is REPORTED once: the age at
             * the moment of recovery cannot be read after the fact, because
             * the beat that ended the stall has already replaced the stamp
             * that would have said how long it was. Keeping the last measured
             * one puts the recovery line within a second of the truth. */
            stall_age_ms = age;
            /* ONCE per stall. A line per check would be twelve lines a minute
             * for as long as the fault lasts, on a log the owner reads to
             * debug everything else, and the twelfth says nothing the first
             * did not. */
            if (!reported) {
                reported = true;
                stall_label = label != NULL ? label : ND_UI_WATCH_IDLE_LABEL;
                /* nd_log_err for BOTH lines, including the recovery, which is
                 * not an error: run_neodct.sh redirects only stderr into
                 * /NeoDCT/User/logs/core.log, and a line about a phone that
                 * stopped responding is worth nothing on a serial console
                 * nobody was attached to. */
                nd_log_err(ND_LOG_UI, "the UI thread has not checked in for %.1f s -- it is in: %s",
                           (double)age / 1000.0, stall_label);
            }
        } else if (reported) {
            reported = false;
            nd_log_err(ND_LOG_UI,
                       "the UI thread is running again; it had been stopped at least "
                       "%.1f s in: %s",
                       (double)stall_age_ms / 1000.0, stall_label);
        }
    }
    return NULL;
}

/* Started by nd_ui_init() and by nothing else. See nd_ui.h for why an app
 * process gets the beats but not the thread. */
static void watch_start(void)
{
    pthread_attr_t attr;
    pthread_condattr_t cattr;
    sigset_t all;
    sigset_t saved;
    const char *off;
    bool have_attr;
    bool have_cattr;
    size_t stack = ND_UI_WATCH_STACK;
    int rc;

    if (g_watch_running)
        return;
    off = getenv(ND_UI_WATCH_ENV);
    if (off != NULL && strcmp(off, "0") == 0)
        return;

    atomic_store_explicit(&g_watch_label, ND_UI_WATCH_STARTUP_LABEL, memory_order_relaxed);
    atomic_store_explicit(&g_watch_hold, 0u, memory_order_relaxed);
    nd_ui_watch_beat();

    if (pthread_mutex_init(&g_watch_mu, NULL) != 0)
        return;

    /* A CLOCK_MONOTONIC condition variable, if this libc will give one.
     *
     * The default base is CLOCK_REALTIME, and this board has no RTC: the clock
     * service sets the wall clock from NTP a minute or so into every boot, and
     * a jump of years is the normal case rather than the exceptional one. A
     * timed wait on the realtime clock would then sleep out that jump, and the
     * one thread in the system that must not go to sleep is this one. Falling
     * back to the default is still better than no watchdog, so a libc without
     * it gets one that can be fooled once per clock step. */
    have_cattr = pthread_condattr_init(&cattr) == 0;
    g_watch_cv_monotonic = have_cattr && pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) == 0;
    rc = pthread_cond_init(&g_watch_cv, g_watch_cv_monotonic ? &cattr : NULL);
    if (have_cattr)
        (void)pthread_condattr_destroy(&cattr);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&g_watch_mu);
        return;
    }

    have_attr = pthread_attr_init(&attr) == 0;
    if (have_attr) {
#ifdef PTHREAD_STACK_MIN
        if (stack < (size_t)PTHREAD_STACK_MIN)
            stack = (size_t)PTHREAD_STACK_MIN;
#endif
        (void)pthread_attr_setstacksize(&attr, stack);
    }

    /* A new thread inherits the creator's signal mask, so blocking everything
     * across the pthread_create() leaves this one unable to receive a signal
     * at all. SIGTERM and SIGINT keep going to the main thread, which is the
     * one that installed handlers for them -- nd_clock.c makes the same
     * argument at length for the same reason. */
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &saved);

    g_watch_quit = false;
    rc = pthread_create(&g_watch_tid, have_attr ? &attr : NULL, watch_thread, NULL);

    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    if (have_attr)
        (void)pthread_attr_destroy(&attr);

    if (rc != 0) {
        /* Not fatal. A phone without a watchdog is the phone every release
         * before this one shipped; a phone that will not boot because it could
         * not start one would be a worse trade than the fault it watches for. */
        nd_log_err(ND_LOG_UI, "cannot start the UI watchdog: %s", strerror(rc));
        (void)pthread_cond_destroy(&g_watch_cv);
        (void)pthread_mutex_destroy(&g_watch_mu);
        return;
    }
    g_watch_running = true;
}

static void watch_stop(void)
{
    if (!g_watch_running)
        return;

    /* Signalled rather than left to time out: nd-shoot builds and tears down a
     * context a dozen times in two seconds, and a join that waited out the
     * poll each time would cost more than the tool does. */
    (void)pthread_mutex_lock(&g_watch_mu);
    g_watch_quit = true;
    (void)pthread_cond_signal(&g_watch_cv);
    (void)pthread_mutex_unlock(&g_watch_mu);

    (void)pthread_join(g_watch_tid, NULL);
    (void)pthread_cond_destroy(&g_watch_cv);
    (void)pthread_mutex_destroy(&g_watch_mu);
    g_watch_running = false;
    g_watch_quit = false;
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
 * T-5 still holds.
 *
 * Public rather than static since 0.5.9b: nd_main.c had already copied it
 * word for word, and the core now re-asks it on every key wait. See
 * keypad_tick() and nd_ui.h. */
bool nd_ui_t9_active_for(bool detected)
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
    /* The label in force before each step, handed back at its end. Every one
     * of the eighteen steps can block on hardware or on storage, and a boot
     * that wedges inside one is the hardest fault on this phone to see from
     * outside -- there is no screen yet for it to look wrong on. */
    const char *saved;
    nd_err rc;

    if (ui == NULL)
        return ND_ERR_INVAL;
    memset(ui, 0, sizeof *ui);
    ui->keypad_fd = -1;
    /* The core is not an app and has no manifest to opt out in, and it is
     * the only process that runs the frame loop the animation needs. */
    ui->app_use_wallpaper = true;
    ui->drives_wallpaper = true;
    g_ring_seen_at = 0.0;

    /* BEFORE the eighteen steps and not after them. Everything below this line
     * can block -- opening the modem, opening a database, probing an i2c bus
     * that is not there -- and a boot that wedges inside one of them is the
     * hardest fault on this phone to see from the outside, because there is no
     * screen yet to look wrong. Started here, the log says which step. */
    watch_start();

    (void)nd_settings_init();

    /* --- step 1 --- */
    saved = nd_ui_watch_begin("opening the databases");
    (void)nd_db_init_all();
    nd_ui_watch_end(saved);

    /* --- step 2: the three services. Each is optional at link time and each
     * runs in simulation when its hardware is absent, which is what makes the
     * whole UI drivable on a desktop. --- */
    saved = nd_ui_watch_begin("opening the modem");
    if (nd_modem_open != NULL)
        (void)nd_modem_open(&ui->modem);
    nd_ui_watch_end(saved);
    saved = nd_ui_watch_begin("opening the battery gauge");
    if (nd_battery_open != NULL)
        (void)nd_battery_open(&ui->battery, -1, -1);
    nd_ui_watch_end(saved);
    saved = nd_ui_watch_begin("opening the notification service");
    if (nd_notify_open != NULL)
        (void)nd_notify_open(&ui->notify);
    nd_ui_watch_end(saved);

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
    /* This process opened its own backend, so it may ask it again later --
     * and it must, because the answer changes. See keypad_tick(). */
    ui->owns_input_backend = true;
    /* An i2c probe against a bus that is not there, or is there and does not
     * answer. It is the step a keyless phone spends longest in, and the one an
     * owner is most likely to be looking at a blank screen through. */
    saved = nd_ui_watch_begin("opening the keypad");
    if (nd_input_open(&ui->input) == ND_OK && ui->input != NULL) {
        ui->keypad_fd = nd_input_fd(ui->input);
        ui->has_matrix_keypad = nd_ui_t9_active_for(nd_input_has_matrix(ui->input));
    } else {
        ui->input = NULL;
        ui->keypad_fd = -1;
        ui->has_matrix_keypad = nd_ui_t9_active_for(false);
    }
    nd_ui_watch_end(saved);

    saved = nd_ui_watch_begin("building the canvas and loading the fonts");
    rc = ui_common_init(ui, fb);
    nd_ui_watch_end(saved);
    if (rc != ND_OK) {
        nd_ui_teardown(ui);
        return rc;
    }

    /* --- step 13, in its real position: AFTER the fonts and the canvas, and
     * BEFORE home_layout, wallpaper, engineering_mode and apps have any
     * meaning. That used to be an ordering difference from the Python --
     * ui_common_init had already assigned all four -- recorded as U-3 in
     * OPEN-QUESTIONS.md. Making them lazy closed it: at this point none of
     * them has been loaded, which is exactly where launcher.py stood. ---
     *
     * ONLY WITH A KEYPAD. This is a blocking modal that waits for a key to
     * dismiss it. On a phone whose keypad did not come up there is no key,
     * so it would sit here for ever -- which is precisely the "stuck on the
     * alpha screen, no keypad response" a keyless phone showed. Skipping it
     * lets core_run() reach its own input-failure screen, which says why. */
    if (nd_input_has_backend(ui->input))
        (void)nd_ui_show_security_notice_once(ui);

    return ND_OK;
}

nd_err nd_ui_init_app(nd_ui *ui, nd_fb *fb, int keypad_fd)
{
    nd_err rc;

    if (ui == NULL)
        return ND_ERR_INVAL;
    memset(ui, 0, sizeof *ui);
    ui->keypad_fd = keypad_fd;
    /* manifest.json's "useWallpaper", read ONCE here rather than per draw --
     * see nd_app.h. Absent, unparseable and non-boolean all mean true, so an
     * app written before the key existed keeps the wallpaper. nd_app_dir()
     * has already been set by nd-apprun; a hand-built app context with no
     * directory also gets true. */
    ui->app_use_wallpaper = nd_app_manifest_use_wallpaper(nd_app_dir());
    /* An app animates too. It has no frame loop of its own, but every widget
     * it opens blocks in nd_ui_wait_for_key(), and that is where the
     * wallpaper is advanced and the widget repainted -- see
     * nd_ui_set_repaint(). An app that opted out never loads a wallpaper at
     * all, so it still opens no decoder and this costs it nothing. */
    ui->drives_wallpaper = true;
    g_ring_seen_at = 0.0;

    /* The beat but not the thread -- nd_ui.h says why an app gets no thread.
     * Stamped anyway so that nd_ui_watch_age_ms() answers honestly in an app
     * process, and so that a label an app sets is not sitting on top of a beat
     * that never happened. */
    atomic_store_explicit(&g_watch_label, ND_UI_WATCH_APP_LABEL, memory_order_relaxed);
    nd_ui_watch_beat();

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
    /* Before the adopt, so it holds even when there is no socket to adopt --
     * which is exactly the case that needs it. */
    nd_svc_mark_app_process();
    nd_svc_client_open_from_env();
    if (keypad_fd >= 0 && nd_input_open_pipe(&ui->input, keypad_fd) != ND_OK)
        ui->input = NULL;

    /* NOT nd_input_has_matrix(ui->input): ui->input is the inherited PIPE and
     * has no matrix by construction, which is why this flag used to be false
     * in every app on every device and took multi-tap, predictive, the # mode
     * cycle and the mode indicator down with it. The core knows, and says so.
     * nd_app.h, and OPEN-QUESTIONS.md BR-3. */
    ui->has_matrix_keypad = nd_ui_t9_active_for(matrix_from_env());

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

    /* First, and unconditionally: everything below frees something the
     * watchdog does not touch, but leaving a thread running over a teardown is
     * how a tool that builds a dozen contexts (nd-shoot) ends up with a dozen
     * of them. A no-op in an app, which never started one. */
    watch_stop();

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
    if (ui->home_.wallpaper_gif != NULL)
        nd_gif_close(ui->home_.wallpaper_gif);
    ui->home_.wallpaper_gif = NULL;
    nd_image_free(ui->home_.wallpaper);
    ui->home_.wallpaper = NULL;
    nd_image_free(ui->home_.chrome);
    ui->home_.chrome = NULL;

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
    /* One of the three beats. A screen that is still being painted is a UI
     * thread that is still alive, whatever else it is doing -- and this is the
     * beat that covers a widget with a loop of its own, which is most of them.
     * Before the guards, because a context with no framebuffer is a state to
     * report, not a reason to stop reporting. */
    nd_ui_watch_beat();
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

/* The calendar's own tick. There is no Python equivalent -- the diary is new
 * -- so the shape follows the two above rather than a port: cheap, silent,
 * and safe to call from the key-read path on every frame.
 *
 * ============ WHY THE CORE POLLS INSTEAD OF THE APP TELLING IT ============
 *
 * A reminder has to arrive while the Calendar app is closed, which is almost
 * always. The app is a separate process that has usually already exited by
 * the time an appointment comes round, so there is nobody to send the
 * message; the core is the only thing still running, and it is also the only
 * thing that owns NotifyService (nd_app.h). So the core reads the table.
 *
 * ============ AND WHY IT IS RATE-LIMITED HERE, NOT INSIDE ============
 *
 * The battery service rate-limits its own poll because the i2c read is the
 * expensive part and every caller wants the cached answer. Here the
 * expensive part is opening sqlite, which nothing else wants, so the clock
 * lives at the one call site. ND_CAL_POLL_S is 15 s; a phone with no
 * calendar.db pays one stat() for each of them. */
static void calendar_tick(nd_ui *ui)
{
    static double next_poll; /* monotonic; 0.0 means "the first frame" */
    nd_cal_event ev;
    int64_t occurrence = 0;
    double now;

    if (ui->shutting_down || ui->notify == NULL || nd_cal_due == NULL ||
        nd_notify_post_event == NULL)
        return;

    now = nd_time_monotonic();
    if (next_poll != 0.0 && now < next_poll)
        return;
    next_poll = now + ND_CAL_POLL_S;

    /* One per poll. A phone catching up on several missed reminders shows
     * the earliest first (nd_cal_due picks it), and the rest arrive over the
     * following polls -- which is what the banner's own counter is for. */
    if (!nd_cal_due(nd_time_now(), &ev, &occurrence))
        return;

    nd_notify_post_event(ui->notify, ev.id, ev.title, ev.start, true);
    if (nd_cal_mark_notified != NULL)
        nd_cal_mark_notified(ev.id, occurrence);
}

/* ============ THE T9 FLAG IS NOT A FACT ABOUT THE BOOT ============
 *
 * ui->has_matrix_keypad was decided once, in nd_ui_init(), from an nd_input
 * that had had exactly one attempt at the i2c bus -- and then two separate
 * releases taught the layer underneath it to change its mind. 0.5.7b added
 * try_reopen_matrix(), which brings the matrix up after a lost udev race;
 * 0.5.9b added nd_input_retry_backend() and a tear-down at
 * ND_MATRIX_DEAD_SCANS for a bus that dies mid-session. Neither told the UI.
 *
 * So a phone that recovered its keypad -- which is exactly the phone the
 * retry was written for -- had working keys and no T9: no multi-tap, no
 * predictive text, no # mode cycle, no mode indicator. On a 16-key matrix
 * with no letter keys, that means letters cannot be typed AT ALL, in
 * Messages, in Contacts, anywhere. And because nd_proc.c deliberately hands
 * this CACHED value to every child in NEODCT_KEYPAD_MATRIX rather than asking
 * the input layer itself, the stale answer was then handed to every app
 * launched for the rest of the session.
 *
 * It is wrong in the other direction too: a UI still drawing the T9 indicator
 * for a matrix nd_input has torn down is lying about which keyboard it is
 * listening to.
 *
 * The fix is to stop remembering it. This runs on every key wait -- two
 * pointer dereferences and one getenv, on a path that is about to sleep for
 * a tenth of a second -- so the field becomes a cache with an owner instead
 * of a verdict taken in the first fraction of a second of a boot. The field
 * itself stays because the app path fills it from the environment and because
 * nd_proc.c reads it there; ui->owns_input_backend is what says which of the
 * two this process is.
 *
 * ui->keypad_fd is refreshed with it and for the same reason: widgets poll
 * that descriptor directly to flush pending keys before their first draw, and
 * nd_input hands out a different one -- or none -- once a backend has been
 * dropped and reopened. */
static void keypad_tick(nd_ui *ui)
{
    bool live;

    if (!ui->owns_input_backend || ui->input == NULL)
        return;

    ui->keypad_fd = nd_input_fd(ui->input);

    live = nd_ui_t9_active_for(nd_input_has_matrix(ui->input));
    if (live == ui->has_matrix_keypad)
        return;

    ui->has_matrix_keypad = live;
    /* Logged because it is the counterpart of try_reopen_matrix()'s own
     * "Input recovered" line: without it a serial log shows the keypad coming
     * back and says nothing about whether text entry came back with it, which
     * is the half the owner actually notices. */
    nd_log(ND_LOG_UI, "T9 %s: the i2c keypad matrix %s.", live ? "ON" : "OFF",
           live ? "is now open" : "is gone");
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

nd_ui_repaint nd_ui_set_repaint(nd_ui *ui, void (*fn)(void *ctx), void *ctx)
{
    nd_ui_repaint saved;

    saved.fn = NULL;
    saved.ctx = NULL;
    if (ui == NULL)
        return saved;

    saved.fn = ui->repaint_.fn;
    saved.ctx = ui->repaint_.ctx;
    ui->repaint_.fn = fn;
    ui->repaint_.ctx = ctx;
    return saved;
}

void nd_ui_restore_repaint(nd_ui *ui, nd_ui_repaint saved)
{
    if (ui == NULL)
        return;
    ui->repaint_.fn = saved.fn;
    ui->repaint_.ctx = saved.ctx;
}

/* Advance the wallpaper and let a blocked widget put itself back on top.
 *
 * Only when a widget has registered a repainter: the core loop draws its own
 * frame straight after this returns, so ticking for it here would change
 * nothing and the old behaviour is worth leaving exactly as it was. */
static void repaint_if_wallpaper_moved(nd_ui *ui)
{
    if (ui->repaint_.fn == NULL || ui->repaint_.running)
        return;
    /* Before the mode, for the same reason nd_ui_widget_timeout() does it in
     * this order: this is a key poll and it must stay free. */
    if (!has_animation(ui))
        return;
    /* HOME and OFF both mean "not from a widget": under HOME the home screen
     * still animates, because that is nd_ui_update()'s tick and not this one.
     * Checked before the tick so that neither mode advances the decoder the
     * core is holding -- a widget must not consume frames the home screen is
     * going to want. */
    if (nd_ui_anim_mode_of(ui) != ND_UI_ANIM_ALWAYS)
        return;
    if (!nd_ui_tick_wallpaper(ui))
        return;

    /* The callback draws, and drawing can reach code that waits for a key --
     * a dialog inside a repaint would recurse until the stack ran out. */
    ui->repaint_.running = true;
    ui->repaint_.fn(ui->repaint_.ctx);
    ui->repaint_.running = false;
}

int32_t nd_ui_read_keypress(nd_ui *ui, double timeout_s)
{
    /* The main beat: every loop in the system that waits for a key comes
     * through here, so this alone covers the home screen, every widget, every
     * app's own loop and the format progress bar. Before the NULL check for
     * the reason nd_ui_present()'s is. */
    nd_ui_watch_beat();
    if (ui == NULL)
        return ND_KEY_NONE;

    repaint_if_wallpaper_moved(ui);

    /* In the CORE these run first, every call. In an APP all three services
     * are NULL and every one is a no-op, which is the plain read nd_ui.h
     * promises. */
    battery_tick(ui);
    modem_tick(ui);
    calendar_tick(ui);
    keypad_tick(ui);
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

double nd_ui_widget_timeout(nd_ui *ui, double dflt)
{
    if (ui == NULL || ui->repaint_.fn == NULL)
        return dflt;
    /* Before the mode, because asking the mode reads two settings and this
     * answers the question for free -- see has_animation(). */
    if (!has_animation(ui))
        return dflt;
    if (nd_ui_anim_mode_of(ui) != ND_UI_ANIM_ALWAYS)
        return dflt;
    return nd_ui_frame_timeout(ui, dflt);
}

int32_t nd_ui_wait_for_key(nd_ui *ui)
{
    for (;;) {
        /* 0.1 s unless THIS WAIT is one that advances the wallpaper, in which
         * case it wakes when the next frame is due and a menu animates at the
         * GIF's own rate. nd_ui_widget_timeout(), not nd_ui_frame_timeout():
         * the difference is a busy-spin, and its comment says why. */
        int32_t key = nd_ui_read_keypress(ui, nd_ui_widget_timeout(ui, 0.1));

        if (key != ND_KEY_NONE)
            return key;
    }
}

/* Every nd_proc_launch_app() in this file goes through here, and the reason is
 * the watchdog rather than the launch.
 *
 * ============ THE CORE IS ALIVE, JUST NOT HERE ============
 *
 * nd_proc_launch_app() does not return until the app does, and for the whole
 * of that the core's UI thread is inside nd_proc.c's wait loop, forwarding key
 * events to the child every 50 ms. It is running. It simply beats nowhere this
 * file can reach. Left alone the watchdog would report a stall five seconds
 * into every app launch and a recovery when the app exited -- two false lines
 * per launch, which is precisely the noise that teaches somebody to stop
 * reading a watchdog.
 *
 * ============ AND THE HOLD RETIRES ITSELF ============
 *
 * This matters more than the false alarms do, because the freeze the watchdog
 * was written for -- the phone dead for minutes while a card formatted -- is
 * exactly a core UI thread wedged in THAT loop, on the broker round-trip mutex
 * the format was holding (nd_svc.c makes the argument at length). A hold that
 * simply stopped looking would be blind to the one fault it exists for.
 *
 * So it does not stop looking; it waits to be shown that somebody in there is
 * reporting. The checker remembers the beat at the hold and resumes the
 * ordinary five-second rule the moment that stamp moves. ONE
 * nd_ui_watch_beat() in nd_proc.c's wait loop, beside pump_keys(), turns app
 * runs into watched time -- with no change here, and nothing to remember to
 * undo. Until that line exists, an app run is silent, and that silence is a
 * measured trade rather than an oversight.
 *
 * Nothing else in the system holds the watchdog, and nothing else should. */
static nd_err launch_app_watched(nd_ui *ui, const nd_app_entry *app, const char *entry,
                                 const char *arg, nd_crash_info *crash_out)
{
    nd_err rc;

    nd_ui_watch_hold();
    rc = nd_proc_launch_app(ui, app, entry, arg, crash_out);
    nd_ui_watch_release();
    return rc;
}

/* Why an app did not open, when the reason is that the core would have had to
 * hand it root. See nd_proc_launch_app(): an image with no ndusr_ut cannot
 * confine the browser, and the answer to that is not to run it.
 *
 * The wording says what happened and what it means, and stops. The cause --
 * a buildroot tree older than the users table -- is a developer's problem and
 * goes to the console, where the log line names the exact rebuild command.
 *
 * It fits: 4 lines against nd_msgdialog's budget of 5. Measured with
 * nd_msgdialog_measure() rather than guessed, which is the whole reason that
 * accessor exists. */
static void show_cannot_confine(nd_ui *ui, const char *app_name)
{
    nd_msgdialog dlg;

    nd_log_err(ND_LOG_OS, "%s was not launched: it cannot be confined on this image.",
               (app_name != NULL) ? app_name : "an app");

    if (nd_msgdialog_init == NULL || nd_msgdialog_show == NULL)
        return;
    nd_msgdialog_init(&dlg, ui, ND_UI_CANNOT_CONFINE_MESSAGE);
    if (nd_msgdialog_set_title != NULL)
        nd_msgdialog_set_title(&dlg, "Blocked");
    if (nd_msgdialog_set_icon != NULL)
        nd_msgdialog_set_icon(&dlg, ND_PATH_WARNING_ICON);
    if (nd_msgdialog_set_button != NULL)
        nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);
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

            /* A FAULTED modem gets no carrier line at all. The element's
             * authored text is the literal "No Service", and leaving that
             * standing on a phone whose radio has died would be the most
             * misleading thing on the screen: "No Service" is what a working
             * phone says in a tunnel. Nothing is the honest answer, and the
             * empty meter beside it plus the notice say the rest.
             *
             * ND_MODEM_LINK_UNREACHABLE is DELIBERATELY NOT dropped here, and
             * the difference is worth stating because the next reader will
             * want to add it. FAULT is a radio that was working a moment ago,
             * so a blank line plus a modal reads as "something just broke".
             * UNREACHABLE is the steady state of a phone that has never
             * managed to open its own modem -- a udev grant that arrived
             * late, S45modem holding the AT-port lock -- and it is reached
             * through paths where the owner may never see a modal at all. So
             * it keeps the line and fills it: nd_modem_operator_display()
             * answers ND_MODEM_UNREACHABLE_CARRIER ("Modem ERROR") and
             * nd_layout.c substitutes it for the placeholder, beside the
             * empty meter nd_modem_signal_level() returns for the same state.
             * That pair is the only thing on the home screen that can tell
             * this apart from a phone in a tunnel -- and it is what replaced
             * the word "Simulation" beside four full bars. */
            if (el->type == ND_EL_TEXT && strcmp(el->text, "No Service") == 0 &&
                nd_ui_status_modem_faulted(ui))
                continue;

            /* THE RED LINE UNDER THE CARRIER, and the two reasons it is
             * skipped.
             *
             * It says "Eng. Mode" because that mode gives every app
             * under /NeoDCT/System/engineering/apps root (nd_proc.h), and a
             * phone handing out root should look different from a phone that
             * is not. So the rule is the plain one: draw it exactly when the
             * privilege is actually being granted.
             *
             * Skipped when the mode is OFF, obviously. Skipped ALSO while the
             * message banner is up, for the same reason the carrier is: the
             * banner is drawn at rows 49..96 and this line sits at row 69,
             * squarely underneath it. Two strings painted over each other is
             * worse than either alone, and the banner is the more urgent of
             * the two.
             *
             * Matching on el->text is the mechanism nd_layout.c already uses
             * for the clock and the carrier -- "there is no marker syntax;
             * these exact strings are the whole mechanism". */
            if (el->type == ND_EL_TEXT && strcmp(el->text, ND_UI_ENG_MODE_LABEL) == 0 &&
                (notify_active || !nd_ui_engineering_mode(ui)))
                continue;

            nd_home_render_element(ui, el);
        }
    } else if (ui->font_s != NULL) {
        /* The Python passes no font here and gets Pillow's built-in bitmap
         * face; see U-2 in OPEN-QUESTIONS.md. */
        (void)nd_draw_text(ui->draw, 10, 10, "No Layout Found", ui->font_s, ND_RGB(255, 0, 0));
    }

    /* --- 3. notification layer: a flashing envelope while unread mail
     * exists, a banner while it is undismissed ---
     *
     * The envelope is the INBOX's icon and follows the text banner only. A
     * reminder gets the two lines and nothing else: an envelope over an
     * appointment would be saying the wrong thing, and the phone has no
     * calendar glyph in ui/resources to say the right one with. The banner
     * names itself, which on two lines of 20 px type is enough. */
    if (banner_is(ui, ND_NOTIFY_KIND_SMS) || nd_ui_unread_sms(ui) > 0) {
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
        /* The banner shares its rows with the signal meter, which
         * ui_home.json puts at x = 210 -- thirty in from the right edge,
         * mirroring the banner's own left margin. Four more for a gap.
         *
         * "10 messages" is 130 px at 20 px and could never have reached it,
         * which is why the Python drew the lines unmeasured. AN EVENT'S NAME
         * CAN: "Parents evening at school" runs straight through the bars. So
         * the lines are fitted now -- which changes nothing for a message
         * banner, and is why home-sms-banner still matches. */
        int32_t max_w = nd_ui_width(ui) - 30 - 34;

        lines[0] = l1;
        lines[1] = l2;
        for (i = 0u; i < n && i < 2u; i++) {
            char fitted[ND_TEXT_LINE_MAX];

            (void)nd_text_ellipsize(fitted, sizeof fitted, lines[i], ui->font_n, max_w);
            (void)nd_draw_text(ui->draw, 30, y, fitted, ui->font_n, ND_WHITE);
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
            /* ND_ERR_PERM is the ONE launch failure the owner has to be told
             * about, because it means an untrusted app was refused rather
             * than run with the core's privileges. Every other failure leaves
             * the app not running, which is its own message; this one leaves
             * the phone SAFER than it would otherwise be and looks identical
             * from the outside. A screen that flashes and returns home is
             * what hid the browser being broken once already. */
            if (launch_app_watched(ui, &apps[choice], NULL, NULL, NULL) == ND_ERR_PERM)
                show_cannot_confine(ui, apps[choice].name);
        } else {
            nd_log_err(ND_LOG_OS, "App launcher not linked; ignoring %s", apps[choice].name);
        }
    }
    /* Always unwind, so one bad app or menu event cannot trap the core loop. */
    ui->state = ND_UI_STATE_HOME;
}

void nd_ui_update(nd_ui *ui)
{
    /* The core's per-frame beat. Redundant with the two below it on a healthy
     * phone and not redundant at all on a sick one: this is the top of the
     * core loop, so it is the one beat that still lands if a frame is being
     * composed and never presented. */
    nd_ui_watch_beat();
    if (ui == NULL)
        return;

    /* The only caller. See nd_ui.h: the home screen, the softkey bar and the
     * app selector all read the wallpaper on the same frame, so advancing it
     * from inside nd_ui_wallpaper() would play a 25 fps GIF at 75. */
    (void)nd_ui_tick_wallpaper(ui);

    switch (ui->state) {
    case ND_UI_STATE_HOME:
        nd_ui_render_home(ui);
        /* present=false: the softkey bar's own flush is suppressed so exactly
         * ONE framebuffer write happens per home frame. */
        /* "Read" is what you do to a message and "View" is what you do to an
         * appointment. One word each, because the strip has room for one. */
        nd_softkey_update(ui->softkey,
                          !nd_ui_status_notify_active(ui)       ? "Menu"
                          : banner_is(ui, ND_NOTIFY_KIND_EVENT) ? "View"
                                                                : "Read",
                          false);
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

/* The installed app of that name, or NULL.
 *
 * ============ IT ASKS FOR THE LIST RATHER THAN READING THE FIELD ============
 *
 * _open_notification read ui->home_.apps directly, and that field is EMPTY
 * until something has called nd_ui_app_list() -- which, since the app list
 * became lazy, is only nd_ui_render_menu(). The home screen does not need it,
 * so it does not load it.
 *
 * On a phone that had booted and never had its menu opened, the count was
 * therefore zero, "Messages" was not found, and pressing Read dismissed the
 * banner and opened nothing. Going through the accessor loads the list at the
 * one moment it is wanted, which is the same directory scan the menu would
 * have paid for anyway. */
static const nd_app_entry *find_app(nd_ui *ui, const char *name)
{
    size_t n = 0u;
    const nd_app_entry *apps = nd_ui_app_list(ui, &n);
    size_t i;

    if (apps == NULL)
        return NULL;
    for (i = 0u; i < n; i++) {
        if (strcmp(apps[i].name, name) == 0)
            return &apps[i];
    }
    return NULL;
}

/* NotifyService "Read" softkey: jump to the new message, or the inbox.
 *
 * _open_notification reads kind, count and latest_data BEFORE dismissing --
 * dismiss() zeroes all three -- and only then decides which of Messages' two
 * entry points to call. Keep that order. */
static void open_sms_notification(nd_ui *ui, int32_t count, int64_t target)
{
    const nd_app_entry *messages = find_app(ui, "Messages");
    char arg[32];

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
        (void)launch_app_watched(ui, messages, ND_APP_ENTRY_OPEN_MESSAGE, arg, NULL);
    } else {
        (void)launch_app_watched(ui, messages, ND_APP_ENTRY_OPEN_INBOX, NULL, NULL);
    }
    ui->home_.unread_sms_ready = false;
}

/* The same shape one level down for a reminder: the event that came due when
 * there is exactly one, and the diary itself when there are several -- which
 * is the same "the one thing, or the list" decision Messages makes, and the
 * reason app_open_event() takes an id at all.
 *
 * A phone with no Calendar app installed simply loses the banner, which is
 * already what happens to a text banner with no Messages app. */
static void open_event_notification(nd_ui *ui, int32_t count, int64_t target)
{
    const nd_app_entry *calendar = find_app(ui, "Calendar");
    char arg[32];

    if (calendar == NULL || nd_proc_launch_app == NULL)
        return;

    if (count == 1 && target >= 0 &&
        nd_snprintf(arg, sizeof arg, "%lld", (long long)target) == ND_OK)
        (void)launch_app_watched(ui, calendar, ND_APP_ENTRY_OPEN_EVENT, arg, NULL);
    else
        (void)launch_app_watched(ui, calendar, ND_APP_ENTRY_RUN, NULL, NULL);
}

static void open_notification(nd_ui *ui)
{
    const char *kind = ND_NOTIFY_KIND_SMS;
    int32_t count = 1;
    int64_t target = -1;

    if (ui->notify != NULL) {
        kind = nd_notify_kind != NULL ? nd_notify_kind(ui->notify) : NULL;
        count = nd_notify_count != NULL ? nd_notify_count(ui->notify) : 0;
        target = nd_notify_latest_data != NULL ? nd_notify_latest_data(ui->notify) : -1;
        if (nd_notify_dismiss != NULL)
            nd_notify_dismiss(ui->notify);
    } else {
        nd_ui_sim_sms_banner(ui, 0);
    }

    /* `if kind != "sms": return` was the whole of the Python's dispatch,
     * because "sms" was the only kind. The banner is still dismissed either
     * way, whatever the kind turns out to be -- including one this build has
     * never heard of. */
    if (kind == NULL)
        return;
    if (strcmp(kind, ND_NOTIFY_KIND_SMS) == 0)
        open_sms_notification(ui, count, target);
    else if (strcmp(kind, ND_NOTIFY_KIND_EVENT) == 0)
        open_event_notification(ui, count, target);
}

static void place_call(nd_ui *ui, const char *number, const char *name);

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
            place_call(ui, ui->dial_buffer, NULL);
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
            nd_contacts_show_selector(ui, "Select", "Call", &target))
            place_call(ui, target.number, target.name);
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

/* An outgoing call, start to finish. handling_call is held for the whole of
 * it for the same reason the incoming path holds it: with it clear, a modem
 * that reports RINGING makes every read_keypress() inside the call screen
 * return ND_KEY_INCOMING_CALL instead of a key -- and the End key stops
 * working until the far end gives up. The modem now refuses a RING during a
 * call anyway; this is the second lock on the same door. */
static void place_call(nd_ui *ui, const char *number, const char *name)
{
    ui->handling_call = true;
    if (nd_dialer_place_call != NULL) {
        (void)nd_dialer_place_call(ui, number, name);
    } else {
        /* The Dialer is not linked: the pre-place_call sequence. */
        if (ui->modem != NULL && nd_modem_dial != NULL)
            (void)nd_modem_dial(ui->modem, number);
        if (nd_dialer_show_calling != NULL)
            nd_dialer_show_calling(ui, number, name);
    }
    ui->handling_call = false;
}

void nd_ui_handle_incoming_call(nd_ui *ui, const char *number)
{
    /* ND_CALL_GONE is the Python's "the caller hung up before we answered",
     * which is also what happens when the Dialer is not linked in. */
    nd_incoming_result result = ND_CALL_GONE;

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

    if (result == ND_CALL_ANSWERED) {
        bool ok;

        if (nd_dialer_answer_call != NULL) {
            ok = nd_dialer_answer_call(ui, number != NULL ? number : "", NULL);
        } else {
            ok = ui->modem != NULL && nd_modem_answer != NULL && nd_modem_answer(ui->modem);
            if (ok && nd_dialer_show_calling != NULL)
                nd_dialer_show_calling(ui, number != NULL ? number : "", NULL);
        }
        if (!ok)
            nd_log(ND_LOG_CORE, "Answer failed; releasing the call.");
        /* show_calling returns on End or on a remote hangup; make sure the
         * line is really down either way -- unless the modem already says
         * it is. A second AT+CHUP on a line that is down is a second round
         * trip the UI thread waits out for nothing, and it was every call's
         * last act. */
        if (ui->modem != NULL && nd_modem_hangup != NULL &&
            (nd_modem_state == NULL || nd_modem_state(ui->modem) != ND_CALL_IDLE))
            (void)nd_modem_hangup(ui->modem);
    } else if (result == ND_CALL_DECLINED) {
        nd_log(ND_LOG_CORE, "Call declined.");
        if (ui->modem != NULL && nd_modem_hangup != NULL)
            (void)nd_modem_hangup(ui->modem);
    } else {
        nd_log(ND_LOG_CORE, "Caller hung up before we answered.");
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

/* Is the radio faulted? Used by the home renderer to drop the carrier line.
 *
 * Weak-linked like every other modem entry point here, so a build without the
 * modem module still links; a core with no ModemService reports false, which
 * is right -- no service is not a broken service. */
bool nd_ui_status_modem_faulted(nd_ui *ui)
{
    if (ui == NULL || ui->modem == NULL || nd_modem_link_state == NULL)
        return false;
    return nd_modem_link_state(ui->modem) == ND_MODEM_LINK_FAULT;
}

/* The modem's fault notice, and it is a copy of the battery one above on
 * purpose -- same latch, same deferral, same modal, so there is one shape in
 * this file for "a service needs to tell the user something" rather than two.
 *
 * The deferral is the load-bearing part: ModemService discovers the fault on
 * its own thread, at whatever moment a read() fails, and a modal drawn from
 * there would land in the middle of somebody else's frame. So the service
 * only LATCHES, and this runs on the UI thread from the home loop, where
 * putting a dialog on the screen is safe and where the user is looking. */
void nd_ui_show_pending_modem_fault(nd_ui *ui)
{
    const char *why;
    nd_msgdialog dlg;

    if (ui == NULL)
        return;
    if (ui->state != ND_UI_STATE_HOME && ui->state != ND_UI_STATE_HOME_DIALING)
        return;
    if (ui->modem == NULL || nd_modem_take_pending_fault == NULL)
        return;
    why = nd_modem_take_pending_fault(ui->modem);
    if (why == NULL)
        return;

    /* The reason goes to the console, not to the screen. "port read failed:
     * Input/output error" is what a developer needs and is no help at all to
     * somebody holding a phone; the screen gets the sentence that tells them
     * what to actually do. */
    nd_log_err(ND_LOG_MODEM, "Modem fault reported to the user: %s", why);

    if (nd_msgdialog_init == NULL || nd_msgdialog_show == NULL)
        return;
    nd_msgdialog_init(&dlg, ui, ND_UI_MODEM_FAULT_MESSAGE);
    if (nd_msgdialog_set_title != NULL)
        nd_msgdialog_set_title(&dlg, "Modem");
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

    if (ui->home_.wallpaper_gif != NULL)
        nd_gif_close(ui->home_.wallpaper_gif);
    ui->home_.wallpaper_gif = NULL;
    nd_image_free(ui->home_.wallpaper);
    ui->home_.wallpaper = NULL;
    ui->home_.wallpaper_ready = false;
    ui->home_.eng_mode_ready = false;
    ui->home_.apps_ready = false;

    /* Settings is an app. Turning wallpaper-everywhere off, or moving the
     * dim, must show on the very next screen the core draws. */
    nd_ui_invalidate_chrome(ui);

    /* Messages may have been read (or arrived) inside the app. */
    ui->home_.unread_sms_ready = false;
}
