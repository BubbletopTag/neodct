# The widget vocabulary

`neodct/src/include/nd_widgets.h` is the real reference and it is
exceptionally well commented -- read it. This is the map: which widget for
which job, and the traps that are not obvious from the signature.

## Choosing

| you want | use | notes |
| --- | --- | --- |
| an app's front menu, one item per screen, big type | `nd_pagedlist` | what a 5190 puts here; `set_values()` gives each row a second line showing what it is set to |
| an ordinary three-row menu | `nd_vlist` | the most-drawn screen after home; digits 1-9 are shortcuts |
| a list of "Level 1..N" | `nd_levelsel` | a `nd_vlist` with an OK softkey |
| the main menu, one big icon | `nd_appsel` | core only |
| a yes/no or a notice | `nd_msgdialog` | Enter accepts, C cancels, by default |
| a one-line field | `nd_textinput` | `set_mask()` for `##:##` style entry |
| a message composer | `nd_textlong` | no `show()`; you own the loop |
| a scrolling help page | `nd_scroller` | |
| a centred label with a big number | `nd_infoscreen` | |
| a step name, bar and percentage | `nd_progress` | redraws only when the percentage changes |
| picture, title, rule, scrolling body | `nd_detailpage` | the only widget that allocates; `free()` it |
| the bottom strip | `nd_softkey` | see the transparency rule below |
| the "5-1" breadcrumb | `nd_header` | |

## Four rules that decide whether it looks right

Quoted from `nd_widgets.h` because they are easy to get wrong and invisible
until someone looks at the panel:

1. **Partial clears are load-bearing.** Most widgets clear rows
   `0..content_bottom` only, so a caller's `nd_softkey_update(..., false)`
   survives into the frame. `MessageDialog` and `PagedList` clear the full
   height instead. Getting either wrong loses or double-draws the softkey.
2. **Text is measured by its ink.** `nd_text_size()` returns the ink box of
   that specific string, so centring visibly shifts depending on the letters.
   `"_"` is 3 px tall at 20 px; `"Ag"` is 21. Not a bug.
3. **Scrollbar notches truncate.** Compute in `double`, cast with
   `nd_trunc32()`, never `round()`.
4. **Six widgets build their own softkey bar inside `draw()`** and three then
   present a second time. The two-stage repaint is visible on the panel and is
   intentional.

## The softkey transparency trick

Exactly one bar in the whole system is transparent: the core's own, built
during `nd_ui_init` step 9, which is why the home screen's strip shows the
wallpaper through it. Every bar an app builds passes `false`. Getting it
backwards makes the home screen a black band, or every app's strip transparent
over stale pixels.

    nd_softkey_init(&bar, ui, false);   /* an app: always false */

## The order callers depend on

    nd_softkey_update(&bar, "Select", false);   /* paint, do not present */
    nd_vlist_show(&list);                       /* its draw presents both */

One frame instead of two. Change either half and the softkey vanishes or
double-draws.

## Lists with dynamic rows

`nd_vlist` takes `const char *const *` and **borrows** the strings -- they must
outlive the widget. Build them into a caller-owned array first. `nd_vlist` does
not fit the text, so a long row runs under the scrollbar; ellipsize each row to
about 215 px (x=10 to the selection bar's right edge at 225) with
`nd_text_ellipsize` before handing them over.

For a screen whose row count comes from a database, heap-allocate the whole
block for the life of the screen and free it before returning --
`CODING-STANDARDS.md` 1.5 will not have an array sized by input on the stack.
`apps/Messages` and `apps/Calendar`'s `show_day()` both do this.

## Masked fields

`nd_textinput_set_mask(&f, "##:##")` turns a field into fixed-shape numeric
entry: every `#` is a slot, everything else is a literal the field emits by
itself as soon as the slot before it is filled. A masked field **bypasses T9
entirely**.

The trap: a masked field that is already full ignores every keypress until
something is deleted. So do **not** prefill one with the current value -- it
costs four presses of C before the first useful press. Name the current value
in the prompt instead (`"Time (was 09:00):"`). `apps/Clock` documents the
reasoning and `apps/Calendar` follows it.

## Blocking widgets and SIGTERM

Every `show()` blocks in its own key loop and does **not** poll
`nd_app_should_exit()`. That is the framework's existing behaviour, and the
core's SIGKILL escalation is the backstop. But the screen your app *sits* on
when nothing is happening is the screen an incoming call arrives at, so for
that one screen prefer your own loop:

    for (;;) {
        int32_t key = nd_ui_read_keypress(ui, 0.1);
        if (nd_app_should_exit()) return;
        if (key == ND_KEY_NONE) continue;
        ...
    }

`nd_cal_month_show()` does this and says why in a comment.

## Widgets drain the channel

`MessageDialog` and `PagedList` flush pending input before their first draw, so
a queued keypress is eaten. In tests this means `sa_send()` does nothing for
them and you need `sa_hold()`, which presses a key without releasing it and
lets the synthesised repeat arrive after the screen is up.
