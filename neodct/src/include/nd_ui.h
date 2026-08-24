/* nd_ui.h -- the context object every widget and every app receives.
 *
 * THIS IS THE BIGGEST CONTRACT IN THE PROJECT. In Python it is NeoDCT_UI from
 * System/core/main.py, a duck-typed grab-bag that framework.py alone touches
 * 142 times. In C it is one struct, passed by pointer to everything.
 *
 * Widgets never touch hardware. They reach through this and nothing else.
 *
 * ============ CONSTRUCTION ORDER IS LOAD-BEARING ============
 *
 * Zero-initialise the whole struct first, then follow the eighteen steps of
 * spec-core-loop.md section 6 in order. The step that catches people out is
 * step 13: the alpha security notice is a BLOCKING MODAL DRAWN FROM INSIDE THE
 * CONSTRUCTOR, and it runs BEFORE engineering_mode, home_layout, wallpaper and
 * apps have been assigned. A widget drawn at that moment sees a half-built
 * context, and that is correct -- it is what the first-boot screen looks like.
 *
 * ============ THE SOFTKEY TRANSPARENCY TRICK ============
 *
 * The Python decides whether a softkey bar is transparent with
 * `not hasattr(ui, 'softkey')`. The core builds its own bar at a moment when
 * that attribute does not yet exist, so the core's bar -- and only the core's
 * bar -- comes out transparent and shows the wallpaper through it. Every bar
 * an app or a dialog builds later sees the attribute and is opaque.
 *
 * DO NOT EMULATE hasattr. Set softkey_exists to true at the same point in
 * construction and pass an explicit boolean to nd_softkey_init(). Exactly one
 * bar in the whole system is transparent.
 *
 * ============ WHAT AN APP MAY TOUCH ============
 *
 * Everything in this struct is readable from an app process. Only the core
 * writes any of it. Settings used to assign ui.wallpaper, ui.engineering_mode
 * and ui.apps directly; it no longer can, and it no longer needs to --
 * OPEN-QUESTIONS.md question 3 is answered: Settings writes only the setting,
 * and the core re-reads system.ui.wallpaper and system.ui.engineering_mode and
 * rescans the app directories AFTER EVERY APP EXIT, exactly as it already
 * re-reads the unread-SMS count. Neither change is observable before the app
 * returns, so this is behaviour-identical.
 */

#ifndef ND_UI_H_INCLUDED
#define ND_UI_H_INCLUDED

#include "nd_battery.h"
#include "nd_draw.h"
#include "nd_fb.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_layout.h"
#include "nd_modem.h"
#include "nd_notify.h"
#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declared here so the geometry helpers below can take a pointer to it
 * before the full definition appears. */
struct nd_ui;

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

/* The framework reads these off the context with a default, so they exist as
 * constants too and a future panel change has one place to edit. */
#define ND_UI_W      240
#define ND_UI_H      175
#define ND_SOFTKEY_H 30

/* The tallest an AppSelector icon may be. 175 on this panel, which means the
 * real cap comes from the per-screen 82 px computation and this only bites on
 * a taller display. */
#define ND_APP_SELECTOR_ICON_MAX 175

/* Derived, and derived the same way the Python derives them. Call the
 * functions rather than the constants where the Python called a helper --
 * _header_divider_y is max(30, (int)(H * 0.11)), which is 30 here only because
 * the floor wins. Anyone hard-coding 30 breaks a future panel; anyone
 * recomputing it wrong breaks today's. */
int32_t nd_ui_width(const struct nd_ui *ui);
int32_t nd_ui_height(const struct nd_ui *ui);
int32_t nd_ui_softkey_height(const struct nd_ui *ui);
int32_t nd_ui_content_bottom(const struct nd_ui *ui);   /* H - SOFTKEY_H = 145 */
int32_t nd_ui_header_divider_y(const struct nd_ui *ui); /* max(30, H*0.11) = 30 */

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

typedef enum { ND_UI_STATE_HOME = 0, ND_UI_STATE_HOME_DIALING, ND_UI_STATE_MENU } nd_ui_state;

#define ND_APP_NAME_MAX 64
#define ND_APP_PATH_MAX 192
#define ND_APP_EXEC_MAX 64
#define ND_APP_MAX      64 /* 24 with engineering mode on; 64 is slack */

/* One entry from a scanned manifest.json. Every field has a default, and the
 * defaults are what an app with a minimal manifest gets:
 *   name -> the folder name, icon -> "<dir>/icon.png", exec -> "main.py"
 *   (now "app.so"), id -> 999.
 * The list is sorted by id, which is what makes the menu deterministic
 * despite the directory scan being in filesystem order. */
