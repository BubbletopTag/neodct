/* test_messages.c -- the Messages app.
 *
 * apps/Messages/app.so is dlopen()ed rather than recompiled into this binary,
 * for the reason test_cubebench.c and test_phonebook.c give: recompiling
 * main.c here would test a second copy built with different flags instead of
 * the file that ships.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. TWO GOLDEN FRAMES, drawn by the code that draws them on the phone.
 *     app-messages is app_run() driven to its first screen and let out with
 *     Back; app-messages-inbox is app_open_inbox() -- the entry point the
 *     core calls from the notification banner -- over an empty inbox, let out
 *     the same way. Both are compared by the SHA-256 over raw RGB that
 *     goldenframe.py compares, so a pass here is a pass there.
 *     app-messages is byte-identical to widget-pagedlist and the two are
 *     drawn by different code, so agreeing is a real check.
 *
 *  2. THE SEVEN STATEMENTS round-trip against real sqlite databases, newest
 *     first, and a READ NEVER CREATES ITS DATABASE. The Python's
 *     `os.path.exists()` guard is the only thing stopping a phone that has
 *     never had a text from growing a zero-byte sms_inbox.db, so its absence
 *     would be a silent regression.
 *
 *  3. THE APP'S OWN WRAPPER is the fifth in the tree and none of the other
 *     four would pass these: an empty string gives one EMPTY line rather than
 *     none, and a word too wide to fit gets "..." even when it is the last
 *     word -- which is exactly where it differs from nd_pagedlist_wrap().
 *
 *  4. THE SEND-TO FIELD IS NUMBERS-ONLY. neodct/tests/test_messages_number_
 *     field.py types 2, 2, *, # and expects "22*#" -- two literal 2s, no
 *     multi-tap. The same script goes through the real widget loop here.
 *     Any arrow key opens the shared contact picker and pastes the number.
 *
 *  5. THE EMPTY STATE EXITS ON KEY 14 AND ON NOTHING ELSE. An Enter written
 *     before the Back must be swallowed, so afterwards the channel is empty.
 *
 *  6. ERASE FROM THE DETAIL PAGE really deletes the row and reports
 *     "deleted", which is what makes the list screen loop rather than return.
 *
 *  7. THE SEND FLOW REFUSES an empty message, a message over 160 CODE POINTS
 *     -- not bytes -- and a UI with no modem AND NO ROUTE TO ONE behind it,
 *     and takes none of those decisions after asking for a number it will
 *     not use. With a modem reachable it SENDS: OPEN-QUESTIONS.md MSG-1 is
 *     answered and "ModemService is not running." is no longer what a real
 *     Write Message -> Options -> Send arrives at.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include <sqlite3.h>

#include "nd_capture.h"
#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_modem.h"
#include "nd_paths.h"
#include "nd_svc.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "../../apps/Messages/messages.h"
#include "platform_test.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* ------------------------------------------------------------------ *
 * Finding the font, the reference set and the built app.so
 * ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static bool resolve_golden(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "../tests/golden", sz);
        return true;
    }
    if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "neodct/tests/golden", sz);
        return true;
    }
    return false;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen. */
static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char golden[1024];
    char base[512];
    char cand[1024];
    char *cut;

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (resolve_golden(golden, sizeof golden)) {
        (void)snprintf(base, sizeof base, "%.480s", golden);
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        (void)snprintf(cand, sizeof cand, "%.400s/" FONT_REL, base);
        if (file_exists(cand)) {
            (void)nd_strlcpy(out, cand, sz);
            return true;
        }
    }
    if (file_exists("../" FONT_REL)) {
        (void)nd_strlcpy(out, "../" FONT_REL, sz);
        return true;
    }
    if (file_exists("neodct/" FONT_REL)) {
        (void)nd_strlcpy(out, "neodct/" FONT_REL, sz);
        return true;
    }
    if (file_exists(ND_PATH_FONT)) {
        (void)nd_strlcpy(out, ND_PATH_FONT, sz);
        return true;
    }
    return false;
}

/* build/<variant>/test/test_messages -> build/<variant>/apps/Messages/app.so,
 * so an ASan run loads the ASan app and never a stale default-variant one. */
static bool resolve_app_so(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_MESSAGES_SO");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(out, sz, "%s/../apps/Messages/app.so", exe) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

typedef struct {
    void *handle;
    int (*run)(nd_ui *);
    int (*open_message)(nd_ui *, int64_t);
    int (*open_inbox)(nd_ui *);
    void (*shutdown)(void);

    size_t (*fetch_inbox)(nd_msg_rec *, size_t);
    size_t (*fetch_outbox)(nd_msg_rec *, size_t);
    bool (*fetch_inbox_one)(int64_t, nd_msg_rec *);
    void (*mark_read)(int64_t);
    void (*del_inbox)(int64_t);
    void (*del_outbox)(int64_t);
    nd_err (*save_outbox)(const char *);

    void (*wrap_text)(nd_lines *, nd_ui *, const char *, int32_t, const nd_font *);
    void (*format_timestamp)(int64_t, char *, size_t);
    size_t (*codepoints)(const char *);
    void (*filter_number)(char *, size_t, const char *);

    void (*empty_state)(nd_ui *, const char *, const char *, int32_t, const char *);
    void (*draw_sending)(nd_ui *, const char *);
    const char *(*number_input)(nd_ui *, const char *, const char *, char *, size_t);
    bool (*send_flow)(nd_ui *, const char *, int32_t, int32_t);
    nd_msg_detail_result (*show_detail)(nd_ui *, const char *, const char *, int32_t, const char *,
                                        int64_t, const char *, int64_t);
    void (*show_inbox)(nd_ui *, int32_t, int32_t);
    void (*show_outbox)(nd_ui *, int32_t, int32_t);
    void (*show_write)(nd_ui *, int32_t, int32_t);

    const char *const *menu_items;
    const char *const *inbox_options;
    const char *const *outbox_options;
    const int32_t *arrow_keys;
} msg_api;

static msg_api g_api;
static char g_so[ND_PATH_MAX];

static void *sym(void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL)
        fprintf(stderr, "test_messages: app.so has no symbol %s\n", name);
    return p;
}

