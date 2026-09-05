# NeoDCT Bible — app id 13

> **In this tree it ships as a `.nap` package, not as a stock app.** The
> Luckfox build is `neodct/packages/Bible-luckfox-armv7.nap`, made with
>
>     neodct/tools/mknap.py --app-dir Bible --so luckfox-armv7=luckfox-armv7/app.so \
>         -o ../../packages/Bible-luckfox-armv7.nap
>
> from this directory. Add `--so qemu-aarch64=qemu-aarch64/app.so` for a
> package either phone can install. Copy the `.nap` onto a memory card and
> install it from **Settings → Install apps**; `docs/NAP-PACKAGES.md` has the
> format. Everything below about `install.sh`, the overlay and the golden
> frames describes the *stock-app* route, which this tree does not take --
> the app is left out of `neodct/overlay/` and out of `neodct/src/apps/` on
> purpose, so no frame moves and the menu count does not change.

The entire Holy Bible on a 240×175 screen with 64 MB of RAM, in C, for the
`c-rewrite` branch. 81 books, 1,402 chapters, 38,073 verses, in 1.70 MB.

Nothing here is committed. It builds clean under the tree's own
`-Werror -Wconversion -Wshadow` and `make test` stays green.

---

## What you install

| file | what it is |
|---|---|
| `Bible/manifest.json` | app id 13, name "Bible" |
| `Bible/icon.png` | 120×120 line-art open book, matching the other icons |
| `Bible/web.ndb` | the text — 1.70 MB, all 81 books |
| `qemu-aarch64/app.so` | 43 KB, aarch64 musl, cortex-a53 |
| `luckfox-armv7/app.so` | 26 KB, armv7 musl, cortex-a7 + NEON-VFPv4 + Thumb-2, hard float |

`./install.sh qemu-aarch64 /path/to/neodct/neodct/overlay`, then rebuild.

`/` is a read-only squashfs under dm-verity, so the app cannot be copied onto
a live phone — it has to arrive with an image or an `.ndsw`. The **pack** can,
though: `./install.sh --user /mnt/NDUSER` copies just `web.ndb` onto the user
partition, which is where the app looks first.

---

## About the translation

**The Message is copyrighted** (NavPress, Eugene Peterson), so I can't ship it
and wouldn't want to hand you a phone image that quietly contains someone's
licensed text. What ships is the **World English Bible** — genuinely public
domain, modern English, reads about as easily as the MSG does. It is the
closest thing to "reading for fun" that can live inside an image with no
strings attached.

If you own a copy of the MSG in any verse-per-line export, `tools/mkbible.py`
turns it into a pack:

```sh
tools/mkbible.py msg_vpl.txt msg.ndb --name MSG --strict
./install.sh --user /mnt/NDUSER      # or just drop msg.ndb in /NeoDCT/User/Bible
```

Two packs on the device means a **Translation** entry appears in the menu.

### GEN Z mode

Not a translation and labelled as one nowhere. It is a word-substitution
filter applied to the WEB text *at draw time* — 72 rules, longest phrase
first, capitalisation carried across, plus an interjection and a tag chosen
from the verse's own book/chapter/verse so a verse reads the same every time
you scroll past it. Nothing is rewritten on disk.

```
Gen 1:31   yo, God saw everything that he had cooked up, and, yo peep this,
           it was hella bussin. There was evening and there was morning,
           a sixth day. fr.

John 3:2   bruh, He pulled up to Jesus by night and hit up bro like,
           "Rabbi, we know that you are a teacher straight outta God..."

John 3:16  not gonna lie, For God so loved the world, that he gave his only
           born Son, that whoever believes in him should not get cooked,
           but have eternal life. no cap.

Matt 5:3   not gonna lie, "Goated are the poor in spirit, for theirs is
           the Kingdom of Heaven.

Job 1:21   ok so He was like, "Naked I pulled up out of my mother's womb...
           Yahweh gave, and Yahweh has taken away. Goated be Yahweh's name."
```


The WEB's terms ask exactly one thing of anyone who changes the text: don't go
on calling the result the World English Bible. So while the mode is on, the
badge in the top-right reads **GEN Z** and never **WEB**. Yahweh, God, Jesus,
Christ, Lord and Spirit are not in the substitution table and won't be — the
joke is the register, not the subject.

---

## Using it

**Main menu** — Continue, Books, Go to…, Search, Random verse, Bookmarks,
Style (WEB ⇄ GEN Z), Translation (when there is more than one pack), About.

**Reader keys**

| key | does |
|---|---|
| up / down, 2 / 8 | scroll a line |
| 0 | page down, with one line of overlap |
| left / right, 4 / 6, `*` / `#` | previous / next chapter |
| 5 | jump to a verse |
| navi | Options — bookmark, go to verse, next/previous chapter, switch style |
| clear | back |

**Go to…** takes `John 3:16`, `1 Cor 13`, `gen 2`, `Joh3:16`, or a bare book
name. **Search** scans all 1,402 chapters, stops at 60 hits, and clear aborts
it mid-scan.

Reading position, bookmarks and the style live in
`/NeoDCT/User/Bible/state.prop`, written the moment they change — not on exit,
because `app_shutdown()` runs from the SIGTERM path and an incoming call
during Judges shouldn't cost you your place.

---

## How the pack works

