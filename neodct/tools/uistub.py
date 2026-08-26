"""Headless stub of the NeoDCT runtime.

Lets host tooling drive the *real* UI code (System/core/main.py,
System/ui/framework.py, the shipped apps) with no framebuffer, no keypad,
no modem and no /NeoDCT rootfs. Frames are captured as PIL images instead
of being written to /dev/fb0.

Three pieces make that possible:

  CapturingFramebuffer  stands in for core.main.Framebuffer
  PathRemap             makes hardcoded "/NeoDCT/..." paths resolve into a
                        staged copy of the overlay
  KeyScript             replays a canned list of evdev key codes instead of
                        blocking on /dev/input/event0

Nothing here ships to the device; the overlay is the only thing copied
into the rootfs.
"""

import builtins
import os
import shutil
import sys
import tempfile

from PIL import Image

# TWO TREES, and the split matters.
#
# The overlay is what ships: fonts, icons, wallpapers, t9.dict, the
# manifests. The Python OS is not in it any more -- it is the reference
# implementation, it must never reach a phone, and it lives in
# neodct/python-reference (see the README there).
#
# A staged phone needs BOTH. The data comes from the overlay because that is
# what the device has; the System.* modules come from the reference because
# that is where they are. stage_overlay() lays the second over the first, so
# the staged root is what /NeoDCT looked like when Python ran it.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_NEODCT = os.path.join(_REPO, "overlay", "NeoDCT")
PY_REFERENCE = os.path.join(_REPO, "python-reference")

# Device geometry (see System/core/main.py).
PANEL_W = 240
PANEL_H = 240
UI_W = 240
UI_H = 175

NEODCT_PREFIX = "/NeoDCT"