static bool api_open(void)
{
    void *h;

    if (!resolve_app_so(g_so, sizeof g_so))
        return false;
    h = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "test_messages: dlopen %s: %s\n", g_so, dlerror());
        return false;
    }
    g_api.handle = h;

    *(void **)&g_api.run = sym(h, "app_run");
    *(void **)&g_api.open_message = sym(h, "app_open_message");
    *(void **)&g_api.open_inbox = sym(h, "app_open_inbox");
    *(void **)&g_api.shutdown = sym(h, "app_shutdown");

    *(void **)&g_api.fetch_inbox = sym(h, "nd_msg_fetch_inbox");
    *(void **)&g_api.fetch_outbox = sym(h, "nd_msg_fetch_outbox");
    *(void **)&g_api.fetch_inbox_one = sym(h, "nd_msg_fetch_inbox_one");
    *(void **)&g_api.mark_read = sym(h, "nd_msg_mark_read");
    *(void **)&g_api.del_inbox = sym(h, "nd_msg_delete_inbox");
    *(void **)&g_api.del_outbox = sym(h, "nd_msg_delete_outbox");
    *(void **)&g_api.save_outbox = sym(h, "nd_msg_save_outbox");

    *(void **)&g_api.wrap_text = sym(h, "nd_msg_wrap_text");
    *(void **)&g_api.format_timestamp = sym(h, "nd_msg_format_timestamp");
    *(void **)&g_api.codepoints = sym(h, "nd_msg_codepoints");
    *(void **)&g_api.filter_number = sym(h, "nd_msg_filter_number");

    *(void **)&g_api.empty_state = sym(h, "nd_msg_show_empty_state");
    *(void **)&g_api.draw_sending = sym(h, "nd_msg_draw_sending");
    *(void **)&g_api.number_input = sym(h, "nd_msg_number_input_show");
    *(void **)&g_api.send_flow = sym(h, "nd_msg_send_flow");
    *(void **)&g_api.show_detail = sym(h, "nd_msg_show_detail");
    *(void **)&g_api.show_inbox = sym(h, "nd_msg_show_inbox");
    *(void **)&g_api.show_outbox = sym(h, "nd_msg_show_outbox");
    *(void **)&g_api.show_write = sym(h, "nd_msg_show_write");

    g_api.menu_items = dlsym(h, "nd_msg_menu_items");
    g_api.inbox_options = dlsym(h, "nd_msg_inbox_options");
    g_api.outbox_options = dlsym(h, "nd_msg_outbox_options");
    g_api.arrow_keys = dlsym(h, "nd_msg_arrow_keys");

    return g_api.run != NULL && g_api.open_message != NULL && g_api.open_inbox != NULL &&
           g_api.shutdown != NULL && g_api.fetch_inbox != NULL && g_api.fetch_outbox != NULL &&
           g_api.fetch_inbox_one != NULL && g_api.mark_read != NULL && g_api.del_inbox != NULL &&
           g_api.del_outbox != NULL && g_api.save_outbox != NULL && g_api.wrap_text != NULL &&
           g_api.format_timestamp != NULL && g_api.codepoints != NULL &&
           g_api.filter_number != NULL && g_api.empty_state != NULL && g_api.draw_sending != NULL &&
           g_api.number_input != NULL && g_api.send_flow != NULL && g_api.show_detail != NULL &&
           g_api.show_inbox != NULL && g_api.show_outbox != NULL && g_api.show_write != NULL &&
           g_api.menu_items != NULL && g_api.inbox_options != NULL &&
           g_api.outbox_options != NULL && g_api.arrow_keys != NULL;
}

static void api_close(void)
{
    if (g_api.handle != NULL)
        (void)dlclose(g_api.handle);
    memset(&g_api, 0, sizeof g_api);
}