Per-chapter zlib blocks behind a fixed index, so opening John 3 is one seek,
one ~2 KB read and one inflate. Resident cost is **69 KB, flat**, whatever you
open:

```
index       18 KB   book table + chapter table + name pool, read once
dictionary  32 KB   the preset zlib window, read once
raw         18 KB   max_raw + 1, the inflate target, reused
verses       1 KB   offsets into raw
```

Nothing allocates per chapter — every buffer is sized at `open()` from a
number the packer stamped in the header.

**The preset dictionary** is the one non-obvious part. 1,402 chapters
compressed separately means 1,402 compressors that each start with an empty
32 KB window and none of which knows "and he said to him" has already occurred
four hundred times. Priming them all with the same block of common phrasing
costs 32 KB in the file and one `inflateSetDictionary()` per chapter, and
measured on this text: **2,026,131 bytes of chapter blob without it,
1,727,653 with** — 14.7%. After paying the 32 KB the block itself costs, the
pack goes from 2,045,054 bytes to 1,779,344 — **259 KB back** on a part with
128 MB of NAND.

`mkbible.py` builds the block by scoring every 2-to-6-word phrase by
`(len-3) × (count-1)` and filling 32 KB best-first. Best-*last* was tried too
— same phrases, same scoring, highest value nearest the data — on the theory
that zlib spends fewer bits on a nearer match. It came out **7.5% bigger**.
The measurement decided it, not the theory.

The packer also folds non-ASCII, because `font.ttf` is 16 KB and covers
printable ASCII — the curly quotes and em dashes in the WEB text would
otherwise render as blank boxes of advance width. `--strict` refuses instead
of folding.

---

## Building

```sh
cd neodct/src && make && make test          # host
tools/build-bible-cross.sh                  # both targets, seconds not hours
```

The cross script does **not** build Buildroot. `app.so` doesn't contain
libneodct, it references it, so linking only needs something that exports the
right symbols — the script reads the host `libneodct.so`'s dynamic symbol
table and generates a stub from it. Two things fall out of that: the link
*verifies* every symbol the app needs actually exists in the real library, and
the `.so` gets correct `DT_NEEDED` entries. It also reuses `src/Makefile` with
`CC`/`PKG_CONFIG` overridden, so the warning flags and RPATH can't drift.

Toolchains from https://musl.cc/ (`aarch64-linux-musl-cross`,
`armv7l-linux-musleabihf-cross`), or point `AARCH64_CC`/`ARMV7_CC` at
Buildroot's own once you have them.

`apps/Bible` is the only app this trick works for — it's the only one whose
includes reach nothing outside libc and zlib.

---

## Two things to know before you commit this

**1. It's a 14th stock app, and that moves the menu.** The AppSelector
scrollbar notch is positioned by `(track_height) / (n_apps - 1)`, so adding an
app shifts it in every menu screenshot, and any app sorting after id 13 gets a
new breadcrumb number. Five golden frames stop matching:

| frame | differs by |
|---|---|
| `menu-settings` | 10 px, scrollbar only |
| `menu-calculator` | 10 px, scrollbar only |
| `menu-koki-mobile` | 10 px, scrollbar only |
| `menu-browser` | 20 px, scrollbar only |
| `menu-music` | 96 px — scrollbar, **and the breadcrumb reads 13 instead of 12** |

`menu-phone-book`, `menu-messages` and `menu-games` are byte-identical.

Those frames were captured from the Python, which had no Bible app, so they
can't be satisfied while a 23rd app exists — re-cutting them from C output
would make the C its own oracle for those five. That's your call, not mine, so
**I left the app out of `neodct/overlay/` entirely** and the tree is green as
it stands. When you want it in an image, `install.sh` puts it there and these
counts need bumping:

```
test/unit/test_appsel.c:399   stock 13 -> 14
test/unit/test_appsel.c:306   add {13, "Bible", "/NeoDCT/System/apps/Bible"} to EXPECTED
test/unit/test_appreg.c:712   stock 13 -> 14
test/unit/test_appreg.c:761   22 -> 23 apps with engineering mode on
test/unit/test_appreg.c:660   notch top 89 -> 87   (step becomes 99/22 = 4.5 exactly)
```

If you'd rather leave the frames alone: give it an id **above 971** (say 972)
and only the scrollbar moves — no breadcrumb changes — which drops
`menu-music` from 96 px to 10.

**2. `web.ndb` is 1.7 MB of generated data.** Probably not something you want
in git. `mkbible.py` rebuilds it in 13 seconds from the eBible.org verse-per-
line file (`https://ebible.org/Scriptures/eng-web_vpl.zip`).

---

## Tests

`test/unit/test_bible.c` — 130 checks. It builds `.ndb` packs in C (with and
without a preset dictionary, so the `Z_NEED_DICT` path is actually exercised),
refuses five corrupted ones, checks the Gen Z table is still ordered
longest-first (get that wrong and a phrase silently stops matching — no crash,
no diagnostic), and drives the reader against a real framebuffer in both
styles.

It runs against the real pack too when it can find one:

```sh
NEODCT_BIBLE_PACK=/path/to/web.ndb make test
```

which adds Genesis 1:1, John 3:16, Psalm 119's 176 verses, and a sweep proving
all of Genesis is printable ASCII. Without a pack that block skips and says
where it looked.
