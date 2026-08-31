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

/* Where an animated wallpaper is allowed to run -- system.ui.wpanimate. The
 * three values are three different costs; see nd_settings.h. */
typedef enum {
    ND_UI_ANIM_ALWAYS = 0, /* home, menus and apps                        */
    ND_UI_ANIM_HOME,       /* the home screen only; no decoder in an app  */
    ND_UI_ANIM_OFF         /* nowhere; a .gif is its first frame          */
} nd_ui_anim_mode;

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
    bool softkey_exists;        /* see the header comment; set at step 9 */
    struct nd_softkey *softkey; /* the core's own, transparent, bar */
    nd_imgcache *image_cache;   /* 32-entry FIFO */

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
        int32_t unread_sms;
        bool wallpaper_ready;
        bool home_layout_ready;
        bool eng_mode_ready;
        bool apps_ready;
        bool unread_sms_ready;

        /* --- the animated wallpaper, when the setting names a .gif --- *
         *
         * `wallpaper` above stays exactly what it always was: one 240x175
         * RGB888 image, dimmed to 30%. An animation does not add a second
         * surface for its consumers to know about -- it REDRAWS THAT ONE, in
         * place, whenever nd_ui_tick_wallpaper() decides a frame is due. So
         * the home screen, the app selector and the transparent softkey bar
         * animate without any of them being told that anything changed.
         *
         * IN PLACE IS A GUARANTEE, not an implementation detail. The pointer
         * nd_ui_wallpaper() returns is stable for the life of the wallpaper,
         * which is what makes nd_appsel_init() -- which stores it for the
         * whole life of the menu -- safe whatever else ticks in between.
         *
         * The decoder is NULL for a .jpg, and for a .gif with one frame:
         * there is nothing to tick, and holding a decoder open to say so
         * would cost 226 KB for no motion. */
        struct nd_gif *wallpaper_gif;
        double wallpaper_due;   /* nd_time_now() when the next frame is owed */
        uint32_t wallpaper_gen; /* bumped whenever `wallpaper`'s pixels move */

        /* The same frame again, dimmed a second time, for the chrome that
         * apps and dialogs sit on -- see nd_ui_chrome_wallpaper(). Derived
         * lazily and rebuilt only when wallpaper_gen moves, so a still
         * wallpaper builds it once for the life of the process and an
         * animated one rebuilds it only on the frames something actually
         * draws chrome. 240x175 RGB888 = 126,000 bytes. */
        nd_image *chrome;
        uint32_t chrome_gen;
        bool chrome_ready; /* the SETTINGS have been read, not the image */
        bool chrome_enabled;
        double chrome_dim;

        /* system.ui.wpanimate, read with the two chrome settings above and
         * cached with them. See nd_ui_anim_mode(). */
        nd_ui_anim_mode anim_mode;
    } home_;

    /* --- services, all owned by the core process --- */
    nd_modem *modem;
    nd_battery *battery;
    nd_notify *notify;

    /* --- how a BLOCKED widget repaints itself; see nd_ui_set_repaint() --- */
    struct nd_ui_repaint_slot {
        void (*fn)(void *ctx);
        void *ctx;
        bool running; /* reentrancy guard: a repaint must not nest */
    } repaint_;

    /* --- whether THIS PROCESS can advance the animation --- *
     *
     * true in the core and in an app; false only for a hand-built context
     * that runs no loop at all, which is what the host test fixtures are.
     * Such a context would hold ~226 KB and a descriptor on the SD card for a
     * frame it could never reach, so it takes the still first frame instead.
     *
     * An app has no frame loop of its own but does animate: every widget it
     * opens blocks in nd_ui_wait_for_key(), which is where the wallpaper is
     * advanced and the widget repainted. See nd_ui_set_repaint(). */
    bool drives_wallpaper;

    /* --- what THIS PROCESS is allowed to draw the wallpaper behind --- *
     *
     * true in the core and in any app whose manifest.json does not say
     * otherwise. nd-apprun reads the app's own manifest before app_run() and
     * clears this for an app that declared "useWallpaper": false, so the
     * framework's shared background call turns back into a black fill for
     * that process and for nothing else. See nd_app.h. */
    bool app_use_wallpaper;

    /* --- transient core state --- */
    char dial_buffer[ND_DIAL_BUFFER_MAX];
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

