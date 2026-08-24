"""System/core/MediaWidget: the one place that knows how to run mpv.

Everything that decides *how* media plays lives here rather than in the
apps, because the decisions are all consequences of the same two facts --
64 MB of RAM and a framebuffer with no DRM -- and they must not drift
apart between the browser, Messages and whatever plays media next.
"""

import json
import os
import signal
import subprocess
import time

import pytest

from System.core import MediaWidget


def _proc_state(pid):
    """'T' while a process is stopped, 'S'/'R' while it runs."""
    with open("/proc/%d/stat" % pid, "rb") as handle:
        return handle.read().rsplit(b")", 1)[1].split()[0].decode()


# --- what kind of media is this -------------------------------------------

def test_video_extensions_are_video():
    assert MediaWidget.kind_for("http://host/watch.avi") == "video"
    assert MediaWidget.kind_for("/card/video/clip.ndv") == "video"
    assert MediaWidget.kind_for("http://host/a.MP4") == "video"


def test_image_extensions_are_image():
    assert MediaWidget.kind_for("http://host/photo.jpg") == "image"
    assert MediaWidget.kind_for("/tmp/mms-part-1.png") == "image"


def test_audio_extensions_are_audio():
    assert MediaWidget.kind_for("http://host/song.mp3") == "audio"


def test_a_query_string_does_not_hide_the_extension():
    assert MediaWidget.kind_for("http://host/watch.avi?id=42&t=6") == "video"


def test_an_unknown_url_is_treated_as_video():
    # A <video> src with no extension is the common case for a stream;
    # guessing video lets mpv decide rather than refusing up front.
    assert MediaWidget.kind_for("http://host/stream") == "video"


# --- the mpv command line -------------------------------------------------

def test_the_url_is_the_last_argument():
    argv = MediaWidget.build_argv("http://host/watch.avi")
    assert argv[-1] == "http://host/watch.avi"


def test_a_url_that_looks_like_an_option_is_still_a_url():
    # mpv reads a leading "-" as an option; "--" ends option parsing.
    argv = MediaWidget.build_argv("--version")
    assert argv[-2:] == ["--", "--version"]


def test_video_goes_to_the_framebuffer_and_never_to_drm():
    argv = MediaWidget.build_argv("http://host/watch.avi")
    assert "--vo=fbdev" in argv
    assert not any("drm" in arg for arg in argv)
    assert not any("gpu" in arg for arg in argv)


def test_the_framebuffer_device_is_configurable():
    argv = MediaWidget.build_argv("x.avi", fbdev="/dev/fb1")
    assert "--fbdev-device=/dev/fb1" in argv


def test_playback_does_not_read_the_users_mpv_config():
    # The rootfs is read-only and there is no way for a user to have put a
    # config anywhere useful; loading one could only ever break playback.
    argv = MediaWidget.build_argv("x.avi")
    assert "--no-config" in argv


def test_decoding_is_single_threaded_and_software():
    argv = MediaWidget.build_argv("x.avi")
    assert "--vd-lavc-threads=1" in argv
    assert "--hwdec=no" in argv


def test_subtitles_are_not_hunted_for():
    argv = MediaWidget.build_argv("x.avi")
    assert "--sub-auto=no" in argv


def test_no_option_that_needs_lua_is_passed():
    """mpv treats an unrecognised option as fatal, not as a warning.

    The NeoDCT build has no Lua in it, so mpv is compiled without --osc,
    --load-scripts or --ytdl -- those options only exist when the scripting
    layer does. Passing one means mpv prints "option not found" to a serial
    log nobody is reading and exits before drawing a frame, which on the
    phone looks exactly like the video failing to load.
    """
    argv = MediaWidget.build_argv("x.avi")
    for absent in ("--osc", "--load-scripts", "--ytdl"):
        assert not any(arg.startswith(absent) for arg in argv), absent


def test_mpv_exits_at_the_end_rather_than_sitting_idle():
    argv = MediaWidget.build_argv("x.avi")
    assert "--keep-open=no" in argv
    assert "--idle=no" in argv


def test_the_demuxer_cache_is_small_enough_to_share_the_phone_with():
    """Measured on the device: the cache is the single largest thing mpv
    allocates, and all of it is dirty.

    With --demuxer-max-bytes=4MiB mpv sits at 19.4 MB RSS, 8.5 MB of it
    anonymous and 7.8 MB dirty -- dirty meaning it can only be swapped,
    not dropped. At 512KiB the same clip plays identically at 16.3 MB RSS,
    6.0 MB anonymous, 5.4 MB dirty. That 2.4 MB is the difference between
    the browser and the player coexisting and the OOM killer choosing one.
    """
    argv = MediaWidget.build_argv("http://host/watch.avi")
    cache = [a for a in argv if a.startswith("--demuxer-max-bytes=")]
    assert cache, "no demuxer cache limit set at all"
    size = cache[0].split("=", 1)[1]
    assert size.endswith("KiB"), size
    assert int(size[:-3]) <= 1024, size


