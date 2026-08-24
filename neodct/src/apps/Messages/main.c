/* apps/Messages/main.c -- the SMS app, app id 2.
 *
 * A one-to-one port of System/apps/Messages/main.py (475 lines): the root
 * PagedList, the inbox, the outbox, the message detail page, the composer,
 * and the send flow with its numbers-only Send To field. The seven sqlite
 * statements are in msg_db.c; the shared contact picker the Send To field
 * opens lives in libneodct as lib/nd_contacts.c, because nd-core opens it too
 * (see nd_contacts.h).
 *
 * ============ FIVE THINGS THAT LOOK WRONG AND ARE PORTED ANYWAY ============
 *
 * 1. `if result == "deleted": continue` IS A NO-OP. It is the last statement
 *    in _show_inbox's and _show_outbox's `while True`, so the loop repeats
 *    whatever the detail screen returned. Backing out of a message therefore
 *    returns to the list, not to the root menu. spec-apps-core.md section 7
 *    reads it the other way round and is wrong; the Python is the oracle.
 *    OPEN-QUESTIONS.md MSG-4.
 *
 * 2. _send_message_flow() TAKES root_id AND sub_index AND NEVER READS THEM.
 *    Both are kept in the C signature so the two files line up.
 *
 * 3. THE INBOX OPTIONS MENU HAS ONE ENTRY, CALLED "Just Erase for now".
 *    That string is on the phone today. It is not a placeholder to be tidied.
 *
 * 4. NO SOFTKEY IS SET BEFORE THE OPTIONS LIST. _show_message_detail opens a
 *    VerticalList without touching the bar first, so whatever the detail
 *    screen left there -- "Options" -- is what shows above the options menu.
 *    VerticalList's draw clears only rows 0..145, which is what makes that
 *    visible rather than harmless.
 *
 * 5. THE CURSOR DOES NOT BLINK WHILE YOU ARE NOT TYPING. Both key loops
 *    check their 0.5 s timer only after wait_for_key() has returned, and
 *    that blocks. An idle composer never toggles. A C loop with a poll()
 *    timeout would blink, and would then differ from the golden frames.
 *
 * ============ WHERE THE ROOT MENU IS BUILT ============
 *
 * run() constructs the PagedList before its loop, so the selected page
 * survives a trip into a submenu -- come back from Write Message and the
 * root is still on "Write Message". Rebuilding it per iteration would reset
 * it, which is visible on screen. The inbox and outbox lists are the
 * opposite: the Python rebuilds those INSIDE the loop, so the cursor goes
 * back to row 0 after every message. Both are reproduced.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_app.h"
#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_modem.h"
#include "nd_t9.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "messages.h"

/* ------------------------------------------------------------------ *
 * The constant tables
 * ------------------------------------------------------------------ */

/* ARROW_KEYS = (103, 105, 106, 108) -- up, left, right, down. */
const int32_t nd_msg_arrow_keys[ND_MSG_ARROW_KEYS_N] = {ND_KEY_UP, ND_KEY_LEFT, ND_KEY_RIGHT,
                                                        ND_KEY_DOWN};

const char *const nd_msg_menu_items[ND_MSG_MENU_ITEMS] = {"Inbox", "Outbox", "Write Message"};

const char *const nd_msg_inbox_options[ND_MSG_INBOX_OPTIONS_N] = {"Just Erase for now"};
const char *const nd_msg_outbox_options[ND_MSG_OUTBOX_OPTIONS_N] = {"Erase", "Send"};

/* The composer's Options menu. Not in messages.h: nothing outside this file
 * needs it and the Python builds it inline. */
static const char *const WRITE_OPTIONS[2] = {"Send", "Save"};

/* Enough wrapped lines for any message the detail page can actually show --
 * the body loop stops at y > 123, which is four lines of 22 px starting at
 * 44. Sixteen leaves room for a font change and costs 4 KB of stack. */
#define DETAIL_BODY_LINES 16

/* "From: " + sender, "Time: " + a 16-character timestamp. */
#define META_LINE_MAX 96

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

/* Python's `//` floors; C's `/` truncates toward zero. The two differ once
 * (screen_w - w) goes negative, which a long word or a wide message makes
 * happen on the centred screens. */
