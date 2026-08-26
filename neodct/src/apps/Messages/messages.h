/* messages.h -- the pieces of the Messages app a unit test can reach.
 *
 * System/apps/Messages/main.py is 475 lines of module-level functions. The
 * seven that touch sqlite live in msg_db.c; the screens, the app's own text
 * wrapper and the three exported entry points live in main.c. Everything a
 * test needs to reach is declared here rather than left static, the way
 * phonebook.h does it, and test/unit/test_messages.c dlopen()s the BUILT
 * app.so and dlsym()s them -- so the test exercises the artefact that ships
 * and not a second copy compiled with different flags.
 *
 * Names follow CODING-STANDARDS.md section 2; the Python name each one came
 * from is on its declaration.
 *
 * ============ WHAT IS NOT HERE ============
 *
 * `_show_stub_screen()` (main.py lines 32-51) has no callers anywhere in the
 * tree and is not ported. spec-apps-core.md says so explicitly.
 */

#ifndef ND_MESSAGES_H_INCLUDED
#define ND_MESSAGES_H_INCLUDED

#include "nd_font.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Messages Style -- NOT a port
 * ------------------------------------------------------------------ *
 *
 * The Python has one Messages app: a three-item menu over an Inbox list and
 * an Outbox list, which is what a 5190 had. That is kept, byte for byte, as
 * CLASSIC. CHAT is a second front end over the same two tables: one row per
 * correspondent, and inside it the exchange as bubbles.
 *
 * ============ CLASSIC IS THE DEFAULT, AND WHY ============
 *
 * Not because it is better -- because it is what is already there. An
 * unparseable setting, a missing setting and a settings.prop that predates
 * this all have to mean "behave exactly as before", or upgrading a phone
 * silently redesigns an app on it. Switching is one row in Settings.
 *
 * ============ THREADING NEEDED A COLUMN THAT DID NOT EXIST ============
 *
 * The inbox has a `sender`. THE OUTBOX HAS NO RECIPIENT -- the Python's
 * schema is (id, message, timestamp) and _save_outbox_message() never had a
 * number to write, because the send flow asks for one and then throws it
 * away. So a sent message could not be attributed to a conversation at all.
 *
 * nd_msg_save_outbox_to() adds one, behind an ALTER TABLE that tolerates
 * having already run. Rows written before it exists have no recipient and
 * are gathered under ND_MSG_PEER_UNKNOWN rather than being hidden: a phone
 * that has been sending texts for a year should not look like it never has.
 */

/* settings.prop, read through nd_settings like every other app preference. */
#define ND_MSG_STYLE_SETTING "system.ui.messages_style"
#define ND_MSG_STYLE_DEFAULT "CLASSIC"

typedef enum { ND_MSG_STYLE_CLASSIC = 0, ND_MSG_STYLE_CHAT } nd_msg_style;

/* Case-insensitive, and tolerant in one direction only: anything that is not
 * recognisably "CHAT" is CLASSIC. NULL and "" included. */
nd_msg_style nd_msg_style_parse(const char *raw);

/* The setting, parsed. */
nd_msg_style nd_msg_style_current(void);

/* The two rows Settings shows, in this order, so index 0 is CLASSIC. */
#define ND_MSG_STYLE_ITEMS 2
extern const char *const nd_msg_style_options[ND_MSG_STYLE_ITEMS];

/* ------------------------------------------------------------------ *
 * The module constants, verbatim
 * ------------------------------------------------------------------ */

/* ROOT_ID_MESSAGES -- "matches \"2-1\" style header", says the Python. */
#define ND_MESSAGES_ROOT_ID 2

/* SMS_MAX_CHARS -- one GSM text-mode segment. Counted in CODE POINTS, not
 * bytes: len() in Python is a code-point count, so a 160-character message
 * with any non-ASCII in it must not be rejected for being 200 bytes. */
#define ND_MSG_SMS_MAX_CHARS 160

/* ARROW_KEYS -- any of the four opens the contact picker from the Send To
 * field. Up, Left, Right, Down, in the Python's tuple order. */
