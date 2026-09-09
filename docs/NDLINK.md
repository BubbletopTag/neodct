# ndlink — adb for NeoDCT OS

One host CLI, two backends, one verb set. `ndlink shot out.png` means the same
thing whether it is talking to QEMU or to a phone on the end of a cable, and
that is the whole design: **a caller must never need to know which target it is
talking to.**

This document describes **what exists**, not what is planned. Anything not
built is under "Not built yet", with the reason.

```
ndlink [-t qemu|hw] [--json] [--timeout S] [--force] VERB [args]
```

`-t` defaults to whichever single target is reachable. If both are, that is an
error rather than a guess — silently picking one is how a run reports a pass
against the thing you were not testing.

## Contracts every verb obeys

**`--json` on every verb, without exception.** Human output is for humans;
agents parse the JSON. One object, always carrying `{"ok","verb","target"}`
plus verb-specific fields. Errors are `{"ok":false,"error","code"}`. An agent
that has to parse prose is a flaky agent.

**Exit codes are a contract.** Do not add meanings to them.

| code | meaning |
|---|---|
| 0 | success |
| 1 | assertion failed (`expect`) — the command worked, the answer was no |
| 2 | not implemented on this backend |
| 3 | target unreachable |
| 4 | lock held by another process |
| 5 | usage error |
| 6 | the target ran the command and the target failed it |

The 1-versus-6 distinction is the one that matters to a script: 1 means the
phone is fine and the screen is wrong, 6 means the phone could not do it.

**Locking.** One `flock` per target under `/tmp`. Two agents driving one phone
produce nonsense that reads like a bug. `--force` overrides; the refusal names
the holding pid.

**Timeouts.** Every verb takes `--timeout S` (default 20). Nothing blocks
forever — an overnight run that wedges is a wasted night.

## Verbs that exist

| verb | what it does | qemu | hw |
|---|---|---|---|
| `devices` | which targets are reachable | yes | **tested** |
| `doctor` | capability report, honestly | stub | **tested** |
| `wait` | block until the target is up | yes | **tested** |
| `shell CMD` | run one command, return output and exit code | exit 2 | **tested** |
| `shot OUT.png` | a PNG of the panel | exit 2 | **tested** |
| `digest` | the panel's frame digest | exit 2 | **tested** |
| `key KEY...` | press keys | written, unverified | needs devkey |
| `logs [--os] [-n N]` | recent log lines; `--os` drops the kernel's half | exit 2 | **tested** |
| `snapshot NAME` | capture a reference digest | exit 2 | **tested** |
| `expect NAME` | assert the panel matches a reference | exit 2 | **tested** |
| `state` | version, RAM, card, backlight, battery as JSON | exit 2 | **tested** |
| `push` / `pull` | move files (ftp) | exit 2 | **tested** |
| `connect` / `disconnect` | remember a hardware address | n/a | **tested** |
| `bugreport [OUT]` | one tarball of everything | exit 2 | **tested** |
| `selftest` | run `nd-selftest` and report | exit 2 | **tested** |
| `record DIR [N]` | N frames into a numbered directory | exit 2 | **tested** |
| `diff A B` | compare two references | host-side | **tested** |
| `reboot [recovery]` | restart the phone | exit 2 | **tested** |
| `watch [--view-only]` | a window that shows the panel and types into it | exit 2 | **tested** |
| `apps` | installed apps on the card | exit 2 | **tested** |
| `install PKG.nap` | sideload an app package | exit 2 | **tested** |
| `uninstall DIR` | remove an installed app | exit 2 | **tested** |
| `update PKG.ndsw` | deliver a system update | exit 2 | **tested** |

"tested" means run against a real Luckfox Pico Mini over the ethernet debug
link, not reasoned about.

## The QEMU/hardware asymmetry, which is deliberate