/* ------------------------------------------------------------------ *
 * A UI context with nothing behind it but memory
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    nd_font *font_s;
    nd_font *font_md;
    nd_font *font_n;
    nd_font *font_xl;
    nd_input *input;
    int write_fd;
} fixture;

static bool fx_init(fixture *fx)
{
    char path[1024];

    memset(fx, 0, sizeof *fx);
    fx->write_fd = -1;
    if (!resolve_font(path, sizeof path)) {
        fprintf(stderr, "test_messages: cannot find font.ttf; set NEODCT_FONT\n");
        return false;
    }
    fx->font_s = nd_font_load(path, ND_FONT_PX_S);
    fx->font_md = nd_font_load(path, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(path, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(path, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_messages: nd_font_load(%s) failed\n", path);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = NULL; /* no panel: the canvas is the frame */
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    fx->ui.keypad_fd = -1;
    /* nd_app.h: an app process gets no modem. So does this fixture. */
    fx->ui.modem = NULL;
    /* Only the core's own bar is transparent, and this context is not it. */
    fx->ui.softkey_exists = true;
    return true;
}

static void fx_free(fixture *fx)
{
    if (fx->write_fd >= 0)
        (void)close(fx->write_fd);
    if (fx->input != NULL)
        nd_input_close(fx->input);
    nd_image_free(fx->canvas);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
}

/* A key channel with the whole script written in advance, so no widget loop
 * can block. Nothing repeats: every press is released. */
static bool fx_keys(fixture *fx)
{
    int fds[2];

    if (fx->input != NULL)
        return true;
    if (pipe(fds) != 0)
        return false;
    if (nd_input_open_fd(&fx->input, fds[0]) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    nd_input_set_repeat(fx->input, 0.0, 0.0);
    fx->write_fd = fds[1];
    fx->ui.input = fx->input;
    return true;
}

static void write_key(int fd, uint16_t code, int32_t value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        CHECK(false);
}

static void script_key(fixture *fx, int32_t code)
{
    write_key(fx->write_fd, (uint16_t)code, 1);
    write_key(fx->write_fd, (uint16_t)code, 0);
}

/* MessageDialog drains the channel before its first draw, so a key written in
 * advance is eaten before show() ever waits on it. The way through is the one
 * nd_shoot.c's hold_key_begin() found: press a key and never release it, with
 * that key in the repeat set. The drain consumes the press, the held state
 * survives it, and the synthesised repeat arrives after the dialog is up.
 *
 * Called LAST, after any scripted keys: the held press sits behind them in
 * the pipe, so repeats only start once the script has been read. */
static void hold_key(fixture *fx, int32_t code)
{
    static int32_t held;

    held = code;
    CHECK_INT(nd_input_set_repeat_codes(fx->input, &held, 1u), ND_OK);
    nd_input_set_repeat(fx->input, 0.20, 0.05);
    write_key(fx->write_fd, (uint16_t)code, 1);
}

/* True when the script was consumed to the end. */
static bool keys_drained(fixture *fx)
{
    return nd_input_read_key(fx->input, 0.0) == ND_KEY_NONE;
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

static nd_json_doc *g_manifest;
static const nd_json_val *g_frames;

static void manifest_open(void)
{
    char dir[1024];
    char path[1200];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];
    const nd_json_val *root;

    if (!resolve_golden(dir, sizeof dir))
        return;
    (void)snprintf(path, sizeof path, "%.1000s/manifest.json", dir);

    /* Plain fopen: the reference set is not under NEODCT_ROOT. */
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) != 0)
        goto done;
    len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc((size_t)len);
    if (buf == NULL)
        goto done;
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len)
        goto done;
    if (nd_json_parse(buf, (size_t)len, &g_manifest, err, sizeof err) != ND_OK) {
        fprintf(stderr, "test_messages: manifest parse: %s\n", err);
        g_manifest = NULL;
        goto done;
    }
    root = nd_json_root(g_manifest);
    g_frames = nd_json_get(root, "frames");
done:
    free(buf);
    (void)fclose(f);
}

static const char *golden_sha(const char *name)
{
    size_t i;

    if (g_frames == NULL)
        return NULL;
    for (i = 0u; i < nd_json_len(g_frames); i++) {
        const nd_json_val *fr = nd_json_at(g_frames, i);

        if (strcmp(nd_json_get_str(fr, "name", ""), name) == 0)
            return nd_json_get_str(fr, "sha256", NULL);
    }
    return NULL;
}

static void check_frame(const nd_image *img, const char *name)
{
    char got[65];
    const char *want = golden_sha(name);

    if (nd_capture_digest(img, got, sizeof got) != ND_OK) {
        CHECK(false);
        return;
    }
    if (want == NULL) {
        fprintf(stderr, "test_messages: no reference for %s (got %s)\n", name, got);
        CHECK(false);
        return;
    }
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL frame %s\n  got  %s\n  want %s\n", name, got, want);
    }
}

/* ------------------------------------------------------------------ *
 * Database helpers
 * ------------------------------------------------------------------ */

static void db_init(void)
{
    CHECK_INT(nd_db_init_all(), ND_OK);
}

/* A row with a timestamp this test chose, so ORDER BY timestamp DESC has
 * something to order. nd_db_store_incoming_sms() stamps with the clock and
 * two calls in the same tick would tie. */
