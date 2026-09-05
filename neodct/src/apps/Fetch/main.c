/* apps/Fetch/main.c -- app id 9009, the screens.
 *
 * fetch_app.h is the specification and the reasoning; ftp.c is the transport
 * and route.c is the sorting. This file is four screens and the order they
 * come in:
 *
 *     Password   -> a one-line field, asked on every launch
 *     Folder     -> a VerticalList of what is up there, Enter descends
 *     Confirm    -> "Download this? <size>, to <folder>"
 *     Progress   -> a bar, then a notice saying where it went
 *
 * ============ WHY THE PASSWORD IS ASKED EVERY TIME ============
 *
 * There is nowhere good to keep it. nd_settings is a plaintext file on the
 * user partition that the shell app can read, the card is removable, and the
 * phone has no keystore. Asking each launch costs a few seconds of multi-tap
 * and means a phone somebody finds in a pub reaches nothing at all. The host
 * and the user name ARE remembered -- they are not secrets, and typing an IP
 * address on a keypad twice would be the thing that stopped this being used.
 *
 * ============ THE LIST IS THE WHOLE NAVIGATION ============
 *
 * There is no left or right on this keypad, so there is no "up one level"
 * key to bind. Clear is Back everywhere in this OS, so Clear leaves a folder
 * for its parent and leaves the root for the menu -- which is what a person
 * already expects from Clear, and needs no instruction on screen.
 *
 * Directories are drawn with a trailing '/' and sort above files, so the
 * shape of the folder is readable in the first two rows on a screen that
 * shows three.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fetch_app.h"
#include "nd_app.h"
#include "nd_draw.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_storage.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* A label is a name plus the decoration the list adds: "/" for a folder or
 * "  4.0 MB" for a file. */
#define FETCH_LABEL_MAX (ND_FETCH_NAME_MAX + 16u)

/* The remote path the list is showing, as "music" or "roms/psx". Bounded by
 * what fetch_build_url can carry alongside the host and a file name. */
#define FETCH_DIR_MAX 256

/* Everything one browse session needs, on the heap: the two arrays together
 * are about fourteen kilobytes and this phone has 53 MB with the browser
 * possibly in it. */
typedef struct {
    fetch_entry entries[ND_FETCH_ENTRIES];
    char labels[ND_FETCH_ENTRIES][FETCH_LABEL_MAX];
    const char *items[ND_FETCH_ENTRIES];
    size_t n;
} fetch_view;

/* The download progress screen, and the context its callback needs. */
typedef struct {
    nd_ui *ui;
    nd_progress bar;
} fetch_dl_ctx;

/* ------------------------------------------------------------------ *
 * Small screens
 * ------------------------------------------------------------------ */

/* The same centred one-liner Settings uses while bluetoothctl runs. Drawn
 * before a blocking call so the phone does not look asleep for the twenty
 * seconds a connect timeout can take. */
static void say_working(nd_ui *ui, const char *what)
{
    nd_softkey bar;
    int32_t w = 0;
    int32_t h = 0;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_content_bottom(ui)),
                            ND_BLACK);
    nd_text_size(ui->font_n, what, &w, &h);
    (void)nd_draw_text(ui->draw, (nd_ui_width(ui) - w) / 2, (nd_ui_content_bottom(ui) - h) / 2,
                       what, ui->font_n, ND_WHITE);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "", true);
    (void)nd_ui_present(ui);
}

static void say_notice(nd_ui *ui, const char *message)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, ui, message);
    nd_msgdialog_set_title(&dlg, "Fetch");
    nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);
}

/* ------------------------------------------------------------------ *
 * The password
 * ------------------------------------------------------------------ */

/* Returns false when the owner pressed Clear rather than typing one.
 *
 * A space is not offered by the field's filter and would break the netrc
 * format anyway (see write_netrc); a password with one in it cannot be typed
 * here, which is a limit worth knowing about when choosing one on the
 * server. */
static bool ask_password(nd_ui *ui, fetch_conn *c)
{
    nd_textinput in;
    const char *typed;

    c->pass[0] = '\0';
    if (nd_textinput_init(&in, ui, "Fetch", "Password", c->pass, sizeof c->pass, "",
                          ND_T9_FILTER_ANY) != ND_OK)
        return false;
    typed = nd_textinput_show(&in);
    return typed != NULL && typed[0] != '\0';
}

/* ------------------------------------------------------------------ *
 * The folder list
 * ------------------------------------------------------------------ */

static void build_labels(fetch_view *v)
{
    size_t i;

    for (i = 0u; i < v->n; i++) {
        if (v->entries[i].is_dir) {
            (void)nd_snprintf(v->labels[i], FETCH_LABEL_MAX, "%s/", v->entries[i].name);
        } else {
            char size[16];

            fetch_format_size(v->entries[i].size, size, sizeof size);
            /* Name first: on a 240 px row the name is what is being chosen
             * and the size is what is being glanced at, so a truncated line
             * should lose the size. */
            if (nd_snprintf(v->labels[i], FETCH_LABEL_MAX, "%s  %s", v->entries[i].name, size) !=
                ND_OK)
                (void)nd_strlcpy(v->labels[i], v->entries[i].name, FETCH_LABEL_MAX);
        }
        v->items[i] = v->labels[i];
    }
}

