# Themes -- changing what the phone looks like

From 0.6.0a the interface is **data**. The colours, the typeface, the icons,
the status sprites *and the decoration itself* are read at startup rather than
compiled in, so a look is something an owner installs from the memory card in
the same way they install an app -- and something a person can write in an
afternoon with a text editor and a folder of PNGs.

The phone ships three looks. **Classic** is the built-in: white type on black
in the pixel typeface, flat and square-cornered. It needs no files, and it is
what an owner gets on first boot and whenever a chosen theme cannot be found.
Beside it the image carries two themes in `/NeoDCT/System/themes/`
(`neodct/overlay/NeoDCT/System/themes/` in the tree):

- **Frutiger Aero** (`FrutigerAero/`) -- glass and gradients in deep sky blue,
  the look the phone briefly had compiled in. It is the proof that the system
  is worth having: it and Classic share no colour, no typeface, no icon and no
  drawing style, and the same widgets draw both.
- **Blossom** (`Blossom/`) -- the same glossy construction in rose pink, with
  a rounded typeface, a pink icon set derived from Aero's, and a polka dot
  wallpaper. It replaces the Hello Kitty theme that used to be an optional
  package, without the character art.

They used to live in `neodct/contrib/themes/` and reach a phone only as `.nap`
packages. They are part of the image now: they are there from the first boot,
an update keeps them current, and -- the reason that matters most -- an app
confined as `ndusr_ut` can read them, which it cannot do for a theme on the
card (see "How it works" below).

The owner's side of it is one screen: **Settings → Theme**. `*` and `#` turn
the pages, each page is drawn *in the theme it is offering*, and NaviKey
applies the one on screen. Backing out puts the previous look back.

## A theme is a directory

```
Blossom/
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

**`ink_light` is "type over the background", not "white type".** It stands on
whatever the background is -- the theme's own sky, the theme's wallpaper, or a
photograph the owner picked afterwards -- so it has to survive all three. Keep
it LIGHT and give it a dark `text_shadow` and a `scrim`: every wallpaper dim
in the framework darkens the picture, so light type gets more legible as the
background gets busier, and dark type gets less. The first pink theme made
`ink_light` charcoal to suit its pale polka dots and was unreadable the moment
the owner chose a dark photograph; Blossom keeps white type with a plum
shadow, deepens its own wallpaper to suit, and puts its dark ink where the
ground is known -- on its light glass panels (`ink_dark`). The bars and the
selection have their own inks (`bar_ink`, `sel_ink`), so they keep white type
either way.

### Bars and panels that are the background

A flat theme says "the title strip is just the background with type on it" by
giving `bar_top`/`bar_bot` the same colours as `sky_top`/`sky_bot`, and "a text
field is a hollow rule" by doing the same with `glass_*`. When the two match
(to within a few levels per channel) the framework does not paint that surface
at all -- it leaves whatever is behind it, which over a wallpaper is the
picture. Classic relies on this: its title, softkey, dialogs and fields are
type and rules standing on the wallpaper, not black boxes pasted over it. Give
the bars a colour of their own and they are painted as plates, as in Aero and
Blossom.

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

A theme that is not in the image ships as a `.nap`, the same package format
as an app. The manifest says which it is:

```json
{
 "name": "Blossom",
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
neodct/tools/mknap.py --app-dir Blossom/ -o Blossom.nap   # no --so
neodct/tools/mknap.py --list Blossom.nap
```

Copy the `.nap` onto the card and install it from **Settings → Install apps**,
which is also where themes arrive. It unpacks into
`/NeoDCT/User/sdcard/themes/<Name>/` and appears in the picker immediately --
no restart, unlike an app. A theme on the card with the same `id` as one in
the image is ignored: the image's copy wins.

### The pictures

`preview.png` and `icon.png` are generated, not drawn by hand:

```sh
neodct/tools/mkthemeart.py neodct/overlay/NeoDCT/System/themes/Blossom
```

That tool stages the theme, runs `nd-shoot` with it selected, and keeps a real
frame. The preview is therefore a genuine rendering of the theme by the real
framework rather than a mock-up that can drift from it.

Two more tools produce parts of a theme rather than drawing them by hand:

```sh
# the typeface: subset to Latin, rename (OFL), and for a face that reserves
# room for other scripts, fit the vertical metrics to what is left
neodct/tools/mkuifont.py --family "NeoDCT Blossom Rounded" --fit-metrics \
    --out neodct/overlay/NeoDCT/System/themes/Blossom/fonts Regular.ttf Bold.ttf

# the icons: Aero's set recoloured by hue band, so both sets stay the same
# objects in two colours
neodct/tools/tinticons.py neodct/overlay/NeoDCT/System/themes/FrutigerAero/icons \
    neodct/overlay/NeoDCT/System/themes/Blossom/icons
```

## Worked example

`neodct/overlay/NeoDCT/System/themes/FrutigerAero/` is the whole glass look as
a theme file: palette, structure, icons, status sprites, typeface and
wallpaper. It is both the reference for the format and a regression test --
it renders the frames the phone rendered when that look was compiled in, and
the suite checks it. `Blossom/` is the same construction recoloured, and the
shorter path to a theme of your own: copy it and change colours.

## For the curious: how it works

`nd_theme.h` used to hold twenty-six `#define`d colours, so the look was a
property of the binary. They are now fields of a struct the UI loads at
startup -- but the *names* did not change, so `ND_TH_BLUE_TOP` still spells
the same thing at all twenty-seven call sites across the OS and now reads the
active palette instead of a folded-in literal. Nothing had to be touched to
make the whole interface themeable.

Applying a theme replaces the palette of the running process, which is why
the picker can preview a look by simply wearing it and repainting. The setting
(`system.ui.theme`) is the single source of truth, and every other process
follows it:

- **The core** -- the home screen and the menu -- never restarts, so it asks
  after every app exit whether the setting has moved (`nd_theme_is_stale()`)
  and, if so, puts the new theme on and reloads its fonts before it draws
  again. Choosing a theme in Settings therefore changes the home screen the
  moment you leave Settings.
- **An app** reads the theme when it starts. It gets the id from the core in
  `NEODCT_UI_THEME`, next to the wallpaper settings, because an app confined as
  `ndusr_ut` -- the browser and everything installed from the card -- cannot
  open `settings.prop`. Such an app can only wear a theme it can read, which is
  every theme in the image and none on the card; for a card theme it falls
  back to Classic.

Choosing a theme that ships a wallpaper sets it; choosing one that does not
puts the wallpaper back to the default **if the current one belongs to a
theme** (it lives under a `themes/` directory). A picture the owner chose
themselves is never touched.

`docs/NAP-PACKAGES.md` covers the package format, `nd_theme.h` the C contract,
and `nd_themeload.c` the loader.
