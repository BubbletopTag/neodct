/* apps/Messages/chat.c -- the conversation front end.
 *
 * NOT A PORT. messages.h explains the setting that chooses it and the column
 * it needed; threads.c does the grouping. This file is the two screens.
 *
 * ============ WHY THESE ARE HAND-DRAWN AND NOT VerticalList ============
 *
 * The conversation LIST could almost be an nd_vlist -- until a row has to
 * carry two lines (a name and a preview) at two sizes, plus an unread mark.
 * nd_vlist draws exactly one string per row in one font, and widening it for
 * one app would change every list on the phone.
 *
 * The conversation VIEW could not be a list at all. A bubble is a rounded box
 * whose width depends on its text, aligned left or right, and the selection
 * has to be able to leave the messages entirely and land on the message box
 * at the bottom.
 *
 * So both are drawn here, with the same pieces every other screen uses --
 * nd_draw, nd_softkey, nd_msg_wrap_text -- and they keep the OS's shape:
 * white ink on black, the softkey bar where it always is, the breadcrumb in
 * the corner. What they borrow from KaiOS is the arrangement, not the paint.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "messages.h"

/* ------------------------------------------------------------------ *
 * Shared geometry
 * ------------------------------------------------------------------ */

#define CHAT_HEADER_H 24 /* the title row, with a rule under it   */
#define CHAT_MARGIN   5

/* ============ THE VERTICAL BUDGET, WHICH IS EXACT ============
 *
 * 145 px of content, and the numbers come from where the fonts actually put
 * INK, measured rather than assumed -- an em size is not a glyph height, and
 * using the em size is what makes one line of text sit in the descenders of
 * the line above it. Relative to a text origin `o`:
 *
 *     font_s  (14 px)   ink o+1 .. o+15   (15 rows)
 *     font_md (18 px)   ink o+3 .. o+19   (17 rows)
 *     font_n  (20 px)   ink o+2 .. o+22   (21 rows)
 *
 * ============ WHY ONLY TWO CONVERSATIONS ARE VISIBLE ============
 *
 * This showed three, and the third was bought by letting the preview overlap
 * the name: a contact whose name has descenders -- "NeoDCT Support" -- had
 * the grey preview drawn straight through the tails of its p's.
 *
 * The arithmetic says that is not fixable at three. A row needs 17 rows of
 * name plus 15 of preview plus the gaps that make them read as two lines,
 * which is a pitch of about 40. Three of those is 120, and the header and
 * the New Message row have already spent 49 of the 145. Three rows fit only
 * by removing the gaps, which is the bug.
 *
 * So the row got the space instead of the count:
 *
 *   24  header: font_n at o=1, ink 3..23, rule at 24 clears it
 *    3  air
 *   22  New Message: font_md at o=y-2, ink y+1..y+17, rule at y+20
 *   96  two rows of 48
 *   ---
 *  145  = content_bottom. Exactly.
 *
 * A row is font_md at o=y+1 (ink y+4..y+20) over font_s at o=y+24 (ink
 * y+25..y+39), rule at y+46. Four clear pixels between the two lines, six
 * under the preview. Change any constant here alone and a row stops fitting
 * -- threads_draw() breaks out rather than drawing one that would overrun,
 * so the symptom is a conversation that silently vanishes. */
#define ROW_H       48
#define ROW_VISIBLE 2
#define NEW_ROW_H   22

/* Bubbles. The 0.72 is the fraction of the screen one may occupy before its
 * text wraps: wide enough for a sentence, narrow enough that left and right
 * are still obviously different sides. */
#define BUBBLE_MAX_W_NUM  72
#define BUBBLE_MAX_W_DEN  100
#define BUBBLE_PAD_X      4
#define BUBBLE_PAD_Y      3
#define BUBBLE_LINE_H     15
#define BUBBLE_GAP        4
#define BUBBLE_MAX_LINES  8

/* The message box at the bottom of a conversation. */
#define BOX_H 20

static const char *nz(const char *s)
{
    return (s != NULL) ? s : "";
}

/* ------------------------------------------------------------------ *
 * A rounded box, in the one way this panel can draw one
 * ------------------------------------------------------------------ */