/* The title over the list: the folder's own name, or the host at the root --
 * so the first screen says which server this is without a settings trip. */
static void list_title(const fetch_conn *c, const char *dir, char *out, size_t out_sz)
{
    const char *slash;

    if (dir[0] == '\0') {
        (void)nd_strlcpy(out, c->host, out_sz);
        return;
    }
    slash = strrchr(dir, '/');
    (void)nd_strlcpy(out, (slash != NULL) ? slash + 1 : dir, out_sz);
}

/* ------------------------------------------------------------------ *
 * Downloading one file
 * ------------------------------------------------------------------ */

/* "1.2 of 4.0 MB", beside the percentage. */
static void progress_detail(void *ctx, int64_t done, int64_t total, char *out, size_t out_sz)
{
    char a[16];
    char b[16];

    (void)ctx;
    fetch_format_size((done < 0) ? 0 : done, a, sizeof a);
    if (total <= 0) {
        (void)nd_strlcpy(out, a, out_sz);
        return;
    }
    fetch_format_size(total, b, sizeof b);
    (void)nd_snprintf(out, out_sz, "%s of %s", a, b);
}

static bool on_progress(void *ctx, int64_t done, int64_t total)
{
    fetch_dl_ctx *d = (fetch_dl_ctx *)ctx;

    /* An incoming call has to be able to take the screen. Returning false
     * kills curl; the .part file is left for the next attempt. */
    if (nd_app_should_exit())
        return false;
    if (nd_progress_draw(&d->bar, (done < 0) ? 0 : done, (total > 0) ? total : 0))
        (void)nd_ui_present(d->ui);
    return true;
}

/* The one-word name of where a file went, for the notice afterwards. */
static const char *kind_name(fetch_dest_kind k)
{
    switch (k) {
    case FETCH_DEST_MUSIC:
        return "Music";
    case FETCH_DEST_GAME:
        return "the PSX app";
    case FETCH_DEST_BIOS:
        return "the PSX app";
    case FETCH_DEST_NAP:
        return "the card, ready to install";
    case FETCH_DEST_OTHER:
    default:
        return "Downloads";
    }
}

static void download_one(nd_ui *ui, const fetch_conn *c, const char *dir, const fetch_entry *e)
{
    char mount[ND_PATH_MAX];
    char dest[ND_PATH_MAX];
    char psx[ND_PATH_MAX];
    char why[ND_FETCH_WHY_MAX];
    char message[256];
    char size_text[16];
    fetch_dest_kind kind = FETCH_DEST_OTHER;
    fetch_dl_ctx dl;
    nd_msgdialog dlg;
    bool psx_installed;
    nd_err rc;

    /* nd_storage_folder() is the mount point plus a name and is gated on the
     * card being ready, so asking it for "." is asking for the root of a
     * ready card and nothing at all otherwise. Same trick nd_nap_find uses. */
    if (!nd_storage_is_ready() || !nd_storage_folder(".", mount, sizeof mount)) {
        say_notice(ui, "The memory card went away.");
        return;
    }
    mount[strlen(mount) - 2u] = '\0'; /* drop the "/." */

    if (nd_snprintf(psx, sizeof psx, "%s/apps/PSX", mount) != ND_OK)
        return;
    psx_installed = nd_path_is_dir(psx);

    if (fetch_dest_path(mount, e->name, psx_installed, dest, sizeof dest, &kind) != ND_OK) {
        say_notice(ui, "That name cannot be saved on the card.");
        return;
    }

    fetch_format_size(e->size, size_text, sizeof size_text);
    if (nd_snprintf(message, sizeof message, "%s\n%s, to %s.", e->name, size_text,
                    kind_name(kind)) != ND_OK)
        return;
    nd_msgdialog_init(&dlg, ui, message);
    nd_msgdialog_set_title(&dlg, "Download?");
    nd_msgdialog_set_button(&dlg, "Download");
    if (nd_msgdialog_show(&dlg) != ND_KEY_ENTER)
        return;

    dl.ui = ui;
    nd_progress_init(&dl.bar, ui, e->name, "Downloading", NULL, progress_detail, NULL);
    (void)nd_progress_draw(&dl.bar, 0, (e->size > 0) ? e->size : 0);
    (void)nd_ui_present(ui);

    why[0] = '\0';
    rc = fetch_download(c, dir, e->name, dest, e->size, on_progress, &dl, why, sizeof why);
    if (rc != ND_OK) {
        say_notice(ui, (why[0] != '\0') ? why : "The download failed.");
        return;
    }

    /* A raw .bin with no cue beside it is a disc PCSX-ReARMed has to guess
     * the layout of. Writing one is nine lines and it is the difference
     * between a game that boots and a game that does not; a failure here is
     * not a failed download, so it is a note rather than an error. */
    if (kind == FETCH_DEST_GAME && fetch_classify(e->name) == FETCH_DEST_GAME &&
        strstr(e->name, ".bin") != NULL) {
        if (fetch_write_cue(dest) == ND_ERR_UNSUPPORTED)
            nd_log(ND_LOG_FETCH, "%s: no cue written", e->name);
    }

    (void)nd_snprintf(message, sizeof message, "Saved %s\nto %s.", e->name, kind_name(kind));
    say_notice(ui, message);
}

