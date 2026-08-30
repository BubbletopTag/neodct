# Security notes — sandboxing, and what the C design should leave room for

**Status: partly implemented.** This file records a design conversation, held before
any of it was built, so the hooks stayed in place. Some of it has since been built --
`SECURITY-PLAN.md` section 7 is the record of exactly what, and where the two
disagree the plan is right, because it was written against the kernel the phone
actually runs.

**For what the phone did before any of that, read `SECURITY-AUDIT.md`.** This file is
a design; that one is a measurement, with the line numbers.

---

## Where the phone stands today

One thing is already right:

- **`/` is read-only squashfs under dm-verity.** Only `/NeoDCT/User` is writable, so
  nothing running on the phone can modify the OS image in place.

Two are not:

- **Everything runs as root.** Every process has every permission. A bug anywhere is a
  bug everywhere.

  *No longer entirely true.* There are two users now, `ndusr` and `ndusr_ut`, and the
  browser and everything it starts are the second one. What is still root is nd-core,
  nd-apprun and therefore every app -- so the sentence holds for an app and not for
  the browser.
- **dm-verity is an integrity guarantee, not an authenticity one.** This paragraph used
  to claim the phone "cannot be permanently backdoored through the update path", on the
  grounds that the signing key lives in the read-only image. That was wrong, and it was
  wrong in the direction that matters. The signature is checked by the Update *app*, in
  userspace; the record it produces (`/NeoDCT/User/.ndsys/pending.prop`) and the verity
  root hash it later installs (`installed.prop`) both live on the **writable** partition,
  and `initramfs/ndsys-apply.sh` verifies no signature before writing the image to the
  system device. So a root process stages its own image, records its own root hash, and
  the next boot verifies cleanly against it forever. `initramfs/init` says as much in its
  own comment. See SECURITY-AUDIT.md section 3 for the chain and the two fixes; the real
  one is `CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG` with the release certificate in the
  kernel, which the init script already names as the upgrade path.

Traditional Unix permissions cannot fix the root problem, because root bypasses them by
definition. Mandatory access control — SELinux — is the mechanism that constrains root,
which makes it the right tool for exactly this situation.

---

## Does the C design get in the way?

No. It is the thing that makes per-app confinement possible at all.

**SELinux domain transitions happen only at `execve()`.** A library loaded with
`dlopen()` does not change a process's security domain; it inherits whatever the process
already has.

So the original idea — apps as `.so` files loaded into the core process — would have
made per-app policy impossible. Every app would have run in the core's domain with the
core's full permissions, and SELinux would have had nothing to grip.

Because the core does `fork()` + `execve()` into `nd-apprun`, the kernel gets a
transition point. Three separate concerns now converge on the same design:

| Concern | Requires |
| --- | --- |
| An app crash must not kill the OS | separate process |
| `fork()` in a threaded process is unsafe | immediate `execve()` |
| Per-app security policy | a domain transition at `execve()` |

**Per-app domains.** One `nd-apprun` binary can still yield different domains per app:
the core calls `setexeccon("nd_app_koki_t")` before `execve()`, reading the target domain
from the app's `manifest.json`. Cost is `libselinux` in the core and nothing structural.

---

## The threat that matters most: a malicious MMS

MediaWidget is the planned shared component for showing images and video, including from
MMS. That makes it the phone's highest-value attack surface, for one reason:

**MMS is zero-click.** Every other attack needs the user to do something — open a page,
install an app. An MMS arrives because someone knows the number. By the time anything
appears on screen, the attacker's bytes have already been parsed by three pieces of C.

This is not hypothetical. **Stagefright** (Android, 2015) was precisely this shape: a
heap overflow in the media parser, reached by MMS, no user interaction, roughly 950
million devices. `libwebp` CVE-2023-4863 was the same idea more recently.

```
attacker → MMS → modem → MMSC fetch over HTTP   ← untrusted WAP/PDU parsing
                       → MediaWidget decode      ← untrusted JPEG/PNG/GIF/video
                       → code execution as root
```

Today the payoff is root: dial premium numbers, send SMS to every contact, read every
message, enable RemoteShell and phone home.

### The trade that has to be stated rather than glossed

While the browser is on screen, it owns the screen.

There is no compositor on a 64 MB phone and there will not be one, so a full-screen
handoff means the untrusted process can draw anything -- including a convincing
imitation of the trusted UI. A person looking at a NeoDCT-shaped dialog inside the
browser has no way to tell it from the real one. That is inherent to the design and
not a bug to be fixed later; the alternative costs a compositor.

What keeps it bounded is worth stating in the same breath, because it is the reason
this is acceptable rather than merely admitted:

- the browser cannot run in the background -- it owns the foreground or it is not
  running;
- the core repaints on exit, so the screen it leaves behind is never the one it drew;
- and it never sees a keypress the core did not route to it. On the phone
  `/dev/input` holds only the synthetic bridge the core makes, so "the browser reads
  what you type into the dialler" is not a thing that can happen -- though it IS a
  thing that happens on QEMU, where `/dev/input/event0` is the real keyboard. The
  emulator is the more dangerous configuration here, which is the reverse of the
  usual and worth knowing before concluding from a QEMU test that the browser is
  contained.