static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static const char *nz(const char *s)
{
    return (s != NULL) ? s : "";
}

/* True for a byte that starts a UTF-8 code point. */
static bool is_lead(unsigned char c)
{
    return (c & 0xC0u) != 0x80u;
}

size_t nd_msg_codepoints(const char *s)
{
    size_t n = 0u;
    size_t i;

    if (s == NULL)
        return 0u;
    for (i = 0u; s[i] != '\0'; i++) {
        if (is_lead((unsigned char)s[i]))
            n++;
    }
    return n;
}

void nd_msg_filter_number(char *dst, size_t dst_sz, const char *src)
{
    size_t out = 0u;
    size_t i;

    if (dst == NULL || dst_sz == 0u)
        return;
    dst[0] = '\0';
    if (src == NULL)
        return;

    for (i = 0u; src[i] != '\0' && out + 1u < dst_sz; i++) {
        char c = src[i];

        if (strchr("0123456789*#+", c) != NULL && c != '\0')
            dst[out++] = c;
    }
    dst[out] = '\0';
}

/* _format_timestamp(ts). `if not ts` is falsy in Python, so 0 -- and only 0,
 * because a timestamp column is an integer -- gives "Unknown time". */
void nd_msg_format_timestamp(int64_t ts, char *out, size_t out_sz)
{
    struct tm tm_out;

    if (out == NULL || out_sz == 0u)
        return;
    if (ts == 0) {
        (void)nd_strlcpy(out, "Unknown time", out_sz);
        return;
    }
    nd_time_localtime((double)ts, &tm_out);
    if (strftime(out, out_sz, "%Y-%m-%d %H:%M", &tm_out) == 0u)
        (void)nd_strlcpy(out, "Unknown time", out_sz);
}

/* ------------------------------------------------------------------ *
 * _wrap_text -- the app's own wrapper
 * ------------------------------------------------------------------ */

static bool fits(nd_ui *ui, const char *candidate, const nd_font *font, int32_t max_width)
{
    int32_t w = 0;

    nd_ui_text_size(ui, candidate, font, &w, NULL);
    return w <= max_width;
}

/* Drop the last code point of an in-place buffer. Python's `word[:-1]` drops
 * one CHARACTER; dropping one byte would split a multi-byte sequence and
 * measure a string the font cannot render. */
static void drop_last_codepoint(char *s)
{
    size_t len = strlen(s);

    while (len > 0u) {
        len--;
        if (is_lead((unsigned char)s[len]))
            break;
    }
    s[len] = '\0';
}

void nd_msg_wrap_text(nd_lines *out, nd_ui *ui, const char *text, int32_t max_width,
                      const nd_font *font)
{
    char current[ND_TEXT_LINE_MAX];
    char candidate[ND_TEXT_LINE_MAX];
    char word[ND_TEXT_LINE_MAX];
    const char *p;
    bool any_word = false;

    if (out == NULL || ui == NULL)
        return;
    nd_lines_clear(out);

    current[0] = '\0';
    p = nz(text);

    for (;;) {
        size_t wlen = 0u;

        /* str.split() with no argument: any run of whitespace is one
         * separator and no empty token is ever produced. */
        while (*p != '\0' && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' ||
                              *p == '\f'))
            p++;
        if (*p == '\0')
            break;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\v' &&
               *p != '\f') {
            if (wlen + 1u < sizeof word)
                word[wlen++] = *p;
            p++;
        }
        word[wlen] = '\0';
        any_word = true;

        if (current[0] != '\0') {
            (void)nd_snprintf(candidate, sizeof candidate, "%s %s", current, word);
        } else {
            (void)nd_strlcpy(candidate, word, sizeof candidate);
        }

        if (fits(ui, candidate, font, max_width)) {
            (void)nd_strlcpy(current, candidate, sizeof current);
            continue;
        }

        if (current[0] != '\0') {
            (void)nd_lines_push(out, current);
            (void)nd_strlcpy(current, word, sizeof current);
        } else {
            /* One word wider than the whole line. Trim a character at a time
             * until the word PLUS the ellipsis fits, then keep the ellipsis
             * -- unconditionally, which is where this differs from
             * nd_pagedlist_wrap(), and `current` is reset to "" so the next
             * word starts a line of its own. */
            char trimmed[ND_TEXT_LINE_MAX];
            char probe[ND_TEXT_LINE_MAX + 4];

            (void)nd_strlcpy(trimmed, word, sizeof trimmed);
            for (;;) {
                if (trimmed[0] == '\0')
                    break;
                (void)nd_snprintf(probe, sizeof probe, "%s...", trimmed);
                if (fits(ui, probe, font, max_width))
                    break;
                drop_last_codepoint(trimmed);
            }
            if (trimmed[0] != '\0') {
                (void)nd_snprintf(probe, sizeof probe, "%s...", trimmed);
                (void)nd_lines_push(out, probe);
            } else {
                (void)nd_lines_push(out, "...");
            }
            current[0] = '\0';
        }
    }

    /* `if not words: return [""]` -- one EMPTY line, never zero of them. */
    if (!any_word) {
        (void)nd_lines_push(out, "");
        return;
    }
    if (current[0] != '\0')
        (void)nd_lines_push(out, current);
}