class CapturingFramebuffer:
    """Drop-in for core.main.Framebuffer that records frames.

    The real driver mmaps /dev/fb0 and blits the 240x175 UI band into the
    middle of the 240x240 panel. This keeps the band frames as handed over
    by the UI, and reproduces the letterboxed panel on demand.
    """

    def __init__(self, panel_size=(PANEL_W, PANEL_H)):
        self.panel_size = panel_size
        self.frames = []
        self.xres, self.yres = panel_size
        self.bpp = 32
        self._budget = None
        self._drawn = 0

    def set_budget(self, frames):
        """Allow only `frames` more draws, then raise ScriptExhausted.

        Real-time apps (Koki) read the evdev fd directly and never call
        read_keypress, so no key-based signal can stop them. Every app
        does draw, though, which makes this the one reliable choke point.
        """
        self._budget = frames
        self._drawn = 0

    def clear_budget(self):
        self._budget = None
        self._drawn = 0

    def update(self, pil_image):
        if self._budget is not None:
            if self._drawn >= self._budget:
                raise ScriptExhausted()
            self._drawn += 1
        # Copy: the UI redraws onto one long-lived canvas, so a reference
        # would leave every captured frame showing the final screen.
        self.frames.append(pil_image.convert("RGB").copy())

    def device_frame(self, index=-1):
        """The band as the physical panel shows it, black bars included."""
        band = self.frames[index]
        panel = Image.new("RGB", self.panel_size, "black")
        panel.paste(
            band,
            ((self.panel_size[0] - band.width) // 2,
             (self.panel_size[1] - band.height) // 2),
        )
        return panel

    def clear(self):
        self.frames.clear()


class PathRemap:
    """Redirect absolute "/NeoDCT/..." paths into a staged overlay copy.

    The runtime hardcodes device paths (fonts, icons, layout JSON, sqlite
    databases). Rather than needing root to create /NeoDCT on the host,
    this patches the path-taking calls the runtime uses so they land in a
    staging directory instead.
    """

    # (module, attribute, indices of the path arguments)
    _TARGETS = (
        ("builtins", "open", (0,)),
        # zipfile (and anything else opening through the io module) reaches
        # io.open by attribute, so patching builtins.open alone misses it.
        ("io", "open", (0,)),
        ("os.path", "exists", (0,)),
        ("os.path", "isfile", (0,)),
        ("os.path", "isdir", (0,)),
        ("os.path", "getsize", (0,)),
        ("os", "listdir", (0,)),
        ("os", "walk", (0,)),
        ("os", "makedirs", (0,)),
        ("os", "mkdir", (0,)),
        ("os", "remove", (0,)),
        ("os", "unlink", (0,)),
        ("os", "stat", (0,)),
        ("os", "access", (0,)),
        # Two-path calls: settings.prop is written atomically via rename.
        ("os", "rename", (0, 1)),
        ("os", "replace", (0, 1)),
        ("shutil", "copyfile", (0, 1)),
        ("shutil", "copy", (0, 1)),
        ("shutil", "copy2", (0, 1)),
        ("sqlite3", "connect", (0,)),
        ("PIL.Image", "open", (0,)),
        ("PIL.ImageFont", "truetype", (0,)),
    )

    def __init__(self, staged_root):
        self.staged_root = str(staged_root).rstrip("/")
        self._saved = []

    def map_path(self, path):
        if isinstance(path, os.PathLike):
            path = os.fspath(path)
        if not isinstance(path, str):
            return path
        if path == NEODCT_PREFIX:
            return self.staged_root
        if path.startswith(NEODCT_PREFIX + "/"):
            return self.staged_root + path[len(NEODCT_PREFIX):]
        return path

    def _resolve(self, dotted):
        import importlib

        if dotted == "builtins":
            return builtins
        return importlib.import_module(dotted)

    def __enter__(self):
        for dotted, attr, arg_indices in self._TARGETS:
            module = self._resolve(dotted)
            original = getattr(module, attr, None)
            if original is None:
                continue
            self._saved.append((module, attr, original))
            setattr(module, attr, self._wrap(original, arg_indices))
        return self

    def _wrap(self, original, arg_indices):
        remap = self.map_path

        def wrapper(*args, **kwargs):
            if args:
                args = list(args)
                for index in arg_indices:
                    if len(args) > index:
                        args[index] = remap(args[index])
            return original(*args, **kwargs)

        wrapper.__name__ = getattr(original, "__name__", "wrapped")
        wrapper.__wrapped__ = original
        return wrapper

    def __exit__(self, exc_type, exc, tb):
        for module, attr, original in reversed(self._saved):
            setattr(module, attr, original)
        self._saved.clear()
        return False


class ScriptExhausted(BaseException):
    """Raised to unwind a blocking widget loop once the script runs out.

    Derives from BaseException (like core.main.IncomingCall) so that app
    code catching Exception cannot swallow it and spin forever.
    """


class KeyScript:
    """Replays evdev key codes in place of the keypad.

    on_exhausted="none"  -> pop() returns None (the runtime keeps polling)
    on_exhausted="raise" -> pop() raises ScriptExhausted to break a
                            blocking .show() loop once the script is done
    idle_budget          -> how many consecutive idle polls to allow before
                            raising. Game loops poll read_keypress instead
                            of blocking, so a plain None lets them spin
                            forever; the budget lets them animate a few
                            frames and then unwinds them.
    """

    def __init__(self, keys=(), on_exhausted="none", idle_budget=None):
        self._keys = list(keys)
        self.on_exhausted = on_exhausted
        self.idle_budget = idle_budget
        self.consumed = []
        self._idle = 0

    def push(self, *keys):
        self._keys.extend(keys)

    def pop(self, timeout=0.1):
        if self._keys:
            key = self._keys.pop(0)
            self.consumed.append(key)
            self._idle = 0
            return key
        if self.on_exhausted == "raise":
            raise ScriptExhausted()
        self._idle += 1
        if self.idle_budget is not None and self._idle > self.idle_budget:
            raise ScriptExhausted()
        return None

    def __len__(self):
        return len(self._keys)


def stage_overlay(dest=None):
    """Copy the repo overlay somewhere writable and return the NeoDCT root.

    The runtime creates databases, settings and ack files under
    /NeoDCT/User; staging keeps all of that out of the working tree.
    """
    if dest is None:
        dest = tempfile.mkdtemp(prefix="neodct-stub-")
    root = os.path.join(str(dest), "NeoDCT")
    if os.path.exists(root):
        shutil.rmtree(root)
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", "t9.dict")
    # t9.dict is nearly 3MB of read-only word list. Copying it into every
    # staged phone put ~3MB per test into /tmp for no benefit -- nothing here
    # types predictively, and t9_dict falls back to no suggestions when the
    # file is absent, which is exactly the behaviour a phone without one has.
    shutil.copytree(REPO_NEODCT, root, ignore=ignore)
    # ...then the Python over the top, into the same tree. dirs_exist_ok is
    # the whole point: System/apps/Clock/ already exists with its icon and
    # manifest from the overlay, and this adds the main.py beside them.
    shutil.copytree(PY_REFERENCE, root, ignore=ignore, dirs_exist_ok=True)
    return root


class StubUI:
    """Boots the real NeoDCT_UI against stubbed hardware.

    Used as a context manager so the path patching is always undone::

        with StubUI(wallpaper="Palestine.jpg") as ui:
            ui.update()
            ui.fb.device_frame().save("home.png")
    """

    def __init__(self, wallpaper=None, settings=None, keys=(),
                 engineering=True, root=None, skip_notice=True,
                 idle_budget=60):
        # Remember whether we staged this ourselves: only then is it ours
        # to delete. stage_overlay returns <tmpdir>/NeoDCT, so the
        # directory to remove is its parent.
        self._staged_dir = None
        if root:
            self.root = root
        else:
            self.root = stage_overlay()
            self._staged_dir = os.path.dirname(self.root)
        self.remap = PathRemap(self.root)
        self.fb = CapturingFramebuffer()
        self.keys = KeyScript(keys, idle_budget=idle_budget)
        self._wallpaper_name = wallpaper
        self._settings = dict(settings or {})
        self._engineering = engineering
        self._skip_notice = skip_notice
        self._ui = None

    # --- lifecycle -------------------------------------------------------

    def __enter__(self):
        if PY_REFERENCE not in sys.path:
            sys.path.insert(0, PY_REFERENCE)

        # PathRemap only covers /NeoDCT, so anything the runtime reads out
        # of /proc still belongs to the build host. That matters for the
        # network check in the Update app: the host has a default route and
        # the phone does not, so without this a stubbed phone believes it
        # is online and starts offering to download releases.
        os.environ["NEODCT_STUB"] = "1"

        self._prepare_user_dir()
        self.remap.__enter__()
        try:
            return self._boot()
        except BaseException:
            self.remap.__exit__(None, None, None)
            raise

    def __exit__(self, exc_type, exc, tb):
        self.remap.__exit__(exc_type, exc, tb)
        self._cleanup()
        return False

    def _cleanup(self):
        """Remove the staged phone.

        stage_overlay() used mkdtemp and nothing ever removed the result,
        so every test left ~16MB in /tmp for the life of the machine. One
        full run leaks a gigabyte; enough runs exceed the tmpfs quota and
        every later test fails in copytree with "Disk quota exceeded",
        which looks nothing like a leak and sends you hunting elsewhere.

        Only directories this instance created are removed -- a caller who
        passed its own root keeps it.
        """
        staged = getattr(self, "_staged_dir", None)
        if not staged:
            return
        self._staged_dir = None
        try:
            shutil.rmtree(staged, ignore_errors=True)
        except Exception:
            pass

    def _prepare_user_dir(self):
        user = os.path.join(self.root, "User")
        os.makedirs(user, exist_ok=True)
        if self._skip_notice:
            # Skips the modal "alpha software" notice on first boot.
            with open(os.path.join(user, ".ack_security_warning"), "w") as f:
                f.write("0")

        values = {
            "system.ui.engineering_mode": "ON" if self._engineering else "OFF",
        }
        if self._wallpaper_name:
            values["system.ui.wallpaper"] = (
                # Stock wallpapers ship inside the read-only image; only
                # user-added ones live under /NeoDCT/User.
                NEODCT_PREFIX + "/System/wallpapers/" + self._wallpaper_name
            )
        values.update(self._settings)

        lines = [f"{key}={value}\n" for key, value in sorted(values.items())]
        with open(os.path.join(user, "settings.prop"), "w") as f:
            f.writelines(lines)

    def _boot(self):
        from System.core import main as core_main

        # Never touch a real /dev/input device: an unreachable path makes
        # NeoDCT_UI fall back to "no input backend", which is what we want
        # since keys come from the script instead.
        dead_input = os.path.join(self.root, "no-such-input")
        original_discover = core_main._discover_keypad_path
        original_keypad_path = core_main.KEYPAD_PATH
        core_main._discover_keypad_path = lambda: dead_input
        # The runtime retries KEYPAD_PATH when discovery fails; point that
        # at the same dead path so no real evdev node is ever opened.
        core_main.KEYPAD_PATH = dead_input
        try:
            ui = core_main.NeoDCT_UI(self.fb)
        finally:
            core_main._discover_keypad_path = original_discover
            core_main.KEYPAD_PATH = original_keypad_path

        ui.read_keypress = self.keys.pop
        ui.wait_for_key = self._wait_for_key
        # Handy back-references for callers driving the UI.
        ui.keys = self.keys
        ui.fb = self.fb
        ui.root = self.root
        ui.stub = self
        self._ui = ui
        return ui

    def install_app(self, source_dir, engineering=False):
        """Copy an app folder into the staged rootfs and rescan.

        Same thing as dropping it in /NeoDCT/System/apps on the device,
        which is all "installing" means on NeoDCT.
        """
        ui = self._ui
        section = "engineering/apps" if engineering else "apps"
        dest = os.path.join(self.root, "System", section,
                            os.path.basename(str(source_dir).rstrip("/")))
        if os.path.exists(dest):
            shutil.rmtree(dest)
        shutil.copytree(str(source_dir), dest,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

        ui.apps = []
        app_dirs = [NEODCT_PREFIX + "/System/apps"]
        if getattr(ui, "engineering_mode", False):
            app_dirs.append(NEODCT_PREFIX + "/System/engineering/apps")
        for app_dir in app_dirs:
            ui._scan_apps_from_dir(app_dir)
        ui.apps.sort(key=lambda app: app["id"])
        return dest

    def simulate_status(self, battery=4, signal=4, carrier=None):
        """Pretend a fuel gauge and a registered modem are present.

        With neither, the home screen honestly draws "?" for the battery
        and "No Service" for the carrier. Screenshots of a normal device
        want the real thing, so the two accessors the home layout reads
        are stubbed here.
        """
        ui = self._ui
        ui.battery.hardware = True
        ui.battery._level = battery
        ui.modem.signal_level = lambda: signal
        ui.modem.operator_display = lambda: carrier
        return ui

    def _wait_for_key(self):
        """Blocking read, but scripted: never spins once the script ends."""
        key = self.keys.pop(0.1)
        if key is None:
            raise ScriptExhausted()
        return key


def run_app(ui, name, keys=(), frame_budget=240):
    """Launch a shipped app by its manifest name and return its frames.

    Mirrors NeoDCT_UI.launch_app (importlib under the module name
    "neodct_app", then run(ui)), but lets ScriptExhausted through instead
    of treating it as a crash -- running out of scripted keys is how the
    harness leaves an app that would otherwise loop forever.
    """
    import importlib.util

    for app in ui.apps:
        if app["name"] == name:
            break
    else:
        available = ", ".join(sorted(a["name"] for a in ui.apps))
        raise KeyError(f"no app named {name!r}; have: {available}")

    ui.keys.push(*keys)
    start = len(ui.fb.frames)

    # importlib reads source through io.open_code (a C call PathRemap does
    # not intercept), so the manifest's /NeoDCT path is mapped up front.
    path = os.path.join(app["path"], app["exec"])
    stub = getattr(ui, "stub", None)
    if stub is not None:
        path = stub.remap.map_path(path)

    spec = importlib.util.spec_from_file_location("neodct_app", path)
    module = importlib.util.module_from_spec(spec)
    ui.fb.set_budget(frame_budget)
    try:
        spec.loader.exec_module(module)
        module.run(ui)
    except ScriptExhausted:
        pass
    finally:
        ui.fb.clear_budget()

    return ui.fb.frames[start:]
