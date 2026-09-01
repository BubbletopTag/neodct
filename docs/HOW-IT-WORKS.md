# How NeoDCT works

A plain-language tour of the phone's software, with an emphasis on what stops
what. No kernel background assumed.

If you want the reasoning behind a specific decision, the header comment on the
file usually has it at length — this document is the map, not the territory.

---

## 1. What the phone is

NeoDCT is a small operating system for a feature phone: 240×175 screen, sixteen
keys, 64 MB of RAM. It runs Linux underneath, and everything above the kernel is
C written for this project.

There are four kinds of program on a running phone:

| | what it is |
|---|---|
| **init** | the first process; starts everything else |
| **nd-core** | the UI. The home screen, the menus, the modem and battery services |
| **the broker** | a small helper that holds the phone's remaining privilege |
| **apps** | one process each, started when you open them, gone when you close them |

Apps are not part of the UI. Opening Clock starts a *separate process*; closing
it ends that process. If an app crashes, it takes nothing with it.

---

## 2. The one rule that explains the rest

**A program can never give itself more power.** It can only be given power by
whatever started it, or throw power away.

Privilege on Linux flows downhill from boot and never uphill. Every protection
in this phone is built on that single fact, so it is worth stating before
anything else.

Here is the whole downhill flow:

```
init                          root
 └─ nd-core                   root ...for about a second
     ├─ forks the broker ───▶  broker    root  ← keeps it, forever
     └─ then throws its own root away
                              nd-core   ndusr  ← the UI, from here on
         └─ apps              ndusr  or  ndusr_ut
```

The broker is not root because of anything clever. It is root because it was
**born root and never gave it up** — a copy of nd-core taken a moment before
nd-core dropped its own. Nothing refills it.

---

## 3. Who is who

Three users exist on the phone. They are ordinary Linux users; there is no
SELinux, no AppArmor, nothing exotic.

| user | who runs as it | what it can reach |
|---|---|---|
| `root` | init, and the broker | everything |
| `ndusr` (1000) | **nd-core**, and normal apps | the screen, the keypad, the sound card, the modem, the battery chip, and your data |
| `ndusr_ut` (1001) | the browser, the media player, and **anything you installed yourself** | the screen, the keypad, the sound card. Nothing else |

`ndusr_ut` is deliberately **not** a member of `ndusr`. That single fact is the
boundary between "an app shares your data" and "an app does not".

It is also not in `dialout`, which is the group that reaches the modem. A
browser that cannot open the modem cannot dial a premium-rate number, and that
is the classic feature-phone attack.

Two group memberships were taken away after they turned out to grant more than
they looked like they did:

- `ndusr` was in `disk`, which reaches the raw storage — every partition,
  readable and writable. Nothing ever needed it.
- `ndusr_ut` is in `audio`, because that is how the browser plays sound, and
  Linux puts *every* sound device in that one group — including the
  microphone. Recording is now a separate group. Playing and recording are no
  more the same permission than hearing and speaking are.

---

## 4. What the broker is for

When nd-core stopped being root, it lost four abilities. Not more, not fewer —
this was measured by making it drop and seeing what broke, rather than reasoned
about:

| the broker does | because it needs |
|---|---|
| start an app as a lower-privileged user | the ability to change user, which only a privileged process has |
| wait for that app to finish | it started it, so only it can collect it |
| power off / restart | `CAP_SYS_BOOT` |
| set the clock | `CAP_SYS_TIME` |

That is the entire list. The broker draws nothing, reads no keys, and opens no
devices.

**The odd one is starting apps.** Giving away privilege is itself something you
need privilege to do — you cannot say "become user 1000" unless you are already
privileged. nd-core lost that ability the moment it dropped, which is why the
broker exists at all.

### The broker refuses things

nd-core is unprivileged now, which means it might one day be compromised. So
every message arriving at the broker is treated as untrusted:

- It will only start something as `ndusr` or `ndusr_ut`. "Run this as root" is
  not a request it will honour.
- Nor is "run this without changing user", which is the same thing said
  differently. Only three specific programs may run undropped, all of them on
  the read-only part of the disk where they cannot be swapped.

- Nor is naming a user the phone does not have, which is the same thing said a
  third way: a name that cannot be looked up means nobody to become, and a
  program told to become nobody stays what it was.
