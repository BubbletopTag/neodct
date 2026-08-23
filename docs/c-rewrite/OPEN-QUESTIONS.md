# Open questions for the C port

Anything an agent could not determine from the code goes here rather than being guessed.
Add an entry with the Python file and line you were reading, then carry on with the rest
of your work.

Format:

## <subsystem> — <one-line question>
**Found in:** `path/to/file.py:123`
**What the Python does:** ...
**Why it is unclear:** ...
**Options:** ...
**Answer:** _(left blank for the project owner)_

---

## core — should an incoming call interrupt an app that never calls read_keypress()?
**Found in:** `neodct/overlay/NeoDCT/System/core/main.py:1073`, and `uistub.py`'s note that
Koki reads the evdev fd directly.
**What the Python does:** `poll_modem()` raises `IncomingCall` from inside
`read_keypress()`. An app that never calls `read_keypress()` — Koki — is therefore never
interrupted by a ringing phone.
**Why it is unclear:** This looks like a bug rather than a design decision, but a strict
1:1 port would reproduce it.
**Options:** (a) reproduce exactly, calls do not interrupt Koki; (b) put the modem on its
own thread in the core process so calls always interrupt. (b) is proposed in
`ARCHITECTURE.md` and is a deliberate deviation.
**Answer:** _(pending — flagged to the project owner 2026-08-23)_