/* The modem's twin of the line above. ModemService latches a fault from its
 * own thread; this pops the modal, once, from the home loop on the UI thread.
 * See nd_modem.h's nd_modem_link for what counts as a fault -- notably NOT a
 * phone that simply has no modem, which is Simulation and is fine. */
void nd_ui_show_pending_modem_fault(nd_ui *ui);

/* What that modal says. Written for somebody holding a phone, not for a
 * developer reading a log: it names the fault, gives the one thing worth
 * trying, and says what it means if that does not work. The reason string
 * from the service goes to the console instead. */
#define ND_UI_MODEM_FAULT_MESSAGE                                       \
    "Modem ERROR!\n\nYou may need to restart the device. If this does " \
    "not fix the issue, there is a potential hardware fault."

/* True when the radio is in ND_MODEM_LINK_FAULT. The home screen drops the
 * carrier line entirely when it is. */
bool nd_ui_status_modem_faulted(nd_ui *ui);

void nd_ui_handle_incoming_call(nd_ui *ui, const char *number);

/* Re-read what an app may have changed. Called after EVERY app exit:
 * system.ui.wallpaper, system.ui.engineering_mode, the app directory scan and
 * the unread-SMS count. This is the mechanism that replaces Settings writing
 * into the core's live memory. */
void nd_ui_refresh_after_app(nd_ui *ui);

/* The scan itself, exposed because Settings' host unit tests drive it. */
size_t nd_ui_scan_apps(const char *dir, nd_app_entry *out, size_t max);

/* Wallpaper: load, resize to 240x175 with LANCZOS, then dim to 30% with the
 * TRUNCATING brightness formula in nd_image.h. NULL on any failure.
 *
 * A .gif loads its FIRST FRAME here. Animation belongs to the context, not to
 * a loose image -- see nd_ui_tick_wallpaper(). */
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
 *
 * ============ WHAT WALLPAPER-EVERYWHERE CHANGED HERE ============
 *
 * "Nothing in apps/ reads any of them" stopped being true for the wallpaper
 * when the framework started drawing it behind app chrome. An app whose
 * manifest does not say "useWallpaper": false now loads it on its FIRST
 * BACKGROUND CLEAR -- lazily still, and once, but it is a cost that app did
 * not pay before, and it is the largest of the four.
 *
 * That is the feature, not an oversight: the pixels have to come from
 * somewhere and the core cannot hand them across a process boundary. The
 * other three are untouched, an app that opted out pays nothing at all, and
 * an app that never clears a background never loads it either.
 *
 * An app that loads it also ANIMATES it, under the default
 * system.ui.wpanimate: it runs no frame loop, but every widget it opens
 * blocks in nd_ui_wait_for_key(), which is where the wallpaper advances --
 * see nd_ui_set_repaint() and drives_wallpaper. That is the one cost
 * system.ui.wpanimate=HOME exists to take back, and it takes back the
 * decoder with it.
 */
nd_image *nd_ui_wallpaper(nd_ui *ui);
const nd_home_layout *nd_ui_home_layout(nd_ui *ui);
bool nd_ui_engineering_mode(nd_ui *ui);

/* The red line under the carrier on the home screen, and the exact string
 * ui_home.json authors for it.
 *
 * It is a MACRO rather than a literal in two files because the layout and the
 * renderer have to agree on it byte for byte: nd_ui_render_home() decides
 * whether to draw the element by strcmp'ing el->text against this, which is
 * the same mechanism nd_layout.c uses for the clock and the carrier ("there
 * is no marker syntax; these exact strings are the whole mechanism"). A typo
 * in either place does not fail to build -- it silently draws the line
 * always, or never. tests/test_home_layout.py pins the two together. */
