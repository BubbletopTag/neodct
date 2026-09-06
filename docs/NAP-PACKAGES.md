# .nap packages -- installing an app from the memory card

A `.nap` -- a **NeoDCT Application Package** -- is how an app that is not
part of the system image gets onto a phone. It is a plain, uncompressed POSIX
ustar archive with a `.nap` extension, holding one app: its `manifest.json`,
its icon, its `app.so` and whatever files the app reads. The owner copies it
onto the memory card, opens **Settings → Install apps**, and picks it. The
phone unpacks it into

    /NeoDCT/User/sdcard/apps/<Name>/

which is the directory the menu already scans and the core already confines.

Nothing about the confinement is new or decided by the package. Everything
under `sdcard/apps` runs as `ndusr_ut`, in a private mount namespace, with no
service socket, and may write only its own `data/` -- `docs/HOW-IT-WORKS.md`
section 7 has the whole argument. A `.nap` is a way of getting files into a
directory whose rules were already written.

The reader is `neodct/src/lib/nd_nap.c`; `neodct/src/include/nd_nap.h` is the
specification and this page is the human-readable version of it.

## Making one

```sh
# one phone: app.so at the root, "arch" in the manifest
neodct/tools/mknap.py --app-dir Bible/ --so luckfox-armv7=luckfox-armv7/app.so -o Bible.nap

# any phone: lib/<tag>/app.so per phone, and the phone picks its own
neodct/tools/mknap.py --app-dir Bible/ \
    --so luckfox-armv7=luckfox-armv7/app.so \
    --so qemu-aarch64=qemu-aarch64/app.so -o Bible.nap

# what the phone would see, and whether it would accept it
neodct/tools/mknap.py --list Bible.nap
```

`--app-dir` is the directory with `manifest.json`, `icon.png` and the app's
own files. It is the same directory that would go under
`neodct/overlay/NeoDCT/System/apps/` if the app were stock -- the manifest
needs no new field, and `mknap.py` adds `"arch"` itself for a one-phone
package. An `app.so` or a `lib/` found inside the directory is ignored with a
warning: the code that ships is the code named on the command line, so a
stale host build sitting next to the manifest cannot be packed by mistake.

`tar` makes an acceptable package too, as long as it is ustar and the layout
below is followed:

```sh
tar --format=ustar -cf Bible.nap -C Bible manifest.json app.so icon.png web.ndb
```

## What is inside

Every entry is relative to the app's directory; there is no top-level folder.
Two shapes are accepted and both come out the same on the card:

```
ONE PHONE                       ANY PHONE (universal)

manifest.json                   manifest.json
icon.png                        icon.png
app.so                          lib/luckfox-armv7/app.so
web.ndb ...                     lib/qemu-aarch64/app.so
                                web.ndb ...
```

In the first shape `manifest.json` carries `"arch": "<tag>"` naming the phone
the `app.so` was built for. In the second, `lib/<tag>/app.so` exists once per
phone the package supports; the installer copies the one matching this phone
to `app.so` and never installs `lib/` itself.

The tags:

| tag | `uname -m` | phone |
| --- | --- | --- |
| `luckfox-armv7` | `armv7l` | the Luckfox Pico Mini B: Cortex-A7, hard float, musl |
| `qemu-aarch64` | `aarch64` | the QEMU development image |
| `host-x86_64` | `x86_64` | a host build; only the unit tests use it |

They name the *target* rather than the bare ISA because "armv7" alone does
not say hard-float, Thumb-2 or which libc, and the two builds this tree makes
are already called luckfox and qemu everywhere else.

A package with no `app.so` for this phone is refused before anything is
written. That is the whole point of the tag: a QEMU image offered an armv7
package says "This package is not for this phone" rather than failing in
`dlopen()` at first launch.

The install directory is derived from the manifest's `"name"` by keeping
letters, digits, `_` and `-` and dropping everything else, so "Phone Book"
installs as `PhoneBook`.

### The `"id"`, and the band it belongs in

The `"id"` orders the app in the menu exactly as it does for a stock app --
and **an installed app's id should be between 100 and 899**.

Stock apps use 1-99 (they run 1-12 today) and the 9xx block is reserved for
the two that must sort last, MusicPlayer at 970 and Power at 971. Everything
installed from a card goes between them.

