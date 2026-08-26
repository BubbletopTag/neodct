# C coding standards for the NeoDCT port

Every agent working on the C rewrite follows this document. It exists because ten agents
writing C in parallel without a shared contract produces ten incompatible styles and a
pile of memory bugs that only show up on the hardware.

Read this before writing a line of code.

---

## 1. The rules that are not negotiable

These cause crashes, memory leaks, or silent corruption on a 53 MB phone. There is no
"but in this case".

### 1.1 fork() is always immediately followed by execve()

The core process runs threads (modem, clock). Forking a threaded process gives the child
only the calling thread. Any mutex another thread held at the moment of the fork stays
locked forever in the child — including the ones inside `malloc()`. The child hangs the
first time it allocates.

```c
pid_t pid = fork();
if (pid == 0) {
    execve(NDAPPRUN_PATH, argv, envp);   /* first statement in the child */
    _exit(127);                          /* only reached if execve failed */
}
```

Between `fork()` and `execve()` you may call **only** async-signal-safe functions. No
`malloc`, no `printf`, no `fopen`. `_exit()`, not `exit()` — `exit()` runs atexit
handlers and flushes the parent's buffers a second time.

### 1.2 Every allocation has exactly one owner

Write down who frees it, in a comment, at the point of allocation. If you cannot name
the owner, the design is wrong — fix the design, do not allocate.

```c
/* owned by the caller; free with nd_image_free() */
nd_image *nd_image_new(int w, int h, nd_pixfmt fmt);
```

### 1.3 Check every allocation and every syscall

No exceptions. `malloc` returns NULL under memory pressure on this device — that is a
normal condition here, not a theoretical one.

```c
nd_image *img = nd_image_new(240, 175, ND_RGB888);
if (!img)
    return ND_ERR_NOMEM;
```

### 1.4 No unbounded string functions

Banned: `strcpy`, `strcat`, `sprintf`, `gets`, `alloca`.
Use: `snprintf`, and the project's `nd_strlcpy` / `nd_strlcat`.

Always check `snprintf`'s return value — it tells you the length it *wanted*, which is
how you detect truncation:

```c
int n = snprintf(buf, sizeof buf, "%s/%s", dir, name);
if (n < 0 || (size_t)n >= sizeof buf)
    return ND_ERR_TOOLONG;
```

### 1.5 No variable-length arrays, no recursion on untrusted input

The default thread stack is small and this device has no swap worth relying on. Anything
sized by input goes on the heap with an explicit cap.

### 1.6 Fixed-width integer types everywhere

`uint8_t`, `int32_t`, `size_t`. Never bare `int` for anything that indexes memory, holds
a pixel, or comes off the wire. The target is 32-bit ARM; assumptions that hold on a
64-bit desktop will not hold there.

### 1.7 Free everything on the way out

Even at exit. Not because the kernel won't reclaim it, but because a clean teardown is
how leak detectors stay useful. `valgrind` output full of "still reachable" hides the
real leak.

---

## 2. Naming and layout

| Thing | Convention | Example |
| --- | --- | --- |
| Public function | `nd_<module>_<verb>` | `nd_draw_rect()` |
| Private function | `static`, no prefix | `static void clip_span()` |
| Type | `nd_<noun>` | `nd_image`, `nd_font` |
| Enum member | `ND_<ENUM>_<NAME>` | `ND_PIXFMT_RGB888` |
| Constant / macro | `ND_UPPER_SNAKE` | `ND_SCREEN_W` |
| Struct member | `snake_case` | `img->line_stride` |
| File | `nd_<module>.c` / `.h` | `nd_draw.c` |

Formatting: 4 spaces, no tabs. Braces on the same line for control flow, on their own
line for function bodies. 100-column soft limit. Run `clang-format` with the repo's
`.clang-format` before committing.

---

## 3. Error handling

One convention across the whole project. Functions that can fail return `nd_err`:

```c
typedef enum {
    ND_OK = 0,
    ND_ERR_NOMEM,
    ND_ERR_IO,
    ND_ERR_INVAL,
    ND_ERR_NOTFOUND,
    ND_ERR_TOOLONG,
    ND_ERR_HARDWARE,
} nd_err;
```

Functions that return a pointer return `NULL` on failure. Never both.

Never fail silently. `nd_log_err()` on the way out, so it reaches the serial console:

```c
if (fd < 0) {
    nd_log_err("fb", "cannot open %s: %s", path, strerror(errno));
    return ND_ERR_IO;
}
```

Use `goto` for cleanup. This is the one place `goto` is correct C, and refusing to use it
produces worse code:

```c
static nd_err load_thing(const char *path, thing **out)
{
    nd_err rc = ND_OK;
    FILE *f = NULL;
    uint8_t *buf = NULL;

    f = fopen(path, "rb");
    if (!f) { rc = ND_ERR_IO; goto done; }

    buf = malloc(size);
    if (!buf) { rc = ND_ERR_NOMEM; goto done; }
    ...
done:
    free(buf);
    if (f) fclose(f);
    return rc;
}
```

---

## 4. Memory discipline on a 53 MB phone

- **Allocate once, reuse.** Screen buffers, scratch rows and glyph caches are allocated
  at startup and live for the process lifetime. Nothing per-frame.
- **No allocation in the render path.** If a drawing function needs scratch space, it
  takes it as an argument or uses a preallocated buffer on the context.