- And it builds the environment for those three programs itself, rather than
  passing on the one it was handed.

Each of those was found separately, after the one before it had been fixed and
tested. The first version refused the *name* "root" and its test passed happily
— because the test never tried leaving the name out, and
`spawn("/bin/sh", …, no user)` was a working root shell for as long as that
lasted. **A boundary that only stops the spelling you thought of is not a
boundary.** It is the most useful thing this project has learned about writing
security tests, and it has now been learned three times.

The environment one is worth understanding, because it is not obvious. Two
programs are allowed to run as root. One of them is a shell script that calls
`mount` and `mkfs` by name, without saying where to find them; the other loads
its real code from a folder named in a variable. So handing either of them a
chosen environment is handing them a chosen *program*, no matter how carefully
the filename was checked. Pinning the path and not the environment pins
nothing.

---

## 5. How an app asks for something

An app cannot do much on its own, so it asks. There are two layers of asking,
and they stack:

```
   Messages  ─┐
   Clock     ─┼─  8 verbs  ──▶  nd-core  ──  4 verbs  ──▶  broker
   Browser   ─┘  (nd_svc)        (ndusr)     (nd_broker)    (root)
```

A **verb** is a named, fixed request — not a general "run this for me". The
security value is entirely in *fixed and named*: the whole list is short enough
to read.

**Apps ask nd-core** for: send a text, modem status, battery reading, battery
quickstart, restart, power off, set the clock, format the memory card.

**nd-core asks the broker** for: start an app, wait for an app, power off,
set the clock.

Most requests never reach the broker. Sending a text is nd-core writing to the
modem itself — it is allowed to, because it is in the `dialout` group. That is a
file permission, not root.

### How an app is connected

When nd-core starts an app it creates a private two-ended pipe and hands the app
one end. There is no address to look up and no name to guess: **the app can talk
to nd-core because it was handed the connection, and it cannot talk to anything
else.**

This is also how nd-core knows who is asking. It gave connection #7 to Messages,
so a request arriving on #7 is from Messages. No identity check is needed
because identity was never in question.

**Untrusted apps are handed no connection at all.** The browser has no way to
send a text — not because a check refuses it, but because there is nothing to
send the request down. That is a stronger property than a refusal: the request
is not expressible.

---

## 6. What an app can and cannot do

| | a normal app (`ndusr`) | an untrusted app (`ndusr_ut`) |
|---|---|---|
| draw on the screen, read keys | yes | yes |
| play sound | yes | yes |
| read your contacts and messages | yes | **no — the folder appears empty** |
| send a text | by asking nd-core | **no — no connection** |
| open the modem directly | yes (it is in `dialout`) | **no** |
| read the phone's signing keys | yes | **no — the folder appears empty** |
| write to your storage | yes | only its own folder |
| read another app's data | yes | only within the untrusted set — see below |
| change its own program | — | **no — its folder is not its** |
| restart the phone | by asking nd-core | **no** |
| become root | no | no |

One row is deliberately not a clean "no". All untrusted things — the browser,
the media player, everything you installed — run as the *same* user, so the
permissions cannot yet tell them apart from each other. They are separated from
*you* completely and from *each other* not at all. A user per installed app is
the fix, and the folder layout is already shaped for it; ext4 is what made it
possible to consider, because FAT had nowhere to write an owner down.

The "appears empty" rows are worth a moment. An untrusted app does not get a
*permission error* when it looks at your contacts — it gets an **empty
directory**. The app runs in its own view of the filesystem where those folders
have been replaced by nothing. Data that is not present cannot be leaked by a
bug in a permission check.

---

## 7. Apps you install yourself

They live on the **memory card**, at `/NeoDCT/User/sdcard/apps`.

Not on the phone's own writable partition, which is eight megabytes and already
holds your contacts, your messages, your settings and the logs. An apps folder
there is a feature that fills the space the phone needs in order to save
anything at all.

**Everything in that folder is untrusted, always**, no matter what it claims
about itself. There is no setting and no way for an app to opt out. The card
comes out of the phone and goes into a computer, so an app there is quite
literally bytes a stranger could have chosen — which is why the rule is about
*where the code is* and never about what the code says about itself.

