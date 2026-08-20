"""
NeoDCT Music Player v4 (The "NeoPod" - Split Layout)
Updates:
- Playback now streams in-process via python-miniaudio instead of
  spawning mpv (~24MB private RSS per process; the OOM killer's first
  pick on real hardware). Tracks decode chunk-by-chunk, so RAM use is
  just the device buffer.
- flac/ogg now playable. aac only via the mpv fallback (miniaudio has
  no aac decoder).
- Progress bar uses the real decoded-frame position under miniaudio;
  the mpv fallback keeps the old wall-clock estimate.
"""

import os
import time
import subprocess
import signal
import io
from PIL import Image, ImageFile
from System.ui.framework import VerticalList, SoftKeyBar, MessageDialog, TextScroller
from System.core import Storage

# 1. Be tolerant of bad MP3 art
ImageFile.LOAD_TRUNCATED_IMAGES = True

try:
    import mutagen
    from mutagen.mp3 import MP3
    from mutagen.id3 import ID3, APIC, TIT2, TPE1, TALB
    HAS_MUTAGEN = True
except ImportError:
    HAS_MUTAGEN = False
    print("[Music] 'mutagen' library not found. Metadata/Art disabled.")

try:
    import miniaudio
    HAS_MINIAUDIO = True
except ImportError:
    HAS_MINIAUDIO = False
    print("[Music] 'miniaudio' library not found. Falling back to mpv.")

# Music lives on the SD card, not in the image: a 50MB read-only rootfs is
# no place for an album. Storage.folder() only answers once a NeoDCT card is
# actually mounted, so scan_music simply finds nothing without one.
MUSIC_FOLDER = "music"

NO_CARD_HELP = (
    "Music is played from an SD card.\n"
    "\n"
    "Format a card as FAT32, make a folder called \"music\" on it, and copy "
    "your .mp3, .flac, .wav or .ogg files into it.\n"
    "\n"
    "Put the card in the phone and your music shows up here. The phone can "
    "set a blank card up for you from Settings."
)

MPV_CMD = [
    "nice", "-n", "-10",
    "mpv",
    "--no-video",
    "--audio-buffer=4",
    "--quiet"
]