#define ND_UI_ENG_MODE_LABEL "Eng. Mode"
/* Returns the scanned list and writes its length to *n_out (which may be
 * NULL). The pointer stays valid until the next nd_ui_refresh_after_app(). */
const nd_app_entry *nd_ui_app_list(nd_ui *ui, size_t *n_out);
size_t nd_ui_app_count(nd_ui *ui);

/* Hand the UI a wallpaper directly, taking ownership of it and freeing
 * whatever was there. Marks it loaded, so the configured one is never read.
 * Also closes any animation, so an injected still stays the still it is.
 * For tests and for nd-shoot; the phone gets its wallpaper from settings.
 *
 * This is the ONE call that changes the pointer nd_ui_wallpaper() returns,
 * and no widget may be holding it across one. */
void nd_ui_set_wallpaper(nd_ui *ui, nd_image *img);

/* ------------------------------------------------------------------ *
 * Animated wallpaper
 * ------------------------------------------------------------------ *
 *
 * A .gif wallpaper is decoded one frame at a time and painted over the SAME
 * nd_image every consumer of nd_ui_wallpaper() already holds, so nothing
 * downstream needs to know an animation exists. What the core does need to
 * know is when to ask for the next frame, and how long it may sleep first.
 */

/* Advance the wallpaper if its current frame has been on screen long enough.
 * Cheap and idempotent: a still wallpaper, a wallpaper that is not due yet
 * and a phone with no wallpaper at all all return false without touching a
 * pixel. true means the image changed and the screen wants repainting.
 *
 * CALLED FROM ONE PLACE, nd_ui_update(). Advancing inside nd_ui_wallpaper()
 * would run the animation once per CALLER -- the home screen, the softkey bar
 * and the app selector each ask for the wallpaper on the same frame -- and
 * play a 25 fps GIF at 75. */
bool nd_ui_tick_wallpaper(nd_ui *ui);

/* How long a caller may block before the wallpaper owes another frame, in
 * seconds, never more than `dflt` and never less than zero. `dflt` back when
 * nothing is animating, which is what makes this safe to wrap around the core
 * loop's existing 0.1 s poll unconditionally.
 *
 * FOR THE CORE LOOP, which advances the wallpaper itself every time round. A
 * caller that does NOT advance it must use nd_ui_widget_timeout() instead --
 * see the warning there. */
double nd_ui_frame_timeout(nd_ui *ui, double dflt);

/* The same question asked by a BLOCKING WIDGET, and the answer is not the
 * same one.
 *
 * nd_ui_frame_timeout() reports the time until a frame is DUE, and once one
 * is overdue that is zero. For the core loop zero is right: it draws the
 * frame immediately and the deadline moves on. For a widget it is a trap --
 * if nothing in this wait advances the wallpaper, the deadline never moves,
 * the timeout stays zero, and the wait becomes a 100% CPU spin on a phone
 * that is sitting still. Measured with the frame forced overdue: one second
 * of wall time in the wait burned 1.0006 s of CPU before this existed and
 * 0.0013 s after, in every mode that does not tick from the widget.
 *
 * So this returns the short timeout only when this wait really is going to
 * advance the wallpaper -- a repainter is registered AND system.ui.wpanimate
 * allows it here -- and `dflt` otherwise. Every loop that waits without
 * registering a repainter (the crash screen, an app's own key loop) therefore
 * polls exactly as it always did. */
double nd_ui_widget_timeout(nd_ui *ui, double dflt);

/* ------------------------------------------------------------------ *
 * The background under the framework's own chrome
 * ------------------------------------------------------------------ *
 *
 * Every list, dialog, text box and info screen used to open by filling itself
 * black. They now call nd_ui_paint_chrome() instead, which paints the
 * wallpaper -- dimmed a second time, so that white 20 px text stays readable
 * over a photograph -- or black, and the widget does not care which.
 *
 * It is black when ANY of these is true:
 *   - there is no wallpaper;
 *   - system.ui.wpeverywhere is off;
 *   - this process is an app whose manifest says "useWallpaper": false.
 */