/* No anti-aliasing and no radius worth the name: at 15 px a "rounded" corner
 * is one pixel taken off each end, which is exactly what a Nokia's own boxes
 * did. Drawn as three rectangles rather than a polygon so the edges land on
 * whole pixels -- nd_draw.h's note about polygon edges at small sizes applies
 * here for the same reason the T9 pencil is plotted per pixel. */
static void chat_box(nd_draw *d, nd_rect r, nd_color fill, bool outline_only)
{
    nd_rect mid = ND_RECT(r.x0, r.y0 + 1, r.x1, r.y1 - 1);
    nd_rect top = ND_RECT(r.x0 + 1, r.y0, r.x1 - 1, r.y0);
    nd_rect bot = ND_RECT(r.x0 + 1, r.y1, r.x1 - 1, r.y1);

    if (outline_only) {
        (void)nd_draw_rect_outline(d, mid, fill, 1);
        (void)nd_draw_rect_fill(d, top, fill);
        (void)nd_draw_rect_fill(d, bot, fill);
        return;
    }
    (void)nd_draw_rect_fill(d, mid, fill);
    (void)nd_draw_rect_fill(d, top, fill);
    (void)nd_draw_rect_fill(d, bot, fill);
}

/* ------------------------------------------------------------------ *
 * The conversation list
 * ------------------------------------------------------------------ */

typedef struct {
    nd_msg_thread *rows;
    size_t n;
    size_t selected; /* 0 is "New Message"; a thread is selected - 1 */
    size_t window;   /* index of the first THREAD row on screen */
} thread_list;

static void draw_scrollbar(nd_ui *ui, size_t first, size_t visible, size_t total, int32_t top,
                           int32_t bottom)
{
    nd_draw *d = ui->draw;
    int32_t x = nd_ui_width(ui) - 5;
    double span;
    double notch;

    (void)nd_draw_line(d, x, top, x, bottom, ND_GRAY, 1);
    if (total <= visible)
        return;
    span = (double)(total - visible);
    /* nd_trunc32 and not round(): every scrollbar in this OS truncates, and
     * one that did not would sit a pixel off the ones beside it. */
    notch = (double)top + ((double)first / span) * (double)(bottom - top - 6);
    (void)nd_draw_rect_fill(d, ND_RECT(x - 2, nd_trunc32(notch), x + 2, nd_trunc32(notch) + 6),
                            ND_WHITE);
}

