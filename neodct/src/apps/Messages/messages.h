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
#define ND_MSG_INBOX_OPTIONS_N  1
#define ND_MSG_OUTBOX_OPTIONS_N 2
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

/* _show_message_detail(...)'s return: None, or the string "deleted" that
 * tells _show_inbox / _show_outbox to loop instead of returning. */
typedef enum { ND_MSG_DETAIL_BACK = 0, ND_MSG_DETAIL_DELETED } nd_msg_detail_result;

nd_msg_detail_result nd_msg_show_detail(nd_ui *ui, const char *title, const char *root_id,
                                        int32_t sub_index, const char *message, int64_t message_id,
                                        const char *sender, int64_t timestamp);

/* _show_inbox(ui, 2, 1) and _show_outbox(ui, 2, 2). */
void nd_msg_show_inbox(nd_ui *ui, int32_t root_id, int32_t sub_index);
void nd_msg_show_outbox(nd_ui *ui, int32_t root_id, int32_t sub_index);

/* _show_write_message(ui, 2, 3) -- the composer. */
void nd_msg_show_write(nd_ui *ui, int32_t root_id, int32_t sub_index);

#ifdef __cplusplus
}
#endif

#endif /* ND_MESSAGES_H_INCLUDED */
