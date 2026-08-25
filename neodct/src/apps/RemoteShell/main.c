/* apps/RemoteShell/main.c -- the switch and the address book, app id 9990.
 *
 * A one-to-one port of System/engineering/apps/RemoteShell/main.py (165
 * lines). Its docstring is the specification:
 *
 *     Remote Shell: turn ssh and sftp to this phone on, and say who to dial.
 *
 *     The work is in System/core/RemoteShell, which explains the shape of the
 *     thing and holds every decision about who can get in. This is the switch
 *     and the address book: a list you can operate with a keypad and one
 *     thumb.
 *
 *     Deliberately plain. The phone is either reachable or it is not, and the
 *     screen should say which without being read carefully.
 *
 * "The work is in System/core/RemoteShell" is the whole of the porting
 * problem here, and rshell.h is where it is written down: that module has no
 * C implementation anywhere in neodct/src, core/nd_main.c still logs
 * "[RSHELL] remote shell unavailable: not linked in this build", and this work
 * package may not add files to lib/ or core/. So it is ported into this app's
 * own directory, under the names spec-storage-settings.md already chose, and
 * the one thing that does not work as a result is named in rshell.h and again
 * at _turn_on() below.
 *
 * ============ THE MENU IS REBUILT EVERY ITERATION, AND MUST BE ============
 *
 * Power's loop rebuilds its VerticalList each pass and the visible effect is
 * only that the cursor goes home. Here it is load-bearing for a different
 * reason: line 0 is "Status: On|Dialling|Off" and line 1 is "Turn off"/"Turn
 * on", so the list IS the status display. Coming back from turning it on has
 * to show it on. Constructing the list once, PhoneBook-style, would leave a
 * screen that says "Off" about a phone the internet can reach.
 *
 * ============ THE ODD BIT: SELECTING LINE 0 DOES NOTHING ============
 *
 * STATUS is a menu entry you can move onto and press Enter on, and nothing
 * happens -- the Python's if/elif chain has no branch for it, and none for any
 * index past FINGERPRINT either. Ported as-is.
 */

#include <stdio.h>
#include <string.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_storage.h"
#include "nd_t9.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "remote_app.h"
#include "rshell.h"

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_rsapp_title = "Remote";

const char *const nd_rsapp_no_card = "No card in the phone.";
const char *const nd_rsapp_ask_turn_on = "Let this phone be reached over the internet?";
const char *const nd_rsapp_turn_on_button = "Turn on";
const char *const nd_rsapp_now_on =
    "Remote Shell is on.\n\nIt stays on across restarts until you turn it off here.";
const char *const nd_rsapp_now_off = "Remote Shell is off.";

/* KEY_ENTER = 28, spelled in the Python because it predates a shared table. */
#define RSAPP_KEY_ENTER ND_KEY_ENTER

/* ------------------------------------------------------------------ *
 * The two dialogs
 * ------------------------------------------------------------------ */

void nd_rsapp_tell(nd_ui *ui, const char *message)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, message);
    nd_msgdialog_set_title(&dialog, nd_rsapp_title);
    nd_msgdialog_set_button(&dialog, "OK");
    (void)nd_msgdialog_show(&dialog);
}

bool nd_rsapp_confirm(nd_ui *ui, const char *question, const char *button)
{
    nd_msgdialog dialog;

    nd_msgdialog_init(&dialog, ui, question);
    nd_msgdialog_set_title(&dialog, nd_rsapp_title);
    nd_msgdialog_set_button(&dialog, button);
    return nd_msgdialog_show(&dialog) == RSAPP_KEY_ENTER;
}

/* ------------------------------------------------------------------ *
 * _menu_lines()
 * ------------------------------------------------------------------ */

const char *nd_rsapp_running_word(bool sshd, bool tunnel)
{
    if (sshd && tunnel)
        return "On";
    if (sshd || tunnel)
        return "Dialling";
    return "Off";
}

