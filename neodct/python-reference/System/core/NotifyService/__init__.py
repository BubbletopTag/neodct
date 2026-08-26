"""NotifyService -- Nokia-style notifications and the ringer.

SMS notifications are deliberately NOT modal (battery warnings keep their
own dedicated path): posting one beeps immediately, and the *visual* part
lives on the HOME screen exactly like the 3310 --

    * "N message(s) received" banner mid-left (carrier line hidden),
    * softkey turns into "Read" (enter opens the message),
    * C/backspace dismisses the banner (messages stay unread in the
      inbox and the envelope keeps flashing until they're actually read),
    * a flashing envelope in the status strip while unread mail exists.

Inside apps nothing visual happens -- the banner is waiting when you get
back to HOME.

Incoming CALLS are the exception: they interrupt whatever app is running
(the 3310 didn't multitask either). main.py raises IncomingCall from
read_keypress, which unwinds the app back to the core loop; the ringer
starts here.

Two audio paths, by design:
  * short beeps (SMS, DTMF) -> fire-and-forget `aplay`, never blocks;
  * the ringtone -> in-process python-miniaudio streaming, looped, so it
    plays user mp3/wma tones with no ~24MB mpv process (same reasoning as
    MusicPlayer). Paths with spaces are passed straight to miniaudio --
    no shell, no quoting to get wrong.
"""

import os
import subprocess

try:
    import miniaudio
    HAS_MINIAUDIO = True
except ImportError:
    HAS_MINIAUDIO = False
    print("[NOTIFY] 'miniaudio' not found; ringtone falls back to mpv.")

TONES_DIR = "/NeoDCT/System/tones"
SMS_TONE = os.path.join(TONES_DIR, "sms.wav")

RING_RATE = 44100
RING_BUF_MS = 500
RING_SETTING = "system.audio.ringtone"
# Last-resort ringtones if the configured one is missing/unplayable.
# Low.mp3 is the system default ringtone across the board.
RING_FALLBACKS = (os.path.join(TONES_DIR, "Low.mp3"),
                  os.path.join(TONES_DIR, "Nokia Tune.mp3"),
                  os.path.join(TONES_DIR, "Ring Ring.mp3"),
                  SMS_TONE)