/* ------------------------------------------------------------------ *
 * _show_empty_state
 * ------------------------------------------------------------------ */

void nd_msg_show_empty_state(nd_ui *ui, const char *title, const char *root_id, int32_t sub_index,
                             const char *message)
{
    nd_header header;
    nd_softkey softkey;
    int32_t screen_w;
    int32_t screen_h;
    int32_t content_bottom;
    int32_t header_y;
    int32_t w = 0;
    int32_t h = 0;
    int32_t y;

    if (ui == NULL || ui->draw == NULL)
        return;

    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* The FULL screen, rows 0..175 -- not the 0..145 most widgets clear. The
     * softkey below is what puts "Back" back. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);
    (void)nd_draw_text(ui->draw, 5, 5, nz(title), ui->font_xl, ND_WHITE);
    (void)nd_draw_line(ui->draw, 0, header_y, screen_w, header_y, ND_WHITE, 1);

    nd_header_init(&header, ui, nz(root_id));
    nd_header_draw(&header, sub_index);

    nd_ui_text_size(ui, nz(message), ui->font_n, &w, &h);
    y = header_y + nd_max32(0, floordiv((content_bottom - header_y) - h, 2));
    (void)nd_draw_text(ui->draw, floordiv(screen_w - w, 2), y, nz(message), ui->font_n, ND_WHITE);

    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Back", false);
    (void)nd_ui_present(ui);

    /* ONLY key 14 leaves. Enter does nothing here. */
    for (;;) {
        int32_t key = nd_ui_wait_for_key(ui);

        if (key == ND_KEY_CLEAR)
            return;
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * _draw_sending
 * ------------------------------------------------------------------ */

void nd_msg_draw_sending(nd_ui *ui, const char *number)
{
    nd_softkey softkey;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t w = 0;
    int32_t h = 0;
    int32_t w2 = 0;
    int32_t y;

    if (ui == NULL || ui->draw == NULL)
        return;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);
    nd_ui_text_size(ui, "Sending...", ui->font_n, &w, &h);
    y = nd_max32(10, floordiv(content_bottom - h, 2) - 12);
    (void)nd_draw_text(ui->draw, floordiv(screen_w - w, 2), y, "Sending...", ui->font_n, ND_WHITE);

    nd_ui_text_size(ui, nz(number), ui->font_s, &w2, NULL);
    (void)nd_draw_text(ui->draw, floordiv(screen_w - w2, 2), y + h + 8, nz(number), ui->font_s,
                       ND_GRAY);

    /* update("", present=False): the strip is cleared and NOTHING is drawn on
     * it. nd_softkey.h is explicit that "" is not an error. */
    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "", false);
    (void)nd_ui_present(ui);
}

/* ------------------------------------------------------------------ *
 * ContactNumberInput
 * ------------------------------------------------------------------ */

static bool is_arrow(int32_t key)
{
    size_t i;

    for (i = 0u; i < ND_MSG_ARROW_KEYS_N; i++) {
        if (nd_msg_arrow_keys[i] == key)
            return true;
    }
    return false;
}