`key` reaches QEMU through the **QEMU monitor** (`sendkey`) and hardware
through the **devkey socket** (`nd-key`). Those are different mechanisms and
they stay different: there is no monitor on a phone, and there is no
guest-side socket reachable from the host under QEMU without networking.

Do not try to unify them. The verb is the same, which is the part that
matters; the transport underneath it is allowed to differ.

Two facts about the monitor, learned expensively by `test_card_flow.sh` and
preserved here: it must be **one connection held open** for the whole
sequence — a connection per key loses most of them, because the command races
the close — and only the *first* keypress is confirmable, because once a
widget with its own read loop is on screen nothing is logged.

## The devenv gate

`key` on hardware needs the **devkey channel**: a `SOCK_DGRAM` socket at
`/run/neodct/devkey` that `nd_input` listens on, and that `nd-key` writes
`"<keycode> <0|1>"` datagrams to.

It only exists on an image built with the **devenv marker**
(`/etc/neodct-devenv`, placed by `neodct/scripts/post-build-devenv-marker.sh`
under `NEODCT_DEVENV_IMAGE=1`). That marker lives on the read-only squashfs
precisely so a running phone cannot create it — the gate has to be something
writable storage cannot set, and that argument is `SECURITY-AUDIT.md` §4 Q5.

No marker means no socket, one log line, and `ndlink doctor` reporting
`devkey socket false`. Nothing else changes; a phone that cannot make a debug
socket still boots.

**Both edges are always sent.** A sender that only presses leaves the key
held, and `nd_input_is_held()` is real state that widgets read — a phone left
believing DOWN is held scrolls on its own. `nd-key --hold` and `--release`
exist for when that is genuinely what you want.

## The framebuffer byte-order trap

`nd-grab` reads the pixel format from `fb_var_screeninfo` and **never assumes
it**. This is not defensive coding; it is a bug this project has already
shipped.

The phone's `/dev/fb0` is the kernel's vfb, which at 32 bpp declares
`red.offset 0` — bytes **R G B x**. Almost everything else on earth declares
`red.offset 16`, i.e. **B G R x**. At 16 bpp vfb is likewise blue-last.

Both halves of NeoDCT once assumed B G R x. Being wrong *together* they looked
correct to each other, and every program that believed the driver — mpv,
NetSurf through libnsfb, the framebuffer console — came out with red and blue
swapped. The detection is one line, and it is `neodct_displayd`'s:

```c
swap_rb = (vinfo.red.offset < vinfo.blue.offset);
```

`--swap-rb` **inverts** that detection rather than selecting an order, so the
flag is for a driver that fills the struct in wrongly and never a way to
guess. `finfo.line_length` is the stride, never `xres * bytespp`.

On the real phone the probe line reads:

```
nd-grab: 240x175 32 bpp stride 960, red@0 blue@16 -> red first
```

When somebody reports "the colours are wrong", that line is the whole
diagnosis. It goes to stderr so it never lands in a PNG on stdout.

## References, and why `expect` does not touch `tests/golden/`

`expect NAME` compares `nd-grab --digest` against `.ndlink/refs/NAME.digest`.

It **never** consults `neodct/tests/golden/`. That directory is the Python
build's output from 0.4.0a and the tree is twenty-six releases past it; it is
not a design authority and a mismatch there is not a finding.

**A screen with a clock on it is not a usable reference.** This was measured,
not guessed: two digests of the home screen a minute apart differ, and
`expect` correctly returned 1. The home screen draws the time, so its digest
changes every minute. Capture references from screens that do not, or expect
to re-cut them constantly.

## Getting the tools onto a phone

`nd-grab` and `nd-key` ship in the image at `/NeoDCT/System/bin`. The rootfs is
a read-only squashfs, so a new one normally means a rebuild and a reflash.

