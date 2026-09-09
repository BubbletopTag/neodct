# Themes -- changing what the phone looks like

From 0.6.0a the interface is **data**. The colours, the typeface, the icons,
the status sprites *and the decoration itself* are read at startup rather than
compiled in, so a look is something an owner installs from the memory card in
the same way they install an app -- and something a person can write in an
afternoon with a text editor and a folder of PNGs.

The phone ships one look, **Classic**: white type on black in the pixel
typeface, flat and square-cornered. It is the built-in, it needs no files, and
it is what an owner gets with nothing installed. The glossy **Frutiger Aero**
look is a theme (`neodct/contrib/themes/FruitigerAero`) -- which is the proof
that the system is worth having, because those two share no colour, no
typeface, no icon and no drawing style, and the same widgets draw both.

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
| `blue_hi` `blue_top` `blue_mid` `blue_bot` | the signature colour: the selected row, a progress fill, an accent |
| `bar_top` `bar_bot` `bar_ink` | the title bar and the softkey strip, and the type on them |
| `sel_ink` | type standing on the selection -- white over glass, black over an inverted row |
| `warn_ink` | a warning in type: the home screen's "Eng. Mode" line |
| `green_*` `amber_*` `red_*` | a confirmation, a warning and a fault -- and Snake's pieces |
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

`text_shadow` and `scrim_ink` are worth setting even in a small theme.

**`ink_light` is "type over the background", not "white type".** A theme with
a light background should make it dark and set `text_shadow` to a pale halo --
white type on a pale ground is the one mistake that makes a pretty palette
unreadable, and it is what the Hello Kitty theme does the other way round from
Frutiger Aero. The bars and the selection have their own ink, so they keep
white type either way.

### Structure

A palette makes the interface pink. It cannot make it *flat* -- recolouring a
glossy plate to black leaves a glossy black plate -- so a theme also carries
switches for the decoration:

```json
"style": {
  "gloss": true, "bevel": true, "gradients": true, "round": true,
  "type_shadow": true, "plate_shadow": true, "bevel_divider": true,
  "icon_glow": true, "reflection": true, "scrim": true,
  "pixel_font": false, "wallpaper_dim": 88, "app_wallpaper_dim": 68
}
```

**Every switch defaults to off**, and that direction is deliberate: a theme
file that says nothing gets the plain, cheap, legible look rather than
inheriting somebody else's gloss.

| key | what it turns on |
| --- | --- |
| `gloss` | the white sheen filling a plate's top half |
| `bevel` | the white hairline just inside a top edge |
| `gradients` | off collapses every ramp to its top colour |
| `round` | off squares every corner, whatever radius a widget asked for |
| `type_shadow` | the shadow under light type, the sheen under dark |
| `plate_shadow` | the soft band a plate casts onto what is below it |
| `bevel_divider` | a dark rule plus a white one, instead of a single line |
| `icon_glow` | the radial glow behind the app selector's icon |
| `reflection` | the icon standing on a glossy floor |
| `scrim` | the readability wash laid over a wallpaper |
| `pixel_font` | draw with the pixel face rather than the UI face |
| `game_colour` | colour in the games -- see below |
| `wallpaper_dim` | 0-100, how far the wallpaper is dimmed on the home screen |
| `app_wallpaper_dim` | the same inside an app, where there is more to read |

The two dims move **with** `scrim`. A theme that scrims can leave the picture
bright, because it darkens only the rows that carry type; a theme that does
not has to dim the whole picture or its white text is unreadable over a bright
one. `system.ui.wpeverywhere_dim` still overrides `app_wallpaper_dim`, so an
owner who has tuned it keeps their value across a theme change.

A theme that ships `fonts/ui.ttf` uses it whatever `pixel_font` says -- "the
pixel face unless I brought my own" needs no third setting.

`game_colour` is a switch rather than a palette entry because monochrome and
colour distinguish things differently, not just prettily. Snake's food and its
body are told apart by SHAPE when there is one ink -- an outlined cell against
filled ones, which is how the phone has always drawn it -- and by hue when
there is a palette to spend. Given one ink, a filled apple and a filled snake
are the same square, so no set of colour names can express the choice. Off, the
game is pixel-for-pixel the one the phone shipped; on, the field takes the
theme's `blue_deep` and the pieces go red and green.

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

`neodct/contrib/themes/FruitigerAero/` is the whole glass look as a theme
file: palette, structure, icons, status sprites, typeface and wallpaper. It is
both the reference for the format and a regression test -- installed, it
renders the frames the phone rendered when that look was compiled in, and the
suite checks it. Copy it and change colours.

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