const char *nd_msg_number_input_show(nd_ui *ui, const char *title, const char *prompt, char *buf,
                                     size_t cap)
{
    nd_textinput field;
    nd_softkey softkey;
    bool cursor_on = true;
    double last_blink;

    if (ui == NULL || buf == NULL || cap == 0u)
        return NULL;

    /* The whole of the subclass: input_filter="numbers", so the T9 engine has
     * only MODE_123 and the keypad types digits, '*' and '#' literally with
     * no multi-tap. */
    if (nd_textinput_init(&field, ui, title, prompt, buf, cap, "", ND_T9_FILTER_NUMBERS) != ND_OK)
        return NULL;

    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "OK", true);
    last_blink = nd_time_now();
    nd_textinput_draw(&field, cursor_on);

    for (;;) {
        int32_t key;

        /* Checked BEFORE the blocking wait, so an idle field never toggles;
         * see note 5 in the file header. */
        if (nd_time_now() - last_blink > 0.5) {
            cursor_on = !cursor_on;
            last_blink = nd_time_now();
            nd_textinput_draw(&field, cursor_on);
        }

        key = nd_ui_wait_for_key(ui);
        if (key == ND_KEY_NONE)
            continue;
        if (nd_app_should_exit())
            return NULL;

        if (is_arrow(key)) {
            nd_contact picked;

            memset(&picked, 0, sizeof picked);
            if (nd_contacts_pick(ui, "Contacts", "OK", NULL, "2-3", &picked, NULL)) {
                /* self.text = str(contact[2]) -- the number, straight in.
                 * The T9 engine and the underline watermark are NOT reset,
                 * so a half-typed word's underline survives onto the pasted
                 * number. That is what the Python does. */
                (void)nd_strlcpy(buf, picked.number, cap);
            }
            nd_textinput_draw(&field, cursor_on);
            nd_softkey_update(&softkey, "OK", true);
            continue;
        }

        switch (nd_textinput_handle_key(&field, key)) {
        case ND_WIDGET_RESULT_CONFIRM:
            return buf;
        case ND_WIDGET_RESULT_CANCEL:
            return NULL;
        case ND_WIDGET_RESULT_TYPED:
        case ND_WIDGET_RESULT_BACKSPACE:
        case ND_WIDGET_RESULT_MODE:
            nd_textinput_draw(&field, cursor_on);
            break;
        case ND_WIDGET_RESULT_NONE:
        case ND_WIDGET_RESULT_EMPTY_BACKSPACE:
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * _send_message_flow
 * ------------------------------------------------------------------ */

static void dialog(nd_ui *ui, const char *text)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, ui, text);
    (void)nd_msgdialog_show(&dlg);
}

/* text.strip(): Python strips the same six ASCII whitespace bytes plus a few
 * Unicode ones no keypad can produce. Written into `out`. */
static void strip_copy(char *out, size_t out_sz, const char *text)
{
    const char *start = nz(text);
    const char *end;
    size_t n;

    while (*start != '\0' && strchr(" \t\n\r\v\f", *start) != NULL && *start != '\0')
        start++;
    end = start + strlen(start);
    while (end > start && strchr(" \t\n\r\v\f", end[-1]) != NULL)
        end--;

    n = (size_t)(end - start);
    if (n >= out_sz)
        n = out_sz - 1u;
    memcpy(out, start, n);
    out[n] = '\0';
}