#define ND_MSG_ARROW_KEYS_N 4
extern const int32_t nd_msg_arrow_keys[ND_MSG_ARROW_KEYS_N];

/* The two lists the detail screen's Options branch shows, chosen by comparing
 * the TITLE STRING -- that is how the Python tells an inbox message from an
 * outbox one, and it is reproduced. */
/* WAS {"Just Erase for now"} and {"Erase", "Send"}. Both now offer Delete
 * and Forward, in both front ends -- the placeholder wording was the
 * Python's and it was never a feature. */
#define ND_MSG_INBOX_OPTIONS_N  2
#define ND_MSG_OUTBOX_OPTIONS_N 3
extern const char *const nd_msg_inbox_options[ND_MSG_INBOX_OPTIONS_N];
extern const char *const nd_msg_outbox_options[ND_MSG_OUTBOX_OPTIONS_N];

/* run()'s PagedList. */
#define ND_MSG_MENU_ITEMS 3
extern const char *const nd_msg_menu_items[ND_MSG_MENU_ITEMS];

/* ------------------------------------------------------------------ *
 * One row of either table (msg_db.c)
 * ------------------------------------------------------------------ *
 *
 * The Python passes the raw sqlite tuple around and indexes it positionally:
 * (id, message, sender, timestamp, is_read) for the inbox and
 * (id, message, timestamp) for the outbox. One struct covers both; an outbox
 * row leaves `sender` empty and `is_read` zero, which is exactly what the
 * outbox screens read.
 */

/* ND_TEXTLONG_CAP is 1024, so a composed message is at most 1023 bytes plus
 * its NUL and a row read back always round-trips. */
#define ND_MSG_TEXT_MAX 1024

/* A sender is a phone number or an alphanumeric originating address; 64 is
 * double the modem's own ND_MODEM_NUMBER_MAX. */
#define ND_MSG_SENDER_MAX 64

/* "* " + sender, for the unread marker the inbox list puts on a row. */
#define ND_MSG_LABEL_MAX (ND_MSG_SENDER_MAX + 2)

/* get_all_messages() is unbounded in Python; CODING-STANDARDS.md section 1.5
 * will not have an array sized by the database, so a list screen reads at
 * most this many rows and shows those. 128 rows is ~139 KB, taken from the
 * heap for the life of one list screen and released before it returns -- a
 * SIM holds about 30 SMS slots, so the cap is well above anything the modem
 * can deliver in one sweep. Recorded in OPEN-QUESTIONS.md as MSG-2. */
#define ND_MSG_LIST_MAX 128

typedef struct {
    int64_t id;
    char message[ND_MSG_TEXT_MAX];
    char sender[ND_MSG_SENDER_MAX]; /* "" for an outbox row */
    int64_t timestamp;
    int32_t is_read; /* 0 for an outbox row */

    /* NOT a port: who a SENT message went to. "" for an inbox row, and also
     * "" for an outbox row written before the column existed. The inbox's
     * `sender` is deliberately left empty for outbox rows even now, because
     * three existing tests assert that it is. */
    char recipient[ND_MSG_SENDER_MAX];
} nd_msg_rec;

/* A negative id is C's spelling of Python's `message_id is None`, which the
 * detail screen tests before it deletes. Real sqlite rowids start at 1. */
#define ND_MSG_NO_ID ((int64_t) - 1)

/* ------------------------------------------------------------------ *
 * The seven statements (msg_db.c)
 * ------------------------------------------------------------------ *
 *
 * Every one of them opens, runs one statement and closes -- nd_db.h's
 * connection policy, and the Python's. Every READ first asks whether the
 * database file exists and answers "nothing" when it does not, because
 * `os.path.exists(INBOX_DB)` is what the Python asks: a phone that has never
 * received a text must not have a zero-byte inbox created by looking at it.
 */

/* _fetch_inbox_messages():
 * SELECT id, message, sender, timestamp, is_read FROM inbox
 * ORDER BY timestamp DESC. Returns how many rows were written. */