### The mitigation: decode in a powerless child

The important move is not "harden MediaWidget". It is **do not decode in the core process
at all**. Decode in a throwaway child that can barely do anything.

```
core (nd_core_t)
  └─ fork + exec  nd-mediadec              domain: nd_mediadec_t
       ├─ receives the data on an already-open fd — needs no filesystem at all
       ├─ landlock: no filesystem access whatsoever
       ├─ seccomp:  read, write, mmap, munmap, exit — nothing else
       ├─ selinux:  no network, no /dev/*, no execve, no unix sockets
       └─ writes raw pixels back on a pipe, then exits
```

Now run a Stagefright-class bug against that. The attacker gets genuine arbitrary code
execution — full control of the instruction pointer — inside a process that can read one
pipe, write one pipe, and exit. It cannot reach the modem, send an SMS, read contacts,
open a socket to exfiltrate, write a file to persist, or `execve` a shell. It dies when
the decode finishes, so there is nothing to persist into.

Full phone compromise becomes a wasted zero-day.

### Two details that matter on a 53 MB phone

**Validate declared dimensions before allocating.** The child hands back raw pixels, so
the core must check width × height against a cap *before* allocating the receiving
buffer. Otherwise an MMS claiming to be 30000×30000 is an out-of-memory kill that needs
no memory-corruption bug at all. Cheap to get right, easy to forget.

**One decoder process per item, not one long-lived one.** No state carries between
images, and the cost is the same ~300–800 KB as any other child. Affordable precisely
because Python is gone.

---

## The underrated risk: the modem

Filesystem confinement matters less here than usual, because `/` is already read-only.
The sharper risk is the modem serial device. Any root process can do:

```sh
echo -e "ATD1900555xxxx;\r" > /dev/ttyUSB2
```

Premium-rate dialling and silent SMS is *the* classic feature-phone attack, and it costs
the victim real money. A NetSurf or MediaWidget bug reaches it today.

SELinux can restrict the modem device to `nd_core_t` alone, so a compromised browser or
decoder physically cannot open it. This is the one place where SELinux clearly beats the
lighter options: Landlock and seccomp handle files and syscalls well, but granular device
`ioctl` control is where SELinux earns its memory.

---

## Per-app least privilege

Each app gets exactly what it needs and nothing else:

| App | Needs | Denied |
| --- | --- | --- |
| Calculator | nothing but the screen | devices, network, all databases |
| PhoneBook | contacts DB | modem, network, SMS DB |
| Messages | contacts + SMS DB, SMS send via core | direct modem access, network |
| Koki | its assets, audio | modem, network, all databases |
| Browser | network, own cache dir | modem, i2c, contacts, SMS |

The browser line is the one that matters: a NetSurf RCE yields a process that can browse
the web and write to its own cache directory. That is a very large reduction from root.

---

## Cost, and a cheaper first step

SELinux policy lives in **kernel memory**. The full reference policy is several MB —
too much for a 64 MB phone. This would need a hand-written minimal policy covering
roughly fifteen domains, not `refpolicy`. It also needs xattr support enabled in
`mksquashfs`; nothing currently enables it.

Both targets run **kernel 6.12.47**, which makes a lighter option available first:
**Landlock + seccomp**. No policy files, no labels, no xattrs, no userspace daemon — a
process restricts *itself*. And `nd-apprun` is the ideal place for it, because it can
open what the app legitimately needs, lock itself down, and only then load untrusted
code:

```
nd-apprun starts  →  opens app dir, asset dir, its own writable dir
                  →  landlock_restrict_self()     ← no new paths after this point
                  →  seccomp filter installed
                  →  dlopen(app.so)               ← app runs already caged
```

Nearly free in RAM, and it gets most of the filesystem confinement.

**Suggested order, when this is picked up:** Landlock + seccomp for apps and the media
decoder first; SELinux later for the browser and the modem device, where mandatory,
non-bypassable policy is worth the memory.

---

## What the C port should do now

Nothing that costs effort — just avoid closing doors:

1. **Keep `fork()` + `execve()` for apps.** Already the plan, for two other reasons.
2. **Route app launching through one function** in the core, so `setexeccon()` has a
   single call site to be added to later.
3. **Give `nd-apprun` an explicit "open everything I need, then load the app" order**, so
   the Landlock call has an obvious home between the two.
4. **Design MediaWidget as a separate decoder process from the start.** This is the one
   item with a real cost if deferred — retrofitting a decode-in-process design into a
   decode-in-child design means changing every caller.
5. ~~**Enable xattrs when building the squashfs.**~~ **Done**, and it turned out to be
   the other half: `mksquashfs` was already storing them -- buildroot passes no
   `-no-xattrs` -- and the kernel was ignoring them. `CONFIG_SQUASHFS_XATTR` is now in
   the fragment, with a comment saying it is there because a kernel cannot ship in an
   `.ndsw` and so this cannot be added later without a reflash.
6. **Never parse untrusted input in the core process.** MMS payloads, WAP/PDU headers,
   downloaded update manifests, web content. Parse in a child, hand back validated data.

Item 4 and item 6 are the ones worth honouring during the port. The rest are free.