bool nd_msg_send_flow(nd_ui *ui, const char *text, int32_t root_id, int32_t sub_index)
{
    char body[ND_MSG_TEXT_MAX];
    char raw[ND_TEXTINPUT_CAP];
    char number[ND_TEXTINPUT_CAP];
    char msg[128];
    char detail[ND_MODEM_DETAIL_MAX];
    size_t n_chars;

    /* Accepted and never read, exactly as in the Python. */
    ND_UNUSED(root_id);
    ND_UNUSED(sub_index);

    if (ui == NULL)
        return false;

    strip_copy(body, sizeof body, text);
    if (body[0] == '\0') {
        dialog(ui, "Message is empty!");
        return false;
    }

    n_chars = nd_msg_codepoints(body);
    if (n_chars > (size_t)ND_MSG_SMS_MAX_CHARS) {
        (void)nd_snprintf(msg, sizeof msg, "Too long for one SMS (%zu/%d).", n_chars,
                          ND_MSG_SMS_MAX_CHARS);
        dialog(ui, msg);
        return false;
    }

    if (nd_msg_number_input_show(ui, "Send To", "Number:", raw, sizeof raw) == NULL)
        return false;

    nd_msg_filter_number(number, sizeof number, raw);
    if (number[0] == '\0') {
        dialog(ui, "No number given!");
        return false;
    }

    /* getattr(ui, "modem", None). In an app process this is ALWAYS NULL --
     * nd_app.h gives the modem, battery and notify handles to the core only
     * -- so the dialog below is what a real send reaches today. The Python
     * had the same branch for the same reason and never took it, because
     * Messages ran inside the core. OPEN-QUESTIONS.md MSG-1. */
    if (ui->modem == NULL) {
        dialog(ui, "ModemService is not running.");
        return false;
    }

    nd_msg_draw_sending(ui, number);

    detail[0] = '\0';
    if (nd_modem_send_sms(ui->modem, number, body, detail, sizeof detail)) {
        dialog(ui, "Message sent!");
        return true;
    }
    (void)nd_snprintf(msg, sizeof msg, "Send failed: %s", detail);
    dialog(ui, msg);
    return false;
}

/* ------------------------------------------------------------------ *
 * _show_message_detail
 * ------------------------------------------------------------------ */

nd_msg_detail_result nd_msg_show_detail(nd_ui *ui, const char *title, const char *root_id,
                                        int32_t sub_index, const char *message, int64_t message_id,
                                        const char *sender, int64_t timestamp)
{
    char body_storage[DETAIL_BODY_LINES][ND_TEXT_LINE_MAX];
    char meta[2][META_LINE_MAX];
    char timestamp_text[64];
    nd_lines body_lines;
    nd_header header;
    nd_softkey softkey;
    size_t n_meta = 0u;
    int32_t screen_w;
    int32_t screen_h;
    int32_t content_bottom;
    int32_t header_y;
    bool is_inbox;

    if (ui == NULL || ui->draw == NULL)
        return ND_MSG_DETAIL_BACK;

    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);
    content_bottom = nd_ui_content_bottom(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* The options branch is chosen by comparing the TITLE STRING. That is how
     * the Python tells the two callers apart; there is no flag. */
    is_inbox = (title != NULL && strcmp(title, "Inbox") == 0);

    nd_msg_format_timestamp(timestamp, timestamp_text, sizeof timestamp_text);
    if (sender != NULL && sender[0] != '\0')
        (void)nd_snprintf(meta[n_meta++], META_LINE_MAX, "From: %s", sender);
    (void)nd_snprintf(meta[n_meta++], META_LINE_MAX, "Time: %s", timestamp_text);

    /* max(20, screen_w - 20) == 220 on this panel. */
    nd_lines_init(&body_lines, body_storage, DETAIL_BODY_LINES);
    nd_msg_wrap_text(&body_lines, ui, message, nd_max32(20, screen_w - 20), ui->font_n);

    nd_header_init(&header, ui, nz(root_id));
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t key;
        int32_t y;
        size_t i;

        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);
        (void)nd_draw_text(ui->draw, 5, 5, nz(title), ui->font_xl, ND_WHITE);
        (void)nd_draw_line(ui->draw, 0, header_y, screen_w, header_y, ND_WHITE, 1);
        nd_header_draw(&header, sub_index);

        y = header_y + 10;
        for (i = 0u; i < n_meta; i++) {
            if (y > content_bottom - 18)
                break;
            (void)nd_draw_text(ui->draw, 10, y, meta[i], ui->font_s, ND_GRAY);
            y += 18;
        }

        y += 4;
        for (i = 0u; i < body_lines.n; i++) {
            if (y > content_bottom - 22)
                break;
            (void)nd_draw_text(ui->draw, 10, y, nd_lines_at(&body_lines, i), ui->font_n, ND_WHITE);
            y += 22;
        }

        nd_softkey_update(&softkey, "Options", false);
        (void)nd_ui_present(ui);

        key = nd_ui_wait_for_key(ui);
        if (key == ND_KEY_CLEAR)
            return ND_MSG_DETAIL_BACK;
        if (nd_app_should_exit())
            return ND_MSG_DETAIL_BACK;

        if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER) {
            nd_vlist options;
            int32_t selection;

            /* NO softkey update before this: the bar still says "Options".
             * See note 4 in the file header. */
            if (is_inbox) {
                nd_vlist_init(&options, ui, "Options", nd_msg_inbox_options,
                              ND_MSG_INBOX_OPTIONS_N, ND_MESSAGES_ROOT_ID);
                nd_header_init(&options.header, ui, nz(root_id));
                selection = nd_vlist_show(&options);
                if (selection == 0 && message_id >= 0) {
                    nd_msg_delete_inbox(message_id);
                    dialog(ui, "Erased!");
                    return ND_MSG_DETAIL_DELETED;
                }
            } else if (title != NULL && strcmp(title, "Outbox") == 0) {
                nd_vlist_init(&options, ui, "Options", nd_msg_outbox_options,
                              ND_MSG_OUTBOX_OPTIONS_N, ND_MESSAGES_ROOT_ID);
                nd_header_init(&options.header, ui, nz(root_id));
                selection = nd_vlist_show(&options);
                if (selection == 0 && message_id >= 0) {
                    nd_msg_delete_outbox(message_id);
                    dialog(ui, "Erased!");
                    return ND_MSG_DETAIL_DELETED;
                }
                if (selection == 1)
                    (void)nd_msg_send_flow(ui, message, ND_MESSAGES_ROOT_ID, sub_index);
            }
            /* Any other title -- there is none today -- falls straight
             * through to the redraw, which is what the Python's if/elif with
             * no else does. */
        }
    }
}

