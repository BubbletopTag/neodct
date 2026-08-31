# Adding a notification kind

## What already exists

NotifyService (`lib/nd_notify.c`, `include/nd_notify.h`) owns two jobs that
share the sound card: the home-screen banner and the ringer. The banner is
four fields and no I/O: a `kind` string, a `count`, a `latest` rowid, and
whatever the kind needs beyond that.

The visible idiom -- and there is deliberately only one -- is: two lines
mid-left on the home screen, the carrier line hidden behind them, the softkey
becoming a verb, C dismissing the banner while the underlying thing stays.
Reuse it. Inventing a second "something happened" look makes the first one
mean less, and the phone has one 240x175 screen to say it on.

Two kinds ship: `ND_NOTIFY_KIND_SMS` and `ND_NOTIFY_KIND_EVENT`.

## Why the core has to poll

Your app is not running when the thing happens -- that is the entire point of a
notification. There is nobody to send a message, and the core is the only
process still alive and the only one holding the NotifyService handle
(`nd_app.h`: an app's `ui->notify` is NULL). So the core reads your store.

That is why a notifying feature's data layer belongs in `neodct/src/lib/` and
not inside the app.

## The five edits

Worked example: the calendar reminder, `git log` for `nd_calendar`.

**1. A kind and a poster** in `nd_notify.h`:

    #define ND_NOTIFY_KIND_EVENT "event"
    void nd_notify_post_event(nd_notify *n, int64_t row_id, const char *title,
                              int64_t when, bool tone);

**2. State and wording** in `nd_notify.c`. A text's banner is derived entirely
from `count`, so it needed no storage; anything that names itself needs a
buffer that survives from the poll that found it to the frame that draws it.
Truncate to `ND_NOTIFY_LINE_MAX` on the way in so nothing downstream has to
know the limit.

Extend `nd_notify_banner_lines()` with a branch for your kind. Keep the
existing shape: one thing names itself, several are counted.

    sms,   one    "1 message"    / "received"
    sms,   many   "3 messages"   / "received"
    event, one    "Dentist"      / "10:30 am"
    event, many   "3 reminders"  / "due"

**3. Decide what happens when two kinds are live.** The shipped answer is one
banner, newest wins, count restarts -- a `take_over()` helper that resets when
the kind changes. Nothing is lost by it: the text is still unread with the
envelope flashing, and the appointment is still in the calendar. Two counters
and a home screen saying two things in two lines is the alternative, and it
does not fit.

**4. A tick in `lib/nd_ui.c`**, beside `battery_tick` and `modem_tick`, called
from `nd_ui_read_keypress`. Rate-limit it at the call site with
`nd_time_monotonic()` -- the expensive part is opening sqlite and nothing else
wants the answer. Guard on `ui->notify == NULL` so it is a no-op in an app
process. A phone with no data should pay one `stat()` per poll, no more.

**5. Dispatch in `open_notification()`** so the softkey opens your app. Follow
the Messages shape: the *one* thing when `count == 1`, the list otherwise --
that is what the extra entry point in `nd_app.h` is for. Add the entry name and
symbol there, and a branch in `apprun/nd_apprun.c`.

## Things that bite

- **The softkey verb.** "Read" is what you do to a message, "View" to an
  appointment. One word each; the strip has room for one.
- **The envelope belongs to the inbox.** It was drawn whenever *any* banner was
  up, which was the same thing while a text was the only kind. Gate it on the
  SMS kind or an appointment gets an envelope over it.
- **Fit the banner lines.** `"10 messages"` is 130 px at 20 px and could never
  reach the signal meter at x=210, which is why the original drew them
  unmeasured. A name can: `"Parents evening at school"` runs straight through
  the bars. `nd_text_ellipsize` to `width - 30 - 34`.
- **There is one chirp.** `/NeoDCT/System/tones/sms.wav` is the only short tone
  the image ships. Share it and say so in a comment, or add an asset.
- **`nd_ui.c` reaches NotifyService through weak symbols.** Add
  `#pragma weak` for anything new you call from there, matching the file's
  convention.

## Testing it

The banner state machine is four fields and no I/O, so it tests directly --
`test/unit/test_notify.c` needs no sound card. Worth pinning: the wording for
one and for many, that an untitled thing still says something, that dismiss
clears your fields (or a later notification inherits a stale name), and the
kind-takeover decision in both directions. The last one is the one a future
change is most likely to break silently.