size_t nd_msg_fetch_inbox(nd_msg_rec *out, size_t max);

/* _fetch_outbox_messages():
 * SELECT id, message, timestamp FROM outbox ORDER BY timestamp DESC. */
size_t nd_msg_fetch_outbox(nd_msg_rec *out, size_t max);

/* _fetch_inbox_message(id):
 * SELECT id, message, sender, timestamp, is_read FROM inbox WHERE id = ?
 * false when the row -- or the database -- is not there. */
bool nd_msg_fetch_inbox_one(int64_t id, nd_msg_rec *out);

/* _mark_read(id): UPDATE inbox SET is_read = 1 WHERE id = ?. Silent on every
 * failure, as the Python is. */
void nd_msg_mark_read(int64_t id);

/* _delete_inbox_message(id) / _delete_outbox_message(id). Also silent. */
void nd_msg_delete_inbox(int64_t id);
void nd_msg_delete_outbox(int64_t id);

/* _save_outbox_message(text): mkdir -p /NeoDCT/User/db, CREATE TABLE IF NOT
 * EXISTS outbox, then INSERT (message, timestamp) with timestamp = int(now).
 *
 * THE CREATE HERE HAS NO `PRAGMA journal_mode=WAL`, unlike the core's
 * init_databases(). On a phone whose outbox the core already made that is
 * invisible -- journal_mode lives in the file header -- but on one where it
 * did not, this app creates a rollback-journal outbox. Reproduced, not
 * fixed; nd_db.h documents the same asymmetry for the call log. */
nd_err nd_msg_save_outbox(const char *text);

/* The same, with a recipient -- which is what makes a sent message land in a
 * conversation. `recipient` NULL or "" behaves exactly like the call above.
 *
 * The column is added by an ALTER TABLE that is run every time and whose
 * "duplicate column name" is swallowed. sqlite has no ADD COLUMN IF NOT
 * EXISTS, and reading back the table's own schema to decide would be more
 * code doing the same thing. */
nd_err nd_msg_save_outbox_to(const char *text, const char *recipient);

/* ------------------------------------------------------------------ *
 * Conversations -- NOT a port
 * ------------------------------------------------------------------ */

/* A phone can hold more correspondents than a screen can list; both caps are
 * heap-allocated for the life of one screen. 64 threads is ~16 KB and 128
 * bubbles is ~136 KB. */
#define ND_MSG_THREADS_MAX 64
#define ND_MSG_BUBBLES_MAX 128

/* The preview is one line of a list row, not a message. */
#define ND_MSG_PREVIEW_MAX 96

/* Where an outbox row with no recipient goes. Shown as a thread rather than
 * dropped: a phone that has been sending texts since before the column
 * existed must not look like it never has. */
#define ND_MSG_PEER_UNKNOWN "(unknown)"

/* The thread key: `number` with everything except 0-9, * # and + removed, so
 * that "555-1234" and "5551234" are one conversation.
 *
 * It does NOT try to reconcile "+15551234" with "5551234". Deciding those are
 * the same needs a country code the phone has not been told, and guessing
 * wrong merges two people's conversations -- which is a worse failure than
 * showing one person twice. */
void nd_msg_peer_key(char *dst, size_t dst_sz, const char *number);

typedef struct {
    char peer[ND_MSG_SENDER_MAX];    /* the key, normalised */
    /* The number AS IT WAS LAST SEEN, punctuation and all. Two things need
     * it and neither wants the key: a reply is addressed to it, and it is
     * what a thread with no matching contact is labelled with -- "555-7777"
     * rather than "5557777", which is the same number spelled the way the
     * person who sent it spells it. */
    char number[ND_MSG_SENDER_MAX];
    char display[ND_MSG_SENDER_MAX]; /* the contact's name, or `number` */
    char preview[ND_MSG_PREVIEW_MAX];
    int64_t last_ts;
    int32_t unread;      /* unread INBOX rows in this thread */
    int32_t n_messages;  /* in and out together */
    bool last_outgoing;  /* whose message the preview is */
} nd_msg_thread;