static void threads_draw(nd_ui *ui, thread_list *tl, nd_softkey *bar)
{
    nd_draw *d = ui->draw;
    int32_t w = nd_ui_width(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);
    int32_t y;
    size_t i;
    bool on_new = (tl->selected == 0u);

    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, w, content_bottom), ND_BLACK);

    (void)nd_draw_text(d, CHAT_MARGIN, 1, "Messages", ui->font_n, ND_WHITE);
    (void)nd_draw_line(d, 0, CHAT_HEADER_H, w, CHAT_HEADER_H, ND_GRAY, 1);

    /* ---- New Message, always the first row ---- */
    y = CHAT_HEADER_H + 3;
    if (on_new) {
        (void)nd_draw_rect_fill(d, ND_RECT(2, y - 2, w - 8, y + NEW_ROW_H - 4), ND_WHITE);
        (void)nd_draw_text(d, CHAT_MARGIN + 4, y - 2, "New Message", ui->font_md, ND_BLACK);
    } else {
        (void)nd_draw_text(d, CHAT_MARGIN + 4, y - 2, "New Message", ui->font_md, ND_WHITE);
    }
    y += NEW_ROW_H;
    (void)nd_draw_line(d, CHAT_MARGIN, y - 2, w - 8, y - 2, ND_GRAY, 1);

    /* ---- one row per conversation ---- */
    for (i = 0u; i < (size_t)ROW_VISIBLE; i++) {
        size_t idx = tl->window + i;
        const nd_msg_thread *t;
        bool selected;
        nd_color name_c;
        nd_color prev_c;
        char name[64];
        char preview[96];

        if (idx >= tl->n)
            break;
        t = &tl->rows[idx];
        selected = (tl->selected == idx + 1u);
        if (y + ROW_H > content_bottom)
            break;

        if (selected) {
            (void)nd_draw_rect_fill(d, ND_RECT(2, y + 1, w - 8, y + ROW_H - 4), ND_WHITE);
            name_c = ND_BLACK;
            prev_c = ND_BLACK;
        } else {
            name_c = ND_WHITE;
            prev_c = ND_GRAY;
        }

        /* An unread thread is marked the way the ported inbox marks an unread
         * row -- a leading "*" -- rather than with a badge this panel has no
         * room for. */
        if (t->unread > 0)
            (void)nd_snprintf(name, sizeof name, "* %s", t->display);
        else
            (void)nd_strlcpy(name, t->display, sizeof name);

        /* The widget does not ellipsize, so both lines are fitted here. */
        {
            char fitted[64];

            (void)nd_text_ellipsize(fitted, sizeof fitted, name, ui->font_md, w - 22);
            (void)nd_draw_text(d, CHAT_MARGIN + 4, y + 1, fitted, ui->font_md, name_c);
        }
        {
            char fitted[96];

            /* An outgoing last message is prefixed, because "Mum / see you
             * at six" reads as something Mum said unless it says otherwise. */
            if (t->last_outgoing)
                (void)nd_snprintf(preview, sizeof preview, "You: %s", t->preview);
            else
                (void)nd_strlcpy(preview, t->preview, sizeof preview);
            (void)nd_text_ellipsize(fitted, sizeof fitted, preview, ui->font_s, w - 22);
            (void)nd_draw_text(d, CHAT_MARGIN + 4, y + 24, fitted, ui->font_s, prev_c);
        }

        y += ROW_H;
        if (!selected)
            (void)nd_draw_line(d, CHAT_MARGIN, y - 2, w - 8, y - 2, ND_GRAY, 1);
    }

    if (tl->n == 0u) {
        (void)nd_draw_text(d, CHAT_MARGIN + 4, CHAT_HEADER_H + 40, "No conversations yet.",
                           ui->font_s, ND_GRAY);
    }

    draw_scrollbar(ui, tl->window, (size_t)ROW_VISIBLE, tl->n, CHAT_HEADER_H + NEW_ROW_H + 4,
                   content_bottom - 4);

    nd_softkey_update(bar, on_new ? "Write" : "Open", true);
}

/* Keeps the selected thread inside the three-row window. */
static void threads_scroll_into_view(thread_list *tl)
{
    size_t sel;

    if (tl->selected == 0u) {
        tl->window = 0u;
        return;
    }
    sel = tl->selected - 1u;
    if (sel < tl->window)
        tl->window = sel;
    else if (sel >= tl->window + (size_t)ROW_VISIBLE)
        tl->window = sel - (size_t)ROW_VISIBLE + 1u;
}

void nd_msg_show_threads(nd_ui *ui)
{
    thread_list tl;
    nd_softkey bar;

    if (ui == NULL || ui->draw == NULL)
        return;

    memset(&tl, 0, sizeof tl);
    tl.rows = calloc((size_t)ND_MSG_THREADS_MAX, sizeof *tl.rows);
    if (tl.rows == NULL)
        return;
    nd_softkey_init(&bar, ui, false);

    for (;;) {
        int32_t key;

        /* Re-read on every pass round the loop, not once: a conversation just
         * left may have gained a sent message, lost one to Delete, or become
         * read. The read is two capped queries and is cheaper than getting
         * this wrong. */
        tl.n = nd_msg_threads(tl.rows, (size_t)ND_MSG_THREADS_MAX);
        if (tl.selected > tl.n)
            tl.selected = tl.n;
        threads_scroll_into_view(&tl);
        threads_draw(ui, &tl, &bar);

        key = nd_ui_wait_for_key(ui);
        if (nd_app_should_exit())
            break;

        if (key == ND_KEY_CLEAR)
            break;
        if (key == ND_KEY_DOWN || key == ND_KEY_8) {
            if (tl.selected < tl.n)
                tl.selected++;
            continue;
        }
        if (key == ND_KEY_UP || key == ND_KEY_2) {
            if (tl.selected > 0u)
                tl.selected--;
            continue;
        }
        if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER) {
            if (tl.selected == 0u) {
                /* The standard composer, exactly as Classic's Write Message
                 * row opens it -- no recipient, so it asks for one. */
                (void)nd_msg_show_write_prefill(ui, ND_MESSAGES_ROOT_ID, 3, NULL, NULL);
            } else {
                const nd_msg_thread *t = &tl.rows[tl.selected - 1u];

                nd_msg_show_thread(ui, t->number, t->display);
            }
            continue;
        }
    }

    free(tl.rows);
}