/* ------------------------------------------------------------------ *
 * The browse loop
 * ------------------------------------------------------------------ */

/* Descend into `name`, or ascend when it is NULL. Returns false when there is
 * nowhere further up -- which is how the loop knows to leave the app. */
static bool dir_push(char *dir, size_t dir_sz, const char *name)
{
    if (name == NULL) {
        char *slash = strrchr(dir, '/');

        if (dir[0] == '\0')
            return false;
        if (slash != NULL)
            *slash = '\0';
        else
            dir[0] = '\0';
        return true;
    }
    if (dir[0] == '\0')
        return nd_strlcpy(dir, name, dir_sz) < dir_sz;
    {
        char joined[FETCH_DIR_MAX];

        if (nd_snprintf(joined, sizeof joined, "%s/%s", dir, name) != ND_OK)
            return false;
        return nd_strlcpy(dir, joined, dir_sz) < dir_sz;
    }
}

static void browse(nd_ui *ui, const fetch_conn *c, fetch_view *v)
{
    char dir[FETCH_DIR_MAX] = "";
    char why[ND_FETCH_WHY_MAX];
    char title[ND_FETCH_NAME_MAX];
    bool reload = true;

    while (!nd_app_should_exit()) {
        nd_vlist list;
        int32_t chosen;

        if (reload) {
            say_working(ui, "Connecting...");
            why[0] = '\0';
            v->n = 0u;
            if (fetch_list(c, dir, v->entries, ND_ARRAY_LEN(v->entries), &v->n, why, sizeof why) !=
                ND_OK) {
                say_notice(ui, (why[0] != '\0') ? why : "Could not reach the server.");
                /* A folder that will not list is not a folder to stay in. At
                 * the root there is nowhere to go but out. */
                if (!dir_push(dir, sizeof dir, NULL))
                    return;
                continue;
            }
            build_labels(v);
            reload = false;
        }

        list_title(c, dir, title, sizeof title);
        if (v->n == 0u) {
            (void)nd_infoscreen_show(ui, "Empty folder", NULL, "Back");
            if (!dir_push(dir, sizeof dir, NULL))
                return;
            reload = true;
            continue;
        }

        nd_vlist_init(&list, ui, title, v->items, v->n, ND_FETCH_APP_ID);
        chosen = nd_vlist_show(&list);
        if (chosen == ND_WIDGET_BACK) {
            if (!dir_push(dir, sizeof dir, NULL))
                return;
            reload = true;
            continue;
        }
        if (chosen < 0 || (size_t)chosen >= v->n)
            continue;

        if (v->entries[chosen].is_dir) {
            if (!dir_push(dir, sizeof dir, v->entries[chosen].name))
                say_notice(ui, "That folder is nested too deeply.");
            else
                reload = true;
            continue;
        }
        download_one(ui, c, dir, &v->entries[chosen]);
        /* The folder is listed again after a download: the server may have
         * grown a .cue beside the .bin somebody just uploaded, and coming
         * back to a stale list would hide it. */
        reload = true;
    }
}

/* ------------------------------------------------------------------ *
 * The app
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    fetch_conn conn;
    fetch_view *view;

    if (ui == NULL)
        return 1;

    memset(&conn, 0, sizeof conn);
    (void)nd_settings_get_copy(ND_FETCH_KEY_HOST, ND_FETCH_HOST_DEFAULT, conn.host,
                               sizeof conn.host);
    (void)nd_settings_get_copy(ND_FETCH_KEY_USER, ND_FETCH_USER_DEFAULT, conn.user,
                               sizeof conn.user);

    /* Nothing can be saved without a card, and finding that out after a
     * password and a download would be the wrong time to learn it. */
    if (!nd_storage_is_ready()) {
        say_notice(ui, "No memory card.\nFetch saves onto the card.");
        return 0;
    }

    if (!ask_password(ui, &conn))
        return 0;

    view = calloc(1u, sizeof *view);
    if (view == NULL) {
        say_notice(ui, "Not enough memory.");
        return 0;
    }
    browse(ui, &conn, view);

    /* The password is on this stack and nowhere else. Wiping it is cheap and
     * means a crash dump taken afterwards does not carry it. */
    memset(&conn, 0, sizeof conn);
    free(view);
    return 0;
}

void app_shutdown(void)
{
    /* Nothing to release: curl is spawned into this app's process group with
     * new_session false, so it goes when this process does, and the netrc it
     * was given is unlinked by fetch_download/fetch_list before either
     * returns. The password lives on app_run's stack. */
}