/* ------------------------------------------------------------------ *
 * The two list screens
 * ------------------------------------------------------------------ */

/* One list screen's worth of rows, plus the strings the VerticalList borrows.
 *
 * Heap, not stack: 128 rows is about 139 KB and CODING-STANDARDS.md section
 * 1.5 keeps anything sized by input off the stack. Allocated when the screen
 * opens, freed before it returns -- one live allocation per list screen, and
 * never one per frame. */
typedef struct {
    nd_msg_rec *rows;
    char (*labels)[ND_MSG_LABEL_MAX];
    const char **items;
    size_t n;
} msg_list;

static void list_free(msg_list *l)
{
    free(l->rows);
    free(l->labels);
    free(l->items);
    memset(l, 0, sizeof *l);
}

static bool list_alloc(msg_list *l)
{
    memset(l, 0, sizeof *l);
    /* 128 * sizeof(nd_msg_rec) = 128 * 1112 = 142,336 bytes. */
    l->rows = calloc(ND_MSG_LIST_MAX, sizeof *l->rows);
    l->labels = calloc(ND_MSG_LIST_MAX, sizeof *l->labels);
    l->items = calloc(ND_MSG_LIST_MAX, sizeof *l->items);
    if (l->rows == NULL || l->labels == NULL || l->items == NULL) {
        list_free(l);
        return false;
    }
    return true;
}