This matters because the menu sort is *stable*: two apps that claim the same
id keep the order the directory happened to return them in, which on ext4 is
install order and is nothing an owner can see or predict. The first two `.nap`
packages ever made both claimed 13 -- the next number after Update's 12 --
because both authors counted the same way, and there was nothing to count
against.

An id outside the band still installs, and so does one that collides. Neither
is worth refusing an app over. Both are written to the log, naming the other
app, and the install screen can say so before it writes anything
(`nd_nap_id_conflict()` in `neodct/src/include/nd_nap.h`).

## What is refused

The archive is a file off a removable card, so it is an attacker's file and
the reader refuses rather than copes. Every refusal is a sentence on the
phone's screen, and nothing is written when one happens:

- an entry that is not a regular file or a directory -- symlinks and hard
  links are how an archive reaches outside the directory it is unpacked into;
- an absolute name, a `..` component, an empty component or a backslash;
- anything under `data/`. That directory is the app's writable storage and
  the *phone* makes it, owned so the app can write it; a package that
  pre-seeded it would arrive with the wrong owner and could overwrite what an
  earlier version of the app saved;
- anything under `lib/` that is not `lib/<tag>/app.so`;
- an `app.so` at the root without `"arch"`, an `"arch"` without an `app.so`
  at the root, or both shapes at once;
- pax and GNU extension headers (`x`, `g`, `L`, `K`). `mknap.py` and
  `tar --format=ustar` never write them;
- a header whose checksum does not match, or a file that claims more bytes
  than the archive holds;
- more than 4096 entries, or a file over 64 MiB, so a crafted header cannot
  fill the card;
- the same file twice.

## How the install goes

The package is validated in full -- every header, every name, the manifest
-- before the first byte is written. Files are then unpacked into a staging
directory beside the app's, and `manifest.json` is written **last**: the menu
shows a directory the moment it has a manifest, so an install that dies half
way leaves a directory with no manifest, which is invisible, and the next
install of the same package removes it.

Installing a package for an app that is already there **replaces it and
keeps its `data/`**. The old directory is renamed aside, its `data/` is moved
into the staged one, the staged one takes the name, and only then is the old
one removed. A failure at any step before the final rename puts the old app
back, data included.

Files land as 0644 and directories as 0755, owned by `ndusr` -- the modes
`neodct-sdcard`'s `apply_layout()` restates on every mount. The one thing
`ndusr` cannot do is make `data/` belong to `ndusr_ut`; that needs
`CAP_CHOWN`, which the core gave up. So after an install Settings asks the
core for `nd_svc_layout_card()`, which runs `neodct-sdcard layout` as root
through the broker and creates `data/` with the right owner. The verb takes no
argument, for the same reason the format takes none: the helper lays out the
card at the phone's own mountpoint and decides for itself whether the card is
one of ours. If that step fails the app is still installed and in the menu;
it just cannot save anything until the card is next mounted, and Settings
says so.

## Where the phone looks

**Settings → Install apps** lists every `*.nap` in three places on a ready
NeoDCT card: the card's root, its `apps/` folder, and `untrusted/`, which is
where the browser puts a download. A card in the old FAT format cannot hold
apps and the screen says so and points at **Memory card**, which offers the
reformat.

## Giving keys to a program that is not an `app.so`

An `app.so` reads the keypad through `nd_input_read_key()` and needs nothing
here: the core hands it an inherited pipe and every press and release arrives
on it.

**A real binary your app starts cannot read that pipe.** It is a private
protocol -- no `/dev/input` path, no `ioctl`, no `EVIOCGBIT` -- so an
emulator, a viewer, a player or anything else written to read a keyboard will
not see a single key from it. Those programs scan `/dev/input` instead, and on
the phone `/dev/input` is **empty**: the keypad is a PCF8575 matrix on i2c
that the core scans in software, and a matrix is not an input device.

So ask for one, in `manifest.json`:

```json
{
    "name": "PlayStation",
    "id": "113",
    "useKeypadDevice": true
}
```

The core then creates a synthetic keyboard *before* it starts your app, waits
until the node it produced is actually readable, and puts its path in your
environment:

```c
const char *node = nd_app_key_evdev();   /* "/dev/input/event3", or NULL */
```

Pass that to whatever you are starting, however it wants it -- an argument, an
environment variable of its own, a config line. `neodct-play` (the media
player wrapper) reads `NEODCT_KEY_EVDEV` itself, so a video plays with working
keys if you simply let your child inherit your environment.