def test_an_ipc_socket_is_passed_when_asked_for():
    argv = MediaWidget.build_argv("x.avi", ipc_socket="/run/neodct/mpv.sock")
    assert "--input-ipc-server=/run/neodct/mpv.sock" in argv


def test_no_ipc_option_when_there_is_no_socket():
    argv = MediaWidget.build_argv("x.avi", ipc_socket=None)
    assert not any(arg.startswith("--input-ipc-server") for arg in argv)


def test_an_image_is_held_on_screen_and_has_no_audio():
    argv = MediaWidget.build_argv("photo.jpg")
    assert "--image-display-duration=inf" in argv
    assert "--audio=no" in argv


def test_audio_only_media_still_gets_a_video_output_for_the_cover():
    argv = MediaWidget.build_argv("song.mp3")
    assert "--vo=fbdev" in argv
    assert "--audio=no" not in argv


# --- keys -----------------------------------------------------------------

def test_the_clear_key_quits_back_to_the_application():
    assert MediaWidget.ipc_command(14) == ["quit"]


def test_the_navikey_toggles_pause():
    assert MediaWidget.ipc_command(28) == ["cycle", "pause"]


def test_left_and_right_seek():
    assert MediaWidget.ipc_command(105) == ["seek", "-10", "relative"]
    assert MediaWidget.ipc_command(106) == ["seek", "10", "relative"]


def test_up_and_down_change_the_volume():
    assert MediaWidget.ipc_command(103) == ["add", "volume", "5"]
    assert MediaWidget.ipc_command(108) == ["add", "volume", "-5"]


def test_star_and_hash_seek_a_minute():
    assert MediaWidget.ipc_command(42) == ["seek", "-60", "relative"]
    assert MediaWidget.ipc_command(43) == ["seek", "60", "relative"]


def test_the_number_keys_jump_to_a_percentage():
    assert MediaWidget.ipc_command(2) == ["seek", "10", "absolute-percent"]
    assert MediaWidget.ipc_command(6) == ["seek", "50", "absolute-percent"]
    assert MediaWidget.ipc_command(11) == ["seek", "0", "absolute-percent"]


def test_an_unbound_key_does_nothing():
    assert MediaWidget.ipc_command(50) is None
    assert MediaWidget.ipc_command(999) is None


def test_a_command_encodes_as_one_line_of_json():
    line = MediaWidget.encode_command(["seek", "10", "relative"])
    assert line.endswith(b"\n")
    assert json.loads(line) == {"command": ["seek", "10", "relative"]}


# --- suspending the application while mpv runs ----------------------------

@pytest.fixture
def sleeper():
    proc = subprocess.Popen(["sleep", "30"])
    try:
        yield proc
    finally:
        proc.send_signal(signal.SIGCONT)
        proc.kill()
        proc.wait()


def test_the_application_is_stopped_for_the_duration(sleeper):
    with MediaWidget.suspended(sleeper.pid):
        # Give the kernel a moment to actually park it.
        for _ in range(50):
            if _proc_state(sleeper.pid) == "T":
                break
            time.sleep(0.01)
        assert _proc_state(sleeper.pid) == "T"
    assert _proc_state(sleeper.pid) != "T"


def test_the_application_is_resumed_even_if_playback_raises(sleeper):
    with pytest.raises(RuntimeError):
        with MediaWidget.suspended(sleeper.pid):
            raise RuntimeError("mpv fell over")
    assert _proc_state(sleeper.pid) != "T"


def test_suspending_nothing_is_allowed():
    with MediaWidget.suspended(None):
        pass


def test_a_process_that_dies_while_suspended_is_not_an_error(sleeper):
    with MediaWidget.suspended(sleeper.pid):
        sleeper.send_signal(signal.SIGCONT)
        sleeper.kill()
        sleeper.wait()


def test_we_refuse_to_suspend_ourselves():
    # play() suspends its parent; if that ever resolved to this process the
    # phone would stop dead with mpv holding the screen and no way back.
    with pytest.raises(ValueError):
        with MediaWidget.suspended(os.getpid()):
            pass


def test_we_refuse_to_suspend_init():
    with pytest.raises(ValueError):
        with MediaWidget.suspended(1):
            pass