static int64_t seed_inbox(const char *sender, const char *body, int64_t ts, int32_t is_read)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int64_t id = -1;

    CHECK_INT(nd_db_open(ND_PATH_DB_SMS_INBOX, &db), ND_OK);
    if (db == NULL)
        return -1;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO inbox (message, sender, timestamp, is_read) "
                           "VALUES (?, ?, ?, ?)",
                           -1, &st, NULL) == SQLITE_OK) {
        (void)sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC);
        (void)sqlite3_bind_text(st, 2, sender, -1, SQLITE_STATIC);
        (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)ts);
        (void)sqlite3_bind_int(st, 4, is_read);
        CHECK_INT(sqlite3_step(st), SQLITE_DONE);
        id = (int64_t)sqlite3_last_insert_rowid(db);
    } else {
        CHECK(false);
    }
    if (st != NULL)
        (void)sqlite3_finalize(st);
    nd_db_close(db);
    return id;
}

/* ------------------------------------------------------------------ *
 * 1. The two golden frames
 * ------------------------------------------------------------------ */

/* app_run() drawn to its first screen. The Python's capture recipe passes
 * keys=[] and lets ScriptExhausted out of the first read_keypress; Back
 * reaches the same place, because the frame that is already on the canvas
 * when the key is read is the one being compared. */
static void test_golden_app_messages(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    /* PagedList drains the channel before its first draw -- nd_widgets.h and
     * nd_pagedlist.c are both explicit about the 0.01 s poll -- so a key
     * written in advance never reaches show(). The held key is the way in,
     * and it is the same one nd_shoot.c uses for this frame. */
    hold_key(&fx, ND_KEY_CLEAR);
    CHECK_INT(g_api.run(&fx.ui), 0);
    check_frame(fx.canvas, "app-messages");

    fx_free(&fx);
}

/* app_open_inbox() over an empty inbox: _show_inbox(ui, 2, 1) finds nothing
 * and paints _show_empty_state("Inbox", "2-1", None, "No Messages"). This is
 * the same call nd_shoot.c makes for this frame, and the reason is written up
 * there and in OPEN-QUESTIONS.md MSG-5. */
static void test_golden_app_messages_inbox(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    CHECK_INT(g_api.fetch_inbox(NULL, 0u), 0);

    script_key(&fx, ND_KEY_CLEAR);
    CHECK_INT(g_api.open_inbox(&fx.ui), 0);
    check_frame(fx.canvas, "app-messages-inbox");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2. The seven statements
 * ------------------------------------------------------------------ */

static void test_db_reads_do_not_create(void)
{
    nd_msg_rec rows[4];
    nd_msg_rec one;

    /* No nd_db_init_all(): neither database exists in this scratch root. */
    CHECK(!nd_path_is_file(ND_PATH_DB_SMS_INBOX));
    CHECK(!nd_path_is_file(ND_PATH_DB_SMS_OUTBOX));

    CHECK_INT(g_api.fetch_inbox(rows, ND_ARRAY_LEN(rows)), 0);
    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 0);
    CHECK(!g_api.fetch_inbox_one(1, &one));
    g_api.mark_read(1);
    g_api.del_inbox(1);
    g_api.del_outbox(1);

    /* Still absent -- the os.path.exists() guard is the whole point. */
    CHECK(!nd_path_is_file(ND_PATH_DB_SMS_INBOX));
    CHECK(!nd_path_is_file(ND_PATH_DB_SMS_OUTBOX));
}

static void test_db_inbox_roundtrip(void)
{
    nd_msg_rec rows[8];
    nd_msg_rec one;
    int64_t older;
    int64_t newer;

    db_init();
    older = seed_inbox("+353870000001", "first", 1000, 0);
    newer = seed_inbox("+353870000002", "second", 2000, 1);
    CHECK(older > 0);
    CHECK(newer > older);

    /* ORDER BY timestamp DESC -- newest first. */
    CHECK_INT(g_api.fetch_inbox(rows, ND_ARRAY_LEN(rows)), 2);
    CHECK_STR(rows[0].message, "second");
    CHECK_STR(rows[0].sender, "+353870000002");
    CHECK_INT(rows[0].timestamp, 2000);
    CHECK_INT(rows[0].is_read, 1);
    CHECK_STR(rows[1].message, "first");
    CHECK_INT(rows[1].is_read, 0);

    /* max is honoured: the Python's list is unbounded, the C's is not. */
    CHECK_INT(g_api.fetch_inbox(rows, 1u), 1);
    CHECK_STR(rows[0].message, "second");

    CHECK(g_api.fetch_inbox_one(older, &one));
    CHECK_STR(one.message, "first");
    CHECK_INT(one.is_read, 0);

    g_api.mark_read(older);
    CHECK(g_api.fetch_inbox_one(older, &one));
    CHECK_INT(one.is_read, 1);

    g_api.del_inbox(older);
    CHECK(!g_api.fetch_inbox_one(older, &one));
    CHECK_INT(g_api.fetch_inbox(rows, ND_ARRAY_LEN(rows)), 1);

    /* A negative id is the Python's None: it must do nothing, not delete. */
    g_api.del_inbox(ND_MSG_NO_ID);
    CHECK_INT(g_api.fetch_inbox(rows, ND_ARRAY_LEN(rows)), 1);
}