`NULL` means the core made no device. That is **normal** on a development
board, where a real keyboard already exists and the program will find it by
itself; a second synthetic device there would make every press arrive twice.
It also means "something went wrong", and in that case the core has already
written the reason to the log.

### What arrives on the node

By default, the sixteen keys the phone has, as themselves, press and release:

| key | evdev code |
| --- | --- |
| NaviKey | `KEY_ENTER` (28) |
| C | `KEY_BACKSPACE` (14) |
| Up / Down | `KEY_UP` (103) / `KEY_DOWN` (108) |
| 1-9, 0 | `KEY_1`..`KEY_9`, `KEY_0` (2-11) |
| `*` | `KEY_LEFTSHIFT` (42) |
| `#` | `KEY_BACKSLASH` (43) |

NeoDCT keycodes *are* Linux keycodes, so nothing is translated and nothing is
lost. There is no Left or Right key on this phone and the node does not invent
one; if your program needs a d-pad, map 2/4/6/8 yourself -- that is your
control scheme to choose, and the reason the default is raw.

If your program expects a **keyboard** rather than a keypad, add a map and the
core will translate on the way through:

```json
{ "useKeypadDevice": true, "keypadDeviceMap": "browser" }
```

| map | what the node carries |
| --- | --- |
| `raw` (default) | the sixteen keys above, unchanged |
| `browser` | 2/4/5/6/8 as a d-pad, and `#` cycles the rest of the pad through abc / ABC / 123 so text can be typed. What the web browser uses. |
| `shell` | multi-tap letters everywhere, `*` as Tab. For a terminal. |

An unrecognised map name is treated as `raw`, with a line in the log naming
the word that was not understood.

### Why the core makes it and not you

Your app runs as `ndusr_ut`, and `ndusr_ut` may **read** input devices and may
never **create** one. That is deliberate and it is not going to change: this
phone has no compositor, the app on screen owns the whole panel, and a process
that could both draw the interface and synthesise keypresses into it could
drive the real phone underneath -- dial a number, send a message, accept a
prompt the owner never saw.

Reading is safe and writing is not, so the two were split: the core, which is
already reading every key and already decides what they mean, owns the write
end; your app is handed a path it can only read. If you find code that opens
`/dev/uinput` from inside an app, it is from before this existed and it has
not worked since 0.5.0b.

## What an installed app does not get

Worth knowing before you design around something that is not there:

- **no service socket.** Every `nd_svc_*` call answers "not present": no
  battery reading, no modem status, no clock set, no SMS. See
  `docs/HOW-IT-WORKS.md` section 7 for why.
- **no view of** `/NeoDCT/System/engineering`, `/NeoDCT/System/keys`,
  `/NeoDCT/System/tones`, `/NeoDCT/System/wallpapers`, `/NeoDCT/User/db`,
  `/NeoDCT/User/.remote`, `/NeoDCT/User/.ndsys`, `/NeoDCT/User/.seedrng` or
  `/NeoDCT/User/browser`. They are not unreadable, they are *empty*: a
  read-only mode-0000 tmpfs is mounted over each one.
- **one writable directory**, its own `data/`, which the phone creates.

## Building an app.so for the phone without building Buildroot

`neodct/contrib/Bible/src/tools/build-bible-cross.sh` shows the fast path: a
standalone musl cross toolchain and a stub `libneodct` generated from the
host library's symbol table, so the link *verifies* every symbol the app
needs exists and the `.so` gets the right `DT_NEEDED` entries. It works for
any app whose includes reach nothing outside libc and zlib; anything touching
freetype, libpng, sqlite or libcrypto needs the real Buildroot toolchain.

## The first package

`neodct/packages/Bible-lf.nap` is the Bible app (`neodct/contrib/Bible`),
built for the Luckfox only; `neodct/packages/Bible-qemu-aarch64.nap` is the
same app for the development image. Each is about 1.8 MB, most of which is the
World English Bible in the app's own per-chapter zlib pack. Copy one onto a
card, put the card in the phone, and install it from Settings.

(The Luckfox package was called `Bible-luckfox-armv7.nap` until it was renamed
to match the sibling PlayStation repo's `-lf` convention. If a script of yours
still names the old path, that is why it cannot find it.)