typedef struct {
    char name[ND_APP_NAME_MAX];
    char icon[ND_APP_PATH_MAX]; /* absolute */
    char path[ND_APP_PATH_MAX]; /* the app's directory, absolute */
    char exec[ND_APP_EXEC_MAX];
    int32_t id;
} nd_app_entry;

#define ND_DIAL_BUFFER_MAX 32

/* ------------------------------------------------------------------ *
 * The context
 * ------------------------------------------------------------------ */

typedef struct nd_ui {
    /* --- geometry, assigned in construction step 5 --- */
    int32_t w;              /* 240 */
    int32_t h;              /* 175 */
    int32_t softkey_h;      /* 30  */
    int32_t content_bottom; /* 145 */

    /* --- surfaces --- */
    nd_image *canvas;  /* 240x175 RGB888 = 126,000 bytes. The one long-lived
                        * UI surface. Never cleared wholesale by the core loop;
                        * each render path paints its own background first. */
    nd_draw *draw;     /* bound to canvas */
    nd_fb *fb;         /* the panel */
    nd_image *scratch; /* preallocated 240x(H-SOFTKEY_H) RGB column, lent to
                        * DetailPage so it does not allocate 81,360 bytes per
                        * frame the way the Python does. */

    /* --- fonts, construction step 12 --- */
    nd_font *font_s;  /* 14 */
    nd_font *font_md; /* 18 -- unused by the core, used by apps */
    nd_font *font_n;  /* 20 -- the default face */
    nd_font *font_xl; /* 24 */

    /* --- input --- */
    nd_input *input;
    int keypad_fd;          /* -1 when absent. Widgets poll it DIRECTLY to
                                  * flush pending keys before their first draw. */
    bool has_matrix_keypad; /* framework._t9_active(): the T9 indicator is
                                  * drawn only when this is true */

    /* --- chrome --- */
    nd_ui_state state;
    bool softkey_exists;         /* see the header comment; set at step 9 */
    struct nd_softkey *softkey;  /* the core's own, transparent, bar */
    nd_imgcache *image_cache;    /* 32-entry FIFO */

    /* --- the home screen's own state. PRIVATE, and named with a trailing
     * underscore so that it says so at every use: until the matching _ready
     * flag is set these are all still zero, so reading one directly gets a
     * NULL wallpaper and an empty app list rather than the real thing.
     * Go through nd_ui_wallpaper(), nd_ui_home_layout(),
     * nd_ui_engineering_mode() and nd_ui_app_list(); see "Lazy home state"
     * below. --- */
    struct {
        nd_image *wallpaper;         /* 240x175 RGB dimmed to 30%, or NULL */
        nd_home_layout *home_layout; /* parsed ui_home.json, or NULL */
        bool engineering_mode;
        nd_app_entry apps[ND_APP_MAX];
        size_t n_apps;
        bool wallpaper_ready;
        bool home_layout_ready;
        bool eng_mode_ready;
        bool apps_ready;
    } home_;

    /* --- services, all owned by the core process --- */
    nd_modem *modem;
    nd_battery *battery;
    nd_notify *notify;

    /* --- transient core state --- */
    char dial_buffer[ND_DIAL_BUFFER_MAX];
    int32_t unread_sms;
    bool handling_call;
    bool shutting_down;
} nd_ui;

/* ------------------------------------------------------------------ *
 * Lifecycle -- core process only
 * ------------------------------------------------------------------ */

/* The eighteen steps. Zero-initialises *ui first. May block on the first-boot
 * security notice. */
nd_err nd_ui_init(nd_ui *ui, nd_fb *fb);
void nd_ui_teardown(nd_ui *ui);

/* An app process builds its context from what nd-apprun inherited: the
 * framebuffer, the key channel, the fonts. It gets no modem, no battery and no
 * notify handle -- those live in the core and an app that needs them asks
 * across the boundary. */
nd_err nd_ui_init_app(nd_ui *ui, nd_fb *fb, int keypad_fd);

/* ------------------------------------------------------------------ *
 * The calls widgets and apps actually make
 * ------------------------------------------------------------------ */

/* ui.get_text_size(text, font) -- INK extents. See nd_font.h; this is a thin
 * forward so widget code reads the way the Python did. */
void nd_ui_text_size(const nd_ui *ui, const char *text, const nd_font *f, int32_t *w, int32_t *h);

/* ui.get_image(path[, max_size][, scale]) -- cached, RGBA, NULL on any
 * failure. THE RETURNED IMAGE IS OWNED BY THE CACHE: blit from it, do not free
 * it, and do not hold it across another get_image() that could evict it.
 *
 * The "/home" path fixup is reproduced: a path starting "/home" that contains
 * "NeoDCT" is rewritten as "/NeoDCT" + everything after the last "NeoDCT".
 * That is a development left-over and some manifests may still carry one. */