/* The extra-dimmed wallpaper, or NULL for "use black". Owned by the UI and
 * rebuilt under the caller whenever the animation advances -- blit from it,
 * do not free it, do not keep it. */
const nd_image *nd_ui_chrome_wallpaper(nd_ui *ui);

/* Paint `r` (INCLUSIVE, like every other rectangle here) with the chrome
 * background: the matching REGION of the wallpaper, or black. The region
 * matters -- a widget that clears rows 0..145 must not be handed the
 * wallpaper's rows 0..145 stretched, it must be handed its rows 0..145, or
 * the softkey strip stops lining up with the screen above it. */
void nd_ui_paint_chrome(nd_ui *ui, nd_rect r);

/* The whole screen, and the content area above the softkey bar: the two
 * rectangles almost every call site wants. Both are passed exactly as the
 * widgets have always written them -- see the note at the implementation
 * about the off-by-one that is deliberately preserved. */
void nd_ui_paint_chrome_full(nd_ui *ui);
void nd_ui_paint_chrome_content(nd_ui *ui);

/* system.ui.wpanimate, read once per process and again after each app exit.
 * Public because nd-shoot and the tests want to see which mode is in force
 * without re-reading the setting themselves. */
nd_ui_anim_mode nd_ui_anim_mode_of(nd_ui *ui);

/* Drop the cached chrome image and re-read both settings. Called from
 * nd_ui_refresh_after_app(), because Settings is an app and turning the
 * feature off must show on the next screen drawn. */
void nd_ui_invalidate_chrome(nd_ui *ui);

/* ------------------------------------------------------------------ *
 * Repainting a widget that is blocked on a key
 * ------------------------------------------------------------------ *
 *
 * THE PROBLEM THIS SOLVES. nd_ui_update() advances the wallpaper once per
 * frame, and the core loop calls it. But every widget is a BLOCKING loop --
 * nd_vlist_show() draws once and then sits in nd_ui_wait_for_key() until you
 * press something -- and while one is up, the core loop is inside it. So the
 * animation stopped dead the moment a menu opened, which is most of the time
 * anyone is looking at the phone.
 *
 * A widget therefore hands over a way to redraw itself for the duration. The
 * key wait advances the wallpaper and, on the frames where it moved, calls
 * that back; the widget's own draw() puts the menu back on top of the new
 * background. Nothing else about the widget changes, and a widget that does
 * not register one behaves exactly as it did.
 *
 * IT IS SAVE-AND-RESTORE, NOT SET-AND-CLEAR, because widgets nest: a dialog
 * opened from a list must give the list its repainter back on the way out.
 *
 *     nd_ui_repaint saved = nd_ui_set_repaint(ui, my_repaint, self);
 *     ... blocking loop ...
 *     nd_ui_restore_repaint(ui, saved);
 *
 * THE CALLBACK MUST NOT BLOCK and must not wait for a key itself -- it is
 * called from inside the key wait. Drawing and presenting is all it may do.
 * A nested call is refused rather than recursed into.
 */
typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
} nd_ui_repaint;

/* Returns what was there, to be handed to nd_ui_restore_repaint(). */
nd_ui_repaint nd_ui_set_repaint(nd_ui *ui, void (*fn)(void *ctx), void *ctx);
void nd_ui_restore_repaint(nd_ui *ui, nd_ui_repaint saved);

/* The unread-SMS count. It lives with the home state because that is the
 * only thing that reads it: the flashing envelope in nd_ui_render_home().
 *
 * It was the most expensive thing in an app launch by a wide margin --
 * 60-70 ms of the 130, measured on the phone with NEODCT_BENCH -- because
 * counting it opens SQLite, and every app process was doing that for a
 * number no app has ever looked at. On a desktop it costs 0.005 ms, which
 * is exactly why it was not noticed until it was measured on the target. */
int32_t nd_ui_unread_sms(nd_ui *ui);
void nd_ui_set_unread_sms(nd_ui *ui, int32_t n);

#ifdef __cplusplus
}
#endif

#endif /* ND_UI_H_INCLUDED */