- **Caches are byte-budgeted, never entry-counted.** The Python engine already learned
  this the hard way — see `docs/KOKI_PORT_NOTES.md`. Keep the same budgets and the same
  `NEODCT_KOKI_IMG_CACHE_KB` / `NEODCT_KOKI_FX_CACHE_KB` overrides.
- **Return memory to the kernel on app exit.** With process-per-app this is now free:
  the process exits and everything goes back.
- **Know your buffer sizes.** One 240×175 RGB888 frame is 126,000 bytes. One 240×240
  RGB565 panel frame is 115,200 bytes. Write the arithmetic in a comment next to the
  allocation.

---

## 5. Logging

Keep the existing colour scheme — modem blue, system green, errors red. It is in
`neodct/overlay/etc/neodct-colors.sh` and users recognise it.

```c
nd_log(ND_LOG_MODEM, "AT+CSQ -> %d,%d", rssi, ber);
nd_log_err("update", "signature check failed");
```

Tags match the Python ones exactly: `CORE`, `UI`, `MODEM`, `BATT`, `NOTIFY`, `CLOCK`,
`FB`, `INPUT`, `OS`, `RSHELL`, `UPDATE`. Existing log-scraping and the serial console
output stay recognisable.

---

## 6. Portability

Builds must be clean under:

```
-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion
-Wstrict-prototypes -Wmissing-prototypes -Wvla
```

`-Wconversion` will be annoying. It is on because implicit narrowing on 32-bit ARM is a
real source of pixel-offset bugs that do not reproduce on a desktop.

Target is ARM Cortex-A7 with NEON, 32-bit, little-endian. It must also build and run on
x86-64 Linux, because that is where the golden-frame tests run. So:

- No assumptions about pointer size or struct padding.
- No unaligned access. ARM will fault where x86 quietly works.
- No endianness assumptions in file or wire formats — read and write bytes explicitly.

---

## 7. Testing

Every module gets a unit test that runs on the host.

```
tests/
  unit/         host unit tests, one per module
  golden/       reference PNGs captured from the Python build
  frames/       C output, compared against golden/
```

A test that needs hardware is not a unit test. Put it in `tests/hw/`, and it runs only
on the device.

### Golden frames are no longer a gate

The golden-frame reference is the **Python build's output**, captured before any C was
written, and while the port was in progress a C change that altered pixels failed the
test. That was the right rule then: it is how "one-to-one" became something a machine
checked instead of something a person argued about.

The port is done, and the owner has since ruled that the frames have served their
purpose — applications are now being deliberately redesigned to look *different* from
the Python, and a rule that fails a build for changing pixels is now a rule against
doing the work. **A golden frame is not a reason to leave a screen the way it is, and
it is not something to ask permission about before changing a screen.**

What they are still good for, and why they are kept:

- **Regression, not conformance.** When you change screen A, the frames for screens
  B through Z are a cheap check that you did not disturb them. The Messages Chat style
  is the worked example: Classic was untouched, so `app-messages` and
  `app-messages-inbox` still match, and *that* is the useful signal.
- A frame you deliberately changed gets **re-cut**, not argued with:

      ./build/default/bin/nd-shoot --out /tmp/frames
      python3 neodct/tools/goldenframe.py --compare neodct/tests/golden /tmp/frames

  then copy the new PNG over the old one and update `manifest.json`. Say so in the
  commit message. Do not cut a *new* frame for a *new* screen — a redesigned screen's
  test is its unit test, not a picture of itself that can only ever agree with it.

---

## 8. Comments

Follow the existing project convention, which `AGENTS.md` states and the Python codebase
is genuinely good about:

> **Comments explain why, not what.**

```c
/* The pencil is plotted per pixel rather than with line()/polygon(): at the ~15px
 * this renders at, a polygon's edges land wherever rounding puts them and the barrel
 * comes out either a hairline or twice the weight of the font beside it. */
```

Not:

```c
/* loop over rows */          /* <- says nothing the code doesn't */
```

Every `.c` file opens with a block comment saying what the module is for and any
ordering or lifetime guarantees callers depend on.

---

## 9. When porting a specific piece of Python

1. **Read the Python first, completely.** Including the comments — they document
   reasoning you will not otherwise recover, and losing it is how a port introduces bugs
   the original had already fixed.
2. **Keep the same names.** A Python `VerticalList` becomes `nd_vlist`. Someone reading
   both should see the correspondence instantly.
3. **Keep the same magic numbers.** Every coordinate, colour, timeout and key code stays
   identical. If a number looks wrong, it is probably load-bearing — port it as-is and
   raise it as a question, do not fix it.
4. **Port the bug too.** If the Python has a quirk that shows on screen, the C reproduces
   the quirk. One-to-one means one-to-one. Note it in the spec; do not silently correct
   it.
5. **Absolute paths are load-bearing.** `/NeoDCT/System/...` stays absolute — `AGENTS.md`
   is explicit about this and the host test harness depends on it.

---

## 10. What to do when you are unsure

Do not guess, and do not quietly pick something reasonable.

Write it down in your module's section of `docs/c-rewrite/OPEN-QUESTIONS.md`, with the
file and line of the Python you were reading, and carry on with the rest of your work.
The project owner is following along and will answer.

A wrong guess that compiles is much more expensive than a question.