/* One message in a thread. */
typedef struct {
    int64_t id;
    char text[ND_MSG_TEXT_MAX];
    int64_t timestamp;
    bool outgoing;
} nd_msg_bubble;

/* Every conversation, most recently active FIRST -- which is what a chat list
 * is. Returns how many rows were written.
 *
 * The display name comes from nd_contacts_lookup_name(); a number with no
 * contact shows as the number. */
size_t nd_msg_threads(nd_msg_thread *out, size_t max);

/* One conversation, oldest FIRST -- the opposite of the list, because that is
 * the order a transcript reads in. `peer` is a key from nd_msg_peer_key(), or
 * a raw number, which is normalised here. */
size_t nd_msg_thread_messages(const char *peer, nd_msg_bubble *out, size_t max);

/* Marks every unread inbox row in one thread as read. Opening a conversation
 * is reading it. */
void nd_msg_thread_mark_read(const char *peer);

/* ------------------------------------------------------------------ *
 * The app's own text wrapper (main.c)
 * ------------------------------------------------------------------ */

/* _wrap_text(ui, text, max_width, font) -- the FIFTH wrapper in the tree and
 * different from all four in nd_text.h and from PagedList's:
 *
 *   - splits on ANY whitespace with no empty tokens, so newlines vanish
 *   - an empty string gives exactly one empty line, never zero
 *   - a word too wide to fit alone is trimmed one CHARACTER at a time until
 *     the word plus "..." fits, and gets that "..." appended -- ALWAYS,
 *     including when it is the last word, which is where it differs from
 *     nd_pagedlist_wrap()
 *   - after such a word `current` is reset to "", so the next word starts a
 *     fresh line instead of joining the trimmed one
 *
 * Trimming drops whole UTF-8 code points, because Python trims characters.
 */
void nd_msg_wrap_text(nd_lines *out, nd_ui *ui, const char *text, int32_t max_width,
                      const nd_font *font);

/* _format_timestamp(ts): strftime("%Y-%m-%d %H:%M", localtime(ts)), except
 * that a FALSY timestamp -- which in Python is 0 as well as None -- gives
 * "Unknown time". */
void nd_msg_format_timestamp(int64_t ts, char *out, size_t out_sz);

/* len(text) as Python counts it: code points, not bytes. */
size_t nd_msg_codepoints(const char *s);

/* "".join(c for c in number if c in "0123456789*#+") */
void nd_msg_filter_number(char *dst, size_t dst_sz, const char *src);

/* ------------------------------------------------------------------ *
 * The screens (main.c)
 * ------------------------------------------------------------------ */

/* _show_empty_state(ui, title, root_id, sub_index, message). root_id is the
 * compound breadcrumb string ("2-1"); sub_index is ND_MSG_NO_SUB for the
 * Python's None. Clears the FULL screen, unlike most widgets, and returns
 * only when key 14 arrives -- nothing else exits it. */
#define ND_MSG_NO_SUB (-1)
void nd_msg_show_empty_state(nd_ui *ui, const char *title, const char *root_id, int32_t sub_index,
                             const char *message);

/* _draw_sending(ui, number). Clears rows 0..content_bottom, centres
 * "Sending..." and the number under it, blanks the softkey band with the
 * empty string, and presents. No key loop. */
void nd_msg_draw_sending(nd_ui *ui, const char *number);

/* ContactNumberInput(ui, title, prompt).show().
 *
 * A TextInput pinned to input_filter="numbers" -- the subclass adds nothing
 * but that argument and the arrow-key hook, so in C it is a constructor
 * argument and a loop, not a type. Any of the four arrow keys opens the
 * shared contact picker with title="Contacts", btn_text="OK",
 * header_root="2-3" and autofills the picked contact's number.
 *
 * `buf` is caller-owned and receives the text. Returns buf on confirm and
 * NULL on cancel, which is the Python's `return self.text` / `return None`. */