During bring-up there is a shortcut, and it is the difference between a
ten-second loop and a ten-minute one: `/NeoDCT/User` is mounted `rw,noatime,
nosuid,nodev` and **deliberately not `noexec`**
(`neodct/initramfs/init:198` carries the reasoning). So a freshly
cross-compiled binary can be pushed over ftp to `/NeoDCT/User/bin/` and run
from there. ndlink prefers that copy when it exists, because during iteration
it is the newer of the two.

```sh
# cross-compile, push, run -- no reflash
make -C buildroot O=$PWD/build-luckfox neodct-rebuild
# then ftp build-luckfox/target/NeoDCT/System/bin/nd-grab to /NeoDCT/User/bin/
```

## Transport

`shell` is telnet; `push`/`pull` are ftp. Both are the debug-link daemons
described in `docs/DEBUG_LAN.md`, and both are unauthenticated by design on a
cable between one phone and one computer.

ndlink is POSIX shell, matching `run_qemu.sh`, `cloud-test.sh` and
`sdcard.sh`. It shells out to one helper, `ndlink-telnet`, and that is a
deliberate exception worth stating: busybox `telnetd` hands out an
**interactive** shell on a pty, which echoes its input, prints prompts, and
frames nothing. `printf ... | telnet | sed` gets all three wrong and can only
decide "has it finished" with a fixed sleep. Since ndlink exists to make
hardware testing trustworthy, its own transport must not be the flakiest part
of it, so the helper does the handshake properly: echo off, a nonce marker
before the command, and a marker carrying `$?` after it. That is what lets
`shell` return 6 for "the phone ran it and it failed" rather than a shrug.

The repo already ships python3 host tooling (`mkupdate.py`, `uistub.py`,
`mknap.py`, `goldenframe.py`), so this adds no dependency that was not
already required to build an image.

## watch: the panel on your desk, and you can type into it

`ndlink watch` opens a VNC window on the panel — and **your keystrokes go to the
phone**. Arrows navigate, Enter is NaviKey, Backspace is C, `0`–`9`, `*` and `#`
are themselves, and `m` is MENU. `--view-only` takes the typing away.

**There is deliberately no custom viewer.** nd-vncd forwards RFB key events into
the devkey channel, so *any* standard VNC client is already a control window:
`vncviewer`, `gvncviewer`, noVNC in a browser, a VNC app on a tablet. A bespoke
GUI would have meant maintaining a second RFB client and a second key map to
arrive at the same place supporting fewer clients. `watch` finds a viewer and
points it at the phone.

This is the seam the first version of nd-vncd left unwired, and the reason it
was left is unchanged: **not uinput**. Apps never read `/dev/input`, and
`nd_input`'s `is_our_injector()` exists so a core cannot read back what it
wrote. Keys go to the devkey channel, which *is* the core's key source — a key
from a VNC client is merged into the same queue the i2c matrix feeds, so held
state, repeat and T9 all behave exactly as they do for a real press.

There is no `ptrAddEvent`. The phone has no pointer, and inventing one would let
a flow work over VNC that cannot work on the hardware.

**Say the consequence plainly: with keys wired, VNC is no longer a view, it is
control.** It sits behind the same engineering-mode gate and the same bound
address as telnet, which already grants everything, so this adds reach rather
than privilege — but a "screen sharing" port that can also type is worth
knowing about.

Proved on hardware: three `XK_Down` over RFB walked the menu from item 1 to
item 4, and `XK_Return` opened Settings.

## Sideloading

`ndlink install PKG.nap` pushes the package to `/tmp` on the phone and unpacks
it with `nd-nap`, a thin CLI over `lib/nd_nap.c` — the same reader Settings
uses. Arch matching, name sanitising, replace-and-keep-`data/`, the id-conflict
band and the rollback when a replacement fails half way all stay in that one
file. A second implementation of any of it would be a second set of rules for
what a package may do, which is the one thing a package format must not have.

