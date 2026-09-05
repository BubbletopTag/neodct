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
installs as `PhoneBook`. The `"id"` orders the app in the menu exactly as it
does for a stock app.

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

## Building an app.so for the phone without building Buildroot

`neodct/contrib/Bible/src/tools/build-bible-cross.sh` shows the fast path: a
standalone musl cross toolchain and a stub `libneodct` generated from the
host library's symbol table, so the link *verifies* every symbol the app
needs exists and the `.so` gets the right `DT_NEEDED` entries. It works for
any app whose includes reach nothing outside libc and zlib; anything touching
freetype, libpng, sqlite or libcrypto needs the real Buildroot toolchain.

## The first package

`neodct/packages/Bible-luckfox-armv7.nap` is the Bible app
(`neodct/contrib/Bible`), built for the Luckfox only. It is 1.8 MB, most of
which is the World English Bible in the app's own per-chapter zlib pack. Copy
it onto a card, put the card in the phone, and install it from Settings.