const char *nd_msg_number_input_show(nd_ui *ui, const char *title, const char *prompt, char *buf,
                                     size_t cap);

/* _send_message_flow(ui, text, root_id, sub_index) -> bool.
 *
 * root_id and sub_index are ACCEPTED AND NEVER READ, exactly as in the
 * Python. They are kept so the two files line up.
 *
 * true only when the network accepted the message. */
bool nd_msg_send_flow(nd_ui *ui, const char *text, int32_t root_id, int32_t sub_index);

/* The same flow with the recipient already known, which is what a reply from
 * inside a conversation has. `to` NULL or "" means ask, which is what the
 * ported call above does.
 *
 * ============ IT NOW RECORDS THE SENT MESSAGE ============
 *
 * The Python's _send_message_flow() writes NOTHING on success: a sent message
 * is not saved anywhere unless the user separately chooses Options -> Save,
 * so the Outbox only ever held drafts. That is a gap rather than a decision --
 * the screen is called "Outbox" and did not contain what had been sent -- and
 * a conversation cannot show your own half of it at all.
 *
 * So a successful send now writes the message to the outbox with its
 * recipient. Classic's Outbox gains the sent messages it always implied it
 * had; Chat gains the right-hand bubbles. Recorded in OPEN-QUESTIONS.md. */
bool nd_msg_send_flow_to(nd_ui *ui, const char *text, int32_t root_id, int32_t sub_index,
                         const char *to);

/* _show_message_detail(...)'s return: None, or the string "deleted" that
 * tells _show_inbox / _show_outbox to loop instead of returning. */
typedef enum { ND_MSG_DETAIL_BACK = 0, ND_MSG_DETAIL_DELETED } nd_msg_detail_result;

nd_msg_detail_result nd_msg_show_detail(nd_ui *ui, const char *title, const char *root_id,
                                        int32_t sub_index, const char *message, int64_t message_id,
                                        const char *sender, int64_t timestamp);

/* ------------------------------------------------------------------ *
 * The chat front end -- NOT a port
 * ------------------------------------------------------------------ */

/* The conversation list: "New Message" first, then one row per
 * correspondent, most recent first, each showing the name and a preview of
 * the last message. Returns when the user backs out. */
void nd_msg_show_threads(nd_ui *ui);

/* One conversation as bubbles, oldest at the top.
 *
 * Up and Down move a selection through the bubbles AND onto the message box
 * at the bottom, which is the one row that is not a message: choosing it
 * opens the composer already addressed to this correspondent. Choosing a
 * bubble offers Delete and Forward.
 *
 * `peer` is a key or a raw number; `display` is what the title bar shows and
 * may be NULL, in which case the number is used. */
void nd_msg_show_thread(nd_ui *ui, const char *peer, const char *display);

/* _show_inbox(ui, 2, 1) and _show_outbox(ui, 2, 2). */
void nd_msg_show_inbox(nd_ui *ui, int32_t root_id, int32_t sub_index);
void nd_msg_show_outbox(nd_ui *ui, int32_t root_id, int32_t sub_index);

/* _show_write_message(ui, 2, 3) -- the composer. */
void nd_msg_show_write(nd_ui *ui, int32_t root_id, int32_t sub_index);

/* The composer, opened with something already in it. NOT a port.
 *
 * `body` prefills the text -- which is what Forward is: the message you chose,
 * in a new composer, with the cursor after it.
 *
 * `to` prefills the RECIPIENT, so a reply from inside a conversation does not
 * ask for a number you have already named by opening the thread. NULL for
 * either means "ask", which is what the ported call does for both.
 *
 * Returns true when a message was actually sent, so a thread view knows
 * whether to reload itself. */
bool nd_msg_show_write_prefill(nd_ui *ui, int32_t root_id, int32_t sub_index, const char *body,
                               const char *to);

#ifdef __cplusplus
}
#endif

#endif /* ND_MESSAGES_H_INCLUDED */
