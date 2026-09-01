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

Until recently `ndusr` was also in group `disk`, which reaches the raw storage —
every partition, readable and writable. That was a mistake and it has been
removed: nothing on the phone ever needed it.

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

That second rule was missing at first, and for a short time
`spawn("/bin/sh", …, no user)` was a working root shell. The test asserted that
the *name* "root" was refused and passed happily, because it never tried leaving
the name out. **A boundary that only stops the spelling you thought of is not a
boundary** — it is the most useful thing this project has learned about writing
security tests.

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
| restart the phone | by asking nd-core | **no** |
| become root | no | no |

The "appears empty" rows are worth a moment. An untrusted app does not get a
*permission error* when it looks at your contacts — it gets an **empty
directory**. The app runs in its own view of the filesystem where those folders
have been replaced by nothing. Data that is not present cannot be leaked by a
bug in a permission check.

---

## 7. Apps you install yourself

`/NeoDCT/User/apps` is where an app you installed lives, as opposed to one that
came with the phone.

**Everything in that folder is untrusted, always**, no matter what it claims
about itself. That is not a setting and there is no way for an app to opt out.

The reason is where the folder lives. `/NeoDCT/User` is the writable part of the
phone, and it *survives system updates* — so unlike the built-in apps, a bad app
here is not removed by reinstalling the OS. Anything that can put a file there
gets code that runs when you open it.

The built-in apps are in a different place entirely: a read-only, cryptographically
verified image. They cannot be modified without replacing the whole system, which
requires a signature the phone checks at boot.

---

## 8. What is deliberately not protected

An honest list matters more than a reassuring one.

**All normal apps share one user.** Clock, Messages and a bible app all run as
`ndusr`, so they are not separated *from each other*. Any of them can read your
contacts. The separation that exists today is "built-in versus installed", not
"per app". Per-app users are the obvious next step.

**The broker is a big program running as root.** It only *does* four things, but
it is a copy of nd-core, so it *contains* the whole framework — the screen code,
the database library, the font renderer. It should be locked down further, most
cheaply with seccomp.

**A compromised UI can lie to the broker within its allowance.** It cannot get
root, but it can start apps and restart the phone, because those are things it
is allowed to ask for.

**Nothing here protects against someone with the phone in their hands** and a
serial cable, or against a flaw in the kernel itself.

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