class _MiniaudioPlayer:
    """In-process streaming playback: one ALSA device fed by miniaudio's
    chunked decoder. Same reasoning as Koki's mixer -- no spawn latency,
    a single ALSA client, kilobytes of buffer instead of an mpv process."""

    EXTS = (".mp3", ".wav", ".flac", ".ogg")
    RATE = 44100

    def __init__(self):
        self.device = None
        self.stream = None
        self.is_paused = False
        self._ended = False
        self._frames = 0

    def play(self, full_path):
        self.stop()
        self._ended = False
        self._frames = 0
        buf_ms = int(os.environ.get("NEODCT_MUSIC_ABUF_MS", "500"))
        # The device asks for up to one period (buf_ms) of frames per
        # callback; the stream's decode buffer must be at least that big
        # or it raises mid-playback. 2x for headroom (~350KB at 500ms).
        frames_to_read = max(16384, self.RATE * buf_ms * 2 // 1000)
        stream = miniaudio.stream_file(
            full_path,
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=2, sample_rate=self.RATE,
            frames_to_read=frames_to_read)
        wrapped = miniaudio.stream_with_callbacks(
            stream,
            progress_callback=self._on_progress,
            end_callback=self._on_end)
        next(wrapped)
        device = miniaudio.PlaybackDevice(
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=2, sample_rate=self.RATE,
            buffersize_msec=buf_ms, app_name="NeoDCT Music")
        device.start(wrapped)
        self.device = device
        self.stream = wrapped
        self.is_paused = False

    def _on_progress(self, nframes):
        self._frames += nframes

    def _on_end(self):
        self._ended = True

    def stop(self):
        if self.device:
            try:
                self.device.close()
            except Exception:
                pass
        self.device = None
        self.stream = None
        self.is_paused = False

    def toggle_pause(self):
        # Pausing = stopping the device; the stream generator keeps its
        # position, so start() with the same generator resumes in place.
        if not self.device or self._ended:
            return
        if self.is_paused:
            self.device.start(self.stream)
            self.is_paused = False
        else:
            self.device.stop()
            self.is_paused = True

    def is_finished(self):
        return self.device is None or self._ended

    def position(self):
        return self._frames / self.RATE


class _MpvPlayer:
    """External-process fallback for when python-miniaudio is missing
    (or NEODCT_MUSIC_AUDIO=subprocess forces it)."""

    EXTS = (".mp3", ".wav", ".aac", ".flac", ".ogg")

    def __init__(self):
        self.process = None
        self.is_paused = False

    def play(self, full_path):
        self.stop()
        self.process = subprocess.Popen(
            MPV_CMD + [full_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        self.is_paused = False

    def stop(self):
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=0.2)
            except Exception:
                try:
                    self.process.kill()
                except Exception:
                    pass
            self.process = None
            self.is_paused = False

    def toggle_pause(self):
        if not self.process:
            return
        # mpv can exit between our poll and this signal (track just ended);
        # signalling a dead pid raises ProcessLookupError and crashes the app.
        try:
            if self.is_paused:
                os.kill(self.process.pid, signal.SIGCONT)
                self.is_paused = False
            else:
                os.kill(self.process.pid, signal.SIGSTOP)
                self.is_paused = True
        except (ProcessLookupError, OSError):
            self.is_paused = False

    def is_finished(self):
        return self.process is None or self.process.poll() is not None

    def position(self):
        return None


def _pick_player():
    # NEODCT_MUSIC_AUDIO=subprocess forces mpv; =miniaudio makes its
    # absence a hard disable rather than a fallback (same convention as
    # NEODCT_KOKI_AUDIO).
    forced = os.environ.get("NEODCT_MUSIC_AUDIO", "")
    if forced != "subprocess" and HAS_MINIAUDIO:
        print("[Music] audio: in-process miniaudio streaming")
        return _MiniaudioPlayer()
    if forced == "miniaudio":
        print("[Music] NEODCT_MUSIC_AUDIO=miniaudio but module missing; audio disabled")
        return None
    print("[Music] audio: external mpv processes")
    return _MpvPlayer()


class MusicPlayer:
    def __init__(self, ui):
        self.ui = ui
        self.softkey = SoftKeyBar(ui)
        self.playlist = []
        self.player = _pick_player()

    def music_dir(self):
        """The card's music folder, or None when no card is mounted."""
        return Storage.folder(MUSIC_FOLDER)

    def scan_music(self):
        self.playlist = []
        exts = self.player.EXTS if self.player else ()
        music_dir = self.music_dir()
        if exts and music_dir and os.path.exists(music_dir):
            for root, dirs, files in os.walk(music_dir):
                for f in sorted(files):
                    if f.lower().endswith(exts):
                        full_path = os.path.join(root, f)
                        self.playlist.append(full_path)

    def get_metadata(self, filepath):
        meta = {
            "title": os.path.basename(filepath),
            "artist": "Unknown Artist",
            "album": "",
            "art": None,
            "length": 0
        }

        if HAS_MUTAGEN:
            try:
                audio = MP3(filepath, ID3=ID3)

                if audio.tags:
                    if "TIT2" in audio.tags: meta["title"] = str(audio.tags["TIT2"])
                    if "TPE1" in audio.tags: meta["artist"] = str(audio.tags["TPE1"])
                    if "TALB" in audio.tags: meta["album"] = str(audio.tags["TALB"])

                    for tag in audio.tags.values():
                        if isinstance(tag, APIC):
                            try:
                                meta["art"] = Image.open(io.BytesIO(tag.data))
                                break
                            except Exception:
                                meta["art"] = None

                meta["length"] = audio.info.length
            except Exception as e:
                print(f"[Music] Metadata error: {e}")

        # mutagen's MP3 class only handles mp3; miniaudio can report the
        # duration of anything it can decode (wav/flac/ogg too).
        if not meta["length"] and HAS_MINIAUDIO:
            try:
                meta["length"] = miniaudio.get_file_info(filepath).duration
            except Exception:
                pass

        # Many rips carry full ID3 text tags but no embedded APIC frame,
        # so fall back to sidecar art sitting next to the track.
        if meta["art"] is None:
            meta["art"] = self.find_folder_art(filepath)

        return meta

    def find_folder_art(self, filepath):
        folder = os.path.dirname(filepath)
        try:
            entries = {e.lower(): e for e in os.listdir(folder)}
        except OSError:
            return None
        for name in ("cover", "folder", "front", "album", "albumart"):
            for ext in (".jpg", ".jpeg", ".png"):
                real = entries.get(name + ext)
                if real:
                    try:
                        return Image.open(os.path.join(folder, real))
                    except Exception:
                        pass
        return None

    def format_time(self, seconds):
        m = seconds // 60
        s = seconds % 60
        return f"{m:02d}:{s:02d}"

    def play_file(self, full_path):
        if self.player is None:
            return False
        try:
            self.player.play(full_path)
            return True
        except Exception as e:
            print(f"[Music] playback failed: {e.__class__.__name__}: {e}")
            # A DecodeError means this file is bad -- the backend is fine.
            # Anything else from miniaudio (usually device init) means the
            # in-process path is unusable, so drop to mpv for the session.
            if HAS_MINIAUDIO and isinstance(self.player, _MiniaudioPlayer) \
                    and not isinstance(e, miniaudio.DecodeError) \
                    and os.environ.get("NEODCT_MUSIC_AUDIO", "") != "miniaudio":
                print("[Music] falling back to mpv")
                self.player = _MpvPlayer()
                return self.play_file(full_path)
            return False

    def stop(self):
        if self.player:
            self.player.stop()

    def run_now_playing(self, filepath):
        screen_w = getattr(self.ui, "W", 240)
        screen_h = getattr(self.ui, "H", 175)
        softkey_h = getattr(self.ui, "SOFTKEY_H", 30)
        content_bottom = getattr(self.ui, "content_bottom", screen_h - softkey_h)
        header_h = max(24, int(screen_h * 0.08))

        # 1. Load Data
        self.ui.draw.rectangle((0, 0, screen_w, screen_h), fill="black")
        lw, lh = self.ui.get_text_size("Loading...", self.ui.font_n)
        self.ui.draw.text(((screen_w - lw) // 2, max(10, (content_bottom - lh) // 2)), "Loading...", font=self.ui.font_n, fill="white")
        self.ui.fb.update(self.ui.canvas)

        meta = self.get_metadata(filepath)

        # 2. Resize Art (Smaller now, for side layout)
        display_art = None
        art_size = min(100, max(64, int(screen_w * 0.42)))

        if meta["art"]:
            try:
                meta["art"].draft("RGB", (art_size * 2, art_size * 2))
                meta["art"].load()
                display_art = meta["art"].resize((art_size, art_size), Image.Resampling.NEAREST)
            except Exception:
                display_art = None

        start_time = time.time()
        paused_at = 0
        total_paused_duration = 0

        # --- NEW LAYOUT CONSTANTS ---
        art_x = 8
        art_y = header_h + 12
        text_x = art_x + art_size + 8
        text_width = max(30, screen_w - text_x - 8)
        bar_width = max(48, screen_w - 20)
        bar_x = (screen_w - bar_width) // 2
        bar_y = content_bottom - 18

        # Keep art fully above progress/timestamps.
        available_media_h = max(48, bar_y - art_y - 18)
        art_size = min(art_size, available_media_h)

        if display_art and display_art.size != (art_size, art_size):
            display_art = display_art.resize((art_size, art_size), Image.Resampling.NEAREST)

        needs_redraw = True

        while True:
            # Check Status
            if self.player.is_finished():
                self.stop()
                return

            # Calc Time (wall-clock fallback for backends without a
            # real position; miniaudio overrides it below)
            now = time.time()
            if self.player.is_paused:
                if paused_at == 0: paused_at = now
                current_elapsed = paused_at - start_time - total_paused_duration
            else:
                if paused_at != 0:
                    total_paused_duration += (now - paused_at)
                    paused_at = 0
                current_elapsed = now - start_time - total_paused_duration

            pos = self.player.position()
            if pos is not None:
                current_elapsed = pos

            # DRAW
            if needs_redraw:
                # Clear Content Area
                self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")

                # -- Header --
                self.ui.draw.rectangle((0, 0, screen_w, header_h), fill="white")
                w, h = self.ui.get_text_size("Now Playing", self.ui.font_s)
                self.ui.draw.text(((screen_w - w) // 2, max(2, (header_h - h) // 2)), "Now Playing", font=self.ui.font_s, fill="black")

                # -- Album Art (Left) --
                if display_art:
                    self.ui.canvas.paste(display_art, (art_x, art_y))
                    self.ui.draw.rectangle((art_x - 1, art_y - 1, art_x + art_size, art_y + art_size), outline="white")
                else:
                    self.ui.draw.rectangle((art_x, art_y, art_x + art_size, art_y + art_size), outline="white")
                    # Note Icon
                    cx, cy = art_x + (art_size // 2), art_y + (art_size // 2)
                    self.ui.draw.ellipse((cx - 8, cy + 8, cx + 1, cy + 17), fill="white")
                    self.ui.draw.line((cx + 1, cy + 12, cx + 1, cy - 12), fill="white", width=2)
                    self.ui.draw.line((cx + 1, cy - 12, cx + 14, cy - 8), fill="white", width=2)

                # -- Info (Right) --
                # Helper to truncate text to fit right column
                def truncate(text, font, max_w):
                    t = text
                    w, _ = self.ui.get_text_size(t, font)
                    while w > max_w and len(t) > 0:
                        t = t[:-1]
                        w, _ = self.ui.get_text_size(t + "...", font)
                    return t + "..." if len(t) < len(text) else t

                # Title (Bold)
                t_str = truncate(meta["title"], self.ui.font_n, text_width)
                self.ui.draw.text((text_x, art_y), t_str, font=self.ui.font_n, fill="white")

                # Artist (Regular)
                a_str = truncate(meta["artist"], self.ui.font_s, text_width)
                self.ui.draw.text((text_x, art_y + 25), a_str, font=self.ui.font_s, fill="#cccccc")

                # Album (Regular, below Artist)
                if meta["album"]:
                    al_str = truncate(meta["album"], self.ui.font_s, text_width)
                    self.ui.draw.text((text_x, art_y + 45), al_str, font=self.ui.font_s, fill="#999999")

                # -- Progress Bar (Bottom) --
                self.ui.draw.rectangle((bar_x, bar_y, bar_x + bar_width, bar_y + 4), fill="#333333")

                if meta["length"] > 0:
                    pct = min(1.0, current_elapsed / meta["length"])
                else:
                    pct = 0

                fill_width = int(bar_width * pct)
                self.ui.draw.rectangle((bar_x, bar_y, bar_x + fill_width, bar_y + 4), fill="white")

                # Timestamps
                curr_str = self.format_time(int(current_elapsed))
                self.ui.draw.text((bar_x, bar_y - 15), curr_str, font=self.ui.font_s, fill="white")

                if meta["length"] > 0:
                    total_str = "-" + self.format_time(int(max(0, meta["length"] - current_elapsed)))
                    w, h = self.ui.get_text_size(total_str, self.ui.font_s)
                    self.ui.draw.text((bar_x + bar_width - w, bar_y - 15), total_str, font=self.ui.font_s, fill="white")

                self.softkey.update("Pause" if not self.player.is_paused else "Play")
                needs_redraw = False

            # Input
            key = self.ui.read_keypress(1.0)
            if key is None:
                needs_redraw = True
                continue

            needs_redraw = True
            if key == 14: # STOP
                self.stop()
                return
            elif key == 28: # PAUSE
                self.player.toggle_pause()

    def run(self):
        while True:
            screen_w = getattr(self.ui, "W", 240)
            content_bottom = getattr(
                self.ui,
                "content_bottom",
                getattr(self.ui, "H", 175) - getattr(self.ui, "SOFTKEY_H", 30),
            )
            self.scan_music()
            if not self.playlist:
                self.ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
                y = max(12, int(content_bottom * 0.35))
                self.ui.draw.text((10, y), "No Music Found", font=self.ui.font_n, fill="white")
                self.ui.draw.text((10, y + 30), "Add mp3s to:", font=self.ui.font_s, fill="gray")
                self.ui.draw.text((10, y + 50), "/User/music", font=self.ui.font_s, fill="gray")
                self.softkey.update("Exit")
                while True:
                    k = self.ui.wait_for_key()
                    if k in (14, 28): return

            display_list = [os.path.basename(p) for p in self.playlist]
            vlist = VerticalList(self.ui, "Music", display_list, app_id=4)
            sel = vlist.show()
            if sel == -1: return

            full_path = self.playlist[sel]
            if self.play_file(full_path):
                self.run_now_playing(full_path)

def run(ui):
    app = MusicPlayer(ui)
    if app.music_dir() is None:
        # Say why there is nothing to play rather than showing an empty list.
        MessageDialog(ui, "No SD card.\nMusic is played from a card.",
                      button_text="More").show()
        TextScroller(ui, NO_CARD_HELP).show()
        return
    try:
        app.run()
    finally:
        app.stop()