/* ------------------------------------------------------------------ *
 * One conversation
 * ------------------------------------------------------------------ */

/* A bubble, laid out. The wrap is done once when the conversation is read
 * rather than once per frame: scrolling redraws several times a second and
 * re-measuring every line each time is how a 240x175 screen starts to feel
 * slow on a Cortex-A7. */
typedef struct {
    int32_t h;
    int32_t w;
    size_t n_lines;
    char lines[BUBBLE_MAX_LINES][ND_TEXT_LINE_MAX];
} bubble_layout;

static void layout_bubble(nd_ui *ui, const nd_msg_bubble *b, bubble_layout *out)
{
    char storage[BUBBLE_MAX_LINES][ND_TEXT_LINE_MAX];
    nd_lines lines;
    int32_t max_text_w =
        (nd_ui_width(ui) * BUBBLE_MAX_W_NUM) / BUBBLE_MAX_W_DEN - 2 * BUBBLE_PAD_X;
    size_t i;
    int32_t widest = 0;

    memset(out, 0, sizeof *out);
    nd_lines_init(&lines, storage, (size_t)BUBBLE_MAX_LINES);
    /* The app's own wrapper, so a bubble breaks lines the same way the ported
     * detail screen does. */
    nd_msg_wrap_text(&lines, ui, b->text, max_text_w, ui->font_s);

    out->n_lines = lines.n;
    for (i = 0u; i < lines.n && i < (size_t)BUBBLE_MAX_LINES; i++) {
        int32_t lw = 0;

        (void)nd_strlcpy(out->lines[i], nd_lines_at(&lines, i), ND_TEXT_LINE_MAX);
        nd_text_size(ui->font_s, out->lines[i], &lw, NULL);
        if (lw > widest)
            widest = lw;
    }
    if (out->n_lines > (size_t)BUBBLE_MAX_LINES)
        out->n_lines = (size_t)BUBBLE_MAX_LINES;

    out->w = widest + 2 * BUBBLE_PAD_X;
    out->h = (int32_t)out->n_lines * BUBBLE_LINE_H + 2 * BUBBLE_PAD_Y;
}

typedef struct {
    nd_msg_bubble *msgs;
    bubble_layout *lay;
    size_t n;
    size_t selected; /* n means the message box */
    /* The index of the topmost bubble DRAWN, not a pixel offset. A pixel
     * offset slices whichever bubble straddles the top edge, and on a panel
     * this small that reads as a rendering fault: the border is gone and the
     * first row of text is cut through the middle of the glyphs. Scrolling a
     * whole bubble at a time costs a few pixels of slack at the bottom and
     * never looks broken. */
    size_t first;
} chat_view;

/* The message box is one past the last bubble, so Down walks off the end of
 * the conversation and onto it -- which is what "use the up and down arrow to
 * select the text box" means. */
static bool on_box(const chat_view *v)
{
    return v->selected >= v->n;
}

/* Stacked height of bubbles [from, to_excl), gaps included. */
static int32_t bubbles_height(const chat_view *v, size_t from, size_t to_excl)
{
    int32_t h = 0;
    size_t i;

    for (i = from; i < to_excl && i < v->n; i++)
        h += v->lay[i].h + BUBBLE_GAP;
    return h;
}