void nd_rsapp_menu_lines(char lines[][ND_RSAPP_LINE_MAX], size_t n)
{
    nd_rs_status state;
    nd_rs_settings current;
    bool up;

    if (lines == NULL || n < (size_t)ND_RSAPP_MENU_ITEMS)
        return;

    nd_rs_status_get(&state);
    nd_rs_settings_get(&current);
    up = state.sshd || state.tunnel;

    (void)snprintf(lines[ND_RSAPP_STATUS], ND_RSAPP_LINE_MAX, "Status: %s",
                   nd_rsapp_running_word(state.sshd, state.tunnel));
    (void)nd_strlcpy(lines[ND_RSAPP_TOGGLE], up ? "Turn off" : "Turn on", ND_RSAPP_LINE_MAX);
    /* `current["host"] or "not set"` -- an unset relay is the common state on
     * a fresh phone and reads better than an empty line. */
    (void)snprintf(lines[ND_RSAPP_RELAY], ND_RSAPP_LINE_MAX, "Relay: %s",
                   (current.host[0] != '\0') ? current.host : "not set");
    (void)snprintf(lines[ND_RSAPP_LOGIN], ND_RSAPP_LINE_MAX, "Login: %s", current.user);
    (void)snprintf(lines[ND_RSAPP_PORT], ND_RSAPP_LINE_MAX, "Port: %s", current.port);
    (void)nd_strlcpy(lines[ND_RSAPP_KEYS], "Copy keys from card", ND_RSAPP_LINE_MAX);
    (void)nd_strlcpy(lines[ND_RSAPP_FINGERPRINT], "This phone's key", ND_RSAPP_LINE_MAX);
}

/* ------------------------------------------------------------------ *
 * The three text fields
 * ------------------------------------------------------------------ */

/* The three differ only in prompt, which setting they write and which input
 * filter they use, so one helper is the whole of _set_relay/_set_login/
 * _set_port. `field` picks which of save_settings' three arguments is filled;
 * the other two stay NULL, which is Python's None -- leave that one alone. */
static void edit_setting(nd_ui *ui, const char *prompt, const char *initial, nd_t9_filter filter,
                         int field)
{
    char buf[ND_TEXTINPUT_CAP];
    nd_textinput entry;
    const char *value;

    if (nd_textinput_init(&entry, ui, nd_rsapp_title, prompt, buf, sizeof buf, initial, filter) !=
        ND_OK)
        return;
    value = nd_textinput_show(&entry);
    /* `if value is None: return` -- cancel leaves the setting untouched. An
     * EMPTY confirmed value is saved, and for user and port save_settings
     * turns that back into the default. */
    if (value == NULL)
        return;

    (void)nd_rs_settings_save(field == ND_RSAPP_RELAY ? value : NULL,
                              field == ND_RSAPP_LOGIN ? value : NULL,
                              field == ND_RSAPP_PORT ? value : NULL, NULL);
}

/* "The relay's address. IPv6 is the likely answer -- mobile data here is IPv6,
 * so the relay has to be reachable over it." No input filter, so the
 * punctuation cycle is available for the colons. */
static void set_relay(nd_ui *ui)
{
    nd_rs_settings current;

    nd_rs_settings_get(&current);
    edit_setting(ui, "Relay host:", current.host, ND_T9_FILTER_ANY, ND_RSAPP_RELAY);
}

static void set_login(nd_ui *ui)
{
    nd_rs_settings current;

    nd_rs_settings_get(&current);
    edit_setting(ui, "Login:", current.user, ND_T9_FILTER_LETTERS, ND_RSAPP_LOGIN);
}

static void set_port(nd_ui *ui)
{
    nd_rs_settings current;

    nd_rs_settings_get(&current);
    edit_setting(ui, "Relay port:", current.port, ND_T9_FILTER_NUMBERS, ND_RSAPP_PORT);
}

/* ------------------------------------------------------------------ *
 * The three actions that talk to the module
 * ------------------------------------------------------------------ */

void nd_rsapp_copied_message(char *out, size_t n, const char *taken)
{
    if (out == NULL || n == 0u)
        return;
    (void)snprintf(out, n,
                   "Copied: %s.\n\nDelete them from the card now -- anyone who "
                   "takes the card out can read them.",
                   (taken != NULL) ? taken : "");
}

/* "Take the operator's keys off the card." */
static void copy_keys(nd_ui *ui)
{
    char taken[ND_RS_TAKEN_MAX];
    char why[ND_RS_ERRMSG_MAX];
    char message[ND_RSAPP_MSG_MAX];

    /* Storage.MOUNT_POINT, the constant, NOT Storage.folder() -- this does not
     * care whether the card has the five NeoDCT folders on it. */
    if (!nd_path_is_dir(ND_SD_MOUNT_POINT)) {
        nd_rsapp_tell(ui, nd_rsapp_no_card);
        return;
    }
    if (nd_rs_install_keys_from_card(ND_SD_MOUNT_POINT, taken, sizeof taken, why, sizeof why) !=
        ND_OK) {
        /* `except RemoteShell.RemoteShellError as exc: _tell(ui, str(exc))` */
        nd_rsapp_tell(ui, why);
        return;
    }
    nd_rsapp_copied_message(message, sizeof message, taken);
    nd_rsapp_tell(ui, message);
}