void nd_msg_show_inbox(nd_ui *ui, int32_t root_id, int32_t sub_index)
{
    msg_list list;
    char header_root[16];

    if (ui == NULL)
        return;
    if (nd_snprintf(header_root, sizeof header_root, "%d-%d", root_id, sub_index) != ND_OK)
        return;
    if (!list_alloc(&list)) {
        nd_log_err(ND_LOG_UI, "Messages: out of memory opening the inbox");
        return;
    }

    for (;;) {
        nd_vlist v_list;
        nd_softkey softkey;
        int32_t selection_index;
        size_t i;

        list.n = nd_msg_fetch_inbox(list.rows, ND_MSG_LIST_MAX);
        if (list.n == 0u) {
            /* sub_index is None here, so the breadcrumb is just "2-1". */
            nd_msg_show_empty_state(ui, "Inbox", header_root, ND_MSG_NO_SUB, "No Messages");
            break;
        }

        for (i = 0u; i < list.n; i++) {
            if (list.rows[i].is_read != 0) {
                (void)nd_strlcpy(list.labels[i], list.rows[i].sender, ND_MSG_LABEL_MAX);
            } else {
                (void)nd_snprintf(list.labels[i], ND_MSG_LABEL_MAX, "* %s", list.rows[i].sender);
            }
            list.items[i] = list.labels[i];
        }

        /* Rebuilt every iteration, as the Python rebuilds it: the cursor goes
         * back to row 0 after every message. */
        nd_vlist_init(&v_list, ui, "Inbox", list.items, list.n, ND_MESSAGES_ROOT_ID);
        nd_header_init(&v_list.header, ui, header_root);
        nd_softkey_init(&softkey, ui, false);

        nd_softkey_update(&softkey, "Open", false);
        selection_index = nd_vlist_show(&v_list);
        if (selection_index == ND_WIDGET_BACK)
            break;
        if ((size_t)selection_index >= list.n)
            break;

        nd_msg_mark_read(list.rows[selection_index].id);
        (void)nd_msg_show_detail(ui, "Inbox", header_root, selection_index + 1,
                                 list.rows[selection_index].message, list.rows[selection_index].id,
                                 list.rows[selection_index].sender,
                                 list.rows[selection_index].timestamp);
        /* `if result == "deleted": continue` -- and the loop repeats either
         * way, because that continue is the last statement in the body. */
        if (nd_app_should_exit())
            break;
    }

    list_free(&list);
}

void nd_msg_show_outbox(nd_ui *ui, int32_t root_id, int32_t sub_index)
{
    msg_list list;
    char header_root[16];

    if (ui == NULL)
        return;
    if (nd_snprintf(header_root, sizeof header_root, "%d-%d", root_id, sub_index) != ND_OK)
        return;
    if (!list_alloc(&list)) {
        nd_log_err(ND_LOG_UI, "Messages: out of memory opening the outbox");
        return;
    }

    for (;;) {
        nd_vlist v_list;
        nd_softkey softkey;
        int32_t selection_index;
        size_t i;

        list.n = nd_msg_fetch_outbox(list.rows, ND_MSG_LIST_MAX);
        if (list.n == 0u) {
            nd_msg_show_empty_state(ui, "Outbox", header_root, ND_MSG_NO_SUB, "No Messages");
            break;
        }

        /* The rows ARE the labels here -- no "* " marker and no read state.
         * Pointed at in place, so a long draft is not truncated before the
         * list's own fitter has seen it. */
        for (i = 0u; i < list.n; i++)
            list.items[i] = list.rows[i].message;

        nd_vlist_init(&v_list, ui, "Outbox", list.items, list.n, ND_MESSAGES_ROOT_ID);
        nd_header_init(&v_list.header, ui, header_root);
        nd_softkey_init(&softkey, ui, false);

        nd_softkey_update(&softkey, "Open", false);
        selection_index = nd_vlist_show(&v_list);
        if (selection_index == ND_WIDGET_BACK)
            break;
        if ((size_t)selection_index >= list.n)
            break;

        /* No _mark_read: an outbox row has no is_read column. */
        (void)nd_msg_show_detail(ui, "Outbox", header_root, selection_index + 1,
                                 list.rows[selection_index].message, list.rows[selection_index].id,
                                 NULL, list.rows[selection_index].timestamp);
        if (nd_app_should_exit())
            break;
    }

    list_free(&list);
}

/* ------------------------------------------------------------------ *
 * _show_write_message -- the composer
 * ------------------------------------------------------------------ */