static void chat_draw(nd_ui *ui, chat_view *v, const char *title, nd_softkey *bar)
{
    nd_draw *d = ui->draw;
    int32_t w = nd_ui_width(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);
    int32_t view_top = CHAT_HEADER_H + 2;
    int32_t view_bottom = content_bottom - BOX_H - 2;
    int32_t y;
    size_t i;

    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, w, content_bottom), ND_BLACK);

    /* The transcript starts at v->first, whose top sits exactly on view_top,
     * so nothing is ever cut by the header. Only the bottom edge clips, and
     * that is the cue that there is more below. */
    y = view_top;
    for (i = v->first; i < v->n; i++) {
        const bubble_layout *bl = &v->lay[i];
        bool out = v->msgs[i].outgoing;
        bool selected = (!on_box(v) && v->selected == i);
        int32_t bx = out ? (w - 8 - bl->w) : 6;
        nd_rect box = ND_RECT(bx, y, bx + bl->w, y + bl->h);
        size_t k;

        if (y > view_bottom)
            break; /* and everything after it is further down still */

        /* OUTGOING is filled white with black text, INCOMING is an outline
         * with white text. That is the strongest two-way distinction a
         * one-bit-looking panel has, and it survives being photographed. */
        if (out)
            chat_box(d, box, ND_WHITE, false);
        else
            chat_box(d, box, ND_WHITE, true);

        /* The selection is a second rule just outside the bubble rather than
         * an inverted fill: inverting an outgoing bubble would make it look
         * incoming. */
        if (selected) {
            (void)nd_draw_rect_outline(d, ND_RECT(box.x0 - 2, box.y0 - 2, box.x1 + 2, box.y1 + 2),
                                       ND_GRAY, 1);
        }

        for (k = 0u; k < bl->n_lines; k++) {
            (void)nd_draw_text(d, bx + BUBBLE_PAD_X, y + BUBBLE_PAD_Y + (int32_t)k * BUBBLE_LINE_H,
                               bl->lines[k], ui->font_s, out ? ND_BLACK : ND_WHITE);
        }
        y += bl->h + BUBBLE_GAP;
    }

    /* A last bubble hanging past the fold is painted over -- one rectangle,
     * and cheaper than clipping every draw call inside the loop. */
    (void)nd_draw_rect_fill(d, ND_RECT(0, view_bottom + 1, w, content_bottom), ND_BLACK);

    /* The header goes on LAST so the transcript can never reach it, and the
     * title is ellipsized because a contact name is whatever the phone book
     * says it is. */
    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, w, CHAT_HEADER_H), ND_BLACK);
    {
        char fitted[64];

        (void)nd_text_ellipsize(fitted, sizeof fitted, nz(title), ui->font_n, w - 14);
        (void)nd_draw_text(d, CHAT_MARGIN, 1, fitted, ui->font_n, ND_WHITE);
    }
    (void)nd_draw_line(d, 0, CHAT_HEADER_H, w, CHAT_HEADER_H, ND_GRAY, 1);

    /* Without this there is no cue at all that the conversation continues off
     * either edge -- the transcript has no row separators to count. */
    if (v->n > 0u)
        draw_scrollbar(ui, v->first, i - v->first, v->n, view_top, view_bottom);

    /* ---- the message box ---- */
    {
        nd_rect box = ND_RECT(4, content_bottom - BOX_H, w - 6, content_bottom - 2);

        if (on_box(v)) {
            (void)nd_draw_rect_fill(d, box, ND_WHITE);
            (void)nd_draw_text(d, box.x0 + 5, box.y0 + 2, "Message", ui->font_s, ND_BLACK);
        } else {
            (void)nd_draw_rect_outline(d, box, ND_GRAY, 1);
            (void)nd_draw_text(d, box.x0 + 5, box.y0 + 2, "Message", ui->font_s, ND_GRAY);
        }
    }

    if (v->n == 0u) {
        (void)nd_draw_text(d, CHAT_MARGIN + 4, view_top + 30, "No messages.", ui->font_s,
                           ND_GRAY);
    }

    nd_softkey_update(bar, on_box(v) ? "Write" : "Options", true);
}

/* Moves the window so the anchor bubble is wholly visible, and only then --
 * a selection that is already on screen must not make the transcript jump.
 *
 * The anchor is the selected bubble, or the newest one when the cursor is on
 * the message box: the box is not in the transcript, and the newest message
 * is what you want to see while you reply to it. */