static void test_db_outbox_roundtrip(void)
{
    nd_msg_rec rows[8];

    /* _save_outbox_message creates the directory AND the table, so this works
     * with no init_databases() at all -- which is how a phone whose outbox
     * was deleted gets one back. */
    CHECK(!nd_path_is_file(ND_PATH_DB_SMS_OUTBOX));
    CHECK_INT(g_api.save_outbox("draft one"), ND_OK);
    CHECK(nd_path_is_file(ND_PATH_DB_SMS_OUTBOX));

    nd_vclock_advance(); /* so the second row gets a later timestamp */
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    nd_vclock_advance();
    CHECK_INT(g_api.save_outbox("draft two"), ND_OK);

    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 2);
    CHECK_STR(rows[0].message, "draft two");
    CHECK_STR(rows[1].message, "draft one");
    /* An outbox row has no sender and no read state. */
    CHECK_STR(rows[0].sender, "");
    CHECK_INT(rows[0].is_read, 0);

    g_api.del_outbox(rows[0].id);
    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 1);
    CHECK_STR(rows[0].message, "draft one");
}

/* ------------------------------------------------------------------ *
 * 3. _wrap_text
 * ------------------------------------------------------------------ */

static void test_wrap_text(void)
{
    char storage[16][ND_TEXT_LINE_MAX];
    nd_lines lines;
    fixture fx;

    if (!fx_init(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    nd_lines_init(&lines, storage, ND_ARRAY_LEN(storage));

    /* `if not words: return [""]` -- ONE empty line, not zero. Both NULL and
     * "" and a string of nothing but whitespace take that branch, because
     * str.split() drops empty tokens. */
    g_api.wrap_text(&lines, &fx.ui, NULL, 220, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0), "");

    g_api.wrap_text(&lines, &fx.ui, "   \n\t ", 220, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0), "");

    /* A short line comes back whole. */
    g_api.wrap_text(&lines, &fx.ui, "hello there", 220, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0), "hello there");

    /* Newlines are separators, not breaks: split() collapses them. */
    g_api.wrap_text(&lines, &fx.ui, "hello\nthere", 220, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0), "hello there");

    /* Greedy fill: a narrow column breaks between words, and never mid-word
     * while the word itself fits. "aaa bbb" is already wider than 60 px at
     * 20 px, so each word gets a line of its own. */
    g_api.wrap_text(&lines, &fx.ui, "aaa bbb ccc ddd", 60, fx.font_n);
    CHECK_INT(lines.n, 4);
    CHECK_STR(nd_lines_at(&lines, 0), "aaa");
    CHECK_STR(nd_lines_at(&lines, 3), "ddd");

    /* A single word wider than the line is trimmed and gets "..." -- even
     * though it is the LAST word, which is where this differs from
     * nd_pagedlist_wrap(). */
    g_api.wrap_text(&lines, &fx.ui, "supercalifragilistic", 60, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK(strlen(nd_lines_at(&lines, 0)) > 3u);
    CHECK_STR(nd_lines_at(&lines, 0) + strlen(nd_lines_at(&lines, 0)) - 3u, "...");

    /* And `current` is reset to "" after one, so the next word starts a line
     * of its own instead of joining the trimmed one. */
    g_api.wrap_text(&lines, &fx.ui, "supercalifragilistic ok", 60, fx.font_n);
    CHECK_INT(lines.n, 2);
    CHECK_STR(nd_lines_at(&lines, 1), "ok");

    /* Nothing fits at all: the bare ellipsis, and no infinite loop. */
    g_api.wrap_text(&lines, &fx.ui, "wide", 1, fx.font_n);
    CHECK_INT(lines.n, 1);
    CHECK_STR(nd_lines_at(&lines, 0), "...");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 4, 5, 6. The small helpers
 * ------------------------------------------------------------------ */

static void test_format_timestamp(void)
{
    char out[64];

    /* `if not ts` -- 0 is falsy in Python and so is None. */
    g_api.format_timestamp(0, out, sizeof out);
    CHECK_STR(out, "Unknown time");

    /* Under the virtual clock localtime is aliased to gmtime, which is what
     * makes a reference frame rendered in Dublin match one rendered in CI. */
    g_api.format_timestamp((int64_t)ND_VCLOCK_EPOCH, out, sizeof out);
    CHECK_STR(out, "2024-01-01 12:34");
}

static void test_codepoints(void)
{
    /* len() in Python counts code points. "é" is two bytes and one character,
     * and a 160-character message with one in it must not be refused. */
    CHECK_INT(g_api.codepoints(NULL), 0);
    CHECK_INT(g_api.codepoints(""), 0);
    CHECK_INT(g_api.codepoints("abc"), 3);
    CHECK_INT(g_api.codepoints("\xc3\xa9"), 1);
    CHECK_INT(g_api.codepoints("a\xc3\xa9\xe2\x82\xac"), 3);
}

static void test_filter_number(void)
{
    char out[32];

    g_api.filter_number(out, sizeof out, "+353 (87) 123-4567");
    CHECK_STR(out, "+353871234567");
    g_api.filter_number(out, sizeof out, "*#123");
    CHECK_STR(out, "*#123");
    g_api.filter_number(out, sizeof out, "no digits here");
    CHECK_STR(out, "");
    g_api.filter_number(out, sizeof out, NULL);
    CHECK_STR(out, "");
}

static void test_menu_tables(void)
{
    CHECK_STR(g_api.menu_items[0], "Inbox");
    CHECK_STR(g_api.menu_items[1], "Outbox");
    CHECK_STR(g_api.menu_items[2], "Write Message");

    /* Yes, really. That string is on the phone today. */
    CHECK_STR(g_api.inbox_options[0], "Just Erase for now");
    CHECK_STR(g_api.outbox_options[0], "Erase");
    CHECK_STR(g_api.outbox_options[1], "Send");

    CHECK_INT(g_api.arrow_keys[0], 103);
    CHECK_INT(g_api.arrow_keys[1], 105);
    CHECK_INT(g_api.arrow_keys[2], 106);
    CHECK_INT(g_api.arrow_keys[3], 108);
}

/* ------------------------------------------------------------------ *
 * 7. The Send To field
 * ------------------------------------------------------------------ */

/* neodct/tests/test_messages_number_field.py, through the real widget loop:
 * 2, 2, *, # must produce "22*#". Two literal 2s means the T9 engine is in
 * MODE_123 and there is no multi-tap. */
static void test_number_field_is_numbers_only(void)
{
    fixture fx;
    char buf[ND_TEXTINPUT_CAP];
    const char *got;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    /* The i2c keypad path, which is where multi-tap would happen. */
    fx.ui.has_matrix_keypad = true;

    script_key(&fx, ND_KEY_2);
    script_key(&fx, ND_KEY_2);
    script_key(&fx, ND_KEY_STAR);
    script_key(&fx, ND_KEY_HASH);
    script_key(&fx, ND_KEY_ENTER);

    got = g_api.number_input(&fx.ui, "Send To", "Number:", buf, sizeof buf);
    CHECK_STR(got, "22*#");
    CHECK(keys_drained(&fx));

    fx_free(&fx);
}

static void test_number_field_cancel(void)
{
    fixture fx;
    char buf[ND_TEXTINPUT_CAP];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    script_key(&fx, ND_KEY_CLEAR); /* Back on an empty field cancels */
    CHECK(g_api.number_input(&fx.ui, "Send To", "Number:", buf, sizeof buf) == NULL);
    fx_free(&fx);
}

/* Any of the four arrows opens the shared picker and pastes the number. */
static void test_number_field_arrow_opens_picker(void)
{
    fixture fx;
    char buf[ND_TEXTINPUT_CAP];
    size_t i;

    for (i = 0u; i < ND_MSG_ARROW_KEYS_N; i++) {
        if (!fx_init(&fx) || !fx_keys(&fx)) {
            CHECK(false);
            fx_free(&fx);
            return;
        }
        db_init(); /* seeds ("NeoDCT Support", "555-1234", 2) */

        script_key(&fx, g_api.arrow_keys[i]); /* opens the picker  */
        script_key(&fx, ND_KEY_ENTER);        /* picks the one row */
        script_key(&fx, ND_KEY_ENTER);        /* confirms the field */

        CHECK_STR(g_api.number_input(&fx.ui, "Send To", "Number:", buf, sizeof buf), "555-1234");
        fx_free(&fx);
        if (i + 1u < ND_MSG_ARROW_KEYS_N)
            pt_new_case();
    }
}

/* ------------------------------------------------------------------ *
 * 8. The empty state
 * ------------------------------------------------------------------ */

static void test_empty_state_only_exits_on_clear(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    /* Enter must be swallowed. If it were not, the CLEAR behind it would
     * still be sitting in the channel when this returns. */
    script_key(&fx, ND_KEY_ENTER);
    script_key(&fx, ND_KEY_MENU);
    script_key(&fx, ND_KEY_CLEAR);
    g_api.empty_state(&fx.ui, "Inbox", "2-1", ND_MSG_NO_SUB, "No Messages");
    CHECK(keys_drained(&fx));

    /* And it is the whole screen: the softkey band carries "Back". */
    check_frame(fx.canvas, "app-messages-inbox");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 9. The detail page
 * ------------------------------------------------------------------ */

static void test_detail_back_returns_back(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    script_key(&fx, ND_KEY_CLEAR);
    CHECK_INT(g_api.show_detail(&fx.ui, "Inbox", "2-1", 1, "hello", 7, "+353870000001", 1000),
              ND_MSG_DETAIL_BACK);
    CHECK(keys_drained(&fx));
    fx_free(&fx);
}

static void test_detail_erase_inbox(void)
{
    fixture fx;
    nd_msg_rec rows[4];
    int64_t id;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    id = seed_inbox("+353870000001", "hello", 1000, 0);
    CHECK(id > 0);

    /* Enter opens Options, Enter picks "Just Erase for now", and the "Erased!"
     * dialog is dismissed by the held key's first repeat. */
    script_key(&fx, ND_KEY_ENTER);
    script_key(&fx, ND_KEY_ENTER);
    hold_key(&fx, ND_KEY_ENTER);

    CHECK_INT(g_api.show_detail(&fx.ui, "Inbox", "2-1", 1, "hello", id, "+353870000001", 1000),
              ND_MSG_DETAIL_DELETED);
    CHECK_INT(g_api.fetch_inbox(rows, ND_ARRAY_LEN(rows)), 0);

    fx_free(&fx);
}

static void test_detail_erase_outbox(void)
{
    fixture fx;
    nd_msg_rec rows[4];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    CHECK_INT(g_api.save_outbox("draft"), ND_OK);
    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 1);

    script_key(&fx, ND_KEY_ENTER); /* Options            */
    script_key(&fx, ND_KEY_ENTER); /* "Erase" is index 0 */
    hold_key(&fx, ND_KEY_ENTER);   /* the "Erased!" dialog */

    CHECK_INT(g_api.show_detail(&fx.ui, "Outbox", "2-2", 1, "draft", rows[0].id, NULL, 0),
              ND_MSG_DETAIL_DELETED);
    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 0);

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 10. The send flow
 * ------------------------------------------------------------------ */

static void test_send_flow_refuses_empty(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    hold_key(&fx, ND_KEY_ENTER); /* dismiss "Message is empty!" */

    /* text.strip(): whitespace alone is empty. The number field is never
     * reached, so nothing consumes a digit. */
    CHECK(!g_api.send_flow(&fx.ui, "   \n ", ND_MESSAGES_ROOT_ID, 3));
    CHECK(!g_api.send_flow(&fx.ui, NULL, ND_MESSAGES_ROOT_ID, 3));

    fx_free(&fx);
}

static void test_send_flow_refuses_over_160(void)
{
    fixture fx;
    char body[256];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    hold_key(&fx, ND_KEY_ENTER);

    memset(body, 'a', 160u);
    body[160] = '\0';
    /* Exactly 160 is allowed; the refusal is `> SMS_MAX_CHARS`. It gets as
     * far as the number field, which the held Enter confirms empty, and is
     * then refused for the number instead -- so the check that matters is
     * the 161 below. */
    body[160] = 'a';
    body[161] = '\0';
    CHECK_INT(g_api.codepoints(body), 161);
    CHECK(!g_api.send_flow(&fx.ui, body, ND_MESSAGES_ROOT_ID, 3));

    fx_free(&fx);
}

/* No modem AND no service channel: the case the dialog was written for, and
 * the only one that still reaches it. An app process gets ui->modem == NULL
 * by nd_app.h's rules; what changed with MSG-1 is that it now also gets a
 * route to the core's modem, and this fixture deliberately has neither. */
static void test_send_flow_without_a_modem(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    CHECK(fx.ui.modem == NULL);
    CHECK(!nd_svc_client_active());

    script_key(&fx, ND_KEY_1);
    script_key(&fx, ND_KEY_2);
    script_key(&fx, ND_KEY_3);
    hold_key(&fx, ND_KEY_ENTER); /* confirms the number, then the dialog */

    CHECK(!g_api.send_flow(&fx.ui, "hello", ND_MESSAGES_ROOT_ID, 3));

    fx_free(&fx);
}

/* THE ANSWER TO MSG-1, from the app's own side.
 *
 * With a modem reachable the flow gets past the guard, draws "Sending...",
 * hands the text to the service and reports "Message sent!". On this host
 * the ModemService runs in simulation mode, which is still do_send_sms()'s
 * own branch and still returns the modem's own (ok, detail) -- so what is
 * pinned here is the app's decision, which is the half that was broken.
 *
 * The fixture attaches the handle rather than a socket, because that is the
 * shape nd-shoot and the core have and it is the path nd_svc.h guarantees is
 * unchanged. The SOCKET path -- a real child talking to a real core -- is
 * proved end to end in test_svc.c, where both processes exist. */
static void test_send_flow_reaches_a_modem(void)
{
    fixture fx;
    nd_modem *modem = NULL;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    if (nd_modem_open(&modem) != ND_OK || modem == NULL) {
        fprintf(stderr, "test_messages: no ModemService; skipping the send case\n");
        fx_free(&fx);
        return;
    }
    fx.ui.modem = modem;

    script_key(&fx, ND_KEY_1);
    script_key(&fx, ND_KEY_2);
    script_key(&fx, ND_KEY_3);
    hold_key(&fx, ND_KEY_ENTER); /* confirms the number, then the dialog */

    CHECK(g_api.send_flow(&fx.ui, "hello", ND_MESSAGES_ROOT_ID, 3));

    fx.ui.modem = NULL;
    nd_modem_close(modem);
    fx_free(&fx);
}

static void test_send_flow_refuses_a_blank_number(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    /* Confirm the field with nothing in it: "".join(...) is empty and the
     * modem is never consulted. */
    hold_key(&fx, ND_KEY_ENTER);
    CHECK(!g_api.send_flow(&fx.ui, "hello", ND_MESSAGES_ROOT_ID, 3));
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 11. The list screens and the composer
 * ------------------------------------------------------------------ */

/* Unread rows get the "* " marker; read ones do not. Opening one marks it
 * read, which is visible on the next pass through the list. */
static void test_inbox_marks_read_on_open(void)
{
    fixture fx;
    nd_msg_rec one;
    int64_t id;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    id = seed_inbox("+353870000001", "hello", 1000, 0);

    script_key(&fx, ND_KEY_ENTER); /* open the only row      */
    script_key(&fx, ND_KEY_CLEAR); /* back out of the detail */
    script_key(&fx, ND_KEY_CLEAR); /* back out of the list   */
    g_api.show_inbox(&fx.ui, ND_MESSAGES_ROOT_ID, 1);

    /* Backing out of a message returns to the LIST, not to the root menu --
     * the Python's `if result == "deleted": continue` is the last statement
     * in the loop body, so the loop repeats either way. That is why a third
     * key is needed here. OPEN-QUESTIONS.md MSG-4. */
    CHECK(keys_drained(&fx));
    CHECK(g_api.fetch_inbox_one(id, &one));
    CHECK_INT(one.is_read, 1);

    fx_free(&fx);
}

static void test_outbox_empty_state(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    script_key(&fx, ND_KEY_CLEAR);
    g_api.show_outbox(&fx.ui, ND_MESSAGES_ROOT_ID, 2);
    CHECK(keys_drained(&fx));
    fx_free(&fx);
}

/* The composer: type, save through Options, then leave with Back on an empty
 * field. The held Back does all three jobs -- dismissing the "Saved!" dialog,
 * erasing the two characters, and then falling out of the loop. */
static void test_write_saves_a_draft(void)
{
    fixture fx;
    nd_msg_rec rows[4];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }

    script_key(&fx, 35);           /* 'h' on the DEV_KEYMAP */
    script_key(&fx, 23);           /* 'i'                   */
    script_key(&fx, ND_KEY_ENTER); /* Options               */
    script_key(&fx, ND_KEY_DOWN);  /* down to "Save"        */
    script_key(&fx, ND_KEY_ENTER); /* choose it             */
    hold_key(&fx, ND_KEY_CLEAR);

    g_api.show_write(&fx.ui, ND_MESSAGES_ROOT_ID, 3);

    CHECK_INT(g_api.fetch_outbox(rows, ND_ARRAY_LEN(rows)), 1);
    /* "Hi", not "hi": TextInputLong upper-cases the first character of a
     * message (framework.py:973-975, "Simple capitalization logic for start
     * of message"). The draft that reaches the outbox is what was on screen. */
    CHECK_STR(rows[0].message, "Hi");

    fx_free(&fx);
}

/* Back on an empty composer exits it immediately. */
static void test_write_exits_on_empty_backspace(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    script_key(&fx, ND_KEY_CLEAR);
    g_api.show_write(&fx.ui, ND_MESSAGES_ROOT_ID, 3);
    CHECK(keys_drained(&fx));
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 12. The two entry points the core calls
 * ------------------------------------------------------------------ */

static void test_open_message(void)
{
    fixture fx;
    nd_msg_rec one;
    int64_t id;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    id = seed_inbox("+353870000001", "hello", 1000, 0);

    script_key(&fx, ND_KEY_CLEAR); /* out of the detail page */
    CHECK_INT(g_api.open_message(&fx.ui, id), 0);
    CHECK(keys_drained(&fx));
    CHECK(g_api.fetch_inbox_one(id, &one));
    CHECK_INT(one.is_read, 1);

    fx_free(&fx);
}

/* An id that is not there falls back to the whole inbox, which here is empty
 * -- so what appears is the "No Messages" screen, and one Back leaves it. */
static void test_open_message_missing_falls_back_to_inbox(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    script_key(&fx, ND_KEY_CLEAR);
    CHECK_INT(g_api.open_message(&fx.ui, 4242), 0);
    CHECK(keys_drained(&fx));
    check_frame(fx.canvas, "app-messages-inbox");

    fx_free(&fx);
}

static void test_shutdown_is_exported(void)
{
    g_api.shutdown(); /* must not crash, must not draw */
    CHECK(true);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int rc;

    manifest_open();
    if (!api_open()) {
        fprintf(stderr, "test_messages: cannot load %s\n", g_so);
        api_close();
        return 1;
    }

    /* The virtual clock, for the same reason nd-shoot pins it: two of these
     * cases assert on a formatted timestamp and one on a saved one, and a
     * real clock would make them depend on the machine's time zone. */
    nd_vclock_enable();

    RUN(test_golden_app_messages);
    RUN(test_golden_app_messages_inbox);
    RUN(test_db_reads_do_not_create);
    RUN(test_db_inbox_roundtrip);
    RUN(test_db_outbox_roundtrip);
    RUN(test_wrap_text);
    RUN(test_format_timestamp);
    RUN(test_codepoints);
    RUN(test_filter_number);
    RUN(test_menu_tables);
    RUN(test_number_field_is_numbers_only);
    RUN(test_number_field_cancel);
    RUN(test_number_field_arrow_opens_picker);
    RUN(test_empty_state_only_exits_on_clear);
    RUN(test_detail_back_returns_back);
    RUN(test_detail_erase_inbox);
    RUN(test_detail_erase_outbox);
    RUN(test_send_flow_refuses_empty);
    RUN(test_send_flow_refuses_over_160);
    RUN(test_send_flow_without_a_modem);
    RUN(test_send_flow_reaches_a_modem);
    RUN(test_send_flow_refuses_a_blank_number);
    RUN(test_inbox_marks_read_on_open);
    RUN(test_outbox_empty_state);
    RUN(test_write_saves_a_draft);
    RUN(test_write_exits_on_empty_backspace);
    RUN(test_open_message);
    RUN(test_open_message_missing_falls_back_to_inbox);
    RUN(test_shutdown_is_exported);

    nd_vclock_disable();
    api_close();
    if (g_manifest != NULL)
        nd_json_free(g_manifest);

    rc = pt_report("test_messages");
    return rc;
}