**It installs as `ndusr`, not as root**, and that is not incidental. telnetd
hands out a root shell, so the obvious version installed as root and produced
an app the UI does not own: `root:root` files where every other app has
`ndusr:ndusr`, and a `data/` directory — which the app writes as `ndusr_ut`
through group `ndusr_ut` — that comes out unwritable. It looks installed and
then misbehaves. Measured against a Settings-installed app side by side, which
is the only way that difference shows up.

```sh
neodct/tools/mknap.py --app-dir Calculator/ --so luckfox-armv7=Calculator/app.so -o Calc.nap
ndlink install Calc.nap     # -> {"ok":true,...,"arch_ok":true,"needs_restart":false}
ndlink apps                 # -> ["Bible","Calculator","PSX"]
ndlink uninstall Calculator
```

`nd-nap inspect` exits **1** when the package is fine but built for another
phone, so a script can tell "bad package" from "wrong phone".

## Updates: `update` delivers, it does not apply

`ndlink update PKG.ndsw` copies the package into `/NeoDCT/User/sdcard/update/`
with the ownership the owner's UI expects, which is where the phone already
looks for one. **It does not install it**, and that limit is deliberate.

Applying an update means verifying a signature, writing `pending.prop` and
`pending.img`, and letting the initramfs applier do the write on the next boot.
This tool must not reimplement that chain: a second implementation of "is this
update allowed" is precisely the thing an update system cannot have. `adb
sideload` has the same shape — it hands the package to the updater and the
updater decides.

So after `ndlink update`, open **Settings → System Update** on the phone. Once
the devkey channel ships that last step becomes `ndlink key` presses like any
other flow.

## Two shell traps this file already hit

**Variables are global, and verbs call verbs.** `bugreport` calls `shot`, which
assigns `_out` for its own purposes — so `bugreport`'s own `_out` was silently
overwritten by the callee and the tarball was written to the screenshot's path.
Every variable in `verb_bugreport` now carries a `_br_` prefix. Any new verb
that calls another verb must do the same; POSIX sh has no `local`.

**A helper that emits a statement cannot take arguments.** `remote_tool()` first
emitted a complete `if … fi`, so `$(remote_tool nd-grab) out.png` expanded to
`… fi out.png`. It emits a path *expression* now. The failure surfaced three
layers away, as the transport losing its framing.

## Not built yet

- **`text`, `apps`, `launch`/`stop`, `db`, `settings`, `install`/`uninstall`,
  `dev`, `update`, `backup`/`restore`, `script`.** Stage B and beyond. The
  transports they need (`hw_shell`, `hw_push`, `hw_pull`, `remote_tool`) exist
  and are tested, so these are mostly verb bodies rather than new machinery.
  `text` and `launch` additionally need the devkey socket, since both drive the
  UI by pressing keys.
- **`reboot` is written but has not been run.** Deliberate: the phone is the
  only way to test any of this and there is no maskrom access tonight, so a
  reboot that did not come back would have ended the session. Test it first
  thing with the link in front of you.
- (nothing outstanding from this list)
- **`ndlinkd`** — a device daemon replacing telnet round trips. Telnet is an
  interactive protocol with no clean status, and it will get old around the
  fiftieth scripted `shot`. The CLI should not be able to tell the difference.

## The QEMU backend is UNVERIFIED

Written, never run. `ndlink doctor -t qemu` says so, and it must keep saying so
until somebody clears it — an untested backend that looks tested is worse than
a stub, because the first person to use it will trust it.

The reason it is unverified is not laziness: **the QEMU image cannot currently
be built on this workstation.** Buildroot's GCC 14.3.0 fails to compile its own
`libcody` against a modern host GCC:

```
../../libcody/cody.hh:113:24: error: no matching function for call to
    'S2C(const char8_t [2])'
```

That is the known `u8""`-became-`char8_t` breakage when a host GCC 15 builds
GCC 14. Until the toolchain is patched or pinned, `build-qemutest` has no
compiler and the QEMU path is unavailable — which is exactly why the hardware
link, and this tool, matter.