static void chat_scroll_into_view(nd_ui *ui, chat_view *v)
{
    int32_t view_h = nd_ui_content_bottom(ui) - (CHAT_HEADER_H + 2) - BOX_H - 2;
    size_t anchor;

    if (v->n == 0u) {
        v->first = 0u;
        return;
    }
    anchor = on_box(v) ? v->n - 1u : v->selected;
    if (anchor >= v->n)
        anchor = v->n - 1u;
    if (v->first > anchor)
        v->first = anchor;

    /* Drop whole bubbles off the top until the anchor's bottom is inside the
     * viewport. A single bubble taller than the viewport can never satisfy
     * that, so the loop stops when it becomes the window on its own and the
     * tail runs off the bottom -- the same thing Classic's detail page does
     * with a message longer than one screen. */
    while (v->first < anchor && bubbles_height(v, v->first, anchor + 1u) > view_h)
        v->first++;
}

/* Delete / Forward on one bubble. Returns true when the conversation has to
 * be re-read -- which a delete does and a forward does not. */
static bool bubble_options(nd_ui *ui, const nd_msg_bubble *b)
{
    static const char *const OPTIONS[2] = {"Delete", "Forward"};
    nd_vlist menu;
    nd_softkey bar;
    int32_t sel;

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "OK", false);
    nd_vlist_init(&menu, ui, "Options", OPTIONS, 2u, ND_MESSAGES_ROOT_ID);
    sel = nd_vlist_show(&menu);

    if (sel == 0) {
        /* An inbox id and an outbox id are both rowids and can collide, so
         * `outgoing` is what decides which table this deletes from. Getting
         * that wrong deletes somebody else's message. */
        if (b->outgoing)
            nd_msg_delete_outbox(b->id);
        else
            nd_msg_delete_inbox(b->id);
        return true;
    }
    if (sel == 1) {
        /* Forward: the composer with this text in it and no recipient, so it
         * asks who to send it to -- the same thing Classic's Forward does. */
        (void)nd_msg_show_write_prefill(ui, ND_MESSAGES_ROOT_ID, 3, b->text, NULL);
    }
    return false;
}

void nd_msg_show_thread(nd_ui *ui, const char *peer, const char *display)
{
    chat_view v;
    nd_softkey bar;
    char title[ND_MSG_SENDER_MAX];
    bool reload = true;

    if (ui == NULL || ui->draw == NULL || peer == NULL)
        return;

    (void)nd_strlcpy(title, (display != NULL && display[0] != '\0') ? display : peer,
                     sizeof title);

    memset(&v, 0, sizeof v);
    v.msgs = calloc((size_t)ND_MSG_BUBBLES_MAX, sizeof *v.msgs);
    v.lay = calloc((size_t)ND_MSG_BUBBLES_MAX, sizeof *v.lay);
    if (v.msgs == NULL || v.lay == NULL) {
        free(v.msgs);
        free(v.lay);
        return;
    }
    nd_softkey_init(&bar, ui, false);

    /* Opening a conversation is reading it. */
    nd_msg_thread_mark_read(peer);

    for (;;) {
        int32_t key;

        if (reload) {
            size_t i;

            v.n = nd_msg_thread_messages(peer, v.msgs, (size_t)ND_MSG_BUBBLES_MAX);
            for (i = 0u; i < v.n; i++)
                layout_bubble(ui, &v.msgs[i], &v.lay[i]);
            /* Open on the message box, which pins the view to the newest
             * message -- a conversation you open should show its end. */
            v.selected = v.n;
            reload = false;
        }
        chat_scroll_into_view(ui, &v);
        chat_draw(ui, &v, title, &bar);

        key = nd_ui_wait_for_key(ui);
        if (nd_app_should_exit())
            break;

        if (key == ND_KEY_CLEAR)
            break;
        if (key == ND_KEY_UP || key == ND_KEY_2) {
            if (v.selected > 0u)
                v.selected--;
            continue;
        }
        if (key == ND_KEY_DOWN || key == ND_KEY_8) {
            if (v.selected < v.n)
                v.selected++;
            continue;
        }
        if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER) {
            if (on_box(&v)) {
                /* Addressed already: a reply from inside a conversation must
                 * not ask who it is to. */
                if (nd_msg_show_write_prefill(ui, ND_MESSAGES_ROOT_ID, 3, NULL, peer))
                    reload = true;
            } else if (v.selected < v.n) {
                if (bubble_options(ui, &v.msgs[v.selected]))
                    reload = true;
            }
            continue;
        }
    }

    free(v.msgs);
    free(v.lay);
}