void nd_msg_show_write(nd_ui *ui, int32_t root_id, int32_t sub_index)
{
    char text[ND_TEXTLONG_CAP];
    char options_root[16];
    nd_textlong input_widget;
    nd_softkey softkey;
    bool cursor_on = true;
    double last_blink;

    if (ui == NULL)
        return;
    if (nd_snprintf(options_root, sizeof options_root, "%d-%d", root_id, sub_index) != ND_OK)
        return;
    if (nd_textlong_init(&input_widget, ui, "Write", text, sizeof text, "", ND_T9_FILTER_ANY) !=
        ND_OK)
        return;

    nd_softkey_init(&softkey, ui, false);
    last_blink = nd_time_now();
    nd_textlong_draw(&input_widget, cursor_on);
    nd_softkey_update(&softkey, "Options", true);

    for (;;) {
        int32_t key;

        if (nd_time_now() - last_blink > 0.5) {
            cursor_on = !cursor_on;
            last_blink = nd_time_now();
            nd_textlong_draw(&input_widget, cursor_on);
            nd_softkey_update(&softkey, "Options", true);
        }

        key = nd_ui_wait_for_key(ui);
        if (key == ND_KEY_NONE)
            continue;
        if (nd_app_should_exit())
            return;

        if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER) {
            nd_vlist options;
            int32_t selection;

            nd_vlist_init(&options, ui, "Options", WRITE_OPTIONS, ND_ARRAY_LEN(WRITE_OPTIONS),
                          ND_MESSAGES_ROOT_ID);
            nd_header_init(&options.header, ui, options_root);
            selection = nd_vlist_show(&options);

            if (selection == 0) {
                if (nd_msg_send_flow(ui, nd_textlong_get_text(&input_widget), root_id, sub_index))
                    return; /* sent -- leave the composer */
                /* failed or cancelled: fall through, keep the draft on screen */
            } else if (selection == 1) {
                (void)nd_msg_save_outbox(nd_textlong_get_text(&input_widget));
                dialog(ui, "Saved!");
            }

            nd_textlong_draw(&input_widget, cursor_on);
            nd_softkey_update(&softkey, "Options", true);
            continue;
        }

        if (nd_textlong_handle_key(&input_widget, key) == ND_WIDGET_RESULT_EMPTY_BACKSPACE)
            return; /* Back on an empty field exits the composer */
        nd_textlong_draw(&input_widget, cursor_on);
        nd_softkey_update(&softkey, "Options", true);
    }
}

/* ------------------------------------------------------------------ *
 * run(ui) and the two entry points the core calls
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_pagedlist menu;

    if (ui == NULL)
        return 1;

    /* Built ONCE, outside the loop -- see the file header. root_id is the
     * integer 2 here and PagedList formats it with "%s", so there is no
     * padding and no dash. */
    nd_pagedlist_init(&menu, ui, "Messages", nd_msg_menu_items, ND_MSG_MENU_ITEMS, "2", true);

    for (;;) {
        int32_t sel = nd_pagedlist_show(&menu);

        if (sel < 0)
            return 0;

        if (sel == 0)
            nd_msg_show_inbox(ui, ND_MESSAGES_ROOT_ID, 1);
        else if (sel == 1)
            nd_msg_show_outbox(ui, ND_MESSAGES_ROOT_ID, 2);
        else if (sel == 2)
            nd_msg_show_write(ui, ND_MESSAGES_ROOT_ID, 3);

        if (nd_app_should_exit())
            return 0;
    }
}

/* open_message(ui, message_id) -- the home screen's Read softkey with exactly
 * one unread SMS. nd-apprun reaches it as `nd-apprun <dir> open_message <id>`. */
int app_open_message(nd_ui *ui, int64_t message_id)
{
    nd_msg_rec row;

    if (ui == NULL)
        return 1;

    if (!nd_msg_fetch_inbox_one(message_id, &row)) {
        /* `return _show_inbox(ui, ROOT_ID_MESSAGES, 1)` */
        nd_msg_show_inbox(ui, ND_MESSAGES_ROOT_ID, 1);
        return 0;
    }

    nd_msg_mark_read(row.id);
    (void)nd_msg_show_detail(ui, "Inbox", "2-1", 1, row.message, row.id, row.sender, row.timestamp);
    return 0;
}

/* open_inbox(ui) -- the same softkey with several unread. */
int app_open_inbox(nd_ui *ui)
{
    if (ui == NULL)
        return 1;
    nd_msg_show_inbox(ui, ND_MESSAGES_ROOT_ID, 1);
    return 0;
}

/* Nothing is held open between screens: every database handle is closed by
 * the statement that opened it, the list allocation is freed before its
 * screen returns, and no child process is ever spawned. The symbol exists
 * because nd_app.h requires every app to export one. */
void app_shutdown(void) {}
