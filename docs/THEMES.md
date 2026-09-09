# Themes -- changing what the phone looks like

From 0.6.0a the interface is **data**. The colours, the typeface, the icons
and the status sprites are read at startup rather than compiled in, so a look
is something an owner installs from the memory card in the same way they
install an app -- and something a person can write in an afternoon with a
text editor and a folder of PNGs.

The owner's side of it is one screen: **Settings → Theme**. `*` and `#` turn
the pages, each page is drawn *in the theme it is offering*, and NaviKey
applies the one on screen. Backing out puts the previous look back.

## A theme is a directory

```
HelloKitty/
  theme.json          the palette, and the theme's name -- the only required file
  fonts/ui.ttf        optional, replaces the UI typeface
  fonts/ui-bold.ttf   optional
  icons/<App>.png     optional, one per app DIRECTORY name: Messages.png, Clock.png
  img/...             optional, mirrors ui/resources/img -- battery, signal, envelope
  wallpaper.jpg       optional
  preview.png         optional, what the picker and the installer show
```

**Everything except `theme.json` is optional, and a missing part means "keep
what the system has".** That is what makes a theme small: a recolour is nine
lines of JSON and nothing else. It still themes an app that was installed
after the theme was written, because the lookup happens when a file is opened
and not when the theme is installed.

Themes live at `/NeoDCT/System/themes/` (shipped with the image) and
`/NeoDCT/User/sdcard/themes/` (installed by the owner). A theme is never
executed: it is JSON, pictures and a font.

## theme.json

Every key is optional except `id`. Anything the file does not mention keeps
its built-in value, so this is a complete, legal theme:

```json
{
  "id": "rose",
  "name": "Rose",
  "palette": { "blue_top": "#ED538E", "blue_bot": "#B12B5F" }
}
```

`id` is what the phone stores when the theme is chosen, so it has to be
stable: renaming it orphans the owner's choice. `name` is what the picker
shows and falls back to the id.

### The palette

Colours are `#RRGGBB` and nothing else. The names are the ones the framework
draws with, so they describe a ROLE rather than a hue -- `blue_*` is the
signature colour even in a theme with no blue in it.

| key | what it paints |
| --- | --- |
| `blue_hi` `blue_top` `blue_mid` `blue_bot` | the signature plate: title bars, the selected row, the softkey |
| `blue_deep` | the 1 px dark cut around every plate and divider |
| `glass_top` `glass_bot` | the frosted panel content sits on |
| `chrome_hi` `chrome_top` `chrome_bot` | bezels, the bevel hairline, the scrollbar track |
| `sky_top` `sky_bot` | the background gradient behind everything |
| `green_top` `green_bot` | a confirmation, and a healthy battery |
| `amber_top` `amber_bot` `red_top` `red_bot` | a warning and a fault |
| `ink_dark` `ink_light` `ink_muted` | type on a light plate, on a dark one, and secondary |
| `text_shadow` | what sits under light type -- see below |
| `text_sheen` | the highlight under dark type, the letterpress effect |
| `scrim_ink` | the readability wash laid over a wallpaper |

`text_shadow` and `scrim_ink` are worth setting even in a small theme. Both
are navy in the stock look because navy darkens a blue interface without
desaturating it, and navy under pink type reads as a bruise.

### Alphas

```json
"alpha": { "scrim_top": 96, "scrim_bot": 0, "appsel_scrim": 70, "shadow": 150, "sheen": 110 }
```

`0`-`255`, clamped rather than refused. `scrim_top` is how hard the wash
leans on the wallpaper behind text: raise it for a bright wallpaper, and
leave `scrim_bot` at 0 or the softkey strip shows a step where the scrim
stopped. `sheen` is the white gloss filling the top half of every plate --
`0` turns the glass look off entirely, which is how a flat theme is made.

## Packaging one

A theme ships as a `.nap`, the same package format as an app. The manifest
says which it is:

```json
{
 "name": "Hello Kitty",
 "type": "theme",
 "icon": "icon.png",
 "version": "1.0",
 "author": "...",
 "description": "Shown on the install screen."
}
```

**`"type"` absent means `"app"`**, so every package built before this field
existed installs exactly as it did. A theme package carries no `app.so` and
no `arch` -- it is the same file on every phone -- and one that *does* carry
program code is refused.

```sh
neodct/tools/mknap.py --app-dir HelloKitty/ -o HelloKitty.nap   # no --so
neodct/tools/mknap.py --list HelloKitty.nap
```

Copy the `.nap` onto the card and install it from **Settings → Install apps**,
which is also where themes arrive. It unpacks into
`/NeoDCT/User/sdcard/themes/<Name>/` and appears in the picker immediately --
no restart, unlike an app.

### The pictures

`preview.png` and `icon.png` are generated, not drawn by hand:

```sh
neodct/tools/mkthemeart.py neodct/contrib/themes/HelloKitty
```

That tool stages the theme, runs `nd-shoot` with it selected, and keeps a real
frame. The preview is therefore a genuine rendering of the theme by the real
framework rather than a mock-up that can drift from it.

## Worked example

`neodct/contrib/themes/FruitigerAero/` is the stock look written out as a
theme file. Every value in it is the built-in one, which makes it both the
reference for the format and a regression test: installed, it renders frames
byte-identical to the compiled-in default. Copy it and change colours.

## For the curious: how it works

`nd_theme.h` used to hold twenty-six `#define`d colours, so the look was a
property of the binary. They are now fields of a struct the UI loads at
startup -- but the *names* did not change, so `ND_TH_BLUE_TOP` still spells
the same thing at all twenty-seven call sites across the OS and now reads the
active palette instead of a folded-in literal. Nothing had to be touched to
make the whole interface themeable.

Applying a theme replaces the palette of the running process, which is why
the picker can preview a look by simply wearing it and repainting. Other
processes pick the change up when they next start; the setting
(`system.ui.theme`) is the single source of truth.

`docs/NAP-PACKAGES.md` covers the package format, `nd_theme.h` the C contract,
and `nd_themeload.c` the loader.