The built-in apps are somewhere else entirely: a read-only, cryptographically
verified image. They cannot be changed without replacing the whole system, which
needs a signature the phone checks at boot.

### Why the card is ext4 now

It used to be FAT32, the format every camera and every cheap USB stick uses.
FAT32 has one property that turned out to decide this whole design: **it does
not record who owns a file.**

On a FAT card, permissions are not stored on the card at all. They are decided
once, when the card is plugged in, and applied identically to every single file
on it. So "this app may read its own program but may not change it" is not a
sentence a FAT card can hold. Neither is "downloads are writable but your music
is not" — the old card needed a *second partition* just to say that much.

ext4 records an owner, a group and permissions for every file and folder
separately. One card then says all of it:

```
/NeoDCT/User/sdcard/
  apps/            you may install; an app may not
    Bible/         the app's program — the app can read it, not write it
      app.so
      data/        the app's own storage, and the only thing it can write
  untrusted/       downloads, and the browser's and media player's state
  music/           an app cannot read this at all
  wallpapers/
  tones/
```

**An app cannot modify its own program.** Its folder belongs to you, not to it.
That is what makes uninstalling mean something, and it is why an app cannot
arrange to still be there after you remove it.

**An app cannot read your music, or list the card, or see another app's
folder.** Not because a check refuses — because the permissions on the card say
it is not its.

Two honest notes. The phone **re-applies these permissions every single time it
mounts the card**, because you can carry a card to a computer and change them
there; permissions on removable media are a claim until the phone has checked
them. And Windows and macOS need a helper to read ext4, where Linux reads it
out of the box. That is the price of a card that knows who owns what.

If you have a card from an older NeoDCT, the phone still reads it — your music
and wallpapers work exactly as before — and Settings will offer to reformat it
so it can hold apps. That erases the card, so it asks first and it will never
do it on its own.

---

## 8. What is deliberately not protected

An honest list matters more than a reassuring one.

**All normal apps share one user.** Clock, Messages and a bible app all run as
`ndusr`, so they are not separated *from each other*. Any of them can read your
contacts. The separation that exists today is "built-in versus installed", not
"per app". Per-app users are the obvious next step, and the card layout is
already arranged so that adding them is a change of owner rather than a change
of design.

**Untrusted apps are not separated from each other either.** The browser and
anything you installed are the same user, so they can reach each other's
storage. The browser's own profile is now hidden from installed apps, which
closes the case that mattered, but it is a patch over the real gap rather than
the gap being closed.

**The broker is a big program running as root.** It only *does* four things, but
it is a copy of nd-core, so it *contains* the whole framework — the screen code,
the database library, the font renderer. It should be locked down further, most
cheaply with seccomp.

**A compromised UI can lie to the broker within its allowance.** It cannot get
root, but it can start apps and restart the phone, because those are things it
is allowed to ask for.

**Nothing here protects against someone with the phone in their hands** and a
serial cable, or against a flaw in the kernel itself.

**Nor against someone with your memory card.** Everything on it can be read and
rewritten on any Linux computer. That is not a hole so much as the reason
everything on the card is treated as untrusted, and the reason the phone
re-applies the card's permissions every time it mounts it.

---

## 9. Checking it yourself

Two ways, and neither requires taking anyone's word for it.

**`nd-selftest`** is an engineering app that checks the live phone: that the
users exist with the right ids and groups, that the folder permissions are what
they should be, that an untrusted process really is denied the things it should
be. It reports PASS/FAIL per check.

**The confinement probes** are five apps that deliberately try forbidden things
and report what stopped them — sending a text as you, writing where they should
not, installing an unsigned update, restarting the phone, and leaving something
behind that would run on the next boot. They are not shipped in a release; see
`neodct/tests/pentest-apps/README.md` for how to build an image with them.

---

## Where to read more

| for | read |
|---|---|
| how to build and run it | `AGENTS.md` |
| the security design and its reasoning | `docs/c-rewrite/SECURITY.md` |
| the threat model this was written against | `docs/c-rewrite/SECURITY-AUDIT.md` |
| the users and groups, with the argument for each | `neodct/configs/users-table.txt` |
| the broker, in detail | `neodct/src/include/nd_broker.h` |
| how apps ask for things | `neodct/src/include/nd_svc.h` |
| writing an app | `.claude/skills/neodct-app/SKILL.md` |