void nd_rsapp_fingerprint_message(char *out, size_t n, const char *fingerprint)
{
    if (out == NULL || n == 0u)
        return;
    /* `RemoteShell.host_fingerprint() or "unknown"` */
    (void)snprintf(out, n, "This phone:\n%s",
                   (fingerprint != NULL && fingerprint[0] != '\0') ? fingerprint : "unknown");
}

/* "So the first connection can be checked against something." */
static void show_fingerprint(nd_ui *ui)
{
    char fingerprint[ND_RS_ERRMSG_MAX];
    char why[ND_RS_ERRMSG_MAX];
    char message[ND_RSAPP_MSG_MAX];

    if (nd_rs_ensure_host_key(why, sizeof why) != ND_OK) {
        nd_rsapp_tell(ui, why);
        return;
    }
    (void)nd_rs_host_fingerprint(fingerprint, sizeof fingerprint);
    nd_rsapp_fingerprint_message(message, sizeof message, fingerprint);
    nd_rsapp_tell(ui, message);
}

static void turn_on(nd_ui *ui)
{
    nd_rs_status state;
    char why[ND_RS_ERRMSG_MAX];

    if (nd_rs_start(&state, why, sizeof why) != ND_OK) {
        nd_rsapp_tell(ui, why);
        return;
    }
    /* The second sentence of this message is NOT TRUE IN THIS BUILD. It is
     * true of the Python, whose launcher calls RemoteShell.start_if_enabled()
     * at boot; nd_main.c wants to do the same but resolves the symbol weakly
     * out of libneodct, and nd_rs_start_if_enabled() currently lives in this
     * app's .so. The string is the Python's and stays; see rshell.h for what
     * has to move to make it honest. */
    nd_rsapp_tell(ui, nd_rsapp_now_on);
}

static void turn_off(nd_ui *ui)
{
    nd_rs_status state;

    (void)nd_rs_stop(&state, true);
    nd_rsapp_tell(ui, nd_rsapp_now_off);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL)
        return 1;

    for (;;) {
        char lines[ND_RSAPP_MENU_ITEMS][ND_RSAPP_LINE_MAX];
        const char *items[ND_RSAPP_MENU_ITEMS];
        nd_vlist menu;
        nd_softkey bar;
        int32_t choice;
        size_t i;

        /* Rebuilt every pass: the list is the status display. */
        nd_rsapp_menu_lines(lines, ND_RSAPP_MENU_ITEMS);
        for (i = 0u; i < (size_t)ND_RSAPP_MENU_ITEMS; i++)
            items[i] = lines[i];

        nd_vlist_init(&menu, ui, nd_rsapp_title, items, ND_RSAPP_MENU_ITEMS, ND_RS_APP_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);

        choice = nd_vlist_show(&menu);

        if (choice < 0)
            return 0;
        if (choice == ND_RSAPP_TOGGLE) {
            nd_rs_status state;

            /* Asked again rather than reused from _menu_lines(): the Python
             * calls status() a second time here, and between the two the
             * relay can have dropped the tunnel. */
            nd_rs_status_get(&state);
            if (state.sshd || state.tunnel)
                turn_off(ui);
            else if (nd_rsapp_confirm(ui, nd_rsapp_ask_turn_on, nd_rsapp_turn_on_button))
                turn_on(ui);
        } else if (choice == ND_RSAPP_RELAY) {
            set_relay(ui);
        } else if (choice == ND_RSAPP_LOGIN) {
            set_login(ui);
        } else if (choice == ND_RSAPP_PORT) {
            set_port(ui);
        } else if (choice == ND_RSAPP_KEYS) {
            copy_keys(ui);
        } else if (choice == ND_RSAPP_FINGERPRINT) {
            show_fingerprint(ui);
        }
        /* No branch for ND_RSAPP_STATUS. See the header comment. */

        /* Not in the Python, which had exceptions to unwind it. nd_app.h: a
         * loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* The children this app starts -- sshd and the tunnel loop -- are the whole
 * point of it and MUST outlive it: the operator turns Remote Shell on and then
 * goes back to using the phone. So there is nothing to release here, and the
 * symbol exists because nd_app.h requires every app to export one.
 *
 * ssh-keygen is the only child this app waits for, and it is already reaped by
 * the time any screen can be left. */
void app_shutdown(void) {}