class NotifyService:
    def __init__(self):
        print("[NOTIFY] Initializing NotifyService...")
        self._kind = None        # active banner kind ("sms") or None
        self._count = 0          # how many arrivals the banner covers
        self._latest_data = None  # e.g. newest inbox row id
        self._ring_device = None  # miniaudio PlaybackDevice while ringing
        self._ring_proc = None    # mpv fallback process while ringing

    # --- posting -----------------------------------------------------------

    def post_sms(self, message_row_id, tone=True):
        """Register one received SMS: beep now, banner until dismissed."""
        self._kind = "sms"
        self._count += 1
        self._latest_data = message_row_id
        if tone:
            self.play_tone(SMS_TONE)

    # --- home-screen state ---------------------------------------------------

    def active(self):
        return self._kind is not None

    def kind(self):
        return self._kind

    def count(self):
        return self._count

    def latest_data(self):
        return self._latest_data

    def banner_lines(self):
        """Two text lines for the home screen, 3310-style."""
        if self._kind != "sms":
            return ()
        noun = "message" if self._count == 1 else "messages"
        return ("%d %s" % (self._count, noun), "received")

    def dismiss(self):
        """C pressed (or the messages were opened): clear the banner."""
        self._kind = None
        self._count = 0
        self._latest_data = None

    # --- audio ---------------------------------------------------------------

    def play_tone(self, path):
        """Fire-and-forget beep; never blocks, never raises."""
        try:
            if not os.path.exists(path):
                print(f"[NOTIFY] Tone missing: {path}")
                return False
            subprocess.Popen(
                ["aplay", "-q", path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        except Exception as exc:
            print(f"[NOTIFY] Tone playback unavailable: {exc}")
            return False

    # --- the ringer ----------------------------------------------------------

    def ringtone_path(self):
        """Configured ringtone, or the first fallback that exists.

        Filenames with spaces are normal here ("Brave Scotland.mp3") --
        the path is never handed to a shell, so nothing splits it.
        """
        configured = None
        try:
            from System.core.SettingsStorage import get_setting
            configured = str(get_setting(RING_SETTING, "")).strip()
        except Exception as exc:
            print(f"[NOTIFY] Ringtone setting unreadable ({exc}).")
        if configured and os.path.exists(configured):
            return configured
        if configured:
            print(f"[NOTIFY] Ringtone missing: {configured!r}")
            # A renamed/re-encoded tone (e.g. .wma -> .mp3) still rings:
            # try the same basename with the other known extensions.
            stem = os.path.splitext(configured)[0]
            for ext in (".mp3", ".wav", ".wma", ".flac", ".ogg"):
                if os.path.exists(stem + ext):
                    print(f"[NOTIFY] Using {stem + ext} instead.")
                    return stem + ext
        for path in RING_FALLBACKS:
            if os.path.exists(path):
                print(f"[NOTIFY] Falling back to ringtone {path}.")
                return path
        # Anything playable at all beats a silent ring.
        try:
            for name in sorted(os.listdir(TONES_DIR)):
                if name.lower().endswith((".mp3", ".wav", ".wma")):
                    return os.path.join(TONES_DIR, name)
        except Exception:
            pass
        return None

    def start_ring(self):
        """Loop the ringtone until stop_ring(). Never blocks or raises."""
        self.stop_ring()
        path = self.ringtone_path()
        if path is None:
            print("[NOTIFY] No ringtone available; ringing silently.")
            return False
        if HAS_MINIAUDIO:
            try:
                # Decode once, then loop the whole buffer in-process: a
                # ringtone is a few seconds, and looping a stream_file
                # generator would need a re-open per repeat.
                decoded = miniaudio.decode_file(
                    path, output_format=miniaudio.SampleFormat.SIGNED16,
                    nchannels=2, sample_rate=RING_RATE)
                device = miniaudio.PlaybackDevice(
                    output_format=miniaudio.SampleFormat.SIGNED16,
                    nchannels=2, sample_rate=RING_RATE,
                    buffersize_msec=RING_BUF_MS, app_name="NeoDCT Ring")
                generator = self._loop_generator(decoded.samples)
                next(generator)
                device.start(generator)
                self._ring_device = device
                print(f"[NOTIFY] Ringing: {path}")
                return True
            except Exception as exc:
                print(f"[NOTIFY] miniaudio ring failed ({exc}); trying mpv.")
                self._ring_device = None
        try:
            self._ring_proc = subprocess.Popen(
                ["mpv", "--no-video", "--quiet", "--loop-file=inf", path],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"[NOTIFY] Ringing (mpv): {path}")
            return True
        except Exception as exc:
            print(f"[NOTIFY] Ringer unavailable: {exc}")
            self._ring_proc = None
            return False

    @staticmethod
    def _loop_generator(samples):
        """Endless playback generator over one decoded ringtone."""
        total = len(samples)
        pos = 0
        required = yield b""
        while True:
            frames = required * 2          # stereo: 2 samples per frame
            chunk = samples[pos:pos + frames]
            pos += frames
            if len(chunk) < frames:        # wrap around for the loop
                pos = frames - len(chunk)
                chunk = chunk + samples[0:pos]
            if total == 0:
                return
            required = yield chunk

    def stop_ring(self):
        if self._ring_device is not None:
            try:
                self._ring_device.close()
            except Exception:
                pass
            self._ring_device = None
            print("[NOTIFY] Ringer stopped.")
        if self._ring_proc is not None:
            try:
                self._ring_proc.terminate()
                self._ring_proc.wait(timeout=0.3)
            except Exception:
                try:
                    self._ring_proc.kill()
                except Exception:
                    pass
            self._ring_proc = None
            print("[NOTIFY] Ringer stopped.")

    def ringing(self):
        return self._ring_device is not None or self._ring_proc is not None