const nd_image *nd_ui_get_image(nd_ui *ui, const char *path);
const nd_image *nd_ui_get_image_max(nd_ui *ui, const char *path, int32_t max_size);
const nd_image *nd_ui_get_image_scaled(nd_ui *ui, const char *path, double scale);

/* ui.wait_for_key() / ui.read_keypress(timeout).
 *
 * In the CORE these run the battery, modem and ring ticks first, every call,
 * and may return ND_KEY_INCOMING_CALL where the Python raised IncomingCall.
 * In an APP they are a plain read from the inherited channel, and an incoming
 * call arrives as SIGTERM instead -- see nd_app.h. */
int32_t nd_ui_wait_for_key(nd_ui *ui);
int32_t nd_ui_read_keypress(nd_ui *ui, double timeout_s);

/* Present the canvas. Equivalent to nd_fb_update(ui->fb, ui->canvas), spelled
 * the way every widget spells it. */
nd_err nd_ui_present(nd_ui *ui);

/* ------------------------------------------------------------------ *
 * Core-owned screens and plumbing. An app must not call these.
 * ------------------------------------------------------------------ */

void nd_ui_render_home(nd_ui *ui);
void nd_ui_render_home_dialing(nd_ui *ui);
void nd_ui_render_menu(nd_ui *ui); /* blocks until the menu is dismissed */
void nd_ui_update(nd_ui *ui);      /* the per-frame dispatcher */
void nd_ui_handle_input(nd_ui *ui, int32_t code);
void nd_ui_show_pending_battery_warning(nd_ui *ui);
void nd_ui_handle_incoming_call(nd_ui *ui, const char *number);

/* Re-read what an app may have changed. Called after EVERY app exit:
 * system.ui.wallpaper, system.ui.engineering_mode, the app directory scan and
 * the unread-SMS count. This is the mechanism that replaces Settings writing
 * into the core's live memory. */
void nd_ui_refresh_after_app(nd_ui *ui);

/* The scan itself, exposed because Settings' host unit tests drive it. */
size_t nd_ui_scan_apps(const char *dir, nd_app_entry *out, size_t max);

/* Wallpaper: load, resize to 240x175 with LANCZOS, then dim to 30% with the
 * TRUNCATING brightness formula in nd_image.h. NULL on any failure. */
nd_image *nd_ui_load_wallpaper(const char *path);

/* ------------------------------------------------------------------ *
 * Lazy home state
 * ------------------------------------------------------------------ *
 *
 * The wallpaper, the parsed ui_home.json, the engineering-mode flag and the
 * app-directory scan are the HOME SCREEN's state. In the Python they were
 * loaded once by the core's constructor and every app saw them for free,
 * because an app was exec_module()d straight into that same process.
 *
 * A C app is its own process, so it would have to load all four again -- and
 * measured on the phone that is 154 ms of the 180 ms an app spent starting
 * up, for a wallpaper it does not draw, a home layout it does not render and
 * an app list it does not show. Nothing in apps/ reads any of them; the only
 * readers are the home screen, the app selector, the in-call chrome and the
 * crash screen, all of which run in the core.
 *
 * So they are loaded on first read instead of at construction. The core's
 * first home frame pays exactly what its constructor used to, an app process
 * pays nothing, and an app that DOES one day want the wallpaper still gets
 * it -- which a flag saying "skip this in apps" could not have given it.
 *
 * nd_ui_refresh_after_app() invalidates all four rather than reloading them,
 * so returning from an app costs the reload only if the home screen is
 * actually drawn again.
 */
nd_image *nd_ui_wallpaper(nd_ui *ui);
const nd_home_layout *nd_ui_home_layout(nd_ui *ui);
bool nd_ui_engineering_mode(nd_ui *ui);
/* Returns the scanned list and writes its length to *n_out (which may be
 * NULL). The pointer stays valid until the next nd_ui_refresh_after_app(). */
const nd_app_entry *nd_ui_app_list(nd_ui *ui, size_t *n_out);
size_t nd_ui_app_count(nd_ui *ui);

/* Hand the UI a wallpaper directly, taking ownership of it and freeing
 * whatever was there. Marks it loaded, so the configured one is never read.
 * For tests and for nd-shoot; the phone gets its wallpaper from settings. */
void nd_ui_set_wallpaper(nd_ui *ui, nd_image *img);

#ifdef __cplusplus
}
#endif

#endif /* ND_UI_H_INCLUDED */
